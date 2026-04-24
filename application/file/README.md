# JarvisAgent File System Subsystem Documentation

## Overview
The file subsystem provides:
- Asynchronous directory monitoring (`FileWatcher`).
- Script catalog population and lookup (`ScriptRegistry`).

It consists of:
- `FileWatcher` — polling watcher that emits filesystem events on the global event queue.
- `ScriptRegistry` — in-memory catalog of `@jarvis-script` metadata parsed from files under `scripts/`.

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

## File Index
- `file/fileWatcher.h/.cpp`
- `file/scriptRegistry.h/.cpp`
