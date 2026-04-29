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

### Cyber-security hardening pass
Plan: `doc/misc/cybersec-hardening-dev-plan.md`.  Source: `doc/combinedCyberSecAudit.md` (729 findings: 54 CRITICAL, 239 HIGH, 279 MEDIUM, 157 LOW).  4-domain split, 4 combined sessions with §19.  Session order: S1=D2 web+cloud+assistant (densest CRITICAL surface), S2=D3 core engine, S3=D1 workflow orchestration, S4=D4 app infrastructure.

### C++ safety hardening pass
Plan: `doc/misc/cpp-safety-hardening-dev-plan.md`.  Source: `doc/combinedSafetyAudit.md` (1243 findings: 13 CRITICAL, 277 HIGH, 483 MEDIUM, 470 LOW).  Same 4-session schedule as cyber-sec, run together.  Memo organized as Rust-emulating C++ defaults.

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

## §19 deferred companion work (from today's hand-off)

Today's TUI invalid-UTF-8 stress test surfaced and fixed the leak at the TestInterface boundary.  These two related boundaries were spec'd but punted into §19, scheduled for D1 (S3):

- **`SanitizeUtf8` at real AI reply parsers** — `application/json/replyParser{API1..API5}.cpp`.  Defense in depth for the rare-but-possible "provider returns corrupt bytes" case.
- **`SanitizeUtf8` at captured stdout/stderr** — `application/workflow/shellTaskExecutor.cpp` + `pythonTaskExecutor.cpp`.  `TruncateUtf8Safe` only handles boundary cuts, not bad bytes within the buffer.

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
