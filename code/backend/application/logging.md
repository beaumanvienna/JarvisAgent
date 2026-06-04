# JarvisAgent Logging & Terminal System Documentation

## Overview

The logging subsystem consists of:
- **StatusRenderer** — aggregates AI-call counters and the last-3-runs bar for the terminal status window
- **Log** — configures two spdlog loggers into `std::cout`
- **TerminalLogStreamBuf** — custom `std::streambuf` routing all logs to ncurses + optional file
- **TerminalManager** — ncurses UI with log window + status window
- **Python log.py** — Python-side logging formatted for redirection into C++

All components work together to produce clean, line‑based logging in both the ncurses terminal and an optional logfile.

---

# StatusRenderer

Drives the terminal's status window — two fixed-height rows that mirror the
signals the dashboard consumes from `/api/status` + `/api/settings/keys/status`,
so the TUI and web views stay in sync.

1. **Row 1 — status LEDs** — edition prefix, then key state (locked / unlocked
   / wrong-password / missing-file), AI in-flight (with braille spinner while
   anything is in flight), active workflow runs, MCP sidecar state, cloud
   connector health, run totals (succeeded / failed), and a Python-offline
   indicator when the engine pool is down.
2. **Row 2 — last runs** — up to 3 rolling entries (`✓ workflowId (12s ago)`),
   same shape as the dashboard's `LastRunsBar`. When keys are locked or no
   providers are loaded, the row carries a sealed-keys hint instead since the
   TUI has no unlock affordance.

### Architecture
- **Provider-driven, polled per render.** `JarvisAgent` registers two
  `std::function` providers via `SetRuntimeSnapshotProvider` and
  `SetLastRunsProvider` once during `OnStart`. `BuildStatusLines` calls
  them on every `TerminalManager::Render` tick.
- **Thread-safe.** All state is guarded by `m_Mutex`. Provider functions are
  copied out under the lock and invoked unlocked, so a slow provider doesn't
  block re-registration.
- **Defense layering for byte safety.** Externally-sourced fields
  (workflow IDs, unknown key-status echoes) are bounded per-field at the
  entry point so one outsized value can't crowd out the row. The renderer's
  UTF-8 truncator validates continuation bytes before consuming them, so a
  malformed lead can't over-skip past the actual character boundary.
  ncurses-level sanitization (replacing malformed UTF-8 / C0 controls with
  `?`) is the responsibility of `TerminalManager::SanitizeForCurses`
  downstream — see the comment block at the top of
  `code/backend/application/log/statusRenderer.h` for the canonical contract.

### Key Operations
- `SetRuntimeSnapshotProvider(provider)` / `SetLastRunsProvider(provider)`
  — registered once during `JarvisAgent::OnStart`.
- `BuildStatusLines(outLines, maxColumns)` — assembles both rows; called
  by `TerminalManager` on every render.
- `GetRowCount()` — always returns 2; `[[nodiscard]]` because
  `TerminalManager` sizes the status window from it.

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

# Crow web server (WebServer log routing)

The embedded Crow HTTP framework has its own logger with a default `CerrLogHandler` that writes straight to `std::cerr`. On its own that bypasses the ncurses layer and overpaints the status window on every connection event. `WebServer::WebServer` installs a custom `crow::ILogHandler` (`CrowSpdlogHandler` in `webServer.cpp`) that forwards every Crow log line through `LOG_CORE_*`, so Crow output lands in `log.txt` and the ncurses LOG window exactly like our own logs. One specific benign warning — `"Could not start adaptor: ssl/tls alert certificate unknown"`, raised every time an untrusted-cert client attempts TLS — is suppressed at the shim level since it is not actionable in development.

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
- Output is forwarded **line-by-line** as log messages so `TerminalLogStreamBuf` can enforce the "one log message = one line" rule.
- Captured output is **routed through `SanitizeUtf8(...)` before reaching `LOG_APP_INFO`** at every flush site (per-line, per-fragment, residual-flush at end-of-stream).  Spawned commands can emit malformed UTF-8 / control characters / overlong encodings that would otherwise corrupt the ncurses pipeline or crash dashboard JSON serialisation.  The `SanitizeUtf8` helper lives in `code/backend/application/workflow/workflowTypes.h`; its sibling `TruncateUtf8Safe(N)` bounds size and respects multi-byte boundaries.
- The executor logs:
  - the command being executed (e.g. `[shell] Command: ...`)
  - each output line (e.g. `[shell:<taskId>] <line>`)

### Log-injection discipline (codebase-wide)

The `SanitizeUtf8` + `TruncateUtf8Safe` pair is the convention for **every** boundary where externally-sourced bytes (AI provider replies, fixture file reads, captured stdout/stderr, HTTP error body fragments, JSON field values) enter the codebase.  Without it, a malicious upstream can land arbitrary bytes — including ANSI escape codes, embedded `\r`/`\n` for log forging, or invalid UTF-8 that crashes downstream JSON serialisation — into `log.txt`, the dashboard WebSocket stream, and the ncurses TUI.  Sanitise once at the boundary; don't sprinkle defensive copies through downstream code.

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
- Status information is driven by AI dispatch events + last-runs snapshot through StatusRenderer

This pipeline is deterministic, thread‑safe, and avoids mixing or partial-line output.
