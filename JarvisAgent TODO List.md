# JarvisAgent TODO List

This list tracks **general project TODOs and high-level features** for JarvisAgent.

See also:
- `application/workflow/doc/todo.md` — C++ backend TODOs (workflow engine, runtime manager, task executors)
- `workflow-editor/todo.md` — Frontend TODOs (React workflow editor UI)

---

## ~~1. GitHub CI and cross-platform testing~~ ✅
- ~~Linux, macOS, and Windows workflows are green~~ ✅
- ~~Fix smoke test segfault (TTY / ncurses / config path)~~ ✅ graceful exit when config.json missing; Core destructor restores cout/cerr rdbuf
- ~~Run on actual macOS~~ ✅ tested on miniMac (macOS Tahoe) — TUI works, packaging verified
- ~~Run on actual Windows~~ ✅ tested — TUI works, packaging verified

---

## 2. ~~Dockerization~~ ✅
- Cherry-picked Ahmet's PR #1 (preserving authorship); PR closed with comment
- 3-stage Dockerfile: dashboard (Node.js), C++ builder, runtime (Ubuntu 24.04)
- Runtime includes: markitdown, md2pdf-mermaid, Google Chrome, python3-dev
- Headless mode: TTY detection in TerminalManager + KeyboardInput
- Fixed crash in SetStatusCallbacks (guarded RecreateWindowsIfNeeded with m_Initialized)
- Docker CI workflow pushes to ghcr.io; badge added to README
- All 4 CI checks green: Docker ✅ Linux ✅ macOS ✅ Windows ✅

---

## ~~3. Terminal UI~~ ✅
- ~~PDCurses on macOS: backend VT is configured, needs to be tested~~ ✅ tested on miniMac (macOS Tahoe) — log + status windows, green theme
- ~~PDCurses on Windows: backend Wincon is configured, needs to be tested~~ ✅ tested on real Windows — TUI works

---

## ~~4. Workflow system~~ ✅
- ~~Manual trigger via browser-based terminal prompt~~ — dropped; workflow editor UI covers trigger/run/status
- ~~Workflows for individual lines from spreadsheets~~ — done: `csv` and `text_lines` per-item filters + `portfolioDividendAnalysis` and `goKartComplianceCheck` demo workflows

---

## ~~5. Native Google Gemini reply parser~~ ✅
- ~~Implement a dedicated reply parser for the native Gemini API~~ ✅ `ReplyParserAPI3` (`replyParserAPI3.h/.cpp`)
- New `InterfaceType::API3` added to `configParser.h`, recognized in `configParser.cpp` and `webServer.cpp`
- `CurlWrapper::AuthStyle::XGoogApiKey` — sends `x-goog-api-key:` header instead of `Authorization: Bearer`
- `SessionManager::DispatchQuery()` builds Gemini-native request body and constructs URL with model: `{base}/models/{model}:generateContent`
- `SessionManager` resolves API key via `key_name` from interface config (not just default provider); lazy re-resolve at dispatch time for keys added after startup
- Frontend dropdown in `AiManagerView.tsx` includes "API3 (Gemini native)" option
- Config: set `"API": "API3"`, URL to `https://generativelanguage.googleapis.com/v1beta`, model to e.g. `gemini-2.5-flash-lite`
- Tracked `packaging/config.json.example` with all 7 interfaces (including both Gemini entries + gpt-4.1/MAX for maximum reasoning quality); all 11 packaging scripts updated to use it instead of gitignored `config.json`
- Documentation updated: `json.md`, `jarvisagent.1`, `jarvisagent.html`, `jarvisagent.md`, `keys.md`, `api-endpoints.md`, `README.md`
- E2E tested: `exampleMakefile4` completed successfully twice via Gemini native API3 (`gemini-2.5-flash-lite`)

---

## ~~6. Python Engine parallelization~~ (moved to Remaining TODOs #1)

---

## ~~7. Multi-user support for system-wide installs~~ ✅
- ~~When installed system-wide via deb/rpm/Arch (`/opt/jarvisagent/`), `queue/` and `workflows/` are owned by root~~
- ~~Non-root users cannot write to these directories without `sudo`~~

**Solution: user-space launcher script** (implemented for all platforms)

- **Linux:** `packaging/Linux/jarvisagent-launcher.sh` — shared launcher for RPM, DEB, Arch, AppImage, Flatpak. Per-user `~/JarvisAgent`, `ln -sfn` symlinks, venv activation, browser launch. Supports `--home DIR`, `--no-browser`, `JARVISAGENT_HOME` env var.
- **macOS:** DMG `build-dmg.sh` inline launcher, Homebrew formula `jarvisagent.rb`. Uses `open` for browser launch.
- **Windows:** `jarvisagent.bat` with directory junctions (`mklink /J`, no admin). `setup-venv.bat` for manual venv repair. Uses `start` for browser launch.
- Docker: no launcher needed (headless mode).
- All platforms: per-user Python venv setup on first run, browser launch on first run, no `rm` commands.

---

## ~~8. Workflow editor improvements + AI assistance~~ ✅
- ~~**AI → JCWF:** User describes a workflow in natural language (prompt), AI generates a valid `.jcwf` file~~ ✅ Multi-stage pipeline in `AiJcwfService`: decompose → generate JCWF (batched fan-out) → generate scripts → review → validate → fix. Supports both **Python** and **shell (bash)** script generation with POSIX awk rules, host OS detection, and positional arg mapping. Validator uses `WorkflowFileIndex` to suggest file path corrections. E2E verified with `cyber2` (Python) and `cyber3` (shell) workflows. See `example/workflows/cyber2_e2e.md` and `cyber3_e2e.md`.
- ~~**Fix Script button:** Runtime script fix — reads failed script + stderr, sends to AI, user reviews fix in `ScriptReviewPanel`~~ ✅ `FixFailedScriptAsync()` in `aiJcwfService.cpp`, WS handler in `webServer.cpp`.
- ~~**JCWF → AI:** User loads an existing `.jcwf` file, AI generates a human-readable summary/documentation~~ ✅
- ~~Integration point: workflow editor UI sends prompt to backend, backend calls AI provider, returns structured JCWF JSON~~ ✅
- ~~Validation: generated JCWF should pass `workflowValidator` before being offered to the user~~ ✅ Validator runs after generation; warnings trigger a fix stage with AI auto-correction. Script content validation (shebang, metadata, pipefail) added for on-disk scripts.
- ~~UX: "Generate from prompt" button and "Explain this workflow" button in the editor~~ ✅

---

## ~~9. Webhook trigger type~~ ✅
- ~~A dedicated `"type": "webhook"` trigger~~ ✅ Implemented across 4 phases:
  - Per-workflow HMAC-SHA256 signature verification (`X-Webhook-Signature` header)
  - `POST /api/webhook/<workflowId>` endpoint with context injection
  - Completion callback POST to `callbackUrl` when run finishes (fire-and-forget, 15s timeout)
  - Frontend webhook trigger editing (secret field + endpoint URL hint)
  - n8n custom node v2 with webhook/legacy endpoint toggle + HMAC signing

---

## ~~10. Error branching / conditional edges~~ ✅
- ~~Currently workflow runs are atomic — if a task fails, the run fails~~ — solved with `control_nodes` (branch) + `controlflow` edges + `expose_error_signal`
- Implemented via `control_nodes` array with `"type": "branch"` and `controlflow` edges with `"kind": "normal"/"error_signal"/"on_error"` — more expressive than the originally proposed `on_error` field
- Runtime: `FireBranchIfReady` in `workflowRuntimeManager.cpp` activates selected path, skips unselected. Rule A: handled failures don't fail the run.
- Verified with `exampleMakefile5`: AI generates code with deliberate error → shell fails → branch_1 error path → ai_call_fix → shell_retry → run hello
- Changes: `workflowJsonParser.cpp`, `workflowValidator.cpp`, `workflowRuntimeManager.cpp`, `workflowTypes.h`, JC Workflow Specification §7

---

## ~~11. Built-in retries with backoff~~ ✅
- ~~Per-task retry configuration~~ ✅ `"retries": { "max_attempts": N, "backoff_ms": N }` in JCWF task or `defaults`
- `TryScheduleRetry` in `workflowRuntimeManager.cpp`: linear backoff (`m_BackoffMs * attempt`), `m_RetryAfterTime` respected by dispatch loop
- Deadlock detector accounts for retry-pending tasks
- Retries are exhausted before error branching (#10) fires

---

## ~~12. Browser-based AI chat terminal~~ ✅
- ~~Phases 1–10 implemented~~ ✅ xterm.js terminal in "Assistant" tab, `/ws/assistant` WebSocket, `AssistantController` + `AssistantSession` + `ContextAssembler` + `ToolRegistry` (31 tools) + `MemoryStore` + `WorkspaceIndexer`. Persistent sessions (JSONL), workspace memory (`memory.json`), file index with cached AI summaries (`file_index.jsonl`). Slash commands: `/help`, `/status`, `/runs`, `/log`, `/memory`, `/index`, `/sessions`, `/new`, `/clear`. Ghost-text auto-completion + Ctrl+R history search. Multi-step tool loop (max 10 iterations) with loop detection. User approval flow for all mutating tools (60s timeout). Mutating tools: `run_shell`, `write_file`, `edit_file`. JCWF development tools (8): `jcwf_read/explain/validate/read_plan/write_plan/generate/fix_task/write_script`. Runtime control tools: `workflow_pause/resume/stop`, `get_dashboard_status`. Response validation (keyword overlap + path existence). 70-test suite covering all tools, approval flow, loop detection, access control. Assistant button greyed out when no AI provider configured. See `application/assistant/ai-assistant.md`.

---


## ~~13. Headless server mode~~ ✅
- Headless mode works via automatic TTY detection (`isatty()` in `keyboardInput.cpp` and `terminalManager.cpp`)
- Docker without `-it` runs headless out-of-the-box — verified with `/api/status`
- Added `--headless` flag to `scripts/run-docker.sh` (omits `-it`), documented in README.md
- Explicit `--headless` CLI flag for the binary itself deferred (auto-detection covers all current use cases)

## ~~14. Man page~~ ✅
- Created `doc/jarvisagent.1` (troff) — covers intro, CLI, config.json fields, env vars, AI setup, JCWF summary, workflow editor
- Created `doc/jarvisagent.html` — standalone HTML version for Windows and online viewing
- Installed as system man page in DEB, RPM, Arch, Homebrew packages (`/usr/share/man/man1/jarvisagent.1.gz`)
- Included in doc/ directory for Flatpak, AppImage, macOS DMG
- Added "User Manual" section to README.md with links to man page and HTML

---

## ~~15–18~~ (moved to Remaining TODOs #4–7)

---

## ~~19. Packaging testing~~ ✅
- [x] **macOS:** DMG tested on real hardware, CI smoke test passes
- [x] **Windows:** MSI/ZIP tested on real hardware
- [x] **Linux (deb):** Tested, launcher script works
- [x] **Linux (rpm):** CI builds RPM, versioned artifact names
- [x] **Linux (Arch):** CI builds pkg.tar.zst, versioned artifact names
- [x] **Linux (Flatpak):** End-to-end tested (install, first-run, make-example workflow)
- [x] **Linux (AppImage):** CI builds, versioned filename
- [x] **All platforms:** First-run experience verified (venv auto-creation, config.json, example workflows)



## ~~20. AI test button~~ ✅
- ~~Add a test button in AI manager of workflow-editor react app~~ ✅
- Direct curl ping with 10s timeout (bypasses SessionManager). Backend: `POST /api/settings/ai-interfaces/test`. Frontend: LED indicator (green=success, red=failure, yellow=testing). Bad URLs fail instantly (e.g. HTTP 404).
- Edit modal: centered overlay with blurred backdrop replaces inline edit form.

---

## ~~21. Settings dialog for workflow editor + JCWF assistant provider config~~ ✅
- ~~Add a settings dialog in the workflow editor for `config.json` fields~~ ✅ Gear-icon modal with Default AI Interface, Max Threads, Max File Size, JCWF Batch Size, Verbose toggle. Backend: `GET/PUT /api/settings/config`.
- ~~JCWF assistant provider override (PROV file selection)~~ ✅ "JCWF AI Interface" dropdown in Settings modal selects a non-default AI interface for the Generate / Explain / Fix Script pipeline. Stored as `jcwf_ai_interface` in `config.json`. Backend: `HandleConfigSettingsPut` persists the index; `AiJcwfService` resolves the selected interface and writes `PROV_provider.json` sidecar files. E2E verified: `api.openai.com/gpt-4.1-mini/API2` correctly appears in PROV files for both decompose and generate stages.

---

## ~~22. Python task stdout/stderr capture~~ ✅
- ~~Capture stdout/stderr from Python task scripts~~ ✅ Inline `_JarvisTee` class in `PythonEngine` duplicates output to both the original `_JarvisRedirect` (real-time terminal) and a `StringIO` buffer. `ExecuteWorkflowTask` returns captured strings; `PythonTaskExecutor` stores them in `TaskInstanceState` and writes `stdout.txt`/`stderr.txt` to the task working directory. Cross-platform (pure Python). Verified with "Print Hello World" workflow showing `Hello, World!` in the editor tooltip.

---

## ~~23. AI Manager modal styling consistency~~ ✅
- ~~AI Manager "Add Interface" modal used transparent backdrop with blur, inconsistent with Settings modal~~ ✅ Replaced inline styles with `modalOverlay` + `modalContent` + `modalHeader` + `modalBody` CSS classes matching `SettingsModal.tsx`. Solid dark background, no transparency, no blur.

---

## ~~24. Workflow reload re-fires all auto triggers~~ ✅
- ~~Navigating to the Workflows page triggered `POST /api/workflows/reload` which re-registered all auto triggers and fired them immediately~~ ✅ Added `bool fireImmediately` parameter to `TriggerEngine::AddAutoTrigger` (default `true`). `WorkflowTriggerBinder::RegisterAll` accepts `bool fireAutoTriggers` and propagates it. `HandleWorkflowsReloadPost` passes `false` — triggers are re-registered (cron/file_watch still work) but auto triggers don't re-fire. Startup path unchanged.

---

## ~~25. PDCurses MAX_UNICODE assertion crash~~ ✅
- ~~`PDC_transform_line` in `vendor/pdcursesmod/vt/pdcdisp.c:363` crashed with `assert(ch < MAX_UNICODE)` when AI responses contained emoji characters above the BMP (e.g. 🌟 U+1F31F)~~ ✅ Replaced `assert` with a `break` guard — if `ch >= MAX_UNICODE`, the inner loop exits and the outer loop handles the character correctly via the `USING_COMBINING_CHARACTER_SCHEME` path.

---

## Remaining TODOs

### ~~1. Python Engine parallelization~~ ✅
- ~~Add support for multiple independent PythonEngine instances~~
- ~~Ensure each interpreter instance owns its own GIL~~
- ~~Store PythonEngine instances in std::vector~~
- ~~Default engine count: 4~~
- ~~Allow override via config.json~~
- ~~Expose internal task-queue size for load balancing~~
- ~~Dispatch OnEvent() to the PythonEngine with the lowest queued workload~~
- ~~Ensure isolated interpreter state per engine~~
- **Done:** `PythonEnginePool` manages N sub-interpreters via `Py_NewInterpreterFromConfig()`. Load-balanced dispatch to engine with smallest queue depth. Hooks route to engine[0] only. Configurable via `"python engines"` in config.json (default 4, range [1,16]). Currently uses shared GIL (`PyInterpreterConfig_SHARED_GIL`) for C extension compatibility on Python 3.12; true per-interpreter GIL available when Python 3.13+ is the minimum. Verified end-to-end with 60 parallel Python tasks in `portfolioPythonAnalysis` workflow.

### ~~2. Browser-based AI chat terminal~~ ✅
- ~~Implemented (Phases 1–4).~~ See item #12 above and `application/assistant/ai-assistant.md`.

### ~~3. Show broken JCWFs in the workflow editor~~ ✅
- ~~JCWF containers that fail to parse are silently dropped from the WorkflowRegistry~~ ✅
- **Done:** Registry tracks `m_BrokenWorkflows` (container path + stem + error message). `/api/workflows` includes `broken[]` array in response. Workflow editor list view shows broken workflows with red left border, red title, and yellow error message. Users can see which `.jcwf` files are broken and why without digging through logs.

### ~~4. Sub-workflows / workflow-call node~~ ✅
- ~~Invoke one JCWF from another as a task~~ ✅
- ~~Enables modular composition of complex pipelines~~ ✅
- **Done:** `sub_workflow` task type with `workflow_file` field. `SubWorkflowTaskExecutor` enqueues child `WorkflowRun` and returns `WaitingExternal`; `PropagateSubWorkflowCompletions()` in runtime manager propagates child completion/failure back to parent. Cancellation propagates to child runs. DFS cycle detection + depth limit (max 10) in validator. `.jcwf` is now a zip container format: `global.json` (workflow-wide metadata) + canvas JSONs + sub-workflow folders (folder name = display name). `JcwfContainer` utility wraps miniz for Extract/Pack/ReadFile. Registry extracts and loads containers recursively. Editor: dashed-border sub-workflow nodes, "+ Sub-Workflow" button, inspector with workflow_file input, double-click navigation, breadcrumb bar, collapsible tree view in left sidebar. AI assistant tools updated for zip format. All example workflows converted. Packaging scripts updated.

### 4. ~~Launchpad PPA~~ ✅
- ~~Upload source-code DEB package to Launchpad PPA: https://launchpad.net/~beauman/+archive/ubuntu/marley~~
- ~~Test end-to-end: `sudo add-apt-repository ppa:beauman/marley && sudo apt install jarvisagent`~~
- **Done (v0.8.2):** published, installed, and tested end-to-end. Shared launcher creates `~/JarvisAgent` with user-space Python venv on first run.

### ~~5. Unified auth model — MCP API keys + sessions, legacy admin token removed~~ ✅
- All Engine REST auth migrated to MCP API keys (per-user identity, RBAC admin/operator/viewer, audit trail)
- Studio authenticates MCP clients via the same MCP key store — browser UI still open on localhost
- Dashboard login flow: `POST /api/auth/login` with an MCP key → HttpOnly + SameSite=Strict session cookie (8h sliding timeout)
- WebSocket auth moved to Crow's `.onaccept` handshake — no in-band auth messages
- `engine_api_token.txt` and the shared bearer-token code path **removed entirely** — no legacy fallback in `Authenticate()`
- `SecureString` RAII buffer (`mlock()` / `explicit_bzero()`) holds the master password; one password unlocks both `keys.json.enc` and `mcp_keys.json.enc`
- First-run admin enrollment token auto-generated and logged to stderr when `mcp_keys.json.enc` is empty
- MCP sidecar expects `J9T_TOKEN=mcp_...`; shared service-credential pattern deliberately unsupported
- See `Adhoc Workflow Submission and MCP plan.md` and `doc/cyber security.md` for the full design

### ~~5a. Adhoc workflow submission + MCP configure-plane tools~~ ✅
- `POST /api/workflows/run-adhoc` — one-shot JCWF execution without permanent registration; scripts must pre-exist under `scripts/` (no script upload through this endpoint, hard security boundary)
- `AdhocWorkflowManager` — per-run isolated folder `_adhoc/<ts>_<counter>_del-<ts>/`, restart-safe reaper thread (60s interval) using the delete-at suffix, per-user disk quota via `McpKeyManager`, `meta.json` attribution
- Per-run AI call cap (`max_ai_calls_per_jcwf` in config; default 0 = unlimited; bumped inflight default 100→1000, clamp widened to [1, 10000])
- `on_completion` cleanup wired through `WorkflowRuntimeManager::SetRunTerminalObserver` so folders wipe the moment the run ends; TTL policies handled by the reaper; `retain` skipped entirely
- MCP configure-plane tools: `manage_connections`, `manage_keys`, `upload_workflow`, `validate_workflow`, `get_run_logs`, `whoami`, `run_adhoc_workflow`
- Dashboard: `LastRunsBar` with rolling last-3 runs, "adhoc" pill, relative time; MCP Keys admin panel with enrollment dialog

### ~~5b. Adhoc `ai_call` tasks hang — FileWatcher doesn't watch per-run adhoc queue folders~~ ✅

Fixed via Option A — extend `FileWatcher` with dynamic `AddPath`/`RemovePath`; `AdhocWorkflowManager` registers each run's `_adhoc/<user>/<run>/queue/` at stage time and unregisters on completion / reap. Adhoc `ai_call` end-to-end verified live: submit → dispatch → provider → `succeeded` in ~2 s; `session_managers_total` increments properly (was perpetually 0 before the fix). Integration test `test_adhoc_aicall_roundtrip` in `test/test_auth_mcp.py` locks this down (gated on `--with-ai` so clean installs without a provider still pass the rest of the suite — 104/104 with the flag).

Spec clarification landed at the same time (`doc/JC_Workflow_Specification.md` §3.3.6.3, `doc/jcwf_generation_guide.md`): `file_outputs` on `ai_call` is allowed — Pattern A (zero-copy `outputs` slot) and Pattern B (`file_outputs` → external destination, e.g. adhoc agents writing to `~/dev/<project>/...`) are both valid. Validator emits `file_output_outside_working_tree` as Info (portability note; external-project agent use supported) and `file_output_triggers_extra_ai_query` as Warning (the real forbidden case — destination inside own queue folder with a requirement-firing filename).

Related cleanup flagged but not fixed in this pass (post-1.0): `FireBranchIfReady` log line fires with `completedState=6` (`WaitingExternal`) — misleading; `TimeoutWaitingExternalTasks()` didn't catch these stalls and should be revisited when Option E lands.

### 5d. Repository layout + root-folder hygiene (pre-1.0 cleanup)

The repository grew organically and the root is crowded. Group sources under a single `code/` tree, prune unused artefacts, and clean up root-level files that don't belong there. Target: before 1.0 ships (GitHub first impression matters).

**Source reorganisation:**

- Create `code/` with three subtrees:
  - `code/backend/` — move `engine/` and `application/` into it
  - `code/frontend/` — move `dashboard/` and `workflow-editor/` into it
  - `code/mcp/` — move `mcp/` into it
- `vendor/`, `test/`, `scripts/`, `packaging/`, `integration/`, `tools/` stay at the root (or move under `code/` — decide later; simpler to leave alone for the first pass)
- `premake5.lua` paths and the Studio / Engine `removefiles` patterns all need retargeting (`application/**` → `code/backend/application/**` etc.)
- Dashboard + editor build scripts (`cd dashboard/ui && npm run build`) — every doc/script touch point
- CI workflow path filters (`.github/workflows/*.yml`)
- CLAUDE.md, DEVELOPMENT.md, INSTALL.md, README.md — update all references
- Doc sweep: `doc/architecture.md`, `doc/cloud-integration.md`, `doc/jarvisagent.md` / `.1` / `.html` — hundreds of `application/` / `engine/` / `dashboard/` / `workflow-editor/` mentions
- WebServer static-file paths for dashboard + editor UIs (`dashboard/ui/dist/`, `workflow-editor/ui/dist/`) — update to new locations
- IDE workspace configs if any (`.vscode/` is gitignored so probably fine)

**Runtime folders (remove from git tree, keep on disk at runtime):**

- `queue/` — tracked only via an empty `.gitignore` placeholder. `git rm -r --cached queue/`, then add `/queue/` to the top-level `.gitignore`. The backend creates the folder on first start from `config.json`'s `"queue folder"` value.
- `workflows/` — same story. `git rm -r --cached workflows/`, add `/workflows/` to `.gitignore`.
- `_adhoc/` — already gitignored; already not tracked. No change needed.

**Root-level cleanup:**

- `.npm-tools/package.json` — 12-byte stub listing `@mermaid-js/mermaid-cli` as a dep. The only reference in the repo (`packaging/Linux/jarvisagent-launcher.sh`) installs into `$USER_HOME/.npm-tools`, *not* the repo's `.npm-tools/`. The checked-in copy is a leftover. Delete the folder + add `/.npm-tools/` to `.gitignore` for safety.
- `jarvis_agent.example.env` — 56 bytes, only referenced as a commented-out example in `docker-compose.example.yml`. Functionally unused. Delete.
- Docker files in root: `Dockerfile`, `docker-compose.example.yml`, `docker-entrypoint.sh`, `.dockerignore`. Consider moving to `packaging/Docker/` to declutter the root. Touch points: `.github/workflows/docker-publish.yml` (`file: ./Dockerfile`), `scripts/run-docker.sh`, any `.flatpak-builder` snapshots. Minor, but reduces visual noise in the root.
- `application/workflow/doc/aiCallArchitecture.md` — superseded by the AI Dispatch Refactor (§5g). Describes the file-watcher-driven completion flow that no longer exists. Delete as part of the cleanup sweep (no historical-note preamble left in its place — per the no-legacy rule).

**Out of scope (already verified correct):**

- `vendor/tracy/include/` stays unconditionally in include paths. `defines { "TRACY_ENABLE" }` is only added when `--tracy` is passed to premake, and no Tracy `.cpp` is compiled into the binary. Tracy macros are no-ops without the define. Ahmet's Docker memory-leak concern does not apply to default builds.

**Rollout order:**

1. Runtime folders (`queue/`, `workflows/`) — tiny change, no code impact.
2. Root-level cleanup (`.npm-tools/`, `jarvis_agent.example.env`) — tiny change.
3. Docker file relocation — coordinated with CI + launcher + Flatpak.
4. Source tree reorg — biggest diff, leave for last. One commit or a branch with multiple logical commits (move → premake5.lua update → doc sweep → CI filters).

Reference: this item captured after the 2026-04-18 MCP 1.0 commit landed +10,602 / -1,866 lines; the reorg will be bigger still in line-count but almost all `git mv` + path-string edits.

### 5c. Runtime-driven `ai_call` dispatch (Option E) — pulled into §5g (pre-1.0)

Absorbed into the AI dispatch refactor (§5g below). Original scope kept here for reference:

- Today `ai_call` completion relies on the `FileWatcher → FileAddedEvent → file categorizer → SessionManager → AiRequestPool` chain. That round-trip is elegant for the original file-drop use case (user drops PROB files into `queue/` manually) but increasingly awkward for runtime-authored workflows where the runtime already knows exactly what to dispatch.
- **Direction:** replace the file-watcher-mediated dispatch for runtime-initiated `ai_call` tasks with a direct path: `AiCallTaskExecutor` assembles the `SessionManager` environment, calls `AiRequestPool::Submit` directly, receives the response via callback, no synthetic file events. Keeps the file-watcher for the manual drop workflow only (or deletes it if the drop workflow is deprecated).
- Removes: the dependency on a dynamically-mutating `FileWatcher` watch set for adhoc (superseded by this refactor), the `m_PendingByOutputPath` passive-registration scheme, a full directory-scan tick every 100 ms.

### 5g. AI dispatch refactor (pre-1.0 — priority) — **landed on `refactor/ai-dispatch`**

**Core dispatch + legacy rip** (exampleMakefile4, aiZipDemo, jarvisCppDocu and ~15 other JCWFs live-validated on OpenAI + Anthropic):
- [x] Typed `AiInvocation` envelope (`application/workflow/aiInvocation.h`) + typed `AiReply` (`aiReply.h`).
- [x] `AiRequestPool::Submit(AiInvocation, callback)` — direct envelope dispatch through `CurlMultiDispatcher`.
- [x] `AiCallTaskExecutor` builds an envelope and calls `Submit` directly.  Fails the task immediately on Submit rejection (no legacy fallback).
- [x] Relaxed env rule: STNG / CNTX / TASK optional (warning-only); empty concatenated body still rejected; PROV sidecar is write-only (replay tooling only).
- [x] `IRequestBuilder` + `RequestBuilderAPI1/2/3/4`; `ReplyParser` base extended with `GetError`/`GetUsage`/`GetFinishReason`/`GetSystemFingerprint`/`GetStructuredOutput` virtuals, concretes implement; `ReplyParserAPI4` for Anthropic `/v1/messages`.
- [x] `CurlWrapper::AuthStyle::AnthropicXApiKey` (`x-api-key` + `anthropic-version: 2023-06-01`) wired in both `curlWrapper.cpp` (single-call) and `curlMultiDispatcher.cpp` (async).
- [x] `SchemaValidator` (simdjson-backed Draft 2020-12 subset: type, properties, required, additionalProperties, items, enum, min/max{,Length}, pattern, oneOf, anyOf, $ref, $defs). Unsupported keywords rejected at schema-load time.
- [x] `output_schema` + `output_retries` parsed on `ai_call`; Submit's reply path validates, retries with validator feedback up to budget, writes `<prob>.output.json` on structured success or `<prob>.output.txt` on free-text path.
- [x] Determinism defaults (`m_DeterminismTemperature`, `m_DeterminismSeed`, `m_DeterminismRecordSystemFingerprint` on `EngineConfig`).
- [x] `<prob>.transcript.json` per PROB — request + response turns with interface/model/settings/messages/usage/finish_reason.
- [x] `TestInterface` as new `InterfaceType::Test` — short-circuits curl, loads reply from fixture path (`m_Url`).
- [x] `EventCategoryAi` + `AiCallStartedEvent` / `AiCallCompletedEvent` / `AiCallFailedEvent` posted from Submit lifecycle.
- [x] `max_context_tokens` parsed on `ApiInterface`; `MarkdownSectionSplitter` + `ChunkPlanner` utilities live; advisory warning fires when prompt exceeds budget.
- [x] `AiRequestPool::GetDirectDispatchInflight` drives the dashboard "AI queries in flight" LED (via `/api/status` → `ai_calls_inflight`) and the `/api/debug/signals` counter.
- [x] MCP: new `reload_workflows` tool in `mcp/src/tools.ts` so JCWF edits on disk can be picked up without restart.
- [x] Editor-side migration: `AiJcwfService::RunSingleAiCall` (used by `GenerateAsync` / `ExplainAsync` / `FixFailedScriptAsync`) and `AssistantController::RunSingleAiCall` both switched to envelope + `std::promise`.  Dropped the `PROB_<id>_<ts>.txt` naming everywhere.
- [x] **Deleted** `SessionManager` class and the chat dispatch that rode on it (dormant — no UI consumer).  `ChatMessagePool` + `HandleChatPost` + WebSocket `type == "chat"` handler all gone.
- [x] **Deleted** `AiRequestPool::OnProbFileEvent`, `IsDirectDispatchActive`, `m_DirectDispatchActive`, `OnCurlDispatched`.  Submit calls `ActivateDeadlineForOutputPath` internally so the file-activity watchdog disarms correctly for workflow-bound envelopes.
- [x] `TriggerEngine` owns its own dedicated `FileWatcher` (empty primary root, paths added dynamically via `AddFileWatchTrigger` → `AddPath` and removed on `ClearAll` / `ClearWorkflowTriggers`).  `FileWatcher` now supports an empty primary root (skips initial scan + primary-gone-shutdown check).
- [x] `AdhocWorkflowManager` no longer takes a `FileWatcher*` — direct dispatch obsoletes the per-run-queue watch registrations.
- [x] Retired queue-folder `FileWatcher` in `JarvisAgent`.  `GetQueueFileWatcher()` and the PROB-handling/SessionManager-forwarding blocks in `OnEvent` removed.
- [x] Bonus fix — `MaterializeCntxFilesFromQueueBinding` now promotes path-reference CNTX files to inline with content populated so the envelope's user message actually includes them (this was a latent bug surfaced by jarvisCppDocu after the cutover).
- [x] Dashboard UI — `SessionManagersPanel` + `SessionStatus` deleted; StatusBar LED reads `ai_calls_inflight` from the status poll directly; `session_managers_*` status/debug fields dropped and replaced with `ai_calls_inflight`.
- [x] Deleted `application/workflow/doc/aiCallArchitecture.md`.
- [x] Docs sweep: architecture.md, api-endpoints.md, JC_Workflow_Specification.md §3.3.6, jcwf_generation_guide.md (embedded into the binary at build time), application/README.md, webServer.md, session/README.md + renamed fileWriter.md, file/README.md, logging.md, assistant/README.md — all scrubbed of SessionManager / ChatMessagePool / OnProbFileEvent references and "refactor" callouts.  `combinedDocumentation.md` is auto-generated by jarvisCppDocu — not hand-edited; regenerate on next run.

**Independent remaining features (carry into post-1.0 or follow-ups):**
- [ ] Chunking fan-out (emit N envelopes per oversized PROB, concat replies into one output). Planner + advisory already in.
- [ ] JCWF schema gap-close: close gaps in `doc/jcwf.schema.json` vs. `workflowJsonParser` + parser↔schema contract test. `tools/generateEmbeddedHeaders.py` prebuild + `kJcwfSchemaJson` already compiled into the binary.
- [ ] Wire `AiJcwfService::GenerateAsync` to set `AiInvocation.m_OutputSchemaJson = kJcwfSchemaJson` for schema-enforced JCWF generation with validator-error retry.
- [ ] TUI / dashboard consumers subscribing to `EventCategoryAi` (events are posted; consumed only by the aggregated "in flight" LED today).
- [ ] Contract tests under `test/dispatch/`.

**Known live-observed issues not yet fixed:**
- [ ] **60-second `WaitingExternal` timeout kills slow Claude calls** — observed 2026-04-20 with `ai-zip-demo` on Anthropic Sonnet 4.6: one of three concurrent ai_calls took ~65 s, the runtime gave up at 60 s, the real reply landed 5 s later and was orphaned. Fix: bump the default in `WorkflowRuntimeManager::TimeoutWaitingExternalTasks` and/or honour `AiInvocation.m_Timeout` (envelope already carries 120 s default). Easy follow-up.
- [ ] Claude Haiku 4.5 occasionally ignores STNG "no markdown fences" rules and wraps output in ` ```cpp … ``` `. Model-behaviour quirk. Mitigation: strengthen STNG wording in example JCWFs, or post-process to strip fences.

- HTTP/2 multiplexing + libcurl multi transport + disk-first philosophy + async completion model — all preserved.
- Out of scope: native LLM tool-calling (§5e — post-1.0), Claude Code PoC (§5f — post-1.0), additional cloud-native AI adapters (§5h — post-1.0 / pre-1.0 for enterprise).
- Dev-plan doc `AI dispatch refactor.md` has been mined into `doc/architecture.md` §"AI Dispatch Pipeline" and is safe to delete.
- Loose follow-up: `tools/replayTranscript.py` — nominally-planned dispatch debugging tool that reads a `<prob>.transcript.json` and re-emits the exact request body against the same provider, for reproducing drift. Not built; add when first real replay need comes up.

### 5h. Additional AI backend adapters (Bedrock + Azure OpenAI)

Both are real gaps for customers who can't bring OpenAI/Anthropic API keys through procurement but already have cloud contracts.  Both fit cleanly on top of the envelope architecture — new `InterfaceType` variant + per-provider `RequestBuilder` / `ReplyParser` + new `AuthStyle` on `CurlWrapper`.  Reply parsing is mostly trivial because Bedrock and Azure OpenAI both return OpenAI-compatible response bodies for chat-completion-shaped routes.

**5h.1 — AWS Bedrock adapter (new `InterfaceType::API5`).**  Post-1.0, pre-1.0 if an enterprise customer pulls.
- Endpoint: `POST https://bedrock-runtime.{region}.amazonaws.com/model/{modelId}/invoke`.  Routes to Claude / Llama / Titan / Nova depending on `modelId`.
- Auth: **SigV4 signed requests** (AWS access key + secret + region).  New `CurlWrapper::AuthStyle::AwsSigV4` that computes the signature from the request body + headers + timestamp.  Not a small piece — canonical request, string-to-sign, HMAC-SHA256 chain — but bounded (~300 lines, well-documented).
- Request body shape varies per underlying model family (Anthropic-inside-Bedrock ≠ Llama-inside-Bedrock).  `RequestBuilderAPI5` needs a per-family selector — cleanest done by sniffing `modelId` prefix (`anthropic.claude-*` vs `meta.llama*` vs `amazon.titan*`).
- Reply parser: Bedrock wraps each family's native response in its own envelope; unwrap and delegate to the matching existing family parser (e.g. for anthropic-inside-bedrock, reuse `ReplyParserAPI4` logic).
- Chunking / max_context_tokens: pick up automatically from the model-name fallback table (`claude*` → 200 K, `llama*` → 128 K, etc.).
- Target: post-1.0, bump to pre-1.0 when first enterprise customer asks.

**5h.2 — Azure OpenAI adapter.**  Post-1.0.
- Endpoint: `POST https://{resource}.openai.azure.com/openai/deployments/{deployment}/chat/completions?api-version={ver}`.
- Auth: **`api-key:` header** (not `Authorization: Bearer`).  New `CurlWrapper::AuthStyle::AzureApiKey`.
- Request body: identical to OpenAI Chat Completions — reuse `RequestBuilderAPI1`'s body, just override URL composition and auth style.  Either a new `InterfaceType` (say `API1Azure`) that shares the builder/parser with API1 but picks a different URL template and auth style, or a flag on the existing API1 config.
- Reply parser: reuse `ReplyParserAPI1` as-is.
- The `?api-version=` query parameter is deployment-specific; carry it in the interface config (`"api_version": "2024-08-01-preview"`).
- Smaller lift than Bedrock — ~100 lines.

Both land without touching the envelope, schema validator, chunking, reduce pass, transcripts, or event layer.  The envelope architecture was designed for exactly this kind of additive backend expansion.

### 5e. Native LLM tool-calling (post-1.0) — Assistant + JCWF `ai_call`

**Background.** Modern LLM APIs (OpenAI `tool_calls[]`, Gemini `functionCall`, Anthropic `tool_use` content blocks) support structured tool-calling natively: the request carries a list of tools (name + description + parameters JSON-schema); the model can choose to emit a structured call instead of text; the client executes the tool locally and feeds the result back for another turn. This is what agl and pydantic-ai are built around. The "AI Dispatch Refactor" (§5g) makes native tool-calling trivial to plumb in once the envelope is typed.

**5e.1 — AI Assistant: replace the `<tool_call>` regex parser with native tool-calling.** Target: post-1.0.
- Today `application/assistant/assistantTools.h:98` `ParseToolCalls()` regex-extracts `<tool_call>name(arg=value, ...)</tool_call>` tags from free-text replies. Hand-rolled, breaks when the model gets chatty inside the tags, escapes a quote wrong, or nests a tag inside a code fence.
- After AiInvocation lands: the assistant builds an invocation with `m_Tools = [...]` (one per registered `ToolDef`); providers return native tool calls via `ReplyParser::GetToolCalls()`; the assistant controller loops exactly as today, but from real structured fields instead of regex. Tag parser can stay as a fallback for interfaces that don't expose tools.
- Scope: extend `ReplyParser` with `GetToolCalls()`, add tool declarations to request builders (API1/API2/API3/API4), swap `AssistantController` over to structured calls. L3 approval flow (`requiresApproval`) unchanged.

**5e.2 — Post-1.0: bring tool-calling to JCWF `ai_call` tasks.** Pairs with §5g (AI dispatch refactor).
- JCWF `ai_call` gains optional `tools: [...]`. Each tool has `{name, description, parameters_schema, handler_task}` where `handler_task` is a normal JCWF task (python/shell/internal).
- When the model emits a tool call, `AiCallTaskExecutor` enqueues the handler task synchronously with the tool arguments, awaits its `.output.txt`, appends a `ToolReturn` message to the invocation, and re-dispatches — same loop pattern as the Assistant, inside one `ai_call` task.
- Most existing example JCWFs don't benefit (explicit DAGs already make the decisions the author wants). Real candidates: `slackQAndABot` (classic tool-using Q&A bot), `redmineTriageBot` / `gitHubIssueDemo` / `jiraIssueDemo` (triage decisions that benefit from looking up related items mid-call), `hamburg-tourist-day-planner` (live lookups).
- New design work: bounded `max_tool_turns`, deterministic-replay story (record-and-replay of tool choices for tests), interaction with JCWF freshness model (an `ai_call` that took tool paths A→B today vs A→C tomorrow), transcript format extension to capture tool turns.
- Target: post-1.0. Track in `application/workflow/doc/todo.md` under "future refactors" when it's time to start.

### 5f. j9t as orchestrator of other AI tools — Claude Code PoC

**Background.** Today, `ai_call` tasks hit an LLM chat/completions endpoint and get text back. The LLM is a *reasoning primitive*. But there's a richer class of backends out there — **coding agents** like Claude Code, Cursor Agent, Aider, OpenAI Codex — that are themselves orchestrators: they read files, edit code, run shell commands, iterate until a goal is met. Wiring j9t to call into these tools turns JarvisAgent from "workflow engine that calls LLMs" into "workflow engine that orchestrates *other AI orchestrators*." That's a meaningful expansion of what a j9t workflow can do in one step.

**PoC scope — Claude Code as an AI backend.**
- Claude Code has a headless mode: `claude -p "<prompt>"` runs non-interactively and prints the response to stdout. Permission mode flags (`--permission-mode=plan` / `acceptEdits` / `default`) control whether it's allowed to modify files. Working directory is whatever the process starts in.
- Auth is piggybacked on the user's existing Claude subscription (Claude Max / Pro), via the CLI's login state. **No API key management needed from j9t** — the user authenticates `claude` once, j9t just spawns it. Nice ergonomic win.
- Proposed integration path: new `InterfaceType::ClaudeCode` ("API5") handled differently from HTTP-based interfaces. Instead of building an HTTP body, the `AiCallTaskExecutor` path writes STNG/CNTX/TASK/PROB as today, concatenates them into a single prompt, spawns `claude -p "<prompt>"` in the task's working directory, captures stdout, wraps it in an `AiReply`. No `CurlMultiDispatcher` involvement — the process is the transport.
- Lives behind the same `AiInvocation` envelope as HTTP interfaces; a workflow author just selects `api_interface: claude-code` on the task. Rest of the workflow is unchanged.

**Risks / open questions to settle during the PoC.**
- Concurrency: `claude` is a heavy process. Spawning 60 in parallel for a CSV fan-out workflow would not be fine. Cap concurrent Claude Code instances per workflow run (new config: `max_concurrent_claude_code`).
- Permission safety: default to `--permission-mode=plan` (read-only) unless the task explicitly opts into mutation. Engine edition should probably refuse mutation mode entirely.
- Determinism: Claude Code is even more non-deterministic than a raw LLM call (it makes multiple internal tool choices). No `seed`, no `system_fingerprint`. The `.transcript.json` from the dispatch refactor captures the top-level prompt/response; what the Claude Code session does internally is opaque to us. That's fine for a PoC, limiting for production.
- Cost: Claude Max / Pro usage, not per-API-token. Different billing model than API1/API4.
- Cross-platform: `claude` CLI availability on Linux/macOS/Windows varies. May need a config flag to point at a specific binary path.

**Longer term — other candidates beyond Claude Code.**
- **j9t → j9t** (cross-instance orchestration) via the existing MCP sidecar — already possible, just hasn't been wired as a PoC.
- **Cursor Agent** (headless mode, if/when exposed) — similar shape to Claude Code.
- **Aider** — open-source, scriptable, git-aware. Good natural fit since j9t is git-friendly.
- **OpenAI Codex CLI** — same shape; different auth.

**Target: post-1.0, after the dispatch refactor lands.** The `InterfaceType` seam we're building in the refactor is what makes this PoC cheap — add a new variant + a new `IRequestBuilder`-equivalent (or a parallel `IAiTransport` abstraction since the transport isn't HTTP) + a new reply parser that just wraps stdout text. Most of the machinery (queue files, envelope, retry, transcript) is reused.

Claude's joke-but-not-a-joke tagline for this one: *"j9t orchestrates the orchestrators."*

### 6. Landing page for new users (pre-1.0)
- Create a welcoming landing page / website for JarvisAgent
- Should explain what JarvisAgent is, key features, screenshots, and download links
- Target audience: first-time visitors who discover the project

### 7. Promotion video (pre-1.0)
- Create a demo / promotion video showcasing JarvisAgent
- Cover: workflow creation in the editor, running workflows, dashboard monitoring, multi-platform support
- Target: GitHub README embed, YouTube, social media

### ~~8. Enable HTTP/2 for AI provider requests~~ ✅
- ~~Enable HTTP/2 in `CurlWrapper` for improved network performance when communicating with AI provider APIs~~ ✅
- Phase 1: vendored nghttp2, set `CURLOPT_HTTP_VERSION_2TLS` in `CurlWrapper::Query()` — all AI requests now negotiate HTTP/2 via ALPN, log line confirms "HTTP/2 (HTTP 200)" per query.
- Phase 2: `CurlMultiDispatcher` — dedicated I/O thread with `curl_multi` + `CURLPIPE_MULTIPLEX`; all concurrent requests to the same host share one TCP/TLS connection; zero thread-pool threads blocked on network I/O. `SessionManager` converted from futures-based dispatch to async callback via `Submit(data, callback)`.
- Verified: log shows HTTP/2 for all queries; burst of 10+ completions within 1 second confirms multiplexing is active.

---

### ~~10. Fix JCWF examples for first install + fix currently broken JCWFs~~ ✅

#### ~~Part A — Clean first-install experience~~ ✅
- Set `"enabled": false` on auto trigger for all AI-requiring workflows: `aiCarMaintenancePipeline`, `aiZipDemo`, `vehicleTroubleshootingGuide` — `manual` trigger retained so they run once configured.
- `make-example` auto trigger left enabled (no AI required, always green).
- `vehicleTroubleshootingGuide` removed from all packaging (DEB, RPM, Flatpak, macOS DMG, Homebrew, Windows ZIP) — too fragile (Chrome/markitdown dependency, low demo value).

#### ~~Part B — Fix currently broken example workflows~~ ✅
- `aiCarMaintenancePipeline` — auto trigger disabled (was firing without API keys). ✅
- `bookSummaryPipeline` / `portfolioDividendAnalysis` — stale failure badges from prior session; Python scripts already use `**kwargs`. Cleared on next run. ✅
- `vehicleTroubleshootingGuide` — removed from packaging entirely. ✅
- `make-example` — root cause was `taskPathResolver.cpp` not implementing spec §3.3 ("omit `working_directory` → default to Workflow Base Directory"). Fixed in C++: `ResolveTaskWorkingDirectoryPath()` now falls back to `workflowBaseDirectoryPath` when the resolved path is empty. `working_directory: ""` lines removed from JCWF (field omitted). ✅
- Python `**kwargs` fixes: `scripts/printFileInfo.py` and `scripts/combineEngineTroubleshootingGuide.py` updated to accept `**kwargs` (PythonEngine injects `context` kwarg on every call). ✅
- All packaged workflows verified: full green dashboard. ✅

---

### ~~11. Add Windows native scripting in PowerShell for the JC workflow engine and for the AI assistant~~ ✅
- ~~Currently all shell execution (workflow engine `ShellTaskExecutor` and AI assistant tools) assumes bash (MSYS2 / Git Bash) on Windows~~ ✅
- ~~Add native PowerShell support: detect `.ps1` scripts and route through `powershell -ExecutionPolicy Bypass -File ...`~~ ✅
- ~~Workflow engine: update `ExecuteCommandWithCapturedOutput` to dispatch by script extension (`.sh` → bash, `.ps1` → PowerShell)~~ ✅
- ~~Workflow validator: accept PowerShell scripts (skip bash shebang check for `.ps1`)~~ ✅
- ~~Argument quoting: add `QuoteForPowerShell` function (single-quote wrap, inner `'` → `''`)~~ ✅
- ~~AI assistant `run_shell`: detect platform and use PowerShell as the default shell on Windows when bash is not available~~ ✅
- `use_bash` config toggle: PowerShell default, `use_bash: true` probes PATH for bash and falls back to PowerShell if not found ✅
- `.sh` → `.ps1` sibling resolution: executor looks for `.ps1` sibling before executing, fails with clear error if missing ✅
- AI JCWF pipeline (aiJcwfService): strengthened Windows OS detection, added `.ps1` generation/validation/fix/review rules ✅
- Added `compile.ps1`, `archive.ps1`, `link.ps1`, `run.ps1` PowerShell siblings for the make-example workflow ✅
- Settings UI: `use_bash` checkbox (Windows-only, gated on `platform` field from config API) ✅

**Follow-up:** ~~`run_shell` timeout on Windows — currently runs to completion with no kill mechanism. Need `CreateProcess()` + `WaitForSingleObject(handle, 30000)` + `TerminateProcess()` on timeout, matching the 30s Linux/macOS watchdog.~~ ✅ Implemented: `CreateProcess` + reader thread + `WaitForSingleObject(30s)` + `TerminateProcess` on expiry.

---

### 9. Dual-edition architecture — **j9t Engine** vs **j9t Studio**

#### Motivation (cybersecurity)

JarvisAgent is gaining powerful developer-facing features: a visual workflow
editor, AI-powered JCWF generation that writes scripts to disk, and an upcoming
AI assistant with persistent memory, file system access, and shell command
execution.  These capabilities are essential for a **development workstation**
but represent a significant attack surface on a **production server** whose only
job is to orchestrate and execute pre-defined workflows.

Shipping a single binary forces production operators to accept IDE-grade
privileges they never need. Two compile-time editions solve this cleanly.

#### Naming

| Edition | Binary name | Purpose |
|---------|-------------|---------|
| **j9t Engine** | `jarvisAgent` | Lean orchestration server. Read-only workflows, run monitoring, webhook triggers. No editing, no AI assistant, no script generation. |
| **j9t Studio** | `jarvisAgent` | Full developer IDE. Everything in Engine **plus** workflow editor, AI JCWF generation/explain/fix, AI assistant, config editing from browser, script writing. |

Same binary name, different compile-time feature set. Packaging can
distinguish them (e.g. `jarvisagent` vs `jarvisagent-studio` DEB packages,
or a single package with a build flag).

#### Feature matrix

| Feature / Subsystem | Engine | Studio | Security rationale |
|---------------------|:------:|:------:|-------------------|
| **Workflow execution** (RuntimeManager, task executors) | ✅ | ✅ | Core function of both |
| **Workflow loading & validation** (WorkflowRegistry) | ✅ | ✅ | Needed to run workflows |
| **Trigger engine** (auto, cron, file_watch, webhook) | ✅ | ✅ | Production triggers |
| **AI provider integration** (SessionManager, AiRequestPool) | ✅ | ✅ | Powers ai_call tasks in workflows |
| **Python engine** (embedded CPython for python tasks) | ✅ | ✅ | Powers python tasks in workflows |
| **REST API — status** (`GET /api/status`) | ✅ | ✅ | Health check |
| **REST API — workflow list** (`GET /api/workflows`) | ✅ | ✅ | Read-only listing |
| **REST API — run control** (run, cancel, pause, resume, stop) | ✅ | ✅ | Operate pre-loaded workflows |
| **REST API — run monitoring** (active runs, last runs, run detail) | ✅ | ✅ | Observe execution |
| **Webhook endpoint** (`POST /api/webhook/:id`) | ✅ | ✅ | External trigger integration |
| **n8n integration** (`POST /api/integrations/n8n/start`) | ✅ | ✅ | External trigger integration |
| **Task heartbeat** (`POST /api/task/:id/heartbeat`) | ✅ | ✅ | Watchdog keep-alive |
| **WebSocket — run snapshots & log streaming** (`/ws`) | ✅ | ✅ | Real-time monitoring |
| **Dashboard UI** (React read-only monitoring) | ✅ | ✅ | Observe system state |
| **TUI** (ncurses terminal) | ✅ | ✅ | Server console |
| **Key management** (status, unlock) | ✅ | ✅ | Needed to decrypt API keys for workflow execution |
| **Provider list** (`GET /api/settings/providers`) | ✅ | ✅ | Read-only provider info |
| **Log viewer** (`GET /api/log`, analyze-last-run) | ✅ | ✅ | Debugging production runs |
| **Shutdown** (`POST /api/shutdown`) | ✅ | ✅ | Graceful server stop |
| | | | |
| **Workflow Editor UI** (React visual DAG editor) | ❌ | ✅ | Allows creating/modifying workflows — dev-only |
| **Workflow CRUD** (`POST/PUT/DELETE /api/workflows`) | ❌ | ✅ | Mutates workflow definitions on disk |
| **Workflow reload** (`POST /api/workflows/reload`) | ❌ | ✅ | Re-scans workflow directory |
| **Workflow versioning** (list/get/restore versions) | ❌ | ✅ | Version management is an editing feature |
| **AI JCWF generation** (generate, explain, fix-script via WS) | ❌ | ✅ | AI writes JCWF + scripts to disk |
| **Script writing** (`ai-write-scripts` WS handler) | ❌ | ✅ | Writes executable files to disk |
| **Script check / registry** (`GET /api/scripts/*`) | ❌ | ✅ | Editor support tool |
| **File check** (`GET /api/files/check`) | ❌ | ✅ | Editor support tool |
| **AI interface CRUD** (`POST/PUT/DELETE /api/settings/ai-interfaces`) | ❌ | ✅ | Mutates config.json |
| **AI interface test** (`POST /api/settings/ai-interfaces/test`) | ❌ | ✅ | Dev testing tool |
| **Config editing** (`PUT /api/settings/config`) | ❌ | ✅ | Mutates config.json from browser |
| **Config reload** (`POST /api/settings/config/reload`) | ❌ | ✅ | Triggers config re-read |
| **Provider CRUD** (`POST/PUT/DELETE /api/settings/providers`) | ❌ | ✅ | Mutates provider config |
| **Provider set-default** (`POST /api/settings/providers/:name/default`) | ❌ | ✅ | Mutates config |
| **AI Assistant** (planned — L1/L2/L3) | ❌ | ✅ | File reading, code search, shell execution, persistent memory |
| **Chat POST** (`POST /api/chat`) | ❌ | ✅ | Interactive chat (writes PROB files) |

#### Backend control — C++ preprocessor define `J9T_STUDIO`

A single define gates all Studio-only code:

```cpp
// In webServer.cpp RegisterRoutes():
#ifdef J9T_STUDIO
    // Workflow CRUD (create, update, delete)
    CROW_ROUTE(m_Server, "/api/workflows")
        .methods("POST"_method)([this](crow::request const& req) { ... });
    // ... PUT, DELETE, versions, validate, reload ...

    // AI interface + config + provider management
    // Script check / file check
    // Workflow Editor UI serving (/editor, /assets)
#endif

// In webServer.cpp RegisterWebSocket() onmessage:
#ifdef J9T_STUDIO
    else if (type == "ai-explain-jcwf") { ... }
    else if (type == "ai-generate-jcwf") { ... }
    else if (type == "ai-write-scripts") { ... }
    else if (type == "ai-fix-failed-script") { ... }
    else if (type == "chat") { ... }
#endif

// In webServer.h:
#ifdef J9T_STUDIO
    AiJcwfService m_AiJcwfService;
#endif
```

**premake5.lua** — add a `--studio` option:

```lua
newoption {
    trigger     = "studio",
    description = "Build j9t Studio edition (workflow editor + AI assistant)"
}

filter "options:studio"
    defines { "J9T_STUDIO" }
```

Build commands:
- `premake5 gmake` → Engine (default, production-safe)
- `premake5 gmake --studio` → Studio (full IDE)
- `make config=release` works for both — the define is baked into the generated Makefile

Engine-excluded source files (`assistantController.cpp`, `aiJcwfService.cpp`
method bodies) should still compile but have their entry points `#ifdef`-guarded
so the linker doesn't pull in unused code. Alternatively, wrap entire files:

```cpp
// assistantController.cpp
#ifdef J9T_STUDIO
// ... full implementation ...
#endif
```

#### Frontend control

The frontend is **less security-critical** because all dangerous operations
(file writes, shell execution, config mutation) happen on the backend. The
frontend merely presents UI and relays user intent. If the backend rejects
the request (Engine edition), the frontend button does nothing.

**Recommended approach: two shipped UI bundles**

| Edition | Shipped UIs | Notes |
|---------|-------------|-------|
| Engine | `dashboard/ui/dist` only | Monitoring dashboard; no `/editor` route |
| Studio | `dashboard/ui/dist` + `workflow-editor/ui/dist` | Full editor + dashboard |

The workflow-editor React app does **not** need a compile-time split. It simply
isn't shipped in Engine builds. The `build-ppa.sh` / CI scripts skip copying
`workflow-editor/ui/dist` for Engine packages.

If a single frontend build is preferred (simpler CI), use a Vite env var:

```bash
VITE_J9T_EDITION=engine npx vite build   # hides editor tabs
VITE_J9T_EDITION=studio npx vite build   # full UI (default)
```

In `App.tsx`:
```tsx
{import.meta.env.VITE_J9T_EDITION !== 'engine' && (
    <Route path="/editor/*" element={<WorkflowEditorView />} />
)}
```

But since the backend already won't serve `/editor` in Engine mode, this is
defense-in-depth, not strictly required.

#### Packaging

| Package | Edition | Contents |
|---------|---------|----------|
| `jarvisagent` (DEB/RPM/Arch) | Engine | Binary without `J9T_STUDIO`, dashboard UI only |
| `jarvisagent-studio` (DEB/RPM/Arch) | Studio | Binary with `J9T_STUDIO`, dashboard + editor UIs |
| Docker `ghcr.io/beaumanvienna/jarvisagent` | Engine | Production container |
| Docker `ghcr.io/beaumanvienna/jarvisagent-studio` | Studio | Development container |
| macOS DMG / Windows MSI | Studio | Desktop users are developers |
| AppImage / Flatpak | Studio | Desktop users are developers |

#### Implementation steps

- [x] Add `--studio` option to `premake5.lua`, set `J9T_STUDIO` define
- [x] Wrap Studio-only routes in `#ifdef J9T_STUDIO` in `webServer.cpp` (via `RegisterStudioRoutes()`)
- [x] Wrap Studio-only WS handlers in `#ifdef J9T_STUDIO` in `webServer.cpp`
- [x] Wrap `AiJcwfService` member + `AssistantController` member in `#ifdef J9T_STUDIO` in `webServer.h`
- [x] Wrap `m_AiJcwfService.Shutdown()` in `SignalStop()` with `#ifdef J9T_STUDIO`
- [x] Guard `/editor` + `/assets` static file serving with `#ifdef J9T_STUDIO`
- [x] Guard `POST /api/chat` with `#ifdef J9T_STUDIO`
- [x] Add `edition` + `capabilities` fields to `GET /api/status`
- [x] Both editions compile cleanly (Engine 11.6 MB, Studio 12.0 MB)
- [x] Update `build-ppa.sh` to support `--edition engine|studio`
- [x] Update CI workflows to build both editions (Linux: Engine+Studio, macOS: Engine+Studio→DMG, Windows: Engine→ZIP + Studio→MSI)
- [x] ~~When AI assistant is implemented, all assistant modules are `#ifdef J9T_STUDIO`~~ — `AssistantController` member + `ShutdownAssistantController()` + `/ws/assistant` route all guarded
- [x] Update README with edition descriptions
- [x] Frontend: dashboard reads `edition`+`capabilities` from `/api/status`; hides Workflow Editor link and Run buttons in Engine mode; `fetchKeysStatus` gracefully handles Engine 404

---

### ~~12. Security audit logging~~ ✅

Dedicated security log for all auth-related events. `Security` spdlog logger writing to `log/security.txt` (rotating, 10 MB x 5 files) + ostream sink for TUI/console. `LOG_SECURITY_INFO`/`LOG_SECURITY_WARN` macros in `engine.h`. Logged events: auth success/failure (IP, endpoint, reason), rate limit triggered, webhook accepted/rejected (IP, workflowId, reason), shutdown requested, run control actions (cancel/pause/resume/stop with runId). All events include IP address and timestamp.

### ~~13. Built-in TLS (HTTPS)~~ ✅

`CROW_ENABLE_SSL` defined in `premake5.lua` (OpenSSL already linked). `TlsCert`/`TlsKey` fields in `EngineConfig` + `configParser.cpp`. When both set and files exist, `m_Server.ssl_file(cert, key)` serves HTTPS on port 8443. Missing/invalid files refuse to start. `GET /api/status` includes `"tls": true/false`. Both editions supported.

### ~~14. Token expiration and rotation~~ ✅ (superseded — see item 5)

Historical: the former `engine_api_token.txt` admin bearer token had a 90-day expiry + auto-rotation cycle. Superseded by the MCP API key system (item 5 above). The entire `engine_api_token.txt` code path has been removed; key expiry is now a per-user property of each MCP key with self-renewal via `POST /api/auth/mcp-keys/self-renew`.

### ~~15. Failed auth lockout~~ ✅

`AuthFailureRecord` struct + `m_AuthFailures` map (IP → {count, first_failure}), guarded by `m_RateLimitMutex`. After 10 failed auth attempts within 5 minutes, IP locked out for 15 minutes (403 + `Retry-After: 900`). Lockout checked before rate limiting. Successful auth clears failure count. Cleanup piggybacks on rate limit cleanup cycle. Lockout events logged to security log.

### ~~16. Enterprise security hardening~~ ✅

**Security headers:** `SetSecurityHeaders` adds CSP, X-Frame-Options (DENY), X-Content-Type-Options, Referrer-Policy, Permissions-Policy to all responses. HSTS added when TLS enabled.

**Request body size limit:** `MaxRequestBodyMB` config field (default 10 MB). Oversized requests rejected with 413 before parsing. Applied to webhook and n8n endpoints.

**Gateway-trusted identity headers:** `TrustedProxyHeader` / `TrustedRoleHeader` config fields. When set, `Authenticate()` trusts gateway-injected user/role headers. Falls back to bearer token. Security log includes user identity and auth method.

**RBAC:** Three roles (admin > operator > viewer). Admin-only: shutdown, security log. Operator+: run control, app log. Viewer+: workflow list, run monitoring. Bearer token grants admin (backward compatible). Gateway mode defaults to viewer (least privilege). `insufficient_role` → 403.

**Documentation:** `doc/cyber security.md` updated with 11 new abbreviations (RBAC, MFA, WAF, OIDC, JWT, etc.), recommended deployment architecture (Internet → WAF → API Gateway → j9t), explicit "Direct Public Internet Exposure — Not Supported" section, multi-tenant guidance.

