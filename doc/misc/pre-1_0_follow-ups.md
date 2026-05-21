# Pre-1.0 follow-ups dev plan

Closeout plan for the three tail-end groups tracked in `todo.md` before the alpha (2026-04-05):

- **§5i follow-ups** (Studio/Engine split tail) — 3 items, 1 already done
- **Cloud integration tail** — 1 item, scope reduced after verification
- **Loose follow-ups** — 14 items (todo.md said 11; recount confirmed 14)
- **KeyManager hardening tail** (carry-over from prior hand-off, never propagated into `todo.md`) — 4 items

Total entering this plan: **22 items**. Closed at intake: **1** (§5i.3 — already fixed in code, todo.md was stale). Net actionable: **21 items**, organised into **14 sittings** ordered safety-first → cleanup → verification → tooling → post-1.0 tail.  **Sittings 1+2 closed 2026-05-18**; **Sittings 3+4 closed 2026-05-18**; **Sitting 5 closed 2026-05-19**; **Sitting 6 closed 2026-05-19**; **Sitting 7a closed 2026-05-19** (with 7b + 7c carved out as follow-ups for the connector + parser sweeps) — see `doc/misc/hand-off.md` for the migration records.  **9 sittings remain** as of 2026-05-19 (Sittings 7b + 7c + 8 + 9 + 10 + 11 + 12 + 13 + 14).

Predecessor plans (context, not dependencies):
- `cybersec-hardening-dev-plan.md` — §18 four-domain hardening (S1–S4, complete)
- `cpp-safety-hardening-dev-plan.md` — §19 Rust-emulating C++ defaults (complete)
- `cloud-integration-dev-plan.md` — Phases 0–12 (complete; one tail item below)
- `engine-studio-capability-review.md` — §5i Studio/Engine split (main work complete; tail below)
- `AI dispatch refactor.md` — §5g AI dispatch refactor (main work complete; post-1.0 slices)

---

## Design decisions (settled 2026-05-17)

| Q | Item | Decision |
|---|------|----------|
| Q1 | §5i.1 routing | Move `TestAiInterface` into `aiRequestPool` (both editions get the route). Drop the `#ifdef J9T_STUDIO` in `webServer.cpp:5447`. |
| Q2 | EventQueue overflow | Hard cap at **1000** unprocessed events. On hit: `LOG_CORE_ERROR` + best-effort emergency shutdown from producer thread (main loop is wedged; `std::exit(EXIT_FAILURE)` after log flush). Not lossy buffering. |
| Q3 | SigV4 shim | Clean design — thread typed `AwsCredential` reference through `QueryData` to the signer, drop `m_Params` reinjection. Extend MockTransport to capture+verify signed Authorization headers. Add Bedrock dispatch fixture. First real customer hits a tested path. |
| Q4 | API-shape sweep | All 22 sites + 50–80 caller-side updates in one sitting. Return `std::expected<T, Error>` with a typed `Error` enum/struct (not `std::string`). |

---

## Closed at intake (no work)

- **§5i.3 Bootstrap admin user collides with role name** — `application/web/mcpKeyManager.cpp:402-403` already sets `m_User = "boss"`, `m_Role = "admin"`. StatusBar renders `boss / admin`, no collision. todo.md text was stale.

---

## Sittings (safety → cleanup → verification → tooling)

### Sitting 1 — Writer atomicity sweep — **closed 2026-05-18**

Shared `EngineCore::AtomicWriteFile` helper landed in `engine/auxiliary/file.{h,cpp}`; ~30 atomic-write site touches across 21 files; 7 streaming/recoverable sites across 4 files got the exception-safety pattern.  Scope went past the ~15-site plan estimate once cloud-connector summary writers and assistant-store writers were folded in.  See `doc/misc/hand-off.md` 2026-05-18 entry for the full migration table.

---

### Sitting 2 — `EventQueue` fail-fast cap — **closed 2026-05-18**

`ProducerId` enum (7 variants + `NumVariants`) and `EventQueue::kMaxUnprocessedEvents = 1000` landed.  Tagged `PushEvent(EventPtr, ProducerId)` is the only signature — all 15 producer call sites migrated.  Cap-hit path: snapshot under lock → release → `LOG_CORE_ERROR` with depth + per-producer breakdown + deliberate-trade message → flush both spdlog sinks → `std::exit(EXIT_FAILURE)`.  See `doc/misc/hand-off.md` 2026-05-18 entry "Sitting 2 — EventQueue fail-fast cap" section.  Acceptance fixture (synthetic stress test) deferred to Sitting 11 (umbrella verification sitting).

---

### Sitting 3 — Restore broken `HandleWorkflowVersionRestorePost` — **closed 2026-05-18**

**Problem.** `webServer.cpp:2206-2294` reads `.jcwf` version file as raw bytes via `std::ifstream` (line 2239) and passes those bytes to `WorkflowRegistry::SaveOrUpdateWorkflowFromJson` (line 2281), which expects plain JSON. Since `.jcwf` is always a zip container (per `feedback_no_legacy_jcwf`), restore fails immediately with `restore_failed: UNCLOSED_STRING`. Auth check, path validation, TOCTOU-safe read, and best-effort backup all run correctly — only the final write step is wrong.

**Fix.** `workflowRegistry.h` exposes exactly one public write-path (`SaveOrUpdateWorkflowFromJson`, line 95-97); zip handling is hidden in the private `LoadContainer` (line 116). Two options:
- (a) Add a new public method `UpsertJcwfFromZipBytes(string const& id, std::string const& zipBytes)` that wraps the existing container ingestion path. Cleaner public API.
- (b) Have `SaveOrUpdateWorkflowFromJson` sniff for the zip magic bytes (`PK\x03\x04`) and dispatch internally. Less new API surface.

Decision deferred to implementation time; (a) is the safer default unless registry already has a sniff/dispatch in another reader.

**Acceptance.** Live test: edit `exampleMakefile4` twice (generating 2 versions), call `POST /api/workflows/exampleMakefile4/versions/<ts>/restore`, verify the restored workflow loads, parses, and runs end-to-end. Verify the failing-before fixture still fails-closed (not the no-op success-on-corrupt-input shape).

**Effort.** Small (~half day).

---

### Sitting 4 — Studio/Engine §5i tail (handler move + WS extraction) — **closed 2026-05-18**

Two §5i items, paired because both touch `webServer.cpp` and both reduce `#ifdef J9T_STUDIO` count.

**§5i.1 — `TestAiInterface` → `aiRequestPool` (Engine parity).** Extract `TestAiInterface` from `aiJcwfService.cpp:1172-1202` into a new `AiRequestPool::TestInterface(InterfaceIndex, ...)` method. `webServer.cpp:5416-5456` drops its `#ifdef` block and calls the pool. `aiJcwfService::TestAiInterface` becomes a thin caller (Studio retains it for assistant-side test paths). Engine admins gain the `/api/ai/interfaces/<n>/test` route for operational verification of provider config.

**§5i.2 — AI-WebSocket dispatch extraction.** `webServer.cpp:3779-3959` has a 181-line `#ifdef J9T_STUDIO` block in the `/ws` `.onmessage` lambda dispatching `ai-explain-jcwf`, `ai-generate-jcwf`, `ai-write-scripts`, `ai-fix-failed-script`, `chat`. Extract into `WebServer::HandleAssistantWebSocketMessage` and move to `webServer_studio.cpp`. Current `#ifdef J9T_STUDIO` count in `webServer.cpp` is 8 (todo.md said 10); post-extraction target is ~5.

**Acceptance.** Studio build runs all assistant tests (`test_assistant.py --with-ai --auto-approve`); Engine build runs Test button against live OpenAI/Anthropic interface and reports success. `grep -c "ifdef J9T_STUDIO" webServer.cpp` drops by ≥3.

**Effort.** Medium (~1 day for both, paired).

---

### Sitting 5 — Editor MCP login parity (AdminLoginDialog lift to shared-ui) — **closed 2026-05-19**

`AdminLoginDialog` + `MasterPasswordDialog` (dashboard's richer version with bootstrap + admin-key issuance) + auth helpers (`authFetch`/`whoami`/`serverLogout` + `j9t-auth-required` event) lifted to `shared-ui/{components,api}/`.  Both dashboard and editor import from `@shared/...` now; in-app duplicates deleted per `feedback_no_legacy`.  Editor App.tsx gained mount-time whoami probe, WS-reconnect re-probe, j9t-auth-required listener, AdminLoginDialog mount, and a "Sign out" button in the top nav.  Shared `KeysUnlockResponse` extended with `admin_key`/`bootstrapped`/`mcp_keys_loaded` (matched dashboard shape); the duplicate `KeysStatusResponse`/`KeysUnlockResponse` in `dashboard/ui/src/types.ts` deleted.  See `doc/misc/hand-off.md` 2026-05-19 entry.

---

### Sitting 6 — SigV4 clean design + MockTransport capture + Bedrock fixture — **closed 2026-05-19**

Typed-credential pipeline landed end-to-end.  `QueryData::m_AwsCredential` (shared_ptr to a deep-copy snapshot taken under KeyManager's lock) replaces the `m_Params` stringy reinjection in `aiRequestPool::ResolveProviderParams`.  `awsSigV4.cpp::Apply()` reads from the typed credential and fail-closes if null on the SigV4 path.  MockTransport invokes the signer for SigV4-when-credential-populated and captures the resulting Authorization header into a process-global ring buffer (cap 32); `/api/debug/signals` exposes `last_mock_signatures` (debug-build only).  `QueryData::m_AmzDateOverride` threads an optional fixture-meta-driven timestamp into the signer so the KAT signature is byte-deterministic.  New `test/dispatch/test_bedrock_sigv4.py` follows the bootstrap-then-lock crypto-KAT pattern; locked signature is `1a6d6607ae8458641685888fa012825e591fb38ca4db178eab28d9a9b07ae021`.  All 4 binary configs (Studio Debug+Release, Engine Debug+Release) build clean; all 6 per-API mock-fault suites stayed green (6/6 each).  See `doc/misc/hand-off.md` 2026-05-19 Sitting 6 entry for the migration record.

---

### Sitting 7a — C++23 toolchain bringup + typed error scaffolds (RegistryError pilot) — **closed 2026-05-19**

`premake5.lua` bumped to `cppdialect "C++23"`; new `--clang` opt-in route through libc++ for local dev on clang ≤18 (libstdc++ `<expected>` requires `__cpp_concepts >= 202002L` but clang 18 reports 201907L — fixed in clang 19+; libc++ has its own `<expected>` without the guard).  `linux-workflow.yml::package-rpm` swapped to `gcc-toolset-13` + `scl enable` wrap (Rocky 9's system gcc 11.5 predates libstdc++'s `<expected>`).  All other CI targets (Linux/macOS/Windows/Arch/Flatpak/Docker amd64+arm64) ship `<expected>` natively at their current default compiler versions — verified empirically via Docker probes + GitHub runner-images repo (see `doc/misc/hand-off.md` 2026-05-19 Sitting 7a entry for the full matrix).

Three subsystem-scoped typed error scaffolds landed under their owning directories: `application/cloud/connectorError.{h,cpp}`, `application/workflow/parserError.{h,cpp}`, `application/workflow/registryError.{h,cpp}`.  Each has `enum class XxxErrorCode { ... UnknownError }` (no `default:` arm — `-Wswitch` catches missing variants), `struct XxxError { Code; std::string m_Details; static Make(...) }`, and `Describe(Code) -> std::string_view`.

Pilot conversion: `WorkflowRegistry::RemoveWorkflow` (the smallest surface — 1 method, 1 caller in `webServer_studio.cpp`) flipped from `bool + std::string& errorMessage` to `[[nodiscard]] std::expected<void, RegistryError>` with `NotFound` / `PathRefused` / `IoError` codes.  Caller logs `Describe(err.m_Code)` + `err.m_Details` at WARN — typed category + human message.

All 4 binary configs build clean.  Regression tests green: `test_mock_dispatch_hermetic`, `test_bedrock_sigv4` (SigV4 KAT signature unchanged), `test_api5_mock_errors` 6/6.

### Sitting 7b — Connector subsystem sweep (deferred from 7)

**Scope.** Convert `ICloudConnector::TestConnection` base virtual + 13 concrete overrides + `ConnectorHttp::ValidatePublicHttpEndpoint` + `PostgresConnector::ValidatePostgresParams` to `std::expected<void, ConnectorError>`.  ~190 return sites across the 13 connector `.cpp` files + the single caller in `webServer.cpp` + ~17 intra-connector callers of ValidatePublicHttpEndpoint.

**Per-site work.**  Each `errorMessage = "X"; return false;` becomes `return std::unexpected(ConnectorError::Make(Code::Y, "X"));`.  Code selection per site requires reading context (InvalidConfig / CredentialMissing / CredentialInvalid / InvalidEndpoint / NetworkError / AuthFailure / HttpError / OAuthError / UnknownError — see `application/cloud/connectorError.h`).

**Acceptance.** Build all 5 targets clean; the single REST caller in `webServer.cpp::HandleTestConnection` records typed `ConnectorErrorCode` on the circuit breaker (currently records only the boolean) and echoes `Describe(code)` + `m_Details` to the JSON response.

**Effort.** Medium-large (~half-day to a day). Mechanical sweep; the typing-per-site is the careful part.

### Sitting 7c — Workflow JSON parser sweep (deferred from 7)

**Scope.** Convert the ~15 chained parser methods in `workflowJsonParser.{h,cpp}` + `workflowJsonParserDetails.{h,cpp}` (`ParseTask`, `ParseTaskInputs`, `ParseTaskOutputs`, `ParseTaskQueueBinding`, `ParseTaskEnvironment`, `ParseFilter`, `ParseFilterSource`, `ParseFilters`, `ParseTrigger`, `ParseTriggers`, `ParseDataflow`, `ParseSingleDataflow`, `ParseRetries`, `ParseDefaults`, `ParseControlNodes`, plus `RequireObject` / `RequireArray` shape helpers) to `std::expected<void, ParserError>`.

**Why all-or-nothing.** Inter-method calling: `ParseTasks` calls `ParseTask` calls `ParseTaskInputs` etc.  Converting only some leaves awkward mixed-signature internal calls.  Single subsystem sweep.

**Acceptance.** Build clean; JCWF parse rejections emit typed `ParserErrorCode` + the JSON-path-bearing `m_Details`; the existing 36-case fault batteries stay green (parsers don't surface in the parser-fault suite directly, but JCWF-shape regressions surface as the validator can't load a workflow).

**Effort.** Medium (~half-day). Bulk is the 15-method sweep + ~20 intra-file caller updates.

---

### Sitting 8 — SecureString-only path through HTTP layer

**Problem.** 9 cloud-connector sites materialise `api->m_ApiKey.Get()` into plain `std::string` to build `Authorization: Bearer ...` headers (jira, github, redmine, polarion, googleSheets, slack, azureBlob, plus polarionClient + s3 with view-based variant). `std::string` is fine for correctness (lifetime bounded by HTTP call) but on a compromised process the secret can be recovered from heap residue after the call returns. Defense in depth, not a current vulnerability.

**Fix.** Thread `SecureString const&` (or non-copying view) through the HTTP-build path:
1. `IAuthSigner` / `IRequestBuilder` take `SecureString const&` for bearer-token / API-key paths (not `std::string`).
2. Final materialisation into curl `slist` happens at the last moment in a wiped local buffer (or directly into a curl-owned buffer if libcurl exposes the right API; otherwise a `SecureString`-backed scratch buffer).
3. Cloud connectors stop calling `.Get()` and pass `SecureString const&` through to the signer.

Cross-cuts the 9 sites + `engine/curlWrapper/authSigner.{h,cpp}`.

**Acceptance.** Heap-scan test: dispatch a known-token bearer request, allow it to complete, then scan the process heap for the token's byte pattern → no match. Build all 5 targets clean.

**Effort.** Medium (~1 day).

---

### Sitting 9 — `SanitizeUserSlug` collision fix + migration

**Problem.** `adhocWorkflowManager.cpp:104-117` strips disallowed chars to `_`: `"bob+admin@example.com"` and `"bob_admin@example.com"` both → `"bob_admin@example.com"`. Adhoc layout `_adhoc/<user_slug>/<run>/` lets one user enumerate / interfere with another's adhoc artefacts via the shared parent directory.

**Fix.** Append `_<8 hex chars of SHA-256(original_user)>` to the sanitised slug. Distinct names stay distinct. Migration: existing `meta.json` files reference legacy slugs — read `owner_slug` from `meta.json` on enumeration, don't re-derive from `m_User`. New writes always use the hashed form.

**Acceptance.** Two user names that pre-fix collapsed to the same slug produce distinct hashed slugs. Existing adhoc folders (legacy slug in `meta.json`) remain reachable via their stored `owner_slug`. New adhoc workflow runs land under hashed-slug folders.

**Effort.** Small-medium (~half day; migration logic is the careful part).

---

### Sitting 10 — `ConfigParser` 36-field boilerplate refactor

**Problem.** `configParser.cpp::Parse()` has 26 `else if` branches (lines 207–566); `ParseInterfaces()` has 10 more (lines 693–883). Total **36 fields** (todo.md said ~30), each 5–7 lines of `get_X().get(target)` → `LOG_CORE_ERROR` → store → `++count` boilerplate. Pattern is identical per field-type (string / int64 / bool / numeric-with-bounds). Cross-ref `feedback_cpp_discipline` ("extract a helper before a third site appears" — we have 36).

**Fix.** Behaviour-neutral collapse to per-type helpers:
- `ParseStringField(value, key, target, fieldEnum, occurrences)`
- `ParseInt64Field(...)` / `ParseBoolField(...)`
- `ParseNumericFieldWithBounds(..., min, max)` for capped integers

Each becomes a single-line call. Total `Parse()` + `ParseInterfaces()` shrinks from ~700 lines to ~100, with the 4 helpers ~40 lines each.

**Acceptance.** Build clean across 5 targets. `test/run_tests.py --all` passes. Hand-craft a malformed config fixture (will be reused in Sitting 11) and verify the same ERROR-then-continue behaviour pre/post.

**Effort.** Medium (~1 day). Purely additive helpers + 36 site rewrites.

---

### Sitting 11 — Verification: malformed config.json tests + D1 negative-path fixtures

Two test-coverage items folded together (same shape: feed malformed input, assert rejection + ERROR log + engine survival).

**Malformed `config.json` tests.** Build `test/config/test_malformed_configs.py` against a fixture set under `test/config/fixtures/`: negative-numeric.json, type-mismatch.json, unknown-API.json, oob-API-index.json, url-substring-attack.json. Each fixture must produce (a) the expected ERROR log, (b) `Parse()` returns the right `State`, (c) `ConfigChecker::Check()` rejects, (d) j9t stays alive.

**D1 negative-path fixtures.** Build `test/hardening/` with one test per rejection branch surfaced during S3 sittings 6+7+8+9: path-traversal in file_watch / aiRequestPool / registry / adhoc / db_query / polarion; row+byte+timeout caps in db_query; size cap on adhoc JCWF + AI output; reaper CV wake-on-stop; WorkflowRegistry mutex stress under concurrent reload+PUT; inflight-counter race (mock-transport-driven).

Closest existing pattern: `test/dispatch/test_envelope_empty_body_rejected.py`. Reuse its shape (malformed input → REST API call → assert HTTP status + ERROR log substring + engine still responsive on follow-up `/api/info`).

**Acceptance.** Every rejection branch and every cap has at least one test verifying (a) operation fails with documented error code/message, (b) corresponding ERROR log line fires, (c) engine remains alive. Total ~15-20 new tests.

**Effort.** Medium (~1 day).

---

### Sitting 12 — Cloud tail: persist `email_watch` watermark across restart

**Problem (scope-corrected).** todo.md described email_watch as "fires on poll timer regardless of IMAP". Reality: `triggerEngine.cpp:787-884` + `emailConnector.cpp::CheckForNewMail:446-575` already do proper IMAP UID filtering with first-poll watermark seeding. Real gap: `m_LastSeenUid` is a runtime-only field on `EmailWatchTriggerInstance` (`triggerEngine.h:340`). After j9t restart, the watermark is re-seeded from current IMAP state — mail that arrived during the restart window becomes the new baseline (silent skip), not a trigger event.

**Fix.** Persist last-seen UID per `(connection_name, folder)` pair into a small state file at `queue/.email_watermark.json` (or alongside the trigger's JCWF metadata if the architecture prefers per-workflow). Write after each successful poll (atomic-write per Sitting 1's pattern). Load on startup before the first poll fires.

**Acceptance.** Test: (1) start j9t, seed watermark on first poll, (2) send a test mail, (3) restart j9t mid-poll, (4) verify the test mail still fires the trigger after restart (current behaviour: silent skip).

**Effort.** Small (~half day).

---

### Sitting 13 — `tools/replayTranscript.py`

**Problem.** Nominally-planned dispatch debugging tool from §5g. Reads `<prob>.transcript.json` and re-emits the exact request body against the same provider, for reproducing drift. Not built; transcript format is stable per `doc/architecture.md:103` and `doc/misc/AI dispatch refactor.md`.

**Fix.** Single Python script under `tools/replayTranscript.py`: arg = path to transcript.json, read recorded request envelope + provider identity, signal hot-path to bypass the JCWF runtime, post directly to provider's HTTP endpoint with current credentials (resolved from the same `KeyManager` as a live dispatch), pretty-print the diff between recorded and current response.

Trigger condition: first real "this dispatch worked yesterday, broke today" report. Don't build speculatively.

**Acceptance.** Replay a recorded transcript from `test/dispatch/fixtures/` against MockTransport, get byte-identical response. Replay against live provider, get structured diff against recorded response.

**Effort.** Small-medium (~half-1 day). Depends partly on whether replay needs to skip SigV4 re-signing (different timestamp = different signature) — likely yes, with a `--re-sign` flag.

---

### Sitting 14 — KeyManager hardening tail (4 small items)

**Problem.** Four hardening items flagged in the prior session's hand-off carry-over list that were never propagated into `todo.md` and so missed the original plan intake. Folded in here at JC's direction; small enough to bundle into one sitting.

1. **`KeyManager::SetDefaultProvider` empty-name handling** (audit MEDIUM, `combinedCyberSecAudit.md` line 2446). Current behaviour: `name.empty()` branch silently clears the default provider. Surprising at call sites that pass through an uninitialised string. **Fix:** add explicit `KeyManager::ClearDefaultProvider()` separator method; make `SetDefaultProvider(name)` reject empty names (return false + log).

2. **HIGH TOCTOU race in `Unlock` between `filesystem::exists` and `Load`** (`combinedCyberSecAudit.md` line 2398). `KeyManager::Unlock` checks file existence, then calls `Load` — a concurrent `SetKeysFilePath` between the two would race. Mitigated in practice (path is set at startup and rarely changes), still worth closing. **Fix:** capture the path once under `m_Mutex` at entry, use the captured value for both the exists-check and the `Load` call; reject empty path early.

3. **HIGH `LoadPlaintext` / `SavePlaintext` available without build-guard** (line 2414). Development-only methods compiled into production builds. **Fix:** wrap with `#ifdef J9T_DEVELOPMENT_BUILD` (or equivalent — verify the existing dev-build define before introducing a new one). Caller sites already in test code; if any production caller surfaces during the sweep, that's a finding to escalate.

4. **HIGH `ParseProvidersJson` unbounded allocation** (line 2421). Provider count and per-field string length have no caps; a malicious keystore could OOM the parser on `Load`. **Fix:** add `kMaxProviders` (e.g., 1024) + `kMaxFieldLength` (e.g., 4096) compile-time caps; fail-closed with typed `Error` if exceeded.

**Acceptance.** Build all 5 targets clean. Synthetic test feeds (a) empty-string `SetDefaultProvider` → rejection, (b) malformed keystore with 100k providers → parser rejection + ERROR log, (c) `Unlock` exercised under concurrent `SetKeysFilePath` (relaxed — single-threaded test is acceptable here since the production exposure is theoretical). `LoadPlaintext` / `SavePlaintext` linker errors in release build confirm the build-guard.

**Effort.** Medium (~half-1 day). Items 1 and 3 are ~half hour each; items 2 and 4 are ~1-2 hours each.

---

## Post-1.0 / opportunistic

- **`RedactingFormatter::format` per-line allocation** (`engine/log/log.cpp:55-73` + `secretRedactor.cpp:92-110`). Two heap allocs per log line when secrets registered (formatter materialises `std::string(payload)`, then `Redact()` makes another `result = message`). No-secrets fast path is already alloc-free. Pure perf; defer until profiling shows it hot under realistic load. Fix shape: thread-local buffer + early return when no secret matches.

---

## Sitting ordering rationale

1. **Sittings 1–3** are safety-first: writer atomicity, OOM defence, broken-feature restoration. Each ships independently and reduces latent risk.
2. **Sittings 4–5** are Studio/Engine cleanup — small, low-risk, finish what §5i started.
3. **Sitting 6** is the SigV4 + Bedrock work, scheduled before the broader API-shape sweep so the typed credential reference is in place when Sitting 7 lands.
4. **Sitting 7** is the API-shape sweep — single big sitting, lands on the cleaner foundations from Sittings 1–6.
5. **Sittings 8–10** are remaining cleanup (SecureString-HTTP, slug collision, ConfigParser refactor) — independent, parallelisable if multiple sittings happen close together.
6. **Sitting 11** is verification, intentionally last among the substantive work — it tests everything that came before.
7. **Sittings 12–14** are the small tail (email watermark, replay tool, KeyManager hardening). Sitting 14's four KeyManager items could also fold into Sitting 7's API-shape sweep opportunistically (the typed `Error` return on `SetDefaultProvider` / `ParseProvidersJson` lands naturally then) — keep them grouped if Sitting 7 happens close in time, otherwise ship Sitting 14 as a clean standalone.

Sittings 1, 2, 3, 4, 5, 12, 14 are each half-day to one day and can pair into combined sittings if time permits. Sittings 6, 7, 8, 10 are full sittings on their own.

Total estimated effort: ~11–14 working days across 14 sittings.

---

## Hand-off

Each sitting should follow the established hand-off log format (`doc/misc/hand-off.md`): prepend a new entry at session end with "What landed", "What's verified", "Architecture notes", "Open items / next-session candidates", "Gotchas", "Files in working tree".

Cross-references to update on plan completion:
- `todo.md` — remove the closed sections; preserve the "Loose follow-ups" entries that turn out to need further deferral.
- `MEMORY.md` — add a feedback memory for "new error-returning APIs use `std::expected`" after Sitting 7.
- `doc/misc/hand-off.md` — final entry notes plan completion + any deferred items.
