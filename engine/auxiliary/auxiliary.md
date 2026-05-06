# JarvisAgent Engine – Auxiliary Module Documentation

This document covers the **auxiliary module** of the JarvisAgent engine:

- `file.h / file.cpp` — filesystem helper utilities  
- `threadPool.h / threadPool.cpp` — wrapper around `BS::thread_pool`  
- `TracyClient.cpp` — Tracy profiler client integration

The descriptions below are based only on the actual JarvisAgent sources plus the upstream `BS::thread_pool` documentation.

---

## 1. Filesystem Utilities (`auxiliary/file.*`)

Namespace:

```cpp
namespace AIAssistant::EngineCore
```

### File existence

```cpp
bool FileExists(char const* filename);
bool FileExists(std::string const& filename);
bool FileExists(std::filesystem::directory_entry const& entry);
```

- Uses `std::ifstream` / `directory_entry::exists()` to determine whether a path refers to an existing file or directory.

### Directory checks

```cpp
bool IsDirectory(char const* filename);
bool IsDirectory(std::string const& filename);
```

- Wraps `std::filesystem::is_directory`.
- The `std::string` overload is exception-safe: any exception results in `false`.

### Path manipulation

```cpp
std::string GetFilenameWithoutPath(std::filesystem::path const& path);
std::string GetPathWithoutFilename(std::filesystem::path const& path);
std::string GetFilenameWithoutExtension(std::filesystem::path const& path);
std::string GetFilenameWithoutPathAndExtension(std::filesystem::path const& path);
std::string GetFileExtension(std::filesystem::path const& path);
```

- Thin wrappers over `path.filename()`, `path.parent_path()`, `path.stem()`, and `path.extension()`.
- On Windows, explicitly converts the resulting `std::filesystem::path` to `std::string` via `.string()`.

### Working directory

```cpp
std::string GetCurrentWorkingDirectory();
void SetCurrentWorkingDirectory(std::filesystem::path const& path);
```

- `GetCurrentWorkingDirectory()` returns `std::filesystem::current_path()` as `std::string`.
- `SetCurrentWorkingDirectory()` assigns `std::filesystem::current_path(path)`.

### Directory creation

```cpp
bool CreateDirectory(std::string const& filename);
```

- Uses `std::filesystem::create_directories`.
- On MSVC builds: returns `IsDirectory(filename)` after creation.
- On other platforms: returns the boolean result of `create_directories`.

### File copy

```cpp
bool CopyFile(std::string const& src, std::string const& dest);
```

- Opens `src` and `dest` as binary streams.
- Copies data via `destination << source.rdbuf()`.
- Returns `true` only if both streams are valid at the end.

### File size

```cpp
std::ifstream::pos_type FileSize(std::string const& filename);
```

- Opens file in `std::ifstream::ate | std::ifstream::binary` mode.
- Returns `tellg()` result (size in bytes).

### Add trailing slash

```cpp
std::string& AddSlash(std::string& filename);
```

- Ensures the path ends with a platform-appropriate separator:
  - `/` on non-MSVC builds
  - `\` on MSVC
- Only appends a separator if one is not already present.

### Newest timestamp

```cpp
fs::file_time_type GetNewestTimestamp(std::vector<fs::path> const& files);
```

- Iterates over all provided paths.
- For each existing path, calls `fs::last_write_time`.
- Returns the maximum timestamp encountered, or `fs::file_time_type::min()` if none exist.
- Exceptions (e.g., inaccessible paths) are silently ignored.

---

## 2. Thread Pool Wrapper (`auxiliary/threadPool.*`)

Namespace:

```cpp
namespace AIAssistant
```

JarvisAgent wraps the upstream `BS::thread_pool` class to centralize thread‑pool usage, add a small amount of synchronization, and expose a two-phase shutdown gate (`RequestStop` → `Shutdown`) that lets curl callbacks and other long-running work observe shutdown without prematurely refusing in-flight tasks.

### Class definition

```cpp
class ThreadPool
{
public:
    ThreadPool();

    void RequestStop();    // Flip stop flag (curl callbacks abort); pool keeps running queued work.
    void Shutdown();       // Refuse new tasks (atomic with concurrent Submit), drain queue. Idempotent.
    void Reset(size_t numThreads);  // Reconfigure thread count. Rejected post-Shutdown.

    [[nodiscard]] size_t Size() const;
    [[nodiscard]] bool IsStopped() const;

    template <typename FunctionType,
              typename ReturnType = std::invoke_result_t<std::decay_t<FunctionType>>>
    [[nodiscard]] std::future<ReturnType> SubmitTask(FunctionType&& task);

    [[nodiscard]] std::vector<std::thread::id> GetThreadIDs() const;

private:
    void LogPostShutdownSubmit() const;  // Defined in cpp to avoid an engine.h cycle in the header.

    BS::thread_pool<> m_Pool;
    std::mutex m_Mutex;
    std::atomic<bool> m_Stopped{false};
    std::atomic<bool> m_ShutdownDrained{false};
};
```

### Construction

```cpp
ThreadPool::ThreadPool();
```

- Default‑constructs `BS::thread_pool<> m_Pool;`.
- Per upstream library semantics, this **immediately creates a pool of worker threads**, typically with `std::thread::hardware_concurrency()` threads (unless configured otherwise).  `Core::Start()` calls `Reset(maxThreads + THREADS_REQUIRED_BY_APP)` afterwards to set the actual engine thread count.

### Lifecycle gates — `RequestStop` vs `Shutdown`

The two "stop"-flavoured methods serve distinct purposes; pick the right one:

| Method | Effect | Use case |
|---|---|---|
| `RequestStop()` | Atomically sets `m_Stopped=true`.  `IsStopped()` now returns true so curl progress callbacks abort in-flight transfers; `SubmitTask` short-circuits.  Does NOT join workers or drain the queue. | Phase 1 of engine shutdown — tell long-running work to wind down. |
| `Shutdown()` | Atomically (under `m_Mutex`) flips `m_Stopped=true`, then calls `m_Pool.wait()` to drain queued tasks, then sets `m_ShutdownDrained=true`.  Idempotent: a second call is a no-op (no re-log, no re-wait). | Phase 2 of engine shutdown — block until every queued task finishes. |

The two-flag design (`m_Stopped` + `m_ShutdownDrained`) is deliberate: a prior `RequestStop` sets `m_Stopped=true`, but a later `Shutdown` must still run its drain.  Using `m_Stopped` for Shutdown's idempotency would short-circuit the drain after `RequestStop`, leaving queued tasks in limbo.

### Resetting thread count

```cpp
void ThreadPool::Reset(size_t numThreads);
```

- Delegates to `m_Pool.reset(numThreads)`: waits for currently running tasks to complete, keeps queued tasks, recreates the pool with the new thread count, resumes processing.
- **Rejected if `m_Stopped` is set.**  Calling `Reset` on a stopped wrapper would create fresh worker threads while `SubmitTask` continued to short-circuit (since `m_Stopped` stays true) — wasted threads + silently-dropped tasks.  Treats the call as a programming error, logs `LOG_CORE_WARN`, and skips the reset.
- `Core::Start()` calls this once at engine init (`maxThreads + THREADS_REQUIRED_BY_APP`).

### Querying pool size + stop flag

```cpp
size_t ThreadPool::Size() const;     // current worker thread count
bool   ThreadPool::IsStopped() const; // m_Stopped.load(); curl callbacks consult this
```

### Submitting tasks

```cpp
template <typename FunctionType, typename ReturnType>
std::future<ReturnType> ThreadPool::SubmitTask(FunctionType&& task);
```

- Acquires `m_Mutex` and reads `m_Stopped` under the lock.  Atomic with `Shutdown`'s flag flip — a Submit either commits before `Shutdown` observes the stop (and the task runs as part of the drain) or sees `m_Stopped=true` and short-circuits.
- **Post-Shutdown short-circuit.**  When `m_Stopped` is true, `SubmitTask` does NOT call into the underlying pool.  It logs `LOG_CORE_WARN("[ThreadPool] SubmitTask called after Shutdown - returning default-valued future")` and returns a `std::future<ReturnType>` whose backing `std::promise` is already satisfied with `ReturnType{}` (or `void` for void-returning tasks).  Callers that wait on the future see immediate completion with a default-constructed value.
- **Move semantics preserved.**  The forwarding reference `FunctionType&& task` is passed to `m_Pool.submit_task(std::forward<FunctionType>(task))` so an rvalue callable (e.g. a lambda built with `std::move`'d captures) is moved into the pool, not copied.
- The log line lives in `threadPool.cpp` (not the header) to avoid pulling `engine.h` into a header that's transitively included by `core.h` — would form a cycle.

### Inspecting worker IDs

```cpp
std::vector<std::thread::id> ThreadPool::GetThreadIDs() const;
```

- Delegates to `m_Pool.get_thread_ids()`.
- Returns the OS thread IDs for all worker threads in the pool (useful for debugging or profiling).

---

## 3. Tracy Profiler Integration (`auxiliary/TracyClient.cpp`)

`TracyClient.cpp` is the standard single‑translation‑unit integration recommended by the Tracy profiler:

- Unconditionally includes `common/TracySystem.cpp`.
- When `TRACY_ENABLE` is defined, it compiles in:
  - Core Tracy client (`TracyProfiler.cpp`, `TracyCallstack.cpp`, etc.).
  - LZ4 compression, socket handling, and optional callstack/backtrace support.
- On MSVC, the file adds the required system libraries via `#pragma comment(lib, ...)`.

There is **no JarvisAgent‑specific logic** in this file; it simply makes the Tracy client available when profiling is enabled.

---

## Summary

The auxiliary module provides:

- Cross‑platform filesystem helpers used throughout the engine (`file.*`).
- A thin, synchronized wrapper around `BS::thread_pool` for engine task execution (`threadPool.*`).
- A one‑stop integration point for the Tracy profiler (`TracyClient.cpp`).

These components support the core engine without containing application‑specific behavior.
