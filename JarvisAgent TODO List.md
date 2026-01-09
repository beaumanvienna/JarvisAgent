# JarvisAgent TODO List

This list tracks the remaining work for JarvisAgent.

---

## 1. GitHub CI (Linux, Windows, and MacOS Build)
- Linux, macOS, and Windows workflows are green 
- Fix smoke test failing (TTY / ncurses / config path)
- Run on actual Windows and macOS operating systems, not just the Github actions workflow

---

## 2. Dockerization (in progress)
- Finish / merge PR #1 (Ahmet): “CI/CD build and docker for deployment”
  - Update Dockerfile build + runtime stages to Ubuntu 24.04
  - Remove deadsnakes PPA / hardcoded Python version; use system Python (`python3`, `python3-dev`, `python3-pip`)
  - Remove any ncurses packages (PDCursesMod is vendored + statically linked)
  - Remove TRACY_NO_INVARIANT_CHECK changes (Tracy is off by default)
  - Resolve merge conflicts (`config.json`, `engine/log/terminalManager.cpp`)
- Verify working headless mode in container
- Verify working deployment (docker-compose example)

---

## 3. Terminal UI (new)
- PDCurses on macOS: backend VT is configured, needs to be tested
- PDCurses on Windows: backend Wincon is configured, needs to be tested

---

## 4. Workflow system
- Manual trigger via browser-based terminal prompt (remote control from a web page)
- Workflows for individual lines from spreadsheets

---

## 5. Python Engine parallelization (new)
- Add support for multiple independent PythonEngine instances
- Ensure each interpreter instance owns its own GIL
- Store PythonEngine instances in std::vector
- Default engine count: 4
- Allow override via config.json
- Expose internal task-queue size for load balancing
- Dispatch OnEvent() to the PythonEngine with the lowest queued workload
- Ensure isolated interpreter state per engine

---

## 6. Browser-based terminal prompt (remote control from a web page) (new)
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
