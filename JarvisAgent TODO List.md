# JarvisAgent TODO List

This list tracks the remaining work for JarvisAgent.

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
- Investigate per-user data directories (e.g. `~/.local/share/jarvisagent/`) like the Flatpak/AppImage approach
- Or use a shared group (`jarvisagent`) with appropriate permissions
- The launcher script and config.json should support user-level overrides for `queue folder` and `workflows folder`
- `md2pdf` (from the Python venv at `/opt/jarvisagent/.venv`) is not on PATH by default — the launcher or shell scripts need to activate the venv or add it to PATH so workflows like `vehicleTroubleshootingGuide` can find it

---

## 9. Browser-based AI chat terminal (new)
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
