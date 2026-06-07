# JarvisAgent TODO

Open items consolidated 2026-04-28.  Closed-items history archived in `doc/misc/`:
- `doc/misc/JarvisAgent TODO List.md` — global archive
- `doc/misc/application-workflow-todo.md` — backend workflow archive
- `doc/misc/workflow-editor-todo.md` — frontend archive

The two scope-specific live files (`code/backend/application/workflow/doc/todo.md`, `code/frontend/workflow-editor/todo.md`) currently hold only headers — no open items.

**See also:**
- `doc/misc/hand-off.md` — session hand-off log; read latest entry first when picking up.
- `doc/misc/pre-1_0_follow-ups.md` — 14-sitting closeout plan for everything below except the five "Pre-1.0" non-engineering items.
- `doc/misc/cybersec-hardening-dev-plan.md` — §18 plan (4-domain split, 4 sessions combined with §19).
- `doc/misc/cpp-safety-hardening-dev-plan.md` — §19 plan (Rust-emulating C++ defaults).
- `doc/misc/AI call performance optimization.md` — §5g design reference (refactor complete).

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
- **[CRITICAL] `jcwfContainer` zip extraction** — (1) path traversal via symlink race (TOCTOU between the validation and extraction passes) in `Extract`; (2) `ReadFile` does not validate `internalPath`, allowing arbitrary zip-entry access.  Confine every entry under the destination root (reject `..` / absolute / symlink-escape), re-check on the actual extracted fd, and bound `mz_zip_reader_extract_file_to_heap`.
- **[HIGH] `urlPolicy::ValidateAiInterfaceUrl` DNS rebinding** — validation resolves loopback but libcurl re-resolves at connect.  For plain `http://`, allow only literal `127.0.0.1` / `[::1]` (no DNS), or pin the validated address into the dispatch via `CURLOPT_RESOLVE` / socket-open re-check.
- **[HIGH] `liveTransport` DEBUG TLS bypass** — the `#ifdef DEBUG` localhost `SSL_VERIFYPEER/HOST=0` block: confirm it cannot reach a shipped binary; gate behind an explicit runtime test-only flag with a per-exercise WARN, or drop it for a per-test CA bundle.
- **[HIGH] host/header values echoed unbounded** — cap the host string in `urlPolicy` error/log details (~256 chars) and verify `liveTransport` `publicHeaders` values can't carry CR/LF.

### Whole-system threat & hazard analysis (not file-by-file)
The per-compilation-unit cyber-sec audit (`jarvisCppCyberSecAudit`) reviews each `.h`/`.cpp` in isolation, so it cannot surface emergent / architectural / data-at-rest issues — it never flagged that `connections.json` held plaintext endpoint URLs + credential references on disk, because no single file's review sees the aggregate at-rest exposure (the cyber-sec write-up also treated it as benign runtime data).  Add a **system-level pass**: a data-flow + trust-boundary + data-at-rest inventory (STRIDE-style) enumerating every sensitive datum, where it lives at rest and in what form, and which trust boundaries it crosses.  Likely a new audit JCWF mode fed `doc/architecture.md` + the connection/keystore inventory rather than per-file source.  Acceptance: such a pass would have flagged `connections.json` (and any future plaintext secret store) up front.

### [DOC GAP, pre-1.0] User manual has no "Cloud Connections" section
Found dogfooding as a new user (2026-06-06): `doc/jarvisagent.md` has an `AI SETUP` section but **no parallel `CLOUD CONNECTIONS` section** — a new user has nowhere in the manual to learn how to add a cloud connection (Connections tab, connector types, the OAuth consent flow, master-password re-auth on create).  The only real walkthrough (Google Sheets) lives in `example/workflows/sheetsQuizGrader.md` (per-workflow, undiscoverable), and `doc/cloud-integration.md` (dev/architecture-oriented) isn't linked from the manual or its `SEE ALSO`.  Fix: add a `CLOUD CONNECTIONS` section to the manual (dashboard Connections tab + OAuth-PKCE flow at a user level, master-password re-auth), and add `cloud-integration.md` + the per-connector example workflows to `SEE ALSO`.

### Dashboard polish
- **Anchor the title bar; scroll only the workflows list** — the top "N JCWFs in flight" summary scrolls away on a long active-run list.  Make the header sticky and confine the overflow scroll to the workflows-list container.
- **"Throttled" LED is sticky — never clears** — once on it stays on.  Likely driven off a cumulative counter (`dispatcher_total_throttled`) or a one-way flag; drive it from a recent-window / decaying signal so it clears after throttling stops.  Investigate the LED's source signal first.
- **Run button → Cancel button while running** — when a workflow's Run button is pressed it currently greys out/disables; instead turn it into a **Cancel** button so an inadvertently-started JCWF can be cancelled from the same control (wire to the existing cancel-run path).
- **GitHub "Star" link** — add a direct link/button in the dash to star the repo on GitHub (one-click from j9t).

### Curated example-workflow list — single source of truth + standardize to 5
The bundled-workflow list is hand-copied into ~11 build scripts and has drifted into 5 different sets; deb (`build-deb.sh` 4 vs `debian/rules` 6) and RPM (`build-rpm.sh` 4 vs `jarvisagent.spec` 5) even contradict themselves.  Standardize **every** target (deb both paths, RPM both paths, Arch, AppImage, Flatpak, macOS DMG + Homebrew, Windows) to the same 5: `aiCarMaintenancePipeline aiZipDemo make-example portfolioDividendAnalysis vehicleTroubleshootingGuide` (core4 + vehicleTroubleshootingGuide).  Drive it from one shared source the build scripts read, not N hand-copies.  Docker currently ships a 17-set — align or document why it differs.  (Done 2026-06-06: un-pinned the OpenAI `global.json::defaults.ai.provider` on all shipped examples except `bookSummaryPipeline`, kept as the one real/copyright-bound example.)

### Packaging — final pre-beta retest (all targets)
One full install→serve pass of every package on a clean checkout shortly before the beta announce (not per-change); confirm the served UIs load in each.  Linux dev-box targets (AppImage / Deb / Flatpak / Docker amd64) already pass, and `apt install` from the PPA round-trips (0.8.8); still to run on their own hardware: macOS DMG + arm64 Docker (miniMac), RPM (Rocky), Arch (Manjaro), Windows.

### AI interface probe timeout — consider per-interface config (post-1.0, low)
`AiRequestPool::TestInterface` uses a fixed `kTestTimeoutMs` (bumped 30→90 s on 2026-06-03 to cover local-LLM cold-loads on slow HW).  A per-interface override in `config.json` would let cloud interfaces keep a snappy probe while local Ollama/vLLM endpoints get a long cold-load window.  Not urgent; the 90 s blanket value is fine for now.

### Landing page for new users
Welcoming landing page / website explaining what JarvisAgent is, key features, screenshots, download links.  Target: first-time visitors who discover the project.

### Promotion video
Demo / promotion video covering workflow creation in the editor, running workflows, dashboard monitoring, multi-platform support.  Target: GitHub README embed, YouTube, social.

---

## Pre-1.0 follow-ups (planned)

The §5i, cloud-integration tail, and loose-follow-ups entries previously here have been consolidated into a 14-sitting dev plan: **`doc/misc/pre-1_0_follow-ups.md`**.

- 21 actionable items across 14 sittings, ~11–14 working days estimated
- Ordered safety → cleanup → verification → tooling
- §5i.3 (bootstrap admin user collides with role) closed at intake — `mcpKeyManager.cpp:402-403` already renders `boss / admin`, no collision
- Cloud `email_watch` entry reframed: IMAP UID check already wired; remaining gap is watermark persistence across restart (Sitting 12)
- Editor master-password entry reframed: dialog already in editor; remaining gap is MCP login parity (Sitting 5)
- Four KeyManager hardening items from a prior hand-off's carry-over list (audit MEDIUM/HIGH findings, never propagated here) folded in as Sitting 14
- `RedactingFormatter::format` per-line allocation was a Loose follow-up; deferred to the plan's Post-1.0 tail (pure perf, profile-gated)

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
