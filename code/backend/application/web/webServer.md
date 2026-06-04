# WebServer Documentation (JarvisAgent)

## Overview

`WebServer` provides the HTTP and WebSocket interface for JarvisAgent.
It uses the **Crow** C++ microframework to:

- Serve the dashboard UI (`code/frontend/dashboard/ui/dist`)
- Serve the workflow editor UI (`code/frontend/workflow-editor/ui/dist`)
- Expose the workflow CRUD / validation / run APIs
- Expose adhoc submission + artifact retrieval for MCP agents
- Maintain live WebSocket connections for real-time updates
- Broadcast workflow run snapshots and Python runtime status to all websocket clients

See `doc/api-endpoints.md` for the full endpoint reference.

---

## Functional Description

### Core Responsibilities

- Start/stop an embedded Crow web server
- Expose REST API routes
- Handle incoming WebSocket connections
- Maintain a set of active websocket clients
- Push asynchronous events (workflow run state, Python status)
- Operate safely across threads using locks, atomics, and Crow's own threading

---

## High-Level Operation

### Startup
`Start()`:

- Marks server as running
- Submits the Crow `.run()` call to the global `ThreadPool`
- Binds on `config.json "port"` (default 8443 for TLS)
- Enables multithreaded Crow request handling

### Static UIs

| Route | Serves |
|-------|--------|
| `/` | Dashboard (`code/frontend/dashboard/ui/dist/index.html`) |
| `/editor` | Workflow editor (`code/frontend/workflow-editor/ui/dist/index.html`) |

Assets under `/assets/*` are served from the respective `dist/` trees.

### WebSocket `/ws`

#### On Connect
- Adds client connection to `m_Clients`
- Queues current workflow run snapshots for the new client
- Logs connection

#### On Disconnect
- Removes client from `m_Clients`
- Logs disconnection

#### Supported `type` values
- `"ping"` — heartbeat from the dashboard.
- `"workflow-runs-request"` — returns a full workflow-runs snapshot.
- `"quit"` — pushes an `EngineEventShutdown` event. Replies `{"type":"quit-ack","message":"Shutdown initiated."}`.

Unknown types respond with `{"error":"unknown type"}`.

---

## Broadcast Helpers

### **Broadcast(std::string const& jsonMessage)**
Sends arbitrary JSON text to **all** active websocket clients.

### **BroadcastJSON(std::string const& jsonString)**
Alias of `Broadcast`, separated for readability.

### **BroadcastPythonStatus(bool pythonRunning)**
Sends `{"type":"python-status","running":true|false}` to all websocket clients.

### **BroadcastWorkflowRunsSnapshot() / BroadcastWorkflowRunsLastSnapshot()**
Pushes workflow run state so the dashboard can reflect progress without polling.

---

## Thread-Safety Notes

- `m_Clients` is guarded by `m_Mutex`
- `m_Running` is atomic
- Crow handles request threading
- Broadcasting locks the client set only during iteration

---

## Summary

`WebServer` is a multithreaded, Crow-based communication hub that connects the JarvisAgent engine with the browser UI and MCP agents. It exposes REST APIs for workflow CRUD and adhoc submission, and provides WebSocket channels for real-time workflow state updates.

Closely integrated with:
- `WorkflowRegistry` / `WorkflowRuntimeManager` (workflow lifecycle)
- `AdhocWorkflowManager` (one-shot JCWFs submitted via MCP)
- `WebSessionManager` (cookie-based sessions for Engine auth)
- Engine event system (shutdown, log streaming)
- `PythonEnginePool` (status updates)

---

## n8n Integration

### Start Workflow from n8n
```
POST /api/integrations/n8n/start
```

**Purpose**
- Accept workflow execution requests initiated by n8n.
- Persist the raw request JSON to disk for traceability.
- Enqueue a workflow run using the provided or generated `runId`.

**Response**
Returns HTTP 202 with workflow and run identifiers.

**Disk Trace**
For each request, JarvisAgent writes:
```
workflows/<workflowId>/<taskName>/n8n/<runId>/request.json
```

The runtime context includes:
- `n8n_request_path`
- `callbackUrl` (if provided)
- All fields from the `context` object

Execution status and results are retrieved via existing workflow-run endpoints.
