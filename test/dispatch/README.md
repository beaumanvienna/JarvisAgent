# Dispatch contract tests

Tests that exercise the envelope-direct AI dispatch path end-to-end against a
running JarvisAgent instance.  Each script is self-contained, exits 0 on
success and non-zero on failure, and can be run individually or stitched into
a higher-level harness.

## Running

All tests assume a running Studio build on the default port (8443, self-signed
TLS — scripts pass `verify=False`).  Most tests require an MCP admin key, and
any test that provisions an AI interface also needs the keystore master
password (the create/delete routes are re-auth-gated):

```
export J9T_TOKEN=mcp_...                 # or pass --token on the CLI
export JARVIS_MASTER_PASSWORD=...        # re-auth gate on AI-interface CRUD
```

Interface provisioning goes through `_provisioning.py` (`create_interface` /
`delete_interface`), which merges the master password into the request body;
it defaults to `$JARVIS_MASTER_PASSWORD`, so the helper-module tests pick it up
automatically.  Several offline tests also assert on `/api/debug/signals`,
which is compiled out of Release — run those against a debug build
(`./jarvisagent.sh --debug`).

### Offline / no live AI

These tests drive the dispatcher end-to-end without any network call outside
localhost.  `MockTransport` is the hermetic AI backend — the dispatcher routes
calls whose
interface has `is_mock: true` through MockTransport's fixture-replay path
instead of LiveTransport's real curl.  Crucially MockTransport feeds the
response body through the **real** ReplyParserAPI{1..6}, AIMD controller,
retry queue, and cascade-cancel queue — only the bottom HTTP layer is
replaced.  Fixtures are full API-shape JSON responses (e.g. OpenAI chat
completions), shared at `test/dispatch/fixtures/api1/golden_success.json`.

```
python3 test/dispatch/test_schema_covers_parser.py         # JCWF schema ⊇ parser
python3 test/dispatch/test_direct_dispatch_signals.py      # refactor signals + no-legacy fields
python3 test/dispatch/test_envelope_empty_body_rejected.py # empty prompt -> Failed
python3 test/dispatch/test_mock_dispatch_hermetic.py       # MockTransport byte-exact round-trip + PROV `mocked: true`
python3 test/dispatch/test_relaxed_env_warnings.py         # Phase 1 relaxed env (STNG/TASK/CNTX optional)
python3 test/dispatch/test_chunking_fanout.py              # Phase 6 chunked fan-out + reduce pass
python3 test/dispatch/test_markitdown_cntx.py              # CNTX office-file auto-conversion
python3 test/dispatch/test_cross_workflow_parallel.py      # cross-workflow concurrency (2026-04-22 bug)
python3 test/dispatch/test_rate_limit_observation_parse.py # §14 Tier A: per-provider strategy parser contract
```

### Per-API parser-fault batteries

One thin driver per InterfaceType iterating over a 6-case fixture battery
(success / billing-exhausted / rate-throttled / auth-failed / overloaded /
malformed-UTF-8 / truncated-body).  Each driver provisions a fresh
`is_mock: true` interface per case, submits an adhoc ai_call, and asserts
terminal state + PROV `mocked: true` + `[error]`/`[critical]` log line
carrying the runId.  Total: 36 cases.

```
python3 test/dispatch/test_api1_mock_errors.py             # OpenAI Chat envelope
python3 test/dispatch/test_api2_mock_errors.py             # OpenAI Responses envelope
python3 test/dispatch/test_api3_mock_errors.py             # Google Gemini envelope (error.status + details[*])
python3 test/dispatch/test_api4_mock_errors.py             # Anthropic Messages envelope (nested error.type; 400/429/401/529)
python3 test/dispatch/test_api5_mock_errors.py             # AWS Bedrock envelope (__type)
python3 test/dispatch/test_api6_mock_errors.py             # Azure OpenAI (reuses ReplyParserAPI1; parity-validation)
```

Shared logic (REST + poll + log-scan + cleanup) lives in
`_per_api_fault_helpers.py` so per-API drivers stay metadata-only;
adding API7 is a 30-line copy-rename.  Workstream A (Sitting 5,
2026-05-15) wired the parsed body discriminator + semantic category
+ runId into `aiRequestPool::OnRequestFailed`'s ERROR line; Workstream E
(Sitting 6, 2026-05-15) extended classification to all 6 InterfaceTypes
and tightened api3 + api5 to body discriminators.  Per-API assertions:

* api1 / api2 / api6 use OpenAI types (`insufficient_quota`,
  `rate_limit_error`, `authentication_error`, `server_error`).
* api3 uses Gemini's `error.details[*].reason`
  (`USER_PROJECT_QUOTA_EXCEEDED` vs `RATE_LIMIT_EXCEEDED` —
  distinguishes billing from throttle within `RESOURCE_EXHAUSTED`)
  plus the status enum for the rest (`UNAUTHENTICATED`, `UNAVAILABLE`).
* api4 uses Anthropic's nested `error.type`
  (`credit_balance_too_low`, `rate_limit_error`,
  `authentication_error`, `overloaded_error`).
* api5 uses AWS exception short names from `__type`
  (`ServiceQuotaExceededException`, `ThrottlingException`,
  `AccessDeniedException`, `ModelStreamErrorException`) — parsed by
  `ReplyParserAPI5`'s `BedrockFamily::AwsError` pre-delegation path.

Real Anthropic + Bedrock have non-OpenAI HTTP-status quirks the
fixtures capture: Anthropic `credit_balance_too_low` returns 400 (not
429); Anthropic `overloaded_error` returns 529 (non-standard); Bedrock
`ServiceQuotaExceededException` returns 400; `AccessDeniedException`
returns 403.

### SigV4 signature KAT (pre-1.0 Sitting 6, 2026-05-19)

```
python3 test/dispatch/test_bedrock_sigv4.py                # Bedrock SigV4 Authorization-header KAT
```

Drives the typed-credential threading end-to-end: provisions a fresh
`aws` credential via `POST /api/settings/providers` (using the
AWS-published example access-key + secret), an `is_mock: true` API5
interface, and an adhoc workflow that dispatches one ai_call.
MockTransport now runs the live `IAuthSigner::Get(...).Apply(...)`
pipeline (when `AuthStyle == AwsSigV4` AND `m_AwsCredential != nullptr`),
captures the resulting Authorization header into a process-global FIFO
ring (`MockSignatureCapture`, cap 32, exposed via
`/api/debug/signals::last_mock_signatures` in debug builds), and replays
the fixture body to keep the parser path unchanged.  The test asserts
both the Authorization-header structure (`Credential=AKIDEXAMPLE/.../bedrock/aws4_request`,
`SignedHeaders=host;x-amz-content-sha256;x-amz-date`) and the locked
signature hex `1a6d660...07ae021`.

The locked signature was authored via the **crypto-test bootstrap
pattern** (`feedback_crypto_test_bootstrap_pattern.md`): placeholder
constant → first run prints the captured value → copy-paste lock.
Pairs with `awsSigV4.cpp::RunSelfTest #2` (signing-key derivation
against AWS-published `kSigning` vector at engine startup) as the
independent reference vector required by the pattern's
"never use bootstrap as the SOLE crypto test" constraint.

Fixture inputs are pinned to overlap with the existing self-test #3
+ #4 chain (`AKIDEXAMPLE`, AWS-published example secret, us-east-1,
bedrock, `20240101T120000Z` AmzDate via fixture `.meta.json`'s new
`x_amz_date_override` field); the actual request body differs from
self-test #4 because the JCWF path runs the real
`BedrockRequestBuilder::BuildBedrockAnthropicBody`, so the locked
signature differs from the self-test value.  Both still exercise the
HMAC chain end-to-end.

Requires DEBUG build (`/api/debug/signals` is compiled out of Release).
Run via `./jarvisagent.sh --debug` before invoking the test.

### WebSocket payload verification (Workstream B + E / Sittings 4 + 6, 2026-05-15)

```
python3 test/dispatch/test_ws_ai_call_failed_payload.py    # ai-call-failed WS schema
```

Subscribes to `/ws` (Bearer-auth at the upgrade handshake), fires an
adhoc ai_call against an `is_mock: true` interface, and asserts the
`ai-call-failed` broadcast carries the Workstream-B fields:
`provider_error_code`, `provider_error_type`, `category` (string-
serialized `ProviderErrorCategory`), `retry_after_seconds` (only when
the fixture's `Retry-After` header is set), and `interface_name`.

Eight cases spanning all 4 production InterfaceTypes (Sitting 6
expanded from 2 → 8 once cross-provider classification landed):

| Case | Fixture | Category | Code | Type |
|------|---------|----------|------|------|
| api1 billing | `api1/error_billing.json` | BillingExhausted | `insufficient_quota` | `insufficient_quota` |
| api1 throttle | `api1/error_throttle.json` | ThrottleRateLimit | `rate_limit_exceeded` | `rate_limit_error` |
| api3 billing | `api3/error_billing.json` | BillingExhausted | `USER_PROJECT_QUOTA_EXCEEDED` | `RESOURCE_EXHAUSTED` |
| api3 throttle | `api3/error_throttle.json` | ThrottleRateLimit | `RATE_LIMIT_EXCEEDED` | `RESOURCE_EXHAUSTED` |
| api4 billing | `api4/error_billing.json` | BillingExhausted | *(empty)* | `credit_balance_too_low` |
| api4 overload | `api4/error_overload.json` | ServiceOverload | *(empty)* | `overloaded_error` |
| api5 billing | `api5/error_billing.json` | BillingExhausted | *(empty)* | `ServiceQuotaExceededException` |
| api5 throttle | `api5/error_throttle.json` | ThrottleRateLimit | *(empty)* | `ThrottlingException` |

Anthropic + Bedrock leave `provider_error_code` empty because neither
provider has a separate `code` field — the type field is the sole
discriminator there.

The test sends a `{"type":"ping"}` heartbeat every 300 ms because j9t's
`DrainPendingBroadcasts` only fires inside the WS `onmessage` handler
(see `webServer.cpp:3921`) — passive subscribers receive nothing without
this.  Server broadcasts are wrapped in `{"type":"batch","messages":[…]}`;
the driver peels the envelope before asserting inner-message fields.

The chunking and markitdown tests use MockTransport too — no network.
Markitdown requires the `markitdown` CLI on PATH (pip install markitdown).
An 8 MB sample PDF at `workflows/in.pdf` is the input fixture.

`test_rate_limit_observation_parse.py` drives every per-provider
`IRateLimitStrategy::Parse(...)` through canned header fixtures via
`POST /api/debug/parse-rate-limit-headers` (debug builds only). Asserts
OpenAI duration syntax (`200ms`/`6s`/`1m30s`), Anthropic ISO 8601 resets,
split input/output token quotas, retry-after on 429, the Empty strategy
for Gemini / Bedrock, plus `DeriveQuotaKey` and `InitialConcurrencyProbe`
per provider. Zero AI calls; ~1 s wall.

### §14 Tier B hermetic dispatcher tests

Eight tests exercise the full dispatcher path (rate-limit controller,
AIMD, token-bucket projection, size-aware budget, `CURLOPT_TIMEOUT_MS`)
against a localhost mock endpoint — zero upstream provider calls, zero
network beyond loopback.

```
python3 test/dispatch/test_tcp_keepalive_set.py          # CURLOPT_TCP_KEEPALIVE policy
python3 test/dispatch/test_observe_idempotent.py         # Observe() idempotent merge
python3 test/dispatch/test_rate_limit_strategy_dispatch.py # Get() dispatch table per InterfaceType
python3 test/dispatch/test_size_aware_budget.py          # §6.2 formula → m_TimeoutMs
python3 test/dispatch/test_quota_key_isolation.py        # same host, different families → independent controllers
python3 test/dispatch/test_aimd_concurrency_cap.py       # 429 halves cap, floor at 1
python3 test/dispatch/test_token_bucket_mirror.py        # remaining=0 + future reset → ShouldAdmit denies
python3 test/dispatch/test_curlopt_timeout_fires.py      # delay past budget → CURLE_OPERATION_TIMEDOUT
```

Phase A (first three) drive existing debug endpoints and don't need any
plumbing beyond Tier A.  Phase B (last five) exercise real curl traffic
through `POST /api/debug/mock-ai-response` (debug builds only) — the
mock returns controlled status + headers + body + delay driven by query
params, with fixtures in `test/dispatch/fixtures/{headers,responses}/`.

Tests share `_tier_b_helpers.py`.  Each Phase B test calls
`POST /api/debug/reset-dispatcher-state` at startup to clear
`m_Controllers` / `m_HostRateLimits` / `m_RecentSubmissions` so repeated
runs in the same j9t process stay isolated.

Localhost SSL verification is disabled in DEBUG builds so the dispatcher
can hit the j9t server's own self-signed cert without a CA bundle entry.
Production paths still verify; the bypass is gated on
`#ifdef DEBUG && (host == localhost|127.0.0.1|::1)`.

### TUI ncurses stress tests

Regression-armor for the ncurses TUI renderer's UTF-8 byte handling.
Two complementary tests cover different boundaries:

```
python3 test/dispatch/test_stress_tui_utf8_heavy.py        # 3-way concurrent jarvisCpp, heavy multi-byte UTF-8
python3 test/dispatch/test_tui_stress_malformed_utf8.py    # 98 concurrent MockTransport dispatches, malformed/CSI/control bytes
```

`test_stress_tui_utf8_heavy.py` runs all three jarvisCpp JCWFs (Docu /
CyberSecAudit / SafetyAudit) simultaneously with every ai_call routed
to a MockTransport interface backed by `fixtures/api1/utf8_heavy.json`
— the OpenAI-shape JSON wraps a content string containing CJK ideographs,
emoji, accented Latin, math symbols, RTL Arabic + Hebrew, combining
diacritics. ~420 tasks, ~16 s wall.

`test_tui_stress_malformed_utf8.py` drives 14 distinct stress fixtures × 7 burst
(≈98 concurrent dispatches)
spanning all 6 InterfaceTypes' `malformed_utf8.json` + `truncated_response.json`,
plus `api1/ugly_csi_escapes.json` (real ESC / BEL / BS / CAN / SUB / OSC
control bytes via JSON `\uXXXX` escapes) and `api1/ugly_real_world.json`
(n8n-style verbose JSON, Polarion XML with BOM + mixed line endings,
RTL/LTR overrides, homoglyph baits, format-string baits).  Asserts:
j9t alive, `log/log.txt` valid UTF-8 end-to-end, no unexpected
`[critical]` lines.  This is the artifact that closes the §19
SanitizeUtf8 verification gap from the safety-hardening arc.

Shared helpers for the heavy stress in `_stress_tui_helpers.py`; the
malformed stress is self-contained.  Both load fixtures via REST-
provisioned `is_mock: true` interfaces and `DELETE` them on exit, so
`config.json` on disk stays untouched.

### Live AI (`--with-ai` style — costs real tokens)

```
python3 test/dispatch/test_api4_anthropic_live.py            # Anthropic end-to-end
python3 test/dispatch/test_output_schema_roundtrip.py        # Phase 3 schema-enforced output
```

The Anthropic live test uses the `api.anthropic.com/claude-opus-4-7/API4`
interface configured in `config.json`.  It submits an adhoc ai_call and
asserts a well-formed reply lands at `<prob>.output.txt` inside the run
folder.

The schema-roundtrip test defaults to `api.openai.com/gpt-4.1-mini/API1`,
sends a prompt with `output_schema`, and asserts the produced
`<prob>.output.json` is valid JSON matching the declared schema, plus that
`ai_structured_submissions` incremented and `ai_schema_validation_retries`
stayed within the `output_retries` budget.

The schema-roundtrip test accepts `--interface <name>` and is the canonical
per-provider liveness check across all four online interface families
(API1 OpenAI Chat, API2 OpenAI Responses, API3 Google Gemini, API4
Anthropic Messages). Run once per interface name from `config.json` to
verify provider plumbing — it exercises the full envelope build +
rate-limit strategy parser + AIMD controller path. Verifying the
controller landed correctly: after the test, `GET /api/debug/signals`
should show a new `dispatcher_controllers[]` entry keyed
`<host>|<modelFamily>` with `current_concurrency_cap` and
`streak_since_last_429` reflecting the run.

Live tests against API5 (AWS Bedrock) and API6 (Azure OpenAI) exist in
`test_api5_bedrock_anthropic_live.py` and `test_api6_live.py` but are
out of scope for routine verification — neither provider is in active
use as of 2026-04-26. The strategy mapping (Empty for API5, OpenAI for
API6) stays as a best-guess until they come online.

Note on InterfaceType mapping (authoritative per the code):

| InterfaceType | Provider | Parser |
|---|---|---|
| API1 | OpenAI Chat Completions | `ReplyParserAPI1` |
| API2 | OpenAI Responses | `ReplyParserAPI2` |
| API3 | Google Gemini native | `ReplyParserAPI3` |
| API4 | Anthropic Messages | `ReplyParserAPI4` |
| API5 | AWS Bedrock | `ReplyParserAPI5` (delegates to API4 / Llama / Titan sub-parser) |
| API6 | Azure OpenAI | `ReplyParserAPI1` (Azure forwards OpenAI-compatible bodies; see `replyParser.cpp:88`) |

## Fixtures

* `fixtures/api1/golden_success.json` — canned OpenAI chat completion
  response for the hermetic / relaxed-env / cross-workflow / chunking
  / markitdown tests.  MockTransport returns this body as-is; ReplyParserAPI1
  parses it and extracts `choices[0].message.content` to write
  `<prob>.output.txt`.  Tests compare against the `content` field
  (after stripping FileWriter's `# Model: …\n` header line).
* `fixtures/api1/utf8_heavy.json` — same OpenAI shape with a content
  string of heavy multi-byte text (CJK ideographs, emoji, accented Latin,
  math symbols, RTL Arabic + Hebrew, combining diacritics).  Drives the
  jarvisCpp-driven TUI byte-safety stress test.
* `fixtures/api{1..6}/{golden_success,error_billing,error_throttle,error_auth,error_overload,malformed_utf8,truncated_response}.json`
  — per-API parser-fault battery.  Each error fixture
  pairs with a `.meta.json` sidecar setting `http_status` (and `Retry-After`
  on throttle / overload).  Body shapes match each provider's real response
  envelope: api1+api2+api6 use OpenAI shape; api3 uses Gemini envelope
  (`error.code` int + `error.status` enum + `error.details[*]`); api4 uses
  Anthropic nested-error shape (`{"type":"error","error":{"type":..,"message":..}}`);
  api5 uses AWS `__type` envelope on errors with Anthropic-on-Bedrock
  shape on success.
* `fixtures/api1/ugly_csi_escapes.json`, `fixtures/api1/ugly_real_world.json`
  — TUI-renderer pathology samples (real ESC / BEL / BS / CAN / SUB / OSC
  control bytes via JSON `\uXXXX` escapes; n8n-style verbose JSON,
  Polarion XML with BOM + mixed line endings, RTL/LTR overrides,
  homoglyph + format-string baits).  Drive `test_tui_stress_malformed_utf8`.
* `fixtures/demos/aiZipDemo/golden_success.json`,
  `fixtures/demos/bookSummaryPipeline/golden_success.json`,
  `fixtures/demos/README.md` — curated canned responses for running the
  mockable demo JCWFs end-to-end via `is_mock: true`.  README documents
  the mockable-vs-not-mockable list (JCWFs whose AI reply feeds into
  `g++` / `python -c` aren't mockable because the canned reply can't
  satisfy compile / eval).
* `fixtures/headers/`, `fixtures/responses/` — header / body fixtures
  for the §14 Tier B mock-AI-response endpoint (independent of MockTransport;
  used by the in-process `POST /api/debug/mock-ai-response` helper).
