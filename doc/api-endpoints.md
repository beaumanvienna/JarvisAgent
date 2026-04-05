# JarvisAgent API Endpoints

Base URL: `http://localhost:8080`

All JSON endpoints return `application/json`. Errors follow the shape:
```json
{ "ok": false, "error": "<code>", "message": "<human-readable>" }
```

### Edition availability

JarvisAgent ships in two editions: **Engine** (lean production server) and **Studio** (full developer IDE). Each endpoint section is annotated with its edition availability. Studio-only endpoints return 404 in Engine mode.

### Authentication (Engine edition)

Engine mode protects all endpoints except health checks and static assets with Bearer token authentication. Studio mode has no authentication (developer workstation).

| Tier | Endpoints | Engine Auth | Studio Auth |
|------|-----------|-------------|-------------|
| Public | `GET /api/status`, `GET /`, `/dash-assets/*` | None | None |
| Webhook | `POST /api/webhook/<id>` | HMAC-SHA256 (required) | HMAC-SHA256 (optional) |
| Admin | All other endpoints | `Authorization: Bearer <token>` | None |
| WebSocket | `WS /ws` | Token-as-first-message | None |

On first Engine start, a 256-bit random token is auto-generated, saved to `engine_api_token.txt` (file permissions `600`), and logged to stdout.

Unauthenticated admin requests return `401 Unauthorized` with `WWW-Authenticate: Bearer` header. Wrong tokens return `403 Forbidden`. Rate-limited requests return `429 Too Many Requests` with `Retry-After` header.

---

## Static / UI — Both editions (dashboard); Studio only (editor)

| Method | Path | Edition | Description |
|--------|------|---------|-------------|
| GET | `/` | Both | Serves the dashboard (React SPA from `dashboard/ui/dist`). |
| GET | `/editor` | Studio | Serves the Workflow Editor (React SPA from `workflow-editor/ui/dist`). |
| GET | `/assets/<path>` | Studio | Serves Vite-built static assets for the editor. |
| GET | `/editor/<path>` | Studio | SPA fallback — serves the editor index for any sub-route. |

---

## Chat — Studio only

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

## Status — Both editions

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Returns a live system status snapshot. |

**Response (200):**
```json
{
  "ok": true,
  "edition": "engine",
  "capabilities": {
    "workflow_crud": false,
    "workflow_run_endpoint": false,
    "ai_assistant": false,
    "ai_jcwf": false,
    "settings_api": false
  },
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
| `edition` | `"engine"` or `"studio"`. Used by the frontend to gate UI elements. |
| `capabilities` | Boolean map of feature availability. Engine: all `false`. Studio: all `true`. |
| `workflows_registered` | Number of JCWF workflows loaded in the registry. |
| `workflow_runs_active` | Number of currently running or queued workflow runs. |
| `session_managers_total` | Total number of session managers (one per queue subdirectory). |
| `session_managers_with_inflight` | Session managers that currently have at least one AI query in flight. |
| `session_managers_inflight_total` | Total number of AI queries currently in flight across all sessions. |
| `websocket_clients` | Number of connected WebSocket clients. |

---

## Workflows — CRUD — read-only (Both), mutating (Studio only)

| Method | Path | Edition | Description |
|--------|------|---------|-------------|
| GET | `/api/workflows` | Both | List all registered workflows. |
| POST | `/api/workflows` | Studio | Create a new workflow from a JCWF JSON body. |
| POST | `/api/workflows/reload` | Studio | Reload all workflows from the `workflows/` directory. |
| GET | `/api/workflows/<id>` | Both | Get the raw JCWF JSON for a specific workflow. |
| PUT | `/api/workflows/<id>` | Studio | Update (overwrite) a workflow's JCWF file. |
| DELETE | `/api/workflows/<id>` | Studio | Delete a workflow's JCWF file from disk. |

### GET /api/workflows
**Response (200):**
```json
{
  "ok": true,
  "workflows": [
    {
      "id": "jarvisCppDocu",
      "label": "JarvisAgent C++ Docu Generator",
      "path": "/abs/path/jarvisCppDocu.json",
      "manual_start": true,
      "is_sub_workflow": false,
      "container_path": "/abs/path/jarvisCppDocu.jcwf"
    }
  ]
}
```
Sub-workflows loaded from a container also appear in the list with `is_sub_workflow: true`, `parent_workflow_id`, and `container_folder`.

### POST /api/workflows
**Request body:** Raw JCWF JSON (canvas with tasks/dataflow). The backend creates a `.jcwf` zip container with `global.json` and canvas JSON.
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
**Response (200):** Canvas JSON content (Content-Type: application/json). For container workflows, returns the canvas JSON from the extracted folder.

### PUT /api/workflows/\<id\>
**Request body:** Raw JCWF JSON. The `id` in the body must match the URL parameter. The backend updates the canvas JSON in the extracted folder and repacks the `.jcwf` container.
**Response (200):**
```json
{ "ok": true, "id": "myWorkflow", "savedPath": "/abs/path/myWorkflow.jcwf" }
```

### DELETE /api/workflows/\<id\>
Deletes both the `.jcwf` zip container and the extracted directory.
**Response (200):**
```json
{ "ok": true, "id": "myWorkflow" }

### GET /api/workflows/\<id\>/tree
Returns the sub-workflow hierarchy for a container workflow.
**Response (200):**
```json
{
  "ok": true,
  "workflowId": "myPipeline",
  "label": "My Pipeline",
  "isContainer": true,
  "children": [
    { "id": "myPipeline__cleanup", "label": "cleanup", "folderPath": "cleanup", "parentId": "myPipeline" }
  ]
}
```

### GET /api/workflows/dependency-graph
Returns the cross-workflow sub-workflow dependency graph.
**Response (200):**
```json
{ "ok": true, "edges": [{ "parent": "parentId", "child": "childId" }] }
```

---

## Workflows — Validation — Studio only

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

## Scripts — Validation — Studio only

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/scripts/check?path=<scriptPath>` | Check if a script exists and is executable. |

### GET /api/scripts/check

Used by the Workflow Editor to provide real-time validation warnings for shell tasks.

**Query parameters:**

| Param | Required | Description |
|-------|----------|-------------|
| `path` | Yes | Script path (must start with `scripts/`). Resolved relative to the JarvisAgent Launch Working Directory. |

**Response (200) — script found:**
```json
{ "ok": true, "path": "scripts/runMake.sh", "exists": true, "executable": true }
```

**Response (200) — script not found:**
```json
{ "ok": true, "path": "scripts/nonexistent.sh", "exists": false, "executable": false }
```

**Response (200) — exists but not executable:**
```json
{ "ok": true, "path": "scripts/combineDocumentation.py", "exists": true, "executable": false }
```

**Response (400) — missing path:**
```json
{ "ok": false, "error": "missing_path", "message": "Query parameter 'path' is required." }
```

**Response (400) — invalid path (not inside `scripts/`):**
```json
{ "ok": false, "error": "invalid_path", "message": "Script path must be inside the 'scripts/' directory.", "path": "noprefix.sh" }
```

**Response (400) — path escapes `scripts/` after resolving `..`:**
```json
{ "ok": false, "error": "invalid_path", "message": "Resolved script path escapes the 'scripts/' directory.", "path": "scripts/../workflows/hello" }
```

Security: the raw path must start with `scripts/` and the lexically-normalized path must remain inside the `scripts/` directory tree. Paths containing `..` are allowed as long as they resolve to a location inside `scripts/` (e.g. `scripts/helpers/../run.sh` → `scripts/run.sh`). See **JC Workflow Specification §3.1.2** (Exceptions) and **§10** (Security Considerations).

---

## File Existence Check (Workflow Editor) — Studio only

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/files/check?path=<filePath>` | Check if a file exists relative to the working directory. |

### GET /api/files/check

Used by the Workflow Editor to provide real-time validation warnings (W badge) for python and shell tasks whose static `file_inputs` reference files that do not exist on disk.

**Query parameters:**

| Param | Required | Description |
|-------|----------|-------------|
| `path` | Yes | Relative file path. Resolved relative to the JarvisAgent Launch Working Directory. Absolute paths are rejected. |

**Response (200) — file found:**
```json
{ "ok": true, "path": "OpenSSH_2k.log", "exists": true }
```

**Response (200) — file not found:**
```json
{ "ok": true, "path": "OpenSSH_2k.log", "exists": false }
```

**Response (400) — missing path:**
```json
{ "ok": false, "error": "missing_path", "message": "Query parameter 'path' is required." }
```

**Response (400) — absolute path rejected:**
```json
{ "ok": false, "error": "invalid_path", "message": "Absolute paths are not allowed.", "path": "/etc/passwd" }
```

**Response (400) — path escapes working directory:**
```json
{ "ok": false, "error": "invalid_path", "message": "Resolved path escapes the working directory.", "path": "../../etc/passwd" }
```

Security: only relative paths are accepted. The resolved path must remain inside the JarvisAgent Launch Working Directory after lexical normalization.

---

## Workflows — Run Control & Monitoring — monitoring (Both), run trigger + clean (Studio only)

| Method | Path | Edition | Description |
|--------|------|---------|-------------|
| POST | `/api/workflows/<id>/run` | Studio | Start a workflow run. Requires `manual_start: true`. |
| DELETE | `/api/workflows/<id>/clean` | Studio | Clean a workflow's queue output directory. |
| GET | `/api/workflow-runs/active` | Both | List all currently active (running/queued) runs. |
| GET | `/api/workflow-runs/last` | Both | Get the last completed run for each workflow. |
| GET | `/api/workflow-runs/<runId>` | Both | Get detailed status of a specific run (including per-task state). |
| POST | `/api/workflow-runs/<runId>/cancel` | Both | Request cancellation of an active run. |
| POST | `/api/workflow-runs/<runId>/pause` | Both | Pause an active run (suspend new task dispatch). |
| POST | `/api/workflow-runs/<runId>/resume` | Both | Resume a paused run. |
| POST | `/api/workflow-runs/<runId>/stop` | Both | Graceful stop: finish in-flight tasks, no new dispatch. |

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

### POST /api/workflow-runs/\<runId\>/pause
Suspends new task dispatch for the run. In-flight tasks continue to completion.
**Response (202):**
```json
{ "ok": true, "paused": true, "runId": "..." }
```

### POST /api/workflow-runs/\<runId\>/resume
Resumes a paused run, re-enabling task dispatch.
**Response (202):**
```json
{ "ok": true, "resumed": true, "runId": "..." }
```

### POST /api/workflow-runs/\<runId\>/stop
Graceful stop: finishes all in-flight tasks but does not dispatch any new ones. Differs from cancel in that in-flight tasks are allowed to complete normally rather than being interrupted.
**Response (202):**
```json
{ "ok": true, "stopRequested": true, "runId": "..." }
```

---

## Integrations — n8n — Both editions

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

## Settings — AI Interfaces — Studio only

Manage the `"API interfaces"` array in `config.json` (in-memory + persist to disk).

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/settings/ai-interfaces` | List all configured AI interfaces. |
| POST | `/api/settings/ai-interfaces` | Create a new AI interface. |
| PUT | `/api/settings/ai-interfaces/<name>` | Update an existing AI interface (by name, URL-encoded). |
| DELETE | `/api/settings/ai-interfaces/<name>` | Delete an AI interface (by name). |
| POST | `/api/settings/ai-interfaces/save` | Persist in-memory AI interfaces to `config.json` on disk. |
| POST | `/api/settings/ai-interfaces/test` | Ping-test a specific AI interface (direct curl, 10s timeout). |
| GET | `/api/settings/config` | Read current scalar config values + platform. |
| PUT | `/api/settings/config` | Update scalar config fields and persist to `config.json`. |
| POST | `/api/settings/config/reload` | Reload `config.json` from disk into memory. |

### GET /api/settings/ai-interfaces
**Response (200):**
```json
{
  "ok": true,
  "api_index": 0,
  "dirty": false,
  "interfaces": [
    { "name": "...", "description": "...", "url": "...", "model": "...", "api_type": "API1", "key_name": "..." }
  ]
}
```

| Field | Description |
|-------|-------------|
| `dirty` | `true` when the in-memory interfaces differ from the on-disk `config.json`. Set by create/update/delete, cleared by save and reload. Used by the editor to show an unsaved-changes badge. |

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

### POST /api/settings/ai-interfaces/test
Sends a minimal prompt ("Say hello") directly to the specified AI interface via curl with a **10-second timeout**. Bypasses the SessionManager queue-file pipeline entirely — this is a lightweight connectivity and authentication check.

**Request body:**
```json
{ "index": 0 }
```

| Field | Required | Description |
|-------|----------|-------------|
| `index` | Yes | 0-based index into the `"API interfaces"` array. |

**Response (200) — success:**
```json
{ "ok": true, "index": 0, "name": "api.openai.com/gpt-4.1/API1", "model": "gpt-4.1", "latency_ms": 1234, "response_preview": "Hello! ..." }
```

**Response (200) — failure:**
```json
{ "ok": false, "index": 0, "name": "api.openai.com/gpt-4.1/API1", "model": "gpt-4.1", "error": "404 Not Found" }
```

**Response (400) — bad request:**
```json
{ "ok": false, "error": "missing_index", "message": "Request body must contain 'index'." }
```

The frontend displays results as a colored LED indicator next to each interface: green = success, red = failure, yellow (pulsing) = testing in progress.

### POST /api/settings/config/reload
Reloads `config.json` from disk, updating in-memory AI interfaces and API index.
**Response (200):**
```json
{ "ok": true, "interface_count": 3 }
```

---

## Settings — Config — Studio only

Read and update the scalar runtime configuration fields stored in `config.json`.

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/settings/config` | Read current config values (and platform info). |
| PUT | `/api/settings/config` | Update one or more config fields and persist to `config.json`. |

### GET /api/settings/config
**Response (200):**
```json
{
  "ok": true,
  "api_index": 0,
  "max_threads": 20,
  "verbose": false,
  "max_file_size_kb": 24,
  "jcwf_batch_size": 1,
  "jcwf_ai_interface": -1,
  "use_bash": false,
  "queue_folder": "../queue",
  "workflows_folder": "../workflows",
  "platform": "linux"
}
```

| Field | Description |
|-------|-------------|
| `api_index` | Active AI interface index (into `"API interfaces"` array). |
| `max_threads` | Worker-thread pool size. |
| `verbose` | Verbose logging enabled. |
| `max_file_size_kb` | Maximum queue file size in kB. |
| `jcwf_batch_size` | JCWF generation batch size. |
| `jcwf_ai_interface` | AI interface override for JCWF operations (`-1` = use global default). |
| `use_bash` | Windows only: prefer bash over PowerShell (`false` = PowerShell default). |
| `queue_folder` | Read-only: path to the queue folder (from `config.json`). |
| `workflows_folder` | Read-only: path to the workflows folder (from `config.json`). |
| `platform` | Read-only: `"linux"`, `"macos"`, or `"windows"`. Used by the frontend to gate Windows-only UI controls (e.g. the `use_bash` checkbox). |

### PUT /api/settings/config
Updates the specified fields in memory and persists them to `config.json`.
**Request body (all fields optional):**
```json
{
  "api_index": 0,
  "max_threads": 20,
  "verbose": false,
  "max_file_size_kb": 24,
  "jcwf_batch_size": 1,
  "jcwf_ai_interface": -1,
  "use_bash": false
}
```
**Response (200):**
```json
{
  "ok": true,
  "api_index": 0,
  "max_threads": 20,
  "verbose": false,
  "max_file_size_kb": 24,
  "jcwf_batch_size": 1,
  "jcwf_ai_interface": -1,
  "use_bash": false
}
```
Returns 400 if a required field is missing or malformed. Validation errors (e.g. `api_index` out of range) return `{ "ok": false, "message": "..." }`.

---

## Settings — Key Management — Studio only

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

## Settings — Providers — Studio only

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
  "dirty": false,
  "default_provider": "openai",
  "providers": [
    { "name": "openai", "display_name": "OpenAI", "endpoint": "https://...", "default_model": "gpt-4", "api_type": "API1", "has_key": true }
  ]
}
```

| Field | Description |
|-------|-------------|
| `dirty` | `true` when the in-memory providers differ from the encrypted keys file on disk. Set by create/update/delete, cleared by save and load/unlock. Used by the editor to show an unsaved-changes badge. |

### POST /api/settings/providers
**Request body:**
```json
{ "name": "anthropic", "display_name": "Anthropic", "endpoint": "https://...", "api_key": "sk-...", "default_model": "claude-3", "api_type": "API1" }
```
`name` is required. Returns 409 if the provider already exists.

### POST /api/settings/providers/save
**Request body (optional):**
```json
{ "master_password": "my-secret" }
```
Falls back to `JARVIS_MASTER_PASSWORD` environment variable if not provided in body. Verifies password against existing keys file before overwriting. Returns 403 on wrong password.

---

## Task Heartbeat — Both editions

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

## Log Viewer — `GET /api/log` (Both), `GET /api/log/security` (Both), `analyze-last-run` (Studio only)

| Method | Path | Edition | Description |
|--------|------|---------|-------------|
| GET | `/api/log` | Both | Fetch application log lines (tail or delta mode). |
| GET | `/api/log/security` | Both | Fetch security log lines (tail or delta mode). |
| GET | `/api/log/analyze-last-run` | Studio | Log-based analysis of a workflow run (requires AI). |

### GET /api/log

**Query parameters:**

| Param | Required | Description |
|-------|----------|-------------|
| `tail` | No | Return the last N lines of `log/log.txt` (default 5000, max 200000). |
| `offset` | No | Return only lines appended since byte offset N (delta mode). |

Provide either `tail` or `offset`, not both. If `offset` is given, only new lines since that byte position are returned.

> **Note:** The dashboard Log Viewer uses `tail` mode for the initial backfill, then receives live updates via the WebSocket `log` message (see below). The `offset` delta-polling mode is retained for backward compatibility and external tools.

**Response (200):**
```json
{
  "ok": true,
  "lines": ["line1", "line2"],
  "byteOffset": 123456,
  "totalSize": 123456
}
```

### GET /api/log/security

Reads the security audit log (`log/security.txt`). Supports the same `tail` and `offset` query parameters as `GET /api/log`. The security log records authentication successes/failures, rate limit events, webhook accept/reject decisions, lockout triggers, shutdown requests, and run control actions — all with IP addresses and timestamps.

The security log is a rotating file (10 MB x 5 files), so older entries are automatically pruned. The dashboard Log Viewer exposes this via a "Security" tab with 3-second delta polling.

**Query parameters:** Same as `GET /api/log` (see above).

**Response (200):** Same format as `GET /api/log`.

**Response (404):** If `log/security.txt` does not exist yet (no security events have been logged).

### GET /api/log/analyze-last-run

Scans `log/log.txt` for `[workflow] run '...' started/completed/failed/cancelled/stopped` markers and collects warning/error/critical lines between them.

**Query parameters:**

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `index` | No | `0` | Run index. `0` = most recent run, `1` = second-to-last, etc. Wraps around at the first run in the log. |

**Response (200) — run found:**
```json
{
  "ok": true,
  "found": true,
  "runIndex": 0,
  "totalRuns": 9,
  "runId": "exampleMakefile4_1772302776",
  "workflowId": "exampleMakefile4",
  "state": "completed",
  "startedAt": "2026-02-28 10:19:36.328",
  "completedAt": "2026-02-28 10:19:39.215",
  "startLine": 950,
  "endLine": 9610,
  "issueCount": 0,
  "issues": []
}
```

**Response (200) — no runs in log:**
```json
{ "ok": true, "found": false, "totalRuns": 0, "message": "No workflow run start found in log." }
```

| Field | Description |
|-------|-------------|
| `runIndex` | 0-based index of the returned run (0 = newest). |
| `totalRuns` | Total number of run-start markers found in the log. Used by the frontend for cycling. |
| `startLine` | 1-indexed line number of the `[workflow] run started` marker. |
| `endLine` | 1-indexed line number of the completion marker, or `-1` if the run is still active. |
| `issues` | Array of `{ line, severity, text }` objects for warning/error/critical lines between start and end. |
| `issueCount` | Length of the `issues` array. |

---

## Shutdown — Both editions

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/shutdown` | Initiate a graceful server shutdown. |

**Response (200):**
```json
{ "message": "Shutdown initiated." }
```
Triggers the same shutdown sequence as pressing `q` or Ctrl+C: global shutdown signal → two-phase parallel subsystem shutdown → watchdog safety net (6s).

---

## Security considerations — Engine edition

All admin endpoints in Engine mode require **Bearer token authentication** (`Authorization: Bearer <token>`). The token is auto-generated on first run and stored in `engine_api_token.txt` (permissions `0600`).

**Security features:**
- **Rate limiting:** 100 req/min per IP, burst of 20 (token bucket)
- **Failed auth lockout:** 10 failures in 5 minutes = 15-minute IP lockout (403 + `Retry-After: 900`)
- **Token expiration:** Tokens older than 90 days are auto-rotated; 7-day expiry warning at startup
- **Audit logging:** All auth events logged to `log/security.txt` (viewable via `GET /api/log/security`)
- **Built-in TLS:** Optional `TlsCert`/`TlsKey` in config.json serves HTTPS on port 8443
- **WebSocket auth:** First message must be `{"type":"auth","token":"..."}` (constant-time comparison)
- **Webhook HMAC:** Per-workflow HMAC-SHA256 signature verification (mandatory in Engine mode)
- **RBAC (Role-Based Access Control):** Three roles — `admin`, `operator`, `viewer`. In gateway mode (`TrustedProxyHeader`/`TrustedRoleHeader` config), role comes from gateway-injected header. Bearer token grants `admin`. Routes enforce minimum role.
- **Request body limit:** `MaxRequestBodyMB` config field (default 10 MB). Oversized requests → 413.
- **Security headers:** CSP, X-Frame-Options (DENY), X-Content-Type-Options, Referrer-Policy, Permissions-Policy on all responses. HSTS when TLS enabled.

**Endpoint role requirements** (Engine mode with RBAC):

| Minimum role | Endpoints |
|-------------|-----------|
| `viewer` | `GET /api/workflows`, `GET /api/workflows/<id>`, `GET /api/workflow-runs/*` |
| `operator` | `POST /api/workflow-runs/<id>/cancel\|pause\|resume\|stop`, `GET /api/log` |
| `admin` | `POST /api/shutdown`, `GET /api/log/security` |

**Data-sensitive endpoints** (protected by auth + RBAC but contain sensitive information when exposed):

| Endpoint | Risk | Min role |
|----------|------|----------|
| `GET /api/log` | Can leak prompt content, output data, file paths, and provider details | operator |
| `GET /api/log/security` | Exposes IP addresses, user identities, and auth event history | admin |
| `POST /api/shutdown` | Shuts down the server process | admin |
| `POST /api/workflow-runs/<id>/cancel\|pause\|resume\|stop` | Run-control: can disrupt running workflows | operator |

Deploy behind an API gateway on a private subnet. See `doc/cyber security.md` for the recommended architecture.

---

## WebSocket — `/ws` — Both editions (core messages); Studio only (AI messages)

A persistent WebSocket connection for real-time communication.

### Client → Server Messages

| Type | Fields | Description |
|------|--------|-------------|
| `chat` | `subsystem`, `message` | Submit a chat message (same as POST /api/chat but via WS). Creates a `PROB_<id>_<ts>.txt` file. |
| `workflow-runs-request` | — | Request the current workflow runs snapshot. Sent once on connect; server pushes updates automatically thereafter. |

### Server → Client Messages

| Type | Description |
|------|-------------|
| `queued` | Acknowledgement of a chat message with `id` and `file` path. |
| `workflow-runs-snapshot` | Full snapshot of active runs with per-task states (including `capturedStdout`/`capturedStderr`). **Server-pushed** on every state change (task start/complete/fail, run start/complete). Also sent on initial `workflow-runs-request`. Replaces the previous 500ms client polling. |
| `python-status` | Broadcast when Python engine status changes (`{ "running": true/false }`). |
| `log` | Live log lines streamed from the server. `{ "type": "log", "lines": ["...", ...] }`. Replaces 500ms REST polling for the Log Viewer page. |
| *(broadcast)* | Any JSON string queued via `Broadcast()` / `BroadcastJSON()` is drained to all clients on next `onmessage`. |
