# ChatMessagePool Documentation (JarvisAgent)

## Overview

`ChatMessagePool` is a memory‑resident message registry used by JarvisAgent to track:
- Incoming chat requests
- Their lifecycle (created → answered → expired)
- Their dispatch into the filesystem queue
- Their delivery back to the browser over WebSockets

It provides fast, lock‑protected insertion and timed expiration of messages, and automatically grows its internal storage.

---

## Data Model

### ChatMessageEntry
Each logical chat message is stored as a struct:

```cpp
struct ChatMessageEntry {
    uint64_t id;
    std::string subsystem;
    std::string message;
    std::chrono::steady_clock::time_point timestamp;
    bool answered;
    bool expired;
};
```

### Internal Storage

- `m_Entries` — fixed‑size vector that grows by doubling size when needed
- `m_FreeIndices` — queue of reusable indices into `m_Entries`
- `m_NextId` — atomic unique ID generator
- `m_ActiveCount` — number of valid entries currently in use

---

## High-Level Behavior

### Lifecycle of a Message

1. **Added** via `AddMessage()`
2. **Stored** in `m_Entries[index]`
3. **Optionally answered** via `MarkAnswered()`
4. **Or expires** after 30 seconds via `RemoveExpired()`
5. **Then recycled** back into the free-index pool

### Growth Behavior

If free slots are exhausted and active/total ≥ grow threshold:

```
oldSize → oldSize * 2
```

New indices are pushed into `m_FreeIndices`.

---

## Public Interface

### **ChatMessagePool(size_t initialSize = 100, double growThreshold = 0.7)**
Initializes:
- Preallocates `initialSize` entries
- Fills the free-index queue
- Logs initialization

---

### **uint64_t AddMessage(subsystem, message)**  
**Implements:**
- Acquire lock  
- Grow pool if needed  
- Pop a free index  
- Fill a new `ChatMessageEntry`  
- Increase active count  
- Return unique message ID  

---

### **void MarkAnswered(id, answerText)**  
**Implements:**
- Acquire lock  
- Find entry with matching ID  
- If active and not expired:  
  - Mark `answered = true`  
  - Broadcast JSON:  
    ```
    {"type":"output","id":<id>,"text":<answer>}
    ```
  - Reset entry and recycle index  
- If not found (expired earlier):  
  - Log warning  
  - Broadcast:
    ```
    {"type":"late-answer","id":<id>,"text":<answer>}
    ```

---

### **void RemoveExpired()**
**Implements:**
- Called by `Update()` or periodically by engine  
- Expiration timeout = **30 seconds**  
- For each active entry:
  - If timed out:
    - Log expiration
    - Broadcast:
      ```
      {"type":"timeout","id":<id>,"text":"Message expired after 30 seconds."}
      ```
    - Reset entry and recycle index

---

### **void Update()**
Simply calls `RemoveExpired()`.

---

### **size_t Size() const**
Returns total capacity of the pool.

### **size_t ActiveCount() const**
Returns number of active (non-free) entries.

---

## Thread Safety

All operations touching message state use a **single mutex**:
- `AddMessage`
- `MarkAnswered`
- `RemoveExpired`

ID assignment uses an atomic counter.  
Broadcasting is delegated to the `WebServer`.

---

## Summary

`ChatMessagePool` is the central working memory for JarvisAgent’s chat subsystem.  
It provides:

- Fast, lock‑protected insertion  
- Automatic ID generation  
- Automatic expiration  
- Live WebSocket replies for:
  - Answered messages
  - Late answers
  - Timeouts  
- Automatic pool growth under load  

It is tightly integrated with:
- `WebServer` (broadcasting)
- `ChatMessagePool::AddMessage()` usage in HTTP/WebSocket handlers
- Filesystem queue generation  
