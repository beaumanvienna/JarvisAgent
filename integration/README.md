# integration/

This folder contains everything related to external integrations for JarvisAgent.

## n8n

### Start a workflow run (disk-first traceability)

**Endpoint**

`POST /api/integrations/n8n/start`

**Purpose**

- Accept a workflow start request from n8n.
- Persist the raw request JSON to disk for traceability.
- Enqueue a workflow run with the provided `runId` (or auto-generated).

**Request body**

```json
{
  "workflowId": "hamburg-tourist-day-planner",
  "taskName": "n8n",
  "runId": "hamburg-2026-01-19",
  "callbackUrl": "https://<n8n-host>/webhook/jarvisagent-hamburg",
  "context": {
    "date": "2026-01-19",
    "timezone": "Europe/Berlin",
    "language": "de",
    "location": "Hamburg",
    "rainCategory": "some_rain",
    "weatherJson": "{...raw open-meteo json...}"
  }
}
```

**Response** (202 Accepted)

```json
{
  "ok": true,
  "workflowId": "hamburg-tourist-day-planner",
  "runId": "hamburg-2026-01-19",
  "requestPath": "workflows/<workflowId>/<taskName>/n8n/<runId>/request.json"
}
```

### Trace files written by JarvisAgent

For each start request, JarvisAgent writes:

- `workflows/<workflowId>/<taskName>/n8n/<runId>/request.json`

The workflow run context will also include:

- `n8n_request_path`: path to `request.json`
- `n8n_task`: the `taskName` used for persistence
- `callbackUrl`: if provided
- all keys under the request `context` object (strings stored as-is; non-strings stored as raw JSON text)

### Getting run status

Use the existing workflow run endpoints:

- `GET /api/workflow-runs/<runId>`
- `GET /api/workflow-runs/active`
- `GET /api/workflow-runs/last`

### n8n node bundle

An internal n8n custom node bundle lives here:

- `integration/n8n-node/`

It provides a basic node that calls `POST /api/integrations/n8n/start`.
