# j9t Roadmap

## 1.0 Release — Feature Freeze Scope ✅ (landed 2026-04-18)

### Adhoc Workflow Submission ✅

Turn j9t into a general-purpose orchestration backend by allowing external callers to submit workflows dynamically via the REST API.

- ✅ `POST /api/workflows/run-adhoc` accepts a JCWF payload
- ✅ Stages the workflow in a per-user directory (`_adhoc/<user_slug>/<run>/`), executes it, returns results
- ✅ Enables autonomous AI agents (Claude Code, custom agents, any HTTP client) to compose and submit workflows on the fly
- ✅ Cleanup policies (on_completion / ttl_1h / ttl_24h / ttl_48h / ttl_72h / retain) with admin-configured per-user ceiling
- ✅ Pre-stage script-existence check — caller-supplied scripts are refused; only admin-deployed scripts under `scripts/` are referenceable
- ✅ Artifact plane: list + download endpoints + MCP tools so agents can retrieve run outputs without filesystem access
- ✅ Script catalog: `GET /api/scripts` and `list_scripts` MCP tool so agents can discover what's pre-deployed

### MCP Configure-Plane Tools ✅

The current MCP interface covers the "run" plane (list/run/status/cancel workflows). Add tools for the "configure" plane so MCP clients are fully self-sufficient without falling back to curl:

- ✅ `manage_connections` — CRUD + test connections
- ✅ `manage_keys` — create/update credentials
- ✅ `upload_workflow` — submit a JCWF (Studio-only registration; adhoc submission handles transient JCWFs)
- ✅ `get_run_logs` — tail task output without reading local files
- ✅ `whoami` — identity, role, quota for the current MCP key
- ✅ `list_run_files` + `get_run_file` — artifact retrieval plane
- ✅ `list_scripts` — script catalog discovery
- ✅ `run_adhoc_workflow` — submit transient JCWFs
- ✅ `validate_workflow` — JCWF validation (Studio-only)

All tools proxy to REST endpoints under per-user MCP API key authentication; security is unified across both editions. See `Adhoc Workflow Submission and MCP plan.md` for the full design record.

---

## Post-1.0 — Future Directions

### AI Assistant Generator

An AI assistant framework where the agent's "tools" are JCWFs. The assistant has:

- A predefined set of tool-workflows it can invoke
- Access to project specs and documentation it can read and understand
- The ability to compose new workflows on the fly (via adhoc submission)
- Project folder awareness for context-driven orchestration

j9t becomes the execution layer for an autonomous agent that can reason about goals and orchestrate multi-step workflows to accomplish them. Builds on the JCWF format, AI generation pipeline, and MCP interface already in place.

### Bounded Loops in JCWFs (Evaluate)

Investigate bounded iteration support for use cases like iterative AI refinement ("generate, review, revise" cycles). Key constraints:

- Must preserve termination guarantees (mandatory `max_iterations` + explicit break condition)
- Progress tracking and visualization must remain clear
- Most polling patterns are already covered by the trigger system (email_watch, s3_watch, etc.)
- Retry-until-success is already handled at the executor level

Evaluate real demand before committing — the DAG model's simplicity is a feature, not a limitation.

### AI Streaming (SSE)

Today the dispatcher buffers each AI response fully and then parses. Switching to Server-Sent Events streaming buys two real wins:

- **Idle-event watchdog** distinguishes "model thinking" from "TCP stuck" — replaces the size-aware `CURLOPT_TIMEOUT_MS` budget with "no event in N seconds = stuck" semantics. More accurate for slow generations.
- **Live progress in dashboard** — token-by-token rendering of in-flight ai_call output instead of all-or-nothing on completion.

Scoped post-1.0: per-provider SSE parser, per-event state machine in each `ReplyParser`, transcript-per-event vs transcript-per-reply decision, schema validator on assembled stream rather than partial chunks, retry semantics ("got `message_start` then stream broke — retry from scratch or accept partial?"). 2-3 day refactor.

Design hook already in place: `RateLimitController::Observe()` is idempotent by replacement so a future split into `ParseHeaders()` + `ParseBody()` (streaming requires this) is mechanical without changing controller state semantics. See `AI call performance optimization.md` §10 decision 4.
