# AI Provider Error Visibility — Development Plan

**Status:** ✅ **CLOSED 2026-05-15** — all 8 sittings landed.  Foundation 1 + 2 (2026-05-14): `IInterfaceTransport` refactor + `MockTransport` + cyber-sec hardening + TestInterface removal.  Foundation 3 (2026-05-15): per-API fault batteries + TUI byte-safety stress + curated demo fixtures.  Sittings 4-8 (2026-05-15, single working day): Workstream B (`AiError` plumbing + §3.5 abstractions + WS payload) → A (log enrichment via `OnRequestFailed(AiError)`) → E (cross-provider classification — Gemini `details[*].reason`, Anthropic `error.type`, Bedrock AWS `__type`) → C (`ProviderAlertBanner` + per-row hazard glyph + dashboard WS consumer) → D (`ProviderHealthSnapshot` + `/api/providers/health` + 6th "AI Health" LED + popover + WS `cap-changed` push).  All workstream acceptance bullets verified; full dispatch suite 24/24 + per-API fault matrix 36/36 + WS payload 8/8.  Follow-ups in §8 are post-1.0 polish, not blocking.
**Target:** j9t 1.0 polish — better signaling when an AI provider rejects work for reasons the user can act on (billing exhaustion, rate limits, auth failures, model deprecation).

---

## 1. Charter — what's in scope, what isn't

In scope:
- Plumbing the billing-vs-throttle distinction from the per-interface `ReplyParser` → `AiError` → `AiCallFailedEvent` → WebSocket → React dashboard.
- A live view of AIMD throttle state so a pinned cap is visible without waiting for the next failure.
- A cross-provider error-code matrix grounded in reading each `replyParserAPI{1..5}` end-to-end.
- A DEBUG- and Release-safe MockTransport that enables canned-response workflows (trust building, credit conservation, hermetic CI testing) and full per-interface fault injection (parser-level UTF-8 hardening verification).

Out of scope:
- Cyber-sec / safety findings owned by the closed-out S1-S5 hardening arc.  *Exception:* this plan's Foundation workstream closes the §19 SanitizeUtf8 *verification* gap — that's completing existing hardening work, not introducing new findings.
- Provider-side billing automation (auto top-up, quota request).  j9t observes provider state; it doesn't manage the provider account.
- Reworking AIMD itself.  The throttle algorithm is unchanged; the fix is making its state visible.
- Anything that would log secrets.  Provider error bodies remain subject to the existing `RedactingFormatter` discipline.

### Architectural constraints

- **MockTransport is available in all 4 build targets** (Studio/Engine × Debug/Release) — no `#ifdef DEBUG` gating.  Cyber-sec hardening is at the boundary:
   - **Fixture path containment**: every fixture path (config.json `fixture_path`, `.meta.json` `body_path`) goes through `ConfineUnderProjectRoot` per `feedback_path_containment_scope` + `feedback_path_confinement_edition` (Engine strict; Studio relaxed).  Prevents arbitrary file reads via crafted fixture paths.
   - **Fixture size cap**: per-fixture cap (e.g. 10 MB; final value set at implementation time).  Fail-closed with ERROR log on overrun.
   - **Byte-content sanitization**: fixture bytes reaching logs/TUI go through `SanitizeUtf8` per `feedback_established_safety_patterns`.
   - **HTTP status bounds**: `.meta.json` `http_status` allowlist `{200..599}`; reject otherwise with ERROR.
   - **Response header allowlist**: `.meta.json` `headers` restricted to a small set (`Content-Type`, `Retry-After` initially; extensible).  Not free-form.
   - **Operator transparency**: INFO log on first call routed through MockTransport per provider after startup; `mocked: true` in PROV sidecar; dashboard popover shows a "(mocked)" badge for `is_mock: true` interfaces.
   - **`is_mock` flag is admin-only**: settable via config.json (admin-write only) or authenticated admin REST endpoint.  Same access surface as `api_key`.
- **TUI byte-safety is an explicit acceptance criterion** for the Foundation's per-interface fixture work.  `SanitizeUtf8`/`TruncateUtf8Safe` are the safety net from the §19 hardening (`feedback_established_safety_patterns`).  Foundation Sitting 3 verifies the safety net under malformed-UTF-8 stress AND real-world external content samples (n8n callback payloads, Polarion XML output).  Any TUI crash or terminal corruption means the sitting does not close.
- **No hardcoded provider names in UI.**  All user-facing copy (banner text, LED labels, popover columns, hazard tooltips) uses generic phrasing OR the user-configured `m_InterfaceName` from `config.json`.  Hardcoded provider names (`OpenAI`, `Anthropic`, etc.) and provider-specific URLs are not used.
- **TestInterface is removed** as part of Foundation F2.  Replaced by the orthogonal `is_mock: bool` flag on provider registry entries (config.json) — `is_mock: true` plus a real `api_type` is the strict superset of TestInterface's behavior and goes through the real parsers.  JCWF spec loses the `"Test"` `api_type` entry.  Per `feedback_no_legacy` — clean removal, no compat shim.
- **PROV portability vs cyber-sec.**  Provenance sidecars reference provider entries from admin-owned config.json, so JCWFs are non-portable across installs without name alignment.  This is by design — JCWF authors cannot inject `endpoint: https://attacker.example.com`, route to personal API keys, or bypass admin policy (e.g. air-gapped-only providers).  Provider resolution is one admin-controlled gate per `feedback_auth_funnel_one_gate`.  The `is_mock` flag turns the portability gap into a graceful-degradation surface: receivers can run shared JCWFs against mocked providers without provisioning real keys.

---

## 2. Background — what the engine already knows but doesn't surface

The HTTP 429 path is the load-bearing example.  When a provider returns HTTP 429 with a body like `{"error": {"code": "insufficient_quota", "type": "insufficient_quota", "message": "..."}}` (billing exhaustion) or `{"error": {"code": "rate_limit_error", ...}}` (genuine throttling), the engine parses both discriminators identically and the operator-visible surfaces (dispatcher log line, dashboard, run analyzer) render them the same.  AIMD halves the per-API concurrency cap on each 429 regardless of cause, so persistent billing-exhaustion 429s pin the cap at the floor with no recovery.

What the engine extracts but does not propagate:

| Signal | Where it lives today | Where it's lost |
|---|---|---|
| `error.code = "insufficient_quota"` | Parsed at `replyParserAPI1.cpp:262` into `ErrorInfo::m_Code` | Dropped by `GetError()` at `replyParserAPI1.cpp:71-92` — only `m_Kind=Provider`, `m_HttpStatus=429`, `m_Message` propagate |
| `error.type = "insufficient_quota"` | Same | Same |
| AIMD cap pinned at floor | `m_CurrentConcurrencyCap` exposed in `/api/debug/signals` | No dashboard component reads it |
| `m_StreakSinceLast429` | Same | Same |
| WARN log inside the parser ("InsufficientQuota response received") | `replyParserAPI1.cpp:273-284` | At WARN level and missing runId — invisible to the dashboard run analyzer (which filters ERROR + runId-substring) |

This is primarily a plumbing problem.  The parser-side discriminator extraction for API1/API2 is in place; the data is discarded before reaching any user-visible surface.  Three of the four other providers (Anthropic, Gemini, Bedrock) don't parse the discriminator at all and need parser extension.

---

## 3. Cross-provider error-body audit (parity matrix)

This is the load-bearing baseline for §4 workstream E.

### AiError struct, current shape

`application/workflow/aiReply.h:38-54`

```cpp
struct AiError
{
    enum class Kind { None, Http, Parse, SchemaValidation, Timeout, Transport, Provider };
    Kind m_Kind;
    int m_HttpStatus;
    std::string m_Message;
};
```

Three fields.  No `m_Code`, no `m_Type`, no `m_Category`, no `m_RetryAfterSeconds`.  This struct is the bottleneck — every provider-specific signal funnels through it.

### Per-API status (post-Sitting-6 — all 6 InterfaceTypes ship category classification)

| Aspect | API1 OpenAI Chat | API2 OpenAI Responses | API3 Gemini | API4 Anthropic Messages | API5 Bedrock | API6 Azure OpenAI |
|---|---|---|---|---|---|---|
| Error body parsed at all | ✓ | ✓ | ✓ (int `code`, string `status`, string `message`, `details[*]`) | ✓ (different shape — `{"type":"error","error":{"type":"...","message":"..."}}`) | ✓ (AWS `__type` envelope handled in `ReplyParserAPI5` ctor before family delegation) | Inherits API1 (Azure returns OpenAI-compatible bodies; routed to `ReplyParserAPI1` per `replyParser.cpp:88`) |
| `error.message` extracted | ✓ | ✓ | ✓ | ✓ | ✓ (from `__type` envelope or delegate) | Inherits API1 |
| `error.type` extracted | ✓ | ✓ | ✓ (via `m_Status`) | ✓ (nested `error.type`) | ✓ (`__type`, prefix-stripped to short name) | Inherits API1 |
| `error.code` extracted | ✓ | ✓ | ✓ (`m_DetailReason` from `details[*].reason` surfaces as `m_ProviderErrorCode`) | ✗ (Anthropic has no separate code field; `m_ProviderErrorCode` stays empty) | ✗ (AWS uses `__type` only; `m_ProviderErrorCode` stays empty) | Inherits API1 |
| Billing-exhaustion discriminator detected | `insufficient_quota` | `insufficient_quota` | `RESOURCE_EXHAUSTED` + `USER_PROJECT_QUOTA_EXCEEDED` / `BILLING_DISABLED` | `credit_balance_too_low` | `ServiceQuotaExceededException` | Same as API1 by inheritance |
| Throttle discriminator detected | `rate_limit_error` | `rate_limit_error` | `RESOURCE_EXHAUSTED` + `RATE_LIMIT_EXCEEDED` | `rate_limit_error` | `ThrottlingException` | Same as API1 |
| Auth discriminator detected | `authentication_error` | `authentication_error` | `UNAUTHENTICATED` / `PERMISSION_DENIED` | `authentication_error` / `permission_error` | `AccessDeniedException` / `UnauthorizedOperation` | Same as API1 |
| Overload discriminator detected | `server_error` | `server_error` | `UNAVAILABLE` / `DEADLINE_EXCEEDED` | `overloaded_error` / `api_error` | `ModelStreamErrorException` / `ModelTimeoutException` / `ModelNotReadyException` / `InternalServerException` / `ServiceUnavailableException` | Same as API1 |
| `m_ProviderErrorCode` / `m_ProviderErrorType` / `m_Category` returned via `AiError` | ✓ | ✓ | ✓ | ✓ | ✓ | Inherits API1 |

### Per-provider notes

- **OpenAI Chat (API1) + OpenAI Responses (API2):** Shared `ParseOpenAiStyleError` helper (Workstream B) — `type` + `code` + `message` + `param`.  Classification via `ClassifyOpenAiStyleErrorType` covers `insufficient_quota` / `rate_limit_error` / `authentication_error` / `permission_error` / `server_error` / `model_not_found` / `invalid_request_error`.
- **Gemini (API3):** Response shape `{"error": {"code": <int>, "status": "<RESOURCE_EXHAUSTED|...>", "message": "...", "details": [...]}}`.  `ParseError` walks `details[]` (capped 32 entries) for the first `reason` string and stores it in `m_DetailReason`.  `GetError` surfaces it as `m_ProviderErrorCode` so the log line + WS payload disambiguate `USER_PROJECT_QUOTA_EXCEEDED` / `BILLING_DISABLED` (billing) from `RATE_LIMIT_EXCEEDED` (throttle).  Classification falls back to status enum for the non-RESOURCE_EXHAUSTED categories.
- **Anthropic (API4):** Response shape `{"type": "error", "error": {"type": "<discriminator>", "message": "..."}}`.  `GetError` maps the nested `error.type` to category — 7 variants covered.  HTTP-status synthesis preserved alongside (e.g. `credit_balance_too_low` synthesizes 400, but `AiRequestPool`'s curl-error branch overlays the real network status when present).
- **Bedrock (API5):** New `BedrockFamily::AwsError` variant detected by re-iterating the body to probe `__type` after the success-shape probes.  `ExtractAwsError` strips any `com.amazonaws...#` prefix.  `ClassifyAwsBedrockException` maps 11 AWS exception names to categories.  `ReplyParserAPI5` ctor populates `m_AwsErrorType` + `m_AwsErrorMessage` directly (no delegate) for AwsError; `GetError` returns the classified AiError ahead of the delegate path.  Success-path bodies still go through the Anthropic / Llama / Titan delegates.
- **Azure OpenAI (API6):** Routed to `ReplyParserAPI1` in `replyParser.cpp:88` because Azure returns OpenAI-compatible Chat-Completions bodies.  Inherits API1's classification transparently; no API6-specific parser exists or is needed.  Fixture battery validates parity, not new parsing.

### Known unknowns (closed — Workstream E shipped)

- ~~Gemini (API3): exact error-detail field names for v1 vs v1beta endpoints~~ — resolved: `details[*].reason` field is stable across both endpoints in captured fixtures.  Future-Gemini-API drift would surface as `m_DetailReason` staying empty → conservative `ThrottleRateLimit` fallback on `RESOURCE_EXHAUSTED`.
- ~~Anthropic (API4): full error-type catalog~~ — resolved: 7 known variants covered (`credit_balance_too_low`, `rate_limit_error`, `authentication_error`, `permission_error`, `overloaded_error`, `api_error`, `invalid_request_error`, `not_found_error`).  Unknown new types fall through to `m_Category=Unknown` (raw type still propagates for logs).
- ~~Bedrock (API5): `LimitExceededException` third billing code~~ — `ClassifyAwsBedrockException` doesn't list `LimitExceededException` today; add to the mapper if it appears in a captured fixture.  Map would be `BillingExhausted`.
- ~~Azure OpenAI (API6): byte-for-byte parity with OpenAI Chat error shape~~ — confirmed by api6's 6-case fault matrix.  Drift would break api6 fixtures first, before any user-visible regression.

### Audit acceptance

Every cell in the parity matrix is grounded in a captured fixture under `test/dispatch/fixtures/api{1..6}/error_*.json` (with paired `.meta.json` for HTTP status + headers).  Fixtures are inferred-from-provider-docs rather than captured-from-live; one live smoke per provider when an API key is available would tighten the verification (open item in the hand-off log).

---

## 3.5. Architecture — data structures, helpers, abstractions

Three small abstractions land alongside this work.  Each is introduced because it has an immediate consumer inside this plan — no speculative scaffolding (`feedback_cpp_discipline` "extract before the third site").

### A. `ProviderErrorCategory` semantic enum

Lives in `application/workflow/aiReply.h`.  Parsers populate it; UI branches on it.  Raw provider strings (`m_ProviderErrorCode`, `m_ProviderErrorType`) are kept on `AiError` for logs/debugging but never branched on in UI code — this keeps provider details out of React.

```cpp
enum class ProviderErrorCategory : uint8_t
{
    Unknown,            // body unparseable or no recognized discriminator
    BillingExhausted,   // OpenAI insufficient_quota, Anthropic credit_balance_too_low, Gemini BILLING_DISABLED, Bedrock ServiceQuotaExceededException
    ThrottleRateLimit,  // genuine throttle; Retry-After applies; AIMD operating normally
    AuthFailure,        // bad API key / expired credential / wrong region
    ServiceOverload,    // provider-side capacity, transient (Anthropic overloaded_error, Gemini UNAVAILABLE)
    ModelNotFound,      // model_not_found / deprecated / not-available-on-account
    InvalidRequest,     // 4xx with malformed input (caller bug, won't retry)
};
```

Closed enum — `static_assert(static_cast<int>(ProviderErrorCategory::InvalidRequest) == 6)` at switch sites per `feedback_cpp_discipline`.

### B. `ParseOpenAiStyleError` shared helper

API1 (OpenAI Chat) and API2 (OpenAI Responses) duplicate `ParseError` logic byte-for-byte — they share the OpenAI body shape.  Azure OpenAI reuses API1's parser, inheriting the duplication.  One free function consumed by all three sites:

```cpp
// In application/json/replyParser.cpp (or sibling util TU).
// Returns a populated ErrorInfo from an OpenAI-style error body.
// Consumed by replyParserAPI1 + replyParserAPI2 (and indirectly Azure via API1).
ErrorInfo ParseOpenAiStyleError(simdjson::dom::object const& errorObj);
```

No interface, no template — a free function.  Classification is a pure function per provider (no state); strategy pattern would be over-engineering.

### C. `ProviderHealthSnapshot` struct

Lives in `engine/curlWrapper/curlMultiDispatcher.h`.  Single-snapshot-per-interface value type for the AIMD state.  One getter returns an internally consistent snapshot inside a single critical section — beats threading 6+ atomics through the public surface and avoids torn reads.

```cpp
struct ProviderHealthSnapshot
{
    InterfaceType m_Interface;
    std::string m_InterfaceName;                              // user-configured label from config.json
    bool m_IsMock;                                            // true if served by MockTransport — included in this struct's initial creation in Sitting 8
    int m_CurrentCap;
    int m_MaxCap;
    int m_FloorCap;
    std::chrono::system_clock::time_point m_LastErrorAt;
    std::string m_LastErrorCode;
    std::string m_LastErrorType;
    ProviderErrorCategory m_LastErrorCategory;
    uint64_t m_ConsecutiveErrors;
    uint64_t m_SuccessStreakSinceLastError;
    std::chrono::system_clock::time_point m_CapPinnedAtFloorSince;  // for "sustained cap-at-floor" LED red rule
};

// On CurlMultiDispatcher's per-interface rate controller:
ProviderHealthSnapshot SnapshotHealth() const;  // single critical section, atomic copy
```

`m_InterfaceName` is the user-configured label — what surfaces in UI labels and banner copy.

### Final `AiError` shape

```cpp
struct AiError
{
    enum class Kind { None, Http, Parse, SchemaValidation, Timeout, Transport, Provider };
    Kind m_Kind;
    int m_HttpStatus;
    std::string m_Message;

    // Added by workstream B + E
    std::string m_ProviderErrorCode;                          // raw, e.g. "insufficient_quota" — logs only
    std::string m_ProviderErrorType;                          // raw, e.g. "insufficient_quota" — logs only
    ProviderErrorCategory m_Category{ProviderErrorCategory::Unknown};   // UI branching
    std::optional<int> m_RetryAfterSeconds;                   // from Retry-After header when present
};
```

`std::optional<int> m_RetryAfterSeconds` per `feedback_rust_emulating_defaults` ("`std::optional` over nullable ptr") — used by the popover's "Retrying in 12s…" affordance.

### Wire formats

**`.meta.json` fixture-sidecar schema** (Foundation F2):

```jsonc
{
  "http_status": 200,                   // int, allowlist {200..599}
  "headers": {                          // object, key allowlist {"Content-Type", "Retry-After"}
    "Content-Type": "application/json",
    "Retry-After": "12"
  },
  "body_path": "subpath/to/body.json"   // optional, ConfineUnderProjectRoot-resolved; omit to use fixture body verbatim
}
```

**PROV sidecar additions** (Foundation F2):

```jsonc
{
  // ... existing PROV fields ...
  "mocked": true,                       // present and true when call routed through MockTransport
  "fixture_path": "fixtures/api1/golden_success.json"   // present only when mocked=true
}
```

**WebSocket `ai-call-failed` payload additions** (workstream B):

```jsonc
{
  // ... existing fields (workflow_id, run_id, task_name, error_kind, http_status, error_message) ...
  "provider_error_code": "insufficient_quota",
  "provider_error_type": "insufficient_quota",
  "category": "BillingExhausted",       // string-serialized ProviderErrorCategory
  "retry_after_seconds": 12,            // optional, omitted when not present
  "interface_name": "my-openai-prod"    // user-configured label
}
```

### Out-of-scope abstractions (over-engineering at current size)

- **`IErrorClassifier` interface / strategy pattern.**  Pure functions per provider suffice.  Virtual dispatch for 5 always-known providers is overhead with no upside.
- **Sub-struct `AiError::Provider { code, type, category, retryAfter }`.**  Flat fields at the current size (8 total) beat nested.  Reconsider only if `AiError` grows past ~12 fields.
- **Generic WebSocket message builder.**  `BroadcastAiCallFailed` is one call site.  Not at the "extract before the third copy" threshold.

---

## 4. Workstreams

One Foundation workstream (3 sittings, prerequisite) plus five workstreams A–E (one sitting each unless flagged).  Sequence: Foundation → A → B → C → D → E.

### Workstream Foundation — MockTransport test + curated-mock infrastructure (3 sittings)

Workstreams A, D, and E need to exercise dispatcher behavior (retry exhaustion, AIMD cap halving, real per-interface ReplyParsers) on synthetic error responses — without burning real provider credit.  `MockTransport` replaces the curl layer below the dispatcher, leaving the dispatcher's full code path (queueing, AIMD, retries, parser dispatch) running with a fixture-driven mock at the bottom.

Architectural ground rules (per §1):

- MockTransport is available in **all 4 build targets** (Studio/Engine × Debug/Release).  Cyber-sec hardening is at the boundary, not enforced by build mode.
- Selection is config-driven via the orthogonal `is_mock: bool` flag on provider registry entries (admin-only, same access surface as `api_key`).
- TestInterface (`InterfaceType::Test`) is removed as part of F2.  `is_mock: true` + a real `api_type` is the strict superset.
- Live-call behavior is unchanged.  All existing tests using real providers pass before AND after Foundation sittings — verifiable: full test suite in Debug + Release after each sitting.

#### Foundation Sitting 1 — `IInterfaceTransport` abstraction + `LiveTransport` refactor (behavior-neutral)

**Boundary — narrow (curl bottom-half only).**  `LiveTransport` owns the curl machinery: easy-handle setup, `IAuthSigner::Apply` integration, multi-handle add/perform/info-read, response capture (body + raw headers + HTTP status + HTTP-version label), and the per-easy-handle slist / `m_Headers` / `m_PostData` storage.  Everything above that — inbox, AIMD admission gate, per-(host, modelFamily) `RateLimitController`s, retry queue, cascade-cancellation queue, debug counters, the I/O thread — stays in `CurlMultiDispatcher`.  The transport hands raw response headers + status back to the dispatcher so the existing `ParseRateLimitHeaders` path keeps driving real AIMD on every request (including, in Sitting 2, synthetic responses from MockTransport).  This is what makes the originating billing-vs-throttle bug directly testable in CI without touching real providers.

Wire shape (resolved at the start of Sitting 1, captured here so the boundary can't drift later):

```cpp
// engine/curlWrapper/interfaceTransport.h
class IInterfaceTransport
{
public:
    struct Response
    {
        QueryResult m_Result;             // ok or transport-level error (CURLcode / pre-flight)
        std::string m_Body;               // response body
        std::string m_RawHeaders;         // unparsed header block — fed into ParseRateLimitHeaders
        long        m_HttpStatus{0};      // CURLINFO_RESPONSE_CODE
        std::string m_HttpVersionLabel;   // "HTTP/2", "HTTP/1.1", "HTTP/1.0"
    };
    using Callback = std::function<void(Response)>;

    virtual ~IInterfaceTransport() = default;

    // Submit a request.  Fires `cb` exactly once when the request completes
    // (asynchronously for LiveTransport; may be synchronous for MockTransport).
    virtual void Submit(CurlWrapper::QueryData const& queryData, Callback cb) = 0;

    // Cascade-cancellation hook — abort any in-flight requests matching cancelKey.
    // LiveTransport drives `curl_multi_remove_handle` + cleanup; MockTransport may no-op
    // when its responses are always synchronous.
    virtual void Cancel(std::string const& cancelKey) = 0;

    // I/O-thread tick — called from the dispatcher's I/O loop.  LiveTransport drives
    // `curl_multi_perform` + `curl_multi_info_read` here.  MockTransport no-ops.
    virtual void Pump() = 0;
};
```

Touchpoints:
- New `engine/curlWrapper/interfaceTransport.h` — defines `IInterfaceTransport` per the wire shape above.
- New `engine/curlWrapper/liveTransport.{h,cpp}` — extracts the curl + auth-signer + multi-handle path verbatim into `LiveTransport : IInterfaceTransport`.  Includes `SetupEasyHandle`, the multi-handle pump body from `IoThreadFunc`, completion harvesting via `curl_multi_info_read` from `DrainCompleted`, and per-handle cleanup.  `ParseRateLimitHeaders` stays in `CurlMultiDispatcher` and is called by the dispatcher when it receives a `Response` from the transport (so AIMD remains single-sourced).  Refactor, not rewrite: lines move with minimal modification; comments + log lines preserved verbatim.
- `engine/curlWrapper/curlMultiDispatcher.{h,cpp}` — dispatcher constructs and holds `std::unique_ptr<IInterfaceTransport>` defaulting to `LiveTransport`.  `Submit()` forwards to the transport's `Submit`; the dispatcher's I/O thread calls `m_Transport->Pump()` each tick and routes the `Response` into the existing retry/AIMD/completion-callback path.  `CancelByCancelKey` forwards the matching cancel to the transport while also draining the inbox + retry queue locally.
- `engine/json/configParser.{h,cpp}` — adds `is_mock: bool` + `fixture_path: string` fields to the provider registry schema.  Parsed, validated for type, stored on the interface struct.  Not yet acted on (`[[maybe_unused]]` until F2 wires dispatch selection).
- **`ProviderHealthSnapshot` deferred to Sitting 8.**  The plan previously listed `m_IsMock` on this struct as a Sitting-1 deliverable; the struct itself is only introduced in workstream D (Sitting 8).  Letting it land complete there with `m_IsMock` as one of its initial fields beats a half-defined skeleton sitting for seven sittings.

Acceptance:
- All existing tests pass — `test/dispatch/*` full suite + `test/run_tests.py --all` + assistant tests.  No behavioral change end-to-end.
- All 4 binaries build + green (Studio Debug, Studio Release, Engine Debug, Engine Release).
- `is_mock: true` in config.json parses without error; LOG_APP_INFO on startup confirms the flag was seen.
- Dispatcher → transport diff is verifiable as a verbatim move of the curl path: comments preserved, log lines preserved, no AIMD / retry / cancel logic moved out of `CurlMultiDispatcher`.

Risk: low (refactor with no behavior change) but bounded because the refactor touches the dispatcher's hot path.

#### Foundation Sitting 2 — `MockTransport` + cyber-sec hardening + dispatch wiring + TestInterface removal + JCWF spec update

The behavior-change sitting.  All §1 hardening items land in the FIRST commit, not retrofitted.

Touchpoints:
- New `engine/curlWrapper/mockTransport.{h,cpp}` — `MockTransport : IInterfaceTransport`.  Reads fixture body from `fixture_path` (config-supplied, `ConfineUnderProjectRoot`-gated).  Optionally reads sibling `.meta.json` per §3.5 wire format (status allowlist `{200..599}`; header allowlist `{Content-Type, Retry-After}`).  Per-fixture size cap (final value at implementation time).  All paths logged + audited.
- `engine/curlWrapper/curlMultiDispatcher.{h,cpp}` — selection logic: if `is_mock: true` on the provider → `MockTransport` for that request; else `LiveTransport`.
- `application/workflow/aiRequestPool.{cpp,h}` — remove TestInterface code path at `aiRequestPool.cpp:1304-1393`.  All ai_calls now go through `CurlMultiDispatcher` regardless of mock status; mock-ness is a transport-layer concern, transparent to the request-pool.
- PROV writer site (`application/workflow/aiInvocation.h` or sibling) — add `mocked: true` + `fixture_path` fields to PROV sidecar per §3.5 wire format when call routed through MockTransport.
- `LOG_APP_INFO` on first call routed through MockTransport per provider after startup: `"AiRequestPool: provider '{}' configured with is_mock=true; serving from fixture '{}'"`.
- `engine/json/configParser.{h,cpp}` — validate `is_mock: true` requires `fixture_path` to be present and `ConfineUnderProjectRoot`-resolvable; reject with ERROR otherwise.  Validate `api_type` is not `"Test"`; legacy `"Test"` value → ERROR with migration message pointing at `is_mock` flag.
- `doc/JC_Workflow_Specification.md` line 1118 — remove `"Test"` from the api_type list.  Add an `is_mock` row to the provider registry table.  Spec describes the user-visible flag only, no implementation details.
- `doc/jarvisagent.md` (config reference) — document `is_mock` + `fixture_path` fields with hardening notes (admin-only, path-confined, size-capped).
- Migration sweep — any test JCWF or example config using `api_type: "Test"` updates to `api_type: "API1"` (or whichever was intended) + `is_mock: true` + `fixture_path: ...`.

Acceptance:
- All 4 binaries build + green.
- Synthetic test (any build): provider with `is_mock: true` + a `golden_success.json` fixture → ai_call returns the canned reply through the real parser → PROV sidecar contains `"mocked": true`.
- Cyber-sec acceptance:
  - Fixture path attempting `../../etc/passwd` → rejected at config parse time with ERROR.
  - Fixture file exceeding the size cap → rejected at load time with ERROR.
  - `.meta.json` with `http_status: 999` → rejected with ERROR.
  - `.meta.json` with `headers: { "X-Evil": "..." }` → header dropped (not in allowlist) + WARN log.
  - Fixture containing malformed UTF-8 → bytes flow through `SanitizeUtf8` before reaching logs/TUI.
- TestInterface removal: grep for `InterfaceType::Test` returns zero hits in `application/` + `engine/`.
- Legacy config with `api_type: "Test"` → ERROR at startup with migration guidance.

Risk: medium-high.  Carries the secure-by-default obligation; getting any boundary mitigation wrong is the worst-case outcome.  Sitting closes only when every cyber-sec acceptance bullet has a corresponding green test.

#### Foundation Sitting 3 — Per-interface fixture batteries + parser-level fault tests + TUI safety + curated demo JCWF fixtures

Touchpoints:
- `test/dispatch/fixtures/api1/` through `test/dispatch/fixtures/api6/` — capture (from real provider responses where possible) OR hand-craft fixtures.  Per interface:
  - `golden_success.json` — known-good response.
  - `error_billing.json` — provider's billing-exhaustion variant.
  - `error_throttle.json` — provider's throttle variant.
  - `error_auth.json` — auth failure.
  - `error_overload.json` — provider-side overload variant.
  - `malformed_utf8.json` — byte-level pathology: orphan continuation bytes, surrogate halves, truncated multi-byte sequences, overlong encodings, codepoints > U+10FFFF.
  - `truncated_response.json` — body cut off mid-stream.

  API6 (Azure OpenAI) reuses API1's body shape verbatim; its battery is a parity-validation copy rather than independent research.  Per-API body shapes: api1 + api6 + api2 follow the OpenAI envelope; api3 uses the Gemini error envelope (`error.code` int + `error.status` enum + `error.details[*]`); api4 uses the Anthropic nested-error envelope (`{"type":"error","error":{"type":"...","message":"..."}}`); api5 uses the AWS `__type` envelope.
- New test drivers `test/dispatch/test_api{1..6}_mock_errors.py` — one per interface, parametrized across the fault fixtures.  Each test configures the provider with the fixture + `is_mock: true`, runs a synthetic ai_call, and asserts (a) dispatch ended in failure with the expected HTTP status, (b) the PROV sidecar contains `"mocked": true` + correct `fixture_path`, (c) an ERROR (or, for recoverable variants, WARN) log line fires referencing the runId, (d) (after Workstream A landed in Sitting 5) the body discriminator from `m_ProviderErrorType` appears on the ERROR line.  WS-payload fields (`m_ProviderErrorCode`, `m_Category`, `m_RetryAfterSeconds`, `interface_name`) are covered by the separate `test_ws_ai_call_failed_payload.py` driver shipped with Workstream B (Sitting 4).  api5 still asserts HTTP status only — Workstream E (Sitting 6) lands the AWS `__type` pre-parse and api5 tightens then.
- TUI byte-safety stress test — new `test/dispatch/test_tui_stress_malformed_utf8.py`: drive ~100 concurrent ai_call tasks through MockTransport serving the `malformed_utf8.json` battery.  Assertions:
  - j9t process survives (`pgrep` confirms still alive).
  - `log/log.txt` remains valid UTF-8 throughout (Python `'strict'` decode mode).
  - TUI does NOT crash (no `terminate()`, no signal-kill, no ncurses stderr noise).
  - Terminal state recoverable after test.
- Real-world content TUI stress — fixtures including (a) n8n webhook callback payload samples (verbose JSON with embedded HTML / unicode emoji / multi-byte CJK), (b) Polarion XML output samples (declared encoding mismatches, BOM marks, mixed line endings), (c) deliberately ugly logger content (CSI/escape sequences, NUL bytes, RTL/LTR override marks).  Confirms TUI handles real-world ugly external content, not just hand-crafted bad UTF-8.
- Curated demo JCWF fixtures — capture/hand-craft canned responses for mockable demo JCWFs:
  - `aiZipDemo` — single-shot summarization; one canned response fits.
  - `bookSummary` — text summarization; one canned per-section response, possibly a small set for fan-out variety.
  - Stretch (verify feasibility, not blocking): `jarvisCppDocu`, `jarvisCppCyberSecAudit`, `jarvisCppSafetyAudit` (144 calls each, structurally repetitive — a handful of templates may cover them).
  - Not mockable, documented as such: any JCWF where the AI response feeds into `g++` / `python -c` (response must be syntactically valid compilable code that adapts to input — canned responses cannot satisfy this).  Listed in the curated-fixture doc with a one-line rationale per JCWF.

Acceptance:
- All per-interface fault tests pass.  Parity matrix at §3 has every cell ground-truthed against captured fixtures (api1-6).
- TUI stress tests pass (process survives, log stays valid UTF-8, no crashes, terminal recoverable).
- §19 SanitizeUtf8 verification gap is closed and documented — the captured fixtures + tests are the verification artifact.
- All fixtures committed under `test/dispatch/fixtures/api{1..6}/` + `test/dispatch/fixtures/demos/<jcwf-name>/`.
- `aiZipDemo` + `bookSummary` runnable end-to-end via `is_mock: true` against a Release binary, verified by workflow-shape correctness.

Risk: medium.  Fixture capture is research-style; wire shape per provider matters (especially Gemini's `error.details[*]` and Bedrock's `__type` envelope).  Demo-JCWF fixture curation is judgment-heavy — when a single canned response stops covering a fan-out, decide between adding fixtures and marking the JCWF as not-fully-mockable.

---

### Workstream A — Log enrichment (one sitting) — ✅ landed 2026-05-15 (Sitting 5)

Goal: the final 429 ERROR line carries the parsed `error.code` + `error.type` + `m_Category` + runId, so the run analyzer surfaces "billing vs throttle" without any new event types or UI work.

What landed:
- `application/workflow/aiRequestPool.{h,cpp}::OnRequestFailed` — signature changed from `(path, std::string message)` to `(path, AiError const&)`.  Emits a single consolidated ERROR with the body discriminator + semantic category + runId/workflowId/taskId, matching CLAUDE.md's "Subsystems without run context return errors via their data types and let the upstream caller — which has the runId in scope — emit the ERROR log."  Shipped line shape (verified in `log/log.txt`):
  ```
  [error] [AiRequestPool] OnRequestFailed HTTP 429 (code='insufficient_quota',
          type='insufficient_quota', category=BillingExhausted)
          run='adhoc_…' workflow='_adhoc_…' task='echo'
          message='You exceeded your current quota, …' path='…'
  ```
  Three call sites updated: the curl-callback error branch, the exception path, and the schema-retry submission-failure path (the last constructs an `AiError{Kind::SchemaValidation, …}` so the consolidated line still gets a kind + message).
- `engine/curlWrapper/curlMultiDispatcher.cpp` — the existing `LOG_CORE_ERROR("HTTP 429 ... retries exhausted ...")` line drops to `LOG_CORE_WARN`.  The enriched ERROR fires from `aiRequestPool::OnRequestFailed`; the dispatcher's WARN keeps the curl-side technical context (cancelKey + quotaKey + retry count) without creating a duplicate ERROR for the same failure.
- The intermediate per-attempt WARN at line ~764 was **not** extended with code/type — the dispatcher doesn't parse the body inline, and adding a body-parse on every retry attempt would be wasteful for a WARN that already has the host + HTTP-status + attempt count it needs.  Final user-visible enrichment happens at the OnRequestFailed boundary, which has the parsed body in scope.
- `test/dispatch/test_api{1,2,3,4,6}_mock_errors.py` — 20 driver cases tightened: `expected_log_substring` changed from HTTP-status (`(429)`, `(401)`, `(503)`) to body discriminator (`insufficient_quota`, `rate_limit_error`, `RESOURCE_EXHAUSTED`, `credit_balance_too_low`, `overloaded_error`, etc.).  api1/2/6 use OpenAI types; api3 uses Gemini's `status` enum (with `RESOURCE_EXHAUSTED` colliding on billing+throttle until Workstream E disambiguates via `details[*].reason`); api4 uses Anthropic's nested `error.type`.  **api5 keeps HTTP-status substrings** because the AWS `__type` envelope isn't recognised by any ReplyParser today — Workstream E (Sitting 6) lands the `__type` pre-parse and api5 tightens then.

Acceptance (all verified 2026-05-15):
- ✅ MockTransport 429-`insufficient_quota` body → ERROR log contains `insufficient_quota`, `BillingExhausted`, and the runId.
- ✅ MockTransport 429-`rate_limit_error` body → different `code`, different `category` (`ThrottleRateLimit`) in the log line.
- ✅ Run analyzer surfaces both cases as distinct issues against the affected runId (each has a unique `code` + `category` substring).
- ✅ Full dispatch suite 24/24 on Debug; 36-case fault matrix 6/6 per API; WS payload + TUI byte-safety stress unchanged.

Risk: very low.  Logging change behind the existing log macros (`feedback_use_log_macros`).

### Workstream B — `AiError` plumbing + §3.5 abstractions (one sitting)

Goal: the discriminator survives all the way from `ReplyParser` to the WebSocket payload.

Touchpoints:
- `application/workflow/aiReply.h` — extend `AiError` per §3.5 final shape: add `m_ProviderErrorCode`, `m_ProviderErrorType`, `m_Category` (new `ProviderErrorCategory` enum), `m_RetryAfterSeconds` (`std::optional<int>`).
- `application/json/replyParser.cpp` (or sibling util TU) — introduce `ParseOpenAiStyleError(simdjson::dom::object)` free function per §3.5-B.
- `application/json/replyParserAPI1.cpp::GetError()` (around line 71-92) — call the new shared helper; populate `m_ProviderErrorCode`, `m_ProviderErrorType`, classify into `m_Category` (`insufficient_quota` → `BillingExhausted`, `rate_limit_error` → `ThrottleRateLimit`, `authentication_error` → `AuthFailure`).
- `application/json/replyParserAPI2.cpp::GetError()` (around line 345-379) — replace inline duplicate with shared helper; same classification.
- `application/json/replyParserAPI3.cpp` / `API4.cpp` / `API5.cpp` — extend signatures to populate the new fields with `Unknown` category by default; full classification for non-OpenAI providers lives in workstream E.
- `application/workflow/aiRequestPool.cpp` — capture `Retry-After` header on 429 from the curl response (currently only the AIMD path reads it) and stash in `AiError.m_RetryAfterSeconds`.
- `application/web/webServer.cpp::BroadcastAiCallFailed` (around line 4406-4421) — add `provider_error_code`, `provider_error_type`, `category` (string-serialized), `retry_after_seconds`, `interface_name` to the WebSocket message JSON per §3.5 wire format.

Acceptance:
- WebSocket consumer (test script using j9t MCP or raw WS client) connects and receives a synthetic 429; the message has the new fields populated.
- Existing log lines that render `AiError` are reviewed — no regressions; the new fields are additive.

Risk: low.  Struct extension + JSON field additions.  Field names are deliberately neutral (`m_ProviderErrorCode`, not provider-specific).

### Workstream C — Reactive UI banner + hazard glyph (one sitting) — ✅ landed 2026-05-15 (Sitting 7)

Goal: when an AI call fails with a recognized actionable category, the dashboard shows a banner identifying the affected interface.  Workflow rows whose interfaces are degraded get an inline hazard glyph.  All four categories (`BillingExhausted`, `AuthFailure`, `ModelNotFound`, `ServiceOverload`) land in this sitting.

What landed:
- **`dashboard/ui/src/components/WorkflowsPanel.tsx`** — new `ProviderAlertBanner` block rendered above the workflows table.  Mirrors `.no-keys-banner` (top-of-panel, body text + dismiss X) but uses the red severity tokens.  Copy templates per §5.3 keyed by category: BillingExhausted → "Account billing limit reached", AuthFailure → "Interface credentials rejected", ServiceOverload → "Interface overloaded — retrying", ModelNotFound → "Requested model not available".  Throttle + Unknown + InvalidRequest are deliberately silent (AIMD handles throttling; the AI Health LED in Sitting 8 surfaces it instead).
- **`dashboard/ui/src/hooks/useWebSocket.ts`** — first dashboard handler for `ai-call-failed`.  Dedup state: `Map<"${interface}|${category}", ProviderAlertEntry>` (count, firstSeenAt, lastSeenAt, errorCode, errorType, message, retryAfterSeconds, httpStatus).  Burst dedup happens at insert time — N identical failures produce 1 banner with "Affected calls: N".  Also handles `ai-call-completed` to auto-clear every alert keyed by the same interface.  `dismissProviderAlert` callback for the X-button.
- **`dashboard/ui/src/App.css`** — `.provider-alert-banner` (red severity), `.provider-alert-body`, `.provider-alert-dismiss`, plus a `.hazard-icon-red` modifier on the existing `.hazard-icon` for the per-row degraded-interface variant.
- **`dashboard/ui/src/components/WorkflowsPanel.tsx`** — per-row red hazard glyph: scans each workflow's `interface_names` against the active alert map, paints `⚠` red with tooltip `AI interface '<name>': <category>` when any match.  Severity order: Billing > Auth > ModelNotFound > ServiceOverload.
- **`application/web/webServer.cpp`** — `/api/workflows` serializes `interface_names: string[]` (from `WorkflowDefinition::m_RequiredAiProviders`).  Lets the dashboard mark only affected rows, not every `has_ai_call` row.
- **`application/workflow/workflowRegistry.cpp`** — populates `m_RequiredAiProviders` at workflow-load time (the field was declared but never populated — dead since introduction; this also fixes the latent "required providers exist" check in `workflowRuntimeManager.cpp:2801` that was a no-op).  Inline `ExtractProviderFromParams` simdjson helper; updated both the root-load and sub-workflow-load scan loops in lockstep.
- **`application/workflow/aiCallEvents.h` + `application/web/webServer.{h,cpp}` + `application/jarvisAgent.cpp` + `application/workflow/aiRequestPool.cpp`** — `AiCallCompletedEvent` gained `m_InterfaceName` (parallel to Sitting-4's `AiCallFailedEvent`); `BroadcastAiCallCompleted` gained the `interface_name` JSON field.  This is what makes the dashboard's auto-dismiss actually fire — the original `ai-call-completed` payload was probName-only, so the dashboard had no way to identify which interface had recovered.

Acceptance (verified 2026-05-15):
- ✅ E2E per category: MockTransport burst with each recognized category → one banner per `(interface_name, category)` (3 distinct banners for billing + auth + overload combos).  Verified visually via JC's screenshot.
- ✅ Burst dedup: 3 identical billing failures → one banner with `Affected calls: 3`.
- ✅ X-dismiss removes a single banner; re-firing brings it back at count=1.
- ✅ Auto-dismiss: success call against `mock_demo_billing` → BillingExhausted banner vanishes within ~1s; other banners (different interfaces) untouched.
- ✅ Generic copy (no provider brand names; interface label only).
- ✅ All 4 binaries green; 36-case fault matrix + 8-case WS payload unchanged.

Deferred (out of scope, captured as follow-ups in the Sitting-7 hand-off):
- Page refresh persistence — banners are in-memory only; the Sitting-8 `/api/providers/health` endpoint will hydrate cross-refresh state.
- Orphaned-banner cleanup after interface delete — server-side delete doesn't broadcast a "stop tracking this interface" signal, so banners from deleted mock interfaces persist until X-dismissed or refresh.  Post-1.0 polish (Sitting 8 may add the broadcast trivially as part of `/api/providers/health`).
- "Dismiss all" button — handy after a test-suite burst stacks ~18 banners.  5-line React + 1-line hook extension.  Post-1.0 polish.

Risk: medium.  Touches three layers (WS schema, React component, dedup state).

### Workstream D — Proactive AIMD cap render (one sitting) — ✅ landed 2026-05-15 (Sitting 8)

Goal: live view of AIMD throttle state in the dashboard.  Catches degraded-throughput situations before a fail event, including intermittent 429s where the cap drifts but doesn't pin.

Complementary to workstream C: C is reactive (diagnosis-by-failure); D is proactive (diagnostic surface, visible even with no current failure).

What landed (backend):
- **`application/workflow/providerHealth.h`** (new) — `ProviderHealthSnapshot` value type per §3.5-C: per-interface AIMD cap state (`m_CurrentCap` / `m_MaxCap` / `m_FloorCap`) + last-error fields (raw code/type/message, category, http status, retry-after) + counters (consecutive errors, success streak) + `m_CapPinnedAtFloorSince` for the safety-net rule.  Sibling `InterfaceHealthState` for the pool's mutable tracking.
- **`application/workflow/aiRequestPool.{h,cpp}`** — `m_HealthMutex`-guarded `m_HealthPerInterface: unordered_map<string, InterfaceHealthState>`; updated by the curl callback on every completion (success bumps streak; failure records last-error).  `SnapshotProviderHealth()` joins per-interface state with the dispatcher's `GetDebugSnapshot().m_Controllers` (cap from controller, last-error from health map) inside a single critical section.  Pin-tracking: after the last-error update, callback re-reads the dispatcher cap and flips `m_CapPinnedAtFloorSince` at floor-boundary crossings.
- **`application/web/webServer.{h,cpp}`** — new `GET /api/providers/health` endpoint serving the snapshot vector (one entry per configured `api_interfaces[]`) as JSON.  Timestamps wired as Unix milliseconds (Date constructor friendly).  All 4 build targets — no debug gating.
- **`engine/event/event.h`** — new `EventType::AiCapChanged`.
- **`application/workflow/aiCallEvents.h`** — `AiCapChangedEvent` (payload-free wake signal).
- **`engine/curlWrapper/curlMultiDispatcher.{h,cpp}`** — new `OnCapChangedCallback` set once at construct time; `ParseRateLimitHeaders` snapshots cap before/after `controller.Observe()` and fires the callback when the value mutates.  `application/jarvisAgent.cpp` registers the callback to push `AiCapChangedEvent` and routes the event to `webServer.BroadcastCapChanged()` which broadcasts `{"type":"cap-changed"}`.  Bounded broadcast rate: dispatcher only fires on actual mutation, not every observation.

What landed (frontend):
- **`dashboard/ui/src/types.ts`** — `ProviderHealth` + `ProvidersHealthResponse` types mirror the wire shape from `HandleProvidersHealthGet`.
- **`dashboard/ui/src/api.ts`** — `fetchProvidersHealth()` (gracefully returns empty interfaces on 401 / network failure).
- **`dashboard/ui/src/hooks/usePolling.ts`** — extended to fetch `/api/providers/health` alongside the existing endpoints on each 5s tick.
- **`dashboard/ui/src/hooks/useWebSocket.ts`** — handler for `cap-changed` messages; calls a registered callback (set by App.tsx to `usePolling.refresh`) to refetch the snapshot within milliseconds instead of waiting for the next poll cycle.
- **`dashboard/ui/src/components/StatusBar.tsx`** — 6th LED in the existing `.led-group`: severity computed per §4-D (recent-severe within 30s, sustained-pin >30s — see note below).  Click toggles a click-anchored popover with the per-interface table: Interface | Cap | Last error (category badge + raw code + relative time) | Retry in.  `(mocked)` badge for `is_mock` interfaces.  Shared-cap footnote when 2+ interfaces share a `quota_key`.  Frontend pin-tracking via `useRef` as a safety net for the rare case where the backend timestamp lags.
- **`dashboard/ui/src/App.css`** — `.ai-health-popover` + `.cat-badge` + `.cat-badge-red/amber/muted` + `.ai-health-mock` + `.ai-health-shared` + `.ai-health-footnote`.

**LED color rules (as shipped):**

| State | Trigger |
|---|---|
| Green `#22c55e` | All configured interfaces at ceiling, no recent errors |
| Amber `#eab308` | Any interface with `cap < ceiling` (AIMD throttling — normal recovery) |
| Red `#ef4444` | (a) Any interface with `cap == floor` for >30s sustained (`cap_pinned_at_floor_since_ms` > 30s ago), OR (b) any interface in `BillingExhausted` / `AuthFailure` / `ServiceOverload` / `ModelNotFound` within last 30s |
| Grey `#334155` | No interfaces configured |

Threshold tuning note: the dev plan originally specified 60s for the sustained-pin window.  Shipped value is **30s**, aligned with the 30s recent-severe window.  Rationale: the 30s→60s gap created a 30-second amber dip mid-outage where the provider was still failing but the LED briefly suggested recovery.  Aligning both windows at 30s gives continuous red coverage through sustained outages.  Rule (a) remains the safety net for `m_Category=Unknown` cases.

Acceptance (verified 2026-05-15 via JC screenshots + curl probes):
- ✅ Run a JCWF that drives the cap up: popover shows `1/48` for the affected interface, real interfaces show `—` when not yet dispatched.
- ✅ MockTransport 429 burst: cap halves observable in `/api/providers/health` within ms (cap-changed WS event fires per Observe mutation; dashboard refetches on receipt).
- ✅ Leave cap pinned past the 30s window: LED stays red continuously via the sustained-pin rule (backend timestamp + frontend safety net).
- ✅ Page refresh during ongoing issue: backend `cap_pinned_at_floor_since_ms` survives the reload, LED renders red immediately on mount.

Risk: low-medium.  Mostly new components on existing data.

Deferred (out of scope, captured as follow-ups):
- **First-observation pin-set edge case** — on the very first completion against a fresh controller, the pin update reads cap AFTER `Observe` ran but in the rare race where cap was briefly > floor between billing-burst halves the pin may not get set until the next completion.  Verified harmless: the frontend safety-net pin tracker fills in within 30s.
- **Pin update only fires on AiRequestPool curl callbacks** — interfaces sharing a quotaKey with a degraded one won't get their pin field updated until they themselves dispatch.  Acceptable for shipping: `/api/providers/health` shows the same cap value (joined via quotaKey) for shared-controller interfaces, and the dashboard's shared-cap footnote makes the relationship visible.

### Workstream E — Cross-provider error parsing extensions (one sitting) — ✅ landed 2026-05-15 (Sitting 6)

Goal: API3 (Gemini), API4 (Anthropic), API5 (Bedrock) parse their billing-exhaustion + throttle discriminators with the same fidelity as API1.  API6 (Azure OpenAI) inherits API1 transparently — no parser change needed; Sitting 3's api6 fixtures are the regression guard.

What landed:
- `application/json/replyParserAPI3.{h,cpp}` — `ErrorInfo` gained `m_DetailReason` (first `error.details[*].reason` value).  `ParseError` now walks the `details[]` array (capped at 32 entries) and captures the first `reason` string.  `GetError` surfaces the detail reason as `m_ProviderErrorCode` (so `USER_PROJECT_QUOTA_EXCEEDED` vs `RATE_LIMIT_EXCEEDED` distinguishes billing from throttle even when both share `status=RESOURCE_EXHAUSTED`) and classifies via reason→category mapping with status-only fallback for auth (`UNAUTHENTICATED`/`PERMISSION_DENIED`→`AuthFailure`), overload (`UNAVAILABLE`/`DEADLINE_EXCEEDED`→`ServiceOverload`), not-found (`NOT_FOUND`→`ModelNotFound`), invalid (`INVALID_ARGUMENT`/`FAILED_PRECONDITION`→`InvalidRequest`), and a conservative `RESOURCE_EXHAUSTED`-without-reason fallback to `ThrottleRateLimit`.
- `application/json/replyParserAPI4.cpp` — `GetError` extended with the full classification mapping: `credit_balance_too_low`→`BillingExhausted`, `rate_limit_error`→`ThrottleRateLimit`, `authentication_error`/`permission_error`→`AuthFailure`, `overloaded_error`/`api_error`→`ServiceOverload`, `invalid_request_error`→`InvalidRequest`, `not_found_error`→`ModelNotFound`.
- `application/json/replyParserAPI5.{h,cpp}` — new `BedrockFamily::AwsError` variant; `DetectFamily` re-iterates the body to probe `__type` after the family-shape probes.  New `ExtractAwsError` helper parses `__type` + `message` and strips any `com.amazonaws...#` prefix so the short exception name reaches classification.  New `ClassifyAwsBedrockException` mapper covers 11 exception names (`ServiceQuotaExceededException`→`BillingExhausted`, `ThrottlingException`→`ThrottleRateLimit`, `AccessDeniedException`/`UnauthorizedOperation`→`AuthFailure`, `ValidationException`→`InvalidRequest`, `ResourceNotFoundException`→`ModelNotFound`, `ModelStreamErrorException`/`ModelTimeoutException`/`ModelNotReadyException`/`InternalServerException`/`ServiceUnavailableException`→`ServiceOverload`).  Ctor populates `m_AwsErrorType` + `m_AwsErrorMessage` directly (no delegate) for the AwsError branch; `GetError` returns the classified AiError ahead of the delegate fallback.
- `test/dispatch/test_api3_mock_errors.py` — driver tightened from `RESOURCE_EXHAUSTED` (which collided on billing+throttle pre-Sitting-6) to `USER_PROJECT_QUOTA_EXCEEDED` / `RATE_LIMIT_EXCEEDED` (now distinguishable via the details reason).
- `test/dispatch/test_api5_mock_errors.py` — driver tightened from HTTP-status substrings to AWS exception names (`ServiceQuotaExceededException`, `ThrottlingException`, `AccessDeniedException`, `ModelStreamErrorException`).  Sitting 5's "api5 stays on HTTP status" exception is now closed.
- `test/dispatch/test_ws_ai_call_failed_payload.py` — expanded from 2 cases (api1 only) to **8 cases** spanning api1 + api3 + api4 + api5 with full WS-payload field assertions (`category`, `provider_error_code`, `provider_error_type`, `retry_after_seconds`, `interface_name`, `http_status`).  Drove out the hard-coded `api_label` JCWF naming (now `case["api_type"].lower()`).

Acceptance (all verified 2026-05-15):
- ✅ Parity matrix at §3 has every cell ground-truthed against the Sitting-3 fixtures.
- ✅ Each provider's billing-exhaustion code surfaces in the Workstream-A ERROR log identically to API1's — verified in `log/log.txt` across api1/3/4/5.
- ✅ Each provider's `category` surfaces in the Workstream-B WebSocket message — verified by the 8-case `test_ws_ai_call_failed_payload.py`.
- ✅ API6 (Azure) inherits identical behaviour without touching its dispatch path — api6 mock_errors driver pass with OpenAI discriminators.
- ✅ Full dispatch suite 24/24 on Debug; 36-case fault matrix 6/6 per API with body-discriminator assertions; TUI byte-safety stress 98/98.

---

## 5. UI signaling — reuse existing patterns

### 5.1 Existing inventory

The j9t dashboard uses vanilla React + vanilla CSS with hex-literal color tokens.  No MUI, no shadcn, no toast library.  No centralized notification manager.  New surfaces match the existing component vocabulary; no new visual language is introduced.

**Color tokens currently in use:**

| Token | Hex | Surface bg / border | Used by |
|---|---|---|---|
| Critical | `#ef4444` | `#2a1010` / `#4a1a1a` | `MasterPasswordDialog.mpd-error`, `.python-warning`, ScriptsPanel inline error |
| Warning | `#eab308` | `#2a1a00` / `#6b4f00` | `.no-keys-banner`, `.hazard-icon` |
| Success | `#22c55e` | — | Connection LED, completed counter |
| Muted | `#334155` / `#8a8a8a` | — | idle LEDs, queued state |
| Interactive | `#3b82f6` (blue), `#a855f7` (purple) | — | workflow-running LED, MCP LED, adhoc badge |

**Existing alert patterns:**

| Pattern | Behavior | Reference |
|---|---|---|
| `.no-keys-banner` (top-of-panel banner with optional action button) | Persistent until state resolves; two variants today | `WorkflowsPanel.tsx:105-130` + `App.css:148-176` |
| StatusBar LED row (5 LEDs today) | Always-on, color-coded by health, short label | `StatusBar.tsx:24-156` |
| Collapsed multi-source indicator ("N healthy" / "circuit open") | Established pattern for "many things, one LED + summary label" | `StatusBar.tsx:78-84` |
| `.hazard-icon` (inline ⚠ glyph) | Ambient indicator on workflow rows; rows also get `.row-no-keys` opacity | `WorkflowsPanel.tsx:155-159` |
| Inline error box (component-scoped) | Persistent until state clears | `ScriptsPanel.tsx:135-148` |
| `.python-warning` (inline status-bar text) | Persistent until subsystem recovers | `StatusBar.tsx:157-159` |
| Run-status chips (last-runs bar) | Inline color-coded glyphs (✓✗⊘…) + adhoc badge | `LastRunsBar.tsx:70-97` |
| Modal error (`MasterPasswordDialog.mpd-error`) | Reserved for modal-overlay flows | `MasterPasswordDialog.tsx:1-100` |

### 5.2 Mapping to this plan's surfaces

All user-facing labels use either (a) `m_InterfaceName` from `config.json`, (b) generic phrasing ("Account billing limit reached"), or (c) collapsed counts ("AI: 1 unavailable").  Banner action buttons are dismiss-only; no external links.

**(a) Billing-exhausted banner (workstream C)** — mirror `.no-keys-banner`:

| Aspect | Choice |
|---|---|
| Component shape | Same as `.no-keys-banner`: top-of-panel, body text + dismiss X |
| Severity color | Red (`#2a1010` / `#4a1a1a` / `#ef4444`) — amber is "configuration missing"; red is "active failure" |
| Placement | Top of Workflows panel (same as `.no-keys-banner`) |
| Persistence | Auto-dismiss on next successful call from same interface, OR user-dismiss for the session |
| De-dup | One banner per `(interface_name, category)` pair |
| Action | Dismiss only (X icon) |

Copy template:

```
Title:  "Account billing limit reached"
Body:   "Interface '<interface_name>' is rejecting requests (<raw provider_error_code>).
         Workflows using this interface are stalled on retries.
         Affected calls: <N>."
```

**(b) AI Health LED (workstream D)** — 6th LED in `.led-group`, color rules per §4-D.

Label format mirrors the existing Cloud Health collapsed-summary pattern:

```
"AI: healthy"           // all green
"AI: 2 throttled"       // 2 interfaces in amber
"AI: 1 unavailable"     // one interface in red, count-based
```

**(c) Per-interface detail popover (workstream D)** — click/hover on the AI Health LED:

| Column | Source |
|---|---|
| Interface (user-configured label) | `ProviderHealthSnapshot.m_InterfaceName` |
| Cap | `m_CurrentCap / m_MaxCap` (e.g. `12 / 16`) |
| Last error | `m_LastErrorAt` (relative) + `m_LastErrorCode` (raw) + category badge |
| Retry in | `m_RetryAfterSeconds` countdown when present |
| Badge | "(mocked)" when `m_IsMock` is true |

Density consistency with today — no separate chips in the status bar.

**(d) Inline hazard glyph (workstream C)** — extend `.hazard-icon`:

Workflow rows whose ANY ai_call task uses an interface in `BillingExhausted` / `AuthFailure` / `ServiceOverload` get the `⚠` glyph in red (`#ef4444`) instead of amber.  Tooltip: "AI interface '<interface_name>': <category-label>".  Same React component, same CSS class structure; additional state branch on category.

### 5.3 Category copy table

Same surfaces accommodate all four recognized categories with copy-only differences — phrasing generic, no provider names, no external links:

| Category | Banner title | LED behavior | Action |
|---|---|---|---|
| `BillingExhausted` | "Account billing limit reached" | red + count | Dismiss only |
| `AuthFailure` | "Interface credentials rejected" | red + count | Dismiss only (rotation via existing AI keys view) |
| `ServiceOverload` | "Interface overloaded — retrying" | red briefly; auto-clears on next success | none (informational) |
| `ModelNotFound` | "Requested model not available" | red + count | Dismiss only |
| `ThrottleRateLimit` | (no banner — normal operation, AIMD handles it) | amber when cap drops | none |

---

## 6. Sitting plan

| Sitting | Workstream | Output | Verification |
|---|---|---|---|
| ✅ **1** | Foundation 1 (`IInterfaceTransport` + `LiveTransport` refactor + `is_mock` schema parsed-but-unused) | `IInterfaceTransport` (narrow boundary: curl bottom-half only) + `LiveTransport` extracted; AIMD / retry / cancel stay in `CurlMultiDispatcher`; `is_mock` + `fixture_path` parsed by ConfigParser (no runtime effect yet).  Behavior-neutral.  **Landed 2026-05-14.** | All 4 binaries build + green.  `is_mock: true` in config parses without error.  Diff is a verbatim move of the curl path; no AIMD / retry / cancel logic relocated. |
| ✅ **2** | Foundation 2 (`MockTransport` all builds + cyber-sec hardening + dispatch wiring + TestInterface removal + JCWF spec update) | `MockTransport` implementation in all 4 builds with all §1 hardening from the first commit (path confinement, size cap, status/header allowlist, sanitization, INFO log, PROV `mocked: true`).  Dispatch wiring on `is_mock: true`.  TestInterface code path removed.  JCWF spec line 1118 updated.  Legacy `api_type: "Test"` → ERROR with migration message.  **Landed 2026-05-14.** | All 4 binaries build + green.  Synthetic test: `is_mock: true` provider with `golden_success.json` → ai_call returns canned reply through real parser → PROV contains `"mocked": true`.  Every cyber-sec acceptance bullet has a green test. |
| ✅ **3** | Foundation 3 (Per-interface fixture batteries + parser fault tests + TUI safety + curated demo JCWF fixtures) | Fixtures per API (success + 4 error variants + malformed UTF-8 + truncated body) across api1-6 (50 files), 7 thin per-API/TUI test drivers + 1 shared helper, curated demo fixtures for `aiZipDemo` + `bookSummaryPipeline` + README documenting mockable-vs-not-mockable.  §19 SanitizeUtf8 verification gap closed.  **Landed 2026-05-15.** | All per-interface fault tests pass (36 cases); TUI stress: 98 dispatches, log valid UTF-8, no unexpected criticals, j9t alive; `aiZipDemo` + `bookSummaryPipeline` runnable end-to-end via `is_mock: true` (verified). |
| ✅ **4** | B (`AiError` plumbing + §3.5 abstractions) | `ProviderErrorCategory` closed enum + `CategoryToString` in `aiReply.h`; `ParseOpenAiStyleError`/`ClassifyOpenAiStyleErrorType` shared helpers in `replyParser.{h,cpp}`; API1+API2 GetError/ParseError refactored to use helper; API3+API4 populate raw fields (Unknown category, full classification deferred to Sitting 6); `m_RetryAfterSeconds` threaded through the dispatcher's `Callback` 3rd arg + `ParseRateLimitHeaders` return; HTTP-error body re-parse in `aiRequestPool.cpp` so the dispatcher's short-circuit doesn't hide the discriminator; `AiCallFailedEvent` carries `m_InterfaceName`; `BroadcastAiCallFailed` extended with 5 new JSON fields.  **Landed 2026-05-15.** | All 4 binaries green.  WS test client (`test_ws_ai_call_failed_payload.py`) sees `category`, `provider_error_code`, `provider_error_type`, `retry_after_seconds=12`, `interface_name` on simulated 429s — 2/2 cases pass.  36-case fault matrix + TUI byte-safety stress still green.  Pre-Workstream-A: log line still carries HTTP-status only (will be enriched in Sitting 5). |
| ✅ **5** | A (log enrichment) | `OnRequestFailed(path, AiError const&)` (signature change — was `(path, std::string message)`) emits a single consolidated ERROR with `HTTP {status} (code='{ProviderErrorCode}', type='{ProviderErrorType}', category={CategoryToString(...)}) run='...' workflow='...' task='...' message='...' path='...'`.  Dispatcher's "HTTP 429 retries exhausted" line drops from ERROR to WARN (kept for curl-side technical context: cancelKey + quotaKey + retry count).  Three call sites updated: curl-error branch + exception branch + schema-retry submission failure (last builds an `AiError{Kind::SchemaValidation, …}`).  Sitting-3 drivers tightened to body discriminators (`insufficient_quota`, `rate_limit_error`, `RESOURCE_EXHAUSTED`, `credit_balance_too_low`, `overloaded_error`, etc.) for api1/2/3/4/6; api5 stays on HTTP-status until Workstream E (Sitting 6) lands the AWS `__type` pre-parse.  **Landed 2026-05-15.** | All 4 binaries green.  Full dispatch suite 24/24 on Debug.  36-case fault matrix 6/6 per API with body-discriminator assertions.  WS payload + TUI byte-safety stress still green.  Verified end-to-end: HTTP 429 `insufficient_quota` and HTTP 429 `rate_limit_error` produce distinct ERROR lines distinguishable by `category=BillingExhausted` vs `category=ThrottleRateLimit`. |
| ✅ **6** | E (cross-provider parsing — API3 Gemini, API4 Anthropic, API5 Bedrock) | API3: `m_DetailReason` field on `ErrorInfo` + `details[*]` walk (capped 32) → `m_ProviderErrorCode` carries `USER_PROJECT_QUOTA_EXCEEDED` / `RATE_LIMIT_EXCEEDED` / `BILLING_DISABLED`; classification via reason then status fallback.  API4: full `error.type` → category map (7 variants).  API5: new `BedrockFamily::AwsError` variant + `ExtractAwsError` helper (strips `com.amazonaws...#` prefix) + `ClassifyAwsBedrockException` mapper (11 AWS exception names); ctor populates `m_AwsErrorType` + `m_AwsErrorMessage` directly with no delegate; `GetError` returns the classified AiError ahead of delegate fallback.  API6 inherits API1 transparently — no parser change.  Drivers: api3 tightened to detail-reason strings, api5 tightened to AWS exception names; WS payload test expanded 2→8 cases across api1/3/4/5.  **Landed 2026-05-15.** | All 4 binaries green.  Full dispatch suite 24/24 on Debug.  36-case fault matrix 6/6 per API.  WS payload 8/8 covering BillingExhausted + ThrottleRateLimit + ServiceOverload across api1/3/4/5.  Verified in `log/log.txt`: Gemini `RESOURCE_EXHAUSTED` now distinguishable as billing vs throttle via the reason field; Anthropic `credit_balance_too_low` + `overloaded_error` classify correctly; Bedrock `ServiceQuotaExceededException` + `ThrottlingException` classify correctly. |
| ✅ **7** | C (banner + hazard glyph; all four categories) | `ProviderAlertBanner` block in `WorkflowsPanel.tsx` (mirror of `.no-keys-banner`, red severity); `useWebSocket.ts` gained first-in-dashboard handlers for `ai-call-failed` (dedup'd into `Map<"${interface}\|${category}", ProviderAlertEntry>` with count) + `ai-call-completed` (auto-clear by interface); `dismissProviderAlert` callback for X-button; `.provider-alert-banner` + `.hazard-icon-red` CSS tokens; `/api/workflows` serializes new `interface_names: string[]` from `WorkflowDefinition::m_RequiredAiProviders` (which was declared but never populated — Sitting 7 also fixed that dead-field latent bug).  `AiCallCompletedEvent` + `BroadcastAiCallCompleted` extended with `interface_name` so the auto-dismiss path actually has a key to match on (parallel to Sitting-4's `AiCallFailedEvent` change).  **Landed 2026-05-15.** | All 4 binaries green.  3-banner stack verified visually via JC's screenshot for BillingExhausted + AuthFailure + ServiceOverload combos.  Burst dedup verified: 3 identical billing failures → 1 banner, "Affected calls: 3".  X-dismiss removes individual banners.  Auto-dismiss on next success vanishes the matching banner within ~1s.  36-case fault matrix + 8-case WS payload still green. |
| ✅ **8** | D (`ProviderHealthSnapshot` + AI Health LED; hybrid live updates) | `ProviderHealthSnapshot` value type in new `application/workflow/providerHealth.h`; `AiRequestPool::SnapshotProviderHealth()` joins per-interface health (last-error + counters + cap-pin timestamp) with dispatcher's `GetDebugSnapshot()` controller cap; new `GET /api/providers/health` REST endpoint; new `EventType::AiCapChanged` + `AiCapChangedEvent` + dispatcher `OnCapChangedCallback` fired on cap mutation → `webServer::BroadcastCapChanged()` emits `{"type":"cap-changed"}` for sub-second LED updates without polling.  6th LED in `StatusBar.tsx` ("AI Health"); click-anchored popover with Interface/Cap/Last-error/Retry-in columns + `(mocked)` badge + shared-cap footnote.  Backend pin tracking populates `m_CapPinnedAtFloorSince` from curl callback; frontend pin tracker via `useRef` as safety net.  Sustained-pin threshold tuned from §4-D's 60s to **30s** to align with recent-severe window (avoids amber dip mid-outage).  **Landed 2026-05-15.** | All 4 binaries green.  REST endpoint returns 11 interfaces with cap state.  Verified visually via JC screenshots: LED renders `🔴 AI: 2 unavailable` while two mock interfaces are pinned at floor with `BillingExhausted`/`AuthFailure` categories; popover lists all interfaces with correct per-interface state including `(mocked)` badges + relative-time last-error column; cap-changed WS event fires sub-second on dispatcher Observe mutations; backend pin timestamp survives page refresh. |

Total: **8 sittings, all complete (2026-05-15).**  Plan closed.

---

## 7. Acceptance — when this plan is "done"

The plan is closed out when:

- Every cell in §3's parity matrix is ground-truthed against a real response body.
- The 429 path emits an ERROR log line with `error.code` + `error.type` + `category` + runId from `aiRequestPool::OnRequestFailed`.
- The `AiError` struct carries `m_ProviderErrorCode` + `m_ProviderErrorType` + `m_Category` + `m_RetryAfterSeconds`, propagated end-to-end to the WebSocket payload including `interface_name`.
- The dashboard renders a deduplicated banner for each recognized actionable category (`BillingExhausted`, `AuthFailure`, `ModelNotFound`, `ServiceOverload`) — generic copy, dismiss-only, mirroring `.no-keys-banner`.
- The dashboard renders a continuously-updated AI Health LED (6th in StatusBar) with click-through popover showing per-interface state including `(mocked)` badge for `is_mock: true` interfaces.
- `is_mock: true` + a real `api_type` produces a working hermetic loopback in all 4 build targets, with `ConfineUnderProjectRoot` + size cap + status/header allowlist + UTF-8 sanitization enforced at the boundary.
- TestInterface (`InterfaceType::Test`) is removed; the JCWF spec no longer lists `"Test"` as an `api_type` variant.
- §19 SanitizeUtf8 verification gap is closed — captured fixtures + tests are the verification artifact.
- `aiZipDemo` + `bookSummary` run end-to-end through `is_mock: true` providers against a Release binary.

Cross-cuts these auto-memory entries on the way through: `feedback_log_failures` (ERROR + runId), `feedback_use_log_macros` (no `std::cout`), `feedback_simdjson_only` (parser extensions), `feedback_cpp_discipline` (no `default:` on closed enums; helper-before-third-site), `feedback_path_containment_scope` (fixture path confinement), `feedback_path_confinement_edition` (Engine strict; Studio relaxed), `feedback_established_safety_patterns` (`SanitizeUtf8`), `feedback_auth_funnel_one_gate` (admin-only mock flag), `feedback_no_legacy` (TestInterface removal, no compat shim).

---

## 8. After this plan

Two follow-ups likely flushed out by the work but not in this plan's scope:

- **Provider-account health probe** — periodic lightweight call to each provider's quota/billing endpoint (where one exists) so j9t knows about billing exhaustion before the next workflow runs.  Significant work, separate dev plan.
- **Workflow auto-pause on hard provider failure** — if every call to a given provider has failed for N minutes, auto-pause workflows that depend on that provider.  Friendly to overnight runs that would otherwise burn retry budget against a known-down provider.  Behavior change, would need a JCWF opt-in.

Both are post-1.0 candidates; the present plan does not block on them.
