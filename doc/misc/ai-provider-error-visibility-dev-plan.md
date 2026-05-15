# AI Provider Error Visibility — Development Plan

**Status:** dev plan, rev 3.  Foundation Sittings 1 + 2 landed (2026-05-14).  Remaining: Foundation 3, A, B, C, D, E (6 sittings).
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

### Per-API status

| Aspect | API1 OpenAI Chat | API2 OpenAI Responses | API3 Anthropic Messages | API4 Gemini | API5 Bedrock |
|---|---|---|---|---|---|
| Error body parsed at all | ✓ | ✓ | ✓ (different shape — int `code`, string `status`, string `message`) | ✓ | Delegates to API4 or Bedrock sub-parsers |
| `error.message` extracted | ✓ | ✓ | ✓ | ✓ | Delegated |
| `error.type` extracted | ✓ | ✓ | ✗ (no `type` field) | ✓ | Delegated |
| `error.code` extracted | ✓ | ✓ | ✓ (as int) | ✗ | Delegated |
| Billing-exhaustion discriminator detected | `insufficient_quota` | `insufficient_quota` | **none** (would need `credit_balance_too_low`) | **none** (would need `RESOURCE_EXHAUSTED` + reason) | **none** (would need `ServiceQuotaExceededException` via `__type`) |
| Throttle discriminator detected | `rate_limit_error` | `rate_limit_error` | **none** (would need `rate_limit_error`) | `rate_limit_error` | **none** (would need `ThrottlingException` via `__type`) |
| Auth discriminator detected | `authentication_error` | `authentication_error` | (via HTTP 401) | `authentication_error` | Delegated |
| `m_Code` / `m_Type` returned via `AiError` | ✗ | ✗ | ✗ | ✗ | ✗ |

### Per-provider notes

- **OpenAI Chat (API1) + OpenAI Responses (API2):** The discriminator is parsed and immediately discarded.  Plumbing-only fix.
- **Azure OpenAI:** Shares API1's parser (per CLAUDE.md adapter list).  Inherits the API1 fix.
- **Anthropic (API3):** Response shape is `{"type": "error", "error": {"type": "<discriminator>", "message": "..."}}` — the `type` field is one level deeper than API1's.  Today's parser only takes the int `code` + string `status` + `message`.  Needs a small extension to grab the nested `error.type`.
- **Gemini (API4):** Response shape is `{"error": {"code": <int>, "status": "<RESOURCE_EXHAUSTED|...>", "message": "...", "details": [...]}}`.  Billing-vs-throttle distinction hides inside `error.details[*].@type` and `error.details[*].reason` (`"RATE_LIMIT_EXCEEDED"` vs `"USER_PROJECT_QUOTA_EXCEEDED"` vs `"BILLING_DISABLED"`).  Current parser doesn't reach these fields.
- **Bedrock (API5):** Errors arrive as `{"__type": "ServiceQuotaExceededException", "message": "..."}` or `{"__type": "ThrottlingException", "message": "..."}`.  API5 delegates to API4 or to `LlamaBedrockReply` / `TitanBedrockReply` — none of which expect the AWS error envelope.  Needs a Bedrock-specific error path that reads `__type` before delegating.

### Known unknowns (research subtasks for workstream E)

- Gemini's exact error-detail field names for v1 vs v1beta endpoints — SDK docs and raw HTTP API drift.
- Anthropic's full error-type catalog beyond `rate_limit_error` / `overloaded_error` / `credit_balance_too_low`.
- Bedrock's `ServiceQuotaExceededException` vs `ThrottlingException` — confirm both exist; confirm whether some models surface a third code for billing (`LimitExceededException`).
- Azure OpenAI: confirm byte-for-byte parity with OpenAI Chat error shape (`aoai-api-simulator` may not exercise this).

### Audit acceptance

Before workstream E starts, every cell in the parity matrix is grounded in a real response body — either pulled from a captured `<prob>.transcript.json` or from the provider's public reference.  No "✓ inferred" cells.

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
- `test/dispatch/fixtures/api1/` through `test/dispatch/fixtures/api5/` — capture (from real provider responses where possible) OR hand-craft fixtures.  Per interface:
  - `golden_success.json` — known-good response.
  - `error_billing.json` — provider's billing-exhaustion variant.
  - `error_throttle.json` — provider's throttle variant.
  - `error_auth.json` — auth failure.
  - `error_overload.json` — provider-side overload variant.
  - `malformed_utf8.json` — byte-level pathology: orphan continuation bytes, surrogate halves, truncated multi-byte sequences, overlong encodings, codepoints > U+10FFFF.
  - `truncated_response.json` — body cut off mid-stream.
- New test drivers `test/dispatch/test_api{1..5}_mock_*.py` — one per interface + per fault category.  Each test configures the provider with the fixture + `is_mock: true`, runs a synthetic ai_call, and asserts (a) `AiError.m_ProviderErrorCode` matches expected, (b) `AiError.m_Category` matches expected, (c) ERROR log line fires with runId.
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
- All per-interface fault tests pass.  Parity matrix at §3 has every cell ground-truthed against captured fixtures.
- TUI stress tests pass (process survives, log stays valid UTF-8, no crashes, terminal recoverable).
- §19 SanitizeUtf8 verification gap is closed and documented — the captured fixtures + tests are the verification artifact.
- All fixtures committed under `test/dispatch/fixtures/api{1..5}/` + `test/dispatch/fixtures/demos/<jcwf-name>/`.
- `aiZipDemo` + `bookSummary` runnable end-to-end via `is_mock: true` against a Release binary, verified by workflow-shape correctness.

Risk: medium.  Fixture capture is research-style; wire shape per provider matters (especially Gemini's `error.details[*]` and Bedrock's `__type` envelope).  Demo-JCWF fixture curation is judgment-heavy — when a single canned response stops covering a fan-out, decide between adding fixtures and marking the JCWF as not-fully-mockable.

---

### Workstream A — Log enrichment (one sitting)

Goal: the final 429 ERROR line carries the parsed `error.code` + `error.type` + `m_Category` + runId, so the run analyzer surfaces "billing vs throttle" without any new event types or UI work.

Touchpoints:
- `application/workflow/aiRequestPool.cpp::OnRequestFailed` — emit the enriched ERROR here, where runId/workflowId/taskName are in scope.  Matches CLAUDE.md convention: "Subsystems without run context return errors via their data types and let the upstream caller — which has the runId in scope — emit the ERROR log."  Line shape:
  ```cpp
  LOG_APP_ERROR("AiRequestPool::OnRequestFailed: HTTP {} (code='{}', type='{}', category={}) "
                "run='{}' workflow='{}' task='{}': {}",
                aiError.m_HttpStatus, aiError.m_ProviderErrorCode, aiError.m_ProviderErrorType,
                CategoryToString(aiError.m_Category), runId, workflowId, taskName, aiError.m_Message);
  ```
- `engine/curlWrapper/curlMultiDispatcher.cpp:1066` — the existing `LOG_CORE_ERROR("HTTP 429 rate limit for query {} …")` line drops to WARN with raw status only.  The enriched ERROR fires from `aiRequestPool::OnRequestFailed`.
- `engine/curlWrapper/curlMultiDispatcher.cpp:962` — the retry WARN line at intermediate 429s gets the same code/type enrichment (still WARN, no runId required at this depth).

Acceptance:
- TestInterface (now `is_mock: true`) emits a 429-`insufficient_quota` body → ERROR log contains `insufficient_quota`, `BillingExhausted`, and the runId.
- Same against a `rate_limit_error` body → different code+category in the log line.
- Run analyzer surfaces both cases as distinct issues against the affected runId.

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

### Workstream C — Reactive UI banner + hazard glyph (one sitting)

Goal: when an AI call fails with a recognized actionable category, the dashboard shows a banner identifying the affected interface.  Workflow rows whose interfaces are degraded get an inline hazard glyph.  All four categories (`BillingExhausted`, `AuthFailure`, `ModelNotFound`, `ServiceOverload`) land in this sitting.

Today the dashboard has zero handlers for `ai-call-failed` WebSocket messages.  Workstream C closes that gap as a side benefit; the banner is the lead feature.

Touchpoints (frontend) — reuse existing patterns per §5:
- `dashboard/ui/src/components/WorkflowsPanel.tsx:105-130` + `dashboard/ui/src/App.css:148-176` — the `.no-keys-banner` component is the precedent.  New `ProviderAlertBanner` lives in the same shape, shifted to red severity (`#2a1010` / `#4a1a1a` / `#ef4444`).  Same placement (top of Workflows panel, persistent).
- WebSocket dispatcher — find the `/ws` subscriber (likely in App.tsx or a hook).  Add handler for `ai-call-failed` messages.  Branch on `category` field, not on raw `provider_error_code`.
- De-dup: one banner per `(interface_name, category)` pair regardless of failure burst count; counter shows "Affected calls: N".
- Dismiss semantics: (a) auto-dismiss on first successful call from the same interface after the issue, OR (b) user-dismiss via X icon (stays dismissed for the rest of the session via in-memory state).  No external action links.
- `dashboard/ui/src/components/WorkflowsPanel.tsx:155-159` (`.hazard-icon`) — extend the existing missing-keys hazard glyph: workflow rows whose ANY ai_call task uses an interface in `BillingExhausted` / `AuthFailure` / `ServiceOverload` get the `⚠` glyph in red (`#ef4444`) instead of amber, with tooltip identifying the category and `interface_name`.

Touchpoints (backend support):
- `/api/providers/health` REST endpoint (introduced by workstream D) provides the data for "Affected calls: N" counter and lets the banner survive a page refresh while the issue is ongoing.

Acceptance:
- E2E per category: MockTransport burst with each recognized category → one banner per `(interface_name, category)` + N inline hazards; auto-dismiss on next success.  Verified for `BillingExhausted`, `AuthFailure`, `ModelNotFound`, `ServiceOverload`.
- Same JCWF run producing dozens of 429s → exactly one banner with the count.
- Page refresh — banner re-appears if the issue is ongoing, doesn't appear if it cleared.

Risk: medium.  Touches three layers (WS schema, React component, dedup state).

### Workstream D — Proactive AIMD cap render (one sitting)

Goal: live view of AIMD throttle state in the dashboard.  Catches degraded-throughput situations before a fail event, including intermittent 429s where the cap drifts but doesn't pin.

Complementary to workstream C: C is reactive (diagnosis-by-failure); D is proactive (diagnostic surface, visible even with no current failure).

Touchpoints (backend):
- `engine/curlWrapper/curlMultiDispatcher.h` — implement `ProviderHealthSnapshot SnapshotHealth() const` per §3.5-C; one snapshot per interface.  Tracks `m_CapPinnedAtFloorSince` for the sustained-pin red rule.
- `application/web/webServer.cpp` — new endpoint `GET /api/providers/health` returning `[{interface, snapshot}, ...]` for all 4 build targets.
- Event: `EventCategoryAi::CapChanged` for live updates via WebSocket; same handler as `ai-call-failed`.  REST endpoint serves the initial snapshot on dashboard mount and on WS reconnect for backfill.

Touchpoints (frontend) — extend StatusBar, do NOT add new components:
- `dashboard/ui/src/components/StatusBar.tsx:24-156` — add a 6th LED to the existing `.led-group` row.  Same component shape as the existing 5 LEDs.  Name: "AI Health".  Color states + label format per §5.
- Popover on click/hover of the new LED — list configured interfaces with cap (e.g. `12/16`), last error timestamp (relative), raw `m_ProviderErrorCode`, category badge, retry countdown (from `m_RetryAfterSeconds`).  Each row keyed by `m_InterfaceName`.  When two rows share the same `InterfaceType` (e.g. two OpenAI Chat interfaces), the cap value appears on both rows with a tooltip footnote noting the shared rate state.  Rows for `is_mock: true` interfaces carry a "(mocked)" badge.

**LED color rules:**

| State | Trigger |
|---|---|
| Green `#22c55e` | All configured interfaces at ceiling, no recent errors |
| Amber `#eab308` | Any interface with `cap < ceiling` for <60s (AIMD throttling transient — normal recovery) |
| Red `#ef4444` | (a) Any interface with `cap == floor` for >60s sustained (`m_CapPinnedAtFloorSince` > 60s ago), OR (b) any interface in `BillingExhausted` / `AuthFailure` / `ServiceOverload` within last 30s |
| Grey `#334155` | No interfaces configured |

Rule (a) is the safety net: even when classification fails (so `m_Category` stays `Unknown`), a cap pinned at floor for >60s renders red — independent path to the same signal.

Acceptance:
- E2E test: run a JCWF that drives the cap up; observe cap rendered correctly.
- E2E test: synthesize a 429 burst via MockTransport; observe cap halve in real time via WS event.
- E2E test: leave cap pinned for 60+ seconds; observe red transition.
- E2E test: page refresh during ongoing issue → REST snapshot restores the correct LED state immediately.

Risk: low-medium.  Mostly new components on existing data.

### Workstream E — Cross-provider error parsing extensions (one sitting)

Goal: API3 (Anthropic), API4 (Gemini), API5 (Bedrock) parse their billing-exhaustion + throttle discriminators with the same fidelity as API1.

Touchpoints:
- `application/json/replyParserAPI3.cpp` — extract `error.type` from the nested shape; map `credit_balance_too_low`, `rate_limit_error`, `overloaded_error`, `permission_error` to the appropriate `ProviderErrorCategory`.
- `application/json/replyParserAPI4.cpp` — extract `error.status` (Gemini's top-level enum: `RESOURCE_EXHAUSTED`, `UNAVAILABLE`, etc.) AND `error.details[*]` (typed sub-errors carrying the actual reason).  Billing-vs-throttle distinction on Gemini lives here.
- `application/json/replyParserAPI5.cpp` — add a Bedrock-specific error path that reads `__type` before delegating to API4/Llama/Titan parsers.  Today on a Bedrock error, the parser delegates to API4 which doesn't know about `__type` and falls through to a generic "unrecognized response shape" parse error.

Acceptance:
- Parity matrix at §3 has every cell ground-truthed against the fixtures captured in Foundation Sitting 3.
- Each provider's billing-exhaustion code surfaces in the workstream-A ERROR log and the workstream-B WebSocket message identically to API1's.

Risk: medium.  Parser code is small but matrix research determines correctness — getting Gemini's `details[*].reason` field name wrong silently breaks billing detection on Gemini.  Pair-reviewed against the captured fixtures from F3.

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
| **3** | Foundation 3 (Per-interface fixture batteries + parser fault tests + TUI safety + curated demo JCWF fixtures) | Fixtures per API (success + 4 error variants + malformed UTF-8 + truncated body); test drivers per interface; TUI stress tests with malformed UTF-8 + real-world ugly content; curated demo fixtures for `aiZipDemo` + `bookSummary` (stretch: jarvisCpp audits).  §19 SanitizeUtf8 verification gap closed. | All per-interface fault tests pass; TUI stress: process survives, log valid UTF-8, no ncurses crashes, terminal recoverable; `aiZipDemo` + `bookSummary` runnable end-to-end via `is_mock: true`. |
| **4** | A (log enrichment) | Enriched ERROR log line with `m_ProviderErrorCode` + `m_ProviderErrorType` + `m_Category` + runId, fired from `aiRequestPool::OnRequestFailed` | MockTransport 429-`insufficient_quota` → ERROR log contains `insufficient_quota` + `BillingExhausted` + runId.  Same with `rate_limit_error` → different code+category. |
| **5** | B (`AiError` plumbing + §3.5 abstractions) | `ProviderErrorCategory` enum, `ParseOpenAiStyleError` helper, `m_RetryAfterSeconds`; all propagated to WebSocket including `interface_name` | WS test client sees `category`, `provider_error_code`, `provider_error_type`, `retry_after_seconds`, `interface_name` on simulated 429s. |
| **6** | E (cross-provider parsing — API3, API4, API5) | All four non-API1 parsers populate `m_Category` for billing/throttle/auth/overload variants | Parity matrix has every cell ground-truthed; fixtures driven through each parser end-to-end. |
| **7** | C (banner + hazard glyph; all four categories) | Banner mirrors `.no-keys-banner` in red severity, generic copy, dismiss-only; `.hazard-icon` red variant on affected workflow rows; WebSocket subscription for `ai-call-failed` (first in the dashboard); all four categories rendered per §5.3 | E2E per category: MockTransport burst → one banner per `(interface_name, category)` + N inline hazards; auto-dismiss on next success.  All four categories verified. |
| **8** | D (`ProviderHealthSnapshot` + AI Health LED; hybrid live updates) | `SnapshotHealth()` on dispatcher; `/api/providers/health` endpoint; 6th LED in `StatusBar.tsx`; per-`m_InterfaceName` popover with "(shared cap)" footnote and "(mocked)" badge; `EventCategoryAi::CapChanged` WS event with REST snapshot on mount | Drive AIMD cap up/down via MockTransport 429 burst; LED color transitions per §4-D rules including the "cap pinned at floor for >60s" red rule. |

Total: **8 sittings.**  Foundation sittings (1-3) are each scope-bounded and do not collapse.

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
