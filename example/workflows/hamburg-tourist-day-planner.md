# hamburg‑tourist‑day‑planner Workflow – n8n Integration Demo

## Executive Summary

The **hamburg‑tourist‑day‑planner** workflow demonstrates how JarvisAgent integrates with **n8n** as an external orchestrator. An n8n workflow fetches live weather data for Hamburg, then triggers this JCWF workflow via the REST integration endpoint, passing weather context. JarvisAgent generates a tourist day plan with AI and returns the result to n8n via a callback URL.

At its core, this workflow shows:

- how n8n triggers a JCWF workflow via `POST /api/integrations/n8n/start`,
- how external context (`date`, `timezone`, `weatherJson`, `rainCategory`) flows into AI prompts,
- how a python finalization task posts results back to n8n's callback URL.

---

## Pipeline Overview

```
n8n (weather fetch)
  │
  │  POST /api/integrations/n8n/start
  │  context: { date, timezone, rainCategory, weatherJson, callbackUrl }
  │
  ▼
┌──────────────────┐
│ plan              │  ai_call: generate a tourist day plan for Hamburg
└────────┬─────────┘
         ▼
┌──────────────────┐
│ finalize          │  python: format result and POST back to n8n callback
└──────────────────┘
```

---

## Task Details

### 1. plan (ai_call)

Generates a markdown day plan using weather context injected by n8n.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Mode | `blocking` |
| System prompt | "You are a helpful travel planner." |
| User prompt | Includes `{{context.date}}`, `{{context.timezone}}`, `{{context.rainCategory}}`, `{{context.weatherJson}}` |
| Output | `plan_markdown` (string) |

The prompt instructs the AI to produce a full‑day tourist itinerary for Hamburg, adapted to the current weather conditions.

### 2. finalize (python)

Receives the AI‑generated plan and the n8n callback metadata, then posts the result back.

| Field | Value |
|-------|-------|
| Type | `python` |
| Depends on | `plan` |
| Entrypoint | `tasks/finalize.py` |
| Inputs | `plan_markdown`, `n8n_request_path`, `callbackUrl`, `n8n_task` |
| Output | `result_markdown` (string) |

---

## Context Variables (Provided by n8n)

| Variable | Description |
|----------|-------------|
| `context.date` | Target date for the day plan |
| `context.timezone` | Timezone string (e.g. `Europe/Berlin`) |
| `context.rainCategory` | Rain likelihood category from weather API |
| `context.weatherJson` | Full weather forecast JSON |
| `context.callbackUrl` | n8n webhook URL to receive results |
| `context.n8n_request_path` | Path to the persisted n8n request JSON |
| `context.n8n_task` | n8n task identifier |

---

## n8n Integration Flow

1. **n8n** fetches weather data for Hamburg from an external API.
2. **n8n** sends `POST /api/integrations/n8n/start` with `workflowId: "hamburg-tourist-day-planner"` and weather context.
3. **JarvisAgent** persists the request JSON, creates a workflow run, and seeds the context.
4. The `plan` task generates a day plan via AI.
5. The `finalize` task posts the result markdown back to n8n's `callbackUrl`.
6. **n8n** receives the result and continues its own workflow (e.g. send email, format PDF).

---

## Running

This workflow is designed to be triggered by n8n, not manually. However, it can be tested via curl:

```bash
curl -s -X POST http://localhost:8080/api/integrations/n8n/start \
  -H 'Content-Type: application/json' \
  -d '{
    "workflowId": "hamburg-tourist-day-planner",
    "context": {
      "date": "2026-02-17",
      "timezone": "Europe/Berlin",
      "rainCategory": "light",
      "weatherJson": "{\"temp\": 5, \"description\": \"overcast clouds\"}",
      "callbackUrl": "https://n8n.example.com/webhook/callback"
    }
  }'
```

---

## Key Concepts Demonstrated

- **n8n integration** — external orchestrator triggers JCWF workflows via REST
- **Context injection** — weather data flows from n8n into AI prompt templates
- **Callback pattern** — python task posts results back to the calling system
- **Two‑task pipeline** — minimal AI → finalize pattern for integration workflows
