# JarvisAgent Architecture

JarvisAgent is an autonomous C++ background service for AI-assisted document and workflow processing.  
It integrates a **Python scripting engine**, a **file-driven orchestration core**, and an **embedded web server** that exposes a **browser-based chatbot interface** for interactive use.

This document describes the **software architecture**, **communication layers**, and **internal API contracts**.

---

## 🧩 Summary

| Layer | Implementation | Purpose |
|--------|----------------|----------|
| **Core Engine** | C++ | File-based AI orchestration, dependency tracking, REST I/O |
| **Scripting Layer** | Python | Extensibility, preprocessing, and automation via hooks |
| **Web Server** | C++ (`crow`) | Serves chatbot frontend and WebSocket API |
| **Frontend (Bot UI)** | HTML5 + Tailwind CSS + Vanilla JS | Chat interface for end user |
| **IPC (Internal)** | Shared filesystem (`queue/`) | Communication between web server and JarvisAgent core |
| **Client–Server Protocol** | JSON over WebSocket | Bidirectional chat and status updates |

---

## ⚙️ Core Components

### 1. JarvisAgent Core (C++)

**Responsibilities:**
- Monitors the `queue/` directory for file additions, modifications, or deletions.
- Categorizes files by prefix (`STNG`, `CNTX`, `TASK`) or no prefix for requirements, questions, problem reports, and queries.
- Assembles AI queries by combining subsystem context and new inputs.
- Dispatches asynchronous REST requests to AI backends (GPT-4 / GPT-5).
- Writes resulting outputs to `.output.txt` (for text jobs) and `.output.md` (for multi-chunk Markdown) files.
- Provides live runtime feedback via the terminal `StatusLineRenderer`.
- Detects and skips files whose output is already up-to-date.
- Reacts to chunk lifecycle: `.md` → chunked `.md` → AI summaries → combined `.output.md`.

---

### 2. Python Scripting Engine

**Purpose:** Extend JarvisAgent through dynamic, user-defined automation scripts, including PDF conversion, Markdown chunking, and chunk-output recombination.

**Integration:** The scripting engine is embedded via the CPython C API and exposes four hooks:

| Hook | Description |
|-------|-------------|
| `OnStart()` | Invoked during application startup. |
| `OnUpdate()` | Present for API completeness but currently unused. |
| `OnEvent(event)` | Main handler for file-driven workflow: PDF conversion, MD chunking, chunk-output combining. |
| `OnShutdown()` | Executed before application exit. |

**FileEvent Highlights handled in Python:**

- **PDF Added:**  
  Converts PDF to Markdown unless an up-to-date `.md` exists.
- **Markdown Added:**  
  If large, chunked into `chunk_XXX.md` files. If an existing combined output is newer, chunking is skipped.
- **Chunk Output Added (`chunk_XXX.output.md`):**  
  When all outputs for a document are present, merges them into a final `<file>.output.md` file.

---

## ARCHITECTURE DETAILS

<br>

This section expands on the operational details for JarvisAgent’s web interface and runtime communication.

---

## 🔐 Design Goals

- **Transparent pipeline** — Every step materialized as a file for traceability and offline auditing.
- **Event-driven** — File watcher + selective rebuilds; no redundant work.
- **Embeddable** — Lightweight, single-binary server using the `crow` micro web framework.
- **Extensible** — Python scripting hooks enable custom preprocessing (MarkItDown, chunking logic, recombination logic).
- **Operator-friendly** — Terminal status line + browser chat UI with live updates.
- **Binary-safe** — Non-text files are detected and ignored or preprocessed by the scripting layer.
- **Idempotent processing** — Files are skipped efficiently when outputs are newer.

---

## 🧠 Communication Protocol Summary

| Channel | Direction | Technology | Payload |
|--------|-----------|------------|--------|
| Browser ↔ WebServer | Bidirectional | **WebSocket** | JSON events (chat, status, responses, errors) |
| WebServer → JarvisCore | One-way | **Shared filesystem (queue/)** | Text files (`STNG/ CNTX/ TASK/ PROB/ REQ/`, Markdown chunks) |
| JarvisCore ↔ AI Backend | Request/Response | **HTTP REST (libcurl)** | OpenAI-style JSON |
| JarvisCore → WebServer | Event-driven | **Filesystem change detection** | Output files trigger WS pushes |
| WebServer → Browser | Push | **WebSocket** | Responses, status, errors |

---

## 🌐 Embedded Web Server

**Library:** [`Crow micro web framework`](https://github.com/CrowCpp/Crow) (header-only, HTTPS-capable)

**Responsibilities:**
- Serve static assets (HTML, JS, CSS) for the chat UI.
- Provide REST endpoints for chat submission and status.
- Maintain WebSocket sessions for live progress and answer delivery.
- Bridge the browser to the file-based IPC with JarvisCore.

**Mounts (suggested):**
- `GET /` → index.html  
- `GET /assets/*` → static assets  
- `POST /api/chat` → submit problem reports  
- `GET /api/status` → system snapshot  
- `GET /ws` → WebSocket endpoint  

---

## 📡 API Endpoints

### `POST /api/chat`

Queues a user message for a **specific subsystem** by creating a file in the `queue/<subsystem>` directory.

**Request**
```json
{
  "subsystem": "engine",
  "message": "Engine knocks at idle after warmup; MIL is off."
}
```

**Behavior**
- Ensures `queue/<subsystem>/` exists.
- Writes a problem report file:  
  `queue/engine/PROB_YYYYMMDD_HHMMSS.txt`
- Returns the file path for UI correlation.

**Response**
```json
{
  "status": "queued",
  "file": "engine/PROB_20251107_193045.txt"
}
```

---

### `GET /api/status`

Returns a **snapshot** of JarvisAgent runtime state.

```json
{
  "state": "SendingQueries",
  "outputs": 4,
  "inflight": 2,
  "completed": 12,
  "uptime": "00:12:43"
}
```

---

## 🔌 WebSocket API (`/ws`)

Real-time channel for status, progress, and answers.

**Event Types:**

| Type | When | Payload |
|------|------|---------|
| `status` | State change or periodic pulse | `{ state, inflight, completed }` |
| `update` | A file job changes state | `{ file, status }` |
| `response` | Job produced user-facing text | `{ subsystem, file, text }` |
| `error` | Recoverable issue | `{ message, detail? }` |

---

## 💬 Bot Frontend (Web UI)

**Stack**
- HTML5 + Tailwind CSS  
- Vanilla JS  
- WebSocket transport  

**Responsibilities**
- Submit subsystem + message via `POST /api/chat`.
- Maintain WebSocket session.
- Render chat transcript and progress indicators.
- Display Markdown-capable responses.

**Message Flow**
1. Browser submits `POST /api/chat`.
2. Web server writes `PROB_*.txt` to queue.
3. Core detects file, builds prompt, dispatches AI call.
4. AI reply written to `.output.txt` or `.output.md`.
5. Web server pushes results via WebSocket.
6. UI displays answer.

---

## 🔀 Message Flow Overview

```
Browser (Bot UI)
   │ POST /api/chat { subsystem, message }
   ▼
Web Server (crow)
   │ writes PROB_*.txt
   ▼
JarvisAgent Core
   │ builds prompt (STNG + CNTX + TASK + PROB)
   │ async HTTP to GPT-4/5
   ▼
AI Backend
   │ JSON response
   ▼
JarvisAgent Core
   │ writes *.output.txt / *.output.md
   ▼
Web Server
   │ WebSocket push {response}
   ▼
Browser
   │ render answer
```
