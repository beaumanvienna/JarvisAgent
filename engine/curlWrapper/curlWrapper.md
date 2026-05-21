# JarvisAgent Engine: Curl Wrapper Module Documentation

## Overview

The Curl Wrapper module provides a thread-local, RAII-safe wrapper around **libcurl**, used by JarvisAgent to send HTTP POST requests to AI provider APIs.  Six provider adapters are supported via the `IAuthSigner` strategy: OpenAI Chat / OpenAI Responses (`Bearer`), Google Gemini native (`x-goog-api-key`), Anthropic Messages (`x-api-key` + `anthropic-version`), Azure OpenAI deployments (`api-key`), AWS Bedrock (SigV4), and any provider compatible with one of these auth styles (e.g. Ollama via `Bearer`).

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
- Delegate authentication-header production to `IAuthSigner` (Bearer / x-goog-api-key / x-api-key / api-key / AWS SigV4) — no per-style branching in this layer
- Configure POST body and write callback (bounded + exception-safe; see Section 7)
- Run the request with an optional per-request timeout
- Accumulate the response body into an internal buffer (auto-cleared at every `Query()` entry)
- Report errors through the CORE logger with `url` + `quotaKey` substrings on fail paths
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

Input structure for a single AI HTTP request.  Sync callers (assistant, jcwfService Test-connection) populate the auth-relevant fields and leave the dispatcher-routing fields at their sentinel defaults; `AiRequestPool::Submit` fills the dispatcher-routing fields when constructing requests for the async path.

```cpp
struct QueryData
{
    // Auth + transport (used by both sync CurlWrapper::Query and async dispatcher)
    std::string m_Url;                          // API endpoint URL
    std::string m_Data;                         // JSON POST body
    std::string m_ApiKey;                       // Static-header credential (Bearer / x-api-key / api-key / x-goog-api-key); ignored on the SigV4 path — see m_AwsCredential
    AuthStyle m_AuthStyle{AuthStyle::Bearer};   // Authentication header style — selects the IAuthSigner
    long m_TimeoutMs{0};                        // 0 = no timeout; >0 = max transfer time in ms
    std::unordered_map<std::string, std::string> m_Params{}; // Per-style auxiliary non-secret fields (e.g. SigV4 reads "service" with default "bedrock"); secret material lives in typed credential pointers, not this map

    // Typed-credential snapshot (Sitting 6).  Populated at submit time when AuthStyle is AwsSigV4;
    // null for the other styles which read m_ApiKey directly.  shared_ptr to a deep copy taken
    // under KeyManager's lock so the request's view of the credential is stable across concurrent
    // RemoveProvider / SetDefaultProvider mutations.
    std::shared_ptr<AwsCredential const> m_AwsCredential{};

    // Optional AWS SigV4 timestamp override ("YYYYMMDDTHHMMSSZ"). Mock-only — set by
    // MockTransport from <fixture>.meta.json::x_amz_date_override so the captured
    // Authorization is byte-deterministic for signature KAT tests.  Live paths leave
    // this empty; SigV4Signer::Sign falls back to FormatAmzDateNow().
    std::string m_AmzDateOverride;

    // Dispatcher-routing fields (async path only — see Section 15)
    int m_InterfaceType{-1};                    // ConfigParser::EngineConfig::InterfaceType encoded as int; -1 = unknown / sync caller
    std::string m_QuotaKey;                     // "<host>|<modelFamily>" controller-map key composed by AiRequestPool::Submit
    int64_t m_EstimatedInputTokens{-1};         // Pre-dispatch token estimate fed to the controller's bucket projection
    std::string m_CancelKey;                    // Per-task ID used by CancelByCancelKey() to abort across inbox / retry / active set
    int m_MaxConcurrency{-1};                   // -1 = unset / use dispatcher default; 0 = explicit zero; >0 = explicit value
    int m_MaxRetries429{-1};
    int m_MaxRetriesTransient{-1};
    int m_BaseRetryMs{-1};

    bool IsValid() const;
};
```

### `AuthStyle`

```cpp
enum class AuthStyle
{
    Bearer = 0,        // Authorization: Bearer <key>           (OpenAI Chat + Responses APIs; also Ollama, GitHub, Slack, Polarion)
    XGoogApiKey,       // x-goog-api-key: <key>                 (Google Gemini native)
    AnthropicXApiKey,  // x-api-key: <key> + anthropic-version  (Anthropic Messages)
    AzureApiKey,       // api-key: <key>                        (Azure OpenAI deployment URLs)
    AwsSigV4           // Authorization (AWS4-HMAC-SHA256) + X-Amz-Date + X-Amz-Content-Sha256  (AWS Bedrock — signs the request body)
};
```

`IAuthSigner::Get(style)` returns the singleton signer for each variant.  Adding a new style triggers `-Wswitch` (no `default:` arm) at every consumer; the helper throws `std::logic_error` for unknown integer values as a runtime backstop.  Per-style validation (empty / whitespace credentials, SigV4 typed-credential presence + region, etc.) lives inside each signer's `Apply()` — see `doc/cyber security.md` "IAuthSigner Security".

### Validation

`IsValid()` is a coarse pre-flight check: it returns `false` (and logs `LOG_CORE_ERROR`) if any of `m_Url`, `m_Data`, `m_ApiKey` is empty.  This is intentionally cheap and style-agnostic.  Style-specific validation (e.g. SigV4 requires `m_AwsCredential` non-null with non-empty `m_SecretAccessKey` and `m_Region`) lives inside `IAuthSigner::Apply()` and runs after `IsValid()` — a request that passes `IsValid()` may still be rejected by the signer with `QueryErrorCode::NoApiKey` if a style-specific field is missing.  Note that SigV4 paths skip the `m_ApiKey`-blank check inside `IsValid()` implicitly: `AiRequestPool::ResolveApiKey` populates `m_ApiKey` with the AWS access_key_id for legacy compatibility, but the actual signer reads `m_AwsCredential->m_AccessKeyId` directly.

---

## 6. Query Execution Flow

`QueryResult CurlWrapper::Query(QueryData const& queryData)`:

1. Checks `m_Initialized` — returns `QueryResult::Fail(CurlNotInitialized, …)` if not ready.
2. Validates `queryData.IsValid()` — returns `QueryResult::Fail(InvalidQueryData, …)` on coarse pre-flight failure (empty URL / data / API key).
3. Clears `m_ReadBuffer` so sequential queries on the same `CurlWrapper` don't concatenate (see Section 7).
4. Calls `IAuthSigner::Get(m_AuthStyle).Apply(queryData, headers, errorMessage)` to produce auth headers per the selected style.  On signer rejection (empty / whitespace credential, SigV4 null `m_AwsCredential` or missing region, etc.) returns `QueryResult::Fail(NoApiKey, …)` and emits `LOG_CORE_ERROR("CurlWrapper::Query: auth signer rejected request url='...' quotaKey='...': ...")`.
5. Builds a `CurlSlist` with the signer-produced auth headers + `Content-Type: application/json`.
6. Sets curl options:
   - `CURLOPT_URL`, `CURLOPT_HTTPHEADER`, `CURLOPT_POSTFIELDS`
   - `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2TLS` (HTTP/2 over ALPN; falls back to HTTP/1.1)
   - `CURLOPT_WRITEFUNCTION` / `CURLOPT_WRITEDATA` → bounded + exception-safe append to `m_ReadBuffer`
   - `CURLOPT_TIMEOUT_MS` from `m_TimeoutMs` (0 = no timeout)
   - `CURLOPT_CAINFO` if a CA bundle path is available (see Section 9)
   - Progress callback that aborts mid-request on engine shutdown signal
7. Captures `qnum` locally (`uint32_t qnum = ++m_QueryCounter`) so subsequent log lines stay correlated under concurrent calls.
8. Calls `curl_easy_perform(m_Curl)`.
9. On `CURLE_OK` reads HTTP response code via `curl_easy_getinfo(CURLINFO_RESPONSE_CODE)`.
10. Returns `QueryResult::Ok()` on success (HTTP <400), or `QueryResult::Fail(errorCode, …)` on curl-level error / HTTP ≥400 — fail paths log `LOG_CORE_ERROR` with `qnum`, `url`, and `quotaKey` substrings for run-analyzer surfacing (Section 12).

The async path (`CurlMultiDispatcher`, Section 15) shares steps 4 (signer) and 6 (curl options) verbatim — no per-style branching diverges between sync and async surfaces.

---

## 7. Response Buffering

The write callback appends incoming chunks into `m_ReadBuffer`, with three safety properties:

- **Bounded.** Response body is capped at 32 MiB.  When an append would push the buffer past the cap, the callback returns a short-write to libcurl, which translates to `CURLE_WRITE_ERROR` and aborts the transfer.  Defends the engine against a runaway server streaming gigabytes into a single `std::string`.
- **Exception-safe.** The callback body is wrapped in `try / catch (...)`; `std::string::append` can throw `bad_alloc` / `length_error`, and an exception escaping a libcurl callback is UB (libcurl is C).  On any throw the callback returns 0 and libcurl aborts cleanly.
- **Auto-cleared.** `Query()` calls `m_ReadBuffer.clear()` at entry.  This makes `CurlManager::GetThreadCurl()`'s thread-local-reuse pattern safe by default — sequential queries on the same `CurlWrapper` no longer concatenate the previous response into the next one.

After a request:

```cpp
std::string& GetBuffer();   // Access the response body for the most recent Query
void Clear();               // Defensive no-op; Query() now clears at entry
```

The same 32 MiB body cap is enforced by `MultiWriteCallback` in `liveTransport.cpp` (the curl write callback that backs the async path); both surfaces fail closed identically.

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

Uses the CORE logger.  Failure-path lines are `LOG_CORE_ERROR` and carry `url` + `quotaKey` substrings so the dashboard's Run Analyzer (which filters issues by run-identifier substring) surfaces them.  `IsValid()` field-empty logs are `LOG_CORE_ERROR` (per-request misconfiguration), not `LOG_CORE_CRITICAL`.

The query number `qnum` is captured locally at dispatch (`uint32_t qnum = ++m_QueryCounter`) and reused in every subsequent log line; reading the static `m_QueryCounter` again on the error path could observe an increment from a concurrent `Query()` on another thread and produce a mismatched qnum.

Examples:

```
thread 1234 got a good curl
curl_easy_init() failed
sending query 42
query 42 used HTTP/2 (HTTP 200)
curl error (code 7): Couldn't connect to server url='https://api.example.com/v1/...' quotaKey='api.example.com|model-x'
HTTP error 503 for query 42 url='https://api.example.com/v1/...' quotaKey='api.example.com|model-x'
HTTP 429 rate limit for query 42 — AI provider rejected the request (too many requests or insufficient credits) url='...' quotaKey='...'
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
| Multi-provider auth | `AuthStyle::{Bearer, XGoogApiKey, AnthropicXApiKey, AzureApiKey, AwsSigV4}` via `IAuthSigner` |
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

### Dispatcher / transport split

The dispatcher does not own the curl machinery directly.  It composes with two `IInterfaceTransport` implementations (`engine/curlWrapper/interfaceTransport.h`) and selects between them per-request:

- **`LiveTransport`** (`engine/curlWrapper/liveTransport.{h,cpp}`) — the production HTTPS path.  Owns the curl easy/multi handles, `IAuthSigner` integration, write/header/progress callbacks, response capture (body + raw headers + HTTP status + version label).
- **`MockTransport`** (`engine/curlWrapper/mockTransport.{h,cpp}`) — fixture-driven replay for hermetic dispatch tests, demo-JCWF replay, and CI without provider credit burn.  Reads a config-supplied JSON fixture (with optional `<fixture>.meta.json` sibling controlling HTTP status, headers, and an `x_amz_date_override` for deterministic SigV4 captures) and synthesises the same `Response` shape LiveTransport produces.  When the request's `m_AuthStyle == AwsSigV4` AND `m_AwsCredential != nullptr`, MockTransport additionally invokes the signer and captures the resulting Authorization header into a process-global ring buffer (`MockSignatureCapture`, cap 32, FIFO eviction).  The captures are surfaced via `/api/debug/signals::last_mock_signatures` (debug-build only) — see `test/dispatch/test_bedrock_sigv4.py` for the canonical SigV4 KAT consumer.  Tests that DON'T care about signing leave `m_AwsCredential` null (e.g. parser-only fault suites configure `key_name=""`); MockTransport skips the signer for those and replays the fixture unchanged.

Selection is per-request: `CurlMultiDispatcher::DrainInbox` routes requests whose `QueryData::m_IsMock` is `true` to MockTransport, everything else to LiveTransport.  Both transports share the dispatcher's monotonic `RequestId` counter + `OnTransportComplete` sink — the dispatcher doesn't track which transport carried which id.  Completions correlate via the `RequestId` carried through `IInterfaceTransport::Submit` → completion callback.

Everything above the transports — inbox, AIMD admission gate, per-`QuotaKey` controllers, retry queue, cascade-cancel queue, debug counters, the I/O thread loop — stays in `CurlMultiDispatcher`, so the AIMD code path runs unchanged whether the bottom is real HTTPS or a fixture.  Synthetic 429 → real `RateLimitController::Observe(was_429=true)` → real cap halving → real retry queue.  See `doc/cyber security.md` "MockTransport Security" for the cyber-sec posture at the fixture-load boundary (path confinement, size cap, status/header allowlists, admin-only `is_mock`).

### Dispatch pipeline

```
AiRequestPool::Submit(envelope)
   ↓ build QueryData (URL, body, auth, m_QuotaKey, m_EstimatedInputTokens, m_TimeoutMs, m_CancelKey,
   ↓                 m_IsMock, m_FixturePath)
   ↓ disarm AiRequestPool's pre-dispatch file-activity watchdog (handoff complete)
   ↓
CurlMultiDispatcher::Submit(queryData, callback)
   ↓ push to inbox + m_{Live,Mock}Transport->Wakeup()
   ↓ I/O thread:
   ↓   DrainPendingCancellations()  (fire user callback Fail; cleanup on both transports)
   ↓   DrainInbox()                  (controller.ShouldAdmit gate; allocate RequestId;
   ↓                                  select transport on queryData.m_IsMock; transport->Submit)
   ↓   DrainRetryQueue()             (re-enter inbox when retry-ready)
   ↓   m_LiveTransport->Pump()       (curl_multi_perform + HarvestCompletions → OnTransportComplete)
   ↓   m_MockTransport->Pump()       (deliver queued fixture responses → OnTransportComplete)
   ↓                                 (OnTransportComplete: ParseRateLimitHeaders + route 429/transient → retry queue,
   ↓                                  else fire user callback)
   ↓   m_LiveTransport->Wait(timeoutMs)  (only LiveTransport blocks; Mock has no I/O to wait on)
```

### Adaptive rate-limit + concurrency control

The dispatcher composes three mechanisms keyed by `(host, modelFamily)` via `QueryData::m_QuotaKey`:

- **`IRateLimitStrategy`** (`rateLimitStrategy.h`) — per-`InterfaceType` parser of provider-specific response headers into a normalized `RateLimitObservation`. Implementations: `RateLimitStrategyOpenAI` (API1/API2/API6), `RateLimitStrategyAnthropic` (API4 — split input/output token quotas, ISO 8601 resets, retry-after), `RateLimitStrategyEmpty` (API3 Gemini, API5 Bedrock). Also provides `EstimateInputTokens(prompt)` (chars/4 default), `DeriveQuotaKey(model)` (model→family), and `InitialConcurrencyProbe()`.
- **`RateLimitController`** (`rateLimitController.h`) — per-`QuotaKey` adaptive controller. `ShouldAdmit(currentInflight, estimatedInputTokens)` projects the token bucket forward (deny if overshooting) and compares against the AIMD cap. `Observe(observation, was429)` updates state idempotently — known fields replace, unknown fields preserve, multiple calls per request produce identical state. AIMD: cap halves on 429, +1 every 5 clean completions, lower bound 1, upper bound from `config.rate_limit.max_concurrency`.
- **Server-directed waits** — `Retry-After` / `*-reset` headers are floors on the next admission time.

State surfaces via `GetDebugSnapshot()` → `/api/debug/signals dispatcher_controllers[]`.

### Cascade cancellation

`CancelByCancelKey(cancelKey)` is thread-safe; it pushes the key into a pending-cancellations queue and wakes the I/O thread.  `DrainPendingCancellations()` runs on the I/O thread and walks three locations:

1. **Inbox** — drops matching entries; fires user callback `Fail(CURLE_ABORTED_BY_CALLBACK, "request cancelled (run terminated)")` synchronously.
2. **Retry queue** — same: drops + fires callback.
3. **Active set** (in-flight at the transport) — fires the user callback synchronously, erases the dispatcher-side `PendingDispatch` entry, then calls `m_Transport->CancelByCancelKey(cancelKey)`.  The transport's implementation walks its own in-flight map, runs `curl_multi_remove_handle` + slist free + `curl_easy_cleanup` for each match, and drops the entries silently — no transport-side completion callback fires (the dispatcher already fired the user callback, so the transport firing too would double-fire).  Curl handle mutations stay single-threaded relative to `curl_multi_perform` because both the cancel and the pump run on the dispatcher's I/O thread.

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
