# PythonEngine Documentation (JarvisAgent)

## Overview

`PythonEngine` embeds a full CPython interpreter inside JarvisAgent.  
It loads a Python automation script, discovers lifecycle hooks, redirects Python stdout/stderr into the JarvisAgent terminal, and processes events asynchronously via a dedicated worker thread.

---

## Functional Description

### Core Responsibilities

- Initialize and manage a CPython interpreter.
- Import a user‑provided script (e.g., `scripts/main.py`).
- Discover optional hook functions:  
  **OnStart**, **OnUpdate**, **OnEvent**, **OnShutdown**.
- Redirect Python stdout/stderr via `JarvisRedirectPython()`.
- Dispatch tasks asynchronously using a dedicated worker thread and task queue.
- Convert C++ events into Python dictionaries.
- Guarantee safe GIL (Global Interpreter Lock) handling.
- Cleanly shut down the interpreter and release all Python references.

### High‑Level Operation

1. **Initialize()**
   - Starts Python, configures stdout/stderr redirection, imports script, discovers hooks, starts worker thread.

2. **Task Dispatch**
   - Public API (`OnStart`, `OnUpdate`, `OnEvent`) enqueues tasks.
   - Worker thread acquires GIL, calls Python functions safely.

3. **Event Delivery**
   - Events are converted into Python dictionaries:
     ```python
     {
         "type": "FileAdded",
         "path": "path/to/file"
     }
     ```

4. **Shutdown**
   - Enqueues Python `OnShutdown()`.
   - Stops worker thread.
   - Releases Python references under GIL.

---

## Using PythonEngine in the Application

Typical usage pattern:

```cpp
PythonEngine pythonEngine;

pythonEngine.Initialize("scripts/main.py");
pythonEngine.OnStart();

// When filesystem or system events occur:
pythonEngine.OnEvent(eventPtr);

// Shutdown:
pythonEngine.Stop();
```

Python side (`main.py`):

```python
def OnStart():
    print("Python initialized.")

def OnEvent(event):
    print("Received:", event)
```

---

## Member Function Requirements

Below are the key member functions and the software requirements each one fulfills.

---

### **Initialize(std::string const& scriptPath)**  
**Implements:**
- Start CPython (`Py_Initialize`).
- Redirect Python stdout/stderr to the JarvisAgent logger.
- Extract script directory + module name.
- Add script folder to `sys.path`.
- Import module using CPython API.
- Retrieve `OnStart`, `OnUpdate`, `OnEvent`, `OnShutdown` if defined.
- Launch worker thread.
- Release GIL so the worker thread can reacquire it.

---

### **Stop()**  
**Implements:**
- Enqueue Python `OnShutdown`.
- Signal worker to stop.
- Join worker thread.
- Acquire GIL and safely decref all Python objects.
- Reset engine state.

---

### **OnStart()**  
**Implements:**
- Enqueue a Python task of type `OnStart`.

### **OnUpdate()**  
**Implements:**
- Enqueue a Python task of type `OnUpdate`  
  (no longer used in JarvisAgent, but still supported).

### **OnEvent(std::shared_ptr<Event>)**  
**Implements:**
- Package any C++ event into Python dictionary.
- Enqueue a Python `OnEvent` task.

---

### **WorkerLoop()**  
**Implements:**
- Wait for tasks using condition variable.
- Reacquire GIL with `PyGILState_Ensure()`.
- Call appropriate Python hook.
- Handle Python exceptions via `PyErr_Print`.
- Exit cleanly on stop.

---

### **EnqueueTask(PythonTask const& task)**  
**Implements:**
- Thread‑safe push into queue.
- Wake worker thread.

---

### **CallHook(PyObject*, char const*)**  
**Implements:**
- Call zero‑argument Python function.
- Log and print exceptions.

---

### **CallHookWithEvent(PyObject*, char const*, Event const&)**  
**Implements:**
- Build Python dict for event.
- Call Python function with argument.
- Handle errors gracefully.

---

### **BuildEventDict(Event const&)**  
**Implements:**
- Create a new Python dictionary.
- Insert `"type"` for all events.
- Insert `"path"` if it is a filesystem event.

---

### **Reset()**  
**Implements:**
- Release Python references.
- Clear script/module state.
- Mark engine as non‑running.

---

## Additional Notes

### Threading + GIL Safety
- Only the worker thread runs Python code.
- C++ threads may enqueue tasks at any time.
- GIL is handled automatically using `PyGILState_Ensure()` / `PyGILState_Release()`.

### Error Handling
- Any Python exception prints to stdout, which is redirected to JarvisAgent logs.
- Missing hooks are explicitly logged but not treated as errors.

### Output Redirection
All Python `print()` output is captured and routed to:

```
extern "C" void JarvisRedirectPython(char const* message)
```

This keeps logs unified inside JarvisAgent.

---

## Summary

`PythonEngine` is a fully asynchronous, robust CPython integration layer enabling JarvisAgent to run automation scripts safely and efficiently. It handles interpreter initialization, script loading, event delivery, logging, and lifecycle management—all without blocking the rest of the application.

It is the core mechanism that allows Python scripts to control JarvisAgent’s automation workflows.

