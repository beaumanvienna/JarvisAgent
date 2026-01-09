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

JarvisAgent wraps the upstream `BS::thread_pool` class to centralize thread‑pool usage and add a small amount of synchronization.

### Class definition

```cpp
class ThreadPool
{
public:
    ThreadPool();

    void Wait();
    void Reset(size_t numThreads);
    [[nodiscard]] size_t Size() const;

    template <typename FunctionType,
              typename ReturnType = std::invoke_result_t<std::decay_t<FunctionType>>>
    [[nodiscard]] std::future<ReturnType> SubmitTask(FunctionType&& task);

    [[nodiscard]] std::vector<std::thread::id> GetThreadIDs() const;

private:
    BS::thread_pool<> m_Pool;
    std::mutex m_Mutex;
};
```

### Construction

```cpp
ThreadPool::ThreadPool();
```

- Default‑constructs `BS::thread_pool<> m_Pool;`.
- Per upstream library semantics, this **immediately creates a pool of worker threads**, typically with `std::thread::hardware_concurrency()` threads (unless configured otherwise).
- JarvisAgent does not change this behavior; the wrapper simply holds the pool instance.

### Waiting for tasks

```cpp
void ThreadPool::Wait();
```

- Delegates to `m_Pool.wait()`.
- Blocks until **all currently queued and running tasks** in the underlying `BS::thread_pool` have completed.

### Resetting thread count

```cpp
void ThreadPool::Reset(size_t numThreads);
```

- Delegates to `m_Pool.reset(numThreads)`.
- Upstream behavior:
  - Waits for all currently running tasks to complete.
  - Keeps queued tasks.
  - Recreates the internal pool with the new thread count (`numThreads`).
  - Resumes processing queued and newly submitted tasks.
- JarvisAgent uses this in `Core::Start()` to configure the engine’s thread count (engine threads + app‑required threads).

### Querying pool size

```cpp
size_t ThreadPool::Size() const;
```

- Returns `m_Pool.get_thread_count()` from `BS::thread_pool`.
- Reflects the **current** number of worker threads.

### Submitting tasks

```cpp
template <typename FunctionType, typename ReturnType>
std::future<ReturnType> ThreadPool::SubmitTask(FunctionType&& task);
```

- Acquires `m_Mutex` to serialize calls into `m_Pool.submit_task(task)`.
- Returns the `std::future<ReturnType>` produced by `BS::thread_pool`.
- Task semantics are exactly those of `BS::thread_pool::submit_task`:
  - The task is a callable with no parameters (arguments can be captured in a lambda).
  - Future can be used to wait for completion and obtain the task’s return value.

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
