# JarvisAgent TODO List

This list tracks the remaining work for JarvisAgent.

---

## 1. GitHub CI — Ubuntu (in progress)
- Fix smoke test failing (TTY / ncurses / config path)

---

## 2. Windows Build (not started)
- Generate MSVC project via premake5
- Compile using MSVC
- Use PDCurses for ncurses UI on Windows
- Add Windows CI runner

---

## 3. Dockerization (in progress)
- Convert Dockerfile to Ubuntu 24.04
- Remove deadsnakes PPA
- Use python3/python3-dev from system
- Replace ncurses5 with ncurses6
- Remove TRACY_NO_INVARIANT_CHECK
- Verify working headless mode

---

## 4. Terminal UI (new)
- Vendor PDCurses-wide source code for Linux, macOS, Windows
- Prefer PDCurses over system ncursesw6
- Use system ncursesw6 on only as fallback

---
