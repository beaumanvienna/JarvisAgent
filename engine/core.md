# JarvisAgent Engine Core Documentation

## Overview

The **engine core** provides lifecycle management, event dispatch, multithreading, logging redirection, terminal UI, and integration with the JarvisAgent application layer. It is responsible for initializing subsystems, running the main loop, and shutting down cleanly.

This document describes:
- `Core`
- engine startup (`engine()` / `main`)
- application contract (`Application`)
- logging macros
- key responsibilities and execution flow

---

## Core Responsibilities

### 1. Initialize global systems
- Install SIGINT handler (`Core::SignalHandler`).
- Disable CTRL‑C echo on terminals.
- Construct `TerminalManager` and redirect `std::cout` / `std::cerr` to `TerminalLogStreamBuf`.
- Open `/tmp/log.txt` for file logging.
- Create engine + application spdlog loggers (`Core::g_Logger`).

### 2. Load and validate configuration
- JSON config is parsed via:
  ```cpp
  ConfigParser parser("./config.json");
  parser.Parse(engineConfig);
  ConfigChecker().Check(engineConfig);
  ```
- On failure: engine returns `EXIT_FAILURE`.

### 3. Start engine subsystems (`Core::Start`)
- Store engine config.
- Initialize the thread pool:
  ```
  m_ThreadPool.Reset(maxThreads + THREADS_REQUIRED_BY_APP);
  ```
- Start keyboard input thread (`KeyboardInput::Start()`).
- Initialize terminal UI (`TerminalManager::Initialize()`).

### 4. Main loop (`Core::Run`)
Executed until `Application::IsFinished()` returns true:
1. Call application update callback:
   ```cpp
   app->OnUpdate();
   ```
2. Drain and dispatch events from `EventQueue`:
   - Pop all events.
   - Dispatch engine‑handled events (`AppErrorEvent`).
   - Forward unhandled events to the application:
     ```cpp
     app->OnEvent(eventPtr);
     ```
3. Render terminal UI:
   ```
   m_TerminalManager->Render();
   ```
4. Sleep for configured duration to prevent CPU overuse.

### 5. Shutdown (`Core::Shutdown`)
- Stop keyboard input.
- Run `CurlWrapper::GlobalCleanup()`.
- Shutdown terminal manager.
- Wait for all thread pool tasks (`ThreadPool::Wait()`).
- Flush and restore `std::cout` / `std::cerr`.
- Destroy terminal buffer and close log file.

---

## Event Handling

### Pushing events
Any subsystem pushes events via:
```cpp
Core::g_Core->PushEvent(std::make_shared<EventType>(args));
```

### Dispatch in main loop
`EventDispatcher` checks type and marks handled; unhandled events are passed to the application.

Example of engine‑handled event:
```cpp
dispatcher.Dispatch<AppErrorEvent>([](AppErrorEvent& e) {
    LOG_CORE_CRITICAL("Engine handled AppErrorEvent {}", e.GetErrorCode());
    return true;
});
```

---

## Logging Integration

### Unified logging pipeline
All logs (engine, application, Python) flow through:
1. `std::cout` → `TerminalLogStreamBuf`
2. Curses terminal window
3. `/tmp/log.txt`

### Logger access
```
LOG_CORE_INFO(...)
LOG_APP_ERROR(...)
```

### Assertions
```
CORE_ASSERT(condition, message)
CORE_HARD_STOP(message)
```

---

## Application Contract

Applications must implement:

```cpp
class Application {
public:
    virtual void OnStart() = 0;
    virtual void OnUpdate() = 0;
    virtual void OnEvent(std::shared_ptr<Event>&) = 0;
    virtual void OnShutdown() = 0;
    virtual bool IsFinished() const = 0;
};
```

JarvisAgent uses `JarvisAgent::Create()` to construct the app.

---

## Engine Entry Points

### `engine(argc, argv)`
Responsible for:
- Creating `Core`
- Parsing + validating config
- Running application lifecycle
- Calling `Core::Shutdown` and returning proper exit code

### `main()`
Delegates entirely to `engine()`.

---

## Summary

The engine core:
- Owns all system initialization and shutdown.
- Runs the main event loop.
- Drives threading, logging, terminal UI.
- Bridges Python and C++ via the event system.
- Provides a clean separation between engine duties and application logic.

This is the central orchestration layer of JarvisAgent.

