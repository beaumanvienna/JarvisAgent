# WebServer Documentation (JarvisAgent)

## Overview

`WebServer` provides the HTTP and WebSocket interface for JarvisAgent.  
It uses the **Crow** C++ microframework to:

- Serve the dashboard UI (`index.html`)
- Receive chat messages over HTTP (`POST /api/chat`)
- Provide system status (`GET /api/status`)
- Maintain live WebSocket connections for real‑time updates
- Broadcast messages and Python runtime status to all websocket clients

---

## Functional Description

### Core Responsibilities

- Start/stop an embedded Crow web server
- Expose REST API routes
- Handle incoming WebSocket connections
- Maintain a set of active websocket clients
- Push asynchronous events (queued messages, status updates, Python status)
- Operate safely across threads using locks, atomics, and Crow’s own threading

---

## High‑Level Operation

### 1. Startup
`Start()`:

- Marks server as running
- Submits the Crow `.run()` call to the global `ThreadPool`
- Begins listening on port **8080**
- Enables multithreaded Crow request handling

### 2. Serving the Dashboard UI
Route:
```
GET /
```
Serves `web/index.html`.  
If missing → returns `404`.

### 3. API Endpoints

#### `POST /api/chat`
- Expects JSON: `{ "subsystem": "...", "message": "..." }`
- Parses using **simdjson**
- Writes a file into:  
  `queue/<subsystem>/ISSUE_<id>.txt`
- Adds message to ChatMessagePool
- Returns:
```
{"status": "queued", "id": <id>, "file": "<path>"}
```

#### `GET /api/status`
Currently returns a static JSON template:
```
{
  "type": "status",
  "name": "../queue/ICE",
  "state": "SendingQueries",
  "outputs": 4,
  "inflight": 1,
  "completed": 7
}
```

### 4. WebSocket `/ws`

#### On Connect
- Adds client connection to `m_Clients`
- Logs connection

#### On Disconnect
- Removes client from `m_Clients`
- Logs disconnection

#### On Message
Parses incoming JSON using simdjson:

##### `type == "chat"`
- Extracts subsystem + message text
- Stores chat entry in ChatMessagePool
- Writes file:  
  `queue/<subsystem>/PROB_<id>_<timestamp>.txt`
- Replies:
```
{"type": "queued", "id": <id>, "file": "<filename>"}
```

##### `type == "quit"`
- Pushes an EngineEventShutdown into the event system
- Sends acknowledgment:
```
{"type": "quit-ack", "message": "Shutdown initiated."}
```

##### Anything else
Responds:
```
{"error":"unknown type"}
```

---

## Member Function Requirements

### **Start()**
- Prevents duplicate startup
- Launches Crow server asynchronously on port **8080**
- Enables multithreading + signal suppression
- Logs startup

### **Stop()**
- Signals Crow to stop
- Waits on running task
- Logs shutdown

---

### **Broadcast(std::string const& jsonMessage)**
- Sends arbitrary JSON text to **all** active websocket clients

### **BroadcastJSON(std::string const& jsonString)**
Alias of `Broadcast`, separated for readability.

### **BroadcastPythonStatus(bool pythonRunning)**
Sends:
```
{"type":"python-status","running":true/false}
```
to all websocket clients.

This is used by the dashboard to show:
- 🟢 “Python online”
- 🔴 “Python offline”

---

## Internal Helpers

### RegisterRoutes()
Creates:
- `/` → static HTML
- `/api/chat` → POST
- `/api/status` → GET

### RegisterWebSocket()
Creates the websocket at `/ws`, and attaches:
- `.onopen`
- `.onclose`
- `.onmessage`

### HandleChatPost()
- Parses JSON with simdjson
- Writes message to filesystem
- Returns structured JSON response

### HandleStatusGet()
- Returns current processing status (currently static template)

---

## Thread‑Safety Notes

- `m_Clients` is guarded by `m_Mutex`
- `m_Running` is atomic
- Crow handles request threading
- Broadcasting locks the client set only during iteration

---

## Summary

`WebServer` is a multithreaded, Crow‑based communication hub that connects the JarvisAgent engine with the browser UI. It supports REST APIs for message ingestion and provides WebSocket channels for real‑time updates such as chat messages, subsystem state, and Python runtime status.

It forms the backbone of the web dashboard system and is tightly integrated with:
- File‑queue system
- ChatMessagePool
- Engine event system
- PythonEngine (for status updates)

---

