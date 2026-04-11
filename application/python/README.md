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
- Release Python references safely under the GIL (note: the current implementation does **not** call `Py_Finalize()`; the interpreter remains initialized for the process lifetime).

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

4. **Workflow task execution (`ExecuteWorkflowTask`)**
   - Runs **synchronously on the calling thread** (not on the worker thread).
   - Parses task params JSON for `module` + `function`.
   - Calls the Python function with `inputValues` as keyword arguments.
   - Optionally provides a `context` dict (see below).

5. **Shutdown**
   - Enqueues Python `OnShutdown()` (best-effort; see `Stop()` notes below).
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

### **bool Initialize(std::string const& scriptPath)**  
**Implements:**
- Start CPython (`Py_Initialize`). Returns `true` on success (or if already running), `false` on failure.
- Redirect Python stdout/stderr to the JarvisAgent logger (implemented via `ctypes.CDLL(None)` calling `JarvisRedirectPython`).
- Extract script directory + module name.
- Add script folder to `sys.path`.
- Import module using CPython API.
- Retrieve `OnStart`, `OnUpdate`, `OnEvent`, `OnShutdown` if defined.
- Launch worker thread.
- Release GIL so the worker thread can reacquire it.

---

### **Stop()**  
**Implements:**
- Enqueue Python `OnShutdown` **(best-effort)**.
- Signal worker to stop and join worker thread.
- Acquire GIL and safely `DECREF` Python objects.
- Mark engine as not running.
- Note: does **not** call `Py_Finalize()`.

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

### **ExecuteWorkflowTask(TaskDef const& taskDefinition, ...)**
**Implements:**
- Runs a workflow task **synchronously on the calling thread** under the GIL.
- Parses `taskDefinition.m_ParamsJson` (JSON) for string fields `module` and `function`.
- Imports the module, resolves the callable, and invokes it with `inputValues` as keyword arguments.
- If the task declares an input named `context`, a `context` dict is provided as a keyword argument.
- If `context` is not declared but the first call fails with an error that looks like a missing `context`, the call is retried once with `context` attached.
- The `context` dict contains the provided `contextValues` plus reserved keys:
  - `_task_id`
  - `_task_working_directory`
- Treats a return value of `None` as success with no outputs.
- If a dict is returned, outputs are extracted as UTF-8 strings into `outputValuesOut`.

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
- The worker thread runs Python lifecycle hooks (`OnStart`, `OnUpdate`, `OnEvent`, `OnShutdown`).
- Workflow tasks (`ExecuteWorkflowTask`) run Python code **on the calling thread** (synchronously) under the GIL.
- C++ threads may enqueue tasks at any time.
- GIL is handled automatically using `PyGILState_Ensure()` / `PyGILState_Release()`.

### Error Handling
- Any Python exception prints to stdout, which is redirected to JarvisAgent logs.
- Missing hooks are explicitly logged but not treated as errors.
- The C entry point `JarvisPyStatus(...)` can be called from Python code to emit a `PythonCrashedEvent` into the engine event queue.

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

