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
- Install signal handlers via `Core::InstallSignalHandlers()`:
  - `SIGINT` and `SIGTERM` → `Core::SignalHandler` (graceful shutdown; second
    arrival force-exits via `_exit(EXIT_FAILURE)`).
  - `SIGPIPE` → `SIG_IGN` (libcurl runs with `CURLOPT_NOSIGNAL=1`, so a
    socket closed mid-write would terminate the process by default; the
    EPIPE surfaces to libcurl as `CURLE_SEND_ERROR` instead).
  - POSIX uses `sigaction` with both signals blocked during handler
    execution and `SA_RESTART` cleared so the engine main-loop sleep
    wakes promptly; Windows uses `signal()` (no `sigaction` available).
  - The handler discipline (async-signal-safe operations only — no
    `LOG_*`, no heap, no mutex) is documented in the comment block on
    `Core::SignalHandler` in `engine/core.cpp`.
- Disable CTRL‑C echo on terminals.
- Construct `TerminalManager` and redirect `std::cout` / `std::cerr` to `TerminalLogStreamBuf`.
- Open `log/log.txt` for file logging.
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
- Start OAuth token refresh loop (`OAuthTokenManager::Start()`).

### 4. Main loop (`Core::Run`)
Executed until `Application::IsFinished()` returns true:
1. Poll the signal flags via `CheckSignalFlags()` — pushes an `EngineEventShutdown` if a SIGINT or SIGTERM arrived since the last iteration. Flags are `volatile std::sig_atomic_t` (POSIX-blessed for signal-handler ↔ main-thread communication; `std::atomic<bool>` is lock-free in practice but not standard-guaranteed async-signal-safe). A two-flag dance (`s_ShutdownRequested` transient + `s_ShutdownAcked` sticky) preserves the "second press force-exits" escape hatch through both rapid double-press and post-ack double-press races.
2. Drain and dispatch events from `EventQueue` (done **before** `OnUpdate` so quit/SIGINT is processed promptly):
   - `m_EventQueue.PopAll()` swaps the underlying queue out under the mutex (O(1)) and drains to a local vector outside the lock — producer threads aren't blocked by main-thread housekeeping.
   - Dispatch engine‑handled events (`AppErrorEvent`).
   - Forward unhandled events to the application:
     ```cpp
     app->OnEvent(eventPtr);
     ```
3. Call application update callback:
   ```cpp
   app->OnUpdate();
   ```
4. Render terminal UI:
   ```
   m_TerminalManager->Render();
   ```
5. Sleep for configured duration to prevent CPU overuse.

### 5. Shutdown (`Core::Shutdown`)
- Stop OAuth token refresh loop (`OAuthTokenManager::Stop()`).
- Stop keyboard input.
- **Two-phase thread pool shutdown** — `m_ThreadPool.RequestStop()` flips the stop flag (curl progress callbacks observe it and abort in-flight transfers; `SubmitTask` short-circuits new submissions); the dispatcher and other long-running async work is given a chance to wind down; then `m_ThreadPool.Shutdown()` takes `m_Mutex` (atomic with concurrent SubmitTask), drains queued tasks via `m_Pool.wait()`, and is idempotent on repeat calls (guarded by a separate `m_ShutdownDrained` atomic so it doesn't conflate with the earlier RequestStop's flag flip).  See `engine/auxiliary/auxiliary.md` Section 2 for the full lifecycle gate semantics.
- Run `CurlWrapper::GlobalCleanup()`.
- Shutdown terminal manager.
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
    Application() = default;
    virtual ~Application() = default;

    // Non-copyable, non-movable: polymorphic-base slicing prevention.
    Application(Application const&) = delete;
    Application& operator=(Application const&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    virtual void OnStart() = 0;
    virtual void OnUpdate() = 0;
    virtual void OnEvent(std::shared_ptr<Event>&) = 0;
    virtual void OnShutdown() = 0;

    [[nodiscard]] virtual bool IsFinished() const = 0;

    // Set by derived OnStart() on a fatal-start path.  If non-empty after
    // OnShutdown returns, the engine surfaces it on stderr before the
    // process exits.
    [[nodiscard]] std::string const& GetFatalStartupMessage() const;

protected:
    std::string m_FatalStartupMessage;
};
```

JarvisAgent uses `JarvisAgent::Create()` to construct the app.

The engine drives the four lifecycle hooks in fixed order:

```
OnStart()                         // once, blocking, may throw
while (!IsFinished())
{
    for each pending event: OnEvent(event)
    OnUpdate()
}
OnShutdown()                      // once, blocking
// stderr-surface GetFatalStartupMessage() if non-empty
```

All four hooks run on the engine's main thread; never concurrent with each other.  Implementations may spawn worker threads internally and own the synchronisation discipline for shared state with those threads (see `JarvisAgent`'s threading + lifetime contract block on `application/jarvisAgent.h` for the canonical pattern, including the `App::g_App` `std::atomic<JarvisAgent*>` global with acquire/release ordering).

The engine does NOT skip OnShutdown when OnStart sets a fatal message — the derived class must arrange OnShutdown to be safe-after-partial-OnStart (typically: nullptr-guard each subsystem reset).

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

---

## Core-Owned Subsystems

`Core` owns and provides access to these subsystems via getter methods:

| Member | Getter | Purpose |
|--------|--------|---------|
| `m_ThreadPool` | `GetThreadPool()` | Shared thread pool for async work |
| `m_KeyManager` | `GetKeyManager()` | Encrypted credential storage (API keys, OAuth, key pairs) |
| `m_OAuthTokenManager` | `GetOAuthTokenManager()` | OAuth2 token lifecycle and background refresh |
| `m_CloudConnectionManager` | `GetCloudConnectionManager()` | In-memory CRUD for cloud connection configs |
| `m_CloudConnectorRegistry` | `GetCloudConnectorRegistry()` | Registry of cloud connector plugins |
| `m_CloudCircuitBreaker` | `GetCloudCircuitBreaker()` | Per-connection circuit breaker for cloud services |
| `m_EngineConfig` | `GetConfig()` | Parsed `config.json` settings |
| `m_TerminalManager` | `GetTerminalManager()` | Curses-based terminal UI |

---

## Directory Structure

```
engine/
  core.h / core.cpp              — Core class, lifecycle, event loop
  engine.h / engine.cpp          — Entry point, logger setup, engine() function
  application.h                  — Application interface contract
  event/                         — EventQueue, EventDispatcher, event types
  log/                           — Log, TerminalManager, TerminalLogStreamBuf, SecretRedactor
  json/                          — ConfigParser, ConfigChecker
  keys/                          — KeyManager, KeyEncryption, OAuthTokenManager, JwtGenerator
  auxiliary/                     — ThreadPool (two-phase shutdown gate: RequestStop → Shutdown), file utilities
  input/                         — KeyboardInput
  curlWrapper/                   — CurlWrapper (sync) + CurlMultiDispatcher (async HTTP/2), IAuthSigner family (Bearer / x-goog-api-key / x-api-key / api-key / SigV4), SigV4Signer (awsSigV4), credValidation, RateLimitController + RateLimitStrategy (per-(host, modelFamily) AIMD)
```

