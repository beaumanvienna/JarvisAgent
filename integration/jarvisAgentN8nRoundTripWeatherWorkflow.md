# JarvisAgent × n8n – Hamburg Tourist Day Planner (Weather‑Aware)

A round-trip demo for a **tourist info booth in Hamburg**:

**n8n schedules + fetches weather → JarvisAgent generates content → n8n publishes it**.

Hamburg is intentionally hardcoded. This is not a generic “travel planner”; it is a daily content generator for a specific kiosk/site.

## Integration contract (efficient + spec-aligned)

### Why this contract
- n8n is best at:
  - scheduling (cron)
  - credentials/secret handling
  - calling external APIs (weather)
  - publishing to external systems (website)
- JarvisAgent is best at:
  - deterministic workflow execution (JCWF DAG)
  - AI-assisted content generation with auditable artifacts

### Contract
JarvisAgent exposes a **single integration endpoint** that accepts a small JSON payload.

- n8n → JarvisAgent: `POST /api/integrations/n8n/start`
  - body: `{ workflowId, runId?, context, callbackUrl?, callbackAuth? }`
- JarvisAgent → n8n: `POST callbackUrl`
  - body: `{ runId, status, startedAt, finishedAt, outputs, error? }`

### Disk-first traceability (implemented)
When JarvisAgent receives `POST /api/integrations/n8n/start` it **persists the raw request JSON to disk** before enqueuing the workflow run:

- `workflows/<workflowId>/<taskName>/n8n/<runId>/request.json`

JarvisAgent also injects additional run context so tasks can locate the trace file:

- `n8n_request_path` = path to `request.json`
- `n8n_task` = the `taskName` used for persistence
- `callbackUrl` (if provided)
- all keys from `context` (strings stored as-is; non-strings stored as raw JSON text)

### Efficient data transfer
JarvisAgent’s workflow run context is currently a `string -> string` map on the C++ side (`ContextMap`).
To avoid schema churn and keep payloads small:

- **n8n sends a compact JSON string** for weather (one field)
- plus a few scalar strings (date, timezone, kiosk language)

This avoids many nested context keys while still preserving full fidelity.

## High-level flow
```
(n8n) Cron @ 08:00 Europe/Berlin
   │
   ├─ HTTP Request: fetch Hamburg weather (Open-Meteo)
   │
   ├─ HTTP Request: start JarvisAgent workflow (date, weatherJson, rainIntensity, callbackUrl)
   │
   └─ Webhook: receive completion payload
        └─ HTTP Request / CMS node: publish Markdown to website

(JarvisAgent) JCWF workflow
   1) AI: categorize rain for today (none / light / rain)
   2) AI: choose activities based on rain category + forecast summary
   3) Python: finalize Markdown (insert date, ensure stable sections)
   4) Shell: POST results back to n8n callbackUrl
```

## n8n workflow sketch (nodes)

### 1) Trigger
- **Cron**: 08:00 daily
- **Timezone**: `Europe/Berlin`

### 2) Fetch weather (recommended API)
**Best pick: Open-Meteo**

Why:
- no API key required (easiest ops for kiosk)
- very simple HTTP+JSON
- includes precipitation fields suitable for “raininess” classification

Hamburg hardcoded coordinates:
- latitude: `53.5511`
- longitude: `9.9937`

Example Open-Meteo request (daily precipitation sum):
```
GET https://api.open-meteo.com/v1/forecast?latitude=53.5511&longitude=9.9937&daily=precipitation_sum,precipitation_hours,weather_code&timezone=Europe%2FBerlin
```

### 3) Start JarvisAgent workflow
**HTTP Request node** to JarvisAgent:

- URL: `http://<jarvis-host>:8080/api/integrations/n8n/start`
- JSON body example:

```json
{
  "workflowId": "hamburg-tourist-day-planner",
  "taskName": "n8n",
  "runId": "hamburg-{{ $now.toISODate() }}",
  "callbackUrl": "https://<n8n-host>/webhook/jarvisagent-hamburg",
  "context": {
    "date": "{{ $now.setZone('Europe/Berlin').toISODate() }}",
    "timezone": "Europe/Berlin",
    "language": "de",
    "location": "Hamburg",
    "rainIntensity": "{{ $json.rainIntensity }}",
    "weatherJson": "{{ JSON.stringify($json.openMeteoRaw) }}"
  }
}
```

JarvisAgent will write this exact request payload to:

- `workflows/<workflowId>/<taskName>/n8n/<runId>/request.json`

### 4) Receive + publish
- **Webhook Trigger**: `/webhook/jarvisagent-hamburg`
- **Publish**:
  - post returned markdown to your website/CMS, or
  - write it to a repo/file store and have the kiosk site serve it.

## JarvisAgent JCWF (core idea)

The workflow uses only **ai_call / python / shell**.

Key rules from `doc/JC_Workflow_Specification.md`:
- `shell` task `params.command` must start with `scripts/`
- Python tasks must declare `params.module` + `params.function`
- Task inputs/outputs are *declared* for validation; runtime values can come from the run context

### Contract expectations inside JarvisAgent
The integration endpoint should create a workflow run and populate `WorkflowRun.m_Context` with the incoming `context` fields.
Since `ContextMap` currently stores strings, the `weatherJson` field should be passed through as a raw JSON string.

```json
{
  "version": "1.0",
  "id": "hamburg-tourist-day-planner",
  "label": "Hamburg Tourist Day Planner (n8n round-trip)",
  "doc": "Given Hamburg weather + date, generate a kiosk-friendly Markdown plan and callback to n8n.",
  "triggers": [
    {
      "type": "manual",
      "id": "manual",
      "enabled": true,
      "params": { "exposed_in_ui": true }
    }
  ],
  "tasks": {
    "planActivities": {
      "id": "planActivities",
      "type": "ai_call",
      "label": "Choose activities based on rain category",
      "working_directory": "hamburg-tourist-day-planner/01_planActivities",
      "params": {
        "provider": "openai",
        "model": "gpt-4.1-mini",
        "mode": "one_shot",
        "prompt_template": "You write content for a tourist information kiosk in Hamburg. Date: {{date}} (timezone {{timezone}}).\n\nInputs:\n- location is always Hamburg\n- rainIntensity is provided . Categorize rain intensity into one of: no_rain | some_rain | rain\n- weatherJson is raw Open-Meteo JSON (string)\n\nTask:\nReturn STRICT JSON with keys:\n- markdown (string): a complete Markdown page for the kiosk website. Include the date in the first heading.\n- summary (string): 1-2 sentence teaser.\n- tags (array of strings): e.g. [\"outdoors\",\"museums\",\"family\"]\n\nContent rules (must follow):\n- If rainCategory=no_rain: emphasize outdoors and roaming (parks, harbor walk, etc.)\n- If rainCategory=some_rain: mixed plan, short outdoor walks + indoor attractions\n- If rainCategory=rain: focus on long indoor attractions with minimal walking outside (large museums, long tours, inside lunch)\n- Do NOT mention other cities. Hamburg only.\n- Keep suggestions realistic and non-controversial."
      },
      "inputs": {
        "date": { "type": "string", "required": true },
        "timezone": { "type": "string", "required": true },
        "rainCategory": { "type": "string", "required": true },
        "weatherJson": { "type": "string", "required": true }
      },
      "outputs": {
        "markdown": { "type": "string" },
        "summary": { "type": "string" }
      }
    },

    "finalizeMarkdown": {
      "id": "finalizeMarkdown",
      "type": "python",
      "label": "Finalize Markdown (date + stable structure)",
      "working_directory": "hamburg-tourist-day-planner/02_finalizeMarkdown",
      "depends_on": ["planActivities"],
      "params": { "module": "workflows.hamburgTouristPlanner", "function": "finalize_markdown" },
      "inputs": {
        "date": { "type": "string", "required": true },
        "markdown": { "type": "string", "required": true }
      },
      "outputs": {
        "markdown": { "type": "string" }
      }
    },

    "postCallback": {
      "id": "postCallback",
      "type": "shell",
      "label": "POST results back to n8n",
      "working_directory": "hamburg-tourist-day-planner/03_postCallback",
      "depends_on": ["finalizeMarkdown"],
      "params": {
        "command": "scripts/postJsonToWebhook.sh",
        "args": ["--url", "${inputs.callbackUrl}", "--json", "${inputs.callbackBodyJson}"]
      },
      "inputs": {
        "callbackUrl": { "type": "string", "required": true },
        "callbackBodyJson": { "type": "string", "required": true }
      }
    }
  },

  "dataflow": [
    { "from_task": "planActivities", "from_output": "markdown", "to_task": "finalizeMarkdown", "to_input": "markdown" },
    { "from_task": "finalizeMarkdown", "from_output": "markdown", "to_task": "postCallback", "to_input": "callbackBodyJson" }
  ]
}
```

### Notes
- The `postCallback` step expects a helper script `scripts/postJsonToWebhook.sh` that posts JSON to an URL.
- If you want to avoid shell, this can later become an `internal` task type (C++ HTTP client) for tighter control.

## Proposed n8n ↔ JarvisAgent interface (concrete)

### Start request (n8n → JarvisAgent)
`POST /api/integrations/n8n/start`

```json
{
  "workflowId": "hamburg-tourist-day-planner",
  "runId": "hamburg-2026-01-19",
  "callbackUrl": "https://<n8n-host>/webhook/jarvisagent-hamburg",
  "callbackAuth": {
    "type": "hmac-sha256",
    "secretName": "N8N_JARVIS_WEBHOOK_SECRET"
  },
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

### Completion callback (JarvisAgent → n8n)

```json
{
  "runId": "hamburg-2026-01-19",
  "status": "success",
  "startedAt": "2026-01-19T08:00:01+01:00",
  "finishedAt": "2026-01-19T08:00:12+01:00",
  "outputs": {
    "markdown": "# Hamburg: Tipps für den 19.01.2026\n...",
    "summary": "Heute: ...",
    "tags": ["museums", "family"]
  }
}
```

## Security notes (minimum viable)
- Allowlist the callback domain, or require `https`.
- Add an HMAC signature header on callbacks (n8n can verify in a Function node).

## Clean-up note
The bottom half of this document previously contained duplicated JCWF fragments; it has been replaced with one coherent, spec-aligned example.

## Why JarvisAgent is a “new player” worth betting on
JarvisAgent’s architecture is built around **reproducibility and efficiency**:

- It is a C++ agent that orchestrates work, reacts to file changes, and avoids unnecessary recomputation (only re-runs when inputs/environment change). 
- It is designed for **parallel processing**, multi-model support, and a live monitoring UI. 
- The workflow editor plan explicitly targets n8n-like observability while keeping JarvisAgent semantics. 

In other words:

- n8n is great at *connecting services*.
- JarvisAgent is great at *executing a validated DAG with AI in the loop*, producing auditable artifacts.
- Together: you get a system that is both **practical today** (integrations) and **scalable long-term** (deterministic automation + first-class AI).

## Optional enhancements (quick wins)
- Add API-key / HMAC signing on both the start call and callback (already called out in the plan). 
- Add a “template gallery” entry for this workflow so users can spin it up in one click. 
- Add a “strict DAG” validation toggle when importing from n8n (great for round-trip demos). 
      "inputs": {
        "runId": { "type": "string", "required": true },
        "city": { "type": "string", "required": true },
        "date": { "type": "string", "required": true },
        "itineraryMarkdown": { "type": "string", "required": true },
        "itinerarySummary": { "type": "string", "required": true }
      },
      "outputs": {
        "callbackBodyPath": { "type": "string" }
      }
    },

    "postCallback": {
      "id": "postCallback",
      "type": "shell",
      "label": "POST results to n8n callbackUrl",
      "working_directory": "smart-city-day-planner/05_postCallback",
      "depends_on": ["formatForN8n"],
      "params": {
        "command": "scripts/postJsonToWebhook.sh",
        "args": ["--url", "{{callbackUrl}}", "--json", "${input[0]}"]
      },
      "file_inputs": ["{{callbackBodyPath}}"]
    }
  },

  "dataflow": [
    { "from_task": "geocodeCity", "from_output": "latitude", "to_task": "fetchForecast", "to_input": "latitude" },
    { "from_task": "geocodeCity", "from_output": "longitude", "to_task": "fetchForecast", "to_input": "longitude" },
    { "from_task": "fetchForecast", "from_output": "forecastSummary", "to_task": "planActivities", "to_input": "forecastSummary" },
    { "from_task": "planActivities", "from_output": "itineraryMarkdown", "to_task": "formatForN8n", "to_input": "itineraryMarkdown" },
    { "from_task": "planActivities", "from_output": "itinerarySummary", "to_task": "formatForN8n", "to_input": "itinerarySummary" },
    { "from_task": "formatForN8n", "from_output": "callbackBodyPath", "to_task": "postCallback", "to_input": "callbackBodyPath" }
  ]
}
```

### Notes on compliance
- **Python tasks** reference `params.module` + `params.function`; JarvisAgent adds `scripts/` to `sys.path` 
- **Shell tasks** must run scripts under `scripts/` (security policy) 
- Task inputs can be resolved from dataflow, context, or literals; missing required inputs fail fast 

## What makes the workflow “smart” (AI where it matters)
JarvisAgent doesn’t use AI as a toy—AI is used exactly where deterministic code becomes brittle:

- Interpreting the forecast into **human-friendly plans**
- Adapting to preferences (budget, pace, indoor/outdoor)
- Producing polished copy for email/Discord

Everything else is deterministic and testable:

- Fetching weather = Python
- Formatting callback payload = Python
- Posting callback = shell

## Why this sells JarvisAgent (the pitch)
JarvisAgent is already designed as an automation agent: it monitors inputs, runs work in parallel, and only regenerates outputs when inputs changed. That complements n8n’s strengths.

Where JarvisAgent fits in the landscape:

- **n8n** is the integration fabric.
- **JarvisAgent** is the “run heavy, verifiable work” engine:
  - deterministic orchestration
  - explicit artifacts
  - clear policies (paths, scripts, isolation)
  - a canonical workflow spec (JCWF)

It also aligns with your integration plan philosophy: treat n8n as an **interchange** or integration format, while JarvisAgent’s internal graph/JCWF remains canonical 

## Security checklist (demo-friendly)
When using workflow JSON and callbacks, assume secrets exist and behave accordingly:

- Don’t log raw imported workflow JSON.
- Redact secrets on import/export; treat exports as sensitive artifacts 
- Sign start requests (API key or HMAC) and optionally allowlist callback domains 

## Demo script (how you present it)
1. Open n8n. Trigger “Smart City Day Planner” manually with `city=Lisbon`.
2. n8n calls JarvisAgent start endpoint.
3. JarvisAgent runs tasks; you show the DAG updating in JarvisAgent’s dashboard/editor (this is exactly the observability goal) 
4. JarvisAgent posts back to the n8n webhook.
5. n8n sends:
   - Email with full itinerary markdown
   - Discord message with highlights

That’s the “new player” message: **JarvisAgent brings deterministic DAG execution + AI‑smart content generation to the automation ecosystem, while still playing nicely with the existing integration hub.**

      "inputs": {
        "runId": { "type": "string", "required": true },
        "city": { "type": "string", "required": true },
        "date": { "type": "string", "required": true },
        "itineraryMarkdown": { "type": "string", "required": true },
        "itinerarySummary": { "type": "string", "required": true }
      },
      "outputs": {
        "callbackBodyJson": { "type": "string" }
      }
    },

    "postCallback": {
      "id": "postCallback",
      "type": "shell",
      "label": "POST completion payload back to n8n",
      "working_directory": "smart-city-day-planner/05_postCallback",
      "depends_on": ["formatForN8n"],
      "params": {
        "command": "scripts/postJsonToWebhook.sh",
        "args": ["${input[0]}", "${input[1]}"]
      },
      "file_inputs": [
        "smart-city-day-planner/04_formatForN8n/callbackUrl.txt",
        "smart-city-day-planner/04_formatForN8n/callbackBody.json"
      ]
    }
  },

  "dataflow": [
    { "from_task": "geocodeCity", "from_output": "latitude", "to_task": "fetchForecast", "to_input": "latitude" },
    { "from_task": "geocodeCity", "from_output": "longitude", "to_task": "fetchForecast", "to_input": "longitude" },
    { "from_task": "fetchForecast", "from_output": "forecastSummary", "to_task": "planActivities", "to_input": "forecastSummary" },
    { "from_task": "planActivities", "from_output": "itineraryMarkdown", "to_task": "formatForN8n", "to_input": "itineraryMarkdown" },
    { "from_task": "planActivities", "from_output": "itinerarySummary", "to_task": "formatForN8n", "to_input": "itinerarySummary" }
  ]
}
```

### Notes about this JCWF
- **Python tasks** must define `params.module` + `params.function` 
- JarvisAgent enforces per-task working directories and has strict runtime path policies (e.g., only AI tasks write into queue) 

## What makes JarvisAgent different (the pitch)
JarvisAgent is not “just another workflow tool.” It is designed as an **agent**:

- A C++ runtime that can run in the background, monitor work, and execute tasks deterministically 
- Built-in dependency tracking so outputs are regenerated only when inputs change 
- A first-class, versioned workflow format (**JCWF**) with validation and strong runtime rules.
- A web dashboard and an in-dashboard graph editor direction that explicitly targets n8n-style UX while preserving JarvisAgent semantics 

In other words:

> **n8n** is fantastic at connecting services.
> **JarvisAgent** is fantastic at running “smart compute” workflows that produce real artifacts and can be validated, debugged, and rerun deterministically.

## Why AI belongs *inside* the workflow
In this demo, AI does something concrete and bounded:

- It receives **structured inputs** (city/date/preferences + forecast summary) and returns **structured JSON**.
- The rest of the workflow treats the AI output as data to be formatted and delivered.

This is how you keep AI “smart” without making the whole system unpredictable.

## Security / safety baseline
If you import/export n8n workflows, assume secrets might exist in workflow JSON; the integration plan explicitly calls out secret scanning and redaction as critical 

For the round-trip callback model, use:
- API key or HMAC signatures
- optional callback-domain allowlist
- rate limiting 

## Where this fits in the broader roadmap
JarvisAgent’s integration plan treats n8n JSON as an interchange format, keeping JarvisAgent decoupled while still supporting round-trip workflows when explicitly enabled 

---

## Demo script (how to present this in 90 seconds)
1. Show the n8n canvas: cron/manual trigger → HTTP Request → webhook → Discord/Email.
2. Click “Run” and emphasize: n8n is delegating **compute**.
3. Show JarvisAgent workflow graph (JCWF): weather fetch → AI planner → callback.
4. Show that JarvisAgent produces artifacts and can re-run deterministically.
5. The Discord message arrives with a weather-aware itinerary.

