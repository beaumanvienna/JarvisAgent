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

When a third + a fourth + ... call site appears, **add to the list above instead of writing a new local helper** — the established pattern is one canonical gate, fail-closed, ERROR-logged on rejection.

---

## File Index
- `file/fileWatcher.h/.cpp`
- `file/scriptRegistry.h/.cpp`
- `file/pathConfinement.h/.cpp`
