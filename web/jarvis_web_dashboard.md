# JarvisAgent Web Dashboard — Technical Documentation

This document describes the **exact behavior** of the JarvisAgent web interface (`index.html`).  
It is concise, accurate, and directly reflects the provided code.

---

# 1. Purpose

The dashboard is a **WebSocket-driven UI** for monitoring and interacting with JarvisAgent.  
It displays:

- Engine debug log  
- Input field for sending chat messages  
- List of AI answers  
- Real-time session boxes (task activity)  
- Python Online/Offline indicator  
- Quit button (sends shutdown request to engine)

Communication occurs via:

```
ws://localhost:8080/ws
```

---

# 2. Layout Overview

### Main UI Sections
1. **Debug Log Panel**  
   `<div id="log">`  
   Receives connection info, raw server messages, and local UI activity.  
   Auto-scrolls on new entries.

2. **Input Area**  
   - Text input: `#input`  
   - Send button: `#sendBtn`  
   Sends JSON messages to engine:
   ```json
   { "type": "chat", "subsystem": "ICE", "message": "..." }
   ```

3. **Answer Panel**  
   `<div id="answers">`  
   Prepend-only list of generated “answer cards”.

4. **Python Status Indicator**  
   `<span id="pythonStatus">`  
   Visible only when engine reports Python offline.

5. **Quit Button**  
   Sends:
   ```json
   { "type": "quit" }
   ```

6. **Session Boxes**  
   Draggable overlays showing live task state per engine session.

---

# 3. WebSocket Protocol

### 3.1 Outgoing Messages (UI → Engine)

| Trigger | Message |
|--------|---------|
| Send button | `{ type: "chat", subsystem: "ICE", message: "<text>" }` |
| Quit button | `{ type: "quit" }` |

### 3.2 Incoming Messages (Engine → UI)

The handler:
```js
ws.onmessage = (event) => { ... }
```

Processes the following message types:

#### **type: "status"**
Updates a session box.

Required fields:
- `name`
- `state`
- `outputs`
- `inflight`
- `completed`

#### **type: "output"**
A normal AI answer.

```json
{ "type": "output", "id": X, "text": "..." }
```

Displayed as an *answer card*.

#### **type: "timeout"**
Message expired before receiving an answer.  
Displayed as a red-styled card.

#### **type: "late-answer"**
Answer arrived after expiration.  
Displayed as a yellow-styled card.

#### **type: "python-status"**
Toggles Python warning indicator.

```json
{ "type": "python-status", "running": true|false }
```

#### Other messages  
Printed raw in the debug log.

---

# 4. Answer Cards

Created by JS helpers:

- `showAnswer(msg)`
- `showTimeout(msg)`
- `showLateAnswer(msg)`

Each card includes:
- Header (ID + context)
- Body (text)
- Color-coded styling depending on case

Cards are inserted at the top of `#answers`.

---

# 5. Session Boxes

Created dynamically when first “status” message for a session arrives.

Each box contains:
- Session name  
- Current state  
- Outputs count  
- In-flight tasks  
- Completed tasks  
- Spinner animation (active when inflight > 0)

### 5.1 Draggable Behavior

`makeDraggable(el)` implements:
- `mousedown` → start move  
- `mousemove` → reposition  
- `mouseup` → stop move  

Boxes default to stacked vertical positions based on their creation order.

### 5.2 Spinner Animation

A timed interval updates spinner glyphs:

```js
⣾ ⣽ ⣻ ⢿ ⡿ ⣟ ⣯ ⣷ …
```

Spinner visible only when `inflight > 0`.

---

# 6. Python Online/Offline Warning

Element:

```html
<span id="pythonStatus">⚠️ Python Offline</span>
```

Toggled by:

```js
updatePythonStatus(running)
```

When `running === false`, the warning becomes visible.

---

# 7. Styling (Summary)

Global theme uses JarvisAgent colors:

- Background: `#1e1e1e`
- Text: `#44a01c`
- Panels: dark gray `#111`
- Answer cards: varying highlight colors depending on message type
- Session boxes: draggable floating windows with white borders

---

# 8. Summary

| Feature | Description |
|---------|-------------|
| WebSocket interface | Full-duplex communication with engine |
| Command input | Chat messages sent to subsystem `"ICE"` |
| Answer panel | Prepend-style message display with color coding |
| Timeout / late-answer | Special handling and UI feedback |
| Session boxes | Live engine activity representation, draggable |
| Python status | Real-time offline indicator |
| Quit button | Sends a shutdown request to engine |

---

