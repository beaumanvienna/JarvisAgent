# hamburg‑tourist‑day‑planner — n8n Integration Demo

## Summary

The **hamburg‑tourist‑day‑planner** workflow demonstrates how JarvisAgent integrates with **n8n** (or any external system). An n8n workflow fetches live weather data for Hamburg, then triggers this JCWF workflow via `POST /api/webhook/hamburg-tourist-day-planner`, passing weather context. JarvisAgent generates a weather‑aware tourist day plan with AI.

This workflow shows:

- **External triggering** — n8n (or `curl`) starts a JCWF workflow via REST
- **Context injection** — weather data flows from the run context into AI prompt templates via `{{variable}}` expansion in inline `queue_binding` content
- **Standard ai_call pattern** — STNG/TASK/CNTX/PROB queue files, all inline

---

## Pipeline

```
n8n (or curl)
  │
  │  POST /api/webhook/hamburg-tourist-day-planner
  │  { context: { date, timezone, rainCategory, weatherJson } }
  │
  ▼
┌──────────────────────────────────────────────┐
│ plan (ai_call)                                │
│ Queue: STNG_plan + TASK_plan + CNTX_weather  │
│        + PROB_plan                            │
│ Output: PROB_plan.output.txt (Markdown)      │
└──────────────────────────────────────────────┘
```

---

## Task: plan (ai_call)

Generates a Markdown day plan using weather context injected from the run context.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Working directory | `../queue/hamburg-tourist-day-planner/01_plan` |
| Inputs (from context) | `date`, `timezone`, `rainCategory`, `weatherJson` |
| Output | `plan_markdown` (string) |
| Timeout | 120 s |

The task declares `inputs` that are resolved from the **run context** (populated by the webhook handler). The `{{variable}}` placeholders in the inline CNTX and PROB content are expanded by the runtime before the queue files are written.

### Queue files

| File | Role | Content |
|------|------|---------|
| `STNG_plan.txt` | Style | "Output only raw Markdown. No fences." |
| `TASK_plan.txt` | Instructions | Activity recommendations adapted to rain category |
| `CNTX_weather.txt` | Context | `Date: {{date}}`, `Rain category: {{rainCategory}}`, weather JSON |
| `PROB_plan.txt` | Request | "Generate the Hamburg tourist day plan for {{date}}" |

---

## Context Variables (provided by caller)

| Variable | Required | Description |
|----------|----------|-------------|
| `date` | Yes | Target date (e.g. `2026-03-21`) |
| `timezone` | No | IANA timezone (default: `Europe/Berlin`) |
| `rainCategory` | Yes | `no_rain`, `some_rain`, or `rain` |
| `weatherJson` | Yes | Raw weather forecast JSON string |

Additional context set automatically by the webhook handler:

| Variable | Description |
|----------|-------------|
| `webhook_request_path` | Absolute path to the persisted `request.json` |
| `webhook_trigger_id` | Trigger ID that matched the incoming request |
| `callbackUrl` | Callback URL for completion notification (if provided) |

---

## Integration Flow

1. **n8n** fetches weather data for Hamburg (e.g. from Open‑Meteo: `GET https://api.open-meteo.com/v1/forecast?latitude=53.5511&longitude=9.9937&daily=precipitation_sum,weather_code&timezone=Europe%2FBerlin`).
2. **n8n** sends `POST /api/webhook/hamburg-tourist-day-planner` with weather context.
3. **JarvisAgent** verifies the webhook trigger, persists the request JSON to disk, creates a workflow run, and seeds the run context with the provided fields.
4. The `plan` ai_call task writes STNG/TASK/CNTX/PROB files (with `{{variable}}` expansion), and the SessionManager dispatches the AI query.
5. The AI response is written to `PROB_plan.output.txt`.
6. If `callbackUrl` was provided, the runtime POSTs the completion payload back to the caller automatically.

---

## Testing with curl

```bash
curl -s -X POST http://localhost:8080/api/webhook/hamburg-tourist-day-planner \
  -H 'Content-Type: application/json' \
  -d '{
    "context": {
      "date": "2026-03-21",
      "timezone": "Europe/Berlin",
      "rainCategory": "some_rain",
      "weatherJson": "{\"temp_max\": 8, \"precipitation_sum\": 2.1, \"weather_code\": 61}"
    }
  }'
```

Check run status:
```bash
curl -s http://localhost:8080/api/workflow-runs/active | jq .
```
