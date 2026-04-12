# JarvisAgent Architecture

This document describes the **system architecture**, **runtime layers**, **deployment editions**, and **communication flow** of **JarvisAgent (j9t)**.

JarvisAgent is a modern C++ workflow orchestration and AI automation platform with a React-based visual editor and a secure production runtime. It runs as an autonomous background service with an embedded web server, a thread-pool-driven workflow engine, and an embedded Python scripting engine.

For the workflow file format itself see [JC_Workflow_Specification.md](JC_Workflow_Specification.md). For the cloud connector framework see [cloud-integration.md](cloud-integration.md). For REST endpoints see [api-endpoints.md](api-endpoints.md).

---

## System Overview

j9t is structured as a layered system:

```text
Browser UI (Workflow Editor + Dashboard)
        │
        │ REST + WebSocket
        ▼
Embedded Web Server (Crow)
        │
        │ workflow control / status / logs
        ▼
Workflow Runtime Engine (DAG executor)
        │
        ├── AI Request Pool (HTTP/2 multiplexed)
        ├── Python Engine (sub-interpreters)
        ├── Shell / internal C++ task executors
        ├── Cloud Connectors (ICloudConnector)
        └── Trigger Engine (cron / file-watch / webhook / manual)
        ▼
Disk-first outputs / dashboard / logs / audit log
```

All inputs, intermediate results, and outputs are persisted to disk before downstream consumption — for traceability, recovery, and auditability.

---

## Core Runtime Layers

| Layer | Implementation | Responsibility |
|---|---|---|
| Frontend | React / TypeScript (Vite) | Workflow editor (ReactFlow), dashboard, monitoring |
| Web Server | C++ / Crow | REST API, WebSocket, auth, routing, static assets |
| Workflow Runtime | C++ | DAG execution, dependency resolution, run control |
| Trigger Engine | C++ | Cron (IANA TZ), file-watch, webhook (HMAC), manual, auto-start |
| Task Executors | C++ pluggable | `ai_call`, `python`, `shell`, `internal` |
| AI Request Pool | C++ + libcurl | Parallel AI dispatch, HTTP/2 single-connection-per-provider |
| Python Engine | CPython embedded | Sub-interpreters with worker threads, load-balanced dispatch |
| Cloud Layer | `ICloudConnector` | Storage, DB, ALM, messaging, collaboration |
| Persistence | Disk-first | Inputs, outputs, logs, checkpoints |
| Engine Core | C++ | Thread pool, event queue, logging (`spdlog`), JSON (`simdjson`), profiling (`tracy`) |

---

## Deployment Editions

JarvisAgent ships as two compile-time editions controlled by the `--engine` Premake5 flag:

| Edition | Flag | Binary | Define | Purpose |
|---|---|---|---|---|
| **j9t Studio** (default) | *(none)* or `--studio` | `jarvisAgent-studio` | `J9T_STUDIO` | Full developer IDE — workflow editor, AI JCWF generation, AI assistant, provider and config management |
| **j9t Engine** | `--engine` | `jarvisAgent-engine` | *(none)* | Lean production server — runs workflows via cron, file-watch, and HMAC-authenticated webhooks. No workflow CRUD, no AI tooling, no unauthenticated run trigger |

Each edition produces a distinctly named binary and uses its own intermediate directory (`bin-int/studio/` vs `bin-int/engine/`), so switching editions triggers a full rebuild automatically.

### Compile-time gating

Studio-only code is controlled at two levels:

1. **File exclusions** (`premake5.lua`) — entire modules (`application/assistant/**`, `application/web/aiJcwfService.*`) are excluded from Engine builds via `removefiles`.
2. **Preprocessor guards** (`#ifdef J9T_STUDIO`) — code within shared files (`webServer.cpp`, `webServer.h`, `jarvisAgent.cpp`) is gated at call sites.

### Route architecture

The web server uses a three-method route split:

- `RegisterCommonRoutes()` — shared by both editions (status, workflow list, run monitoring, log, shutdown, dashboard UI, WebSocket)
- `RegisterEngineRoutes()` — present in both editions (webhook trigger, n8n integration)
- `RegisterStudioRoutes()` — Studio only, wrapped in `#ifdef J9T_STUDIO` (workflow CRUD, validation, run trigger, settings, AI interfaces, editor UI)

### Runtime edition detection

`GET /api/status` returns `edition` (`"engine"` or `"studio"`) and a `capabilities` boolean map. The frontend reads these to hide Studio-only UI elements at runtime — no separate frontend build required.

---

## Workflow Runtime

The workflow engine is **DAG-based**. Workflows are defined as `.jcwf` JSON files describing tasks, their dependencies, triggers, filters, and data flow.

**Supported task types:**

- `ai_call` — dispatched through the AI Request Pool
- `python` — executed by the embedded Python engine
- `shell` — system commands (PowerShell on Windows by default; bash via `use_bash` opt-in)
- `internal` — native C++ task modules

Tasks without mutual dependencies run in parallel via the shared thread pool.

**Workflow features:**

- **Per-item fan-out** — CSV / `text_lines` / Polarion-query filters produce item lists; `per_item` tasks spawn one AI call per item, all running in parallel. Downstream aggregation tasks consume results via glob patterns.
- **Template variables** — `{{binding.field}}` substitution per filter item.
- **Error branching** — branch nodes and controlflow edges route execution on success or failure for retry/recovery patterns.
- **Run control** — pause, resume, stop running workflows via REST API or editor UI.
- **Workflow versioning** — auto-backup on every save with full restore history (Studio only).
- **Cancellation tokens** — cooperative cancellation through the executor stack.
- **Watchdog** — inactivity-based timeout with heartbeat support for long-running shell and Python tasks.

See [JC_Workflow_Specification.md](JC_Workflow_Specification.md) for the full format definition and execution model.

---

## Queue File Processing

Alongside workflows, JarvisAgent also processes a **file-driven queue** of prompt inputs categorized by 4-letter prefix:

| Prefix | Category | Purpose |
|---|---|---|
| `STNG` | Settings | Style / behavior / tone modifiers |
| `CNTX` | Context | Background information for AI prompts |
| `TASK` | Task | Main instruction for the AI |
| `PROV` | Provider | AI provider config (never sent to AI) |
| `REQ` / *(no prefix)* | Requirement | Individual requirements processed against the assembled environment |

`STNG`, `CNTX`, and `TASK` are combined into an **environment** used alongside each individual requirement file during processing.

The file watcher monitors additions, modifications, and removals; the categorizer/tracker maintains modification state and triggers selective reprocessing — environment changes cause a full environment rebuild, requirement changes re-query only that file. Outputs are regenerated only when inputs or the environment have changed.

Office documents (PDF, DOCX, XLSX, PPTX, HTML) are detected as binaries and converted to Markdown via [MarkItDown](https://github.com/microsoft/markitdown), then chunked when oversized.

---

## AI Request Pool

The AI Request Pool dispatches concurrent AI requests in parallel. All outgoing requests share a single HTTP/2 connection per provider via a dedicated I/O thread, so network overhead stays minimal regardless of how many tasks are in flight.

**Supported providers:**

- OpenAI (GPT-4, GPT-4.1-mini, GPT-5) — `Authorization: Bearer` auth
- Google Gemini (native API) — `x-goog-api-key` auth, `/models/{model}:generateContent` URL scheme
- Gemini OpenAI-compatible endpoint

API keys are stored in an AES-256-GCM encrypted key store with master password. Workflow interfaces reference keys by `key_name`, resolved at runtime — no plaintext keys in workflow files or `config.json`.

---

## Communication Architecture

### REST API

The embedded web server (Crow) exposes REST endpoints for:

- workflow CRUD (Studio only)
- workflow runs and run control
- status and capabilities
- logs
- settings and connections
- MCP heartbeat
- webhook triggers (HMAC-signed)

See [api-endpoints.md](api-endpoints.md) for the complete reference.

### WebSocket

Real-time push channel for:

- task state changes
- workflow progress
- log streaming (up to 100k lines, color-coded severity)
- outputs and errors

Engine mode requires `{"type":"auth","token":"<token>"}` as the first WebSocket message.

---

## Cloud Architecture

Cloud integrations are implemented through a unified `ICloudConnector` framework. The connector abstracts authentication (OAuth2, JWT, SigV4, Azure Shared Key, BasicAuth, Bearer) so task executors only deal with resolved credentials. Named `CloudConnection` configs centralise endpoint and key references; secrets stay in the encrypted key store.

**Implemented connectors:**

- **Object storage** — S3 (+ MinIO/R2/Wasabi), Azure Blob (native), Google Cloud Storage (native)
- **Databases** — PostgreSQL (libpq), Snowflake (RSA JWT)
- **ALM** — Polarion (PAT), Jira, GitHub
- **Messaging** — Slack, Email (SMTP/IMAP)
- **Collaboration** — OneDrive (Graph PKCE), Google Sheets

Per-item output piping enables full round-trip pipelines: read from cloud → fan out per item → AI processes each → write results back.

The framework also exposes an **MCP sidecar** so Claude Desktop, Claude Code, and other MCP clients can list and run workflows directly.

See [cloud-integration.md](cloud-integration.md) for the full architecture and per-connector details.

---

## Security Architecture (Engine)

Engine edition includes a full security stack. Studio has no auth (developer workstation — localhost only).

**Authentication:**

- **Token lifecycle:** auto-generated 256-bit random hex on first Engine start, stored in `engine_api_token.txt` (file permissions `600`), logged to stdout once at startup. Tokens auto-expire after 90 days and auto-rotate.
- **REST endpoints:** protected via `Authorization: Bearer <token>` header. `CheckAdminAuth()` validates with constant-time comparison. Returns 401 (missing/malformed) or 403 (wrong token).
- **WebSocket:** token sent as first message `{"type":"auth","token":"..."}`. Unauthenticated connections can only send auth messages.
- **Webhooks:** HMAC-SHA256 via `X-Webhook-Signature` header. In Engine mode, a webhook secret is mandatory per workflow.
- **Public endpoints:** `GET /api/status`, `GET /`, `/dash-assets/*` — no auth required (health checks, dashboard SPA shell).

**Gateway integration:** when deployed behind an API gateway (Kong, AWS API Gateway, Traefik), `TrustedProxyHeader` and `TrustedRoleHeader` in `config.json` let j9t read the authenticated user identity and role from gateway-injected headers.

**RBAC:** three roles — `admin` (full access incl. shutdown, security logs), `operator` (run control, app logs), `viewer` (read-only monitoring). Gateway mode maps roles from headers; bearer token grants admin.

**Defense layers:**

- Per-IP rate limiting (token bucket, 100 req/min, burst 20). Returns 429 with `Retry-After`.
- Failed auth lockout (10 failures → 15-min IP ban).
- Request body size limit (configurable, default 10 MB).
- Security response headers — CSP, X-Frame-Options, HSTS, Referrer-Policy.

**Audit logging:** all auth events, webhook decisions, and run control actions logged to `log/security.txt` with IP, user identity, role, and endpoint. Viewable in the dashboard's Security tab.

**TLS:** built-in HTTPS via `TlsCert`/`TlsKey` in `config.json` (default port 8443), or deploy behind a TLS-terminating reverse proxy.

See [cyber security.md](cyber%20security.md) for the full threat model, deployment architecture, and operator responsibilities.

---

## Python Scripting Engine

The Python engine is embedded via the CPython C API and runs N **sub-interpreters** with per-engine worker threads and load-balanced task dispatch (Python 3.12+; graceful fallback to a single engine on older Python). It is used both for:

- workflow `python` task execution
- application-level extension hooks (`OnStart`, `OnEvent`, `OnShutdown`) for preprocessing such as PDF→Markdown conversion, Markdown chunking, and chunk-output recombination

---

## Design Goals

- **Disk-first** — every step materialized as a file for traceability and offline auditing.
- **Event-driven** — file watcher + selective rebuilds; no redundant work.
- **Massively parallel** — thread pool dispatches hundreds of concurrent AI requests; HTTP/2 multiplexing keeps network overhead minimal.
- **Embeddable** — single-binary server using the Crow micro web framework.
- **Extensible** — Python scripting hooks and pluggable task executors.
- **Operator-friendly** — terminal status line + browser dashboard with live updates.
- **Idempotent** — files are skipped efficiently when outputs are newer.
- **Secure by default in Engine** — auth, RBAC, TLS, audit log, HMAC webhooks.

---

## Related Documentation

- [README.md](../README.md) — high-level overview
- [INSTALL.md](../INSTALL.md) — pre-built package installation
- [DEVELOPMENT.md](../DEVELOPMENT.md) — build from source
- [JC_Workflow_Specification.md](JC_Workflow_Specification.md) — JCWF format
- [cloud-integration.md](cloud-integration.md) — cloud connector framework
- [api-endpoints.md](api-endpoints.md) — REST API reference
- [cyber security.md](cyber%20security.md) — security model
