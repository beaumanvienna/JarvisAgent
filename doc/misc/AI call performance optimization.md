# AI Call Performance Optimization — Development Plan

**Status:** dev plan, rev 7 — **refactor complete (2026-04-28), §14 Tier B hermetic tests landed (2026-04-28).** Phases 1–5 verified live; Tier A green; Tier B 8/8 passing × 3-sweep isolation. Two latent bugs surfaced + fixed during Tier B (rate_limit POST parser dropped overrides; `max_retries_*=0` treated as "use default"). Cyber-sec / safety hardening passes have their own dev plans: `doc/misc/cybersec-hardening-dev-plan.md` and `doc/misc/cpp-safety-hardening-dev-plan.md`. The dev plan body below stays as the design reference for the work that landed.

**Verification milestones:**
- 2026-04-26: Phases 1–5 verified live, `jarvisCppDocu` (138 tasks) green on Anthropic Sonnet in 5 min 43 s, AIMD cap 4 → 16, zero 429s.
- 2026-04-27: §17 dashboard live-update fixed and verified at 3× concurrent load on Sonnet (3 workflows × 140 tasks). §12.1 + §12.2 example JCWFs landed. §14 Tier A (strategy parser hermetic test) green. UTF-8 byte-truncation bug found and fixed during the chase.

---

## Session handoff — 2026-04-27 → next session

Read this first. Self-contained brief on where things stand and what's open.

### What landed 2026-04-27 (uncommitted; today's accumulation on top of yesterday's refactor)

- **§17 dashboard live-update bug — root-caused and fixed.** `WorkflowRuntimeManager::Update()` was sampling per-run fingerprints AFTER `DrainAiRequestCompletions()` had already mutated states, so every `WaitingExternal → Succeeded` for an `ai_call` was invisible to the change detector. Fix: capture fingerprints by `runId` BEFORE the drain. Verified at 3-workflow concurrent load: snapshot broadcasts grew 1:1 with completions where they were stuck at 7 forever before. Closed in TODO list §17.
- **UTF-8 byte-truncation bug — found during §17 chase, fixed.** `m_CapturedStdout = capturedStdout.substr(0, 1024)` cut multi-byte UTF-8 sequences in half (e.g., em dash `—` at byte 1023 leaving an orphaned `0xE2`), `PyUnicode_FromString` failed on the mangled string and set a Python exception that leaked across the next `PyObject_Call` and detonated as the cryptic "<method 'get' of 'dict' objects> returned a result with an exception set" inside whichever Python `dict.get` got hit first. Two fixes: new `TruncateUtf8Safe` helper in `application/workflow/workflowTypes.h` applied at three sites (`workflowRuntimeManager.cpp`, `shellTaskExecutor.cpp`, `pythonTaskExecutor.cpp`), and a defensive `PyErr_Clear()` in `pythonEngine.cpp` so a future stale-exception leak surfaces as a logged ERROR with the taskId instead of a downstream-blamed crash.
- **§12.1 + §12.2 example JCWFs landed.** `scripts/buildJarvisCppDocu.py` rewritten as a single 3-mode generator (`--mode {docu,cyber-sec-audit,safety-audit}`) with per-mode STNG/TASK/PROB and auto-bootstrap of `workflows/<id>/global.json` for new modes. Three packed `.jcwf` files in `example/workflows/`, three companion `.md` docs (cyber-sec + safety newly authored, docu rewritten to match the crisp shape). The combined audit baselines live at `doc/combinedCyberSecAudit.md` (729 findings: 54 CRITICAL, 239 HIGH, 279 MEDIUM, 157 LOW) and `doc/combinedSafetyAudit.md` (1243 findings: 13 CRITICAL, 277 HIGH, 483 MEDIUM, 470 LOW).
- **`combineDocumentation.py` parameterized.** Added `outputFileName`, `documentTitle`, `workflowId` kwargs (defaulted to the docu values for byte-identical legacy behaviour). Wired through the JCWF python task `inputs` block as `default` strings so each workflow's combined output gets the right filename and title.
- **Discovered + fixed a `combineDocumentation.py` audit bug.** With three workflows producing 140 `docu.output.md` each, `_auto_find_docs_root` couldn't tell which queue dir to scan and picked the same fallback for all three. Fix: pass `docsDirectory` explicitly via the canvas `inputs.docsDirectory.default` per workflow. Without this, all three combined outputs would silently contain the docu workflow's content.
- **Master-password dialog priority fix.** When `keysStatus` was stale-cached at "ok" after a j9t restart, `AdminLoginDialog` rendered while the keystore was actually sealed; submitting an MCP key produced a misleading "invalid key" error. Fix in `dashboard/ui/src/App.tsx`: derive `keysSealed` from the live `status?.keys_unlocked` flag too (polled every 3 s on the public `/api/status`), and add `&& !keysSealed` to the AdminLoginDialog gate so the live signal suppresses the MCP-login dialog independent of the cached status.
- **§14 Tier A hermetic test landed.** New debug endpoint `POST /api/debug/parse-rate-limit-headers` exposes `IRateLimitStrategy::Parse(...)` + `DeriveQuotaKey()` + `InitialConcurrencyProbe()` for any (interface_type, header_buffer, body, status, model). Five header fixtures in `test/dispatch/fixtures/headers/`. `test_rate_limit_observation_parse.py` covers OpenAI duration syntax (`1m30s`/`6s`), Anthropic ISO 8601 resets, split input/output token quotas, retry-after on 429, the Empty strategy for Gemini/Bedrock/Test, plus quota-key derivation per provider. Green on first run.
- **WebSocket diagnostic counters.** ~12 new fields on `/api/debug/signals` (`websocket_total_broadcasts_enqueued`, `websocket_total_runs_snapshots_enqueued`, `websocket_total_drains`, `websocket_peak_drain_bytes`, `websocket_peak_drain_duration_us`, etc.). Added during the §17 investigation; kept in as a permanent post-mortem layer for any future WS pacing question.
- **Doc sweep.** `README.md` now mentions the rate maximizer alongside HTTP/2 + multithreading. `doc/architecture.md` got a Key Design Decisions row for the client-driven WebSocket drain pattern. `doc/api-endpoints.md` got the new debug endpoint and the WS diagnostic counter table. `JarvisAgent TODO List.md` §17 marked closed; new §18 (cyber-sec hardening) and §19 (C++ safety hardening) added with sources pointing at the fresh combined audit baselines. `doc/jarvisCppCyberSecAuditFindings.md` deleted (superseded by the fresh `doc/combinedCyberSecAudit.md`).
- **All 4 binaries rebuilt** at the start of the §14 work, verified Engine symbol-isolation invariant (`0` Studio symbols in `bin/Release/jarvisAgent-engine`). The §14 Tier A endpoint was added AFTER that build, so currently only `bin/Debug/jarvisAgent-studio` has it — see "Build state" below.

### What's still open

- **§14 Tier B hermetic tests** — TestInterface header / 429-status / delay injection plus an exposed way to inspect `QueryData::m_TimeoutMs` post-`Submit`. Tests: AIMD halve/grow under forced 429s, token-bucket projection deny, force-429 cap halving, `CURLE_OPERATION_TIMEDOUT`, idempotent `Observe()` belt-and-suspenders, size-aware budget formula readback. ~half-day of TestInterface plumbing in C++.
- **TODO list §18 — Cyber-security hardening pass** — baseline at `doc/combinedCyberSecAudit.md`. Recommended approach (per §12.1 of this plan): build 4 reusable utilities (argv-only shell runner, URL allowlist + canonicalize, path-confinement assert, bearer/api-key redactor) first; most CRITICALs collapse into utility calls.
- **TODO list §19 — C++ safety hardening pass** — baseline at `doc/combinedSafetyAudit.md`. Recommended approach: automated `LOG_*_ERROR` audit pass for the 190 logging findings, RAII wrappers for the 145 resource findings, hand-review the 13 CRITICAL + ~50 highest-value HIGH concurrency / lifetime findings.

### Build state

Only `bin/Debug/jarvisAgent-studio` has the §14 Tier A `POST /api/debug/parse-rate-limit-headers` endpoint. The other three binaries (`bin/Release/jarvisAgent-studio`, `bin/Debug/jarvisAgent-engine`, `bin/Release/jarvisAgent-engine`) were built earlier today before that endpoint was added — they're current with all other today's C++ changes (UTF-8 truncation, fingerprint move, WS counters, dialog fix is React-only). Rebuild all 4 once before any cross-edition test or release artifact.

### Gotchas next-session-Claude should know

These survive from yesterday's plan because they're still load-bearing.

- **Don't restart j9t lightly.** The dispatcher state (controller AIMD caps, observation history) lives in-memory; restart loses it and the AIMD has to re-converge on next workload. Cheap for small tests, but if you're running `jarvisCppDocu` again, that's a real cost.
- **The save-handler hand-rolled JSON serialization is not great long-term.** It works (passes the round-trip test) but it's string concatenation, no escaping for special characters in field values, and one of the cyber-sec audit findings (HIGH "regex-free text substitution on config.json — arbitrary field injection via interface names") is exactly this code path. The `rate_limit` block uses `std::to_string` for numbers which is safe, but the URL/model/name fields are still unescaped. Schedule a proper JSON serializer when the §18 hardening pass touches this file.
- **The 5s file-activity watchdog is the only watchdog left in `AiRequestPool`.** It catches "executor wrote queue files but Submit was never called" — pre-dispatch handoff failure only. Curl owns the in-flight timeout (`CURLOPT_TIMEOUT_MS` = size-aware budget). If you ever need to add a runtime-level safety net for ai_call tasks, do it carefully — the prior dual-timeout layer was actively harmful (killed legitimately-slow Sonnet calls).
- **Default `kDefaultMaxRetries429 = 10` should be enough.** If you see "retries exhausted" in real workloads, the controller's predictive gating is failing — investigate the strategy parser before bumping the retry budget.
- **`api.openai.com|gpt-4.1-mini` in the controller map** — note the OpenAI strategy's `DeriveQuotaKey` produces `gpt-4` for `gpt-4.1-mini` because of the "first two segments split by `-`" rule. This works (groups all gpt-4.x models into one bucket which approximately matches OpenAI's actual quota grouping) but if a future model breaks the convention (e.g. `o1-mini`), revisit `RateLimitStrategyOpenAI::DeriveQuotaKey`. The §14 Tier A test now has explicit assertions on the gpt-4o / gpt-4.1 / claude-sonnet / claude-opus families.
- **The combiner's auto-detect can pick the wrong queue dir if multiple workflows share the same per-task output filename.** Always pass `docsDirectory` explicitly via the canvas `inputs.docsDirectory.default` (the script already does this for the three Cpp workflows; new ones must follow suit).

### Recommended next-session order

1. **Decide commit boundaries.** Today's accumulation spans dispatcher infra + UTF-8 + dialog UX + 3-mode generator + audit baselines + hermetic test foundation. JC's call: one big commit or split (refactor / UI / docs / tests).
2. **Rebuild all 4 binaries** (`make config=release` on Studio, then `premake5 gmake --engine && make config=debug && make config=release`, then back to Studio with `premake5 gmake`) so everything has the §14 Tier A endpoint and the `.build-edition` lands on studio for normal dev.
3. **§18 / §19 hardening passes** — separate sessions. §18 starts with the 4 utility classes; §19 starts with the logging sweep + RAII wrappers. Both have fresh baselines in `doc/combined{CyberSec,Safety}Audit.md`.
4. **§14 Tier B hermetic tests** — half day. TestInterface header injection + post-`Submit` `m_TimeoutMs` readback are the unblocking plumbing.

---
**Target:** j9t 1.0
**Goal:** a single, provider-agnostic adaptive rate-limit controller that maxes out the allowed dispatch rate without provoking 429s, plus a size-aware in-flight budget that replaces the dual-timeout layer. Builds on the uncommitted work in `log/git_diff.txt`; replaces the ad-hoc patches with a unified design.

---

## 1. Problem statement

The dispatcher's job is **dispatch as many concurrent AI requests as the provider will accept, and not one more.** Today:

1. **Reactive only.** We don't learn the per-host rate-limit ceiling until a 429 already happened (initial-burst cap is a hard-coded 4). After 429, we react via `Retry-After` or our own reset-time math.
2. **Per-provider parsing is scattered.** `ParseRateLimitHeaders` knows about OpenAI and Anthropic header names. Gemini (API3), Azure OpenAI (API1Azure), Bedrock (API5), and OpenAI Responses (API2) are silently treated as "no rate-limit feedback" — they fall through to initial-burst cap forever.
3. **Two timeout clocks.** `AiRequestPool::m_Deadline` (per-attempt, extends on retry-queue waits) and `WorkflowRuntimeManager::TimeoutWaitingExternalTasks` (300 s wall-clock from `WaitingExternal` entry, no extension) — the second one kills 100+ tasks even when the first one would have succeeded.
4. **Hard-coded knobs.** `kMaxRetries = 50`, `kInitialBurstCap = 4`, `kMaxActivePerHost = 48`, `kBaseRetryMs = 1000` — none configurable. Different Anthropic tiers (1 vs 4) and different providers want different defaults.
5. **No proactive concurrency control.** The throttle gate decides *when to admit a request* once we know the quota is exhausted. There is no controller that decides *what concurrency level the host can sustain right now*. So the system either throttles to zero or runs at `min(active, kMaxActivePerHost)`; nothing in between.
6. **No usage of `usage` from the response body.** Reply parsers extract `AiUsage{input,output,total_tokens}` and discard it from the rate-limit perspective. Provider headers tell us the *remaining quota after this request*; usage tells us *what this request just consumed*. With both, we can predict whether the *next* request will land — without round-tripping a second time.

---

## 2. Per-provider rate-limit feedback (verified vs. needs-verification)

Confirmed from current code + spec docs we ship; *italics* = needs verification before coding (don't trust ChatGPT or memory; read the official docs):

| Provider | Interface | Headers we already parse | Body feedback | Notes |
|---|---|---|---|---|
| OpenAI Chat Completions | API1 | `x-ratelimit-remaining-{requests,tokens}`, `x-ratelimit-reset-{requests,tokens}` (duration: `200ms`/`6s`/`1m30s`) | `usage.{prompt,completion,total}_tokens` | Used today. |
| OpenAI Responses | API2 | *same family — verify identical names* | *same `usage` shape — verify* | Treated identically to API1 today; verify. |
| Anthropic Messages | API4 | `anthropic-ratelimit-{requests,tokens,input-tokens,output-tokens}-{remaining,reset}` (ISO 8601), `retry-after` (seconds) | `usage.{input,output}_tokens` | Used today. Token quota = MIN of input/output remaining; reset = LATEST of the three reset times. |
| Gemini | API3 | *unverified — Google AI typically returns 429 with `Retry-After` but **no** proactive `remaining` headers* | `usageMetadata.{promptTokenCount,candidatesTokenCount,totalTokenCount}` | Currently no proactive feedback wired up. Falls through to initial-burst cap forever. |
| Azure OpenAI | API1Azure | *unverified — Azure may return `x-ratelimit-*` like OpenAI but separate per-deployment TPM/RPM quotas; `retry-after-ms` header is documented* | *similar to OpenAI* | Adapter recently shipped (commit `31847fb`); rate-limit story not exercised. |
| AWS Bedrock | API5 | *unverified — Bedrock returns `ThrottlingException`; **no** `remaining`-style headers documented; backoff is purely reactive* | varies by underlying model | SigV4-signed; `Retry-After` if present, else exponential backoff. |
| OpenAI Responses (alt) | API6 | *same as API2 — verify name* | same | Naming clarification needed. |

**Phase 3 status (verified live 2026-04-26):**
- Anthropic (API4) — `anthropic-ratelimit-{requests,tokens,input-tokens,output-tokens}-{remaining,reset}` parsed end-to-end; `retry-after` plumbed through. Verified live with claude-sonnet-4-6, claude-opus-4-7, claude-haiku-4-5.
- OpenAI Chat (API1) and OpenAI Responses (API2) — `x-ratelimit-{remaining,reset}-{requests,tokens}` parsed. Verified live with gpt-4.1-mini, gpt-5-nano, gpt-4.1.
- Gemini Native (API3) — verified live: ships **no** proactive rate-limit headers; the Empty strategy is the right call. Backoff is purely reactive on 429s.
- Azure OpenAI (API6) and AWS Bedrock (API5) — **out of scope** for live verification (JC's call 2026-04-26: not in active use). Strategy mapping (OpenAI for API6, Empty for API5) stays as best-guess until / unless they come online.

---

## 3. Unified abstraction

### 3.1 Normalized observation

One small POD that every provider strategy fills in:

```cpp
// engine/curlWrapper/rateLimitObservation.h — new
namespace AIAssistant
{
    struct RateLimitObservation
    {
        // Each field is "unknown" when negative / nullopt — we never overwrite
        // a known value with a missing one.
        int64_t m_RemainingRequests = -1;       // requests left in the current window
        int64_t m_RemainingInputTokens = -1;
        int64_t m_RemainingOutputTokens = -1;
        int64_t m_RemainingCombinedTokens = -1; // when provider only reports one bucket

        std::optional<std::chrono::steady_clock::time_point> m_RequestsResetAt;
        std::optional<std::chrono::steady_clock::time_point> m_TokensResetAt;

        // Set when the response was a 429 (or other throttling status) and the
        // provider explicitly told us how long to wait.
        std::optional<std::chrono::milliseconds> m_RetryAfter;

        // Tokens the request just consumed (from response body, not headers).
        // Lets the controller predict whether the *next* request fits without
        // waiting for the next response cycle.
        int64_t m_ConsumedInputTokens = -1;
        int64_t m_ConsumedOutputTokens = -1;

        bool IsEmpty() const; // all fields unknown
    };
}
```

### 3.2 Per-provider strategy

```cpp
// engine/curlWrapper/rateLimitStrategy.h — new
class IRateLimitStrategy
{
public:
    virtual ~IRateLimitStrategy() = default;

    // Parse what we got back from this provider. Headers + body are both available;
    // body is empty on header-only updates. Return an Observation with whatever
    // fields the provider gave us.
    virtual RateLimitObservation Parse(std::string const& headerBuffer,
                                       std::string const& responseBody,
                                       int httpStatus) const = 0;

    // Hard upper bound on initial concurrency for this provider until the
    // first observation lands. Different providers have different "safe to
    // probe" levels (Anthropic Tier 1 = single-digit; Tier 4 = hundreds).
    virtual int InitialConcurrencyProbe() const = 0;

    // Map a model id ("claude-sonnet-4-6", "gpt-4o-mini", "gemini-2.0-pro") to
    // the family that shares one quota bucket on this provider ("sonnet",
    // "gpt-4o", "gemini-2.0"). Anthropic and OpenAI publish per-model-family
    // quotas; the controller is keyed by (host, family) to keep their AIMD
    // signals independent. Strategies that don't care can return "" — the
    // controller treats that as "one bucket per host."
    virtual std::string DeriveQuotaKey(std::string const& model) const = 0;

    static IRateLimitStrategy const& Get(InterfaceType const&);
};
```

Concrete implementations (one file each, `engine/curlWrapper/rateLimitStrategy{API1,API2,API3,API4,API5,API6,Test}.cpp`) own the per-provider header names and the model→family mapping. `Get()` is a switch over `InterfaceType` with `static_assert(NumInterfaceTypes == N, ...)` per the C++ discipline rule.

This replaces the current `ParseRateLimitHeaders` which is hard-wired to OpenAI + Anthropic.

### 3.3 Quota-keyed adaptive controller

The controller lives in `CurlMultiDispatcher` (it's already on the I/O thread, owns the active set, and sees every HTTP request — non-AI traffic too if any ever lands here). The map is keyed by an opaque `QuotaKey` string — concretely `"<host>|<modelFamily>"` — built by `AiRequestPool::Submit` via `strategy.DeriveQuotaKey(model)` and plumbed through `QueryData::m_QuotaKey`. The dispatcher uses the key as a map index without understanding its internal structure; the strategy owns the format.

This keeps Anthropic Sonnet and Anthropic Opus on independent AIMD signals (they share a host but not a quota bucket) without making the dispatcher aware of "model family."

```cpp
// engine/curlWrapper/rateLimitController.h — new
class RateLimitController
{
public:
    // Called from DrainInbox to decide whether to admit one more request.
    // Returns an "admit" decision plus, on deny, the time at which the
    // controller expects to admit again.
    struct Decision { bool m_Admit; std::chrono::steady_clock::time_point m_NextAttemptAt; std::string m_Reason; };

    Decision ShouldAdmit(int currentInflight,
                         int64_t estimatedInputTokensForRequest) const;

    // Called from DrainCompleted with a fresh observation. MUST be idempotent
    // by replacement: a known field overwrites the prior value, an unknown
    // field preserves the prior value, never accumulates. This makes the
    // future split into ParseHeaders() + ParseBody() (for streaming) a
    // mechanical refactor — multiple Observe() calls per request work
    // correctly without double-counting.
    void Observe(RateLimitObservation const& obs, bool was429);

    // Snapshot for /api/debug/signals.
    struct DebugSnapshot { /* current cap, last obs, predicted next-admit time */ };
    DebugSnapshot Snapshot() const;

private:
    // AIMD on the concurrency cap. Cap halves on 429, additively grows on
    // streak of clean completions. Lower-bounded at 1; upper-bounded at the
    // strategy's InitialConcurrencyProbe() × growth-headroom and clamped
    // by the per-interface config field rate_limit.max_concurrency.
    int m_CurrentConcurrencyCap = 1;
    int m_StreakSinceLast429 = 0;

    // Token-bucket mirror: when m_RemainingTokens - in-flight projection ≤ 0,
    // hold dispatch until m_TokensResetAt. Same for requests bucket.
    RateLimitObservation m_LastObservation;
};
```

### 3.4 The whole thing in `CurlMultiDispatcher`

```
AiRequestPool::Submit:
    queryData.m_QuotaKey = host + "|" + strategy.DeriveQuotaKey(model)
    dispatcher->Submit(queryData, ...)

DrainInbox:
    for each pending request:
        key = pending.m_QueryData.m_QuotaKey
        controller = m_Controllers[key]
        Decision d = controller.ShouldAdmit(activePerKey[key], estimateInputTokens(...))
        if not d.m_Admit:
            push back, log throttle reason ONCE per (key, reason) per 5s
            break
        dispatch()

DrainCompleted (per finished request):
    strategy = IRateLimitStrategy::Get(interfaceTypeForKey(key))
    obs = strategy.Parse(headerBuffer, responseBody, httpStatus)
    controller.Observe(obs, was429 = (httpStatus == 429))
    handle 429 → retry queue (existing logic, but the wait time comes from controller, not heuristic)
```

The current `m_HostRateLimits` map collapses into `m_Controllers` keyed by `QuotaKey`. The throttle-reason logging stays.

---

## 4. The adaptive algorithm

Three cooperating mechanisms, each with a clear job:

### 4.1 Token-bucket mirror (correctness)

The provider runs a token bucket. We mirror it:
- On each observation, update `m_RemainingRequests` / `m_RemainingTokens` and their reset times.
- On each `ShouldAdmit`, project the bucket forward: `effectiveRemaining = m_RemainingRequests - (in-flight requests we've already dispatched but haven't yet observed)`. Same for tokens, using estimated input tokens.
- If projection ≤ 0 and we're before the reset, deny until reset.
- If projection > 0, defer to mechanism §4.2.

This is **deterministic and correctness-driven** — we never knowingly over-shoot the provider's stated limit.

### 4.2 AIMD concurrency cap (max throughput)

We don't know how *fast* we can fire requests inside the bucket. We learn:
- Start at `strategy.InitialConcurrencyProbe()` (provider-specific).
- Every K consecutive successes (no 429), `cap += 1` (additive increase).
- On any 429, `cap = max(1, cap / 2)` (multiplicative decrease).
- `m_CurrentConcurrencyCap` is the upper bound on active requests for this host.

This is **classic AIMD**, the same shape as TCP congestion control. It converges close to the largest sustainable concurrency without manual tuning, and it adapts when the provider's actual ceiling moves (account upgrade, off-peak hours, etc.).

### 4.3 Server-directed waits (etiquette)

When the response carries `Retry-After` or a `*-reset` header, we treat that as a **floor on the next admission time** for the host. We never re-dispatch sooner than the server told us to, even if our own model thinks the bucket has room.

Three mechanisms, cleanly factored: §4.1 ensures we don't overshoot the bucket, §4.2 finds the sustainable rate inside the bucket, §4.3 honors the server's explicit asks.

### 4.4 What this replaces

- Hard-coded `kInitialBurstCap = 4` — replaced by per-strategy `InitialConcurrencyProbe()`.
- Hard-coded `kMaxActivePerHost = 48` — replaced by AIMD cap (still capped from above by HTTP/2 stream cap, e.g., 100 streams).
- Hard-coded `kMaxRetries = 50` — bumped down to a sane default (e.g., 10) because retries should be rare once §4.1+§4.2 stabilize. 50 was needed only because the throttle gate was reactive-only.
- Exponential backoff `kBaseRetryMs * 2^retry` — kept as fallback when no `Retry-After` and no reset header.

---

## 5. Estimating input tokens before dispatch

Two consumers need this: the rate-limit controller's `ShouldAdmit` (§4.1) and the size-aware request budget (§6).

`IRateLimitStrategy::EstimateInputTokens(messages)` returns `tokens ≈ chars / 4`. Self-correcting via `Observe()` once real `usage` lands. Per-provider tokenization can swap in later by overriding the virtual.

---

## 6. Single in-flight budget, owned by the dispatcher

One clock, derived from the request's actual size, ticking only while curl is on the wire. Replaces the existing `AiRequestPool::m_Deadline` machinery and the `WorkflowRuntimeManager` `WaitingExternal` timeout for `ai_call`.

### 6.1 The model

- **Budget = `f(input_tokens, output_tokens, per-interface multipliers)`** plus a fixed connection overhead, multiplied by a safety-margin factor, clamped to `[min_seconds, max_seconds]`.
- **The clock runs only while curl is active on the wire.** Inbox queue waits, retry-queue backoffs, and quota holds don't count — those are managed elsewhere (the controller's `ShouldAdmit`, the retry queue's `m_ReadyAt`).
- **One owner: `CurlMultiDispatcher`**, via `QueryData::m_TimeoutMs` → `CURLOPT_TIMEOUT_MS`. curl already only counts in-flight time and resets per attempt because each retry creates a fresh easy handle.

### 6.2 Where the math lives

`AiRequestPool::Submit` computes the budget from the envelope and passes the result through `QueryData::m_TimeoutMs`:

```cpp
estimatedInputTokens  = strategy.EstimateInputTokens(envelope.m_Messages);
estimatedOutputTokens = envelope.m_Settings.m_MaxTokens.value_or(api->m_DefaultOutputTokens);

double seconds = (estimatedInputTokens  / 1000.0) * cfg.per_1k_input_token_seconds
               + (estimatedOutputTokens / 1000.0) * cfg.per_1k_output_token_seconds
               + cfg.fixed_overhead_seconds;
seconds *= cfg.safety_margin_factor;
seconds  = std::clamp(seconds, cfg.min_seconds, cfg.max_seconds);

queryData.m_TimeoutMs = static_cast<long>(seconds * 1000.0);
```

Then `dispatcher->Submit(queryData, ...)`. Done. Curl handles the rest.

### 6.3 What this lets us delete

The whole `AiRequestPool` deadline-bookkeeping layer becomes redundant — curl already does it correctly:

- `AiRequestPool::PendingEntry::{m_HasDeadline, m_Deadline, m_TimeoutMs, m_CurlDispatched}`
- `AiRequestPool::ActivateDeadlineForOutputPath`, `ExtendDeadlineForOutputPath`
- The main-deadline arm in `AiRequestPool::Update` (the **5s file-activity watchdog stays** — different concern: it catches "executor wrote queue files but Submit was never called", which is a *pre-dispatch* handoff bug, not a request-timeout problem)
- `CurlMultiDispatcher::DispatchedCallback` and `RetryQueuedCallback` (yesterday's diff) — no longer needed, no caller arms a deadline anymore
- `WorkflowRuntimeManager::TimeoutWaitingExternalTasks` for `ai_call` tasks — gate the timeout on `taskType != ai_call`, keep the loop for any other `WaitingExternal` consumer (audit pending; likely just `ai_call` uses it)
- Constants `kDefaultTimeoutMs` (5 min) and `kAiCallMinWaitingExternalTimeoutMs` (2 min)

When `CURLOPT_TIMEOUT_MS` fires, curl returns `CURLE_OPERATION_TIMEDOUT`. `DrainCompleted` already routes non-OK results into `QueryResult::Fail(...)`, which `AiRequestPool`'s existing reply callback turns into `OnRequestFailed(expectedOutputPath, ...)`. The `WaitingExternal → Failed` transition flows through paths that already exist. **No new error-handling code.**

### 6.4 Liveness detection — what curl already catches

| Failure mode | Detected today? | Mechanism |
|---|---|---|
| Internet off, new request | Immediate | `CURLE_COULDNT_CONNECT` |
| Internet off mid-request | Eventually | TCP RST → curl error, or `CURLOPT_TIMEOUT_MS` |
| DNS down | Immediate | `CURLE_COULDNT_RESOLVE_HOST` |
| TLS handshake fail | Immediate | `CURLE_SSL_CONNECT_ERROR` |
| Server returns 5xx | Yes | HTTP code → existing transient-retry path |
| Server slow but generating | No (looks normal) | only `CURLOPT_TIMEOUT_MS` |
| Server quietly stuck | No | only `CURLOPT_TIMEOUT_MS` |

Network-level failures all surface as curl errors today with no extra work. The only gap that the size-aware budget can't tighten is "server accepted my POST and is silently doing nothing" — looks identical to "server is generating tokens." Streaming (SSE) is the only proactive signal that distinguishes them, and it's deferred to post-1.0.

Two small liveness improvements ship with this refactor:

- **`CURLOPT_TCP_KEEPALIVE = 1`** in `SetupEasyHandle`. One line. Helps long-idle in-flight connections notice they're dead. Default `CURLOPT_TCP_KEEPIDLE` (60s) is fine.
- **Periodic "still in flight" log** in `CurlMultiDispatcher::IoThreadFunc` — every 30s, walk `m_Active` and emit one `LOG_APP_INFO` line per host with elapsed time, budget, model, runId/workflowId/taskId. Pure observability — answers "did it hang?" from `log/log.txt` without action. Cost: trivial; one timer + one map walk.

`CURLOPT_LOW_SPEED_LIMIT` + `CURLOPT_LOW_SPEED_TIME` are deliberately **not** set — they would false-fire when a model "thinks" silently for 30s before emitting tokens.

### 6.5 Budget defaults per interface

Provider speeds vary by ~10× across model families. Initial defaults (calibrate from observed runs after Phase 4):

| Interface | per_1k_input_seconds | per_1k_output_seconds | fixed_overhead | Notes |
|---|---:|---:|---:|---|
| API1 OpenAI gpt-4o-mini | 0.05 | 0.30 | 3 | fast tier |
| API1 OpenAI gpt-4o | 0.10 | 0.60 | 3 | |
| API1Azure | match underlying OpenAI model | | | |
| API3 Gemini Pro | 0.10 | 0.50 | 3 | |
| API4 Anthropic Haiku | 0.10 | 0.40 | 5 | |
| API4 Anthropic Sonnet | 0.20 | 0.80 | 5 | |
| API4 Anthropic Opus | 0.40 | 1.50 | 5 | slowest tier |
| API5 Bedrock | varies by underlying model | | | conservative defaults |

Common defaults across all interfaces: `safety_margin_factor = 3.0`, `min_seconds = 30`, `max_seconds = 600`. These numbers are estimates; the real ones come from a few real runs once the controller logs `actual_seconds` per request.

---

## 7. Config exposure

Move from compile-time constants to per-interface config. Two sub-blocks under `rate_limit`: concurrency / retry knobs, and the size-aware request budget.

```jsonc
// config.json — engine.api_interfaces[i] (new optional fields, all with defaults)
{
    "api_type": "API4",
    "url": "https://api.anthropic.com/v1/messages",
    "model": "claude-sonnet-4-6",
    "key_name": "anthropic_main",
    // existing fields ↑

    "rate_limit": {
        "initial_concurrency_probe": 4,    // strategy default if omitted
        "max_concurrency": 48,             // hard ceiling regardless of AIMD growth
        "max_retries_429": 10,
        "max_retries_transient": 2,
        "base_retry_ms": 1000,

        "request_budget": {
            "per_1k_input_token_seconds": 0.20,
            "per_1k_output_token_seconds": 0.80,
            "fixed_overhead_seconds": 5,
            "safety_margin_factor": 3.0,
            "min_seconds": 30,
            "max_seconds": 600
        }
    }
}
```

Per-`InterfaceType` defaults shipped in code (table in §6.5). All fields are optional in `config.json`; missing fields fall back to interface defaults.

Tier-1 Anthropic accounts drop `initial_concurrency_probe` to 1 and `max_concurrency` to 4-8. Tier-4 accounts raise both. Cost-conscious operators clamp `max_concurrency` to pace burn rate (per §10 decision 5).

---

## 8. Observability

Today's `/api/debug/signals` exposes `dispatcher_total_*` counters and `m_HostRateLimits` snapshots. With §3.3 in place, add:

- Per-host current AIMD concurrency cap
- Per-host streak-since-last-429 counter
- Per-host predicted next-admit time
- Per-host last-observed `usage.input/output_tokens` (last request's actual consumption)
- Last `RateLimitObservation` parsed (for debug — confirms the strategy is firing)

Plus a small TUI / dashboard widget per active host showing `cap=N, remaining=R, in-flight=K, next admit in Xs`. Right now operators have to grep `[CurlMultiDispatcher: throttling host=...]` log lines to know what's happening.

---

## 9. Phased implementation

Breaking this into bites that can each ship and be regression-tested independently:

### Phase 1 — Strategy abstraction (no behavior change)
- Add `RateLimitObservation`, `IRateLimitStrategy`, concrete strategies for API1–API6+Test.
- Each strategy initially implements *exactly* what `ParseRateLimitHeaders` does today (so API1 = OpenAI headers, API4 = Anthropic headers, others = empty observation).
- `CurlMultiDispatcher` calls `strategy.Parse(...)` instead of inline header parsing.
- `m_HostRateLimits` stays for now.
- Acceptance: `jarvisCppDocu` (Sonnet) behaves bit-for-bit identical to today's run.

### Phase 2 — Controller + token-bucket mirror
- Introduce `HostRateLimitController` per host. AIMD logic is in place but cap is **clamped** to the existing `kMaxActivePerHost = 48` so behavior doesn't regress.
- Add `Observe(...)` calls in `DrainCompleted`.
- Add `ShouldAdmit(...)` calls in `DrainInbox` (replacing the inline gate).
- Acceptance: same `jarvisCppDocu` run, fewer 429s due to predictive (vs reactive) gating.

### Phase 3 — Per-provider proper headers
- Fill out API3 (Gemini), API1Azure (Azure OpenAI), API5 (Bedrock), API6 strategies based on the §2 verification.
- Acceptance: stress test against each provider individually (small-N, e.g. 20 calls), confirm no 429 storms.

### Phase 4 — Config exposure + dual-timeout fix
- Move hard-coded knobs to `config.json` per-interface (§7).
- Resolve §6 (a or b).
- Acceptance: `jarvisCppDocu` against Tier-1 Anthropic with explicit low concurrency in config; against Tier-4 with explicit high concurrency.

### Phase 5 — Observability + cleanup
- /api/debug/signals additions (§8).
- TUI/dashboard widget.
- Delete dead constants (`kInitialBurstCap`, `kMaxActivePerHost`, etc.) — no compat shims (memory: no legacy).
- Cancel `kMaxRetries = 50` back to ~10.

Each phase is one commit on a branch (`refactor/adaptive-rate-limit`?). Phases 1–2 are mechanical, phase 3 needs each provider tested individually, phase 4 ties it together.

---

## 10. Decisions reference

Settled 2026-04-26. Reference list pointing to where each decision is implemented.

| # | Decision | Lives in |
|--:|---|---|
| 1 | Controller in `CurlMultiDispatcher`, keyed by `(host, modelFamily)` via opaque `QueryData::m_QuotaKey` | §3.3 |
| 2 | Shared AIMD cap across parallel workflows; no priority lanes for 1.0 | §4.2 |
| 3 | Retry queue stays in-memory; no persistence | §3.4 |
| 4 | Streaming deferred; `Observe()` idempotent by replacement protects the future refactor | §3.3 |
| 5 | Throughput-first; `max_concurrency` is the only cost lever in 1.0 | §7 |
| 6 | Single in-flight clock owned by dispatcher via `CURLOPT_TIMEOUT_MS`; size-aware budget; `WorkflowRuntimeManager` `WaitingExternal` timeout dropped for `ai_call` | §6 |
| 7 | Input-token estimate = `chars / 4` in `IRateLimitStrategy::EstimateInputTokens` | §5 |
| 8 | Trust curl + size-aware budget for liveness; add `CURLOPT_TCP_KEEPALIVE = 1` and a 30s in-flight log; defer SSE | §6.4 |

---

## 11. What this doc does not change

- libcurl multi + HTTP/2 transport — unchanged.
- `IRequestBuilder` / `ReplyParser` per-provider abstractions — unchanged.
- The envelope (`AiInvocation`) → `AiRequestPool::Submit` direct dispatch — unchanged.
- Disk-first behavior (PROV sidecar, transcripts, output files) — unchanged.
- Workflow DAG / template variables / fan-out — unchanged.

Scope is strictly the **rate-limit + concurrency control + retry** path inside `CurlMultiDispatcher` and the timeout coordination with `AiRequestPool` / `WorkflowRuntimeManager`.

---

## 12. Example-workload deliverables

Two example JCWFs ship alongside this work. They are the workloads that justify the rate-limit refactor (large per-file AI fan-out) and they're the showcase for what j9t makes easy that doing-it-by-hand-with-a-script doesn't. Both go into `example/workflows/` with companion `.md` docs in the established style.

### 12.1 `jarvisCppCyberSecAudit.jcwf` (move + rename)

The currently-uncommitted `workflows/jarvisCppDocu` (per `log/git_diff.txt` 2026-04-25) has already been repurposed into a security review — STNG/TASK/PROB rewritten to senior-app-sec-engineer prompts, label prefix `Sec:`, severity-graded findings (CRITICAL / HIGH / MEDIUM / LOW / NONE).

Promote it to an example:
- Rename source workflow id `jarvisCppDocu` → `jarvisCppCyberSecAudit`
- Move `workflows/jarvisCppDocu/*` → `workflows/jarvisCppCyberSecAudit/*`
- Pack into `example/workflows/jarvisCppCyberSecAudit.jcwf`
- Author `example/workflows/jarvisCppCyberSecAudit.md` describing scope, expected runtime, expected cost (Sonnet 4.6 default; Opus optional via interface-name override), example output excerpt
- Default API interface = Sonnet 4.6
- The original "documentation generator" purpose is gone (per the no-legacy memory: don't keep both)

**Audit dimensions (already in the rewritten prompt):** input validation, injection (SQL/shell/HTTP-header/log), authn/authz bypass, cryptography misuse (weak algos, hardcoded secrets/IVs/keys, RNG misuse, missing cert verification), memory safety (use-after-free, OOB, lifetime/dangling), races and TOCTOU, secrets leakage in logs/errors, insecure deserialization, SSRF, path traversal, uncontrolled allocation / DoS, TLS configuration.

### 12.2 `jarvisCppSafetyAudit.jcwf` (new)

Sibling JCWF reviewing the *non-security* safety properties of C++ code. Same shape (one ai_call per source file, severity-graded findings, sub-workflow combiner that aggregates per-directory reports), distinct STNG/TASK/PROB.

**Audit dimensions:**
- **Concurrency safety** — data races, missing synchronization, lock ordering / deadlock potential, lock-free pattern correctness, condition-variable spurious-wakeup handling, atomic memory ordering misuse
- **Memory safety** — use-after-free, out-of-bounds access, leaks, double-delete, raw new/delete in modern code, dangling references / iterators, std::span / std::string_view lifetime
- **Lifetime / ownership** — references escaping scope, references captured by reference into async work or thread-pool lambdas (the bug `log/AI dispatch refactor.md` 2026-04-23 caught), unclear ownership across boundaries, smart-pointer hygiene (`unique_ptr` vs `shared_ptr` choice, weak_ptr where cycles are possible)
- **Exception safety** — RAII coverage, no-leak-on-throw, basic vs strong vs nothrow guarantees, exception specifications and `noexcept` correctness, exceptions across DLL/ABI boundaries
- **Resource cleanup** — file handles, sockets, threads, mutexes, libcurl handles closed on every path including errors
- **Move semantics** — moved-from object usage, missing `std::move` on local rvalues, `const&` parameters that should be by-value-and-move, `&&` overloads correctness
- **Fail-path logging completeness** — per CLAUDE.md discipline: every error path emits an `LOG_APP_ERROR` (or appropriate `LOG_*_ERROR`) with `runId` / `workflowId` / `taskId` literals where the call site has them in scope. Flag fail paths that return/throw silently or log at WARN/INFO when the path is unrecoverable
- **Log severity appropriateness** — INFO that should be DEBUG (chatty), WARN that should be ERROR (unrecoverable but not surfaced), ERROR that should be WARN (recoverable but alarming), missing context in log messages
- **`switch` discipline** — `default:` arms over closed enums (per CLAUDE.md "no `default:` on enums we own"), missing `case` for new enum variants, fallthrough without `[[fallthrough]]`
- **Const-correctness** — non-const member functions that don't mutate, parameters by `const&` vs by-value, mutable members justified
- **C++20 idiom uplift** — places where `std::optional`, `std::filesystem`, `std::span`, structured bindings, `[[nodiscard]]`, or concepts would improve safety or expressivity (informational, not blocking)
- **TOCTOU / file-system races** — `exists()` followed by `open()`, race-prone tempfile patterns, missing `O_CREAT|O_EXCL`
- **Style adherence** — Allman braces, `m_` prefix on members, 125-column limit, left-aligned pointer (`int* p`). Quick spot-check, low-severity findings only
- **Rust-by-default-equivalents** — flag C++ code in patterns where Rust's borrow checker, `Send`/`Sync` traits, lifetime annotations, `Option<T>`, `Result<T,E>`, exhaustive `match`, or runtime bounds checks would prevent the bug at compile time. Concretely the model should call out:
  - References / pointers / iterators that could outlive their referent (Rust: borrow checker)
  - References captured by reference into thread-pool / async lambdas (the 2026-04-23 `m_ActiveRuns` bug; Rust: `'static` bound on `spawn`)
  - Shared mutable state without an explicit lock — raw access to data visible from multiple threads (Rust: must wrap in `Mutex`/`RwLock`/`Atomic`, enforced by `Send`/`Sync`)
  - Iterator invalidation paths (Rust: borrow checker)
  - `nullptr`-dereference paths where `std::optional<T>` would force a check (Rust: no nulls; `Option<T>` mandatory)
  - Unchecked container indexing (`vec[i]`, `arr[i]`, raw `*ptr`) where bounds aren't proven (Rust: panics on out-of-bounds at runtime by default; `get()` returns `Option`)
  - Implicit narrowing / signed-overflow arithmetic (Rust: `as` casts explicit, signed overflow panics in debug)
  - Uninitialized member variables / locals (Rust: must initialize before use)
  - Functions that swallow errors silently — `try { ... } catch (...) {}` or ignored return codes (Rust: `Result<T,E>` is `[[nodiscard]]` by default)
  - Cross-thread sharing of types that aren't documented thread-safe (Rust: `Send`/`Sync` enforced)
  - `switch` over an enum without `static_assert(Count == N)` or per-CLAUDE.md discipline (Rust: non-exhaustive match is a compile error)

  For each finding: name the C++ pattern, describe the latent risk, suggest the C++ idiom that emulates the Rust guarantee (`std::optional`, `gsl::not_null`, `std::span` with size, scoped lock + private data, `[[nodiscard]]` on functions returning error codes, exhaustive switch via `static_assert`, etc.). Severity is HIGH when the pattern has caused a real bug in this repo, MEDIUM when the pattern is dangerous but not yet bitten, LOW when it's purely stylistic in this context.

Also ships with companion `.md` doc; default = Sonnet 4.6, Opus opt-in.

**Why two JCWFs not one:** security and safety are different review modes that benefit from focused prompts. A unified prompt would dilute findings ("the model couldn't decide whether to flag this as security or safety"). Two passes → cleaner signal, double the cost — acceptable per §10 decision 5.

### 12.3 Sequencing relative to the rate-limit refactor

Both JCWFs are *consumers* of the new dispatcher behavior — the 137-task workload that exposed the rate-limit problems in the first place. They land in `example/workflows/` after Phase 4 of §9 is stable (config-exposed concurrency, fixed dual-timeout). Running them is the integration test.

### 12.4 Reuse the existing generator script

`scripts/buildJarvisCppDocu.py` (modified yesterday for the security-review prompts) walks the source tree, batches files by directory, emits one ai_call task per file with severity-graded output prompts, and produces the JCWF JSON structure programmatically. Authoring 137 ai_call tasks by hand is impractical; the script is the template. For each new JCWF: copy + rename the script (`buildJarvisCppCyberSecAudit.py`, `buildJarvisCppSafetyAudit.py`), swap STNG / TASK / PROB content, swap the workflow id and label prefix. Same generator pattern.

---

## 13. Documentation, REST, and UI updates required

This refactor changes user-facing surface (config schema, debug signals, dashboard forms), REST surface (settings endpoints, debug endpoints), and architectural docs (dispatch pipeline). Per the no-legacy memory, doc + UI edits land in the same commit as the code that makes them true — never as a follow-up.

### 13.1 Documentation

| Doc | What changes | Phase |
|---|---|---|
| `doc/architecture.md` | Rewrite the "AI Dispatch Pipeline" section to introduce `IRateLimitStrategy` + `RateLimitController`. Add a "Rate-limit + concurrency control" subsection covering token-bucket mirror, AIMD, server-directed waits. Update the Key Design Decisions table with the budget-as-CURLOPT_TIMEOUT_MS choice. | 1, 6 |
| `doc/jarvisagent.md` (**user manual — canonical source**) | New "Rate-limit configuration" subsection under `api_interfaces`. Cover: (a) the full `rate_limit` block with every field documented and per-`InterfaceType` defaults from §6.5; (b) "When to tune" — tier-1 vs tier-4 Anthropic, OpenAI per-tier RPM/TPM, Bedrock account quotas; (c) two worked examples ("Tier 1 Anthropic — small budget" with low `max_concurrency`; "Tier 4 Anthropic — production batch" with high values); (d) how to read `/api/debug/signals` to verify the controller is doing what you expect; (e) call out that `max_concurrency` is the cost-pacing lever for budget-conscious users. | 4 |
| `doc/jarvisagent.1` (man page — regenerated from .md) | Re-derive after .md changes land. Confirm whatever generator produces it (likely `pandoc` or hand-edited; check during impl) — match the same content additions. mtimes show .md newer than .1, so .md is the source. | 4 |
| `doc/jarvisagent.html` (HTML manual — regenerated from .md) | Same: re-derive from .md after content lands. | 4 |
| `doc/api-endpoints.md` | Document the controller fields added to `GET /api/debug/signals` (§8): per-quota-key cap, streak, predicted-next-admit, last-observed usage. | 5 |
| `doc/roadmap.md` | Move "streaming (SSE)" out of the rate-limit refactor scope into a dedicated post-1.0 entry. Note the `Observe()` idempotence as the design hook that makes it cheap when it lands. | 5 |
| `engine/curlWrapper/curlWrapper.md` | Add a section on `CurlMultiDispatcher`'s controller integration — strategy lookup, AIMD cap, throttle reasons, retry-queue routing through inbox. Note `CURLOPT_TIMEOUT_MS` and `CURLOPT_TCP_KEEPALIVE` policy. | 1, 6 |
| `application/workflow/README.md` | Drop any reference to `kAiCallMinWaitingExternalTimeoutMs` and the per-task `WaitingExternal` timeout for `ai_call`. Cross-reference `engine/curlWrapper/curlWrapper.md` for the new ownership story. | 6 |
| `test/dispatch/README.md` | Add the new contract tests (§14). Annotate which use `TestInterface` fixture vs live providers. | every phase |
| `JarvisAgent TODO List.md` | Close the §5g rate-limit follow-ups subsumed by this work. Add a new entry for streaming (SSE) post-1.0 with the `Observe()` idempotence reference. Close the dual-timeout follow-up. Keep the dashboard-WebSocket-ping entry already added 2026-04-25. | 5 |
| `example/workflows/jarvisCppCyberSecAudit.md` | Author from scratch alongside the JCWF promotion (§12.1). | 4 |
| `example/workflows/jarvisCppSafetyAudit.md` | Author from scratch alongside the new JCWF (§12.2). | 4 |

**Not touched:** `CLAUDE.md` (unchanged — discipline rules still hold), `doc/JC_Workflow_Specification.md` (no JCWF format changes), `doc/jcwf_generation_guide.md`, `doc/sub-jcwf_generation_guide.md` (workflow-author-facing, dispatch is invisible to them), `doc/cyber security.md` (threat model unaffected), `mcp/README.md`, `integration/README.md`, `doc/combinedDocumentation.md` (auto-generated per memory).

### 13.2 REST API + persistence

The new `rate_limit` config block is optional on disk (defaults applied per-`InterfaceType` in code) but the server-side handlers must round-trip it without dropping fields, the same way yesterday's commit had to extend handlers for `max_context_tokens`.

| Endpoint | Change | Phase |
|---|---|---|
| `POST /api/settings/ai-interfaces` (create) | Accept optional `rate_limit` object; serialize back to disk via `ConfigParser::SerializeEngineConfig` (or equivalent). Unknown sub-fields default. | 4 |
| `PUT /api/settings/ai-interfaces/<name>` (update) | Same. Crucially: when the body omits `rate_limit`, preserve the existing on-disk value rather than resetting to defaults — same gotcha as the API4-downgraded-to-API1 bug from `log/AI dispatch refactor.md` 2026-04-22 | 4 |
| `GET /api/settings/ai-interfaces` (list) | Include `rate_limit` (with effective defaults filled in) so the dashboard UI can render values without separately knowing per-`InterfaceType` defaults | 4 |
| `POST /api/settings/ai-interfaces/save` | Persist `rate_limit` as a nested JSON object alongside other interface fields | 4 |
| `GET /api/debug/signals` | Add controller fields per §8: per-`QuotaKey` cap, streak, predicted-next-admit, last-observed input/output usage | 5 |
| `GET /api/debug/rate-limit-controllers` *(new, debug builds only)* | Full per-`QuotaKey` controller dump for §14.1 hermetic tests and the dashboard widget | 5 |
| `POST /api/debug/parse-rate-limit-headers` *(new, debug builds only)* | Strategy parser test endpoint per §14.4 | 1 |

`ConfigParser::EngineConfig::ApiInterface` gains a `RateLimit` sub-struct + parser code in `engine/json/configParser.{h,cpp}`. Field-by-field defaults applied in `ResolveRateLimitConfig(InterfaceType)` similar to the existing `ResolveMaxContextTokensFromModel`.

### 13.3 Dashboard UI (`dashboard/ui/`)

The dashboard already manages connection-style interfaces via the AI interfaces form. Two surfaces grow:

| Component | Change | Phase |
|---|---|---|
| `dashboard/ui/src/components/AiInterfaceForm.tsx` (or equivalent — confirm path during impl) | Add a collapsible "Rate limit" section with two grouped sub-forms: (1) **Concurrency / retry** — `initial_concurrency_probe`, `max_concurrency`, `max_retries_429`, `max_retries_transient`, `base_retry_ms`; (2) **Request budget** — six fields per §7. Each field shows the per-`InterfaceType` default as placeholder text; user-set value overrides | 4 |
| `dashboard/ui/src/types.ts` | Add `RateLimitConfig` and `RequestBudgetConfig` interfaces matching the C++ shapes | 4 |
| `dashboard/ui/src/components/StatusBar.tsx` (or new `RateLimitPanel.tsx`) | Per-`QuotaKey` widget: `cap=N, in-flight=K, remaining_req=R, remaining_tok=T, next admit in Xs`. Polls `GET /api/debug/rate-limit-controllers` at 1 Hz when a debug build is detected (signals-driven; gated on a flag) | 5 |
| Empty-state messaging | When `rate_limit` is unset on a freshly-created interface, the form shows defaults clearly and tells the user "all fields optional — defaults shipped per provider tier" | 4 |
| Validation | Client-side: `min_seconds < max_seconds`, `safety_margin_factor >= 1.0`, integer fields `>= 1`. Server is the source of truth, but inline validation prevents typos | 4 |

`npm run build` after each phase that touches the dashboard UI; the C++ binary serves the bundled output.

### 13.4 Workflow editor (`workflow-editor/ui/`)

No expected changes. Rate-limit config is per-interface, not per-task. The editor surfaces interface selection (which the operator picks per `ai_call`), not interface configuration. **Confirm with grep** during Phase 4 that no editor component reads `rate_limit` directly; if any do, they're likely just for display and need the new fields piped through.

### 13.5 TUI (`application/log/statusRenderer.*`)

The TUI's 2-line dashboard-style status (rewritten 2026-04-23) currently shows edition + LEDs + last-runs. Adding per-`QuotaKey` lines would crowd the small surface. Phase 5 decision: either (a) leave TUI as-is and rely on `/api/debug/signals` + dashboard widget for rate-limit visibility, or (b) add a single aggregate line ("rate-limit: 3 hosts active, 12 in flight, 0 backed off"). **Recommend (a)** unless an operator-driven need surfaces; the TUI is for at-a-glance health, not detailed telemetry.

---

## 14. Test scope

Three tiers, each with a clear job. j9t doesn't ship a C++ unit test framework; the established pattern is "Python tests via REST against a running j9t" plus debug endpoints that expose internal state for assertion. New tests follow that pattern.

### 14.1 Hermetic tests (no live provider, no network)

Use `InterfaceType::Test` fixtures + `/api/debug/signals` to assert deterministic behavior. Run on every CI build.

| Test | Asserts |
|---|---|
| `test_rate_limit_strategy_dispatch.py` | Each `InterfaceType` strategy is selected by `IRateLimitStrategy::Get()` — bounce a Test envelope per type, verify `dispatcher_total_dispatched` increments and the right strategy logs |
| `test_rate_limit_observation_parse.py` | Feed canned header buffers (one fixture per provider in `test/dispatch/fixtures/headers/`) through a debug endpoint that calls `IRateLimitStrategy::Parse()` and returns the resulting `RateLimitObservation` as JSON. Assert per-field correctness. Covers OpenAI duration syntax (`200ms`/`6s`/`1m30s`), Anthropic ISO 8601, retry-after, and "no headers at all" |
| `test_quota_key_isolation.py` | Two interfaces with same host but different model families — confirm they get independent `RateLimitController` entries in the debug snapshot, independent caps, no AIMD bleed |
| `test_aimd_concurrency_cap.py` | Drive 50 parallel TestInterface envelopes, force half to return synthesized 429s via fixture status code, verify cap halves and recovers per AIMD math by reading the per-key debug snapshot |
| `test_token_bucket_mirror.py` | TestInterface fixtures populate header buffer with `remaining_requests = 0` and a near-future reset; verify `ShouldAdmit` denies until reset elapses; verify retry-after takes precedence as floor |
| `test_observe_idempotent.py` | Call `Observe()` twice for the same request (one with headers-only, one with body-only); assert the controller's state matches a single combined call. Belt-and-suspenders for the future SSE refactor |
| `test_size_aware_budget.py` | Submit envelopes of varying message sizes; assert `QueryData::m_TimeoutMs` matches the formula in §6.2 with the configured per-interface multipliers |
| `test_curlopt_timeout_fires.py` | TestInterface fixture that delays past the budget; verify curl returns `CURLE_OPERATION_TIMEDOUT`, `OnRequestFailed` fires, the workflow task transitions to Failed |
| `test_tcp_keepalive_set.py` | Trivial — debug endpoint reports the keepalive flag set on every easy handle |

Existing tests **already covering relevant ground** (verify they still pass without modification): `test_direct_dispatch_signals.py`, `test_envelope_empty_body_rejected.py`, `test_testinterface_hermetic.py`, `test_relaxed_env_warnings.py`, `test_cross_workflow_parallel.py`.

### 14.2 Live-backed contract tests (one provider per file)

Live calls, small N (≤10), gated behind env-var like the existing `test_api4_anthropic_live.py`. Run when touching the relevant provider strategy.

| Test | Asserts |
|---|---|
| `test_api1_openai_live.py` | (new) end-to-end: 5 calls, headers parsed, `dispatcher_hosts[].remaining_*` populated. Existing `test_api4_anthropic_live.py` covers Anthropic; add OpenAI sibling |
| `test_api3_gemini_live.py` | (new) — same shape; documents whatever proactive feedback Gemini *actually* returns vs the §2 verification checklist |
| `test_api1Azure_live.py` | (new) — same shape against Azure OpenAI; documents `retry-after-ms` behavior |
| `test_api5_bedrock_anthropic_live.py` | (existing) extend to assert that the controller falls back to AIMD-only when Bedrock returns no proactive headers |

### 14.3 Stress tests (the workloads this refactor exists to make work)

| Test | Asserts |
|---|---|
| `test_stress_137_anthropic_sonnet.py` | Re-run the workload that broke yesterday: 137 ai_call tasks against Sonnet. Pass criteria: ≥135/137 succeed, zero `dispatcher_total_retries_exhausted`, `dispatcher_total_throttled` finite (not a runaway), all completions inside Phase 4 budgets |
| `test_stress_jarvisCppCyberSecAudit.py` | Run §12.1 workflow end-to-end. Same pass criteria; this is the integration test for the entire stack |
| `test_stress_jarvisCppSafetyAudit.py` | Run §12.2 workflow end-to-end |

### 14.4 Test data & infrastructure

- `test/dispatch/fixtures/headers/{openai,anthropic,gemini,azure,bedrock}.txt` — canned header buffers per provider for §14.1 strategy parsers
- `test/dispatch/fixtures/responses/{success,429_with_retry_after,5xx_transient}.json` — body fixtures for forced-failure tests
- New debug endpoint `POST /api/debug/parse-rate-limit-headers` (debug builds only) — accepts `{interface_type, header_buffer, body, http_status}`, returns parsed `RateLimitObservation` as JSON. Lets §14.1 tests drive the strategy without a live HTTP round-trip
- New debug endpoint `GET /api/debug/rate-limit-controllers` (debug builds only) — returns the full per-`QuotaKey` controller state (cap, streak, last observation, predicted-next-admit). `dispatcher_hosts` in `/api/debug/signals` becomes a thin alias for backward compatibility during Phase 1, deleted in Phase 5

---

## 15. Implementation notes / gotchas

Things easy to get wrong that we'll save time noticing now.

- **Locking.** `IoThreadFunc` already holds `m_DebugMutex` (recursive) during `DrainCompleted`. The controller's `Observe()` will be called inside that lock. Don't introduce a new lock that nests differently — keep all controller state under `m_DebugMutex` (which is already recursive for exactly this reason).
- **`switch` discipline.** Every switch over `InterfaceType` in new code (`IRateLimitStrategy::Get`, default-multipliers tables) needs `static_assert(static_cast<int>(InterfaceType::NumVariants) == N, "extend this switch")` per CLAUDE.md. No `default:` arms.
- **Fail-path logging.** Strategy parsers and the controller run on the I/O thread without `runId`/`workflowId`/`taskId` in scope. They return errors via their data types (failed parse → empty observation, controller deny → `Decision`); `AiRequestPool::Submit`'s callback is the place that attaches run/workflow/task identifiers and emits the ERROR-level log. Per the `feedback_log_failures` memory, the runId/workflowId/taskId substring is what the dashboard run analyzer filters on.
- **`QuotaKey` plumbing.** Touches `CurlWrapper::QueryData` and every site that constructs one. Today there are several callers (`AiRequestPool::Submit`, assistant flow, jcwfService). All envelope-bearing call sites go through `AiRequestPool::Submit`, so that's the one place that needs to compute and set `m_QuotaKey`. Non-AI traffic (if any ever hits `CurlMultiDispatcher`) leaves it empty → controller treats empty key as "one bucket per host."
- **Reuse `usage` from reply parsers.** `ReplyParser` already extracts `AiUsage{input_tokens, output_tokens}`. `IRateLimitStrategy::Parse` should consume the parsed body via the existing parser, not re-parse JSON. simdjson is the only allowed parser per memory.
- **`WaitingExternal` audit before deletion.** Before removing the runtime timeout for `ai_call`, grep `WaitingExternal` in `application/workflow/` to confirm only `ai_call` tasks transition through it. If anything else does, gate by `taskType != ai_call` instead of deleting wholesale.
- **Hermetic-test budget calculation.** The size-aware budget formula references `api->m_DefaultOutputTokens`. Add this field to `EngineConfig::ApiInterface` with sensible per-`InterfaceType` defaults (e.g., 4096 for chat models). Resolves the missing-`m_MaxTokens` case in `AiRequestPool::Submit` cleanly.
- **Existing uncommitted work as scaffolding.** `log/git_diff.txt` contains the building blocks (Anthropic header parsing, retry-routing through inbox, deadline reset/extend, debug snapshot, dashboard live-progress fingerprint, security-review prompts in `buildJarvisCppDocu.py`). The plan absorbs all of it: Phase 1 reuses the parsing inside the new strategy abstraction; Phase 6 deletes the deadline machinery in favor of `CURLOPT_TIMEOUT_MS`; §12.1 promotes the security-review JCWF. **The diff itself never lands as a standalone commit** — its bits get folded into the Phase 1+ commits as each piece earns its place. Working tree stays as-is; don't `git checkout` back to HEAD. Don't try to refactor inside the diff either; treat it as the starting state and grow cleanly on top.
- **Audit JCWF file list must track the source tree.** The original `jarvisCppDocu` JCWF (and the §12.1/§12.2 audits derived from it) iterates a header→cpp table at `log/jarvisCppDoc.md` — the script `scripts/buildJarvisCppDocu.py` reads it as `TABLE_FILE`. New C++ files added during this refactor (`rateLimitObservation.h`, `rateLimitStrategy.{h,cpp}`, `rateLimitController.{h,cpp}` — and any future additions) must be added to that table or the audits silently skip them. **Each phase that adds files updates this table in the same commit.** Side concern flagged for cleanup in Phase 5: the table lives under `log/` (gitignored / ephemeral per memory `feedback_log_folder_ephemeral`); load-bearing source lists shouldn't live there. Move it to `scripts/` or a proper `doc/` location so it survives `log/` cleanups.
- **Python token estimator parity.** `EstimateInputTokens` lives in C++, but Python tests in `test/dispatch/` may need to predict the budget for assertions. Mirror the formula in a tiny `test/dispatch/_helpers.py` rather than calling out to a debug endpoint for every assertion; keep the C++ as source-of-truth.
- **Don't add a C++ unit test framework.** j9t's testing pattern is Python-via-REST. A unit-test framework is its own decision and out of scope here. `/api/debug/parse-rate-limit-headers` is the established workaround for "needs to test pure C++ logic in isolation."

---

## 16. Decision log

| Date | Decision | Rationale |
|---|---|---|
| 2026-04-26 | doc created | replace adhoc patches with a unified design |
| 2026-04-26 | controller owner = `CurlMultiDispatcher`, key = `(host, modelFamily)` via opaque `QuotaKey` | I/O-thread data lives here; matches Anthropic/OpenAI per-model-family quota model |
| 2026-04-26 | shared cap across parallel workflows, no priority lanes for 1.0 | one provider bucket = one controller is correct; priority lanes added on demand |
| 2026-04-26 | retry queue stays in-memory, no persistence | restart-mid-batch not a supported scenario |
| 2026-04-26 | streaming deferred; `Observe()` idempotent by replacement | one structural property keeps the future SSE refactor mechanical |
| 2026-04-26 | throughput-first; `max_concurrency` is the only cost lever | cost acceptable for value (Sonnet/Opus audit JCWFs) |
| 2026-04-26 | one timeout clock owned by dispatcher via `CURLOPT_TIMEOUT_MS`; size-aware budget | curl already counts only in-flight time and resets per attempt; deletes ~80 lines of `AiRequestPool` deadline machinery |
| 2026-04-26 | input-token estimate = `chars / 4` (option A) | content-driven variance is the dominant term; provider tokenizer dependency cost not justified |
| 2026-04-26 | trust curl + size-aware budget for liveness; add `CURLOPT_TCP_KEEPALIVE` and 30s in-flight log | covers all detectable failures; SSE deferred |
| 2026-04-26 | Phases 1 / 2 / 4 / 5 acceptance criteria signed off | hermetic suite + live Anthropic + dispatcher_controllers debug snapshot all green; ~80 lines of deadline machinery deleted |
| 2026-04-26 | Phase 3 verified live for API1 / API2 / API3 / API4 | live test_output_schema_roundtrip across all four online interfaces; Gemini confirmed to ship no proactive headers (Empty strategy correct) |
| 2026-04-26 | API5 (Bedrock) and API6 (Azure OpenAI) live verification dropped | not in active use; strategy mapping retained as best-guess for if/when they come online |
| 2026-04-26 | Path-mismatch bug fixed in `AiRequestPool::Submit` | `expectedOutputPath` and the binding-lookup key both now use `.output.json` when `output_schema` is set; old code always used `.output.txt`, masked for fast providers, exposed by Gemini's ~6s latency |
| 2026-04-26 | Watchdog-on-throttle bug fixed (Bug 1) | 5 s file-activity watchdog used to fire on tasks legitimately throttled in the controller's inbox waiting for cap availability. `OnCurlDispatchedForOutputPath` renamed `OnSubmitHandoff` and called from `AiRequestPool::Submit` at handoff time; dispatcher's `DispatchedCallback` plumbing deleted entirely (no consumers). Validated at 15-parallel against gpt-4.1-mini (cap=8 → 7 throttled) and full 137-task jarvisCppDocu. |
| 2026-04-26 | Cascade-cancellation shipped (Bug 2) | When a workflow run terminates (failed / cancelled / stopped), `WorkflowRuntimeManager` now calls `AiRequestPool::CancelRequestsForRun(runId)` once (idempotent via `m_CancelCascadeFired`). New `QueryData::m_CancelKey` (= expectedOutputPath), new `CurlMultiDispatcher::CancelByCancelKey` (drained on the I/O thread to safely abort inbox / retry queue / active set with `curl_multi_remove_handle`). `dispatcher_total_cancelled` counter exposed. Validated under real load: 126 in-flight requests aborted in <1 ms when an upstream task failed, halting Anthropic token burn that would otherwise have continued for orphaned generations. |
| 2026-04-26 | Budget defaults bumped to match Sonnet's actual generation rate | `per_1k_output_token_seconds` 0.80 → 5.0; `safety_margin_factor` 3.0 → 4.0; `min_seconds` 30 → 60. Original defaults targeted fast models (gpt-4o-mini class); Sonnet's ~70 tok/s output meant 4K-token responses needed ~60-90 s but only got 30 s. Now Sonnet 4K-output gets ~110 s budget, fast providers still finish well within bounds. Per-`InterfaceType` overrides via `config.json rate_limit.request_budget` remain the path for further tuning. |
