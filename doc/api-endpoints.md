# JarvisAgent API Endpoints

Base URL: `http://localhost:8080`

All JSON endpoints return `application/json`. Errors follow the shape:
```json
{ "ok": false, "error": "<code>", "message": "<human-readable>" }
```

### Edition availability

JarvisAgent ships in two editions: **Engine** (lean production server) and **Studio** (full developer IDE). Each endpoint section is annotated with its edition availability. Studio-only endpoints return 404 in Engine mode.

### Authentication

j9t accepts exactly three auth mechanisms; there is no legacy bearer-token fallback:

1. **`Authorization: Bearer mcp_...`** — MCP API key validated against the encrypted `mcp_keys.json.enc` store. Carries per-user identity, role, and adhoc-submission flag. Used by MCP sidecars, automation, and the browser login flow.
2. **Session cookie** (`session=...`, HttpOnly + SameSite=Strict + `Secure` when TLS is on) — set by `POST /api/auth/login` after the user submitted a valid MCP key. 256-bit random, server-side only, 8-hour sliding timeout. Used by the dashboard browser UI.
3. **Gateway-injected identity headers** (`X-Forwarded-User` + `X-Forwarded-Role`) — trusted when `TrustedProxyHeader` / `TrustedRoleHeader` are configured. For deployments behind an OIDC/SAML API gateway.

| Tier | Endpoints | Engine | Studio |
|------|-----------|--------|--------|
| Public | `GET /api/status`, `GET /`, `/dash-assets/*` | No auth | No auth |
| Auth bootstrap | `POST /api/auth/mcp-keys/activate`, `POST /api/auth/login`, `POST /api/settings/keys/unlock` | No auth (rate-limited per-IP, master password IS the credential on `/keys/unlock`) | Same |
| MCP heartbeat | `POST /api/mcp/heartbeat` | MCP key required | MCP key required |
| Webhook | `POST /api/webhook/<id>` | HMAC-SHA256 (required) | HMAC-SHA256 (optional) |
| Programmatic / admin | All other endpoints | MCP key / session / gateway | Same for `mcp_` tokens; browser UI open on localhost |
| WebSocket | `WS /ws` | Session cookie validated at `.onaccept` handshake; role pinned per-connection and re-checked for admin-only message types (`ai-write-scripts`) | No auth (localhost browser); role still pinned + re-checked |

**First-run bootstrap:** on Engine's first start with an empty key store, j9t prints an admin enrollment token to stderr (60-minute TTL). Activate it with `POST /api/auth/mcp-keys/activate` to receive your MCP admin key. Subsequent keys are created via `POST /api/auth/mcp-keys/enroll` from an admin session or MCP admin key.

**Role hierarchy:** `admin > operator > viewer`. Routes enforce minimum required role — a viewer attempting admin-only endpoints receives HTTP 403 `insufficient_role`.

**Error responses:** unauthenticated requests return `401 Unauthorized` (`WWW-Authenticate: Bearer`); invalid or expired keys return `403 Forbidden` with `"error":"forbidden"` / `"token_expired"` / `"key_disabled"`; rate-limited requests return `429 Too Many Requests` with `Retry-After`.

---

## Static / UI — Both editions (dashboard); Studio only (editor)

| Method | Path | Edition | Description |
|--------|------|---------|-------------|
| GET | `/` | Both | Serves the dashboard (React SPA from `dashboard/ui/dist`). |
| GET | `/dash-assets/<path>` | Both | Serves Vite-built static assets for the dashboard. Path is canonicalised under `dashboard/ui/dist`; `..`-traversal returns `400 Bad Request`. |
| GET | `/editor` | Studio | Serves the Workflow Editor (React SPA from `workflow-editor/ui/dist`). |
| GET | `/assets/<path>` | Studio | Serves Vite-built static assets for the editor. Path is canonicalised under `workflow-editor/ui/dist/assets`; `..`-traversal returns `400 Bad Request`. |
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
  "ai_calls_inflight": 2,
  "websocket_clients": 1,
  "mcp_connected": false,
  "mcp_last_heartbeat_secs_ago": 12
}
```

| Field | Description |
|-------|-------------|
| `edition` | `"engine"` or `"studio"`. Used by the frontend to gate UI elements. |
| `capabilities` | Boolean map of feature availability. Engine: all `false`. Studio: all `true`. |
| `workflows_registered` | Number of JCWF workflows loaded in the registry. |
| `workflow_runs_active` | Number of currently running or queued workflow runs. |
| `ai_calls_inflight` | Number of `ai_call` envelopes currently in flight via `AiRequestPool::Submit`. |
| `websocket_clients` | Number of connected WebSocket clients. |
| `mcp_connected` | `true` if the MCP sidecar has sent a heartbeat within the last 35 seconds. |
| `mcp_last_heartbeat_secs_ago` | Seconds since last MCP heartbeat (only present when `mcp_connected` is `true`). |

### MCP Heartbeat

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/mcp/heartbeat` | Records an MCP sidecar heartbeat. Called every 15 seconds by the MCP server. Requires a valid MCP key (`Authorization: Bearer mcp_…`). Body capped at 1 KB; pre-auth rate limiter applied per-IP. |

**Response (200):**
```json
{ "ok": true }
```

**Errors:**
- `403 forbidden` — missing or invalid MCP credential.
- `413 payload_too_large` — request body exceeds 1 KB.
- `429 rate_limited` — pre-auth rate limit exceeded; respect `Retry-After`.

### Debug Signals — Debug builds only

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/debug/signals` | Live engine introspection — AI dispatcher, controllers, workflow runs, websocket, python pool, uptime, plus cloud-surface security counters (`cloud_dns_resolved_ip_rejections`, `cloud_endpoint_ssrf_rejections`, `cloud_credential_crlf_rejections`, `cloud_input_validation_rejections`, `cloud_postgres_invalid_sslmode_rejections`, `cloud_postgres_forbidden_param_rejections`).  Returns 404 on Release builds. |
| POST | `/api/debug/parse-rate-limit-headers` | Hermetic-test entry point. Body: `{interface_type, model, header_buffer, body, http_status}`. Calls `IRateLimitStrategy::Parse(...)` and returns the parsed `RateLimitObservation` plus `quota_key` + `initial_concurrency_probe`. Lets `test/dispatch/test_rate_limit_observation_parse.py` exercise every provider strategy without a live HTTP round-trip. Returns 404 on Release builds. |

**Selected fields (rate-limit refactor):**

| Field | Description |
|-------|-------------|
| `dispatcher_total_dispatched` | Lifetime count of HTTP requests handed to `curl_multi_add_handle`. |
| `dispatcher_total_completed` | Lifetime count of requests that returned a non-error result. |
| `dispatcher_total_throttled` | Cumulative count of throttle decisions (push-backs to inbox while waiting for cap availability). Counts cycles, not unique requests. |
| `dispatcher_total_429s` | Lifetime count of 429 responses received. |
| `dispatcher_total_retries_exhausted` | Count of requests that failed after `max_retries_429` retries. |
| `dispatcher_total_cancelled` | Count of requests aborted via cascade cancellation when their workflow run terminated. |
| `dispatcher_active_count` | Requests currently on the wire (curl in-flight). |
| `dispatcher_inbox_size` | Requests waiting in the dispatcher inbox for controller admission. |
| `dispatcher_retry_queue_size` | Requests waiting on a retry backoff. |
| `dispatcher_hosts[]` | Per-host roll-up: `host`, `remaining_requests`, `remaining_tokens`, `req_reset_in_sec`, `tok_reset_in_sec`, `active_count`. |
| `dispatcher_controllers[]` | Per-(host, modelFamily) adaptive controller state — see below. |

**`dispatcher_controllers[]` entry** — primary diagnostic for the rate-limit / AIMD layer:

```json
{
  "quota_key": "api.anthropic.com|claude-sonnet",
  "current_concurrency_cap": 16,
  "streak_since_last_429": 4,
  "remaining_requests": 999,
  "remaining_tokens": 90000,
  "req_reset_in_sec": -1,
  "tok_reset_in_sec": -6,
  "last_consumed_input_tokens": -1,
  "last_consumed_output_tokens": -1
}
```

| Field | Description |
|-------|-------------|
| `quota_key` | `<host>\|<modelFamily>` — opaque identifier, distinct per (account quota bucket). |
| `current_concurrency_cap` | AIMD cap on simultaneous in-flight requests for this key. Halves on 429, additively grows on streak of 5 clean completions. |
| `streak_since_last_429` | Clean completions accumulated toward the next AIMD cap increase. |
| `remaining_requests` / `remaining_tokens` | Last-observed RPM/TPM remaining (from provider headers). `-1` = unknown / provider ships no proactive feedback (e.g. Gemini Native, Bedrock). |
| `req_reset_in_sec` / `tok_reset_in_sec` | Seconds until the bucket refills. Negative = already past, refilled. |
| `last_consumed_input_tokens` / `last_consumed_output_tokens` | Last response's `usage` totals. `-1` until first observation lands. |

This is the canonical place to verify per-interface `rate_limit` config tuning is doing what you expect. See the user manual `doc/jarvisagent.md` "Rate-limit configuration" for the corresponding config schema.

**WebSocket broadcast counters** — added 2026-04-27 while investigating the dashboard live-update bug; kept in as a permanent post-mortem layer. All counters reset on process start.

| Field | Description |
|-------|-------------|
| `websocket_total_broadcasts_enqueued` | Lifetime count of every push into `m_PendingBroadcasts`. |
| `websocket_total_runs_snapshots_enqueued` | Subset: `BroadcastWorkflowRunsSnapshot()` calls. Should track 1:1 with task state transitions during a heavy run. |
| `websocket_total_last_runs_snapshots_enqueued` | Subset: `BroadcastWorkflowRunsLastSnapshot()` calls. |
| `websocket_total_ai_call_events_enqueued` | Subset: `BroadcastAiCallStarted/Completed/Failed` events. |
| `websocket_total_python_status_enqueued` | Subset: `BroadcastPythonStatus()` events. |
| `websocket_total_log_batches_enqueued` | Subset: log-line batches enqueued at drain time. |
| `websocket_total_drains` | Lifetime count of `DrainPendingBroadcasts()` invocations (one per client `ping` frame). |
| `websocket_last_drain_bytes` / `websocket_peak_drain_bytes` | Size of the most recent / largest single batched `send_text` frame. |
| `websocket_last_drain_messages` | Number of messages folded into the most recent batch. |
| `websocket_last_drain_duration_us` / `websocket_peak_drain_duration_us` | Wall time spent inside the most recent / longest drain (build batch + send to all clients). |

Healthy values during an active workflow: drains keep up with pings (one drain per ping interval), peak batch size in the hundreds of KB, peak duration in the tens of ms. A snapshot counter that flatlines while completions are still arriving means a producer-side bug — see TODO List §17 for the historical example.

---

## Workflows — CRUD — read-only (Both, viewer+); mutating CRUD (Studio only, admin); reload + tree + dependency-graph + versions (Both, admin/viewer)

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
| GET | `/api/scripts` | List the script catalog (metadata parsed from `@jarvis-script` headers). |
| GET | `/api/scripts/check?path=<scriptPath>` | Check if a script exists and is executable. |

### GET /api/scripts

Returns the catalog of scripts under `scripts/` that adhoc JCWFs and MCP agents can reference. Metadata is parsed from each script's `@jarvis-script` comment block. The server scans on startup; use `?refresh=1` to re-scan after deploying new scripts without a full j9t restart.

Auth: any authenticated role (viewer+).

**Query parameters:**

| Param | Required | Description |
|-------|----------|-------------|
| `type` | No | Filter by script type: `shell` or `python`. Omit to list everything. |
| `refresh` | No | When present, triggers an on-demand rescan of `scripts/` before returning. |

**Response (200):**
```json
{
  "ok": true,
  "count": 35,
  "scripts": [
    {
      "path": "scripts/parseOpenSshLog.sh",
      "type": "shell",
      "short": "Extract structured OpenSSH attack stats from logs",
      "description": "Parses OpenSSH auth logs and outputs per-IP attack stats…",
      "params": ["input_log", "output_json"],
      "outputs": "Structured JSON stats to output_json",
      "has_shebang": true,
      "has_jarvis_marker": true,
      "executable": true
    },
    {
      "path": "scripts/extractChapters.py",
      "module": "scripts.extractChapters",
      "type": "python",
      "short": "Extract chapter titles from a Markdown file",
      "params": ["input_file"],
      "has_jarvis_marker": true,
      "executable": false
    }
  ]
}
```

`has_jarvis_marker: false` flags scripts that lack the `@jarvis-script` header — typically helper modules that aren't JCWF entry points. `module` is only populated for python scripts and matches the name a JCWF references via `params.module`.

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

## Workflows — Run Control & Monitoring — Both editions (operator+ for run/cancel/pause/resume/stop/clean; viewer+ for monitoring)

| Method | Path | Edition | Description |
|--------|------|---------|-------------|
| POST | `/api/workflows/<id>/run` | Studio | Start a workflow run. Requires `manual_start: true`. |
| POST | `/api/workflows/run-adhoc` | Both | Submit a JCWF for one-shot execution (MCP key with `adhoc_enabled`, role ≥ operator). |
| DELETE | `/api/workflows/<id>/clean` | Studio | Clean a workflow's queue output directory. |
| GET | `/api/workflow-runs/active` | Both | List all currently active (running/queued) runs. |
| GET | `/api/workflow-runs/last` | Both | Get the last completed run for each workflow. |
| GET | `/api/workflow-runs/<runId>` | Both | Get detailed status of a specific run (including per-task state). |
| POST | `/api/workflow-runs/<runId>/cancel` | Both | Request cancellation of an active run. |
| POST | `/api/workflow-runs/<runId>/pause` | Both | Pause an active run (suspend new task dispatch). |
| POST | `/api/workflow-runs/<runId>/resume` | Both | Resume a paused run. |
| POST | `/api/workflow-runs/<runId>/stop` | Both | Graceful stop: finish in-flight tasks, no new dispatch. |
| GET | `/api/workflow-runs/<runId>/files` | Both | List files a run produced (operator reads own; admin any). |
| GET | `/api/workflow-runs/<runId>/files/<path>` | Both | Download a single artifact file (Range supported, 10 MB cap). |

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

### POST /api/workflows/run-adhoc

Submit a JCWF canvas for one-shot execution without permanent registration. Requires an MCP API key with `adhoc_enabled=true` and role ≥ `operator`. The payload **cannot** include shell/python scripts — any script referenced by the JCWF must already exist under `scripts/`.

**Request body:**
```json
{
  "jcwf": { "id": "my_one_shot", "version": "1.0", "tasks": { ... }, ... },
  "context": { "customer": "acme", "region": "us-east-1" },
  "cleanup_policy": "ttl_72h"
}
```

- `jcwf` (required) — complete canvas JSON (same shape as `POST /api/workflows`). The server rewrites the top-level `id` to a unique `_adhoc_<timestamp>_<counter>` so runs don't collide with registered workflows.
- `context` (optional) — key-value pairs seeded into the run's `ContextMap`.
- `cleanup_policy` (optional) — one of `on_completion`, `ttl_1h`, `ttl_24h`, `ttl_48h`, `ttl_72h`, `retain`. Defaults to the key's configured `default_cleanup_policy`.

**Response (202):**
```json
{
  "ok": true,
  "runId": "adhoc_20260417T183022_0001",
  "workflowId": "_adhoc_20260417T183022_0001",
  "cleanup_policy": "ttl_72h",
  "folder_path": ".../_adhoc/20260417T183022_0001_del-20260420T183022"
}
```

**Error responses:**
- `400 malformed_body` / `empty_jcwf` / `invalid_cleanup_policy` — bad submission payload.
- `400 missing_scripts` — a shell `params.command` or python `params.module` referenced by the JCWF does not exist under `scripts/`. The response `"missing"` array lists the offending references (for python it includes the expected `scripts/<mod>.py` path). Scripts cannot be submitted inline — they must be deployed by an admin.
- `403 adhoc_not_enabled` — the MCP key has `adhoc_enabled=false`; ask your admin.
- `403 insufficient_role` — your role is `viewer`.
- `403 mcp_key_required` — session cookie or gateway header is not sufficient; adhoc needs an MCP key.
- `403 policy_exceeds_ceiling` — the requested `cleanup_policy` is longer than the key's `default_cleanup_policy` (admin-configured maximum). Response includes `"ceiling"` with the maximum-allowed policy.
- `413 jcwf_too_large` — submitted JCWF JSON exceeds the 4 MB hard cap. Cap is enforced before any folder is created or registry mutation happens, so a rejected oversized submission has zero filesystem footprint.
- `413 quota_exceeded` — cumulative adhoc disk usage for your user would exceed `disk_quota_mb`.
- `503 adhoc_unavailable` — WorkflowRegistry not yet attached (server startup race — retry).

The run dispatches through the standard `WorkflowRuntimeManager`. Monitor via `GET /api/workflow-runs/<runId>`. AI calls are capped per run by `max_ai_calls_per_jcwf` in `config.json` (0 = unlimited).

**Folder layout:** adhoc runs are staged under `_adhoc/<user_slug>/<timestamp>_<counter>_del-<delete-at>/`. `user_slug` is derived from the MCP key's `user` field (`[A-Za-z0-9._@-]`, other characters collapsed to `_`, capped at 64 bytes) so authorisation for artifact retrieval can be enforced by filesystem path prefix as well as in the handler.

**`ai_call` tasks.** Set each `ai_call` task's `working_directory` to a queue-relative path — e.g. `"../../queue/<task_id>"`. This is resolved relative to the run's workflow base directory (`_adhoc/<user>/<run>/workflows/_adhoc_.../`), so queue-binding files land in `_adhoc/<user>/<run>/queue/<task_id>/`. The runtime registers the adhoc run's queue folder with the file watcher at stage time, so these files trigger the normal AI dispatch pipeline. See `doc/JC_Workflow_Specification.md` §3.3.6 for the queue-binding format and §3.3.6.3 for `file_outputs` semantics on `ai_call` (Pattern A: `outputs` slot for downstream JCWF wiring; Pattern B: `file_outputs` with a destination path for terminal delivery).

### GET /api/workflow-runs/\<runId\>/files

List every file a workflow run produced. Returns both retrieval modes (local filesystem path for same-host agents; download URL for remote agents) plus retention so the caller knows how long the artefacts will live.

Authorization: operator can read own runs; admin can read any run (cross-user reads are audit-logged). Viewer → 403.

**Query parameters:**
- `prefix` (optional) — narrow the listing to entries whose path starts with this value (after lexical normalisation; `..` rejected).

**Response (200):**
```json
{
  "ok": true,
  "runId": "adhoc_20260418T174752_0006",
  "owner": "alice@company.com",
  "owner_slug": "alice@company.com",
  "terminal": true,
  "retention": {
    "policy": "ttl_1h",
    "delete_at": "2026-04-18T18:47:52Z",
    "seconds_remaining": 2931
  },
  "files": [
    {
      "path": "workflows/_adhoc_.../attack_stats.json",
      "size_bytes": 33307,
      "modified_at": "2026-04-18T17:47:54Z",
      "task_id": "parse",
      "content_type": "application/json",
      "local_path": "/home/.../_adhoc/alice@.../workflows/.../attack_stats.json",
      "download_url": "/api/workflow-runs/adhoc_.../files/workflows/.../attack_stats.json"
    }
  ]
}
```

**Error responses:**
- `400 invalid_prefix` — prefix contained `..`.
- `403 insufficient_role` — caller is `viewer`.
- `403 not_owner` — run belongs to another user and the caller is not admin.
- `404 run_not_found` — no such run.

### GET /api/workflow-runs/\<runId\>/files/\<path\>

Stream a single file's bytes. `path` is URL-encoded and resolved lexically relative to the run folder — any escape attempt (`..`, absolute paths, symlinks, directories) is rejected before we touch the filesystem. Range requests are supported; a full-file response is capped at 10 MB to protect the server.

**Response headers (200 / 206):**
- `Content-Type` — detected from extension; `application/octet-stream` fallback.
- `Content-Length` — exact byte count served.
- `X-Content-SHA256` — hex digest of the full file (full-file responses only; Range responses omit it — use the listing endpoint for the canonical hash).
- `X-Run-Id`, `X-Run-Owner` — echoed back for correlation.
- `X-Retention-Delete-At` — compact ISO from the folder name, so streaming clients know how long the URL stays valid.
- `Content-Range` — when the request carried a `Range` header.
- `Content-Disposition` — `inline` by default; pass `?download=1` to force `attachment`.
- `Accept-Ranges: bytes` — always.

**Error responses:**
- `400 path_escape` — resolved path escapes the run folder (even if the landing point is accidentally inside).
- `400 absolute_path_rejected` — path starts with `/`.
- `400 symlink_rejected` — target is a symlink. Never followed, regardless of where it points (closes a TOCTOU class).
- `400 is_directory` — use the listing endpoint instead.
- `400 not_regular_file` — target is a FIFO/socket/device.
- `400 invalid_path` / `missing_path` — malformed input.
- `403 reserved_file` — `meta.json` and `manifest.json` are internal bookkeeping; not served through this endpoint.
- `403 insufficient_role` / `403 not_owner` — same rules as the listing endpoint.
- `404 file_not_found` / `404 run_not_found`.
- `413 file_too_large` — file exceeds the 10 MB single-response cap. Response includes `X-Suggested-Range: bytes=0-10485759`.
- `413 range_too_large` — explicit Range request exceeds the cap.
- `416` — Range header is syntactically valid but outside the file's byte count. Response carries `Content-Range: bytes */<size>`.

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

`callbackUrl` MUST resolve to a public IP — loopback / RFC 1918 / link-local / cloud-metadata addresses are refused at completion time (SSRF gate; see `doc/JC_Workflow_Specification.md` §3.2.6 and `doc/cyber security.md`).
To omit task output content from the callback payload (e.g. for runs handling PII or secrets), set `"callback_include_outputs": "false"` in the `context` object.

**Response (202):**
```json
{ "ok": true, "workflowId": "myWorkflow", "runId": "...", "requestPath": "/abs/path/request.json" }
```

---

## Settings — AI Interfaces — Both editions (admin only)

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
    { "name": "...", "description": "...", "url": "...", "model": "...", "api_type": "API1",
      "key_name": "...", "is_mock": false, "fixture_path": "" }
  ]
}
```

| Field | Description |
|-------|-------------|
| `dirty` | `true` when the in-memory interfaces differ from the on-disk `config.json`. Set by create/update/delete, cleared by save and reload. Used by the editor to show an unsaved-changes badge. |
| `is_mock` | `true` when the dispatcher routes calls to this interface through MockTransport (fixture replay) instead of LiveTransport (real HTTPS). Admin-only — same access surface as `api_key`. |
| `fixture_path` | Path to the on-disk fixture file (relative to project root) MockTransport reads as the canned response body. Required when `is_mock` is `true`. |

### POST /api/settings/ai-interfaces
**Request body:**
```json
{ "name": "optional", "url": "https://...", "model": "gpt-4", "api_type": "API1",
  "description": "...", "key_name": "...", "is_mock": false, "fixture_path": "" }
```
`url` is required. `name` is auto-generated if omitted. Returns 409 on duplicate name.

`api_type` accepts `API1`, `API2`, `API3`, `API4`, `API5`, or `API6`. Legacy `"Test"` is rejected with a 400 + `api_type_test_removed` error pointing at the `is_mock` + `fixture_path` migration. Examples:

```json
// AWS Bedrock: regional base URL in `url`; model is the full Bedrock modelId; key_name points at a provider with credential_type "aws".
{ "url": "https://bedrock-runtime.us-east-1.amazonaws.com",
  "model": "anthropic.claude-3-haiku-20240307-v1:0", "api_type": "API5", "key_name": "bedrock-prod" }

// Azure OpenAI: full deployment URL in `url`; key_name points at a provider with credential_type "api_key".
{ "url": "https://my-resource.openai.azure.com/openai/deployments/gpt-4/chat/completions?api-version=2024-08-01",
  "model": "gpt-4", "api_type": "API6", "key_name": "azure-prod" }

// MockTransport — hermetic fixture replay. The full OpenAI-shape JSON at `fixture_path`
// is fed through ReplyParserAPI1 (or whichever api_type is configured), so the test
// surface is the parser, AIMD, retry queue — the dispatcher's full code path.
{ "url": "https://localhost/_mock_/never_called", "model": "mock-stub", "api_type": "API1",
  "is_mock": true, "fixture_path": "test/dispatch/fixtures/api1/golden_success.json" }
```

**`is_mock` hardening (rejected with 400 at this endpoint):**
- `is_mock: true` without a non-empty `fixture_path` → `is_mock_requires_fixture_path`.
- `fixture_path` outside the project root (absolute escape, `..` traversal, symlink target outside) → `fixture_path_rejected` (`ConfineUnderProjectRoot`-gated).
- Legacy `api_type: "Test"` → `api_type_test_removed` (use `api_type: "API1..6"` + `is_mock: true` + `fixture_path` instead).

Additional MockTransport hardening enforced at request dispatch (not at this endpoint): 10 MiB per-fixture size cap; optional `<fixture>.meta.json` sibling controls HTTP status (must be `[200, 599]`) and headers (allowlist `{Content-Type, Retry-After}` only — others dropped with WARN); PROV sidecar carries `"mocked": true` + the resolved `fixture_path` so post-mortem tooling distinguishes mock dispatches from live ones.  See `doc/jarvisagent.md` "API interfaces" and `doc/cyber security.md` "MockTransport Security" for the complete posture.

### POST /api/settings/ai-interfaces/save
Writes the in-memory interfaces back to the `config.json` file by replacing the `"API interfaces"` array.  String fields (`name`, `description`, `url`, `model`, `key_name`) are JSON-escaped, the patched document is re-parsed with simdjson before the on-disk write, and the rename is atomic — on any 5xx error the existing `config.json` is left unchanged.

**Response (200):**
```json
{ "ok": true, "path": "/abs/path/config.json" }
```

**Response (500) — validation failed (the patched text did not re-parse cleanly):**
```json
{ "ok": false, "error": "validation_failed", "message": "Generated config.json did not re-parse cleanly; aborting write." }
```

**Response (500) — atomic write failed (parent directory unwritable, disk full, etc.):**
```json
{ "ok": false, "error": "write_failed", "message": "Failed to write '/abs/path/config.json': <underlying-error>" }
```

### POST /api/settings/ai-interfaces/test
Sends a minimal prompt ("Say hello") directly to the specified AI interface via curl with a **10-second timeout** — a lightweight connectivity and authentication check.

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

## Settings — Config — Both editions (admin only)

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
Updates the specified fields in memory and persists them to `config.json`.  Persistence patches the named top-level keys in the existing file (preserving comments, field ordering, and layout); the patched text is validated with simdjson and the rename is atomic — on any 5xx error the existing `config.json` is left unchanged.

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

**Response (500) — patched config did not re-parse cleanly:**
```json
{ "ok": false, "error": "validation_failed", "message": "Generated config.json did not re-parse cleanly; aborting write." }
```

**Response (500) — atomic write failed:**
```json
{ "ok": false, "error": "write_failed", "message": "Failed to write '/abs/path/config.json': <underlying-error>" }
```

---

## Settings — Key Management — Both editions

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/settings/keys/status` | Check whether the encrypted key stores are loaded. |
| POST | `/api/settings/keys/unlock` | Provide the master password; unlocks both `keys.json.enc` and `mcp_keys.json.enc`. |

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
{ "ok": true, "status": "ok", "message": "Keys unlocked successfully.", "mcp_keys_loaded": true }
```
The same master password decrypts `keys.json.enc` (AI provider credentials) and `mcp_keys.json.enc` (MCP API keys). Returns 401 on wrong password.

---

## Auth — MCP keys + sessions — Both editions

MCP API keys and dashboard sessions share the auth plane. See [Authentication](#authentication) for the three-path model.

| Method | Path | Min role | Description |
|--------|------|----------|-------------|
| GET | `/api/auth/whoami` | any auth | Identity + role for the current request |
| POST | `/api/auth/login` | public (validates MCP key or gateway header) | Exchange an MCP key for a session cookie |
| POST | `/api/auth/logout` | session | Destroy the session + clear cookie |
| GET | `/api/auth/mcp-keys` | admin | List all MCP keys (hashes only, no raw keys) |
| POST | `/api/auth/mcp-keys/enroll` | admin | Create a single-use enrollment token |
| POST | `/api/auth/mcp-keys/activate` | public (enrollment token *is* the auth) | Exchange an enrollment token for a real MCP key |
| POST | `/api/auth/mcp-keys/self-renew` | current MCP key | Generate a fresh key inheriting all metadata; old key enters 24h grace |
| PUT | `/api/auth/mcp-keys/<key_id>` | admin | Update role / adhoc flag / quota / enabled / description / expires_at |
| DELETE | `/api/auth/mcp-keys/<key_id>` | admin | Revoke a key immediately |

### POST /api/auth/login
**Request body:** `{ "api_key": "mcp_..." }`
**Response (200):** `{ "ok": true, "user": "alice@example.com", "role": "operator" }`
Also sets a `session` cookie (`HttpOnly; SameSite=Strict; Path=/; Max-Age=<session_timeout_hours*3600>`; `Secure` when TLS is enabled). Gateway deployments bypass the body and rely on `X-Forwarded-User` / `X-Forwarded-Role` headers.

### POST /api/auth/logout
No body required. Destroys the server-side session and returns a cookie with `Max-Age=0`.

### GET /api/auth/whoami
**Response (200):** `{ "ok": true, "user": "...", "role": "admin|operator|viewer" }`. Returns 401 when no valid auth is present.

### POST /api/auth/mcp-keys/enroll
**Request body:**
```json
{
  "user": "alice@example.com",
  "role": "operator",
  "adhoc_enabled": true,
  "disk_quota_mb": 1024,
  "default_cleanup_policy": "ttl_72h",
  "description": "Alice's Claude Code key",
  "key_expiry_days": 90,
  "enrollment_ttl_minutes": 30
}
```
**Response (201):** `{ "ok": true, "enrollment_token": "enroll_...", "expires_in_minutes": 30, "user": "...", "role": "..." }`. The admin shares the enrollment token with the user through a secure channel; the admin never sees the final API key.

### POST /api/auth/mcp-keys/activate
**Request body:** `{ "enrollment_token": "enroll_..." }`
**Response (200):** `{ "ok": true, "key_id": "mcp_...", "api_key": "mcp_...", "user": "...", "role": "...", "expires_at": "..." }`. The `api_key` is shown **exactly once** — persist it client-side immediately.

### POST /api/auth/mcp-keys/self-renew
Caller must authenticate with the still-valid MCP key. **Response (200):** `{ "ok": true, "key_id": "mcp_...", "api_key": "mcp_...", "expires_at": "...", "message": "..." }`. Old key is moved to a 24-hour grace period then disabled.

### GET /api/auth/mcp-keys
**Response (200):** `{ "ok": true, "keys": [{ "key_id": "...", "user": "...", "role": "...", "adhoc_enabled": false, "disk_quota_mb": 1024, "default_cleanup_policy": "ttl_72h", "created_at": "...", "expires_at": "...", "last_used_at": "...", "enabled": true, "description": "..." }, ...] }`. Raw keys are never exposed.

### PUT /api/auth/mcp-keys/<key_id>
**Request body:** partial — include any subset of `role`, `adhoc_enabled`, `disk_quota_mb`, `default_cleanup_policy`, `enabled`, `description`, `expires_at`.
**Response (200):** `{ "ok": true }`.

### DELETE /api/auth/mcp-keys/<key_id>
**Response (200):** `{ "ok": true }`. All subsequent requests using the revoked key return 403.

---

## Settings — Providers — Both editions (admin only)

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
If the body omits `master_password`, the server falls back to the password cached in memory from an earlier `POST /api/settings/keys/unlock` call. Verifies the password against the existing encrypted file before overwriting. Returns 403 on wrong password, 400 if no password is cached and the body is empty.

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

## Log Viewer — Both editions (`GET /api/log` operator+; `GET /api/log/security` admin only; `analyze-last-run` operator+)

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

All non-public endpoints require authentication via one of the three supported mechanisms (MCP key, session cookie, or gateway header). See the [Authentication](#authentication) section for details.

**Security features:**
- **MCP API keys:** Per-user credentials (`mcp_` + 64 hex chars) stored only as SHA-256 hashes in the encrypted `mcp_keys.json.enc`. Created via the enrollment flow (`POST /api/auth/mcp-keys/enroll` → `POST /api/auth/mcp-keys/activate`). Each key carries an identity, role, adhoc flag, disk quota, and retention ceiling.
- **Master password in `mlock`'d memory:** `SecureString` buffer prevents swap-to-disk and zeroes on destruction. Unlocked via `POST /api/settings/keys/unlock` after each restart.
- **Dashboard sessions:** HttpOnly + SameSite=Strict + `Secure` (when TLS) cookie; 8-hour sliding timeout; invalidated on restart. `POST /api/auth/login` to create, `POST /api/auth/logout` to destroy.
- **WebSocket auth:** Session cookie validated at the `.onaccept` handshake (Engine). Studio allows browser upgrades without auth.
- **Rate limiting:** 100 req/min per IP, burst of 20 (token bucket)
- **Failed auth lockout:** 10 failures in 5 minutes = 15-minute IP lockout (403 + `Retry-After: 900`)
- **Key expiry and self-renewal:** 90-day default; users renew themselves via `POST /api/auth/mcp-keys/self-renew` within the window; admin re-enrollment required after expiry
- **Audit logging:** All auth events logged to `log/security.txt` with user identity and role (viewable via `GET /api/log/security`)
- **Built-in TLS:** Optional `TlsCert`/`TlsKey` in config.json serves HTTPS on port 8443
- **Webhook HMAC:** Per-workflow HMAC-SHA256 signature verification (mandatory in Engine mode)
- **RBAC:** Three roles — `admin`, `operator`, `viewer` — carried on MCP keys, session cookies derived from them, or gateway `X-Forwarded-Role` (default `viewer`). Routes enforce minimum role; violations return 403 `insufficient_role`.
- **Request body limit:** `MaxRequestBodyMB` config field (default 10 MB). Oversized requests → 413.
- **Security headers:** CSP, X-Frame-Options (DENY), X-Content-Type-Options, Referrer-Policy, Permissions-Policy on all responses. HSTS when TLS enabled.

**Endpoint role requirements** (Engine mode with RBAC):

| Minimum role | Endpoints |
|-------------|-----------|
| `viewer` | `GET /api/workflows`, `GET /api/workflows/<id>`, `GET /api/workflow-runs/*`, `GET /api/auth/whoami`, `GET /api/scripts` |
| `operator` | `POST /api/workflow-runs/<id>/cancel\|pause\|resume\|stop`, `GET /api/log`, `POST /api/workflows/run-adhoc` (plus `adhoc_enabled` on the key), `GET /api/workflow-runs/<id>/files[/<path>]` (own runs only), `POST /api/auth/mcp-keys/self-renew` |
| `admin` | `POST /api/shutdown`, `GET /api/log/security`, `GET \| POST \| PUT \| DELETE /api/auth/mcp-keys/*` (except activate/self-renew), AI interface / connections / provider CRUD |

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

---

## Cloud Connections — Both editions (admin only)

Manage named cloud connections for external service integrations. Connections reference a key from the Keys page and carry type-specific parameters.

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/connections` | List all connections with status. |
| POST | `/api/connections` | Create a new connection. |
| PUT | `/api/connections/<name>` | Update an existing connection (merge semantics). |
| DELETE | `/api/connections/<name>` | Delete a connection. |
| POST | `/api/connections/<name>/test` | Test connectivity via the registered connector. |
| POST | `/api/connections/save` | Persist connections to `connections.json`. |
| GET | `/api/connections/<name>/oauth/authorize` | Start OAuth PKCE flow — returns authorization URL. |
| GET | `/api/connections/<name>/oauth/callback` | OAuth redirect callback — exchanges code for tokens. |

### GET /api/connections
**Response (200):**
```json
{
  "ok": true,
  "dirty": false,
  "connections": [
    {
      "name": "my-polarion",
      "type": "polarion",
      "endpoint": "https://polarion.company.com",
      "key_name": "polarion-pat",
      "auth_type": "bearer",
      "params": { "project_id": "GoKartProcurement" }
    }
  ]
}
```

### POST /api/connections
**Request body:**
```json
{
  "name": "my-s3",
  "type": "s3",
  "endpoint": "https://s3.amazonaws.com",
  "key_name": "aws-creds",
  "auth_type": "sigv4",
  "params": { "region": "us-east-1", "bucket": "workflow-outputs" }
}
```
**Response (201):** `{ "ok": true, "name": "my-s3" }`
Returns 409 if a connection with that name already exists.

### PUT /api/connections/\<name\>
Overlays provided fields on the existing connection. Only specified fields are updated; `params` replaces the entire params map if provided.
**Response (200):** `{ "ok": true, "name": "my-s3" }`

### DELETE /api/connections/\<name\>
**Response (200):** `{ "ok": true }`

### POST /api/connections/\<name\>/test
Tests the connection using the `ICloudConnector::TestConnection()` method for the connection's type.
**Response (200):** `{ "ok": true }`
**Response (400):** `{ "ok": false, "error": "test_failed", "message": "Connection refused" }`
**Response (400):** `{ "ok": false, "error": "no_connector", "message": "No connector registered for type 'xyz'" }`

### POST /api/connections/save
Serializes all connections to `connections.json` in the launch directory.  The write is atomic (tmp-file + rename) — on a 5xx response the existing `connections.json` is left unchanged.

**Response (200):** `{ "ok": true, "path": "/abs/path/connections.json" }`

**Response (500):** `{ "ok": false, "error": "save_failed", "message": "Failed to write '/abs/path/connections.json': <underlying-error>" }`

### GET /api/connections/\<name\>/oauth/authorize
Initiates an OAuth 2.0 authorization code flow with PKCE for the named connection. The connection must have `auth_type: "oauth2"` and a `client_id` parameter.
**Response (200):** `{ "ok": true, "authorize_url": "https://login.microsoftonline.com/..." }`
**Response (400):** `{ "ok": false, "error": "invalid_auth_type", "message": "..." }`

The frontend opens `authorize_url` in a popup window. After user consent, the browser is redirected to the callback endpoint.

### GET /api/connections/\<name\>/oauth/callback
OAuth redirect callback. Microsoft / Google redirect here with `?code=...&state=...` after user consent. The backend validates the `state` parameter against the server-side single-use nonce generated at `/oauth/authorize` time, exchanges the authorization code + PKCE code_verifier for access/refresh tokens, stores them in `OAuthTokenManager`, and returns an HTML page that auto-closes.
**Response (200):** HTML page confirming authorization success.
**Response (400):** Error message if code is missing or the OAuth flow state is invalid.

**Authentication:** This endpoint is **intentionally unauthenticated** — the user-agent redirect from the OAuth provider cannot carry the j9t admin Bearer token.  The CSRF gate is the `state` query parameter (16-byte random nonce, server-side single-use, verified before any code-for-token exchange).  Per RFC 6749 §10.12, `state` IS the security mechanism for an OAuth callback.  The `/oauth/authorize` endpoint that creates the flow remains admin-gated.
