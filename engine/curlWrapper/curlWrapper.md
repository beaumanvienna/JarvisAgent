# JarvisAgent Engine: Curl Wrapper Module Documentation

## Overview

The Curl Wrapper module provides a thread-local, RAII-safe wrapper around **libcurl**, used by JarvisAgent to send HTTP POST requests to AI provider APIs (OpenAI, Google Gemini, Anthropic, Ollama, and any compatible provider).

It ensures:

- Global libcurl initialization happens once across the entire process
- Each worker thread gets its own `CURL*` handle (thread-local via `CurlManager`)
- Clean shutdown and memory safety via RAII wrappers
- Uniform request building (headers, POST body, write callback)
- Per-request API key and authentication style selection
- Configurable per-request timeouts
- Centralized error logging and response buffering
- A unified error code scheme covering transport errors, HTTP status codes, and pre-flight failures

This document covers the following components:

- `CurlManager`
- `CurlWrapper`
- `QueryData` / `QueryResult` / `QueryErrorCode`
- Thread-local CURL instances
- Query execution pipeline
- Header list RAII (`CurlSlist`)
- Response buffering
- CA bundle handling
- Cleanup behavior

---

## 1. `CurlManager`

### Purpose

Provides a **thread-local instance** of `CurlWrapper`.

```cpp
static CurlWrapper& GetThreadCurl()
{
    thread_local CurlWrapper curl;
    return curl;
}
```

### Notes

- Each thread receives exactly one `CurlWrapper` instance.
- `CURL*` handles are not shared across threads — no locking needed.

---

## 2. `CurlWrapper` Class

### Responsibilities

- Initialize libcurl once globally (`curl_global_init`)
- Create and manage a per-instance `CURL*` handle
- Configure authentication headers (Bearer or `x-goog-api-key`) from `QueryData`
- Configure POST body and write callback
- Run the request with an optional per-request timeout
- Accumulate the response body into an internal buffer
- Report errors through the CORE logger
- Delete/cleanup resources correctly

### Key Members

```cpp
CURL* m_Curl;                           // Owned libcurl handle
bool m_Initialized;                     // Whether initialization succeeded
std::string m_ReadBuffer;               // Accumulates response body
static std::atomic<uint32_t> m_QueryCounter;
```

### Construction Behavior

1. Performs **global** libcurl initialization (once per process, under a mutex).
2. Creates a `CURL*` handle via `curl_easy_init`.
3. Sets `m_Initialized = true` if the handle was created successfully.

### Destruction Behavior

- Cleans up `m_Curl` via `curl_easy_cleanup`.
- **Does not** call global cleanup; call `CurlWrapper::GlobalCleanup()` at application shutdown.

---

## 3. Error Codes (`QueryErrorCode`)

Namespace `AIAssistant::QueryErrorCode` defines a unified error code scheme:

| Range | Meaning |
|-------|---------|
| `0` | Success |
| `1–99` | `CURLcode` — libcurl transport errors, used as-is |
| `100–599` | HTTP status codes (RFC 9110) |
| `1000+` | Pre-flight / internal errors |

Named pre-flight constants:

```cpp
constexpr int Success                 = 0;
constexpr int NoApiKey                = 1000;
constexpr int InvalidQueryData        = 1001;
constexpr int CurlNotInitialized      = 1002;
constexpr int ParserError             = 1003;
constexpr int EmptyResponse           = 1004;
constexpr int ExceptionThrown         = 1005;

// AI provider errors (1100+, reserved for future use)
constexpr int QuotaExceeded           = 1100;
constexpr int ContextWindowExceeded   = 1101;
constexpr int ContentPolicyViolation  = 1102;
constexpr int ModelNotFound           = 1103;
```

`QueryErrorCode::Describe(int code)` returns a human-readable label.

---

## 4. `QueryResult`

Returned by `CurlWrapper::Query()`:

```cpp
struct QueryResult
{
    bool m_Ok{false};
    int m_ErrorCode{0};         // 0=success, 1-99=CURLcode, 100-599=HTTP, 1000+=custom
    std::string m_ErrorMessage; // human-readable description

    static QueryResult Ok();
    static QueryResult Fail(int code, std::string message);
};
```

---

## 5. `QueryData`

Input structure for a single AI HTTP request:

```cpp
struct QueryData
{
    std::string m_Url;                          // API endpoint URL
    std::string m_Data;                         // JSON POST body
    std::string m_ApiKey;                       // API key for this request
    AuthStyle m_AuthStyle{AuthStyle::Bearer};   // Authentication header style
    long m_TimeoutMs{0};                        // 0 = no timeout; >0 = max transfer time in ms
    bool IsValid() const;
};
```

### `AuthStyle`

```cpp
enum class AuthStyle
{
    Bearer = 0,  // Authorization: Bearer <key>  (OpenAI, Anthropic, Ollama, etc.)
    XGoogApiKey  // x-goog-api-key: <key>        (Google Gemini native — API3)
};
```

### Validation

`IsValid()` returns `false` (and logs an error) if `m_Url` or `m_Data` is empty.

---

## 6. Query Execution Flow

`QueryResult CurlWrapper::Query(QueryData const& queryData)`:

1. Checks `m_Initialized` — returns `QueryResult::Fail(CurlNotInitialized, …)` if not ready.
2. Validates `queryData.IsValid()` — returns `QueryResult::Fail(InvalidQueryData, …)` on failure.
3. Checks `queryData.m_ApiKey` is non-empty — returns `QueryResult::Fail(NoApiKey, …)` if missing.
4. Constructs a `CurlSlist` with:
   - `Content-Type: application/json`
   - `Authorization: Bearer <key>` (if `AuthStyle::Bearer`) **or** `x-goog-api-key: <key>` (if `AuthStyle::XGoogApiKey`)
5. Sets curl options:
   - `CURLOPT_URL`, `CURLOPT_HTTPHEADER`, `CURLOPT_POSTFIELDS`
   - `CURLOPT_WRITEFUNCTION` / `CURLOPT_WRITEDATA` → appends to `m_ReadBuffer`
   - `CURLOPT_TIMEOUT_MS` if `m_TimeoutMs > 0`
   - `CURLOPT_CAINFO` if a CA bundle path is available (see Section 9)
   - Progress callback that aborts mid-request on engine shutdown signal
6. Calls `curl_easy_perform(m_Curl)`.
7. Reads HTTP response code via `curl_easy_getinfo(CURLINFO_RESPONSE_CODE)`.
8. Returns `QueryResult::Ok()` on success, or `QueryResult::Fail(errorCode, …)` on transport or HTTP error.

---

## 7. Response Buffering

The write callback appends incoming chunks:

```cpp
buffer->append(static_cast<char*>(contents), totalSize);
```

After the request:

```cpp
std::string& GetBuffer();   // Access the accumulated response body
void Clear();               // Reset the buffer for the next query
```

---

## 8. `CurlSlist` (Header Management)

RAII wrapper around `curl_slist`:

```cpp
class CurlSlist
{
    curl_slist* m_List{nullptr};
    ~CurlSlist() { curl_slist_free_all(m_List); }

    void Append(std::string const& str);
    struct curl_slist* Get();
};
```

Guarantees:

- No leaks — destructor always frees the list.
- Safe even if `curl_slist_append` fails partway through.

---

## 9. CA Bundle Handling

On platforms where the system CA bundle path is not automatically detected by libcurl (e.g. some Linux distributions, macOS), `CurlWrapper::GetCaBundlePath()` returns the path to a bundled `cacert.pem`. When non-empty, it is passed via `CURLOPT_CAINFO` to ensure HTTPS certificate verification works without requiring a system CA store.

```cpp
static std::string const& GetCaBundlePath();
```

---

## 10. Global Cleanup

Must be invoked at application shutdown:

```cpp
CurlWrapper::GlobalCleanup();
```

This calls `curl_global_cleanup()`.

---

## 11. Threading Model

- `curl_global_init` is called once under a mutex.
- Each thread receives its own `CurlWrapper` instance via `thread_local` storage in `CurlManager`.
- No `CURL*` handle is shared across threads — no locking needed per-request.

---

## 12. Error Handling & Logging

Uses the CORE logger. Examples:

```
thread 1234 got a good curl
curl_easy_init() failed
query 42 curl error (7): couldn't connect to server
query 42 HTTP error 429
```

---

## 13. Summary of Guarantees

| Guarantee | Mechanism |
|-----------|-----------|
| Thread-safe CURL handles | `thread_local CurlWrapper` in `CurlManager` |
| No global double-init | Static mutex + boolean gate |
| Memory safety | RAII (`CurlSlist`, destructor cleanup) |
| Unified error reporting | `QueryResult` + `QueryErrorCode` |
| Per-request API key | `QueryData::m_ApiKey` (set by caller per request) |
| Multi-provider auth | `AuthStyle::Bearer` / `AuthStyle::XGoogApiKey` |
| Per-request timeout | `QueryData::m_TimeoutMs` → `CURLOPT_TIMEOUT_MS` (size-aware budget when set by `AiRequestPool::Submit`) |
| Graceful shutdown | Progress callback aborts in-flight requests |
| Response buffering | Internal `std::string m_ReadBuffer` |

---

## 14. Minimal Usage Example

```cpp
auto& curl = CurlManager::GetThreadCurl();
curl.Clear();

CurlWrapper::QueryData q;
q.m_Url      = "https://api.openai.com/v1/chat/completions";
q.m_Data     = R"({"model":"gpt-4.1","messages":[{"role":"user","content":"hi"}]})";
q.m_ApiKey   = apiKey;                          // from encrypted keys file
q.m_AuthStyle = CurlWrapper::AuthStyle::Bearer;
q.m_TimeoutMs = 30000;                          // 30 s timeout

QueryResult result = curl.Query(q);
if (result.m_Ok)
{
    std::string response = curl.GetBuffer();
    // parse via ReplyParser...
}
else
{
    LOG_CORE_ERROR("query failed ({}): {}", result.m_ErrorCode, result.m_ErrorMessage);
}
```

---

## 15. `CurlMultiDispatcher` — async parallel I/O thread

While `CurlWrapper::Query` is the synchronous one-shot path used by REST handlers and connection-test endpoints, `CurlMultiDispatcher` is the async parallel path used by every workflow `ai_call` task. One dedicated I/O thread drives `libcurl multi`; HTTP/2 stream multiplexing carries up to ~100 concurrent requests over a single TCP/TLS connection per host.

### Dispatch pipeline

```
AiRequestPool::Submit(envelope)
   ↓ build QueryData (URL, body, auth, m_QuotaKey, m_EstimatedInputTokens, m_TimeoutMs, m_CancelKey)
   ↓ disarm AiRequestPool's pre-dispatch file-activity watchdog (handoff complete)
   ↓
CurlMultiDispatcher::Submit(queryData, callback)
   ↓ push to inbox + curl_multi_wakeup
   ↓ I/O thread:
   ↓   DrainPendingCancellations()  (abort cancelled-run requests first)
   ↓   DrainInbox()                  (controller.ShouldAdmit gate; curl_multi_add_handle)
   ↓   DrainRetryQueue()             (re-enter inbox when retry-ready)
   ↓   curl_multi_perform + DrainCompleted (parse rate-limit headers; route 429/transient → retry queue)
```

### Adaptive rate-limit + concurrency control

The dispatcher composes three mechanisms keyed by `(host, modelFamily)` via `QueryData::m_QuotaKey`:

- **`IRateLimitStrategy`** (`rateLimitStrategy.h`) — per-`InterfaceType` parser of provider-specific response headers into a normalized `RateLimitObservation`. Implementations: `RateLimitStrategyOpenAI` (API1/API2/API6), `RateLimitStrategyAnthropic` (API4 — split input/output token quotas, ISO 8601 resets, retry-after), `RateLimitStrategyEmpty` (API3 Gemini, API5 Bedrock, Test). Also provides `EstimateInputTokens(prompt)` (chars/4 default), `DeriveQuotaKey(model)` (model→family), and `InitialConcurrencyProbe()`.
- **`RateLimitController`** (`rateLimitController.h`) — per-`QuotaKey` adaptive controller. `ShouldAdmit(currentInflight, estimatedInputTokens)` projects the token bucket forward (deny if overshooting) and compares against the AIMD cap. `Observe(observation, was429)` updates state idempotently — known fields replace, unknown fields preserve, multiple calls per request produce identical state. AIMD: cap halves on 429, +1 every 5 clean completions, lower bound 1, upper bound from `config.rate_limit.max_concurrency`.
- **Server-directed waits** — `Retry-After` / `*-reset` headers are floors on the next admission time.

State surfaces via `GetDebugSnapshot()` → `/api/debug/signals dispatcher_controllers[]`.

### Cascade cancellation

`CancelByCancelKey(cancelKey)` is thread-safe; it pushes the key into a pending-cancellations queue and wakes the I/O thread. `DrainPendingCancellations()` (I/O thread only — curl handle mutations must be single-threaded relative to `curl_multi_perform`) drops matching entries from the inbox + retry queue and aborts in-flight `m_Active` handles via `curl_multi_remove_handle` + `curl_easy_cleanup`. Each cancelled request fires its callback with `Fail(CURLE_ABORTED_BY_CALLBACK, "request cancelled (run terminated)")`.

Callers: `AiRequestPool::CancelRequestsForRun(runId)` walks `m_PendingByOutputPath`, finds matching runId entries, fires `CancelByCancelKey` for each. Triggered by `WorkflowRuntimeManager` when `ActiveRun.m_Run.m_HasFailed || m_CancelRequested || m_StopRequested` flips true. Idempotent via `ActiveRun.m_CancelCascadeFired`.

### Per-attempt timeout = `CURLOPT_TIMEOUT_MS`

`AiRequestPool::Submit` computes the size-aware budget from `api->m_RateLimit.m_RequestBudget` and sets `QueryData::m_TimeoutMs`. The dispatcher passes it straight to curl. This replaces the pre-1.0 `AiRequestPool::m_Deadline` machinery (deferred-arm + retry-extension) — curl already counts only in-flight time and resets per attempt. Curl returns `CURLE_OPERATION_TIMEDOUT` when the budget elapses; the standard `Fail(...)` callback path takes over.

### Per-host stream / connection caps

- `CURLMOPT_MAX_HOST_CONNECTIONS = 1` — one TCP connection per host
- `CURLMOPT_MAX_CONCURRENT_STREAMS = 100` — HTTP/2 stream cap over that connection
- `kMaxActivePerHost = 48` — internal hard ceiling, used as the AIMD `hardCap` when no per-interface override is set
- `CURLOPT_TCP_KEEPALIVE = 1` — long-idle in-flight connections notice they're dead

### Lifetime counters (`/api/debug/signals`)

`dispatcher_total_dispatched`, `_completed`, `_throttled`, `_429s`, `_retries_exhausted`, `_cancelled` — all `std::atomic<uint64_t>` so debug snapshot reads are wait-free.

### Liveness detection — what curl catches and what it doesn't

Operational reference for "why didn't j9t notice X?" investigations. The dispatcher relies on curl's standard error mapping plus the size-aware budget; no custom heartbeat layer.

| Failure mode | Detected? | Mechanism |
|---|---|---|
| Internet off, new request | Immediate | `CURLE_COULDNT_CONNECT` |
| Internet off mid-request | Eventually | TCP RST → curl error, or `CURLOPT_TIMEOUT_MS` |
| DNS down | Immediate | `CURLE_COULDNT_RESOLVE_HOST` |
| TLS handshake fail | Immediate | `CURLE_SSL_CONNECT_ERROR` |
| Server returns 5xx | Yes | HTTP code → existing transient-retry path |
| Server slow but generating | No (looks normal) | only `CURLOPT_TIMEOUT_MS` |
| Server quietly stuck | No | only `CURLOPT_TIMEOUT_MS` |

Network-level failures all surface as curl errors with no extra work. The only gap the size-aware budget can't tighten is "server accepted my POST and is silently doing nothing" — looks identical to "server is generating tokens." Streaming (SSE) is the only proactive signal that distinguishes them, and it's deferred to post-1.0 (`Observe()` is idempotent by replacement so the future split into `ParseHeaders()` + `ParseBody()` is mechanical — see `doc/roadmap.md`). `CURLOPT_LOW_SPEED_LIMIT` + `CURLOPT_LOW_SPEED_TIME` are deliberately **not** set — they would false-fire when a model "thinks" silently for 30s before emitting tokens.
