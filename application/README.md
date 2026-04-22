# JarvisAgent Documentation (JarvisAgent Core Application)

## Overview

`JarvisAgent` is the main application class that coordinates all subsystems:
- Script file watching (`scripts/` tree)
- Python scripting (embedded interpreter)
- Web server (REST API, WebSocket, dashboard + workflow editor UIs)
- Terminal status rendering (ncurses)
- Workflow system (registry, runtime manager, trigger engine, task executors)
- AI request pool (envelope-direct `ai_call` dispatch + completion tracking)

It inherits from `Application` and implements the full application lifecycle.
A global singleton is accessible via `App::g_App`.

---

# Responsibilities

- Initialize and start all subsystem components
- Dispatch incoming filesystem and engine events
- Forward file events into `TriggerEngine` (file_watch triggers) with startup suppression
- Forward file events to `ScriptRegistry` for the `scripts/` tree
- Forward events to Python engine
- Integrate with terminal `StatusRenderer`
- Register internal task factories (e.g. `carMaintenance`)
- Register task executors (Shell, AiCall, Python, Internal, SubWorkflow, cloud)
- Initialize the workflow system (registry → executors → runtime manager → trigger engine → trigger binding)
- Two-phase graceful shutdown
- Fatal startup detection (e.g. port already in use)

---

# Lifecycle Methods

## OnStart()

Executed once at application start:

1. Capture startup timestamp (`m_StartupTime`)
2. Register internal task factories (`m_InternalTaskRegistry`)
3. Hook `StatusRenderer` callbacks into `TerminalManager` (dynamic status lines + height)
4. Start subsystems:
   - `WebServer` — REST API + WebSocket on port 8443 (or `config.json "port"`). If the port is in use, sets `m_FatalStartupMessage` and `m_IsFinished = true` (immediate shutdown)
   - `PythonEnginePool` — loads `scripts/main.py`, calls `OnStart()` if init succeeded
   - `AiRequestPool`
   - `CurlMultiDispatcher`
   - `ScriptRegistry` + a dedicated `FileWatcher` for the `scripts/` tree
5. Call `InitializeWorkflows()` (see below)

### InitializeWorkflows()

Called at the end of `OnStart()`:

1. Create `WorkflowRegistry`, load all `.jcwf` files from the workflows directory, validate them
2. Register task executors in `TaskExecutorRegistry`:
   - `ShellTaskExecutor` → `TaskType::Shell`
   - `AiCallTaskExecutor` → `TaskType::AiCall`
   - `PythonTaskExecutor` → `TaskType::Python`
   - `InternalTaskExecutor` → `TaskType::Internal` (wraps `m_InternalTaskRegistry` with a no-op deleter)
   - `SubWorkflowTaskExecutor` → `TaskType::SubWorkflow`
3. Register cloud connectors and task executors (see `application/cloud/README.md`)
4. Wire `WebServer` ↔ `WorkflowRegistry`
5. Create and start `WorkflowRuntimeManager`, wire it to `WebServer`
6. Late-bind `WorkflowRuntimeManager` to `SubWorkflowTaskExecutor`
7. Create `TriggerEngine` (which owns its own `FileWatcher` for `file_watch` triggers) with a callback that enqueues workflow runs via `WorkflowRuntimeManager`
8. Bind all JCWF triggers into `TriggerEngine` via `WorkflowTriggerBinder`

---

## OnUpdate()

Runs every iteration of the engine loop:

1. Tick `AiRequestPool::Update()` (file-activity watchdog + deadline timeouts)
2. Tick `TriggerEngine::Tick(now)` (evaluate cron-based and cloud-watch triggers)
3. Tick `WorkflowRuntimeManager::Update()` — if it returns true (state changed), broadcast `workflow-runs-snapshot` and `workflow-runs-last-snapshot` to WebSocket clients
4. `CheckIfFinished()` — termination logic

---

## OnEvent(shared_ptr\<Event\>&)

Central event router. Processes events in this order:

### 1. Engine events
- `EngineEvent::EngineEventShutdown` → sets `m_IsFinished = true`

### 2. File events
- `FileAddedEvent`, `FileModifiedEvent`, `FileRemovedEvent` → captures `filePath` and `fileEventType`

### 3. Python crash
- `PythonCrashedEvent` → logs critical error, stops Python engine

### 4. TriggerEngine file forwarding
File events are forwarded to `TriggerEngine::NotifyFileEvent()` for `file_watch` triggers, with **startup suppression**: during the first 10 seconds after startup, files whose `last_write_time` predates `m_StartupTime` are silently ignored (prevents stale pre-existing files from firing triggers).

### 5. Script registry routing
Events on files under `scripts/` are routed to `ScriptRegistry::AddOrUpdate` / `Remove`.

### 6. Python forwarding
All events are forwarded to `PythonEnginePool::OnEvent()`.

---

## OnShutdown()

Two-phase graceful shutdown:

### Phase 1 — Signal all subsystems (non-blocking)
- `Core::g_Core->SignalShutdown()` — sets global flag, stops thread pool
- `AiRequestPool::Shutdown()`
- `WorkflowRuntimeManager::SignalStop()`
- `PythonEnginePool::SignalStop()`
- `ScriptFileWatcher::SignalStop()`
- `WebServer::SignalStop()`

### Phase 2 — Wait for all subsystems (blocking, parallel)
- `WorkflowRuntimeManager::WaitStop()` → reset
- `CurlMultiDispatcher::WaitStop()` → reset
- `AiRequestPool` → reset
- Clear `App::g_App`
- `PythonEnginePool::WaitStop()` → reset
- `ScriptFileWatcher::WaitStop()`
- `WebServer::WaitStop()`

---

## IsFinished()

Returns `true` if the app received an `EngineEvent` shutdown request or if a fatal startup error occurred (e.g. the configured port is already in use).

---

# Subsystems Managed

### ScriptFileWatcher
Observes the `scripts/` tree (100ms poll interval) and produces `FileAdded`, `FileModified`, `FileRemoved` events that feed `ScriptRegistry`.

### WebServer
Provides REST API and WebSocket endpoints for:
- Workflow CRUD, validation, run control
- AI interface and provider management
- Dashboard and workflow editor UIs
- Log viewer and run analysis
- Adhoc workflow submission (MCP)

### PythonEnginePool
Pool of sub-interpreters with per-engine GIL. Loads `scripts/main.py`, delivers events via `OnEvent()`.

### WorkflowRegistry
Loads, validates, and stores all `.jcwf` workflow definitions from the workflows directory. Provides CRUD operations for workflows.

### WorkflowRuntimeManager
Executes workflow runs: enqueues runs, dispatches tasks according to the DAG, tracks per-task state, handles pause/resume/stop/cancel. Broadcasts state changes to the WebServer.

### TriggerEngine
Evaluates workflow triggers (cron, file_watch, manual, webhook, s3_watch, onedrive_watch, email_watch, azure_blob_watch, gcs_watch) and fires callbacks to enqueue workflow runs. Owns a dedicated `FileWatcher` for `file_watch` triggers so they can observe arbitrary declared paths.

### AiRequestPool
Receives `AiInvocation` envelopes via `Submit(envelope, callback)`, dispatches them through `CurlMultiDispatcher`, writes `<prob>.output.{txt,json}` + `<prob>.transcript.json`, and matches path-based completions for workflow-bound entries.

### CurlMultiDispatcher
HTTP/2 multiplexing I/O thread shared by all AI invocations. One TCP/TLS connection per host, concurrent streams multiplexed in-place.

### InternalTaskRegistry
Registry of internal task factories (e.g. `carMaintenance`). Tasks are created via registered factory functions.

### TaskExecutorRegistry (singleton)
Maps `TaskType` → `ITaskExecutor`. Registered executors:
- `ShellTaskExecutor` — fork/exec with watchdog, `JARVIS_PORT`/`JARVIS_TASK_ID` env vars
- `AiCallTaskExecutor` — builds `AiInvocation` envelopes and calls `AiRequestPool::Submit`
- `PythonTaskExecutor` — embedded Python execution
- `InternalTaskExecutor` — delegates to `InternalTaskRegistry` factories
- `SubWorkflowTaskExecutor` — nested workflow execution
- cloud task executors — see `application/cloud/README.md`

### StatusRenderer
Draws dynamic ncurses terminal status window showing workflow run state, active queries, and completed queries.

---

# Public API

| Method | Returns | Description |
|--------|---------|-------------|
| `GetWebServer()` | `WebServer*` | Access the web server |
| `GetStartupTime()` | `time_point` | Application start time |
| `GetStartupTimestamp()` | `int64_t` | Startup time as nanoseconds since epoch |
| `GetStatusRenderer()` | `StatusRenderer&` | Terminal status renderer |
| `GetPythonEnginePool()` | `PythonEnginePool*` | Embedded Python engine pool |
| `GetWorkflowRegistry()` | `WorkflowRegistry*` | Workflow definitions |
| `GetScriptRegistry()` | `ScriptRegistry*` | In-tree script catalog |
| `GetAiRequestPool()` | `AiRequestPool*` | In-flight AI request tracker |
| `GetCurlMultiDispatcher()` | `CurlMultiDispatcher*` | Shared HTTP/2 dispatcher |
| `GetWorkflowRuntimeManager()` | `WorkflowRuntimeManager*` | Workflow execution engine |
| `GetInternalTaskRegistry()` | `IInternalTaskRegistry*` | Internal task factory registry |

---

# Application State

### Startup Timestamp
Used to filter out stale `.output` files written before the current run. Also used for startup suppression of file_watch triggers (10-second grace period).

---

# Application Class (Base)

```
class Application {
    virtual void OnStart() = 0;
    virtual void OnUpdate() = 0;
    virtual void OnEvent(std::shared_ptr<Event>&) = 0;
    virtual void OnShutdown() = 0;
    virtual bool IsFinished() const = 0;
};
```

JarvisAgent implements all of these. A static `Create()` factory returns a `unique_ptr<Application>`.

---

# Global Singleton

```cpp
class App {
public:
    static JarvisAgent* g_App;
};
```

Set during `OnStart()`, cleared during `OnShutdown()`. Used by subsystems (e.g. `WebServer`, `AiRequestPool`) to access the application instance.

---

# Summary

`JarvisAgent` is the central orchestrator:
- Bridges engine → workflow system → Python → web → filesystem
- Manages 10+ subsystems with a two-phase parallel shutdown
- Dispatches `ai_call` tasks through the typed `AiInvocation` envelope directly to `AiRequestPool::Submit`
- Provides the full execution pipeline for AI-driven workflow automation
