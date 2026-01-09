# JarvisAgent Documentation (JarvisAgent Core Application)

## Overview

`JarvisAgent` is the main application class that coordinates all subsystems:
- File watching
- Session management
- Python scripting
- Web server
- Chat message handling
- Terminal status rendering

It inherits from `Application` and implements the full application lifecycle.

---

# Responsibilities

- Initialize and start all subsystem components
- Dispatch incoming filesystem and engine events
- Maintain per-session `SessionManager` instances (one per queue folder)
- Handle chat workflow (`PROB_*` and `PROB_*_output` files)
- Forward events to Python engine
- Integrate with terminal `StatusRenderer`
- Maintain application shutdown state

---

# Lifecycle Methods

## OnStart()

Executed once at application start:
- Capture startup timestamp
- Initialize terminal `StatusRenderer` callbacks
- Start:
  - `FileWatcher`
  - `WebServer`
  - `ChatMessagePool`
  - `PythonEngine`
- Trigger Python `OnStart()` hook if initialization succeeded

---

## OnUpdate()

Runs every frame of the engine loop:
- Update all `SessionManager` instances
- Remove expired chat messages
- Broadcast Python status (`python-running`) every 1 second
- Check termination state

---

## OnEvent(shared_ptr<Event>&)

Event router for:
- **EngineEvent** → shutdown
- **FileAdded / FileModified / FileRemoved** → feed into `SessionManager`
- **PythonCrashedEvent** → stop Python engine
- **PROB_* files**:
  - If stale (older than app startup) → ignore
  - If `PROB_*_output` → extract text and call `ChatMessagePool::MarkAnswered()`
- Forward all remaining events to Python

If a file belongs to a path not seen before, a new `SessionManager` is created for it.

---

## OnShutdown()

Clean termination:
- Stop Python engine, report python-running=false
- Stop FileWatcher
- Stop WebServer
- Call `OnShutdown()` on all SessionManagers
- Clear global `App::g_App`

---

## IsFinished()

Returns true if the app received an EngineEvent shutdown request.

---

# Subsystems Managed

### FileWatcher
Observes queue folders and produces `FileAdded`, `FileModified`, `FileRemoved` events.

### SessionManager
Per‑folder state machines that:
- Track environment/settings/context/tasks
- Dispatch queries
- Write output files
- Broadcast session status

### WebServer
Provides REST and websocket endpoints for:
- Pushing chat messages
- Querying session status
- Remote UI updates

### ChatMessagePool
Manages incoming chat texts and outgoing responses with timeout logic.

### PythonEngine
Loads `scripts/main.py`, handles Python hooks, delivers events.

### StatusRenderer
Draws dynamic ncurses terminal information:
- Session states
- Active queries
- Completed queries

---

# Application State

### Startup Timestamp
Used to filter out stale `PROB_*` files written before the current run.

### SessionManagers Map
Key: session directory path  
Value: unique `SessionManager` instance

---

# Application Class (Base)

```
class Application {
    virtual void OnStart() = 0;
    virtual void OnUpdate() = 0;
    virtual void OnEvent(std::shared_ptr<Event>&) = 0;
    virtual void OnShutdown() = 0;
    virtual bool IsFinished() const = 0;
};
```

JarvisAgent implements all of these.

---

# Summary

`JarvisAgent` is the central coordinator:
- Bridges engine → Python → web → filesystem
- Manages all dynamic subsystems
- Routes events correctly
- Provides the full execution pipeline for JarvisAgent’s automation workflow.

It is the top-level orchestrator of the entire application.

