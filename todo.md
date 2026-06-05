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

### 0.8.8 — queued follow-ups (staged in working tree; needs commit + version bump)
- **Strict-refuse args** — `engine.cpp` + `AppRun` reverted; changelog note reverses the 0.8.7 "args ignored" bullet.
- **Scripts copied, not symlinked** — all six launchers fixed (real `scripts/` so workflow script tasks pass path-confinement).
- **MCP default URL** — flip `code/mcp/src/config.ts` `J9T_URL` to `https://localhost:8443`, document `NODE_EXTRA_CA_CERTS`, rebuild `code/mcp/dist`.

### Packaging — final pre-beta retest (all targets)
One full install→serve pass of every package on a clean checkout shortly before the beta announce (not per-change); confirm the served UIs load in each.  Linux dev-box targets (AppImage / Deb / Flatpak / Docker amd64) already pass; still to run on their own hardware: macOS DMG + arm64 Docker (miniMac), RPM (Rocky), Arch (Manjaro), Windows, and `apt install` from the PPA.

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
