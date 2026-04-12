# JarvisAgent Documentation (JarvisAgent Core Application)

## Overview

`JarvisAgent` is the main application class that coordinates all subsystems:
- File watching (queue directory monitoring)
- Session management (per-folder AI query state machines)
- Python scripting (embedded interpreter)
- Web server (REST API, WebSocket, dashboard + workflow editor UIs)
- Chat message handling
- Terminal status rendering (ncurses)
- Workflow system (registry, runtime manager, trigger engine, task executors)
- AI request pool (workflow `ai_call` task completion tracking)

It inherits from `Application` and implements the full application lifecycle.
A global singleton is accessible via `App::g_App`.

---

# Responsibilities

- Initialize and start all subsystem components
- Dispatch incoming filesystem and engine events
- Maintain per-session `SessionManager` instances (one per queue folder, created on demand)
- Handle chat workflow (`PROB_<id>_<ts>` and `PROB_<id>_<ts>_output` files)
- Route PROB output files through `AiRequestPool` first, then `ChatMessagePool`
- Route `.output` files via path-based AI completion matching
- Forward file events into `TriggerEngine` (file_watch triggers) with startup suppression
- Forward events to Python engine
- Integrate with terminal `StatusRenderer`
- Register internal task factories (e.g. `carMaintenance`)
- Register task executors (Shell, AiCall, Python, Internal)
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
   - `FileWatcher` — monitors the queue directory (from `config.json "queue folder"`)
   - `WebServer` — REST API + WebSocket on port 8080. If the port is in use, sets `m_FatalStartupMessage` and `m_IsFinished = true` (immediate shutdown)
   - `ChatMessagePool`
   - `PythonEngine` — loads `scripts/main.py`, calls `OnStart()` if init succeeded
   - `AiRequestPool`
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
3. Register cloud connectors and task executors:
   - `PolarionConnector` + `PolarionWriteTaskExecutor` → `TaskType::PolarionWrite`
   - `S3Connector` + `S3CloudTaskExecutor` → `TaskType::S3`
   - `PostgresConnector` + `DbQueryCloudTaskExecutor` → `TaskType::DbQuery`
   - `OneDriveConnector` + `OneDriveCloudTaskExecutor` → `TaskType::OneDrive`
   - `SnowflakeConnector` + `SnowflakeCloudTaskExecutor` → `TaskType::SnowflakeQuery`
   - `SlackConnector` + `SlackCloudTaskExecutor` → `TaskType::SlackMessage`
   - `EmailConnector` + `EmailCloudTaskExecutor` → `TaskType::EmailSend`
   - `GitHubConnector` + `GitHubCloudTaskExecutor` → `TaskType::GitHubIssue`
   - `JiraConnector` + `JiraCloudTaskExecutor` → `TaskType::JiraIssue`
   - `GoogleSheetsConnector` + `GoogleSheetsCloudTaskExecutor` → `TaskType::SheetsRead`, `TaskType::SheetsWrite`
   - `AzureBlobConnector` + `AzureBlobCloudTaskExecutor` → `TaskType::AzureBlob`
   - `GcsConnector` + `GcsCloudTaskExecutor` → `TaskType::Gcs`
4. Wire `WebServer` ↔ `WorkflowRegistry`
5. Create and start `WorkflowRuntimeManager`, wire it to `WebServer`
6. Late-bind `WorkflowRuntimeManager` to `SubWorkflowTaskExecutor`
7. Create `TriggerEngine` with a callback that enqueues workflow runs via `WorkflowRuntimeManager`
8. Bind all JCWF triggers into `TriggerEngine` via `WorkflowTriggerBinder`

---

## OnUpdate()

Runs every iteration of the engine loop:

1. Update all `SessionManager` instances (AI query state machines)
2. Remove expired chat messages (`ChatMessagePool::RemoveExpired()`)
3. Tick `AiRequestPool::Update()` (check for completed AI requests)
4. Tick `TriggerEngine::Tick(now)` (evaluate cron-based triggers)
5. Tick `WorkflowRuntimeManager::Update()` — if it returns true (state changed), broadcast `workflow-runs-snapshot` and `workflow-runs-last-snapshot` to WebSocket clients
6. `CheckIfFinished()` — termination logic

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

### 5. PROB file handling (AI completion routing)
For files matching `PROB_<id>_<ts>` naming:
- **Stale check:** files with timestamp before startup are ignored
- **Output files (`PROB_*_output`):** routed to `AiRequestPool::OnProbFileEvent()` first (workflow `ai_call` completion). If not claimed, falls through to `ChatMessagePool::MarkAnswered()`

### 6. Path-based AI completion routing
For files with `.output` suffix in the stem (e.g. `PROB_NVDA.output.txt`):
- Stale file guard (pre-startup `last_write_time` → ignore)
- Routed to `AiRequestPool::OnOutputFileCreated()` for path-based workflow completion matching

### 7. SessionManager dispatch
Remaining file events are forwarded to the `SessionManager` for the file's parent directory. If no `SessionManager` exists for that path, one is created on demand.

### 8. Python forwarding
All events are forwarded to `PythonEngine::OnEvent()`.

---

## OnShutdown()

Two-phase graceful shutdown:

### Phase 1 — Signal all subsystems (non-blocking)
- `Core::g_Core->SignalShutdown()` — sets global flag, stops thread pool
- `AiRequestPool::Shutdown()`
- `WorkflowRuntimeManager::SignalStop()`
- `PythonEngine::SignalStop()`
- `FileWatcher::SignalStop()`
- `WebServer::SignalStop()`

### Phase 2 — Wait for all subsystems (blocking, parallel)
- `WorkflowRuntimeManager::WaitStop()` → reset
- `AiRequestPool` → reset
- Clear `App::g_App`
- All `SessionManager::OnShutdown()`
- `PythonEngine::WaitStop()` → reset
- `FileWatcher::WaitStop()`
- `WebServer::WaitStop()`

---

## IsFinished()

Returns `true` if the app received an `EngineEvent` shutdown request or if a fatal startup error occurred (e.g. port 8080 already in use).

---

# Subsystems Managed

### FileWatcher
Observes the queue directory (100ms poll interval) and produces `FileAdded`, `FileModified`, `FileRemoved` events.

### SessionManager
Per-folder state machines (created on demand, keyed by relative directory path) that:
- Track environment/settings/context/tasks
- Dispatch AI queries
- Write output files
- Broadcast session status

### WebServer
Provides REST API and WebSocket endpoints for:
- Chat messages
- Workflow CRUD, validation, run control
- AI interface and provider management
- Dashboard and workflow editor UIs
- Log viewer and run analysis

### ChatMessagePool
Manages incoming chat texts and outgoing responses with expiration-based cleanup.

### PythonEngine
Loads `scripts/main.py`, initializes the embedded Python interpreter, delivers events via `OnEvent()`.

### WorkflowRegistry
Loads, validates, and stores all `.jcwf` workflow definitions from the workflows directory. Provides CRUD operations for workflows.

### WorkflowRuntimeManager
Executes workflow runs: enqueues runs, dispatches tasks according to the DAG, tracks per-task state, handles pause/resume/stop/cancel. Broadcasts state changes to the WebServer.

### TriggerEngine
Evaluates workflow triggers (cron, file_watch, manual, webhook, s3_watch, onedrive_watch, email_watch) and fires callbacks to enqueue workflow runs. Receives file events from `OnEvent()` and time ticks from `OnUpdate()`.

### AiRequestPool
Tracks in-flight `ai_call` task requests. Matches PROB output files and path-based `.output` files to pending requests for workflow task completion.

### InternalTaskRegistry
Registry of internal task factories (e.g. `carMaintenance`). Tasks are created via registered factory functions.

### TaskExecutorRegistry (singleton)
Maps `TaskType` → `ITaskExecutor`. Registered executors:
- `ShellTaskExecutor` — fork/exec with watchdog, `JARVIS_PORT`/`JARVIS_TASK_ID` env vars
- `AiCallTaskExecutor` — AI provider requests via `AiRequestPool`
- `PythonTaskExecutor` — embedded Python execution
- `InternalTaskExecutor` — delegates to `InternalTaskRegistry` factories
- `SubWorkflowTaskExecutor` — nested workflow execution
- 10 cloud task executors — see `application/cloud/README.md`

### StatusRenderer
Draws dynamic ncurses terminal status window showing session states, active queries, and completed queries.

---

# Public API

| Method | Returns | Description |
|--------|---------|-------------|
| `GetWebServer()` | `WebServer*` | Access the web server |
| `GetChatMessagePool()` | `ChatMessagePool*` | Access chat messages |
| `GetStartupTime()` | `time_point` | Application start time |
| `GetStartupTimestamp()` | `int64_t` | Startup time as nanoseconds since epoch |
| `GetStatusRenderer()` | `StatusRenderer&` | Terminal status renderer |
| `GetPythonEngine()` | `PythonEngine*` | Embedded Python engine |
| `GetWorkflowRegistry()` | `WorkflowRegistry*` | Workflow definitions |
| `GetAiRequestPool()` | `AiRequestPool*` | In-flight AI request tracker |
| `GetWorkflowRuntimeManager()` | `WorkflowRuntimeManager*` | Workflow execution engine |
| `GetInternalTaskRegistry()` | `IInternalTaskRegistry*` | Internal task factory registry |
| `GetSessionManagerCount()` | `size_t` | Number of active session managers |
| `GetSessionManagerInflightTotal()` | `size_t` | Total in-flight AI queries across all sessions |
| `GetSessionManagersWithInflight()` | `size_t` | Number of sessions with at least one in-flight query |
| `ForEachSessionManager(Func)` | — | Iterate all session managers |

---

# Application State

### Startup Timestamp
Used to filter out stale `PROB_*` files and `.output` files written before the current run. Also used for startup suppression of file_watch triggers (10-second grace period).

### SessionManagers Map
Key: relative session directory path (e.g. `queue/jarvisCppDocu/01_doc_engine_h`)
Value: unique `SessionManager` instance (created on demand)

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
- Routes file events through TriggerEngine → AiRequestPool → ChatMessagePool → SessionManager
- Provides the full execution pipeline for AI-driven workflow automation
