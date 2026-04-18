# integration/ — External Integration Guide

How to trigger JarvisAgent workflows from **n8n**, **curl**, **CI pipelines**, or any HTTP client.

---

## Quick Start (curl)

```bash
# Start a workflow run via webhook
curl -s -X POST http://localhost:8080/api/webhook/hamburg-tourist-day-planner \
  -H 'Content-Type: application/json' \
  -d '{
    "context": {
      "date": "2026-03-21",
      "timezone": "Europe/Berlin",
      "rainCategory": "some_rain",
      "weatherJson": "{\"temp_max\": 8, \"precipitation_sum\": 2.1}"
    }
  }'

# Check active runs
curl -s http://localhost:8080/api/workflow-runs/active | jq .

# Check a specific run
curl -s http://localhost:8080/api/workflow-runs/<runId> | jq .
```

---

## Endpoints

### POST /api/webhook/\<workflowId\> (recommended)

Start a workflow run via the **webhook trigger**. The workflow JCWF must have a `"type": "webhook"` trigger. Works for n8n, curl, CI, or any HTTP client.

**Request body (all fields optional):**

```json
{
  "runId": "my-run-001",
  "callbackUrl": "https://my-server.com/webhook/callback",
  "context": {
    "date": "2026-03-21",
    "timezone": "Europe/Berlin",
    "rainCategory": "some_rain",
    "weatherJson": "{...}"
  }
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `runId` | No | Custom run ID (auto-generated if omitted). |
| `callbackUrl` | No | URL to POST completion results to when the run finishes. |
| `context` | No | Key-value pairs injected into the workflow run context. Tasks can reference these via declared `inputs`. |

**Response (202 Accepted):**

```json
{
  "ok": true,
  "workflowId": "hamburg-tourist-day-planner",
  "runId": "my-run-001",
  "triggerId": "webhook"
}
```

#### HMAC Signature Verification

If the webhook trigger's JCWF has `"params": { "secret": "my-shared-secret" }`, every request **must** include:

```
X-Webhook-Signature: sha256=<hex-encoded HMAC-SHA256 of the raw request body>
```

Requests with a missing or invalid signature are rejected with HTTP 401. In **Engine mode**, a webhook secret is **mandatory** — webhooks without a configured secret are rejected with HTTP 403. In **Studio mode**, if no secret is configured the webhook is open (no signature check).

**Example with HMAC (bash):**

```bash
BODY='{"context":{"date":"2026-03-21"}}'
SIG=$(echo -n "$BODY" | openssl dgst -sha256 -hmac 'my-shared-secret' | awk '{print $2}')
curl -s -X POST http://localhost:8080/api/webhook/hamburg-tourist-day-planner \
  -H 'Content-Type: application/json' \
  -H "X-Webhook-Signature: sha256=$SIG" \
  -d "$BODY"
```

### POST /api/integrations/n8n/start (legacy)

Older endpoint that also starts workflow runs. Still works but **POST /api/webhook/\<id\>** is preferred for new integrations.

| Field | Required | Description |
|-------|----------|-------------|
| `workflowId` | **Yes** | ID of the JCWF workflow to run. |
| `runId` | No | Custom run ID. |
| `taskName` | No | Disk traceability folder name (default: `n8n`). |
| `callbackUrl` | No | Callback URL. |
| `context` | No | Context key-value pairs. |

### POST /api/workflows/\<id\>/run

Start a workflow run from the editor UI or a simple trigger. Supports optional context.

### GET /api/workflow-runs/active

Returns all currently running workflows with per-task state.

### GET /api/workflow-runs/last

Returns the last completed run for each workflow.

### GET /api/workflow-runs/\<runId\>

Returns detailed state for a specific run, including per-task status, stdout/stderr, and timing.

---

## Completion Callback

When a workflow run finishes (succeeded, failed, cancelled, or stopped), JarvisAgent checks the run context for a `callbackUrl` key. If present, it fires an **async POST** to that URL with a JSON payload describing the run result:

```json
{
  "workflowId": "hamburg-tourist-day-planner",
  "runId": "my-run-001",
  "state": "succeeded",
  "ok": true,
  "completedAt": "2026-03-21T20:15:00Z",
  "tasks": {
    "plan": { "state": "succeeded" },
    "finalize": { "state": "succeeded" }
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `workflowId` | string | Workflow that was run. |
| `runId` | string | Run identifier. |
| `state` | string | Terminal state: `succeeded`, `failed`, `cancelled`, or `stopped`. |
| `ok` | boolean | `true` if no task failed. |
| `completedAt` | string | ISO-8601 UTC timestamp. |
| `tasks` | object | Per-task summary. Each key is a task instance ID with `state` and optional `error`. |

The callback is **fire-and-forget** with a 15-second timeout. If the target server is unreachable, the failure is logged but does not affect the workflow run result.

---

## How Context Injection Works

When you POST to the start endpoint, every key in the `context` object is stored in the workflow run's context map. Tasks can declare `inputs` that match these keys — the runtime resolves them automatically:

```
POST body:  context.date = "2026-03-21"
                ↓
JCWF task:  "inputs": { "date": { "type": "string", "required": true } }
                ↓
Runtime:    {{date}} in queue_binding inline content → "2026-03-21"
```

See `example/workflows/hamburg-tourist-day-planner.jcwf` for a complete working example.

---

## Disk-First Traceability

For each start request, JarvisAgent persists the raw request JSON to disk **before** enqueuing the run:

```
# Webhook endpoint:
workflows/<workflowId>/webhook/<runId>/request.json

# Legacy n8n endpoint:
workflows/<workflowId>/<taskName>/n8n/<runId>/request.json
```

The run context automatically includes:

| Context key | Set by | Value |
|-------------|--------|-------|
| `webhook_request_path` | webhook | Absolute path to persisted `request.json` |
| `webhook_trigger_id` | webhook | Trigger ID that matched the request |
| `n8n_request_path` | n8n/start | Absolute path to persisted `request.json` |
| `n8n_task` | n8n/start | The `taskName` used for persistence |
| `callbackUrl` | both | The callback URL (if provided) |
| *(all context keys)* | both | Strings stored as-is; non-strings as raw JSON text |

---

## Monitoring Runs

> **Engine mode:** All monitoring and control endpoints require bearer token authentication. Add `-H "Authorization: Bearer <token>"` to every request. See the README for details on token setup.

### REST polling

```bash
# Active runs (all workflows)
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:8080/api/workflow-runs/active | jq .

# Specific run
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:8080/api/workflow-runs/<runId> | jq .

# Last completed run per workflow
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:8080/api/workflow-runs/last | jq .
```

### WebSocket (real-time)

Connect to `ws://localhost:8080/ws`. In **Engine mode**, send an auth message first:

```json
{ "type": "auth", "token": "<admin-token>" }
```

Then request run snapshots:

```json
{ "type": "workflow-runs-request" }
```

The server pushes `workflow-runs-snapshot` messages on every state change (task start/complete/fail, run start/complete). No polling needed after the initial request.

---

## n8n Custom Node

An n8n custom node bundle is provided in `integration/n8n-node/`:

- **Node name:** `JarvisAgent: Start Workflow`
- **Endpoint toggle:** Webhook (default, `POST /api/webhook/<id>`) or Legacy (`POST /api/integrations/n8n/start`)
- **HMAC signing:** Automatic when HMAC Secret is configured (webhook mode only)
- **Fields:** Base URL, Endpoint, Workflow ID, Run ID, HMAC Secret, Task Name (legacy), Callback URL, Context (JSON)

### Install into n8n

1. Copy or symlink `integration/n8n-node/` into your n8n custom extensions directory.
2. Restart n8n.
3. Add the "JarvisAgent: Start Workflow" node to a workflow.

---

## Example Workflow

See `example/workflows/hamburg-tourist-day-planner.jcwf` and its companion `hamburg-tourist-day-planner.md` for a complete integration demo:

- n8n fetches Hamburg weather data → triggers JarvisAgent → AI generates a tourist day plan
- Context variables (`date`, `timezone`, `rainCategory`, `weatherJson`) flow into AI prompts via `{{variable}}` expansion in inline `queue_binding` content

---

## Files in this folder

| File | Description |
|------|-------------|
| `README.md` | This file — quick-start guide |
| `n8n-node/` | n8n custom node bundle |
| `completions/` | Shell tab-completion scripts |
