# JarvisAgent Architecture

<br>

JarvisAgent is an autonomous C++ background service for AI-assisted document and workflow processing.  
It integrates a **Python scripting engine**, a **file-driven orchestration core**, and an **embedded web server** that exposes a **browser-based chatbot interface** for interactive use.

This document describes the **software architecture**, **communication layers**, and **internal API contracts**.

---

## 🧩 Summary

| Layer | Implementation | Purpose |
|--------|----------------|----------|
| **Core Engine** | C++ | File-based AI orchestration, dependency tracking, REST I/O |
| **Scripting Layer** | Python | Extensibility, preprocessing, and automation via hooks |
| **Web Server** | C++ (`cpp-httplib`) | Serves chatbot frontend and WebSocket API |
| **Frontend (Bot UI)** | HTML5 + Tailwind CSS + Vanilla JS | Chat interface for end user |
| **IPC (Internal)** | Shared filesystem (`queue/`) | Communication between web server and JarvisAgent core |
| **Client–Server Protocol** | JSON over WebSocket | Bidirectional chat and status updates |

---

## ⚙️ Core Components

### 1. JarvisAgent Core (C++)

**Responsibilities:**
- Monitors the `queue/` directory for file additions, modifications, or deletions.
- Categorizes files by prefix (`STNG`, `CNTX`, `TASK`) or no prefix for requirements, questions, problem reports, queries, in general: line items that need to be processesd against a common environment.
- Assembles AI queries by combining subsystem context and new inputs.
- Dispatches asynchronous REST requests to AI backends (GPT-4 / GPT-5).
- Writes resulting outputs to `.output.txt` files.
- Provides live runtime feedback via the terminal `StatusLineRenderer`.

---

### 2. Python Scripting Engine

**Purpose:** Extend JarvisAgent through dynamic, user-defined automation scripts.  
External tools such as **MarkItDown** are made available through this scripting layer.

**Integration:** The scripting engine is embedded via the CPython C API and exposes four hooks:

| Hook | Description |
|-------|-------------|
| `OnStart()` | Invoked during application startup. |
| `OnUpdate()` | Called every engine tick (approx. once per main loop iteration). |
| `OnEvent(event)` | Triggered when a file or system event occurs (e.g., file added). |
| `OnShutdown()` | Executed before application exit. |

**Event Example:**
```python
def OnEvent(event):
    if event.type == "FileAdded" and event.path.endswith(".pdf"):
        # Convert the PDF into Markdown using MarkItDown
        os.system(f"markitdown \"{event.path}\" --output \"{event.path}.md\"")
