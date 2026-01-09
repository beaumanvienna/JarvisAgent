# JarvisAgent – High-Level Project Overview

This supplemental document summarizes the project as understood by ChatGPT
based on the persistently stored context.  
It complements the more detailed module-level documentation already present in
the repository.

---

## 1. Project Identity

**Project Name:** JarvisAgent  
**Organization:** JC Technolabs  
**Language:** C++20 (core), Python 3.12 (scripting layer)

JarvisAgent is a **modular automation engine** that processes queue folders,
parses incoming documents, converts them using external tools (MarkItDown),
creates markdown chunks, manages AI backends, and exposes a real-time web UI.

---

## 2. Architectural Summary

JarvisAgent consists of two major layers:

### **A. C++ Engine Layer**
Responsible for:
- Event-driven application loop
- Thread pool, timing, filesystem watching
- Queue processing and message categorization
- Session management (outputs, inflight, completed)
- WebSocket server (Crow)
- Python scripting bridge via `ctypes`
- Logging with terminal rendering
- Configuration parsing with simdjson

Key subsystems:
- **EngineCore** – fundamental services (threads, events, logging)
- **Event System** – queue + typed event objects
- **SessionManager** – controls message lifecycle & output files
- **PythonEngine** – hosts the Python worker thread
- **WebServer** – handles browser dashboard and WS connections
- **FileWatcher** – monitors queue folder for new inputs
- **Auxiliary** – filesystem helpers, thread pool, OS wrappers

---

### **B. Python Scripting Layer**
Runs inside a dedicated worker thread and provides:

- Document conversion via **MarkItDown CLI**
- Markdown chunking and reassembly
- Text sanitization and preprocessing
- Error propagation back to C++ (`JarvisPyStatus`)
- Redirecting of Python stdout/stderr into C++ logs
- Handlers:
  - `OnStart()`
  - `OnEvent(event)`
  - `OnShutdown()`

This layer is strictly **stateless** between events and tolerant to soft failures.
Hard failures notify the UI (“Python Offline”).

---

## 3. Web Dashboard

JarvisAgent ships with a draggable, real-time monitoring UI:

- WebSocket-based live status communication
- Session state boxes (waiting, inflight, completed)
- Animated spinner for active requests
- Output panel for final and late answers
- Timeout messages
- “Python Offline” indicator
- Manual quit button

Colors, fonts, and UX match the console theme (#1e1e1e / #44a01c).

---

## 4. Persistent Project Preferences (Meta)

The following conventions are stored as long-term preferences:

- **C++ style**
  - Allman braces
  - `const` appears **after** the type (`std::string const&`)
  - Descriptive variable names, no abbreviations >2 letters
  - Avoid exceptions in engine code
  - Prefer `reserve()/resize()` where applicable
  - No omitted braces, even for single-line bodies
  - camelCase filenames

- **Repository structure**
  - Dedicated Markdown documentation for every subsystem
  - Tools/scripts go into `/tools`
  - Python scripts under `/scripts` with `helpers/`

- **Operational guidelines**
  - `git reset` is dangerous → double check before using
  - Think extra hard before suggesting large design changes
  - Provide downloadable `.md` files because browser formatting interferes

---

## 5. Build Targets (Current + Planned)

### **Current**
- Linux (Ubuntu / Zorin OS)
- clang++ / g++ using Premake5 or Makefile
- MarkItDown CLI required (via `pipx install markitdown[pdf]`)

### **Planned / On TODO List**
- Windows build
- GitHub CI (Linux + Windows)
- Docker build
- Optional integration with Polarion ALM

---

## 6. High-Level Pipeline Overview

```
                     +----------------+
                     |  queue folder  |
                     +--------+-------+
                              |
                        FileWatcher
                              |
                              v
+---------------------+   +-----------------+
|  Document arrives   |→→→|   PythonEngine  |
+---------------------+   +-----------------+
    |                         |
    | MarkItDown conversion   | chunk split if size > limit
    v                         v
markdown.md            chunk_001.md, chunk_002.md, ...
                              |
                        chunk_combiner.py
                              |
                              v
                     markdown.output.md (final)
                              |
                              v
                       SessionManager
                              |
                              v
                 WebServer → Browser Dashboard
```

---

## 7. Philosophy

JarvisAgent is built with three principles:

1. **Line-based, synchronized logging**  
   Both C++ and Python logs are unified and routed via the engine.

2. **Deterministic processing**  
   No race conditions in file handling; no incomplete lines in logs.

3. **Human-readable diagnostics**  
   Python failures surface immediately (“Python Offline”).

---

## 8. Related Documentation

This file should be used together with the module-level `.md` files in:

- `application/`
- `engine/`
- `scripts/`
- `web/`

See `docs_index.md` for a full navigation tree.

---

## 9. Contact & Branding

JC Technolabs  
JarvisAgent – Intelligent Document Processing Framework  
2025
