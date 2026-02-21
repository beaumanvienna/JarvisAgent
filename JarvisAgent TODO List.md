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

---

## 8. Browser-based terminal prompt (remote control from a web page) (new)
- Use a web terminal emulator like xterm.js on the frontend with the existing WebSocket server side (Crow is already in your stack).
- In that setup, you don’t need a “C++ prompt library” at all — you need:
  - a command parser/dispatcher in C++
  - a completion provider in C++ (your own command registry is usually best)
  - a tiny WS protocol: `{ "type":"line"|"complete"|"help", ... }`
- Commands to support (initial):
  - trigger workflows
  - check state
  - get help

---
