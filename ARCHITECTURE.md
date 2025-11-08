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


# ARCHITECTURE_DETAILS

<br>

This document expands the operational details for JarvisAgent’s web interface and runtime communication.

---

## 🔐 Design Goals

- **Transparent pipeline** — Every step materialized as a file for traceability and offline auditing.
- **Event-driven** — File watcher + selective rebuilds; no redundant work.
- **Embeddable** — Lightweight, single-binary server using `cpp-httplib`.
- **Extensible** — Python scripting hooks enable custom preprocessing (e.g., MarkItDown).
- **Operator-friendly** — Terminal status line + browser chat UI with live updates.
- **Binary-safe** — Non-text files are detected and ignored or preprocessed by the scripting layer.

---

## 🧠 Communication Protocol Summary

| Channel | Direction | Technology | Payload |
|--------|-----------|------------|--------|
| Browser ↔ WebServer | Bidirectional | **WebSocket** | JSON events (chat, status, responses, errors) |
| WebServer → JarvisCore | One-way | **Shared filesystem (queue/)** | Text files (`STNG/ CNTX/ TASK/ PROB/ REQ/`) |
| JarvisCore ↔ AI Backend | Request/Response | **HTTP REST (libcurl)** | OpenAI-style JSON |
| JarvisCore → WebServer | Event-driven | **Filesystem change detection** | Output files trigger WS pushes |
| WebServer → Browser | Push | **WebSocket** | Responses, status, errors |

---

## 🌐 Embedded Web Server

**Library:** [`cpp-httplib`](https://github.com/yhirose/cpp-httplib) (header-only, HTTPS-capable)

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
**Description**  
Queues a user message for a **specific subsystem** by creating a file in the `queue/<subsystem>` folder.

**Request**
```json
{
  "subsystem": "engine",
  "message": "Engine knocks at idle after warmup; MIL is off."
}

**Behavior**
- Ensures `queue/<subsystem>/` exists.
- Writes a problem report file:  
  `queue/engine/PROB_YYYYMMDD_HHMMSS.txt`
- Returns the logical file path for client-side correlation.

**Response**
```json
{
  "status": "queued",
  "file": "engine/PROB_20251107_193045.txt"
}


# ARCHITECTURE_DETAILS

## GET /api/status
**Description**  
Provides a **snapshot** of Jarvis runtime state for dashboards or health checks.

**Behavior**
- Reads counters and state from in-process metrics (state machine, futures, counters).
- Returns totals since process start.

**Response**
```json
{
  "state": "SendingQueries",
  "outputs": 4,
  "inflight": 2,
  "completed": 12,
  "uptime": "00:12:43"
}
```

**Field Notes**
- `state`: One of `CompilingEnvironment | SendingQueries | AllQueriesSent | AllResponsesReceived`.
- `outputs`: Count of output files present/known.
- `inflight`: Futures currently running.
- `completed`: Queries completed in this process lifetime.
- `uptime`: HH:MM:SS (human-friendly string).

---

## 🔌 WebSocket API (`/ws`)

**Description**  
Real-time channel for **status**, **progress**, and **answers**. The server **pushes** updates when files complete or state changes.

### Event Types

| Type | When | Payload |
|------|------|---------|
| `status` | State change or periodic pulse | `{ state, inflight, completed }` |
| `update` | A specific file job transitions state | `{ file, status }` |
| `response` | A job produced user-facing text | `{ subsystem, file, text }` |
| `error` | Recoverable issue or failure | `{ message, detail? }` |

### JSON Schemas (minimal)

```json
// status
{ "type": "status", "state": "SendingQueries", "inflight": 3, "completed": 5 }

// update
{ "type": "update", "file": "engine/PROB_20251107_193045.txt", "status": "completed" }

// response
{
  "type": "response",
  "subsystem": "engine",
  "file": "engine/PROB_20251107_193045.txt",
  "text": "Likely detonation from advanced timing. Check knock sensor and base timing."
}

// error
{ "type": "error", "message": "Failed to parse AI response.", "detail": "No 'content' array." }
```

**Notes**
- `status.status` is not used; `status` events convey state via `state` + counters.
- `update.status` values: `queued | running | completed | failed`.
- `response.text` is Markdown-capable; render as rich text in UI if desired.

---

## 💬 Bot Frontend (Web UI)

**Software Stack**
- **UI:** HTML5 + Tailwind CSS  
- **Logic:** Vanilla JavaScript  
- **Transport:** WebSocket (JSON)  
- **State:** In-memory (per tab); no backend persistence required

**Responsibilities**
- Collect subsystem + problem report from user.
- Submit via `POST /api/chat`.
- Maintain a WebSocket connection for live updates.
- Render chat history, show pending indicators/spinner.
- Display errors unobtrusively with retry affordances.

**Message Flow**
1. **User submits** subsystem + message.
2. **Browser → WebServer**: `POST /api/chat`.
3. **WebServer → Filesystem**: writes `PROB_*.txt` in `queue/<subsystem>/`.
4. **JarvisCore** detects new file, **dispatches AI query**.
5. **JarvisCore → Filesystem**: writes `*.output.txt`.
6. **WebServer** detects changed files and **pushes** `response` via WebSocket.
7. **Browser** appends AI answer in the transcript.

---

## 🔀 Message Flow Overview

```
Browser (Bot UI)
   │   POST /api/chat { subsystem, message }
   ▼
Web Server (cpp-httplib)
   │   write queue/<subsystem>/PROB_*.txt
   ▼
Jarvis Core (Watcher + Dispatcher)
   │   build prompt (STNG+CNTX+TASK + PROB)
   │   async HTTP → GPT-4/5
   ▼
AI Backend
   │   JSON reply
   ▼
Jarvis Core
   │   write *.output.txt
   ▼
Web Server
   │   WS push {type:"response", ...}
   ▼
Browser (UI updates)
```
