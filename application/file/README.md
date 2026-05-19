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
- `application/workflow/aiRequestPool.cpp::WriteTextFile` — gates AI-task output file writes; the canonical `confinedPath` is then handed to `EngineCore::AtomicWriteFile` so symlink targets resolve to their real on-tree location *and* a SIGKILL/disk-full mid-write leaves the previous version intact.
- `application/workflow/aiCallTaskExecutor.cpp::WriteTextFile` (public static) — same canonical-path pattern, used by inline queue-file writes, the ConvertWithMarkitdown destination write, and the provider-sidecar JSON write; routes through `EngineCore::AtomicWriteFile`.
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
- `engine/json/configParser.cpp` — `is_mock: true` provider entries have their `fixture_path` gated through `ConfineUnderProjectRoot` at parse time.  Rejection (escape, symlink target outside, unresolvable) marks the interface InvalidAPI with a structured ERROR.  Defense-in-depth with the MockTransport load-time check below.
- `engine/curlWrapper/mockTransport.cpp::LoadFixtureBody` and `LoadFixtureMeta` — every fixture path consumed at request dispatch (primary body + optional sibling `<fixture>.meta.json`) is re-gated through `ConfineUnderProjectRoot`.  Each rejection emits a structured ERROR carrying `cancelKey` + `quotaKey` so the dashboard run analyzer surfaces it.  The same boundary enforces a 10 MiB per-fixture size cap; `.meta.json` parsing layers a `[200, 599]` HTTP-status allowlist and a `{Content-Type, Retry-After}` header-key allowlist on top.
- `application/web/webServer.cpp` — `POST /api/settings/ai-interfaces` and `PUT /api/settings/ai-interfaces/<name>` gate the user-supplied `fixture_path` through `ConfineUnderProjectRoot` before persisting it on the interface struct.  Rejection returns 400 with `fixture_path_rejected` and the offending path.

In addition to `ConfineUnderProjectRoot`, several call sites use **scope-tighter `lexically_relative(<base>)` containment** when the operation must stay inside a narrower subtree than the project root:
- `application/workflow/taskExecutorRegistry.cpp::MaterializeFiles` — `targetFilename` containment under `taskWorkingDirectoryPath`.
- `application/workflow/workflowFileIndex.cpp::FindByRelativePath` — `relativePath` containment under `m_RootDirectory`.
- `application/workflow/scriptCatalog.cpp::Refresh` — `weakly_canonical` containment under the scripts root for every directory entry; combined with an explicit `is_symlink()` skip.
- `application/workflow/jcwfContainer.cpp::Extract` — `weakly_canonical(destPath)` lexicographic prefix check against `weakly_canonical(targetDir)` per zip entry, paired with an `external_attr & 0xF0000000 == 0xA0000000` symlink-entry reject and a name-shape allowlist (no absolute / `..` / NUL).  Containment is **per-extraction targetDir**, NOT project-root — the canonical Zip-Slip pattern; see the workflow README's `jcwfContainer.h/cpp` entry for the validation-pass-then-extraction-pass shape.

The workflow JSON parser (`application/workflow/workflowJsonParser.cpp` + `workflowJsonParserDetails.cpp`) adds a **parse-time syntactic gate** via `IsAcceptedRelativePath` that rejects empty, overlength, and absolute paths in `base_directory`, `working_directory`, `file_inputs`, `file_outputs`, and queue-binding `path` fields.  `..` segments are **allowed** at parse time because the shipped JCWF convention uses `working_directory: "../../queue/<workflow>/<task>"` to navigate from `workflows/<id>/` up to the project-root-anchored queue tree.  This is layered defense — the deeper canonical-containment check happens at the consumer sites listed above; both gates fail closed.

When a third + a fourth + ... call site appears, **add to the list above instead of writing a new local helper** — the established pattern is one canonical gate, fail-closed, ERROR-logged on rejection.

## Atomic-rename writes

Hand-built file writes route through the shared helper `EngineCore::AtomicWriteFile(path, content, errorMessage)` in `engine/auxiliary/file.h`.  It creates parent directories, opens `<final>.tmp.<counter>` with `ofstream` exceptions enabled (`failbit | badbit`), writes, closes, then `fs::rename`s over the destination.  A SIGKILL or disk-full mid-write leaves the previous version of the final file intact instead of a truncated partial that downstream readers parse as malformed.  Helper does NOT log on failure — callers with run/workflow context emit `LOG_APP_ERROR(...)` with the populated `errorMessage` so dashboard run analysis (which keys on runId substrings) can surface the failure.

Use-site categories (non-exhaustive — every hand-built JSON writer in `application/` should route through the helper):

- **AI-dispatch queue + output** — `application/session/fileWriter.cpp::FileWriter::Write` / `WriteWithHeader` (STNG/CNTX/TASK/PROB queue files), `application/workflow/aiRequestPool.cpp::WriteTextFile` lambda (AI-task `.output.{txt,json}`), `application/workflow/aiCallTaskExecutor.cpp::WriteTextFile`, `application/workflow/aiTranscript.cpp` (transcript appender).  Load-bearing because the rename event is what `OnOutputFileCreated` keys dispatch completion off — a torn write would surface as a malformed completion signal.
- **Filter manifests** — `application/workflow/filter/filterManifest.cpp::WriteManifest`, `application/workflow/filter/polarionClient.cpp::WriteItemFile`.
- **Workflow registry persistence** — `application/workflow/workflowRegistry.cpp` (global.json + canvas JSON inside `SaveOrUpdateWorkflowFromJson`).
- **Adhoc workflow meta/manifest** — `application/workflow/adhocWorkflowManager.cpp::WriteMeta` (artifact-attribution gate) and `::WriteManifest` (artifact-listing input).
- **Cloud-task outputs consumed by downstream tasks** — `application/cloud/cloudTaskExecutor.cpp::WriteResponseJson` (shared `response.json`), per-connector summary writers in `emailCloudTaskExecutor`, `slackCloudTaskExecutor`, `googleSheetsCloudTaskExecutor`, `snowflakeCloudTaskExecutor`, and the GitHub `get_file` write in `gitHubCloudTaskExecutor`.
- **Assistant data** — `application/assistant/assistantTools.cpp` (`write_file` / `edit_file` / `jcwf_write_plan` / `jcwf_write_script` / canvas re-pack / `jcwf_generate`), `application/assistant/assistantController.cpp::WriteFile`, `application/assistant/assistantMemory.cpp::Save`, `application/assistant/workspaceIndexer.cpp::SaveIndex`.
- **REST-handler config persistence** — `application/web/webServerHelpers::WriteTextFileAtomic` retains its own implementation because it owns the full parse-edit-revalidate-rename pipeline used by `HandleAiInterfacesSavePost` / `HandleConfigSettingsPut` / `HandleConnectionsSavePost` (`config.json` / `connections.json`).  Behaviour is equivalent; consolidation tracked as a separate cleanup.
- **MCP keystore + REST script writes** — `application/web/mcpKeyManager.cpp::Save` (encrypted blob), `application/web/webServer.cpp` `/ai-write-scripts` handler.
- **Misc** — `application/task/carMaintenanceTask.cpp::TryWriteAllText`.

Writers that intentionally do NOT use `AtomicWriteFile`:

- **Streaming writers with running caps** — `application/cloud/dbQueryCloudTaskExecutor.cpp` streams large result sets to disk with a per-row `tellp()` byte-cap check; buffering the entire result in memory to atomic-rename would defeat the cap.  Gets the exception-safety pattern (`ofstream::exceptions(failbit | badbit)` + try/catch) instead.
- **Append-mode logs** — `application/assistant/assistantSession.cpp::AppendTurn` writes one JSON line per call in append mode; atomic-rename doesn't apply to logical appends.  Gets the exception-safety pattern.
- **Captured stdout/stderr** — `application/workflow/shellTaskExecutor.cpp` and `application/workflow/pythonTaskExecutor.cpp` write `stdout.txt` / `stderr.txt` to the task working directory.  These are operator-diagnostic dumps, not consumed by downstream tasks; exception-safety pattern only.

When adding a new file-output writer, default to `EngineCore::AtomicWriteFile`.  Only opt out for streaming-with-cap, append-mode logs, or operator dumps as above.

---

## File Index
- `file/fileWatcher.h/.cpp`
- `file/scriptRegistry.h/.cpp`
- `file/pathConfinement.h/.cpp`
