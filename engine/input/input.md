# JarvisAgent Engine – Input Module Documentation

This document describes the **KeyboardInput** subsystem located in `engine/input/`.

---

## Overview

`KeyboardInput` provides non-blocking, cross‑platform keyboard monitoring for the engine.  
It runs in its own background task (via the engine's thread pool) and converts key presses into engine events.

Supported features:

- Raw terminal mode on Linux/macOS (non‑canonical, no‑echo)
- `_kbhit()` polling on Windows
- Emits:
  - `EngineEventShutdown` when user presses **q** or **Q**
  - `KeyPressedEvent` for any other printable character
- Integrates directly with the engine event queue (`Core::PushEvent`)

---

## Public API

```cpp
class KeyboardInput
{
public:
    KeyboardInput();
    ~KeyboardInput();

    void Start();
    void Stop();

private:
    void Listen();

    std::atomic<bool> m_Running{false};
    std::future<void> m_ListenerTask;
};
```

### `Start()`

- If already running, returns immediately.
- Marks the input system as running.
- Submits a background task to the engine’s thread pool:

```cpp
m_ListenerTask = Core::g_Core->GetThreadPool().SubmitTask([this]() { Listen(); });
```

### `Stop()`

- Sets `m_Running = false`.
- If the listener task exists:
  - Calls `.wait()` to ensure clean exit.
  - Logs `"Keyboard input stopped"`.

---

## Internal Operation (`Listen()`)

### Linux / macOS branch

- Saves current terminal configuration (`tcgetattr`).
- Enables raw mode:
  - disables canonical input (`ICANON`)
  - disables echo (`ECHO`)
- Logs: `"Keyboard input active. Press 'q' to quit."`
- Loop:
  - Uses `select()` with a 100 ms timeout to poll stdin.
  - If a character arrives:
    - `'q'` or `'Q'` → push `EngineEventShutdown`
    - Any other char (except `
` and EOF) → push `KeyPressedEvent`
- Restores original terminal settings on exit.

### Windows branch

- Uses `_kbhit()` + `_getch()`
- Same key handling logic as Linux
- Sleeps 100 ms per iteration to avoid busy‑looping.

---

## Events Produced

### Quit Event

```cpp
auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
Core::g_Core->PushEvent(event);
```

Triggered by user pressing **q** or **Q**.

### Key Pressed Event

```cpp
auto event = std::make_shared<KeyPressedEvent>(static_cast<char>(ch));
Core::g_Core->PushEvent(event);
```

Triggered by any single printable character.

---

## Threading Behavior

- Keyboard listener runs entirely inside a single worker thread.
- The main engine loop only receives events via `Core::PushEvent`.
- No locks are required; synchronization is handled via:
  - `std::atomic<bool> m_Running`
  - The thread pool’s task lifetime management.

---

## Summary

`KeyboardInput` is a minimal, reliable, non‑blocking input subsystem used primarily for:

- **Graceful shutdown** via keystroke
- **Live command/event injection** during engine execution

It requires no additional libraries, uses platform‑appropriate APIs, and integrates cleanly with the engine’s existing event and thread‑pool systems.
