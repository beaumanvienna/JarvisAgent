# JarvisAgent TODO

Open items consolidated 2026-04-28.  Closed-items history archived in `doc/misc/`:
- `doc/misc/JarvisAgent TODO List.md` — global archive
- `doc/misc/application-workflow-todo.md` — backend workflow archive
- `doc/misc/workflow-editor-todo.md` — frontend archive

The two scope-specific live files (`code/backend/application/workflow/doc/todo.md`, `code/frontend/workflow-editor/todo.md`) currently hold only headers — no open items.

**See also:**
- `doc/misc/hand-off.md` — session hand-off log; read latest entry first when picking up.
- `doc/misc/AI call performance optimization.md` — §5g design reference (refactor complete).

**Completed plans (historical — kept for the record, not open work):**
- ~~`doc/misc/pre-1_0_follow-ups.md`~~ — DONE: Sittings 1–15 closed (2026-05-18 → 2026-05-24), Sitting 13 cancelled; only the Sitting-16 incidental-findings basket was ever a catch-all.
- ~~`doc/misc/cybersec-hardening-dev-plan.md` (§18)~~ — DONE: S1–S5, 2026-04-28 → 2026-05-12, ~93% of CRITs closed.
- ~~`doc/misc/cpp-safety-hardening-dev-plan.md` (§19)~~ — DONE: executed alongside §18; SanitizeUtf8 gap closed via Foundation Sitting 3.

---

## Pre-1.0

### Dogfood the workflow editor (JC)
Write a few non-trivial JCWFs **directly in the editor** rather than as raw JSON: sub-workflow nesting, per-item fan-out, mixed task types (ai_call + python + shell + cloud), file_watch trigger, error-branching edges.  The editor exists and has a 70-test suite, but it's never been driven by JC in anger.  Goal: surface UX gaps, validation-surprise messaging, broken-state visibility, inspector quirks.  Findings inform 1.0 polish or post-1.0 backlog depending on severity.

### Dogfood the AI assistant (JC)
Drive a real conversation through the assistant chat surface: multi-turn tool-use loop, approval flow for mutating tools, the eight `jcwf_*` tools (read / explain / validate / read_plan / write_plan / generate / fix_task / write_script), runtime-control tools (`workflow_pause/resume/stop`, `get_dashboard_status`), slash commands, ghost-text auto-completion, history search, persistent session save/load.  Cross-references §18 D2 hardening triage: the assistant is exactly where the cyber-sec audit found its densest CRITICAL cluster (`assistantTools.h` has five shell-injection findings in a single file + the tool-approval bypass in `assistantController.h`).  Findings reachable in real use should jump the §18 D2 queue; findings unreachable in any plausible workflow get a "skip with reason" entry.  Two-for-one: dogfood validation **and** sharper hardening triage.

### 0.8.8 — queued follow-ups (shipped in `bbd2d98`, PPA-published)
- ~~**Strict-refuse args** — `engine.cpp` + `AppRun` reverted; changelog note reverses the 0.8.7 "args ignored" bullet.~~
- ~~**Scripts copied, not symlinked** — all six launchers fixed (real `scripts/` so workflow script tasks pass path-confinement).~~
- ~~**MCP default URL** — flipped `code/mcp/src/config.ts` to `https://localhost:8443`; `NODE_EXTRA_CA_CERTS` documented; `dist` rebuilt.~~
- ~~**Launcher arg-handling parity** — all six wrappers own arg policy identically (info-flags skip setup, `--home`/`--no-browser`, strict-refuse unknown); version bumped 0.8.8.~~

### Security findings — address at keystore-refactor close-out
Surfaced by the cyber-sec audit once the recently-added files entered its scope; none are in the refactor's own new code (the two MEDIUMs there — `ApiInterfaceManager` interface-count cap + `EncryptedJsonStore` read TOCTOU — are already fixed).  CRITICAL first.
- ~~**[CRITICAL] `jcwfContainer` zip extraction**~~ — done: write-time half of the Zip-Slip defence (`EnsureSafeDirs` refuses a symlink ancestor; `AtomicWriteFile` tmp+rename replaces a symlink destination rather than following it; post-write canonical containment net), zip-bomb caps (per-entry / total uncompressed, entry count), encrypted-entry reject, empty-canonical-root fail-closed; `ReadFile` validates `internalPath` + bounds the entry size before `extract_to_heap`.  E2e: `test/security/test_jcwf_zip_slip.py` (16/16).
- ~~**[HIGH] `urlPolicy::ValidateAiInterfaceUrl` DNS rebinding**~~ — done: connect-time `CURLOPT_OPENSOCKETFUNCTION` loopback re-check (`loopbackGuard.{h,cpp}` — inverse of ConnectorHttp's `OpensocketStrictCallback`) installed on every plain-`http://` AI dispatch (async `LiveTransport` + sync `CurlWrapper::Query`), aborting any non-loopback resolved peer; counter `ai_dispatch_nonloopback_http_rejections` in debug_signals.
- ~~**[HIGH] `liveTransport` DEBUG TLS bypass**~~ — done: confirmed compile-isolated (premake defines `DEBUG` for Debug only / `NDEBUG` for Release → cannot reach a shipped binary); added a per-exercise `LOG_SECURITY_WARN` so a debug build that disables localhost TLS verification is never silent.
- ~~**[HIGH] host/header values echoed unbounded**~~ — done: `urlPolicy` caps the host in error/log details at 256 chars (`CapForLog`); `liveTransport` rejects any `publicHeaders`/auth header carrying CR/LF at the slist-append boundary (fail-closed, ERROR + cancelKey).

### Whole-system threat & hazard analysis (not file-by-file)
The per-compilation-unit cyber-sec audit (`jarvisCppCyberSecAudit`) reviews each `.h`/`.cpp` in isolation, so it cannot surface emergent / architectural / data-at-rest issues — it never flagged that `connections.json` held plaintext endpoint URLs + credential references on disk, because no single file's review sees the aggregate at-rest exposure (the cyber-sec write-up also treated it as benign runtime data).  Add a **system-level pass**: a data-flow + trust-boundary + data-at-rest inventory (STRIDE-style) enumerating every sensitive datum, where it lives at rest and in what form, and which trust boundaries it crosses.  Likely a new audit JCWF mode fed `doc/architecture.md` + the connection/keystore inventory rather than per-file source.  Acceptance: such a pass would have flagged `connections.json` (and any future plaintext secret store) up front.

### ~~[DOC GAP, pre-1.0] User manual has no "Cloud Connections" section~~
DONE: added a `CLOUD CONNECTIONS` section to `doc/jarvisagent.md` (after AI SETUP) — supported-connector table, "Adding a connection" (Settings → Connections tab, master-password re-auth), the OAuth-PKCE consent flow for Google Sheets/OneDrive, and a "using a connection in a workflow" pointer to the per-connector example workflows; added `cloud-integration.md` to Contents + SEE ALSO.  Also fixed the now-stale AI-SETUP ollama example (dropped `key_name` — keyless loopback is correct; j9t rejects credentialed plain-http).

### Dashboard polish
- ~~**Anchor the title bar; scroll only the workflows list**~~ — done: `.status-bar` is `position: sticky` (opaque base layered under the tint) so the LED/run-counter summary stays pinned while a long list scrolls.
- ~~**"Throttled" LED is sticky — never clears**~~ — done, two parts. (1) Root cause was a mislabel: the LED's amber condition was `current_cap < max_cap`, which fires whenever the AIMD cap sits below the configured ceiling — i.e. for any healthy interface that hasn't ramped to max, with zero actual 429s. Flipped the dashboard amber condition to key on **`last_429_at_ms`** (a real 429 within the last 60 s), surfaced per-interface on `/api/providers/health` + `dispatcher_controllers[]`. A never-throttled interface now shows green; a real throttle shows amber clearing ~60 s after the last 429. (2) Independently gave `RateLimitController` **time-based recovery** (RFC 5681 idle restart) so a 429-reduced cap recovers on elapsed wall-clock instead of freezing below the ceiling — fixes the next-burst penalty + lets `cap_recovery_eta_sec` count down. Throttle state is monitorable on the debug port (`hard_cap`, `last_429_at_ms`, `cap_recovery_eta_sec`).
- ~~**Run button → Cancel button while running**~~ — done: while a workflow has an active run, its Run button becomes a red Cancel button wired to `POST /api/workflow-runs/<runId>/cancel`; reverts to Run when the run ends.
- ~~**GitHub "Star" link**~~ — done: ★ Star link in the status bar (opens the repo in a new tab).

### ~~Curated example-workflow list — single source of truth + standardize to 5~~
DONE: created `packaging/curated-workflows.txt` (the canonical 5: `aiCarMaintenancePipeline aiZipDemo make-example portfolioDividendAnalysis vehicleTroubleshootingGuide`); all 10 scriptable packagers now READ it instead of hand-copying (4 `build-*.sh`, Arch `PKGBUILD`, RPM `jarvisagent.spec`, `debian/rules`, Flatpak yml, Homebrew `.rb`, Windows `.ps1`) — drift (4/5/6/17 different sets) eliminated.  **Every target ships the same 5, Docker included** (its static `COPY` can't read the file, so the Dockerfile mirrors the 5 by hand with a sync note).  Read patterns + shell/ruby/yaml syntax verified on the dev box; the actual RPM/DMG/Flatpak/Windows/Arch builds run in the packaging retest.  (Earlier 2026-06-06: un-pinned OpenAI `global.json` provider on shipped examples except `bookSummaryPipeline`.)

### Packaging — final pre-beta retest (all targets)
One full install→serve pass of every package on a clean checkout shortly before the beta announce (not per-change); confirm the served UIs load in each.  Linux dev-box targets (AppImage / Deb / Flatpak / Docker amd64) already pass, and `apt install` from the PPA round-trips (0.8.8); still to run on their own hardware: macOS DMG + arm64 Docker (miniMac), RPM (Rocky), Arch (Manjaro), Windows.

### AI interface probe timeout — consider per-interface config (post-1.0, low)
`AiRequestPool::TestInterface` uses a fixed `kTestTimeoutMs` (bumped 30→90 s on 2026-06-03 to cover local-LLM cold-loads on slow HW).  A per-interface override in `config.json` would let cloud interfaces keep a snappy probe while local Ollama/vLLM endpoints get a long cold-load window.  Not urgent; the 90 s blanket value is fine for now.

### Landing page for new users
Welcoming landing page / website explaining what JarvisAgent is, key features, screenshots, download links.  Target: first-time visitors who discover the project.

### Promotion video
Demo / promotion video covering workflow creation in the editor, running workflows, dashboard monitoring, multi-platform support.  Target: GitHub README embed, YouTube, social.

---

## Pre-1.0 follow-ups — ~~DONE~~

The §5i, cloud-integration tail, and loose-follow-ups entries were consolidated into **`doc/misc/pre-1_0_follow-ups.md`** and **executed** (Sittings 1–15 closed 2026-05-18 → 2026-05-24; Sitting 13 cancelled; only the Sitting-16 incidental-findings basket was a catch-all).  `RedactingFormatter::format` per-line allocation was the one item deliberately deferred to a Post-1.0 perf tail (profile-gated).

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
