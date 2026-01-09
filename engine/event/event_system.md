# JarvisAgent Event System Documentation

## Overview
The event system provides a lightweight, thread‑safe, category‑based mechanism for delivering asynchronous events from the engine (C++) to the application layer and to the embedded Python scripting engine.

It is composed of:
- **Event base class**
- **Typed events** (FileAdded, EngineEvent, PythonCrashed, etc.)
- **EventDispatcher**
- **EventQueue**
- **C++ → Python event dictionary conversion (PythonEngine)**

JarvisAgent uses the system to propagate:
- File system events (from FileWatcher)
- Engine events (Shutdown, errors)
- Python crash notifications
- Input events (optional)
- Timer events (optional)

---

## Event Base Class

All events derive from:

```cpp
class Event {
public:
    virtual EventType GetEventType() const = 0;
    virtual const char* GetName() const = 0;
    virtual int GetCategoryFlags() const = 0;
    bool IsHandled() const;
};
```

Each event type declares:
- Its **static type ID**
- Its **category flags**
- Optional payload (paths, error codes, messages)

Macros:

```
EVENT_CLASS_TYPE(type)
EVENT_CLASS_CATEGORY(category)
```

---

## Event Types

### File System
- **FileAddedEvent(path)**
- **FileRemovedEvent(path)**
- **FileModifiedEvent(path)**  
All carry a `.GetPath()` method.

### Engine
- **EngineEvent(EngineEventShutdown)**  
Used to shut down JarvisAgent safely.

### Python
- **PythonCrashedEvent(message)**  
Triggered by Python engine failures.

### App Errors
- **AppErrorEvent(AppErrorBadCurl)**  
Used to signal CURL failures.

### Timer
- **TimerElapsedEvent(timerID)**

### Input
- **KeyPressedEvent(key)**

---

## Event Categories

```
EventCategoryKeyboard
EventCategoryMouse
EventCategoryTimer
EventCategoryFileSys
EventCategoryApp
EventCategoryEngine
```

Each event can belong to one or more categories.

---

## EventDispatcher

`EventDispatcher` performs type‑safe dispatch:

```cpp
EventDispatcher dispatcher(event);
dispatcher.Dispatch<FileAddedEvent>([](FileAddedEvent& e) {
    ...
});
```

If the type matches, the lambda is executed and the event is marked handled.

---

## EventQueue (Thread‑Safe)

Engine threads push events:

```cpp
m_EventQueue.Push(std::shared_ptr<Event>(...));
```

Main loop pops them:

```cpp
auto events = m_EventQueue.PopAll();
```

Implementation uses:
- `std::mutex`
- `std::queue`
- Move semantics for shared_ptr events.

---

## Engine → Application flow

Inside `Core::Run()`:

1. `app->OnUpdate()` executes.
2. All pending events are popped.
3. Engine handles engine‑level events (shutdown, errors).
4. Unhandled events are forwarded to:
   ```cpp
   app->OnEvent(eventPtr);
   ```

---

## C++ → Python event conversion

`PythonEngine::OnEvent()` enqueues delivery to Python.

Conversion code (`BuildEventDict`):

Python receives:
```python
{
    "type": "FileAdded",
    "path": "/full/path/to/file"
}
```

Only filesystem events include `"path"`.

Python script handles them via:

```python
def OnEvent(event):
    if event["type"] == "FileAdded":
        ...
```

---

## Summary

The JarvisAgent event system provides:
- Lightweight typed events
- Thread‑safe delivery from any thread
- Automatic dispatching
- Full integration with Python via dictionaries
- Support for file, engine, error, input, and timer events

It forms the backbone of real‑time communication between engine components, the application, and the Python automation layer.
