# JarvisAgent Roadmap

<br>

This document outlines the **next development milestones** for JarvisAgent.  
The focus is on rock‑solid core behavior (no freezes, no log spam), safe handling of large files, and a clean foundation for future plugins and dashboards.

---

## 1. Core Stability & Shutdown Behavior

**Goal:** JarvisAgent must always start, run, and shut down cleanly — even with many in‑flight queries.

### 1.1 Graceful Shutdown With In‑Flight Queries
- Add a global **shutdown flag** that is set when `q` is pressed or a termination signal is received.
- Worker threads periodically check this flag and **abort any pending work** as soon as it is safe.
- Add configurable libcurl timeouts:
  - `CURLOPT_CONNECTTIMEOUT_MS`
  - `CURLOPT_TIMEOUT_MS`
- On shutdown:
  - Stop accepting new work.
  - Cancel or let existing requests finish within bounded time.
  - Always reach a deterministic “engine is shutting down” code path.

### 1.2 Robust Status Line Renderer
- Fix the **status line renderer** so that it never overwrites or destroys log messages.
- Explicitly separate:
  - Persistent log lines (spdlog)
  - Ephemeral status line (single line rewritten in place)
- Add a configuration option to fully **disable** the status line from the config file or CLI.
- Verify that logs remain readable when redirected to a file or CI system.

---

## 2. Python Integration (Current & Next Steps)

**Goal:** Use Python as a flexible automation layer without blocking the main engine.

### 2.1 Current State
- JarvisAgent embeds Python and loads `scripts/main.py`.
- The following hooks are already supported:
  - `OnStart()`
  - `OnUpdate()`
  - `OnEvent(event)`
  - `OnShutdown()`
- Python is successfully used for:
  - Detecting PDFs
  - Converting them to Markdown via MarkItDown
  - Logging helper messages and warnings

### 2.2 Next Step: Asynchronous Python Event Queue
- Introduce a dedicated **Python worker thread**:
  - A lock‑free or mutex‑protected queue collects Python events (`FileAdded`, `FileRemoved`, etc.).
  - The main thread enqueue events only; the Python worker processes them in the background.
- Ensure that:
  - Large or slow operations in Python (e.g., PDF conversion) **never block the main loop**.
  - Shutdown waits only for the Python worker to finish its current task or times out gracefully.
- Add metrics/logging for Python queue depth and processing time.

---

## 3. MarkItDown & File Handling

**Goal:** Support rich document formats while keeping GPT usage safe and predictable.

### 3.1 Current State
- Python integration uses **Microsoft’s MarkItDown** to convert:
  - `.pdf` → `.pdf.md`
  - (Future: `.docx`, `.pptx`, `.xlsx`)
- The C++ core now includes:
  - **Binary detection** via magic numbers (ZIP, PNG, PDF, JPEG, GIF, BMP, ELF, PE, etc.).
  - A **config‑driven max file size** (`"max file size in kB"`) enforced in `FileCategorizer`.
  - If a file exceeds this limit:
    - It is ignored by the engine.
    - A `*.output.txt` file is written explaining that the file is too large and was skipped.

### 3.2 Planned Enhancements
- Extend MarkItDown handling in Python to additional formats:
  - `.docx`, `.pptx`, `.xlsx`
- Add optional **sectioning / chunking** for large Markdown files (future):
  - Split large `.md` into smaller logical segments.
  - Feed only relevant sections to GPT based on the requirement context.

---

## 4. PROB File Lifecycle & Logging Hygiene

**Goal:** Make PROB_* request/response files first‑class citizens with clean startup behavior.

### 4.1 Current State
- PROB files are parsed via `ProbUtils::ParseProbFilename`.
- JarvisAgent now:
  - **Ignores stale PROB files** whose timestamp is older than the current app startup time.
  - Routes fresh `PROB_*.output.txt` directly into `ChatMessagePool::MarkAnswered()`.
  - Avoids “Late answer received for expired ChatMessage” spam at startup.

### 4.2 Next Steps
- Add a small **maintenance helper** or script to:
  - Move or delete stale PROB files from `ICE/` and the main queue.
  - Optionally compress/archive old sessions.
- Tighten logging:
  - Only log PROB timestamp info at **debug/trace** level when needed.
  - Keep normal startup logs concise.

---

## 5. Web Interface & Dashboard

**Goal:** Provide a clean, low‑friction way to see what JarvisAgent is doing in real time.

### 5.1 Current State
- A local **Crow** web server is embedded.
- The server already serves:
  - A minimal frontend with a WebSocket connection.
  - Live status updates from the engine (in‑flight, completed, outputs, etc.).

### 5.2 Planned Enhancements
- Stabilize the current dashboard:
  - Ensure updates are consistent and never block shutdown.
  - Make the layout more readable (group by queue folder, state, counts).
- Add features:
  - Per‑request details (which requirement, which model, timings).
  - Simple filters (show only “in‑flight”, “failed”, “oversized”, etc.).
  - Basic performance metrics (average latency, requests per minute).

---

## 6. Python Plugin System (Future)

**Goal:** Allow power users to extend JarvisAgent behavior without changing C++.

- Define a simple **plugin API** in Python:
  - `on_requirement(file_path, context) -> Optional[str]`
  - `on_output(file_path, model, raw_output) -> str`
- Allow plugins to:
  - Auto‑tag incoming requirements.
  - Rewrite or augment prompts.
  - Postprocess responses (e.g., format into Markdown templates, append checklists).
- Use configuration to enable/disable plugins and control their order.

---

## 7. Multi‑Backend Support (Future)

**Goal:** Let JarvisAgent orchestrate multiple AI providers and local models.

- Abstract the “API interface” layer (already partially done via config):
  - Support multiple entries under `"API interfaces"` in `config.json`.
- Planned backends:
  - OpenAI (current)
  - Other hosted APIs (Anthropic, etc.) — if licenses allow
  - Local models via HTTP or Unix sockets
- Add backend selection strategies:
  - Per‑queue or per‑file routing
  - Fallback if one provider is down
  - Cost‑aware or latency‑aware routing

---

## 8. Quality, Testing & Tooling

**Goal:** Keep JarvisAgent maintainable as complexity grows.

- Add unit tests for:
  - `FileCategorizer` (binary detection, max size, PROB handling).
  - Config parsing & validation, including `max file size in kB`.
  - PROB filename parsing and timestamp comparison.
- Add integration tests for:
  - Simple end‑to‑end flows (REQ + CNTX → PROB files → outputs).
  - PDF → Markdown → GPT → `.output`.
- Improve logging defaults:
  - Clear separation between Engine, Application, Python, and Web logs.
  - All critical paths log enough context to debug production issues.

---

## 9. Long‑Term Vision

JarvisAgent continues to evolve into a **unified AI processing hub** that combines:

- On‑disk file orchestration
- Intelligent dependency tracking
- Asynchronous Python‑driven workflows
- Safe, configurable limits for large files
- A lightweight, live web dashboard

The guiding principle remains the same:  
**Do only the work that’s necessary — but do it intelligently, safely, and predictably.**

---

GPL-3.0 License © 2025 JC Technolabs
