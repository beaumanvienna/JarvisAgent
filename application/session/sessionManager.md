# SessionManager — AI Dispatch Architecture

## Overview

`SessionManager` monitors a queue folder, assembles the environment (STNG + CNTX + TASK
files), and dispatches one AI HTTP request per requirement file. All requests are sent
asynchronously via `CurlMultiDispatcher` — a dedicated I/O thread that multiplexes
concurrent requests over a single HTTP/2 connection per provider.

See also: `application/session/sessionManager_fileWriter.md` — file writing, output paths.

---

## HTTP version negotiation

All requests use `CURL_HTTP_VERSION_2TLS`: HTTP/2 is attempted for HTTPS connections via
ALPN during the TLS handshake; if the server does not support HTTP/2 the connection falls
back to HTTP/1.1 transparently. No `config.json` changes needed.

| Scenario | Protocol | Mechanism |
|----------|----------|-----------|
| `http://` URL (plain HTTP) | HTTP/1.1 | HTTP/2 requires TLS; ALPN only works over TLS |
| `https://` URL, server supports HTTP/2 | **HTTP/2, multiplexed** | Negotiated via ALPN; streams share one TCP connection per host |
| `https://` URL, server only supports HTTP/1.1 | HTTP/1.1 | Graceful ALPN fallback — transparent, no error |

---

## Thread model

All AI requests are handled by a single dedicated I/O thread (`CurlMultiDispatcher`).
Thread-pool threads are never blocked on network I/O.

| Property | Value |
|----------|-------|
| Thread-pool threads blocked per request | 0 |
| TCP connections to a single provider | 1 (HTTP/2 multiplexed) |
| Concurrent HTTP/2 streams per connection | up to 100 |
| Thread pool available for other work | yes |

---

## CurlMultiDispatcher

Lives in `engine/curlWrapper/curlMultiDispatcher.h/.cpp`.

### Structure

```
CurlMultiDispatcher
  ├─ CURLM* m_MultiHandle                        // libcurl multi handle
  ├─ std::thread m_IoThread                      // single dedicated I/O thread
  ├─ std::queue<PendingRequest> m_Inbox          // requests submitted from any thread
  ├─ std::mutex m_InboxMutex
  ├─ std::atomic<bool> m_Stopping
  └─ std::unordered_map<CURL*, ActiveRequest> m_Active  // in-flight easy handles
```

### API

```cpp
using Callback = std::function<void(QueryResult, std::string /*responseBody*/)>;
void Submit(CurlWrapper::QueryData const& data, Callback callback);
void SignalStop();   // non-blocking; wakes I/O thread
void WaitStop();     // blocks until I/O thread exits
```

`Submit()` is thread-safe — any thread may call it concurrently.
Callbacks fire on the I/O thread. Callers must not block inside a callback.

### I/O thread loop

```
while (!m_Stopping)
    DrainInbox()           — move Inbox entries → add easy handles to m_MultiHandle
    curl_multi_perform()   — drive all in-flight transfers
    DrainCompleted()       — curl_multi_info_read → fire callbacks, clean up handles
    if stopping && no active handles → break
    curl_multi_poll(50 ms) — sleep until socket activity or curl_multi_wakeup()
```

### Key curl_multi options

```cpp
curl_multi_setopt(m_MultiHandle, CURLMOPT_PIPELINING,             CURLPIPE_MULTIPLEX);
curl_multi_setopt(m_MultiHandle, CURLMOPT_MAX_HOST_CONNECTIONS,   1L);   // one conn per host
curl_multi_setopt(m_MultiHandle, CURLMOPT_MAX_CONCURRENT_STREAMS, 100L);
```

### Shutdown integration

Follows JarvisAgent's two-phase shutdown pattern:

1. `SignalStop()` — sets `m_Stopping`, calls `curl_multi_wakeup()` (non-blocking)
2. `WaitStop()` — joins the I/O thread (blocking)

In-flight requests are aborted by the progress callback: when the thread pool is stopped,
the callback returns 1, causing `curl_easy_perform` to return `CURLE_ABORTED_BY_CALLBACK`.
Shutdown ordering in `JarvisAgent::OnShutdown()`:

```
Signal (non-blocking):  AiRequestPool::Shutdown()  →  CurlMultiDispatcher::SignalStop()
Wait   (blocking):      AiRequestPool::WaitStop()  →  CurlMultiDispatcher::WaitStop()
                        then session managers are destroyed
```

`CurlMultiDispatcher::WaitStop()` must run **before** session managers are destroyed
because callbacks capture `SessionManager* this`.

---

## SessionManager dispatch flow

`DispatchQuery(TrackedFile& requirementFile)`:

1. Builds `CurlWrapper::QueryData` (URL, POST body, API key, auth style, timeout).
2. `++m_InFlightCount` (atomic).
3. Calls `CurlMultiDispatcher::Submit(data, callback)` — returns immediately.
4. Calls `requestPool->OnCurlDispatched(inputFilename)` to register the pending output path.

Callback (fires on I/O thread when the response arrives):

1. Parses JSON via `ReplyParser::Create(apiType, responseBody)` — local variable, no sharing.
2. Writes the AI response to the output file.
3. On success: `++m_CompletedQueriesThisRun`.
4. On failure: locks `m_ResultMutex`, updates `m_LastErrorCode` / `m_LastErrorMessage`,
   `++m_FailedQueriesThisRun`.
5. `--m_InFlightCount` (always, last).

### Thread-safety of shared counters

| Field | Type | Protection |
|-------|------|------------|
| `m_InFlightCount` | `std::atomic<size_t>` | Lock-free |
| `m_CompletedQueriesThisRun` | `std::atomic<size_t>` | Lock-free |
| `m_FailedQueriesThisRun` | `std::atomic<size_t>` | Lock-free |
| `m_LastErrorCode` / `m_LastErrorMessage` | `int` / `std::string` | `m_ResultMutex` |

`ReplyParser` is a local variable inside the callback — no data race possible even with
multiple concurrent callbacks.

### State machine transitions driven by counters

```
CompilingEnvironment  →  SendingQueries       (environment ready, requirements exist)
SendingQueries        →  AllQueriesSent       (m_InFlightCount > 0 after last Submit)
AllQueriesSent        →  AllResponsesReceived (m_InFlightCount == 0)
Any state             →  Failed               (m_FailedQueriesThisRun > 0 && inflight == 0)
```

---

## Throttle

`SessionManager` caps concurrent in-flight requests to avoid overwhelming the provider.
Dispatch is skipped for a given tick when:

```cpp
m_InFlightCount.load() >= MAX_INFLIGHT_QUERIES  // defined in sessionManager.cpp
```

This prevents runaway fan-out on very large queue folders while still saturating
the HTTP/2 connection's stream capacity.
