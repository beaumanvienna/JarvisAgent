# n8n ↔ JarvisAgent Integration Plan — Webhook Trigger & Seamless Integration

> **Last updated:** 2026-03-21
> **Status:** All four phases completed.
> **Covers:** TODO #9 (Webhook trigger type) + Editor TODO (n8n seamless integration)

## Design Philosophy

- **JCWF is canonical.** JarvisAgent does not import/export n8n workflow JSON. n8n interacts with JarvisAgent via REST/webhook — each system runs its own workflows.
- **n8n orchestrates integrations** (APIs, credentials, scheduling). **JarvisAgent executes validated DAGs** with AI in the loop, producing auditable artifacts.
- **Webhook triggers** are the primary integration surface: n8n (or any external system / `curl`) POSTs to JarvisAgent to start a workflow run, and JarvisAgent POSTs back to a callback URL when the run finishes.

---

## Architecture

```
External caller (n8n / curl / CI)
   │
   │  POST /api/webhook/<workflowId>
   │  X-Webhook-Signature: sha256=<hex>
   │  { "context": { ... }, "callbackUrl": "..." }
   │
   ▼
┌─────────────────────────────────────┐
│  JarvisAgent Web Server (Crow)      │
│                                     │
│  1. Look up workflow in registry    │
│  2. Verify webhook trigger exists   │
│  3. Verify HMAC signature (if set)  │
│  4. Enqueue run with context        │
│  5. Return 202 { runId }            │
│                                     │
│  ... runtime executes DAG ...       │
│                                     │
│  6. Run completes → POST callback   │
│     POST <callbackUrl>              │
│     X-Callback-Signature: sha256=…  │
│     { runId, status, outputs, ... } │
└─────────────────────────────────────┘
```

---

## JCWF Trigger Schema

```json
{
  "triggers": [
    {
      "type": "webhook",
      "id": "external",
      "enabled": true,
      "params": {
        "secret": "my-shared-secret"
      }
    }
  ]
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `type` | Yes | Must be `"webhook"`. |
| `id` | Yes | Unique trigger ID within the workflow. |
| `enabled` | No | Default `true`. Disabled triggers reject incoming requests. |
| `params.secret` | No | Shared secret for HMAC-SHA256 verification. If omitted, signature check is skipped (open webhook). |

---

## Phase 1 — Backend: Webhook Trigger Type + Endpoint + HMAC ✅

### 1a. Enum + parser
- Add `Webhook` to `WorkflowTriggerType` in `workflowTypes.h`
- Add `"webhook"` → `WorkflowTriggerType::Webhook` in `workflowJsonParser.cpp`
- Add webhook case to `workflowTriggerBinder.cpp` (register as webhook trigger in TriggerEngine)
- Add webhook trigger validation in `workflowValidator.cpp`

### 1b. TriggerEngine: webhook trigger storage
- Add `WebhookTriggerInstance` struct to `triggerEngine.h` (workflowId, triggerId, secret, enabled)
- Add `AddWebhookTrigger()` and `GetWebhookTrigger()` methods
- Add `m_WebhookTriggers` vector + `m_WebhookIndex` map (workflowId → index) for O(1) lookup

### 1c. REST endpoint
- Add `POST /api/webhook/<workflowId>` route in `webServer.cpp`
- Handler: `HandleWebhookPost(req, workflowId)`
  1. Look up webhook trigger via TriggerEngine
  2. Verify `X-Webhook-Signature` header if secret is configured
  3. Parse optional `context`, `callbackUrl`, `runId` from body
  4. Persist request to disk (same traceability as n8n/start)
  5. Enqueue run with context (including `callbackUrl`, `webhook_request_path`)
  6. Return 202 `{ ok, workflowId, runId }`

### 1d. HMAC-SHA256 verification
- Compute `HMAC-SHA256(secret, requestBody)` → hex
- Compare against `X-Webhook-Signature: sha256=<hex>` header
- Use OpenSSL HMAC (already linked via the crypto dependency)
- Return 401 on mismatch

---

## Phase 2 — Backend: Completion Callback ✅

When a workflow run finishes (succeeded, failed, cancelled, stopped):
1. Check if run context contains `callbackUrl`
2. If present, POST completion payload via CurlWrapper (5s timeout):
   ```json
   {
     "runId": "...",
     "workflowId": "...",
     "status": "succeeded|failed|cancelled|stopped",
     "startedAt": "2026-03-21T08:00:01Z",
     "completedAt": "2026-03-21T08:00:12Z",
     "outputs": { ... },
     "error": "..." 
   }
   ```
3. Optionally sign the callback body with the webhook trigger's secret (via `X-Callback-Signature`)
4. Log success/failure of the callback POST; do not retry (fire-and-forget)

Implementation location: `WorkflowRuntimeManager::Update()` — `FireCompletionCallback()` is called right after the run state is finalized, before the run is erased from `m_ActiveRuns`. Uses raw libcurl on a detached thread (15 s timeout).

---

## Phase 3 — Frontend: Webhook Trigger Editing ✅

- Add `"webhook"` to the trigger type dropdown in the editor inspector
- When selected, show:
  - **Webhook URL** (readonly, auto-generated): `http://<host>:8080/api/webhook/<workflowId>`
  - **Secret** field (password input, optional)
  - **Copy URL** button for convenience
- Validator: warn if secret is empty ("open webhook — no signature verification")

---

## Phase 4 — Docs, n8n Node Update, Cleanup ✅

- Update `doc/api-endpoints.md` with `/api/webhook/<id>` endpoint docs
- Update `integration/README.md` with webhook trigger usage
- Update n8n custom node (`integration/n8n-node/`) to optionally use the new webhook endpoint
- Clean up stale JCWF fragments in `jarvisAgentN8nRoundTripWeatherWorkflow.md`
- Update `JarvisAgent TODO List.md` and `workflow-editor/todo.md`

---

## Existing Infrastructure (preserved)

- `POST /api/integrations/n8n/start` — kept for backward compatibility. The new `/api/webhook/<id>` endpoint is the preferred path for all external callers.
- `POST /api/workflows/<id>/run` — kept for manual runs from the editor UI (requires `manual_start: true`).

---

## Security Checklist

- [x] HMAC-SHA256 on inbound webhooks (optional per-trigger)
- [ ] HMAC-SHA256 on outbound callbacks (using same secret) — deferred (callback is fire-and-forget, no signing yet)
- [x] Never log raw request bodies containing secrets
- [x] Webhook secrets stored in JCWF trigger params (cleartext in .jcwf file) — document that users should keep .jcwf files private or use environment variable references in a future iteration
- [ ] Rate limiting deferred (not MVP)
- [ ] Callback URL allowlisting deferred (not MVP)

---

## Testing Plan

1. **curl smoke test (no HMAC):** `curl -X POST http://localhost:8080/api/webhook/myWorkflow -d '{"context":{"key":"value"}}'`
2. **curl with HMAC:** compute signature, send `X-Webhook-Signature: sha256=<hex>`
3. **Bad signature:** verify 401 response
4. **Disabled trigger:** verify 403 response
5. **No webhook trigger:** verify 404 response
6. **Callback:** start run with `callbackUrl`, verify POST arrives when run finishes
7. **n8n round-trip:** n8n HTTP Request → JarvisAgent webhook → run → callback → n8n webhook trigger

---

## References

- Existing n8n integration endpoint: `POST /api/integrations/n8n/start` (webServer.cpp)
- Hamburg weather demo: `integration/jarvisAgentN8nRoundTripWeatherWorkflow.md`
- n8n custom node: `integration/n8n-node/`
- JC Workflow Specification: `doc/JC_Workflow_Specification.md`
