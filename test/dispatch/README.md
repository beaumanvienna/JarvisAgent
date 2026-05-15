# Dispatch contract tests

Tests that exercise the envelope-direct AI dispatch path end-to-end against a
running JarvisAgent instance.  Each script is self-contained, exits 0 on
success and non-zero on failure, and can be run individually or stitched into
a higher-level harness.

## Running

All tests assume a running Studio build on the default port (8443, self-signed
TLS — scripts pass `verify=False`).  Most tests require an MCP admin key:

```
export J9T_TOKEN=mcp_...                 # or pass --token on the CLI
```

### Offline / no live AI

These tests drive the refactored dispatch end-to-end without any network
call outside localhost.  `MockTransport` (Foundation Sitting 2, 2026-05-14)
is used as the hermetic AI backend — the dispatcher routes calls whose
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
Drives heavy multi-byte reply text through the full j9t pipeline
(MockTransport fixture replay → ReplyParser → AiCallTaskExecutor →
LOG macros → ncurses TUI + log/log.txt + dashboard WebSocket) at
~420 `ai_call` task volume.

```
python3 test/dispatch/test_stress_tui_utf8_heavy.py    # 3-way concurrent jarvisCpp, heavy multi-byte UTF-8
```

The test runs all three jarvisCpp JCWFs (Docu / CyberSecAudit /
SafetyAudit) simultaneously with every ai_call routed to a MockTransport
interface backed by `test/dispatch/fixtures/api1/utf8_heavy.json` —
the OpenAI-shape JSON wraps a content string containing CJK ideographs,
emoji, accented Latin, math symbols, RTL Arabic + Hebrew, combining
diacritics. ~420 tasks, ~16 s wall.

Shared helpers in `_stress_tui_helpers.py`. Tests load each jarvisCpp
JCWF from `workflows/<name>/`, override `params.provider` per task to
the mock interface, and inline `cntx_files` entries as
`{path, content}` placeholders so adhoc submission resolves under the
JCWF size cap.

The hermetic / relaxed-env tests create a transient `is_mock: true`
interface via `POST /api/settings/ai-interfaces`, run, and `DELETE` it
on exit.  They never call `/api/settings/ai-interfaces/save`, so
`config.json` on disk stays untouched.

**Malformed-UTF-8 stress is deferred to Foundation Sitting 3.**  The
pre-Sitting-2 `test_stress_tui_utf8_invalid.py` was designed around
TestInterface's short-circuit (raw bytes flowed straight to the log,
bypassing the parser).  Under MockTransport that approach is structurally
broken — simdjson rejects malformed UTF-8 in JSON strings before the
parser sees it.  Sitting 3 rebuilds it per the dev plan §Foundation
Sitting 3 as `malformed_utf8.json` per-API: valid JSON wrapping content
strings with edge-case codepoints (NUL, BOM, RTL/LTR override marks,
unpaired surrogate halves via `\uXXXX` escapes) so the byte-safety code
at the log boundary still gets exercised without violating JSON
well-formedness.

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
(API1 OpenAI Chat, API2 OpenAI Responses, API3 Gemini Native, API4
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
  TUI byte-safety stress test.
* `fixtures/headers/`, `fixtures/responses/` — header / body fixtures
  for the §14 Tier B mock-AI-response endpoint (independent of MockTransport;
  used by the in-process `POST /api/debug/mock-ai-response` helper).
* `fixtures/api{1..5}/{error_*,malformed_utf8,truncated_response}.json` —
  Foundation Sitting 3 (pending) populates the per-API fault-injection
  battery for parser-level negative-path testing.  See dev plan
  `doc/misc/ai-provider-error-visibility-dev-plan.md` §Foundation Sitting 3.
