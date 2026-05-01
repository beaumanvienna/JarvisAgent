# JarvisAgent TODO

Open items consolidated 2026-04-28.  Closed-items history archived in `doc/misc/`:
- `doc/misc/JarvisAgent TODO List.md` — global archive
- `doc/misc/application-workflow-todo.md` — backend workflow archive
- `doc/misc/workflow-editor-todo.md` — frontend archive

The two scope-specific live files (`application/workflow/doc/todo.md`, `workflow-editor/todo.md`) currently hold only headers — no open items.

**See also:**
- `doc/misc/hand-off.md` — session hand-off log; read latest entry first when picking up.
- `doc/misc/cybersec-hardening-dev-plan.md` — §18 plan (4-domain split, 4 sessions combined with §19).
- `doc/misc/cpp-safety-hardening-dev-plan.md` — §19 plan (Rust-emulating C++ defaults).
- `doc/misc/AI call performance optimization.md` — §5g design reference (refactor complete).

---

## Pre-1.0

### Concurrent-run policy for JCWFs (serialize by default)
Surfaced 2026-04-29 during sitting-4 verification.  When the `--with-ai --auto-approve` test suite exercised `run_workflow`, the AI picked `jarvisCppDocu` (141 tasks) and started it multiple times — the runs raced on the shared queue folder, each run's `OnOutputFileCreated` callback consumed events the other was waiting for, and 82 tasks ended up stranded in `waiting_external` with output files already on disk that no one would transition.  Today the engine has no concurrency guard: there's no single-instance lock, no serialization, nothing.  The dashboard greys out the Run button while a run is active, which **looks** like a guard but only blocks the UI path — REST/MCP/test callers bypass it entirely.  That UX-vs-engine gap is itself a finding to fix.

Three policies, picked in `global.json`:
- `parallel` — current behaviour; both runs proceed concurrently.  Right for genuinely fan-out-by-design workflows but they need per-run queue folders to be safe (separate work).
- `serialize` — second run accepted, sits in `pending` until the first completes; FIFO drain.  Right for "I clicked Run twice / cron stacked / test fired Run twice" — preserves runId observability without overlap.
- `reject` — second run returns 409 Conflict; caller decides retry/fail/log.  Right for IO-exclusive jobs.

**Default policy:** `serialize` — safer surprise.  Workflows that legitimately want parallel execution opt in via `"concurrency": "parallel"` in `global.json` and pair it with the per-run queue folder work below.

Implementation lives in `WorkflowRuntimeManager`:
- New `m_PendingByWorkflow: unordered_map<workflowId, deque<RunRequest>>`, mutex-guarded.
- `EnqueueWorkflowRunAndGetRunId` checks the JCWF's policy.  If `serialize` and there's already an active run for `workflowId`, push to the deque and return the new runId in `pending` state.
- On run completion (succeeded / failed / cancelled / stopped), pop the front of the deque and start the next run.
- Cap the deque (e.g., 32).  Overflow either rejects or drops oldest with `LOG_APP_ERROR`.

Dashboard work pairs with this:
- Render `pending` runs distinctly from `running` runs (different badge / state column).
- Stop button on a pending run dequeues cleanly — no AI calls dispatched yet, just remove from queue.
- Match the dashboard's existing "grey out Run button" UX to the actual engine behaviour: with serialize-default, the Run button can stay enabled and queue the request rather than reject it.

Per-run queue folder (separate, deeper work, not blocking this entry): `queue/<workflowId>/<runId>/<NN>_<task>/` instead of `queue/<workflowId>/<NN>_<task>/`.  Required for `parallel`-opted workflows to be hermetic.  Tracked here as a follow-up rather than its own entry because it only matters once `parallel` is opt-in.

### Cyber-security hardening pass
Plan: `doc/misc/cybersec-hardening-dev-plan.md`.  Source: `doc/combinedCyberSecAudit.md` (729 findings: 54 CRITICAL, 239 HIGH, 279 MEDIUM, 157 LOW).  4-domain split, 4 combined sessions with §19.  Session order: S1=D2 web+cloud+assistant (densest CRITICAL surface), S2=D3 core engine, S3=D1 workflow orchestration, S4=D4 app infrastructure.

### C++ safety hardening pass
Plan: `doc/misc/cpp-safety-hardening-dev-plan.md`.  Source: `doc/combinedSafetyAudit.md` (1243 findings: 13 CRITICAL, 277 HIGH, 483 MEDIUM, 470 LOW).  Same 4-session schedule as cyber-sec, run together.  Memo organized as Rust-emulating C++ defaults.

### Per-interface mock transport for parser fault injection
Sequenced after the 4 hardening sessions.  New `IInterfaceTransport` abstraction with two impls per real `InterfaceType` (API1–API5 + API6 reusing API1's parser): `LiveTransport` (real curl + auth signer, current behavior) and `MockTransport` (canned responses from disk fixtures).  Switch is dispatch-time, driven by the request: if the request carries a `X-J9T-Mock-Fixture: <name>` header (and a hermetic-mode flag), `MockTransport` is selected and serves bytes from `test/dispatch/fixtures/<api>/<name>.json` (or similar); otherwise `LiveTransport` runs unchanged.  Match strategy is **InterfaceType + fixture-name header only** — no URL or full-header matching, keeping the mock cheap to maintain.

Goal trio: (1) byte-level fault injection through real parsers (malformed UTF-8, surrogate halves, truncated multi-byte, overlong encodings) — closes the §19 SanitizeUtf8 verification gap that today's hermetic dispatcher can't reach; (2) per-interface contract tests catching response-shape drift (provider adds/renames fields) without burning quota; (3) reproducible parser regression fixtures.

**Complementary to existing HTTP mocks** (`aoai-api-simulator` for API6, LocalStack for API5) — those keep covering auth + curl + multi-dispatcher behavior with realistic well-formed bodies.  The routing-layer mock focuses on parser/byte pathology where the HTTP mocks are weak.  Not redundant: different layers, different bug classes.

Implementation skeleton:
- New header `engine/curlWrapper/interfaceTransport.h` defines `IInterfaceTransport` (one virtual dispatch method matching today's `CurlMultiDispatcher` request shape).
- `LiveTransport` wraps the existing curl path verbatim (refactor, not rewrite).
- `MockTransport` reads `<fixture>.json` (or `.bin` for binary-pathology fixtures) plus an optional `<fixture>.meta.json` for HTTP status / headers / latency injection.
- Selection at `AiRequestPool::Submit` time: if `m_MockFixture` is set on the envelope, use Mock; else Live.
- Fixtures committed under `test/dispatch/fixtures/api{1..6}/`.  Each interface gets a baseline `golden_response.json` + a battery of pathology fixtures (`malformed_utf8.json`, `truncated_multibyte.json`, `surrogate_half.json`, `overlong.json`, `empty_choices.json`, `missing_finish_reason.json`, etc.).
- New test files `test/dispatch/test_api{1..6}_mock_*.py` per interface drive the dispatcher with fixture names and assert downstream invariants.

### Dogfood the workflow editor (JC)
Write a few non-trivial JCWFs **directly in the editor** rather than as raw JSON: sub-workflow nesting, per-item fan-out, mixed task types (ai_call + python + shell + cloud), file_watch trigger, error-branching edges.  The editor exists and has a 70-test suite, but it's never been driven by JC in anger.  Goal: surface UX gaps, validation-surprise messaging, broken-state visibility, inspector quirks.  Findings inform 1.0 polish or post-1.0 backlog depending on severity.

### Dogfood the AI assistant (JC)
Drive a real conversation through the assistant chat surface: multi-turn tool-use loop, approval flow for mutating tools, the eight `jcwf_*` tools (read / explain / validate / read_plan / write_plan / generate / fix_task / write_script), runtime-control tools (`workflow_pause/resume/stop`, `get_dashboard_status`), slash commands, ghost-text auto-completion, history search, persistent session save/load.  Cross-references §18 D2 hardening triage: the assistant is exactly where the cyber-sec audit found its densest CRITICAL cluster (`assistantTools.h` has five shell-injection findings in a single file + the tool-approval bypass in `assistantController.h`).  Findings reachable in real use should jump the §18 D2 queue; findings unreachable in any plausible workflow get a "skip with reason" entry.  Two-for-one: dogfood validation **and** sharper hardening triage.

### Repository layout + root-folder hygiene
Group sources under `code/{backend,frontend,mcp}`; gitignore runtime folders (`queue/`, `workflows/`); prune root artefacts (`.npm-tools/`, `jarvis_agent.example.env`); consider moving Docker files to `packaging/Docker/`.  Rollout in 4 phases (runtime folders → root cleanup → Docker relocation → source tree reorg).  GitHub first impression matters before 1.0.

### Landing page for new users
Welcoming landing page / website explaining what JarvisAgent is, key features, screenshots, download links.  Target: first-time visitors who discover the project.

### Promotion video
Demo / promotion video covering workflow creation in the editor, running workflows, dashboard monitoring, multi-platform support.  Target: GitHub README embed, YouTube, social.

---

## §5i follow-ups (post-implementation)

The main §5i work landed 2026-04-25 (see `doc/misc/engine-studio-capability-review.md` lines 333-339).  These are the small tail items the review doc tracked at end-of-session:

- **`POST /api/shutdown` skips the role-gate audit line** — real audit-log gap.  Operator denials emit only `mcp_auth_success` and silently 403 instead of the standard `forbidden reason=insufficient_role`.  Trace the shutdown handler's auth check and route it through `CheckAuth(req, "admin")` so the audit log captures denials of the most consequential admin action.
- **`HandleAiInterfaceTestPost` Engine fallback** — today returns `ai_test_not_available_in_engine` and lives in `webServer.cpp` (not `webServer_studio.cpp`) with one inline `#ifdef J9T_STUDIO` around the `m_AiJcwfService.TestAiInterface(...)` call.  Two options: (a) move the test path into `aiRequestPool` so both editions support it (more useful for Engine admins debugging interface config); (b) make the route Studio-only and hide the dashboard's Test button on Engine.  Either is fine — pick one.
- **AI-WebSocket dispatch extraction** — ~150-line `#ifdef J9T_STUDIO` block in `webServer.cpp`'s `/ws` `.onmessage` lambda dispatching `ai-explain-jcwf` / `ai-generate-jcwf` / `ai-write-scripts` / `ai-fix-failed-script` / `chat`.  Extract into a helper (`WebServer::HandleAssistantWebSocketMessage`) and move into `webServer_studio.cpp`.  Mechanical, drops the `#ifdef` count in `webServer.cpp` from 10 to ~7.
- **Bootstrap admin user collides with role name** — first-run admin's user is literally `admin`, which renders as `admin / admin` in the dashboard StatusBar (user / role badge).  Either rename the bootstrap user (e.g. `j9t-bootstrap-admin`) at MCP key activation time, or have the React StatusBar suppress the role badge when user equals role.  Cosmetic only — no security impact.

---

## §5g remaining follow-ups (post-1.0)

The AI dispatch refactor's main work is committed; these are slice items the §5g handoff explicitly tracks for later:

- **JCWF schema gap-close** — close gaps in `doc/jcwf.schema.json` vs. `workflowJsonParser`; add a parser↔schema contract test.  Embedded `kJcwfSchemaJson` already compiled into the binary.
- **Schema-enforced JCWF generation** — wire `AiJcwfService::GenerateAsync` to set `AiInvocation.m_OutputSchemaJson = kJcwfSchemaJson` with validator-error retry.
- **`EventCategoryAi` consumers** — events posted from the dispatch lifecycle; only the aggregated "in flight" LED reads them today.  TUI / dashboard could surface per-call AI events.
- **More `test/dispatch/` contract test slices** — many landed (hermetic, relaxed env, output-schema roundtrip, chunking, markitdown, cross-workflow concurrency, Tier A, Tier B, TUI stress).  Remaining slices tracked as follow-ups; no specific list yet.
- **Live-backed E2E tests for schema-validation retry, chunking, and markitdown** — flagged in `doc/misc/AI dispatch refactor.md` post-merge follow-ups: today these paths are hermetic-only (TestInterface).  Live-backed versions hitting real providers would catch tokenizer / quirk drift the hermetic tests don't.

---

## Post-1.0

### Native LLM tool-calling
- **§5e.1 Assistant** — replace `<tool_call>` regex parser in `assistantTools.h::ParseToolCalls()` with native tool-calling (OpenAI `tool_calls[]`, Gemini `functionCall`, Anthropic `tool_use`).  Extend `ReplyParser` with `GetToolCalls()`; tag parser stays as fallback for interfaces that don't expose tools.
- **§5e.2 JCWF `ai_call`** — JCWF `ai_call` gains optional `tools: [{name, description, parameters_schema, handler_task}]`.  `AiCallTaskExecutor` enqueues handler synchronously, awaits `.output.txt`, appends `ToolReturn`, re-dispatches.  New design needed: bounded `max_tool_turns`, deterministic-replay story, freshness-model interaction, transcript format extension.

### j9t as orchestrator of other AI tools — Claude Code PoC
[from §5f] New `InterfaceType::ClaudeCode` that spawns `claude -p "<prompt>"` instead of HTTP.  Auth via user's Claude Max/Pro subscription (no API key in j9t).  Concurrency cap, default `--permission-mode=plan` (read-only), Engine refuses mutation.  Longer-term candidates: cross-instance j9t→j9t via existing MCP sidecar, Cursor Agent (headless), Aider, OpenAI Codex CLI.

---

## Cloud integration tail (post-implementation)

The bulk of the cloud-integration work landed (Phases 0-12 done; see `doc/misc/cloud-integration-dev-plan.md`).  Two open items remain:

- **`email_watch` trigger doesn't actually check IMAP** — currently fires on the polling timer regardless of whether new mail arrived.  Wire the trigger to perform an actual IMAP UID check before firing so it only triggers on genuinely-new messages.
- **Mailpit (send-only) has no JCWF coverage** — the dashboard's manual cloud-connection test shows 14 healthy, but a full JCWF run only flips 13 to healthy.  Root cause: the cloud integration plan's "Simulatable Without Real Cloud Accounts" list has **two** email Docker mocks (`Mailpit` for SMTP-only send + `GreenMail` for SMTP+IMAP round-trip).  `emailDemo.jcwf` exercises only the GreenMail connection; Mailpit never gets `RecordSuccess` from any workflow.  Two fix options: (a) add a separate send-only `mailpitDemo.jcwf` (or rename it to `emailSendDemo.jcwf`) that drives only `email_send` against the Mailpit connection; (b) parameterize `emailDemo.jcwf` to take the connection name as input and run it twice (once per email mock).

(The two cloud-integration-dev-plan checkboxes for Redmine frontend and Snowflake round-trip E2E were both stale — Redmine has full frontend coverage in `ConnectionsView.tsx` + `WorkflowEditorView.tsx`, and `snowflakeQueryDemo.md` documents the verified round-trip end-to-end.  Plan checkboxes never got flipped; closed here.)

---

## Loose follow-ups

- **`tools/replayTranscript.py`** — nominally-planned dispatch debugging tool from §5g.  Reads a `<prob>.transcript.json` and re-emits the exact request body against the same provider, for reproducing drift.  Not built; add when first real replay need arises.

- **`HandleWorkflowVersionRestorePost` is broken since JCWF moved to zip containers** — Surfaced 2026-04-30 during sitting 8 verification.  `POST /api/workflows/<id>/versions/<ts>/restore` reads the version file as raw bytes via `std::ifstream` then passes those bytes to `WorkflowRegistry::SaveOrUpdateWorkflowFromJson`, which expects plain JSON.  Since `.jcwf` files are always zip containers (per `feedback_no_legacy_jcwf`), the parser fails immediately with `restore_failed: UNCLOSED_STRING`.  The handler's auth check, path validation, TOCTOU-safe read, and best-effort backup all run correctly — only the final write step is wrong.  Fix: replace the `SaveOrUpdateWorkflowFromJson` call with the registry's container-aware write path (likely `UpsertJcwfFromZipBytes` or equivalent — check `workflowRegistry.h` for the right entry point).  Live-verified the failure with `exampleMakefile4`'s most-recent version; verified the live workflow is left untouched on failure (md5 unchanged), so this is a "broken feature, not a corrupting feature".  Rarely exercised — the dashboard's version history UI lists versions but the Restore button is the only client and may not have been tested end-to-end since the zip-container migration.
