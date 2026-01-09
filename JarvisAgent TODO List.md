# JarvisAgent TODO List

This list tracks the remaining work for JarvisAgent.

---

## 1. GitHub CI — Ubuntu (in progress)
- Fix smoke test failing (TTY / ncurses / config path)
- Add macOS CI runner
- Add Windows CI runner
---

## 2. Windows Build (not started)
- Generate MSVC project via premake5
- Compile using MSVC
- Test it

---

## 3. Dockerization (in progress)
- Convert Dockerfile to Ubuntu 24.04
- Remove deadsnakes PPA
- Use python3/python3-dev from system
- Use PDCurses-wide in container instead of system ncurses
- Remove TRACY_NO_INVARIANT_CHECK
- Verify working headless mode

---

## 4. Terminal UI (new)
- PDCurses on macOS: backend VT is configured, needs to be  tested
- PDCurses on Windows: backend Wincon is configured, needs to be  tested

---

## 5. Workflow system (follow-ups)
- Python task executor
- Internal (C++) task executor
- Manual trigger via browser-based terminal prompt (remote control from a web page)
- Conditional tasks (spec + engine): execute tasks depending on boolean or enum return values written to a file (example: AI picks 1 out of N options → writes selection → task is selected)

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

---

## 7. Browser-based terminal prompt (remote control from a web page) (new)
- Use a web terminal emulator like xterm.js on the frontend, and drive it via your existing WebSocket server side (Crow is already in your stack).
- In that setup, you don’t need a “C++ prompt library” at all — you need:
  - a command parser/dispatcher in C++
  - a completion provider in C++ (your own command registry is usually best)
  - a tiny WS protocol: `{ "type":"line"|"complete"|"help", ... }`
- Commands to support (initial):
  - trigger workflows
  - check state
  - get help

---

