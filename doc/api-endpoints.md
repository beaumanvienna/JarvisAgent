# JarvisAgent API Endpoints

Base URL: `http://localhost:8080`

All JSON endpoints return `application/json`. Errors follow the shape:
```json
{ "ok": false, "error": "<code>", "message": "<human-readable>" }
```

---

## Static / UI

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Serves the main chat UI (`web/index.html`). |
| GET | `/editor` | Serves the Workflow Editor (React SPA from `workflow-editor/ui/dist`). |
| GET | `/assets/<path>` | Serves Vite-built static assets for the editor. |
| GET | `/editor/<path>` | SPA fallback — serves the editor index for any sub-route. |

---

## Chat

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/chat` | Submit a chat message for AI processing. |

**Request body:**
```json
{ "subsystem": "ICE", "message": "your question here" }
```
**Response (200):**
```json
{ "status": "queued", "id": 42, "file": "/path/to/ISSUE_42.txt" }
```
Creates an `ISSUE_<id>.txt` file in the queue directory under the given subsystem.

---

## Status

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Returns a live system status snapshot. |

**Response (200):**
```json
{
  "ok": true,
  "workflows_registered": 1,
  "workflow_runs_active": 0,
  "session_managers_total": 12,
  "session_managers_with_inflight": 2,
  "session_managers_inflight_total": 5,
  "websocket_clients": 1
}
```

| Field | Description |
|-------|-------------|
| `workflows_registered` | Number of JCWF workflows loaded in the registry. |
| `workflow_runs_active` | Number of currently running or queued workflow runs. |
| `session_managers_total` | Total number of session managers (one per queue subdirectory). |
| `session_managers_with_inflight` | Session managers that currently have at least one AI query in flight. |
| `session_managers_inflight_total` | Total number of AI queries currently in flight across all sessions. |
| `websocket_clients` | Number of connected WebSocket clients. |

---

## Workflows — CRUD

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/workflows` | List all registered workflows. |
| POST | `/api/workflows` | Create a new workflow from a JCWF JSON body. |
| POST | `/api/workflows/reload` | Reload all workflows from the `workflows/` directory. |
| GET | `/api/workflows/<id>` | Get the raw JCWF JSON for a specific workflow. |
| PUT | `/api/workflows/<id>` | Update (overwrite) a workflow's JCWF file. |
| DELETE | `/api/workflows/<id>` | Delete a workflow's JCWF file from disk. |

### GET /api/workflows
**Response (200):**
```json
{
  "ok": true,
  "workflows": [
    { "id": "jarvisCppDocu", "label": "JarvisAgent C++ Docu Generator", "path": "/abs/path.jcwf", "manual_start": true }
  ]
}
```

### POST /api/workflows
**Request body:** Raw JCWF JSON.
**Response (201):**
```json
{ "ok": true, "id": "myWorkflow", "savedPath": "/abs/path/myWorkflow.jcwf" }
```
Returns 409 if a workflow with that id already exists.

### POST /api/workflows/reload
**Response (200):**
```json
{ "ok": true, "reloaded": true, "workflowCount": 3 }
```

### GET /api/workflows/\<id\>
**Response (200):** Raw JCWF JSON content (Content-Type: application/json).

### PUT /api/workflows/\<id\>
**Request body:** Raw JCWF JSON. The `id` in the body must match the URL parameter.
**Response (200):**
```json
{ "ok": true, "id": "myWorkflow", "savedPath": "/abs/path/myWorkflow.jcwf" }
```

### DELETE /api/workflows/\<id\>
**Response (200):**
```json
{ "ok": true, "id": "myWorkflow" }
```

---

## Workflows — Validation

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/workflows/validate` | Validate raw JCWF JSON (from request body). |
| GET | `/api/workflows/<id>/validate` | Validate a registered workflow's JCWF file on disk. |

**Response (200):**
```json
{
  "ok": true,
  "workflowId": "myWorkflow",
  "errors": [],
  "warnings": [],
  "infos": []
}
```
Each finding has: `code`, `message`, `path` (JSON pointer), `taskId`, `tier`.

---

## Workflows — Run Control & Monitoring

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/workflows/<id>/run` | Start a workflow run. Requires `manual_start: true`. |
| DELETE | `/api/workflows/<id>/clean` | Clean a workflow's queue output directory. |
| GET | `/api/workflow-runs/active` | List all currently active (running/queued) runs. |
| GET | `/api/workflow-runs/last` | Get the last completed run for each workflow. |
| GET | `/api/workflow-runs/<runId>` | Get detailed status of a specific run (including per-task state). |
| POST | `/api/workflow-runs/<runId>/cancel` | Request cancellation of an active run. |

### POST /api/workflows/\<id\>/run

**Request body (optional):**
```json
{
  "context": {
    "user_name": "Alice",
    "environment": "production"
  }
}
```
When provided, the `context` key-value pairs are seeded into the workflow run's `ContextMap` before any task executes. Tasks can resolve these values via the context lookup step of input resolution (see JC Workflow Specification §8.1). If the body is omitted or empty, the run starts with an empty context.

**Response (202):**
```json
{ "ok": true, "enqueued": true, "id": "jarvisCppDocu", "runId": "jarvisCppDocu_1771127438" }
```
Returns 403 if `manual_start` is false. Also broadcasts a `workflow-runs-snapshot` to WebSocket clients.

### DELETE /api/workflows/\<id\>/clean
**Response (200):**
```json
{ "ok": true, "id": "jarvisCppDocu" }
```
Returns 409 if the workflow is currently running.

### GET /api/workflow-runs/active
**Response (200):**
```json
{
  "ok": true,
  "runs": [
    { "runId": "...", "workflowId": "...", "state": "running", "startedAt": "...", "completedAt": "", "taskCount": 68 }
  ]
}
```

### GET /api/workflow-runs/\<runId\>
**Response (200):** Full run detail including per-task states:
```json
{
  "ok": true,
  "run": {
    "runId": "...",
    "workflowId": "...",
    "state": "succeeded",
    "startedAt": "...",
    "completedAt": "...",
    "tasks": [
      { "taskId": "...", "state": "succeeded", "attemptCount": 1, "startedAt": "...", "completedAt": "..." }
    ]
  }
}
```

### POST /api/workflow-runs/\<runId\>/cancel
**Response (202):**
```json
{ "ok": true, "cancelRequested": true, "runId": "..." }
```

---

## Integrations — n8n

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/integrations/n8n/start` | Start a workflow run triggered by n8n. |

**Request body:**
```json
{
  "workflowId": "myWorkflow",
  "runId": "optional-custom-run-id",
  "taskName": "n8n",
  "callbackUrl": "https://n8n.example.com/callback",
  "context": { "key": "value" }
}
```
Only `workflowId` is required. `runId` and `taskName` are auto-generated if omitted.
Persists the request JSON to disk for traceability. Context fields are passed to task executors.

**Response (202):**
```json
{ "ok": true, "workflowId": "myWorkflow", "runId": "...", "requestPath": "/abs/path/request.json" }
```

---

## Settings — AI Interfaces

Manage the `"API interfaces"` array in `config.json` (in-memory + persist to disk).

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/settings/ai-interfaces` | List all configured AI interfaces. |
| POST | `/api/settings/ai-interfaces` | Create a new AI interface. |
| PUT | `/api/settings/ai-interfaces/<name>` | Update an existing AI interface (by name, URL-encoded). |
| DELETE | `/api/settings/ai-interfaces/<name>` | Delete an AI interface (by name). |
| POST | `/api/settings/ai-interfaces/save` | Persist in-memory AI interfaces to `config.json` on disk. |
| POST | `/api/settings/config/reload` | Reload `config.json` from disk into memory. |

### GET /api/settings/ai-interfaces
**Response (200):**
```json
{
  "ok": true,
  "api_index": 0,
  "interfaces": [
    { "name": "...", "description": "...", "url": "...", "model": "...", "api_type": "API1", "key_name": "..." }
  ]
}
```

### POST /api/settings/ai-interfaces
**Request body:**
```json
{ "name": "optional", "url": "https://...", "model": "gpt-4", "api_type": "API1", "description": "...", "key_name": "..." }
```
`url` is required. `name` is auto-generated if omitted. Returns 409 on duplicate name.

### POST /api/settings/ai-interfaces/save
Writes the in-memory interfaces back to the `config.json` file by replacing the `"API interfaces"` array.
**Response (200):**
```json
{ "ok": true, "path": "/abs/path/config.json" }
```

### POST /api/settings/config/reload
Reloads `config.json` from disk, updating in-memory AI interfaces and API index.
**Response (200):**
```json
{ "ok": true, "interface_count": 3 }
```

---

## Settings — Key Management

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/settings/keys/status` | Check whether API keys are loaded. |
| POST | `/api/settings/keys/unlock` | Provide the master password to decrypt stored keys. |

### GET /api/settings/keys/status
**Response (200):**
```json
{ "ok": true, "status": "ok|no_password|wrong_password|no_keys_file", "message": "...", "has_providers": true }
```

### POST /api/settings/keys/unlock
**Request body:**
```json
{ "master_password": "my-secret" }
```
**Response (200):**
```json
{ "ok": true, "status": "ok", "message": "Keys unlocked successfully." }
```
Returns 401 on wrong password.

---

## Settings — Providers

Manage AI provider configurations (name, endpoint, API key, model, type).

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/settings/providers` | List all providers (API keys are **not** returned). |
| POST | `/api/settings/providers` | Create a new provider. |
| PUT | `/api/settings/providers/<name>` | Update an existing provider (partial update). |
| DELETE | `/api/settings/providers/<name>` | Delete a provider. |
| POST | `/api/settings/providers/<name>/default` | Set a provider as the default. |
| POST | `/api/settings/providers/save` | Encrypt and save all providers to the keys file. |

### GET /api/settings/providers
**Response (200):**
```json
{
  "ok": true,
  "default_provider": "openai",
  "providers": [
    { "name": "openai", "display_name": "OpenAI", "endpoint": "https://...", "default_model": "gpt-4", "api_type": "API1", "has_key": true }
  ]
}
```

### POST /api/settings/providers
**Request body:**
```json
{ "name": "anthropic", "display_name": "Anthropic", "endpoint": "https://...", "api_key": "sk-...", "default_model": "claude-3", "api_type": "API2" }
```
`name` is required. Returns 409 if the provider already exists.

### POST /api/settings/providers/save
**Request body (optional):**
```json
{ "master_password": "my-secret" }
```
Falls back to `JARVIS_MASTER_PASSWORD` environment variable if not provided in body. Verifies password against existing keys file before overwriting. Returns 403 on wrong password.

---

## Task Heartbeat

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/task/<taskId>/heartbeat` | Reset the inactivity watchdog timer for a running task. |

**Response (200):**
```json
{ "message": "Heartbeat received." }
```

**Response (404):**
```json
{ "error": "Task not found or no active watchdog." }
```

Called by task code (shell scripts, Python, etc.) to signal liveness during long-running operations. Each heartbeat resets the `timeout_ms` inactivity timer. Shell child processes receive `JARVIS_PORT` and `JARVIS_TASK_ID` environment variables for this purpose.

See **JC Workflow Specification §3.3.3** for full semantics and code examples.

---

## Shutdown

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/shutdown` | Initiate a graceful server shutdown. |

**Response (200):**
```json
{ "message": "Shutdown initiated." }
```
Triggers the same shutdown sequence as pressing `q` or Ctrl+C: global shutdown signal → two-phase parallel subsystem shutdown → watchdog safety net (6s).

---

## WebSocket — `/ws`

A persistent WebSocket connection for real-time communication.

### Client → Server Messages

| Type | Fields | Description |
|------|--------|-------------|
| `chat` | `subsystem`, `message` | Submit a chat message (same as POST /api/chat but via WS). Creates a `PROB_<id>_<ts>.txt` file. |
| `workflow-runs-request` | — | Request the current workflow runs snapshot. |

### Server → Client Messages

| Type | Description |
|------|-------------|
| `queued` | Acknowledgement of a chat message with `id` and `file` path. |
| `workflow-runs-snapshot` | Full snapshot of active runs with per-task states. Sent on request and after run/cancel actions. |
| `python-status` | Broadcast when Python engine status changes (`{ "running": true/false }`). |
| *(broadcast)* | Any JSON string queued via `Broadcast()` / `BroadcastJSON()` is drained to all clients on next `onmessage`. |
