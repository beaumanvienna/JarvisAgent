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

## ~~12. Browser-based AI chat terminal~~ (moved to Remaining TODOs #2)

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

### 1. Python Engine parallelization
- Add support for multiple independent PythonEngine instances
- Ensure each interpreter instance owns its own GIL
- Store PythonEngine instances in std::vector
- Default engine count: 4
- Allow override via config.json
- Expose internal task-queue size for load balancing
- Dispatch OnEvent() to the PythonEngine with the lowest queued workload
- Ensure isolated interpreter state per engine
- **Complexity note:** CPython sub-interpreters + per-interpreter GIL (PEP 684, Python 3.12+) is the cleanest path but has restrictions on C extension modules. Alternative: multiprocessing with IPC.

### 2. Browser-based AI chat terminal
- **Goal:** An AI-powered chat terminal in the browser — like the Cascade terminal in Windsurf.
- **Frontend:** xterm.js terminal emulator on a dedicated page/tab, connected via WebSocket
- **Backend:** Chat message router in C++ (WS protocol), AI provider integration, context injection, command fallback (`/run`, `/status`, `/help`)

### 3. Sub-workflows / workflow-call node
- Invoke one JCWF from another as a task
- Enables modular composition of complex pipelines

### 4. Launchpad PPA
- Upload source-code DEB package to Launchpad PPA: https://launchpad.net/~beauman/+archive/ubuntu/marley
- Test end-to-end: `sudo add-apt-repository ppa:beauman/marley && sudo apt install jarvisagent`

### 5. Landing page for new users
- Create a welcoming landing page / website for JarvisAgent
- Should explain what JarvisAgent is, key features, screenshots, and download links
- Target audience: first-time visitors who discover the project

### 6. Self-hosted Docker registry
- Evaluate hosting our own server for the Docker package instead of relying solely on GHCR
- Benefits: custom domain, no GitHub dependency, potential for private images
- Alternatives: self-hosted Docker registry, Harbor, or a simple VPS with registry

### 7. Promotion video
- Create a demo / promotion video showcasing JarvisAgent
- Cover: workflow creation in the editor, running workflows, dashboard monitoring, multi-platform support
- Target: GitHub README embed, YouTube, social media

### 8. Enable HTTP/2 for AI provider requests
- Enable HTTP/2 in `CurlWrapper` for improved network performance when communicating with AI provider APIs
- libcurl supports HTTP/2 via `CURLOPT_HTTP_VERSION` / `CURL_HTTP_VERSION_2TLS`
- Requires OpenSSL with ALPN support (already vendored)

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

- [ ] Add `--studio` option to `premake5.lua`, set `J9T_STUDIO` define
- [ ] Wrap Studio-only routes in `#ifdef J9T_STUDIO` in `webServer.cpp`
- [ ] Wrap Studio-only WS handlers in `#ifdef J9T_STUDIO` in `webServer.cpp`
- [ ] Wrap `AiJcwfService` member in `#ifdef J9T_STUDIO` in `webServer.h`
- [ ] Wrap `m_AiJcwfService.Shutdown()` in `SignalStop()` with `#ifdef J9T_STUDIO`
- [ ] Guard `/editor` + `/assets` static file serving with `#ifdef J9T_STUDIO`
- [ ] Guard `POST /api/chat` WS handler with `#ifdef J9T_STUDIO`
- [ ] Update `build-ppa.sh` to support `--edition engine|studio`
- [ ] Update CI workflows to build both editions
- [ ] When AI assistant is implemented, all assistant modules are `#ifdef J9T_STUDIO`
- [ ] Update README with edition descriptions

---

