# PythonEngine + PythonEnginePool

## Overview

`PythonEngine` embeds CPython inside JarvisAgent. The runtime spins up an N-engine **pool** of CPython sub-interpreters (PEP 684, Python 3.12+; graceful single-engine fallback on older Python) — each engine owns a dedicated worker thread and processes tasks asynchronously. Workflow `python` tasks are load-balanced across the pool by queue depth; the primary engine (index 0) additionally runs application-level lifecycle hooks (`OnStart`, `OnEvent`, `OnShutdown`).

The two responsible classes:

- `PythonEngine` (`pythonEngine.{h,cpp}`) — one CPython sub-interpreter, one worker thread, one task queue.
- `PythonEnginePool` (`pythonEnginePool.{h,cpp}`) — owns the engines, runs the global `Py_Initialize`, dispatches workflow tasks via `SelectEngine()` (smallest queue depth), routes lifecycle hooks to the primary engine.

---

## Responsibilities

- Initialize and finalize CPython once per process; create / destroy sub-interpreters per engine.
- Import a user-provided startup script (e.g. `scripts/main.py`) into the primary engine and discover optional hooks (`OnStart`, `OnUpdate`, `OnEvent`, `OnShutdown`).
- Execute workflow `python` tasks asynchronously on whichever engine is least loaded.
- Redirect Python `sys.stdout` / `sys.stderr` to `JarvisRedirectPython()` so all script output flows through the application logger.
- Handle the GIL correctly: the worker thread holds its own `PyThreadState` for its sub-interpreter; the GIL is acquired/released around each task body.

---

## Security contracts

`PythonEngine` enforces three runtime gates that workflow authors must satisfy:

1. **Module allowlist.** `params.module` is validated against `ScriptRegistry::FindByModulePath` before `PyImport_ImportModule`. Only modules whose `.py` file under `scripts/` carries a `# @jarvis-script` header (and is therefore in the registry) can be imported. Standard-library modules (`os`, `subprocess`, `ctypes`) are rejected. The registry pointer is wired into the pool at `Initialize` time and propagated to every engine; a null registry causes `Initialize` to fail rather than silently disabling the gate.
2. **`scriptDir` sys.path confinement.** `SetupSubInterpreter` resolves the configured `scriptDir` via `fs::weakly_canonical` against the project root and rejects paths that escape via `..`, absolute mismatch, or symlinks pointing out of tree. Setup fails (returns `false`) on rejection.
3. **`taskWorkingDirectory` confinement.** Each task's working directory is canonicalised against the project root before being inserted into the Python `context["_task_working_directory"]` slot. An attacker-influenced or buggy upstream value that escapes the project root fails the task with a structured error before any user Python runs.

See `doc/cyber security.md` and `doc/architecture.md` "Key Design Decisions" for the threat model and rationale.

---

## Lifetime invariants

- **`m_InterpreterState`** — set once by `PythonEnginePool::Initialize` (via `SetInterpreterState`) and read by the worker thread under the GIL. The pointed-to `PyInterpreterState` has process lifetime: sub-interpreters are torn down only by `Py_Finalize` at process exit, and the pool intentionally keeps the main-thread state alive (see the `Initialize` comment). The worker thread's `PyThreadState_New(m_InterpreterState)` therefore always operates on a live interpreter. An assertion immediately before `PyThreadState_New` makes this contract explicit; a misordered call site (e.g. `StartWorkerThread` before `SetInterpreterState`) trips it loudly in Debug.
- **`m_ScriptRegistry`** — borrowed pointer owned by `JarvisAgent`. The pool is destroyed before the registry, so the pointer is valid for the engine's whole lifetime.
- **`WorkflowTaskRequest::m_InputValues` / `m_ContextValues`** — owned-by-value copies of the caller's maps. The previous design held raw `const*` to caller stack frames; the new owned form is safe even if a future call site switches to fire-and-forget (capture-by-value into async work sites is the codebase rule — references into caller-stack data are a use-after-free waiting to happen).

---

## Concurrency contracts

### `PythonEngine` (per-engine state)

- **`m_Running`** (`std::atomic<bool>`, acquire/release) — set by `StartWorkerThread`, cleared by `WaitStop`, read by every public API early-return guard (`IsRunning`, `OnStart`, `OnUpdate`, `OnEvent`, `SignalStop`, `ExecuteWorkflowTask`). Atomic so cross-thread reads see the right value without depending on `m_QueueMutex`.
- **`m_StopRequested`** (`std::atomic<bool>`, acquire/release) — guarded by `m_QueueMutex` on every existing site; the atomic semantics are explicit in case a future caller needs to read without the lock.
- **`m_TasksCompleted`** (`std::atomic<size_t>`, relaxed) — monitoring counter only, no synchronization role.
- **`m_TaskQueue`** + **`m_QueueCondition`** — guarded by `m_QueueMutex`. Worker waits on the condition variable; producers `notify_one()` after pushing.
- **`WorkflowTaskRequest::m_PromiseSatisfied`** (`std::atomic<bool>`, acq/rel) — guards against a double `set_value` race between the worker-loop main path and the shutdown drain. First writer wins via CAS; subsequent attempts no-op cleanly. Without this guard, a shutdown-mid-task interleave would throw `std::future_error: broken_promise`.

### `PythonEnginePool` (collection-level state)

- **`m_Running`** (`std::atomic<bool>`, acquire/release) — release-stored at the END of `Initialize` (so any reader that observes `true` also sees the fully-populated `m_Engines`), released-stored to false at the START of `SignalStop` (so concurrent readers can bail before tear-down begins). All public methods load it first; `ExecuteWorkflowTask` / hooks short-circuit on false.
- **`m_Mutex`** — guards `m_Engines` mutation only. Held by `Initialize` for each `push_back`, by `SignalStop`/`WaitStop` for the iteration + clear, by `GetEngineCount` / `GetTasksCompleted` for the read. After `Initialize` returns true and before `SignalStop` is invoked, `m_Engines` is stable; `SelectEngine` reads it lock-free on the hot path so dispatch is not serialized across worker threads. The atomic+mutex split matches the pool's lifecycle: heavy/rare init+shutdown under the lock, frequent steady-state dispatch lock-free with explicit ordering.
- **Non-copyable + non-movable** — `=delete` on copy/move ctor + assignment. The engines own raw Python interpreter state that must not migrate.

---

## Public API

### `PythonEnginePool::Initialize(scriptPath, engineCount, scriptRegistry)`
Bootstraps CPython once, creates `engineCount` sub-interpreters via `Py_NewInterpreterFromConfig` (3.12+) or `Py_NewInterpreter` (legacy), wires `scriptRegistry` into every engine, calls `SetupSubInterpreter` on each. Returns `false` if any required argument is missing (`scriptRegistry == nullptr` is rejected up front), if `Py_Initialize` fails, if the resolved scriptDir does not pass `ConfineUnderProjectRoot` (defense-in-depth at the pool boundary; the per-engine `SetupSubInterpreter` also gates), or if no sub-interpreter sets up cleanly. `SetInterpreterState` is invoked **only after** `SetupSubInterpreter` returns true — a failed engine never has its state pointer wired in. Marked `[[nodiscard]]`.

### `PythonEnginePool::ExecuteWorkflowTask(taskDefinition, taskWorkingDirectory, workflowId, runId, inputValues, contextValues, ...outputs...)`
Selects the least-loaded engine and forwards the call. Builds a `WorkflowTaskRequest`, copies the caller's maps into it, enqueues it on the chosen engine's task queue, blocks on the request's promise, and returns the result.

### `PythonEnginePool::OnStart` / `OnUpdate` / `OnEvent`
Routed to the primary engine (index 0) only. Enqueue an `OnStart` / `OnUpdate` / `OnEvent` task on that engine's queue.

### `PythonEnginePool::Stop()` / `SignalStop()` / `WaitStop()`
Three-phase shutdown: signal → wait → finalize. `Stop()` is the convenience wrapper. Each engine drains its queue (any remaining `WorkflowTask` requests get a `"PythonEngine shutting down"` error and are completed via the promise guard) before the worker thread joins.

---

## Hook discovery (primary engine only)

`SetupSubInterpreter(scriptDir, moduleName, loadHooks=true)` on engine 0:

1. Validates `scriptDir` (path-confinement gate).
2. Adds the canonical `scriptDir` and its parent to `sys.path` so both bare imports (`combineDocumentation`) and dotted-package imports (`scripts.combineDocumentation`) work.
3. Imports `moduleName` (`scripts.main` by default).
4. Looks up `OnStart`, `OnUpdate`, `OnEvent`, `OnShutdown` in the module's globals; missing hooks are logged at INFO and skipped.

Engines 1..N skip step 3-4 (`loadHooks=false`) — they exist purely to handle workflow `python` tasks.

---

## Failure-path discipline

Every `fail()` path inside `ExecuteWorkflowTaskOnWorker` emits `LOG_APP_ERROR` with `runId` / `workflowId` / `taskId` literals so the dashboard's Run Analyzer can attribute the failure (per `CLAUDE.md`'s fail-path discipline). Public-API early returns in `ExecuteWorkflowTask` (`!m_Running`, missing params) also log at ERROR. Python exception messages from `consumePythonException` are passed through `SanitizeUtf8` + truncated to 4 KiB before they enter `m_ErrorMessage` or any log line — defends downstream consumers (dashboard JSON, ncurses TUI) against malformed bytes from misconfigured locales or attacker-influenced traceback content.

---

## Calling convention for Python tasks

The runtime calls `module.function(**kwargs, context=dict)` programmatically. Scripts are NOT invoked via CLI — do not use `sys.argv`, `argparse`, or `if __name__ == "__main__":` as the entry point.

`kwargs` carries the resolved task inputs (workflow DAG outputs flow through here). `context` is always attached and contains:

- `context["_task_id"]` — task identifier from the JCWF
- `context["_task_working_directory"]` — canonicalised absolute path to the task's working directory (post-confinement)
- Plus any caller-supplied entries (typical: `_workflow_id`, `_run_id`, `_workflow_base_directory`, `_file_input_<N>`)

Return `None` for no outputs, or a `dict[str, str]` to expose values as output slots (the runtime extracts every key+value as UTF-8 strings).

See `doc/JC_Workflow_Specification.md` §3.3 / §11 for the JCWF-side contract and the `@jarvis-script` header format that registers a script.

---

## Output redirection

All Python `print()` / `sys.stderr.write()` lands in `JarvisRedirectPython(message)` (a C entry point that forwards into the JarvisAgent log). Per-task `_jarvis_cap_stdout` / `_jarvis_cap_stderr` `StringIO` tees additionally capture each task's output for the dashboard's per-task captured-output panel and the on-disk `stdout.txt` / `stderr.txt` files in the task working directory.

`JarvisPyStatus(message)` is a separate C entry point that pushes a `PythonCrashedEvent` into the engine event queue — used by Python code that hits an unrecoverable error and wants to signal the engine.
