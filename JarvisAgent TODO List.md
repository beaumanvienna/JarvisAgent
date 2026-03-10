# JarvisAgent TODO List

This list tracks **general project TODOs and high-level features** for JarvisAgent.

See also:
- `application/workflow/doc/todo.md` — C++ backend TODOs (workflow engine, runtime manager, task executors)
- `workflow-editor/todo.md` — Frontend TODOs (React workflow editor UI)

---

## 1. GitHub CI and cross-platform testing
- ~~Linux, macOS, and Windows workflows are green~~ ✅
- ~~Fix smoke test segfault (TTY / ncurses / config path)~~ ✅ graceful exit when config.json missing; Core destructor restores cout/cerr rdbuf
- Run on actual macOS — testing in progress (miniMac, macOS Tahoe)
- Run on actual Windows

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

## 3. Terminal UI (new)
- ~~PDCurses on macOS: backend VT is configured, needs to be tested~~ ✅ tested on miniMac (macOS Tahoe) — log + status windows, green theme
- PDCurses on Windows: backend Wincon is configured, needs to be tested

---

## 4. Workflow system
- ~~Manual trigger via browser-based terminal prompt~~ — dropped; workflow editor UI covers trigger/run/status
- ~~Workflows for individual lines from spreadsheets~~ — done: `csv` and `text_lines` per-item filters + `portfolioDividendAnalysis` and `goKartComplianceCheck` demo workflows

---

## 5. Native Google Gemini reply parser (new)
- Currently using the OpenAI-compatible legacy endpoint (`generativelanguage.googleapis.com/v1beta/openai/chat/completions`)
- This legacy format works but may lack access to Gemini-specific features (grounding, safety settings, function calling, etc.)
- Implement a dedicated reply parser for the native Gemini API (`generativelanguage.googleapis.com/v1beta/models/...`)
- This would be a new `InterfaceType` (e.g., `API3` / `GeminiNative`)

---

## 6. README.md update (new)
- Update "Planned Features" section: Docker is done, remove WIP label
- Add missing feature highlights: encrypted API keys, per-item filters, task watchdog, dataflow/template engine, dashboard features
- Update multi-model list to include Google Gemini
- Fix any outdated information

---

## 7. Python Engine parallelization (new)
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

## 8. Multi-user support for system-wide installs (new)
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

## 9. Workflow editor improvements + AI assistance (new)
- ~2 weeks of remaining work on the workflow editor UI
- **AI → JCWF:** User describes a workflow in natural language (prompt), AI generates a valid `.jcwf` file
- **JCWF → AI:** User loads an existing `.jcwf` file, AI generates a human-readable summary/documentation of what it does
- Integration point: workflow editor UI sends prompt to backend, backend calls AI provider, returns structured JCWF JSON
- Validation: generated JCWF should pass `workflowValidator` before being offered to the user
- UX: "Generate from prompt" button and "Explain this workflow" button in the editor

---

## 10. Webhook trigger type (future)
- Currently, external systems can already trigger workflows via `POST /api/workflows/<id>/run` and `POST /api/integrations/n8n/start`
- A dedicated `"type": "webhook"` trigger would add:
  - Per-workflow webhook secrets (HMAC signature verification)
  - Cleaner URLs like `/api/webhook/<workflowId>`
  - Declarative intent in the JCWF ("this workflow is designed to be called externally")
- Not a blocker — existing REST API covers the functionality; this is a polish/security item for v0.9+

---

## 11. Error branching / conditional edges (new)
- Currently workflow runs are atomic — if a task fails, the run fails
- Add `"on_error"` field to task definitions in JCWF, e.g. `"on_error": "task_fallback"`
- Support success edges and failure edges in the DAG:
  - `"depends_on"` = success edge (existing)
  - `"on_error"` / `"depends_on_failure"` = failure edge (new)
- Enables powerful patterns:
  - **Retry with different model** — fallback task uses a different AI provider
  - **Fallback provider** — switch from OpenAI to Gemini on failure
  - **Notification task** — Slack/email alert on failure
  - **Auto-debug task** — AI analyzes the error output and suggests fixes
- This is a major maturity jump for the workflow engine
- Requires changes to: `WorkflowRuntimeManager` (task dispatch logic), `workflowJsonParser` (parse `on_error`), `workflowValidator` (validate error edges), `workflowTypes.h` (new edge type), JC Workflow Specification (new section)

---

## 12. Built-in retries with backoff (new)
- Instead of users scripting retries in shell or AI tasks, provide infrastructure-level robustness
- Add per-task retry configuration in JCWF:
  ```json
  "retry": {
    "max_attempts": 3,
    "backoff_ms": 2000
  }
  ```
- The workflow runtime manager handles retries transparently — no changes needed in task scripts
- Backoff strategy: fixed or exponential (e.g. 2s → 4s → 8s)
- Interacts with error branching (#11): retries are exhausted before `on_error` edge fires
- Requires changes to: `WorkflowRuntimeManager` (retry loop + backoff timer), `workflowJsonParser` (parse `retry` block), `workflowValidator` (validate retry params), `workflowTypes.h` (retry config struct), `TaskInstanceState` (attempt counter already exists)

---

## 13. Browser-based AI chat terminal (new)
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


## 14. Headless server mode (new)
- **CLI startup otions**
  - start command: jarvisAgent --headless --port 8080
  - update docs regarding all CLI options (also --version, --help)

## 15. Man page (new)
- Create man page for jarvisAgent

---

## 16. Landing page for new users (new)
- Create a welcoming landing page / website for JarvisAgent
- Should explain what JarvisAgent is, key features, screenshots, and download links
- Target audience: first-time visitors who discover the project

---

## 17. Self-hosted Docker registry (new)
- Evaluate hosting our own server for the Docker package instead of relying solely on GHCR
- Benefits: custom domain, no GitHub dependency, potential for private images
- Alternatives: self-hosted Docker registry, Harbor, or a simple VPS with registry

---

## 18. Promotion video (new)
- Create a demo / promotion video showcasing JarvisAgent
- Cover: workflow creation in the editor, running workflows, dashboard monitoring, multi-platform support
- Target: GitHub README embed, YouTube, social media

---

## 19. Packaging testing (new)
- [ ] **macOS:** Test DMG install + uninstall on real hardware, verify instructions in packaging.md
- [ ] **Windows:** Test MSI install + uninstall, verify PATH entry works, test setup-venv.bat
- [ ] **Linux (deb):** Test install as non-root user workflow (blocked on #8 — launcher script)
- [ ] **Linux (deb/Launchpad):** Build and upload source package to PPA, verify Launchpad builds it
- [ ] **Linux (rpm):** Test install on Fedora/Rocky, verify post-install hooks
- [ ] **Linux (Arch):** Test PKGBUILD install on Manjaro
- [ ] **All platforms:** Evaluate user-friendliness of first-run experience (config.json setup, venv, starting the app)

