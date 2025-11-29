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

## 5. Workflow files (new)
- Add support for JSON task lists defined via JC workflow files
    - Implement JSON loader for JC workflow files
    - Implement task dependency resolver
    - Implement triggers (cron, file-change)

---
