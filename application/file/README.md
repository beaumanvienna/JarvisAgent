# JarvisAgent File System Subsystem Documentation

## Overview
The file subsystem provides:
- Asynchronous directory monitoring (`FileWatcher`).
- Script catalog population and lookup (`ScriptRegistry`).
- Project-root path confinement (`pathConfinement`).

It consists of:
- `FileWatcher` — polling watcher that emits filesystem events on the global event queue.
- `ScriptRegistry` — in-memory catalog of `@jarvis-script` metadata parsed from files under `scripts/`.
- `pathConfinement` — `ConfineUnderProjectRoot(path)` canonical-path gate; the only legitimate way to validate an external string before it reaches `fs::remove*`, Python `sys.path`, or any other filesystem-touching API.

---

## FileWatcher

### Purpose
Asynchronously monitors a directory (recursively) and emits engine events:
- `FileAddedEvent`
- `FileModifiedEvent`
- `FileRemovedEvent`
- Shutdown request if the primary root disappears (skipped when the primary root is empty)

### Behavior
- Runs inside a thread-pool task.
- Sleeps for the configured interval (`100ms` default).
- Initial scan fires `FileAddedEvent` for all existing files under the primary root.
- Skips directories and dotfiles (e.g. Geany temp files).
- Detects:
  - new files → `FileAddedEvent`
  - time-updated files → `FileModifiedEvent`
  - removed files → `FileRemovedEvent`

### Dynamic watch set
- `AddPath(root)` / `RemovePath(root)` register additional roots at runtime. Used by `TriggerEngine` for `file_watch` triggers on arbitrary declared paths. The primary root can be empty — in that case the watcher only observes paths added dynamically and does not request shutdown if a root disappears.

### Stop/Start
- `Start()` launches the watcher on the thread pool.
- `Stop()` / `SignalStop()` + `WaitStop()` terminate the watch task.

---

## Interaction Flow

1. `ScriptFileWatcher` watches `scripts/` and emits filesystem events.
2. The `TriggerEngine`-owned `FileWatcher` watches arbitrary trigger paths and emits events for `file_watch` triggers.
3. `JarvisAgent::OnEvent` routes filesystem events to `TriggerEngine::NotifyFileEvent` and to `ScriptRegistry` (for `scripts/` updates).
4. `PythonEnginePool::OnEvent` forwards events to Python hooks.

Runtime `ai_call` tasks no longer depend on filesystem events — they dispatch through a typed `AiInvocation` envelope via `AiRequestPool::Submit`; the reply callback writes `<prob>.output.{txt|json}` + `<prob>.transcript.json` inline.

---

## pathConfinement

### Purpose
Single canonical helper used everywhere j9t handles an external string that resolves to a filesystem path: workflow `file_outputs` deletion targets, `working_directory` task fields, Python `scriptPath` (the `parent_path` of the registered script entry point), Python `sys.path` entries.

### Contract
`ConfineUnderProjectRoot(std::filesystem::path | std::string) -> std::filesystem::path`

- Resolves the input against `fs::current_path()` (the project root, set by the launcher).
- Returns the canonical absolute path on success.
- Returns an empty path on rejection. Reasons for rejection:
  - `..` segments that escape the project root after canonicalisation.
  - An absolute path that lands outside the project root.
  - A symlink target that points out of tree (closes the symlink-attack vector).
  - Any `weakly_canonical` resolution error.
- Fail-closed: callers MUST treat empty as "refuse the operation" and log at ERROR with the offending input.

### Use sites
- `application/python/pythonEngine.cpp::SetupSubInterpreter` — gates `scriptDir` before storing it on `sys.path`.
- `application/python/pythonEngine.cpp::ExecuteWorkflowTaskOnWorker` — gates `taskWorkingDirectory` before injecting it into Python's context.
- `application/python/pythonEnginePool.cpp::Initialize` — gates the resolved script directory at the pool boundary (defense in depth).
- `application/workflow/workflowRuntimeManager.cpp::CleanWorkflow` — gates every `fs::remove` / `fs::remove_all` target (5 sites: queue dir, glob-matched file_outputs, literal file_outputs, working directories, empty workflow dir).
- `application/workflow/triggerEngine.cpp::NormalizePath` — gates both the file-watch trigger registration path AND the FileWatcher event path (canonical form keys both sides of the comparison so an embedded `..` cannot escape the watched tree).
- `application/workflow/aiRequestPool.cpp::WriteTextFile` — gates AI-task output file writes; the canonical `confinedPath` is also the rename target of the atomic `<final>.tmp.<counter>` write so symlink targets resolve to their real on-tree location *and* a SIGKILL/disk-full mid-write leaves the previous version intact.
- `application/workflow/aiCallTaskExecutor.cpp::WriteTextFile` (public static) — same atomic-rename + canonical-path pattern, used by inline queue-file writes, the ConvertWithMarkitdown destination write, and the provider-sidecar JSON write.
- `application/workflow/aiRequestPool.cpp::OnOutputFileCreated` — gates the read side of the AI-task output pipeline (map-key lookup + file read happen on the same canonical form).
- `application/workflow/aiRequestPool.cpp::RegisterPendingWorkflowTask` — gates the `expectedOutputPath` insert into `m_PendingByOutputPath` so a hostile JCWF can't register a binding under a path outside the project tree.
- `application/workflow/aiRequestPool.cpp::Submit` — log-attribution lookup (`m_PendingByOutputPath` find) AND cancel-key build use the same canonical form for matching across insert/lookup/cancel.
- `application/workflow/aiRequestPool.cpp::OnRequestFailed` — symmetry with the insert side.
- `application/workflow/workflowRegistry.cpp::SaveOrUpdateWorkflowFromJson` — gates the JCWF write target so a malicious editor PUT or adhoc submission can't write the canvas + zip outside the project tree.
- `application/workflow/workflowRegistry.cpp::RemoveWorkflow` — gates the file-deletion side; defense in depth (the path was canonicalised at insert time, but a future bug that lets a non-confined path into the registry can't trigger an arbitrary-file delete via this path).
- `application/workflow/shellTaskExecutor.cpp::ValidateScriptPath` — gates the `command` field of shell tasks; combined with explicit `lexically_relative(<projectRoot>/scripts/)` containment, refuses any script reference that lands outside the trusted scripts directory (also rejects symlink targets that point out of tree).
- `application/workflow/subWorkflowTaskExecutor.cpp::Execute` — gates `taskDefinition.m_WorkflowFile` before registry lookup so a hostile JCWF can't reference a workflow file outside the project tree.
- `application/workflow/pythonTaskExecutor.cpp::ValidateFileInputsExist` — gates each `taskDefinition.m_FileInputs[i]` before the existence check so a hostile JCWF can't read files outside the project tree via Python context plumbing.
- `application/cloud/dbQueryCloudTaskExecutor.cpp::ExecuteCloud` — gates the resolved output-file path so even if `taskWorkingDirectoryPath` itself somehow points outside the project tree (workflow-base-directory bug), the SQL result write stays confined.
- `application/cloud/gitHubCloudTaskExecutor.cpp::ExecuteCloud` (`get_file` branch) — gates `workDir / filename` before the base64-decoded content is written to disk; combined with the `IsValidGitHubName` allowlist on `owner`/`repo` + the `UrlEncodePathSegments` rejection of `..`/`.` segments in the GitHub-side `path`, every leg from AI-supplied param to on-disk write is canonicalised.
- `application/cloud/jiraCloudTaskExecutor.cpp::ExecuteCloud` (`create` branch) — gates `description_file` before the `std::ifstream` open, so a hostile JCWF cannot bait Jira's create-issue executor into reading `/etc/shadow` or anything outside the project tree.
- `application/cloud/redmineCloudTaskExecutor.cpp::ExecuteCloud` (`update_issue` branch) — gates `notes_file` and `assigned_to_id_file` before their reads.
- `application/cloud/slackCloudTaskExecutor.cpp::ExecuteSlackPost` — gates `text_file` and `thread_ts_file` before their reads (closes the same `_file` pattern the audit missed for Slack).
- `application/workflow/filter/filterManifest.cpp::ManifestPath` — gates the manifest target path; combined with an `IsValidFilterId` allowlist (alphanumerics + `_-.`) that rejects path-separator and `..` characters in the filterId itself.
- `application/workflow/aiCallTaskExecutor.cpp::MaterializeCntxFilesFromQueueBinding` and `MaterializeProbFilesFromQueueBinding` — every queue-binding source path is gated through `ConfineUnderProjectRoot` before the file is opened for read.  Project-root-wide (not task-folder-tight) because shipped JCWFs use cross-task data-flow paths like `../../../workflows/<id>/<other_task>/output.json` to read sibling task outputs — the path is outside the task working directory but inside the project tree.

In addition to `ConfineUnderProjectRoot`, several call sites use **scope-tighter `lexically_relative(<base>)` containment** when the operation must stay inside a narrower subtree than the project root:
- `application/workflow/taskExecutorRegistry.cpp::MaterializeFiles` — `targetFilename` containment under `taskWorkingDirectoryPath`.
- `application/workflow/workflowFileIndex.cpp::FindByRelativePath` — `relativePath` containment under `m_RootDirectory`.
- `application/workflow/scriptCatalog.cpp::Refresh` — `weakly_canonical` containment under the scripts root for every directory entry; combined with an explicit `is_symlink()` skip.
- `application/workflow/jcwfContainer.cpp::Extract` — `weakly_canonical(destPath)` lexicographic prefix check against `weakly_canonical(targetDir)` per zip entry, paired with an `external_attr & 0xF0000000 == 0xA0000000` symlink-entry reject and a name-shape allowlist (no absolute / `..` / NUL).  Containment is **per-extraction targetDir**, NOT project-root — the canonical Zip-Slip pattern; see the workflow README's `jcwfContainer.h/cpp` entry for the validation-pass-then-extraction-pass shape.

The workflow JSON parser (`application/workflow/workflowJsonParser.cpp` + `workflowJsonParserDetails.cpp`) adds a **parse-time syntactic gate** via `IsAcceptedRelativePath` that rejects empty, overlength, and absolute paths in `base_directory`, `working_directory`, `file_inputs`, `file_outputs`, and queue-binding `path` fields.  `..` segments are **allowed** at parse time because the shipped JCWF convention uses `working_directory: "../../queue/<workflow>/<task>"` to navigate from `workflows/<id>/` up to the project-root-anchored queue tree.  This is layered defense — the deeper canonical-containment check happens at the consumer sites listed above; both gates fail closed.

When a third + a fourth + ... call site appears, **add to the list above instead of writing a new local helper** — the established pattern is one canonical gate, fail-closed, ERROR-logged on rejection.

## Atomic-rename writes

Several writers materialise their output via the **temp-write-then-rename** pattern: open `<final>.tmp.<counter>` → write → close → `fs::rename`.  A SIGKILL or disk-full mid-write leaves the previous version of the final file intact instead of a truncated partial that downstream readers parse as malformed.  Use sites:

- `application/workflow/filter/filterManifest.cpp::WriteManifest` — uses `<path>.tmp` (single-writer guarantee from per-filter manifest mutex).
- `application/workflow/aiTranscript.cpp::WriteFile` — single in-process mutex serialises writers; temp-then-rename ensures crash-safety.
- `application/workflow/aiRequestPool.cpp::WriteTextFile` — atomic write of AI-task output files (`<prob>.output.{txt,json}`); a `static std::atomic<uint64_t>` counter makes the temp name unique under concurrent writers targeting the same final path.  Load-bearing because the rename event is what `OnOutputFileCreated` keys completion off — a torn write would surface to workflow runtime as a malformed completion signal.
- `application/workflow/aiCallTaskExecutor.cpp::WriteTextFile` (public static) — same atomic + counter pattern, applied to inline queue-file writes + ConvertWithMarkitdown destination + provider-sidecar JSON.
- `application/web/webServerHelpers::WriteTextFileAtomic` — used by `HandleAiInterfacesSavePost`/`HandleConfigSettingsPut`/`HandleConnectionsSavePost` to persist `config.json` / `connections.json` from the REST handlers (full pipeline: parse-edit-revalidate-rename per the architecture decision row).

When adding a new file-output writer, prefer the atomic pattern over open→write→close.  The cost is a single rename syscall per write; the benefit is crash-safety for the entire downstream pipeline that depends on the file's contents being either fully-old or fully-new.

---

## File Index
- `file/fileWatcher.h/.cpp`
- `file/scriptRegistry.h/.cpp`
- `file/pathConfinement.h/.cpp`
