# JarvisAgent Logging & Terminal System Documentation

## Overview

The logging subsystem consists of:
- **StatusRenderer** — builds session-status lines for terminal UI
- **Log** — configures two spdlog loggers into `std::cout`
- **TerminalLogStreamBuf** — custom `std::streambuf` routing all logs to ncurses + optional file
- **TerminalManager** — ncurses UI with log window + status window
- **Python log.py** — Python-side logging formatted for redirection into C++

All components work together to produce clean, line‑based logging in both the ncurses terminal and an optional logfile.

---

# StatusRenderer

Tracks per-session status and renders lines for the terminal.

### Responsibilities
- Store `SessionStatus` for each session:  
  `name`, `state`, `outputs`, `inflight`, `completed`
- Maintain spinner animation when queries are running
- Generate UTF‑8–safe, width‑limited status lines
- Thread‑safe updates

### Key Operations
- `UpdateSession(...)` — record a status row (name + state + counters)
- `BuildStatusLines(outLines, maxWidth)`  
  - Safely truncates UTF‑8 using `SafeTruncateUtf8`
  - Emits `[session] STATE: ...` rows sorted alphabetically
- `GetSessionCount()` — used by TerminalManager to size status window

---

# Log (spdlog Integration)

### Responsibilities
- Create two loggers:
  - `"Engine"`
  - `"Application"`
- Direct *all* output to:
  - An `ostream_sink_mt` → **std::cout**
- Use plain, non‑colored pattern (ncurses cannot display color codes)

### Effect
Every `LOG_*` macro writes to `std::cout`, which is later intercepted by `TerminalLogStreamBuf`.  
No direct file logging is done here.

---

# TerminalLogStreamBuf

Custom stream buffer that captures `std::cout` output.

### Responsibilities
- Receive character output from all loggers
- Buffer until newline, then:
  - Strip ANSI escape codes
  - Drop empty / whitespace-only lines
  - Push clean line into `TerminalManager::EnqueueLogLine()`
  - Optionally write to logfile (if provided)

### Important Behaviors
- `sync()` flushes a full log line
- `overflow()` flushes on newline
- `xsputn()` flushes only when the appended data ends with `
`
- Ensures **every logged message = exactly one line**

---

# TerminalManager (ncurses UI)

### Responsibilities
- Own two ncurses windows:
  - **Log window** (scrolling)
  - **Status window** (session overview)
- Receive log lines from `TerminalLogStreamBuf`
- Render status lines from `StatusRenderer`
- Handle window resizing / redraw
- Apply theme (green foreground)
- Maintain thread‑safe queue of pending log lines

### Key Operations
- `Initialize()` — configure ncurses, create windows
- `Render()` — drain queued logs, repaint UI
- `RenderPaused()` — show pause screen
- `SetStatusCallbacks()` — JarvisAgent provides lambdas that call `StatusRenderer`
- `EnqueueLogLine()` — append line to pending queue

---

# Python log.py

Python-side mirror of C++ logging formatting.

### Responsibilities
- Provide `log_info`, `log_warn`, `log_error`
- Prefix messages with timestamps and tags:
  ```
  [PY][INFO HH:MM:SS] message
  ```
- `print()` ensures each Python log becomes a separate, newline‑terminated line
- Output is redirected by PythonEngine into `JarvisRedirectPython → std::cout`

---

# ShellTaskExecutor (Shell task logging)

Shell tasks must not write directly to the terminal (stdout/stderr), because that bypasses the `spdlog → std::cout → TerminalLogStreamBuf` pipeline and can corrupt the ncurses UI.

### Responsibilities
- Execute the configured shell command/script for a `shell` task.
- Capture **stdout and stderr** from the invoked process.
- Forward captured output back into the normal logging pipeline via `LOG_APP_INFO(...)`.

### Important Behaviors
- Shell execution captures output via a pipe (`popen` / `_popen`) instead of using `std::system`.
- `stderr` is redirected into `stdout` (`2>&1`) so both streams are captured.
- Output is forwarded **line-by-line** as log messages so `TerminalLogStreamBuf` can enforce the “one log message = one line” rule.
- The executor logs:
  - the command being executed (e.g. `[shell] Command: ...`)
  - each output line (e.g. `[shell:<taskId>] <line>`)

---

# End-to-End Logging Flow

```
spdlog → std::cout
         ↓
TerminalLogStreamBuf
  - strip ANSI
  - drop empty lines
  - forward to TerminalManager
  - (optional) logfile
         ↓
TerminalManager
  - queued → rendered in ncurses log window
```

Python logs follow the same path:
```
log.py print() → Python redirect → C++ JarvisRedirectPython → std::cout → TerminalLogStreamBuf
```

Shell task output is normalized into the same path:
```
ShellTaskExecutor (popen / _popen capture stdout+stderr)
         ↓
LOG_APP_INFO(...) → std::cout → TerminalLogStreamBuf → TerminalManager
```

---

# Summary

The logging system ensures:
- All logs (C++ & Python) flow through a single unified pipeline
- Shell task output is captured and forwarded into the same pipeline (no direct stdout/stderr to terminal)
- Every message is line-based, newline-terminated, and ANSI-clean
- Terminal UI stays responsive and UTF-8 safe
- Status information is updated per session through StatusRenderer

This pipeline is deterministic, thread‑safe, and avoids mixing or partial-line output.
