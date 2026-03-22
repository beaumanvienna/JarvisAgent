# integration/n8n-node

Internal n8n custom node bundle for JarvisAgent.

## Node: JarvisAgent: Start Workflow

Starts a JarvisAgent workflow run. Returns the response under `item.json.jarvisAgent`.

### Endpoint modes

| Mode | URL | Description |
|------|-----|-------------|
| **Webhook** (default) | `POST /api/webhook/<workflowId>` | Recommended. Supports HMAC signing. |
| **Legacy n8n/start** | `POST /api/integrations/n8n/start` | Backward-compatible. Includes `taskName` field. |

### Fields

| Field | Modes | Description |
|-------|-------|-------------|
| Base URL | both | JarvisAgent server (default `http://localhost:8080`) |
| Endpoint | both | Webhook (recommended) or Legacy n8n/start |
| Workflow ID | both | JCWF workflow to run |
| Run ID | both | Custom run ID (optional, auto-generated if empty) |
| Task Name | legacy | On-disk traceability folder name (default `n8n`) |
| HMAC Secret | webhook | Shared secret for `X-Webhook-Signature` HMAC-SHA256 signing (optional) |
| Callback URL | both | URL to receive completion POST when the run finishes |
| Context (JSON) | both | JSON object injected into the workflow run context |

### HMAC signing

When using the **Webhook** endpoint with an HMAC Secret, the node automatically computes `HMAC-SHA256(secret, requestBody)` and sends it in the `X-Webhook-Signature: sha256=<hex>` header.

### Completion callback

If a **Callback URL** is provided, JarvisAgent will POST a JSON payload to that URL when the run finishes, containing `workflowId`, `runId`, `state`, `ok`, `completedAt`, and per-task `tasks` summary.

## Install into n8n

1. Copy (or symlink) this folder into your n8n custom extensions directory.
2. Restart n8n.
3. Add the node "JarvisAgent: Start Workflow" to a workflow.

You will need to adapt the exact installation step to your n8n deployment method.
