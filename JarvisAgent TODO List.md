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

## 6. Python Engine parallelization (new)
- Add support for multiple independent PythonEngine instances
- Ensure each interpreter instance owns its own GIL
- Store PythonEngine instances in std::vector
- Default engine count: 4
- Allow override via config.json
- Expose internal task-queue size for load balancing
- Dispatch OnEvent() to the PythonEngine with the lowest queued workload
- Ensure isolated interpreter state per engine
- **Complexity note:** CPython sub-interpreters + per-interpreter GIL (PEP 684, Python 3.12+) is the cleanest path but has restrictions on C extension modules. Alternative: multiprocessing with IPC.

---

## 7. Multi-user support for system-wide installs (new)
- When installed system-wide via deb/rpm/Arch (`/opt/jarvisagent/`), `queue/` and `workflows/` are owned by root
- Non-root users cannot write to these directories without `sudo`

**Proposed solution: user-space launcher script**

A wrapper script (installed to `/usr/bin/jarvisagent` on Linux, available on PATH via MSI on Windows) that:
1. Creates a per-user working directory on first run:
   - **Linux/macOS:** `~/JarvisAgent` (default), overridable via CLI argument or env var
   - **Windows:** `%USERPROFILE%\JarvisAgent` (default)
2. Copies/symlinks the required folder structure from the install location (`/opt/jarvisagent/` or `C:\Program Files\JarvisAgent\`):
   - Read-only assets (binary, dashboard, workflow-editor, doc, scripts) → **symlink** to install dir
   - Writable user data (queue, workflows, log, config.json) → **copy templates** or **create empty dirs**
3. Activates the Python venv (so `markitdown`, `md2pdf` are on PATH)
4. Starts jarvisAgent in the terminal (CWD = user directory)
5. Opens the dashboard (`http://localhost:8080`) in the default browser

This is already how AppImage and Flatpak work (via `AppRun` / `jarvisagent-wrapper.sh`).
The same pattern should be adopted for deb/rpm/Arch/MSI installs.

- `md2pdf` (from the Python venv at `/opt/jarvisagent/.venv`) is not on PATH by default — the launcher or shell scripts need to activate the venv or add it to PATH so workflows like `vehicleTroubleshootingGuide` can find it

---

## ~~8. Workflow editor improvements + AI assistance~~ ✅
- ~~**AI → JCWF:** User describes a workflow in natural language (prompt), AI generates a valid `.jcwf` file~~ ✅ Multi-stage pipeline in `AiJcwfService`: decompose → generate JCWF (batched fan-out) → generate scripts → review → validate → fix. Supports both **Python** and **shell (bash)** script generation with POSIX awk rules, host OS detection, and positional arg mapping. Validator uses `WorkflowFileIndex` to suggest file path corrections. E2E verified with `cyber2` (Python) and `cyber3` (shell) workflows. See `example/workflows/cyber2_e2e.md` and `cyber3_e2e.md`.
- ~~**Fix Script button:** Runtime script fix — reads failed script + stderr, sends to AI, user reviews fix in `ScriptReviewPanel`~~ ✅ `FixFailedScriptAsync()` in `aiJcwfService.cpp`, WS handler in `webServer.cpp`.
- ~~**JCWF → AI:** User loads an existing `.jcwf` file, AI generates a human-readable summary/documentation~~ ✅
- ~~Integration point: workflow editor UI sends prompt to backend, backend calls AI provider, returns structured JCWF JSON~~ ✅
- ~~Validation: generated JCWF should pass `workflowValidator` before being offered to the user~~ ✅ Validator runs after generation; warnings trigger a fix stage with AI auto-correction. Script content validation (shebang, metadata, pipefail) added for on-disk scripts.
- ~~UX: "Generate from prompt" button and "Explain this workflow" button in the editor~~ ✅

---

## 9. Webhook trigger type (future)
- Currently, external systems can already trigger workflows via `POST /api/workflows/<id>/run` and `POST /api/integrations/n8n/start`
- A dedicated `"type": "webhook"` trigger would add:
  - Per-workflow webhook secrets (HMAC signature verification)
  - Cleaner URLs like `/api/webhook/<workflowId>`
  - Declarative intent in the JCWF ("this workflow is designed to be called externally")
- Not a blocker — existing REST API covers the functionality; this is a polish/security item for v0.9+

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

## 12. Browser-based AI chat terminal (new)
- **Goal:** An AI-powered chat terminal in the browser — like the Cascade terminal in Windsurf.
  Not just a command prompt, but a conversational AI interface that can:
  - Answer questions about the system, workflows, and task outputs
  - Trigger and monitor workflow runs
  - Inspect task state, logs, and captured output
  - Suggest fixes for failed tasks
- **Frontend:** xterm.js terminal emulator on a dedicated page/tab, connected via WebSocket (Crow already in stack)
- **Backend:**
  - Chat message router in C++ (WS protocol: `{ "type": "chat" | "command" | "complete", ... }`)
  - AI provider integration for natural language understanding
  - Context injection: system state, active runs, task history
  - Command fallback: direct commands (`/run`, `/status`, `/help`) for non-AI actions

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

## 15. Landing page for new users (new)
- Create a welcoming landing page / website for JarvisAgent
- Should explain what JarvisAgent is, key features, screenshots, and download links
- Target audience: first-time visitors who discover the project

---

## 16. Self-hosted Docker registry (new)
- Evaluate hosting our own server for the Docker package instead of relying solely on GHCR
- Benefits: custom domain, no GitHub dependency, potential for private images
- Alternatives: self-hosted Docker registry, Harbor, or a simple VPS with registry

---

## 17. Promotion video (new)
- Create a demo / promotion video showcasing JarvisAgent
- Cover: workflow creation in the editor, running workflows, dashboard monitoring, multi-platform support
- Target: GitHub README embed, YouTube, social media

---

## 18. Launchpad PPA (new)
- Upload source-code DEB package to Launchpad PPA: https://launchpad.net/~beauman/+archive/ubuntu/marley
- Test end-to-end: `sudo add-apt-repository ppa:beauman/marley && sudo apt install jarvisagent`

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



## 20. AI test button (new)
- Add a test button in AI manager of workflow-ediotr react app
- button should send "say hello world, in Python" or something similar
- visually confirm if the AI is online or not (red/green LED after test before test LED is in off state)

---

## 21. Settings dialog for workflow editor + JCWF assistant provider config
- Add a settings dialog in the workflow editor for `config.json` fields that are
  **not** already covered by the AI Manager page (e.g. queue paths, misc. options).
- Add a setting to select which **API interface index** the JCWF generation assistant
  uses (currently it inherits the default provider from `config.json`).
  - This will write a PROV file for the `_ai_jcwf_service` queue so `SessionManager`
    picks up the override (the PROV plumbing in `AiJcwfService` was removed for now
    because it wasn't needed — it will need to be re-added).
  - An advanced model (e.g. Claude Opus Thinking) may be required for complex JCWF
    generation, so letting the user choose a different provider than the default is
    important.

---

