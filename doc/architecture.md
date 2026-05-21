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
- **Concurrency policy** — per-JCWF `"concurrency"` field (`serialize` default / `parallel` / `reject`) controls what happens when a run is requested while another run of the same workflow is already active.  Serialize queues the second run as `pending` in a per-workflow FIFO and drains it when the active run completes; reject returns HTTP 409.  See [JC_Workflow_Specification.md](JC_Workflow_Specification.md) §3.1.1.
- **Workflow versioning** — auto-backup on every save with full restore history (Studio only).
- **Cancellation tokens** — cooperative cancellation through the executor stack.
- **Watchdog** — inactivity-based timeout with heartbeat support for long-running shell and Python tasks.

See [JC_Workflow_Specification.md](JC_Workflow_Specification.md) for the full format definition and execution model.

### Adhoc submission + artifact retrieval

Beyond registered workflows, external MCP agents can submit one-shot JCWFs via `POST /api/workflows/run-adhoc`. The `AdhocWorkflowManager` (`application/workflow/adhocWorkflowManager.h`) stages each submission into a per-user folder — `_adhoc/<user_slug>/<timestamp>_<counter>_del-<delete-at>/` — with a `meta.json` (owner attribution) and, at completion, a `manifest.json` (file inventory). Scripts referenced by the JCWF must already exist under `scripts/`; the submit handler verifies every `params.command` / `params.module` up front and rejects missing ones with `400 missing_scripts`.

Once a run is staged, its artefacts are discoverable and retrievable through dedicated endpoints:

- `GET /api/workflow-runs/<runId>/files` — lists every regular file in the run folder with path, size, mtime, content-type, plus both a `local_path` (same-host agents) and a `download_url` (remote agents). Retention (`policy`, `delete_at`, `seconds_remaining`) is echoed so callers know how long the artefacts will live.
- `GET /api/workflow-runs/<runId>/files/<path>` — streams a single file. Supports `Range:` for large files; single-response cap is 10 MB. Full-file responses carry `X-Content-SHA256` for integrity verification.
- `GET /api/scripts` — the `ScriptCatalog` (`application/workflow/scriptCatalog.h`) scans `scripts/` at startup and parses each script's `@jarvis-script` metadata header (short / params / outputs / description) so agents can discover what's pre-deployed before composing a JCWF.

Authorization: operators can read their own runs; admins can read any run (with an `admin_cross_user_read` audit line). Path traversal (`..`, absolute paths, symlinks) is rejected before the filesystem is touched. A background reaper thread sweeps runs whose `delete-at` has passed; empty user-slug directories are pruned at the same time so the `_adhoc/` root stays tidy.

For `ai_call` completion handling (envelope dispatch, chunking, reduce pass, schema validation, transcripts, provider adapters) see **AI Dispatch Pipeline** below.  Adhoc `ai_call` goes through the same path via the shared runtime manager.

---

## AI Dispatch Pipeline

```
JCWF ai_call task
  ↓  AiCallTaskExecutor
       ├─ template-resolve {{...}} vars
       ├─ write STNG / CNTX / TASK / PROB / PROV files to queue folder (disk-first)
       │    PROV = provider sidecar (interface name, model, url, key_name, api_type)
       ├─ markitdown-convert any office files in cntx_files (PDF/DOCX/XLSX/PPTX/ODT)
       ├─ structure-aware chunker (if body > interface max_context_tokens)
       └─ construct 1 AiInvocation per PROB × chunk
            (m_InterfaceName mirrors PROV content; envelope is load-bearing, PROV stays for replay/debug)
  ↓  AiRequestPool::Submit(envelope)          [direct in-process call]
  ↓  IRequestBuilder::BuildBody(envelope)     [per-provider: API1/2/3/4/5/6]
  ↓  CurlMultiDispatcher                      [HTTP/2, shared I/O thread]
  ↑  ReplyParser                              [per-provider; returns AiReply]
  ↑  AiReply
  ↓  AiRequestPool::OnReply(handle, reply)    [direct callback]
       ├─ strip single-fence wrappers around the reply text
       ├─ if output_schema: parse+validate with simdjson; retry on failure (≤ output_retries)
       ├─ write <prob>.output.{json|txt}
       └─ write <prob>.transcript.json
  ↓  AiRequestPool::TryPopCompletion()        [non-blocking poll]
  ↓  workflow runtime tick picks up the completion

Event bus (fire-and-forget, observability only):
  AiCallStartedEvent → AiCallCompletedEvent | AiCallFailedEvent
  Category: EventCategoryAi   (TUI / dashboard WebSocket / Python hooks)
```

### Key types and decisions

- **`AiInvocation`** (`application/workflow/aiInvocation.h`) is a pure data struct — no inheritance. Carries interface name, optional model override, `AiSettings` (temperature, seed, max_tokens), messages, optional `output_schema`, `StructuredMode`, timeout, retry policy, queue folder, prob name, optional chunk index/count. This envelope is the unambiguous source of truth at runtime.
- **`AiReply`** carries `Kind` (`Text | Structured | Error`), the response payload, `AiUsage` (input/output/total tokens), finish reason, system fingerprint.  On `Error`, `AiReply::m_Error` (`AiError`) propagates the parsed provider discriminator (`m_ProviderErrorCode` + `m_ProviderErrorType` — raw, for logs), the UI-facing semantic category (`m_Category : ProviderErrorCategory` — closed enum, branched on by the dashboard so provider strings never reach React), and `m_RetryAfterSeconds` (`std::optional<int>`, from the `Retry-After` header threaded through `CurlMultiDispatcher::Callback`'s 3rd arg).  The HTTP-error path in `AiRequestPool` re-parses the response body when the dispatcher short-circuited on a 4xx/5xx, so the discriminator survives end-to-end into the `ai-call-failed` WebSocket payload (see `doc/api-endpoints.md`).
- **`IRequestBuilder`** + **`ReplyParser`** are symmetric per-provider abstractions. Adding a provider means adding one builder and one parser, plus one enum variant — no other code changes.
- **`IAuthSigner`** (`engine/curlWrapper/authSigner.h`) is the single authentication strategy. `[[nodiscard]] bool Apply(QueryData, vector<string>& headers, string& errorMessage) const` is called by both `CurlWrapper::Query` (sync) and `LiveTransport` (async parallel, hosted under `CurlMultiDispatcher`) — no per-style branching in the curl layer.  Returns `false` on validation failure (empty/whitespace credentials, missing SigV4 region/secret); upstream callers emit a structured ERROR with run context (`m_QuotaKey`, `m_CancelKey`, URL) for the dashboard's run analyzer rather than letting the request go out unsigned and bounce off the provider as an opaque 401. Concrete signers: `BearerSigner`, `XGoogApiKeySigner`, `AnthropicXApiKeySigner`, `AzureApiKeySigner`, `SigV4Signer`. SigV4 is hand-rolled on OpenSSL (`engine/curlWrapper/awsSigV4.{h,cpp}`) — no `aws-sdk-cpp` dependency.  A 4-test startup self-test in debug builds checks SHA256-empty + AWS-published key-derivation hex + determinism + a full-chain known-answer signature; intermediate signing keys are RAII-cleansed via `OPENSSL_cleanse`.  See `doc/cyber security.md` "IAuthSigner Security", "CurlMultiDispatcher Security", "CurlWrapper Security", and "SigV4 Signing Security" for the full per-subsystem guarantees (response-body cap, exception-safe C-boundary callbacks, Submit-after-shutdown race closure, fail-path log enrichment, IPv6 host extraction, etc.).
- **`AiRequestPool::OnRequestFailed`** is the symmetric counterpart to `OnOutputFileCreated`. When a curl error or parse failure prevents writing `<prob>.output.*`, the pool calls `OnRequestFailed(path, AiError const&)` to mark the pending entry failed, emit the single consolidated user-visible ERROR log line — `HTTP {status} (code='{ProviderErrorCode}', type='{ProviderErrorType}', category={CategoryToString(...)}) run='...' workflow='...' task='...' message='...' path='...'` — and queue a failed completion so workflow tasks transition out of `waiting_external` deterministically rather than waiting on the `ai_call` deadline.  The dispatcher's intermediate "HTTP 429 retries exhausted" line stays at WARN to avoid a duplicate ERROR for the same failure; the dashboard run analyzer filters on the OnRequestFailed line because it's the one with the runId substring + semantic category.

**Extension seams.** The envelope is deliberately designed to accept additive post-1.0 capabilities without structural change:

- **Native tool-calling** — a future `m_Tools: vector<ToolDef>` field on `AiInvocation` plumbs directly into provider tool-call protocols (OpenAI `tools`, Gemini `functionDeclarations`, Anthropic `tools`). `ReplyParser` gains a `GetToolCalls()` virtual, `AiReply::Kind` gains `ToolCall`. Tracked in `todo.md`.
- **Multi-turn conversation** — `m_Messages` already carries a full message list; a multi-turn executor keeps appending to it across tool-return rounds inside one `ai_call` task.
- **Agent-of-agents / Claude Code orchestration** — the same envelope path drives outbound calls into sub-agents. Tracked in `todo.md`.
- **Additional providers beyond the current six** — each needs one new `InterfaceType` + builder + parser + (if novel auth) one new `IAuthSigner` implementation. Auth shape is the only meaningful axis of variation: SigV4 (Bedrock), api-key header (Azure), Bearer / x-api-key / x-goog-api-key (existing).

All four build on the existing `AiInvocation → IRequestBuilder → CurlMultiDispatcher → ReplyParser → AiReply` pipeline; none require touching transport, schema validation, chunking, reduce pass, transcripts, or the event layer.

### Queue-folder convention (disk-first philosophy)

Every input and output is on disk, every call is replayable. A task's queue folder contains:

| Prefix | Role |
|---|---|
| `STNG_*` | Settings (style, tone, constraints) |
| `CNTX_*` | Context (background, source data, upstream task outputs) |
| `TASK_*` | Task instruction |
| `PROB_*` | One problem/prompt per fan-out item |
| `PROV_*` | Provider sidecar — written per dispatch (interface/model/url/key_name/api_type). **Write-only from the dispatch code path**; only replay tooling reads PROV back. |
| `<prob>.output.{txt,json}` | The AI response |
| `<prob>.output.chunk<i>-of-<N>.txt` | Per-chunk intermediates when chunking fired |
| `<prob>.transcript.json` | Request + response turns, usage, finish_reason, system fingerprint — one per PROB |

STNG / CNTX / TASK are all **optional** — a single PROB with non-whitespace content is enough for dispatch. When any section is missing the runtime logs an advisory ("no STNG content — default settings apply") but proceeds. The old strict-environment rule was a holdover from the file-watcher-driven era when incomplete environments silently stalled runs.

### Structured output

`ai_call` tasks may declare `output_schema` (a JSON Schema Draft 2020-12 subset) and `output_retries`. On reply the pool extracts JSON from the text (tolerating ```json``` fences), validates against the schema, and on failure re-dispatches with a correction message up to `output_retries` attempts. Supported keywords: `type`, `properties`, `required`, `additionalProperties`, `items`, `enum`, `minimum`, `maximum`, `minLength`, `maxLength`, `pattern`, `oneOf`, `anyOf`, `$ref`, `$defs`. Unsupported keywords are rejected at schema-load time, not at reply time — authors see errors before a run starts. Structured mode selection is per-provider: native `response_format: json_schema` on OpenAI (API1/API2) and Gemini (API3), forced-tool shim on Anthropic (API4) with `tool_choice: {"type":"tool","name":"output"}`.

Validated replies land in a `<stem>.output.json` file. Downstream tasks can reference any field via the standard `{{upstreamTaskId.json.PATH}}` template resolver — the runtime parses any `.json` file registered in the upstream's output values and flattens it into dotted-path entries, symmetric with the cloud-task `response.json` mechanism. This lets a schema-validated `ai_call` feed concrete fields into cloud writes, SQL inserts, or other `ai_call` prompts without an intermediate parser task.

### Chunking + reduce

Each `ApiInterface` declares a `max_context_tokens` budget. If not set in `config.json`, a curated model-name fallback table resolves a default (GPT-4-family 128 K, Claude 200 K, Gemini 1.5/2 1 M, Llama/Qwen/DeepSeek/Phi 128 K, Mistral/Mixtral 32 K, unknown 50 K). When an envelope's user message exceeds the budget, `ChunkPlanner` splits only the CNTX portion at markdown section boundaries (preferring `#`/`##`/`###` whole sections, subdividing only when a section alone overruns). N envelopes fan out in parallel — each carries the full STNG/TASK + one slice + original PROB. Once all N replies land, a **reduce envelope** runs: it carries all N partial answers plus the original PROB with an instruction to produce a single unified response. The reduced reply becomes `<prob>.output.txt`. If any chunk fails, the fallback is plain concatenation with HTML comment separators.

Chunking and `output_schema` are mutually exclusive (schema wins — schema enforcement requires a whole-object reply, which a chunked response can't satisfy).

### Reply-fence heuristic

Some models (notably Claude Haiku) occasionally ignore "no fences" STNG instructions and wrap the entire reply in a triple-backtick block — e.g. ```` ```cpp … ``` ````. Downstream compilers, Make, and Python execution choke on those leading backticks, so `AiRequestPool` applies a best-effort `StripWholeReplyFence` pass: if the whole reply is a single fenced block with no nested ```` ``` ```` sequences, the outer fence (plus optional language tag line) is removed. The stripped-count is exposed as `ai_fence_strips` on `/api/debug/signals`.

Diagram / markdown authoring formats keep their fence wrapper — the strip pass honours a language-tag keep-list (`mermaid`, `dot`, `plantuml`, `graphviz`, `latex`, `tex`, `markdown`, `md`) so an AI task emitting a ```` ```mermaid … ``` ```` flowchart for embedding into a larger markdown document survives intact through downstream renderers (`scripts/mermaidMdToPdf.sh`, pandoc, etc.). For workflows where the AI is asked to produce a diagram, the recommended pattern is still `output_schema` with a `mermaid` string field — the combiner then owns the ```` ```mermaid ```` wrapping and the strip pass has nothing to act on.

### Embedded schema and generation guide

`doc/jcwf.schema.json` and `doc/jcwf_generation_guide.md` are embedded into the binary at premake time (`tools/embedAsHeader` in `premake5.lua` → `application/json/jcwfSchema.generated.h` / `jcwfGenerationGuide.generated.h`) as `kJcwfSchemaJson` / `kJcwfGenerationGuide` constants. This means:

- The editor's AI-driven JCWF generate flow (`AiJcwfService::GenerateAsync`) declares `kJcwfSchemaJson` on its envelope and the reply is schema-validated (with retry) before being written — invalid JCWFs never reach the user.
- The generation guide is always at hand for the Generate/Fix pipeline without reading from disk, so it works identically in Studio, Engine-in-Docker, and locked-down installs.
- A contract test (`test/dispatch/test_schema_covers_parser.py`) enforces **schema ⊇ parser**: every field the JCWF parser reads must be declared somewhere in the schema (as a property, enum value, or `$defs` target). CI fails if a parser field isn't in the schema — keeps the schema authoritative for authors and the generator AI.

Office documents in `cntx_files` (PDF / DOCX / XLSX / PPTX / ODT) are converted to Markdown via `markitdown` synchronously during materialization (before registration with the pending-request pool so the file-activity watchdog doesn't tick while conversion is in flight). The conversion timing is logged at INFO (`markitdown START` / `markitdown END` with elapsed ms + bytes in / markdown bytes out) so operators can see exactly what's taking how long. No artificial timeout on the conversion — the per-attempt `CURLOPT_TIMEOUT_MS` budget (size-aware, see "Rate-limit + concurrency control" below) is the only wall once the dispatched HTTP request reaches the wire.

### Supported providers

| Adapter | Endpoint | Providers that work today |
|---|---|---|
| **API1** — OpenAI Chat Completions | `/v1/chat/completions` | OpenAI · Gemini (OpenAI-compat mode) · Groq · Together AI · Fireworks · DeepInfra · Perplexity · xAI Grok · Mistral Platform · GitHub Models · OpenRouter · self-hosted: Ollama · LM Studio · llama.cpp server · vLLM · text-generation-webui |
| **API2** — OpenAI Responses | `/v1/responses` | OpenAI (Responses API) |
| **API3** — Gemini native | `/v1beta/models/{model}:generateContent` | Google Gemini (native) |
| **API4** — Anthropic Messages | `/v1/messages` | Anthropic Claude (Haiku / Sonnet / Opus) |
| **API5** — AWS Bedrock | `/model/{modelId}/invoke` (SigV4-signed) | Bedrock Anthropic / Llama / Titan / Nova families. `RequestBuilderAPI5` dispatches body shape on `modelId` prefix; `ReplyParserAPI5` sniffs the response shape — success bodies delegate (Anthropic-on-Bedrock reuses `ReplyParserAPI4`), AWS error envelopes (`{"__type": "ServiceQuotaExceededException", "message": "..."}` etc.) take a no-delegate path that populates `m_ProviderErrorType` + classifies into `ProviderErrorCategory` directly. |
| **API6** — Azure OpenAI | `/openai/deployments/{deployment}/chat/completions?api-version={ver}` | Azure-hosted OpenAI deployments (`api-key:` header; body identical to API1) |
| **Test** — fixture-driven | in-process | no-network integration tests; `m_Url` points at a fixture file |

API keys are stored in an AES-256-GCM encrypted key store with a master password. Interfaces reference keys by `key_name`, resolved at runtime — no plaintext keys in workflow files or `config.json`.  Every credential value materialising in memory is auto-registered with `SecretRedactor` for log scrubbing — bearer API keys, basic-auth passwords, RSA private key PEMs, AWS `secret_access_key` + `session_token`, OAuth refresh / access / client tokens, and generated JWTs.  The `aws` credential type's `access_key_id` (in `m_ApiKey`) is the documented exception — public per AWS conventions, not registered.  All credential values are stripped from REST GET responses.  See `doc/cyber security.md` "SecretRedactor" for the full coverage table.

### Rate-limit + concurrency control

The dispatcher's controller layer keeps `j9t` close to each provider's RPM/TPM ceiling without provoking 429s. Three cooperating mechanisms, each with a clear job:

**Token-bucket mirror (correctness).** `IRateLimitStrategy` (`engine/curlWrapper/rateLimitStrategy.h`) is a per-`InterfaceType` strategy that parses provider-specific response headers into a normalized `RateLimitObservation` (remaining requests / input-tokens / output-tokens, reset-at times, retry-after). Concrete strategies: `RateLimitStrategyOpenAI` (API1 / API2 / API6 — `x-ratelimit-*` family with duration-string resets), `RateLimitStrategyAnthropic` (API4 — `anthropic-ratelimit-*` family with ISO 8601 resets, split input/output token quotas, `retry-after`), `RateLimitStrategyEmpty` (API3 Gemini, API5 Bedrock, Test — providers that ship no proactive feedback). The controller's `ShouldAdmit` projects the bucket forward by subtracting our in-flight from `RemainingRequests` and by `EstimateInputTokens(prompt)` from `RemainingTokens` — if the projection would go ≤ 0 before the next reset, dispatch is held until the reset.

**AIMD concurrency cap (max throughput).** Inside the bucket, the controller learns the sustainable concurrency by classic additive-increase / multiplicative-decrease — start at `strategy.InitialConcurrencyProbe()` (Anthropic 4, OpenAI 8, Empty 4), `cap += 1` after every 5 consecutive clean completions, `cap = max(1, cap/2)` on any 429. Validated against `jarvisCppDocu` (137-task Sonnet workload): cap converged 4 → 16 across the run with zero 429s, 138/138 succeeded in 5 min 43 s wall.

**Server-directed waits (etiquette).** When a response carries `Retry-After` or a provider-specific `*-reset` hint, the controller treats it as a floor on the next admission — `j9t` never re-dispatches sooner than the server told it to, even if the AIMD cap would otherwise allow.

Controllers are keyed by `(host, modelFamily)` via an opaque `QuotaKey` (`api.anthropic.com|claude-sonnet`, `api.openai.com|gpt-4o`, etc.) computed in `AiRequestPool::Submit` from the strategy's `DeriveQuotaKey(model)` — so Anthropic Sonnet and Anthropic Opus get independent AIMD signals despite sharing `api.anthropic.com`. Live state surfaces in `/api/debug/signals → dispatcher_controllers[]` with cap, streak, last observation, predicted next-admit time. The legacy `dispatcher_hosts[]` view stays as a per-host roll-up.

**Size-aware in-flight budget.** The per-attempt timeout is `CURLOPT_TIMEOUT_MS` set from a budget computed at submit time:

```
seconds = (input_tokens / 1000 × per_1k_input_token_seconds)
        + (max_output_tokens / 1000 × per_1k_output_token_seconds)
        + fixed_overhead_seconds
seconds *= safety_margin_factor
seconds  = clamp(seconds, min_seconds, max_seconds)
```

Curl's timeout only counts time on the wire — inbox waits, controller throttling, and retry-queue backoffs don't burn the budget. Each retry creates a fresh easy handle with a fresh budget. This replaces the pre-1.0 dual-timeout layer (`AiRequestPool::m_Deadline` + `WorkflowRuntimeManager::TimeoutWaitingExternalTasks`) that legitimately-slow Sonnet calls were tripping. All knobs are per-`ApiInterface` in `config.json` under `rate_limit.request_budget` — see the user manual `doc/jarvisagent.md` for the full schema and tier-tuning examples.

**Cascade cancellation.** When a workflow run terminates (failed / cancelled / stopped), `WorkflowRuntimeManager` calls `AiRequestPool::CancelRequestsForRun(runId)` once. The pool walks pending entries, finds matches by run id, and forwards each to `CurlMultiDispatcher::CancelByCancelKey(...)` — which (on the I/O thread, where transport mutations are safe) drops matching entries from the inbox and retry queue, fires the user callback synchronously for each in-flight `m_Active` entry, and asks the transport (`LiveTransport::CancelByCancelKey`) to silently abort the matching curl handles via `curl_multi_remove_handle` + `curl_easy_cleanup`. Cancelled callbacks fire with `Fail(CURLE_ABORTED_BY_CALLBACK, "request cancelled (run terminated)")`. Validated under load: 126 in-flight Anthropic requests aborted in <1 ms when an upstream task failed, halting token burn that would otherwise have continued for orphaned generations. Counter exposed as `dispatcher_total_cancelled` on `/api/debug/signals`.

**Liveness detection.** Network failures (internet off, DNS down, TLS fail, server 5xx) all surface as curl errors today with no extra work. The size-aware budget catches the "server accepted but silently stuck" case. Two small additions ship: `CURLOPT_TCP_KEEPALIVE = 1` so long-idle in-flight connections notice they're dead, and the `dispatcher_active_count` / `dispatcher_inbox_size` / `dispatcher_retry_queue_size` triple on `/api/debug/signals` answers "is the dispatcher healthy?" at a glance. SSE streaming (would let an idle-event watchdog distinguish "model thinking" from "TCP stuck") is post-1.0 — the controller's `Observe()` is idempotent by replacement so a future split into `ParseHeaders()` + `ParseBody()` is mechanical.

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

Engine validates the dashboard session cookie at the WebSocket upgrade handshake via Crow's `.onaccept` hook — no in-band auth message is used. Browser clients must have logged in via `POST /api/auth/login` before the upgrade; unauthenticated upgrades are rejected.

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

## Security Architecture

Studio has no browser-UI auth (developer workstation — localhost only); MCP / programmatic access in Studio uses the same MCP API key store as Engine. Engine additionally requires auth for the browser UI via a login cookie.

**Authentication — exactly three paths, no legacy fallback:**

- **MCP API keys** (`Authorization: Bearer mcp_...`) — per-user credentials stored only as SHA-256 hashes in `mcp_keys.json.enc` (AES-256-GCM + PBKDF2, same master password as `keys.json.enc`). Each key carries identity, role, adhoc flag, disk quota, and retention policy. Raw keys are shown exactly once at activation; comparison uses `CRYPTO_memcmp`.
- **Dashboard session cookie** — `POST /api/auth/login` validates an MCP key and returns an `HttpOnly + SameSite=Strict` cookie (plus `Secure` when TLS is enabled). Server-side session store, 8-hour sliding timeout, destroyed on restart. `POST /api/auth/logout` tears down both cookie and server state.
- **Gateway-trusted identity headers** — when `TrustedProxyHeader` / `TrustedRoleHeader` are configured in `config.json`, j9t trusts `X-Forwarded-User` / `X-Forwarded-Role` injected by an upstream API gateway. Role defaults to `viewer` if the header is absent.

**Provisioning** — admin creates a single-use enrollment token (`POST /api/auth/mcp-keys/enroll`, 30-min TTL), shares it out-of-band; user activates (`POST /api/auth/mcp-keys/activate`) and receives their real key once. Self-renewal (`POST /api/auth/mcp-keys/self-renew`) lets users refresh before the 90-day expiry without admin help; old key enters a 24-hour grace period. On j9t's first run with an empty store, a bootstrap admin enrollment token is logged to stderr.

**REST endpoints:** all non-public routes go through `Authenticate()`, which tries MCP key → session cookie → gateway header in that order. `CheckAdminAuth()` additionally requires `role >= admin`. Non-matching requests return 401 (missing) or 403 (forbidden / `insufficient_role` / `token_expired` / `key_disabled`).

**WebSocket:** upgrade-time validation via Crow's `.onaccept` hook — the same `Authenticate()` logic that guards REST is applied to the upgrade request, so the session cookie (or MCP bearer) must already be valid when the handshake happens. No in-band `type:"auth"` message.

**Webhooks:** HMAC-SHA256 via `X-Webhook-Signature` header.  Webhook secret is mandatory per workflow in **both** editions — `WorkflowTriggerBinder::ParseWebhookParams` fails closed on missing/empty/invalid secret at JCWF parse time; the TriggerEngine validator additionally refuses empty-secret registrations.

**Public endpoints:** `GET /api/status`, `GET /`, `/dash-assets/*`, `POST /api/auth/mcp-keys/activate` (enrollment token *is* the auth), `POST /api/auth/login` (MCP key *is* the auth).

**RBAC:** three roles — `admin` (full access incl. shutdown, security logs, MCP key CRUD), `operator` (run control, app logs, adhoc submission when `adhoc_enabled`), `viewer` (read-only monitoring). The role is carried on the MCP key itself, on the session cookie derived from it, or on `X-Forwarded-Role`.

**Defense layers:**

- Two-tier rate limiting (token bucket): pre-auth per-IP (100 req/min, burst 20) for unauthenticated/invalid traffic; authenticated per-user (1200 req/min, burst 200) once a credential validates. Returns 429 with `Retry-After`.
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

Module imports are gated against the script registry — workflow `params.module` must resolve via `ScriptRegistry::FindByModulePath` before `PyImport_ImportModule` is called.  Unregistered names (including system modules like `os`, `subprocess`, `ctypes`) are rejected with a structured error.  The script directory passed to the sub-interpreter's `sys.path` and every per-task `working_directory` are canonicalised against the project root via `fs::weakly_canonical` and rejected on `..` escape, absolute-path mismatch, or symlink-out-of-tree.  See `application/python/README.md` for the full contract.

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

## Key Design Decisions

Reference record for why things are the way they are. Reading current code is authoritative for behaviour; this table explains rationale a fresh contributor wouldn't recover from the code alone.

| Decision | Resolution | Rationale |
|----------|------------|-----------|
| **MCP key provisioning** | Always manual via single-use enrollment tokens; no auto-generation | Admins make deliberate per-user access decisions. Auto-generated keys would blur the audit trail back to "some bootstrap process". |
| **Unified credential model** | The same `mcp_`-prefixed key serves the dashboard login *and* MCP programmatic access | Agents act on behalf of a human — one issuance, one revocation, one audit trail. No service-account sprawl. |
| **Key storage** | SHA-256 hashes only in `mcp_keys.json.enc`, raw key shown to the user exactly once | Admin never sees the real key — containment if the admin account itself is compromised. Mirrors HashiCorp Vault / Auth0 / AWS IAM enrolment patterns. |
| **Master password handling** | Single master password unlocks both `keys.json.enc` and `mcp_keys.json.enc`; held exclusively in `mlock()`-protected `SecureString`; no `JARVIS_MASTER_PASSWORD` env var | Reduces admin burden (one password) while keeping the secret out of process listings, `docker inspect`, crash dumps, and core files. Unlock is deliberate per-restart so the key material cannot be re-accessed without a human present. |
| **MCP transport security** | TLS required for SSE; stdio assumed local-only and unencrypted | stdio cannot be intercepted across a network; SSE over HTTP would expose keys on the wire. |
| **Rate limiting scope** | Per-IP only for 1.0; per-key limits deferred | Simpler implementation and matches the primary DoS vector (automated probing from single sources). Per-key limits can layer on later without breaking the wire protocol. |
| **Adhoc JCWF size** | Soft cap of 1 GB; per-user disk quota is the real safeguard; `MaxRequestBodyMB` caps individual HTTP requests at 10 MB | JCWF canvases are typically tens of KB — the 10 MB request cap is generous. The 1 GB ceiling accommodates attached binary data if we ever expand that surface. Per-user disk quota is the only actual resource-protection mechanism. |
| **Adhoc run visibility** | Runs visible to all authenticated users for 1.0, with owner identity shown on each entry; per-user privacy deferred | Operator tier is already a trust boundary — hiding peer activity buys little and complicates debugging. Can tighten post-1.0 if a customer asks. |
| **Artifact plane (download/list/catalog) is read-only** | No API path mutates run folders after staging | Mutation routes are an attack surface we don't need. Write access is in the workflow runtime only. |
| **Symlinks never followed on artifact download** | `fs::symlink_status` check, then reject; never `fs::exists` alone | Closes a TOCTOU class where a malicious task could swap a regular file for a symlink pointing outside the run folder between listing and read. |
| **Per-user folder namespace for adhoc** | `_adhoc/<user_slug>/<run>/` | Authorisation is enforced twice — handler ownership check *and* filesystem path prefix. Defence in depth. |
| **Scripts cannot be submitted via API** | Adhoc submit rejects JCWFs whose `params.command` / `params.module` doesn't already exist under `scripts/` | Hard boundary against code injection. Scripts are the only arbitrary-code path in a workflow — gating them to admin-deployed content keeps adhoc submission a safe capability. |
| **Python imports allowlisted at runtime, not just at submission** | `PythonEngine::ExecuteWorkflowTaskOnWorker` calls `ScriptRegistry::FindByModulePath(moduleName)` before `PyImport_ImportModule`; unregistered names are rejected before any Python state is touched. | Defence-in-depth on top of the adhoc-submission gate above. The submission-time check stops the adhoc surface from accepting hostile JCWFs, but workflows registered via `PUT /api/workflows/<id>` (admin) or loaded from disk (Engine bootstrap) bypass that path. The runtime gate covers all entry points uniformly — system modules (`os`, `subprocess`, `ctypes`) cannot be imported regardless of how the workflow arrived. The script registry is populated by `ScriptRegistry::ScanDirectory(scripts/)` at startup and maintained by file-watch events, so the allowlist tracks the on-disk script directory automatically. |
| **Envelope-direct AI dispatch** | `ai_call` tasks (registered and adhoc) build an `AiInvocation` and call `AiRequestPool::Submit` in-process | Removes the file-event round-trip that used to mediate AI completion. Disk-first is preserved: STNG/CNTX/TASK/PROB/PROV are still written for replay/debug, but the envelope is authoritative. `TriggerEngine` owns its own `FileWatcher` instance for `file_watch` triggers on arbitrary declared paths. |
| **`file_outputs` on `ai_call` is allowed, with a portability note** | Validator emits Info for outside-tree destinations, Warning for destinations inside the task's own queue folder with requirement-firing filenames | Supports external-project agent workflows (Studio or Engine-without-Docker writing to `~/dev/<project>/...`) while still flagging the real bug (duplicate AI query when a file_output lands inside its own queue folder as e.g. `summary.txt`). |
| **Failure-path log discipline** | Every C++ fail-path emits a `LOG_*_ERROR` line whose text includes the runId or workflowId as a literal substring | The dashboard's "Run Analysis" panel scopes issues to lines containing this run's identifiers — so concurrent runs don't cross-contaminate. A fail-path log without an id (or at WARN level) is invisible to the analyzer and silently drops out of debugging. Subsystems without run context (parsers, signers) return errors via their data types rather than logging, leaving the upstream caller with the runId in scope to do the ERROR log. |
| **Hand-edited config persistence is find-replace + JSON-escape + simdjson tripwire + atomic rename** | The handlers that persist `config.json` (`HandleAiInterfacesSavePost`, `HandleConfigSettingsPut`) read the existing file as text, splice values in via JSON-escaped concatenation (interfaces) or a depth-aware top-level-key replace (scalars), validate the patched result re-parses cleanly under simdjson, then atomic-rename via `WebServerHelpers::WriteTextFileAtomic`. `connections.json` follows a different path — it is fully owned by the engine, so `HandleConnectionsSavePost` calls `CloudConnectionManager::SerializeToJson()` (parse-and-reserialize from in-memory state) and writes the result via the same `WriteTextFileAtomic`. | A full parse-and-reserialize would lose user comments, custom field ordering, and layout — `config.json` is a hand-edited document, not a generated artifact. The find-replace path preserves the file's shape; `JsonHelper::EscapeJsonString` closes the corruption window where a quoted/backslash/newline-bearing field would break next-restart parsing; the simdjson tripwire fails loudly if the splice produces malformed output; the atomic rename means a 5xx response leaves the on-disk file unchanged. `connections.json` is engine-generated (CRUD via REST), so reserialization from in-memory state is the simpler and correct choice. When extending to a new hand-edited file, route through the four-step find-replace pipeline; for an engine-owned file, `SerializeToJson + WriteTextFileAtomic` is the pattern. |
| **WebSocket broadcasts drain on client-driven pings, not a server-side timer** | Producers (workflow runtime, AI dispatcher, log macros) push into `m_PendingBroadcasts` from any thread; the Crow IO thread drains the queue inside `onmessage`, triggered by the dashboard's 500 ms heartbeat. Each drain folds the queue into one batched `send_text` per client. | Calling `DrainPendingBroadcasts()` from the main thread used to overlap with Crow's async `send_text` on the IO thread and cause silent message loss. Pinning the drain to the IO thread closes that race, but means the drain only fires when *someone* sends a frame. The dashboard's 500 ms ping is the heartbeat that keeps drains flowing. |
| **Per-tenant URL fields live on `ApiInterface`, not `ICredential`** | Azure OpenAI's `resource`/`deployment`/`api_version` and Bedrock's `region` are encoded into `iface.m_Url` (Bedrock: extracted at signing time from the URL host); `AwsCredential` carries `m_Region` as a typed field for SigV4 signing only. | One Azure subscription typically hosts multiple deployments (gpt-4 + gpt-35-turbo + ...) that share an api-key but differ in URL. Putting URL components on the credential would couple deployments. The credential is *who*, the interface is *where*. |
| **Credentials use a typed hierarchy, not a flat struct** | `engine/keys/credential.h` defines `ICredential` (abstract base) + 5 concrete subtypes (`ApiKeyCredential`, `OAuthCredential`, `KeyPairCredential`, `BasicAuthCredential`, `AwsCredential`).  Every secret-bearing field is a `SecureString` (mlock'd, zero-on-destruct).  Consumers `dynamic_cast` to the expected subtype with fail-closed null-check on type mismatch.  Per-provider non-secret extras live in the `m_Params` string map on the base. | A flat `std::variant<ApiKey, OAuth, ...>` was considered but rejected — `dynamic_cast` is cheap (vtable lookup) and allows connectors that legitimately accept multiple shapes (Jira Cloud BasicAuth vs DC PAT, S3's three conventions) to enumerate explicit allowlists.  Type safety prevents the "OAuth refresh-token field accidentally read as a Bearer api_key" class of bug that the legacy `KeyManager::ProviderConfig` flat struct allowed.  Adding a new subtype requires extending `credential.h`, the `CredentialFactory` JSON dispatch, and a virtual `RegisterSecrets()` override — visible compile-time discipline. |
| **SigV4 hand-rolled, no `aws-sdk-cpp`** | `engine/curlWrapper/awsSigV4.{h,cpp}` implements signing on OpenSSL primitives (HMAC-SHA256, SHA256). Debug-build startup self-test runs 4 sub-tests: SHA256-empty constant, AWS-published key-derivation hex, determinism, and a full-chain known-answer signature. Intermediate signing keys are RAII-cleansed via `OPENSSL_cleanse` (`ScopedSecretBytes`); HMAC/SHA256 NULL returns are checked and propagate failure up to `Apply` rather than silently producing gibberish. | `aws-sdk-cpp` is ~50 MB and pulls cmake into the build; our needs are request-signing-only. Hand-rolled is ~400 lines of well-documented crypto plumbing on OpenSSL primitives we already link, with the startup self-test as the AWS-conformance backstop. |
| **Bedrock per-family body/reply dispatch is private, not a public abstraction** | The `modelId`-prefix switch (anthropic.* / meta.llama* / amazon.titan-* / amazon.nova-*) is implemented as private free functions inside `requestBuilderAPI5.cpp` and `replyParserAPI5.cpp`, not as an exposed `IBedrockFamilyBuilder` interface | Only one provider (Bedrock) would ever use a family-builder abstraction, so exposing it publicly would be premature. Internal organization stays clean; the public surface stays minimal. |
| **Adaptive controller keyed by `(host, modelFamily)` via opaque `QueryData::m_QuotaKey`** | `AiRequestPool::Submit` calls `strategy.DeriveQuotaKey(model)` and stuffs `"<host>|<family>"` into the dispatcher's request envelope; the dispatcher uses the string as a map index without parsing it | Anthropic Sonnet and Anthropic Opus share `api.anthropic.com` but have independent provider quotas — same host, different AIMD signals. Opaque key keeps the dispatcher unaware of "model family" semantics; the strategy owns the format. |
| **Retry queue lives in-memory, never persisted** | `CurlMultiDispatcher::m_RetryQueue` is a plain `std::vector<RetryEntry>` cleared on shutdown | Restart-mid-batch is not a supported recovery scenario for j9t — workflows re-trigger from their last persisted DAG state, not from in-flight retry intent. Persistence would buy nothing and add a serialization surface. |
| **Input-token estimate = `chars / 4`, no per-provider tokenizer** | `IRateLimitStrategy::EstimateInputTokens` is a single `chars / 4` heuristic shared across all strategies; real `usage` from response bodies overrides via `Observe()` once the request completes | Content-driven variance (English vs CJK vs code) is the dominant term in any pre-tokenization estimate; bringing in tiktoken / sentencepiece per provider adds a ~30 MB vendor tree, build complexity, and per-provider divergence for sub-percent accuracy on the size-aware-budget formula. The token estimate self-corrects within one round-trip. |
| **Throughput-first dispatch; `max_concurrency` is the only cost-shaping lever** | The controller defaults to "fire as many parallel requests as the provider will accept"; cost containment lives in the per-interface `rate_limit.max_concurrency` config field (operator-tunable), nothing else. No per-key cost caps or priority lanes for 1.0 | Cost concerns are workload-specific (Sonnet at 1× concurrency vs gpt-4o-mini at 32×); making the controller cost-aware would couple it to billing models that vary across providers. `max_concurrency` is the universal escape hatch — bound the parallelism, the spend follows. Per-key budgets and priority lanes can layer on later without changing the controller's contract. |
| **Per-attempt timeout = `CURLOPT_TIMEOUT_MS` from a size-aware budget; runtime `WaitingExternal` timeout dropped for `ai_call`** | `AiRequestPool::Submit` computes `timeoutMs = ((in_tok/1000)*per_in + (out_tok/1000)*per_out + fixed) × safety, clamped[min,max]` from `api->m_RateLimit.m_RequestBudget` and writes it into `QueryData::m_TimeoutMs`. The dispatcher hands it to curl; `WorkflowRuntimeManager::TimeoutWaitingExternalTasks` no longer fires for `ai_call` tasks | Curl already counts only in-flight time (not queue waits) and resets per attempt because each retry creates a fresh easy handle. The pre-1.0 dual-timeout (per-attempt deadline in `AiRequestPool` + 300 s `WaitingExternal` kill in the runtime) used to murder legitimately-slow Sonnet calls when the runtime's clock fired first. Single owner, single clock, ~130 lines of deadline-bookkeeping deleted. |
| **Workflow runtime `Update()` holds `m_Mutex` for the entire tick body; external calls deferred** | `WorkflowRuntimeManager::Update()` acquires `m_Mutex` once around the fingerprint / drain / propagate / tick / finalize / erase block.  External calls (`AiRequestPool::CancelRequestsForRun`, `FireCompletionCallback`, `RunTerminalObserver`) are collected into a `postTickActions` vector inside the lock, executed AFTER release.  `m_RunTerminalObserver` is copied under the lock before invocation. | Pre-fix the tick mutated `m_ActiveRuns` task states without holding the lock while REST snapshot readers (`GetActiveRunsSnapshot`, `TryGetActiveRun`) held it — torn reads of `WorkflowRun` were possible.  Holding the lock across the tick eliminates the race; deferring external calls keeps callbacks / cancel cascades off the lock hot path and removes any lock-order risk if a callee ever touches this manager.  Worker-thread lambdas in `TickActiveRun` now capture `WorkflowDefinition` by value (not by reference), so the captured copy outlives any `WaitStop`-driven `m_ActiveRuns` clear. |
| **Completion-callback SSRF is gated by an https-only allowlist + DNS-resolution check + TLS hardening** | `FireCompletionCallback` runs `IsCallbackUrlAllowed(url, &reason)` before any payload is built: requires `https://` scheme, parses host (with userinfo / IPv6-bracket / port handling), `getaddrinfo`-resolves it, and refuses if any returned address is in loopback / RFC 1918 / CGNAT / link-local / unique-local / multicast / unspecified ranges (with IPv4-mapped-IPv6 unwrap).  TLS knobs: `CURLOPT_SSL_VERIFYPEER`, `CURLOPT_SSL_VERIFYHOST`, `CURLOPT_FOLLOWLOCATION 0`, `CURLOPT_PROTOCOLS_STR/REDIR_PROTOCOLS_STR = "https"`. | The `callbackUrl` is workflow-context, partially attacker-influenced (a JCWF or webhook trigger payload can seed context).  Without the gate, a malicious workflow could exfiltrate task outputs to internal endpoints (`169.254.169.254` cloud metadata, intra-VPC DBs, the local control plane).  Cloud-connector SSRF is gated by a separate `ConnectorHttp` pair — they share the same allowlist philosophy but live in different files because the call patterns differ (connectors take a configured endpoint, the callback takes a per-run context value). |
| **`PythonEnginePool` synchronization: atomic `m_Running` + mutex on `m_Engines`; lock-free reads after init** | The pool's class-level threading contract: `m_Running` is `std::atomic<bool>` (acquire/release ordering); `m_Mutex` guards `m_Engines` mutation in `Initialize` (push_back loop) and `WaitStop` (clear).  After `Initialize` returns true and before `SignalStop` is called, `m_Engines` is stable and lock-free readable from worker threads.  `SignalStop` flips `m_Running` to false BEFORE taking the lock to signal each engine. | The pool's hot path is `ExecuteWorkflowTask` → `SelectEngine` (load-balance over engines).  Holding the mutex on every dispatch would serialize all Python-task submission across the workflow runtime's thread pool — eliminating the parallelism the multi-engine architecture exists to provide.  The atomic+mutex split matches the pool's lifecycle: heavy-but-rare init / shutdown under the lock; frequent steady-state reads lock-free with explicit ordering.  Capture-by-value and threadpool-reuse rules apply at the pool's worker-thread boundary. |
| **`AiRequestPool` inflight counter uses per-submission atomic-flag for exactly-once decrement** | `m_DirectDispatchInflight.fetch_add(1)` at submit time; a `std::shared_ptr<std::atomic_flag> decrementOnce` is captured into the `curlCallback`.  Both decrement sites (schema-retry decrement-then-recurse + end-of-callback) gate via `if (!decrementOnce->test_and_set()) fetch_sub(1)`; first decrement wins, subsequent attempts no-op cleanly. | Pre-fix used `if (load > 0) --` at both sites — that's a check-then-act race: two concurrent decrements observing `count == 1` could both proceed and underflow the unsigned `size_t` to `SIZE_MAX`.  The race is visible to operators via the dashboard's "queries in flight" LED.  The flag pattern is robust against synchronous callbacks (mock dispatchers / dispatcher-error paths returning inline) AND the schema-retry recursion: the per-Submit flag means the recursive call gets its own fresh flag and the outer end-of-callback decrement is suppressed. |
| **`TriggerEngine` email-poll uses identity-based, not index-based, lookup across the lock window** | `Tick()` collects `EmailPollJob`s with `(workflowId, triggerId)` identity (not vector index).  IMAP I/O happens with `m_Mutex` released; the watermark-update path re-acquires the mutex and looks up by identity, dropping the update if the trigger was removed during the network call. | Pre-fix carried `size_t m_Index` and bounded-checked it on re-acquire.  The OOB check was correct but identity-blind — between lock release and re-acquire a concurrent `ClearWorkflowTriggers` + `AddEmailWatchTrigger` could shift the vector and silently misroute the watermark to an unrelated trigger.  Identity-based lookup is fail-safe: if the trigger is gone the update is dropped (with INFO log). |
| **`WorkflowRegistry` single-mutex discipline; private helpers assume lock held** | `mutable std::mutex m_Mutex` guards every public method that touches `m_Workflows` / `m_BrokenWorkflows`.  Private `LoadContainer` / `LoadContainerSubWorkflows` are called from the public `LoadDirectory` under its lock.  `GetSubWorkflowDependencyGraph` iterates the map and resolves child workflow IDs by file path — to avoid recursive-lock deadlock, the helper splits into a public `TryGetWorkflowIdByFilePath` (acquires) and a private `TryGetWorkflowIdByFilePathLocked` (lock-held variant called from inside `GetSubWorkflowDependencyGraph`).  `SaveOrUpdateWorkflowFromJson` wraps the multi-step file/registry work in try/catch with the `m_Workflows` insert as the LAST step. | Pre-fix the registry was unsynchronized.  REST handlers reading workflow state (`GET /api/workflows`, status snapshots), the workflow runtime calling `GetWorkflow` during dispatch, `AdhocWorkflowManager::Stage` calling `SaveOrUpdate`, and the file-watcher reload path could all interleave in the same map.  Single-mutex with discipline-documented private helpers is simpler than reader-writer locks and adequate for the load (registry mutations are rare, reads frequent but cheap).  The "insert is the last step inside the try" pattern means a thrown filesystem exception during `JcwfContainer::Pack` cannot leave the registry holding a half-built definition. |
| **`AdhocWorkflowManager` reaper uses condition_variable; lifecycle mutex serializes Start/Stop** | `m_ReaperCv` lets `StopReaperThread` wake the reaper out of its `wait_for(60s)` immediately instead of waiting for the next 1 s sleep slice.  `m_ReaperLifecycleMutex` serializes Start/Stop — atomic `m_ReaperRunning` alone doesn't make the std::thread member assignment safe under concurrent calls. | Pre-fix used a 1 s busy-poll loop checking `m_ReaperRunning` — Stop could block up to a second.  The 1 s isn't a correctness bug but it's the wrong pattern.  Pre-fix Start/Stop also raced on the std::thread assignment: Start could overwrite a thread handle while a concurrent Stop was joining it.  CV pattern + lifecycle mutex matches the rest of the codebase's reaper / worker-thread idioms. |
| **Shell-task `args[]` are always single-quoted before joining into the `sh -c` string** | `ShellTaskExecutor::JoinArgumentsForSystem` wraps **every** arg in single quotes (with embedded-quote escape via `'\''`) regardless of whether the arg contains whitespace.  `IsSafeArgument` extends the blocklist to `; & \| > < ' " \` $ ( ) \\` as defense in depth. | Pre-fix only single-quoted args containing whitespace; non-whitespace args went unquoted.  Combined with the original `IsSafeArgument` not rejecting `$ ( ) \`, an arg of `$(rm -rf /)` would pass safety, get emitted unquoted, and execute as command substitution under the shell.  Always-quote eliminates the entire injection class — globbing, variable expansion, and command substitution all neutralised regardless of arg content.  Workflow-author globbing expectation lives in the `command` field (concatenated raw); `args[]` is for positional arguments and SHOULD be literal.  This is the load-bearing fix — the blocklist tightening is the secondary belt. |
| **Workflow JSON parser enforces element-count and field-value caps at parse time** | `WorkflowParserLimits` (in `application/workflow/workflowJsonParserDetails.h`) caps tasks at 1000, triggers at 100, dataflows at 10000, filters at 100, file-inputs/outputs/queue-files per task at 1000, inline-content at 1 MB, retry attempts at 100, backoff at 1 h, timeout at 7 days.  `IsAcceptedRelativePath` rejects empty / overlength / absolute values in `base_directory`, `working_directory`, `file_inputs`, `file_outputs`, and queue-binding `path` fields.  `..` segments are deliberately **allowed** at parse time because the shipped JCWF convention uses `working_directory: "../../queue/<workflow>/<task>"` to navigate from `workflows/<id>/` up to the project-root-anchored queue tree — the consumer-side `ConfineUnderProjectRoot` gate is the canonical-form filter that catches the actual `..`-traversal escape.  Negative `timeout_ms` / `max_attempts` / `backoff_ms` are rejected before the cast that would wrap them to near-INFINITY. | A JCWF is operator-trusted in registered form, but the **adhoc** submission surface accepts JCWFs from any authenticated user — and the editor PUT path accepts canvas content from the editor.  Without parse-time caps, a malformed or hostile JCWF could exhaust heap (millions of dataflow edges) or set near-infinite timeouts that disable runtime kill switches.  Parse-time gates fail closed early on the size-and-shape filter; the consumer-side gate is the deeper canonical-form band of the same containment.  Both fail closed. |
| **`ScriptCatalog::Refresh` builds offline, swaps under lock; symlinks skipped + canonical containment** | The new entry vector is materialised entirely outside `m_Mutex`; the lock is acquired only for the final swap so concurrent `List` / `GetByPath` callers aren't blocked for the full I/O duration.  Symlinks are skipped at iterator level (`is_symlink()` check before `is_regular_file()`), and every entry's path is `weakly_canonical`-confined under the canonical scripts root before being parsed. | Pre-fix held the lock for the whole scan (which calls `ParseFile` and reads up to 60 lines per script — easily hundreds of ms for a populated scripts directory).  Pre-fix also followed symlinks into files: a malicious symlink under `scripts/` pointing to `/etc/shadow` would have made it into the catalog with an `m_Path` value the operator would not have noticed, and downstream code that trusts the catalog would have read attacker-targeted files. |
| **`AdhocWorkflowManager::RewriteWorkflowId` uses simdjson DOM rewrite, not regex** | `RewriteWorkflowId` parses the JCWF JSON with `simdjson::dom::parser`, looks up the top-level `id` field, and rebuilds the document by emitting `"id": "<newId>"` first followed by every other top-level field re-serialised via simdjson's element-to-stream operator.  On parse failure or non-object root, the input is passed through unchanged with an ERROR log. | Pre-fix used `std::regex_replace` with `R"(...)"` to replace the first `"id":"..."` substring.  Regex is structurally blind: a stray `"id":"..."` substring in a `description` field, a doc comment, or attacker-shaped lookalike text would be matched first, leaving the actual workflow id untouched.  simdjson DOM rewrite is structurally aware and operates on the actual top-level field. |
| **AI-call task confines queue-binding source paths under the project root** | `aiCallTaskExecutor.cpp::MaterializeCntxFilesFromQueueBinding` and `MaterializeProbFilesFromQueueBinding` route every resolved CNTX/PROB source path through `ConfineUnderProjectRoot` BEFORE opening the file for read.  Project-root containment (rather than task-folder containment) is deliberate — cross-task data flow inside the project (e.g. an `ai_call` reading another task's output via `../../../workflows/<id>/<other_task>/output.json`) is the documented JCWF pattern, so the gate must be project-wide.  `ReadTextFile` enforces a 100 MB size cap (defends against `/dev/zero`, fifos, runaway logs).  `ConvertWithMarkitdown` uses an **allowlist** (`[A-Za-z0-9/._\-+=:]`) on the path before constructing the `popen` shell command — replaces a too-narrow blocklist that missed `;`, `\|`, `&`, `(`, `)`, `<`, `>`, etc.  Provider-sidecar JSON routes every string through `JsonHelper::EscapeJsonString`; `temperature` is parsed as a finite double in `[0,2]` before emission.  `BuildDefaultsMap` logs at WARN on simdjson exceptions (was silent). | Pre-fix the materialise functions trusted the queue-binding `m_Path` after `TaskPathResolver::ResolvePath` (a pure pass-through resolver per `taskPathResolver.h`).  An absolute path like `/etc/shadow` or a `..`-bearing relative path that escaped the project tree would have been read and embedded into the AI request — a CRITICAL-class arbitrary-file-read.  The markitdown blocklist was a textbook insufficient defense; migration to argv-based execution (`posix_spawn` / `fork+execvp`, removing the shell entirely) is tracked as future work, allowlist is the interim fix.  Provider-sidecar JSON injection via `temperature` (extracted as a raw string from `m_ParamsJson` and emitted as a JSON number literal) is closed by numeric parse + range validation. |
| **Hand-built file writes route through one atomic-write helper** | `EngineCore::AtomicWriteFile(path, content, errorMessage)` in `engine/auxiliary/file.h` is the single legitimate atomic-write path used by every hand-built file writer in `application/`.  Behaviour: `create_directories(parent)` → open `<path>.tmp.<atomic-counter>` with `binary | trunc` → enable `out.exceptions(failbit | badbit)` → write inside try/catch → close via scope → `fs::rename` to final.  On any failure the temp is best-effort cleaned up and a rich `errorMessage` is returned to the caller; the helper itself does NOT log (callers with run / workflow context emit `LOG_APP_ERROR` so dashboard run analysis surfaces the failure).  Use sites span the AI dispatch queue + output, filter manifests, workflow registry persistence, adhoc meta/manifest, cloud-task downstream-consumed JSONs, assistant data stores, MCP keystore Save, and REST script writes.  See `application/file/README.md` "Atomic-rename writes" for the categorised use-site list and the documented opt-outs (streaming writers with running caps, append-mode logs, operator-diagnostic stdout/stderr dumps). | Pre-fix the codebase had five independent atomic-write implementations (`aiTranscript::WriteFile`, `aiRequestPool::WriteTextFile`, `aiCallTaskExecutor::WriteTextFile`, `filterManifest::WriteManifest`, plus several copies inside `assistantTools.cpp`) plus a dozen-plus naive open→write→close sites.  Two failure modes: (1) SIGKILL or disk-full mid-write on the naive sites would leave a truncated partial at the canonical path, surfacing downstream as malformed completion signals (`OnOutputFileCreated` parses the partial), broken keystores (`mcpKeyManager` blob), or `5xx`-on-list (`adhocWorkflowManager` manifest); (2) the five parallel implementations were a discipline-rule trigger (the "extract a helper before a third copy appears" rule was already violated) and added drift risk every time the pattern was tweaked.  Single helper closes both.  Atomic rename gives every reader either the previous-version contents or the new-version contents, never a torn write.  The atomic counter (rather than `getpid()`) keeps temp names unique across concurrent writers without pulling platform-specific PID headers. |
| **EventQueue fails fast on a wedged main loop** | `engine/event/eventQueue.h::kMaxUnprocessedEvents = 1000` caps the unprocessed-queue depth.  Every `Push` carries a `ProducerId` tag (`SignalHandler`, `KeyboardInput`, `JarvisAgent`, `AiRequestPool`, `FileWatcher`, `PythonEngine`, `WebServer`) — the queue maintains a per-producer counter of pushes-since-last-drain that `PopAll` resets to zero.  On cap hit, `Push` snapshots the counter under the lock, releases the lock, emits `LOG_CORE_ERROR` with depth + per-producer breakdown + a deliberate-trade message ("bypasses keystore re-seal and audit-log flush that POST /api/shutdown provides"), synchronously flushes both CORE and APP spdlog sinks, then `std::exit(EXIT_FAILURE)` from the producer thread.  The lock-release-before-exit pattern matters: `std::exit` does not unwind the stack, so holding a `lock_guard` through it would leak the mutex.  See `engine/event/event_system.md` for the full mechanism. | Pre-fix the queue was unbounded.  A wedged main loop (long-running embedded Python, synchronous curl slip, deadlocked subsystem) lets producers push indefinitely → OOM.  Not biting today (producers naturally rate-limited under healthy load) but latent.  Lossy buffering as the alternative would mask the underlying bug — the cap-hit ERROR with per-producer breakdown reveals which subsystems were busy while the consumer was stuck, narrowing the diagnosis.  The 1000 threshold is well above any realistic per-tick burst (file-watcher storm, dispatch completion wave on shutdown), so hitting it signals a genuine wedge, not a brief spike. |
| **Edition-specific WebServer behaviour routes through paired `_studio.cpp` / `_engine.cpp` files, not `#ifdef` blocks in shared code** | `WebServer` methods that need different bodies per edition (today: `InitEditionSpecific`, `HandleAssistantWebSocketMessage`) are declared in `webServer.h` with no `#ifdef` guard.  The Studio body lives in `application/web/webServer_studio.cpp`; the Engine body (typically a no-op / `return false`) lives in `application/web/webServer_engine.cpp`.  `premake5.lua` `removefiles` is symmetric: the Engine branch drops `webServer_studio.cpp`, the Studio branch drops `webServer_engine.cpp` — each binary sees exactly one definition.  Both files wrap their bodies in `#ifdef J9T_STUDIO` / `#ifndef J9T_STUDIO` as a defence-in-depth backstop in case the premake gating is ever bypassed.  The shared `webServer.cpp` calls the methods unconditionally (`InitEditionSpecific()` from the constructor, `if (HandleAssistantWebSocketMessage(...))` from the `/ws onmessage` lambda); the linker resolves to the right edition's symbol.  AI connectivity probing (`AiRequestPool::TestInterface`) follows a different pattern — moved out of `AiJcwfService` (Studio-only) into `AiRequestPool` (both editions) so the `/api/settings/ai-interfaces/test` handler can call it unconditionally without needing an edition hook at all. | Pre-1.0 the shared `webServer.cpp` accumulated 8+ `#ifdef J9T_STUDIO` blocks — each one a place where shared code referenced a Studio-only field or called a Studio-only method.  Each ifdef is small but the pattern is cancerous: it spreads with every new Studio-only feature, the diff for adding an assistant route grows by an `#ifdef … #else … #endif` wrapper at every call site, and the cost to add an Engine counterpart later doubles because every existing call site has to be revisited.  File-level isolation is the structural fix: shared code is the same in both editions; edition-specific behaviour is one symbol with two definitions, exactly one of which links.  Same model the codebase already uses for `webServer_studio.cpp` as a whole (Studio-only routes) — this pattern just makes the Engine counterpart explicit so the shared-source ifdef count can drop.  `AiRequestPool::TestInterface` is the alternate path when the behaviour doesn't actually need to differ per edition — just promote the implementation into a both-editions class. |
| **Dashboard WebSocket reconnect uses exponential backoff + `forceReconnect()` on auth-state transition** | `dashboard/ui/src/hooks/useWebSocket.ts` reconnect timer starts at 2 s, doubles on each consecutive failed connect (capped at 30 s), and resets to base on successful `onopen`.  The hook also exposes `forceReconnect()` which cancels the pending backoff timer, resets the delay to base, and dispatches an immediate connect — called from `App.tsx::handleAuthenticated` after `setNeedsAuth(false)`.  Editor mirror lives in `workflow-editor/ui/src/hooks/useStatusWebSocket.ts` for parity (fixed 2 s reconnect, same `forceReconnect()` shape).  The backend `Authenticate()` rate-limit + lockout flow stays unchanged — the fix is purely client-side. | Pre-fix used a fixed 2 s reconnect.  When a dashboard tab opened before the user logged in (no session cookie), every 2 s it would generate one `[security] auth_failure reason=missing_credential` + one `[security] ws_upgrade_rejected` line in the security log.  A long-open-tab scenario produced thousands of warning lines per hour, drowning the genuine security signal.  Exponential backoff settles at ~30 s between attempts after ~5 failures.  The follow-on bug: the backoff grew during the master-password + login flow, leaving the "Connected" LED red for up to 30 s after a successful login.  `forceReconnect()` closes this — login completes, backoff resets, the LED flips green within a frame.  Editor's fixed-2 s hook had the milder version of the same bug; same fix. |
| **New error-returning APIs use `std::expected<T, SubsystemError>`** | `[[nodiscard]] std::expected<T, SubsystemError>` replaces the legacy `bool DoX(..., std::string& errorMessage)` shape across the codebase.  Subsystem-scoped error enums (`ConnectorError` in `application/cloud/connectorError.h` for cloud-connector entry points, `ParserError` in `application/workflow/parserError.h` for workflow JSON parsers, `RegistryError` in `application/workflow/registryError.h` for registry mutators) carry a `Code` enum (no `default:` arm — `-Wswitch` is the enforcement) plus a `std::string m_Details` for the human-readable message + a `Make()` factory + a `Describe()` switch helper.  Caller pattern: `if (auto r = DoX(...); !r) { LOG_APP_ERROR("... run='{}' code={}: {}", runId, Describe(r.error().m_Code), r.error().m_Details); }`.  Sweep complete across all three subsystems: `WorkflowRegistry::RemoveWorkflow` → `RegistryError` with `NotFound` / `PathRefused` / `IoError` codes (Sitting 7a, 2026-05-19); `ICloudConnector::TestConnection` base virtual + 13 concrete overrides + `ConnectorHttp::ValidatePublicHttpEndpoint` + `PostgresConnector::ValidatePostgresParams` → `ConnectorError` with `InvalidConfig` / `InvalidEndpoint` / `CredentialMissing` / `CredentialInvalid` / `NetworkError` / `AuthFailure` / `HttpError` codes (Sitting 7b, 2026-05-20); 3 public + 17 chain + 2 `Require*` + 1 free-standing parser method (23 total) → `ParserError` with `TypeMismatch` / `MissingField` / `ValueOutOfRange` / `SimdjsonError` codes (Sitting 7c, 2026-05-20).  Cloud connector failures now record typed `ConnectorErrorCode` on `CloudCircuitBreaker::RecordFailure` and surface as both `/api/connections/<name>/test` JSON `code` field AND `/api/status::connection_health[].last_failure_code`.  Project bumped to `cppdialect "C++23"` for native `std::expected`; clang ≤18 local dev opts in via `--clang` (libc++); Rocky 9 RPM CI uses `gcc-toolset-13`.  See `DEVELOPMENT.md` "C++23 toolchain notes" for the build-matrix details. | Pre-Sitting-7a the codebase had 22+ `bool + std::string&` methods across cloud connectors (13 `TestConnection` overrides + `ValidatePublicHttpEndpoint` + `ValidatePostgresParams`), workflow parsers (~15 chained `ParseTask` family), and the registry (`RemoveWorkflow`).  Three failure modes: (1) caller-side `if (!ok)` is easy to forget — compiler can't enforce, so a missed check silently treats failure as success; (2) error string is fundamentally for human display, so any programmatic dispatch on `errorMessage.find("...")` is anti-pattern; (3) the contract is informal — "errorMessage is populated on failure" — but compilers don't check it.  `std::expected<T, XxxError>` closes all three: `[[nodiscard]]` forces the caller to handle the result, the typed `m_Code` is the legitimate programmatic-dispatch path, and the type system enforces "value XOR error" semantics.  Subsystem scoping (3 enums) over a single mega-enum keeps each `switch` exhaustively small and per-bug a -Wswitch will narrow the diagnosis to the right subsystem. |
| **Typed-credential snapshot threads through `QueryData` for multi-secret signers** | `QueryData::m_AwsCredential` is a `std::shared_ptr<AwsCredential const>` populated at `AiRequestPool::Submit` time when `AuthStyle == AwsSigV4`.  The snapshot is a deep copy of the resolved KeyManager entry taken **inside** the `WithCredential` callback (so the lock-scoped pointer never escapes), with `SecureString` fields copied via `dst.Set(src.Get())` since they're non-copyable by design.  `SigV4Signer::Apply()` reads `m_SecretAccessKey` / `m_SessionToken` / `m_Region` / `m_AccessKeyId` from the typed snapshot — no more stringy reinjection into `QueryData::m_Params`.  The `service` field (non-secret request-target attribute) stays in `m_Params` with a "bedrock" default.  `MockTransport` invokes the signer when both `AuthStyle == AwsSigV4` AND `m_AwsCredential != nullptr`, capturing the resulting Authorization header into a process-global ring buffer (`MockSignatureCapture`, cap 32, exposed via `/api/debug/signals::last_mock_signatures` in debug builds).  See `engine/keys.md` "Snapshot pattern for deferred dispatch" for the canonical pattern. | Pre-Sitting-6 the credential path was credential → stringy `m_Params` map → typed-back-out at signing time.  Three failure modes: (1) plaintext secret materialised into a heap `std::string` for the dispatch duration, defeating the `SecureString` mlock + zero-on-destruct invariant — heap residue any compromised process can exfil; (2) parallel-construction discipline-rule trigger (`ResolveProviderParams` reinjected `secret_access_key` / `session_token` / `region` while `SigV4Signer::Apply` re-read the same three keys from `m_Params`, a contract that would skew the moment any field shape changed); (3) zero integration-level KAT coverage — `awsSigV4.cpp::RunSelfTest #4` locks the signing-key derivation at startup but doesn't exercise the wire-up from credential resolution through QueryData to the signer.  Typed snapshot closes (1) — secret material stays in `SecureString` until the HMAC compute step needs raw bytes, materialised briefly for the signing call only.  Closes (2) — one source of truth for the credential shape.  Closes (3) — `test/dispatch/test_bedrock_sigv4.py` runs the full pipeline against a fixture-driven mock with a locked Authorization signature; any regression in canonical-request assembly, URL resolution, or HMAC chaining breaks the KAT.  The `shared_ptr` indirection (vs. a value copy) keeps the credential refcount-controlled for paths that queue the request past `WithCredential`'s scope — the dispatcher's inbox + retry queue can hold the request for minutes. |

---

## Related Documentation

- [README.md](../README.md) — high-level overview
- [INSTALL.md](../INSTALL.md) — pre-built package installation
- [DEVELOPMENT.md](../DEVELOPMENT.md) — build from source
- [JC_Workflow_Specification.md](JC_Workflow_Specification.md) — JCWF format
- [cloud-integration.md](cloud-integration.md) — cloud connector framework
- [api-endpoints.md](api-endpoints.md) — REST API reference
- [cyber security.md](cyber%20security.md) — security model
