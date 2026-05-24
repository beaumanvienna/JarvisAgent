# Pre-1.0 follow-ups dev plan

Closeout plan for the three tail-end groups tracked in `todo.md` before the alpha (2026-04-05):

- **§5i follow-ups** (Studio/Engine split tail) — 3 items, 1 already done
- **Cloud integration tail** — 1 item, scope reduced after verification
- **Loose follow-ups** — 14 items (todo.md said 11; recount confirmed 14)
- **KeyManager hardening tail** (carry-over from prior hand-off, never propagated into `todo.md`) — 4 items

Total entering this plan: **22 items**. Closed at intake: **1** (§5i.3 — already fixed in code, todo.md was stale). Net actionable: **21 items + 4 added mid-plan** (Sitting 15 added 2026-05-21 after qwen-7b validated as test AI surfaced the plain-HTTP policy gap; Sitting 8d added 2026-05-22 to close the five residual-leak surfaces 8b documented and 8c's audit empirically confirmed are real but bounded; Sitting 8e added 2026-05-22 to close the four in-house residuals a post-8d walkthrough surfaced — the SecureString-only HTTP path is the kind of work that "wants to be finished, not deferred" so 8e closes the credential-leak chapter for 1.0; Sitting 16 added 2026-05-22 as a collection-point basket for off-topic findings that surface during regular sittings — the last stop before 1.0 tagging), organised into **18 sittings** ordered safety-first → cleanup → verification → tooling → cyber-sec tail → incidental-findings cleanup.  **Sittings 1+2 closed 2026-05-18**; **Sittings 3+4 closed 2026-05-18**; **Sitting 5 closed 2026-05-19**; **Sitting 6 closed 2026-05-19**; **Sitting 7a closed 2026-05-19** (with 7b + 7c carved out as follow-ups for the connector + parser sweeps); **Sittings 7b + 7c closed 2026-05-20** — the std::expected API-shape sweep is now complete across all three subsystems (registry / connectors / parsers).  See `doc/misc/hand-off.md` for the migration records.  **Sitting 8 split 2026-05-22** into 8a (engine + AI dispatch) / 8b (cloud connectors + SigV4 input) / 8c (heap-scan audit artifact, not CI-gated) after exploration found ~31 touch sites vs. the original "9 sites" estimate.  **Sittings 8a + 8b + 8c + 8d + 8e closed 2026-05-22** — the SecureString-only HTTP path is now codified end-to-end across both the AI-dispatch path (8a) and all 11 cloud connectors + workflow filter + AzureSharedKey/SigV4 input phases (8b); the empirical verification artifact (8c) at `test/security/heapScan_test.{h,cpp}` + `heapScan_cloud_scenarios.cpp` is `--heapscan`-gated and confirms zero heap residue across all must-be-zero scenarios; 8d closed the five 8b-documented residual surfaces (cloud HMAC scratch, Google Sheets URL key, postgres conninfo, OAuth refresh POST body, generator return-by-value); 8e closed the four in-house follow-on residuals (OAuth refresh snapshots + `RefreshResult`, `JwtGenerator::Generate` local std::string, `ExchangeJwtForAccessToken` SecureString in+out, engine awsSigV4 canonical-headers via `SecureString::Build`) and added the `engineSigV4(no-churn)` structural-check scenario.  Only the libcurl strdup floor (architectural) remains.  **8 sittings remain** as of 2026-05-22 (Sittings 9 + 10 + 11 + 12 + 13 + 14 + 15 + 16).

**Sitting 9 closed 2026-05-23** — slug collision fix shipped; horizontal sweep into the shared `Sha256Hex` helper at `engine/auxiliary/sha256.{h,cpp}` (consolidated the two pre-existing file-local copies).

**Sitting 10 closed 2026-05-23** — `ConfigParser` boilerplate collapsed into 6 file-local helpers (`ParseStringField`, `ParseStringFieldLogOnly`, `ParseInt64Field` with `NumericPolicy` enum, `ParseUint64Field`, `ParseBoolField`, `ParseInt64FieldWithBounds`); 29 of 38 field-parse sites became single-line calls; 9 outliers stay inline (counter-only / array / platform-conditional / silent-on-failure / nested / migration).  Behaviour-neutral verified two ways: (a) byte-identical happy-path diff of ConfigParser startup log against the pre-refactor binary (zero lines diff aside from timestamps); (b) malformed-config + out-of-range fixtures produce byte-identical ERROR/WARN/INFO output (`ConfigParser: 'description' must be a string`, `port 99999 out of range [0, 65535], defaulting to 0 (auto)`, etc.).  4 fixture files landed under `test/config/fixtures/` for Sitting 11's malformed-config test harness to consume.

**Sitting 14 closed 2026-05-23** — KeyManager hardening tail (4 small items): (1) `[[nodiscard]] bool SetDefaultProvider(name)` rejects empty + unknown, new `ClearDefaultProvider()` separator; (2) `Unlock` TOCTOU closed via new `m_KeysFilePathMutex` + capture-then-use; (3) `LoadPlaintext`/`SavePlaintext` declarations + bodies + caller all `#ifdef J9T_STUDIO`-wrapped (Engine binaries strip the symbols — verified via `nm`); (4) `kMaxKeysFileBytes = 4 MB` cap in `Load`/`LoadPlaintext` + `kMaxProviders = 1024` cap in `ParseProvidersJson`, both fail-closed with structured ERROR.  All 4 binary configs build clean; full regression green (test_auth_mcp 100/100, collision repro 14/14, malformed_configs 46/46, hardening 28/28, inputResolutionTest 4/4).  See `doc/misc/hand-off.md` 2026-05-23 entry.

**Sitting 15 closed 2026-05-23** — Plain-HTTP loopback policy + credentialed-HTTP refusal across config-load and REST CRUD.  New `application/network/urlPolicy.{h,cpp}` exposes `ValidateAiInterfaceUrl(url, keyName) -> std::expected<void, UrlPolicyError>` with five typed codes (MalformedUrl / SchemeRejected / NonLoopbackHttp / UnresolvedHost / CredentialedPlaintextHttp).  Enforcement at `ConfigParser::ParseInterfaces` (fail-closed — rejected entries dropped from `m_ApiInterfaces` with LOG_CORE_ERROR), `HandleAiInterfaceCreatePost`, and `HandleAiInterfaceUpdatePut` (HTTP 400 + LOG_SECURITY_WARN; PUT re-validates after partial updates).  `ConfigChecker::checkUrl` (downstream consumer) widened to accept `http://` in addition to `https://` since any `http://` URL that reaches it has already passed the parse-time gate (loopback-only, no `key_name`).  Per-dispatch LOG_SECURITY_INFO line for every successfully dispatched http:// request (`[security] ai_dispatch_plaintext_http host='...'`).  Two atomic counters on `/api/debug/signals`: `url_policy_rejections` + `credentialed_plaintext_http_rejections`.  Dashboard surface: yellow "loopback HTTP" pill next to http:// URLs in AiManagerView + yellow banner above the interface list when the default `api_index` resolves to a plain-HTTP interface.  New `test/security/test_url_policy.py` (27/27 PASS — 7 rejection variants + 5 accept variants + counter-delta assertions) + new fixture `test/config/fixtures/url_policy_violation.json` (3 rejection categories exercised at ConfigParser).  Shipped `config.json` had `key_name: "ollama"` on 2 ollama interfaces (operator-confusion case the gate caught at first config-load); removed — ollama is unauthenticated.  All 4 binary configs build clean; both React apps rebuild clean; regression green (test_auth_mcp 100/100, collision 14/14, malformed_configs 56/56 across 8 fixtures, negative_paths 28/28, api1_mock_errors 6/6, bedrock_sigv4 KAT unchanged, inputResolutionTest 4/4).  Live verification on 2026-05-24: `jarvisCppCyberSecAudit` ran end-to-end on qwen-7b (`api_index=11` → `http://localhost:11434/v1/chat/completions`) with 146 plaintext dispatch audit lines in `log/security.txt`, GPU 93% utilisation peak, audit completed terminal=succeeded — the local-LLM path is unblocked exactly as Sitting 15 set out to do.  **1 sitting remains** as of 2026-05-24 (Sitting 16 incidental-findings basket; Sitting 13 cancelled).

**Sitting 12 closed 2026-05-23** — email_watch UID watermark persisted across restart at `<queue_folder>/.email_watermarks.json` (atomic-rename per successful poll).  `TriggerEngine` constructor loads the file; `AddEmailWatchTrigger` restores the persisted UID when `(workflowId, triggerId)` matches an entry; post-poll update site mirrors the in-memory watermark into the persisted map + writes the file.  Verified end-to-end live: planted a 2-entry sentinel file, restarted j9t, observed `loaded 2 persisted UID watermark(s)` INFO at startup + `restored persisted UID watermark '99999' for trigger 'email_t1' workflow 'sitting12_canary'` INFO when the matching trigger registered.  Save path is the mirror of load (same in-memory map + same JSON shape, written via `EngineCore::AtomicWriteFile`); live end-to-end of save requires a working IMAP server which the host environment doesn't have (`my-email` connection's empty endpoint causes IMAP-target rejection before any successful poll).  All 4 binary configs build clean; full regression suite green (test_auth_mcp 100/100, collision repro 14/14, malformed_configs 46/46, hardening 28/28, inputResolutionTest 4/4).  See `doc/misc/hand-off.md` 2026-05-23 entry.  **2 sittings remain** as of 2026-05-23 (15 + 16; Sitting 13 cancelled).

**Sitting 11 closed 2026-05-23** — D1 negative-path verification.  Two new test files landed: `test/config/test_malformed_configs.py` (subprocess-based config-parse harness; 46 passing checks across 7 fixtures) + `test/hardening/test_negative_paths.py` (REST-based against running j9t; 28 passing checks across 4 groups covering size caps, path-traversal on three JCWF surfaces, mutex stress + inflight-counter leak detection, and db_query row-cap / timeout / output-path traversal).  3 new fixtures (`unknown_API.json`, `oob_API_index.json`, `url_substring_attack.json`) added on top of Sitting 10's 4 fixtures + `_expected_errors` lists retrofitted into all 7 for the harness.  Sitting 9 regression caught + fixed mid-sitting: `test_adhoc_folder_namespace` had a literal `/_adhoc/{user}/` assertion that didn't accommodate the new `_<8hex>` slug suffix — `test_auth_mcp.py` back to 100/100.  Deferred to Sitting 16 (cannot be cleanly REST-driven from a shared host): polarion WriteItemFile path-traversal, AI output 64 KB size cap, reaper CV wake-on-stop.  See `doc/misc/hand-off.md` 2026-05-23 entry.

**Sittings 9 + 10 + 11 + 12 + 14 + 15 all closed 2026-05-23** — the entire substantive Pre-1.0 work landed in a single day.  **1 sitting remains** as of 2026-05-23 (Sitting 16 incidental-findings basket; Sitting 13 cancelled).

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

### Sitting 7b — Connector subsystem sweep — **closed 2026-05-20**

`ICloudConnector::TestConnection` base virtual + all 13 concrete overrides (azureBlob, email, gcs, gitHub, googleSheets, jira, oneDrive, polarion, postgres, redmine, s3, slack, snowflake) + `ConnectorHttp::ValidatePublicHttpEndpoint` + `PostgresConnector::ValidatePostgresParams` flipped to `[[nodiscard]] std::expected<void, ConnectorError>`.  Per-site `Code` selection: param missing/blank → `InvalidConfig`; endpoint SSRF rejection → `InvalidEndpoint`; ResolveCredentials failure (bridge) → `CredentialMissing`; CRLF / structurally-bad credential → `CredentialInvalid`; `curl_easy_init()` / `CURLE != OK` → `NetworkError`; HTTP 401/403 → `AuthFailure`; HTTP ≥ 400 (other) → `HttpError`.  `CloudCircuitBreaker::RecordFailure` extended to accept optional `ConnectorErrorCode`; `ConnectionHealth::m_LastFailureCode` propagates through `GetHealthSummary` and surfaces on `/api/status::connection_health[].last_failure_code`.  `HandleConnectionTestPost` echoes `code` (typed) + `message` (Details) on failure.  All 4 binary configs build clean; `test_mock_dispatch_hermetic`, `test_bedrock_sigv4` (SigV4 KAT unchanged), `test_api1_mock_errors` 6/6 green.  Live verification: misconfigured my-s3 connection test → HTTP 400 + `code: "network_error"` + breaker records `last_failure_code: "network_error"`.  See `doc/misc/hand-off.md` 2026-05-20 entry.

### Sitting 7c — Workflow JSON parser sweep — **closed 2026-05-20**

All 23 parser methods + 2 Require helpers + 1 free-standing `ParseTaskQueueBinding` flipped to `[[nodiscard]] std::expected<void, ParserError>`: 3 public entry points (`ParseWorkflowJson` / `ParseGlobalJson` / `ParseCanvasJson`), `ParseRootObject`, and the chain of 17 sub-parsers (`ParseTriggers`/`ParseTrigger`, `ParseTasks`/`ParseTask`, `ParseTaskInputs`/`ParseTaskOutputs`/`ParseTaskEnvironment`/`ParseTaskQueueBinding`, `ParseDataflow`/`ParseSingleDataflow`, `ParseRetries`/`ParseDefaults`, `ParseFilters`/`ParseFilter`/`ParseFilterSource`, `ParseControlNodes`/`ParseControlflow`).  Per-site `Code` selection: array/object shape rejection → `TypeMismatch`; required-field absent → `MissingField`; cap-exceeded / negative-value / out-of-allowlist → `ValueOutOfRange`; simdjson decode failure → `SimdjsonError`.  ~150 return sites total.  Utility helpers `ExtractRawJson` and `ElementToString` deliberately left on the legacy bool+errorMessage shape (out of plan scope; parser methods bridge their failures into typed `SimdjsonError` / `TypeMismatch` at the call site).  External callers updated: `workflowRegistry.cpp` (5 sites: registry CRUD + container load + sub-workflow load), `aiJcwfService.cpp` (1 site), `webServer_studio.cpp` (2 sites: POST + PUT), `webServer_helpers.h` (1 site: ValidateJcwfJsonText).  All 4 binary configs build clean.  Verified end-to-end: all 32 shipping JCWFs reload cleanly; malformed JCWFs return human-readable typed-error messages (e.g. `"workflow missing required field: tasks"`, `"tasks must be an object"`, `"task field 'timeout_ms' must be non-negative, got -5"`); regression suite green (`test_mock_dispatch_hermetic`, `test_bedrock_sigv4` KAT unchanged, `test_api1_mock_errors` 6/6).  See `doc/misc/hand-off.md` 2026-05-20 entry.

---

### Sitting 8 — SecureString-only path through HTTP layer

**Problem.** Every HTTP-build site materialises `SecureString::Get() → std::string_view` into plain `std::string` via `"Authorization: Bearer " + view` concatenation — across the 4 static-header signers, `QueryData::m_ApiKey`, the 4 AI-dispatch resolver sites in `aiRequestPool.cpp`, the SigV4 input phase in `awsSigV4.cpp`, the `CloudCredentials::m_Token`/`m_SecretKey` populate sites in 9 cloud connectors, and ~22 inline `"Authorization: Bearer " + token` build sites across cloud + workflow-filter code (verified by exhaustive grep — original "9 sites" undercount). Each concat allocates a heap-resident `std::string` containing the full secret; the slab is not zeroed when the string destructs. On a compromised process the secret is recoverable from heap residue (debugger, core dump, allocator fastbin replay). Defense in depth, not a current vulnerability.

**Fix.** Sized to ~3 commits, split into three sittings:

- **8a — engine + AI dispatch** (~half day). Extend `SecureString` with `Format(prefix, secretView, suffix)` (mlock + copy-and-swap on exception) and `CStr()` (guaranteed null-terminated). Change `IAuthSigner::Apply` signature to add a caller-owned `SecureString& secretHeader` out-param (no `optional`, one fewer `mlock` syscall per request). Refactor the 4 static-header signers (Bearer / x-goog-api-key / x-api-key / api-key). Change `QueryData::m_ApiKey` from `std::string` to `SecureString` + cascade to `IsValid()` (`curlWrapper.cpp:180`) and the 3 `Apply` call sites (`curlWrapper.cpp:297`, `liveTransport.cpp:211`, `mockTransport.cpp:161`). Update the 4 `aiRequestPool.cpp` resolver sites (lines 976, 982, 2023, 2027).

- **8b — cloud connectors + workflow filter + SigV4 input phase** (~half day). Change `CloudCredentials::m_Token`/`m_SecretKey` (`application/cloud/cloudConnector.h:51,54-56`) from `std::string` to `SecureString`. Introduce a `AppendSecretHeader(curl_slist*&, prefix, SecureString const&, SecureString& scratch)` helper to factor the ~22 inline `"Authorization: Bearer " + token` sites across `application/cloud/*.cpp` and `application/workflow/filter/polarionClient.cpp` (lines 348/416/489/558). Fix SigV4 input phase (`awsSigV4.cpp:449-450`) by extending `ScopedSecretBytes` to the input phase. `AwsCredential::m_AccessKeyId` stays public per AWS conventions.

- **8c — heap-scan audit artifact** (separate, NOT CI-gated). New `test/security/heapScan_test.cpp` with dual-mode: smoke variant (scans test process's main arena post-drop, runs in CI) + deep variant (`/proc/self/maps` + `/proc/self/mem`, requires `CAP_SYS_PTRACE`, gated `J9T_HEAPSCAN_DEEP=1`, one-shot pre-1.0 audit artifact — not a merge gate). Smoke variant covers regression detection across all 4 static-header signers + each connector path + SigV4.

**8a closed 2026-05-22** — see `doc/misc/hand-off.md` entry.  **8b closed 2026-05-22** — `CloudCredentials::m_Token`/`m_SecretKey`/`m_Password` → SecureString; new `AppendSecretHeader` helper at `engine/curlWrapper/curlSlistHelper.{h,cpp}` factors ~22 inline `"Authorization: Bearer " + token` sites; AzureSharedKeySigner + cloud SigV4Signer + engine awsSigV4 input phase take SecureString for the secret key; engine awsSigV4 routes X-Amz-Security-Token through the SecureString secretHeader slot; `ContainsCrlf` widened to `string_view`.  All 4 build targets clean; end-to-end smoke green for OpenAI Bearer (AI re-verify), GitHub / Polarion / Slack Bearer connectors, Jira BasicAuth (m_Password path), Azure Blob Shared Key (m_SecretKey via AzureSharedKeySigner).  Known residual leaks documented inline: URL-embedded `?key=` (Google Sheets), postgres connStr `password=`, OAuth refresh POST body, Sign-function-internal HMAC intermediates in cloud SigV4 + AzureSharedKey (bounded by Sign() lifetime, not propagated outward).  **8c closed 2026-05-22** — in-process heap-scan audit at `test/security/heapScan_test.{h,cpp}` + `heapScan_cloud_scenarios.cpp`, gated by premake `--heapscan` (sets `J9T_HEAPSCAN_BUILD`).  Audit-build binary plants a 64-byte hex nonce as the secret for each auth style, drives the auth-build path end-to-end, churns 64 MiB through the allocator, then walks `/proc/self/maps` + reads `/proc/self/mem` (`[heap]` for smoke, `[heap]` + anon mappings for deep — `[stack*]` excluded due to scanner self-detection).  Results: 5/5 must-be-zero scenarios PASS (Bearer / XGoogApiKey / AnthropicXApiKey / AzureApiKey / engineSigV4 — zero residue in both smoke and deep); `AppendSecretHeader` shows the documented libcurl-`strdup` floor (1 hit); cloud SigV4 + AzureSharedKey Sign() residuals evaporated under the churn (stronger result than 8b documented — the leak window is shorter than expected).  `doc/cyber security.md` "SecureString-only HTTP path" section grew the empirical-verification sub-section with the full method + scenario table + result snapshot.  Production builds carry zero audit code (engine.cpp call site and audit module body are both `#ifdef J9T_HEAPSCAN_BUILD`-guarded).

**Out of scope (separate hardening tracks).** `QueryData::m_Data` request body (Anthropic prompts, Bedrock SigV4-signed body); Google Sheets URL-parameter auth variant (secret in `?key=...` query string); OAuth refresh-token round-trip POST body (`client_secret` in form-urlencoded body); libcurl's internal `curl_slist_free_all` `free()` (not `explicit_bzero+free`) — irreducible residue floor, outside threat-model boundary.

**Acceptance.** All 5 build targets clean (Studio Debug+Release, Engine Debug+Release, Rocky 9 / Docker CI). End-to-end smoke against the dispatch path (Bearer + x-goog + x-api-key + api-key signers each exercised via a real connection-test or workflow run). SigV4 KAT `SigV4Signer::RunSelfTest` still passes. 8c smoke variant: zero residue hits for the 32-byte nonce across each signer + connector path.

**Effort.** Three commits (8a + 8b + 8c). Total ~1.5 days. Original "Medium ~1 day" was an undercount — accurate for 8a alone.

**Plan reference.** `/home/beaumanvienna/.claude/plans/serialized-honking-sundae.md`.

---

### Sitting 8d — Square away SecureString residual leaks — **closed 2026-05-22**

Closed same day it was planned.  All 5 residual surfaces 8b documented are now structurally absent — empirically verified by the 8c audit re-run with 2 flag flips + 1 new scenario.  Result: **9 audit scenarios** (8 must-be-zero + 1 architectural floor); **8 PASS, 0 FAIL, 1 expected-residual (libcurl strdup floor)**.  Pre-8d the audit reported 5 pass + 0 fail + 3 expected-residual (libcurl + cloud SigV4 + AzureSharedKey "residual evaporated"); post-8d only the architectural libcurl floor remains.  See `doc/misc/hand-off.md` for the close-out entry.



**Problem.** After 8a + 8b + 8c codified the SecureString-only HTTP path and verified it empirically, five residual leak surfaces remain documented inline at their call sites and listed under "Known residual leaks" in `doc/cyber security.md`.  Each is a separate hardening track that 8b/8c deliberately deferred so the main refactor + audit could land first.  Bundling them now because:

- The 8c heap-scan audit gives a clean empirical regression net — every fix is verifiable end-to-end without relying on code review alone.
- The remaining surfaces are independent of each other (no cross-dependencies) but share theme + reviewer context, so a single sitting is more efficient than five drip-fed small PRs.
- Closing them pre-1.0 removes the "yes, we know about these" footnote from the cyber-sec review artifact.

The five surfaces:
1. Cloud `Sign()` HMAC scratch — `application/cloud/sigV4Signer.cpp` and `application/cloud/azureSharedKeySigner.cpp` materialise the secret into a `std::string` intermediate for the HMAC chain (`awsSecret` and `rawKey` respectively), bounded by `Sign()` lifetime but observable in heap residue during the call.
2. Google Sheets URL `?key=<token>` — the secret appears in the URL query string (`googleSheetsConnector.cpp:170`, `googleSheetsCloudTaskExecutor.cpp:243`).  URL bytes flow through libcurl as a `std::string` (no `SecureString` path), get logged in some debug-curl modes, and may appear in HTTP proxy access logs upstream.
3. Postgres conninfo `password=<secret>` — `postgresConnector.cpp:277` concatenates the password into a `std::string connStr` then passes the whole string to `PQconnectdb`.  The `std::string` heap copy lives until `BuildConnectionString` returns.
4. OAuth refresh-token POST body — `oauthTokenManager.cpp:576-583` builds `postBody` as a `std::string` containing the urlencoded `client_secret` and `refresh_token`, then hands it to `curl_easy_setopt(CURLOPT_POSTFIELDS, ...)`.
5. Generator return-by-value — `OAuthTokenManager::GetAccessToken` returns the access token as `std::string` by value; `JwtGenerator::GenerateSnowflakeJwt` returns the signed JWT by value.  Each caller (5 cloud connectors) does `credentials.m_Token.Set(view)` immediately, but the heap-resident `std::string` returned-by-value lives until the caller's full-expression evaluation completes.

Defense in depth, not current vulnerabilities — but the same defense-in-depth argument that motivated 8a/8b/8c applies here.

**Fix.** Five independent parts; each is its own commit-sized unit.

1. **Cloud HMAC `Sign()` scratch.**  Port the engine-side `ScopedSecretBytes` pattern from `engine/curlWrapper/awsSigV4.cpp:55-75` to the cloud-side signers.  Specifically:
   - `application/cloud/sigV4Signer.cpp:306-313` — replace `std::string awsSecret = "AWS4" + secretKey.Get()` with a `std::vector<unsigned char> awsSecret` built directly from the SecureString view, wrapped in `ScopedSecretBytes` (or whatever name harmonises with the engine sibling).  Widen `HmacSha256` / `HmacSha256Hex` if needed so the byte-vector flavour is supported.  HMAC intermediates `kDate` / `kRegion` / `kService` / `kSigning` likewise wrapped (mirrors engine sibling line-for-line).
   - `application/cloud/azureSharedKeySigner.cpp:277-284` — replace `std::string rawKey = Base64Decode(accountKey.Get())` with a `std::vector<unsigned char> rawKey` written into by a new `Base64DecodeToBytes(std::string_view src, std::vector<unsigned char>& out)` overload that doesn't materialise an intermediate `std::string`.  Wrap in `ScopedSecretBytes` for OPENSSL_cleanse-on-destruct.  HMAC chain consumes the byte vector directly.
   - Consider relocating `ScopedSecretBytes` to `engine/keys/secureString.h` (alongside `SecureString`) so the cloud signers can share the engine-side definition without duplicating.  Alternative: leave one copy per file, accept the small duplication.  Pick one in the implementation sitting based on cross-TU visibility constraints.

2. **Google Sheets URL `?key=` → `X-Goog-Api-Key` header.**  Google's API accepts the same API key in the `X-Goog-Api-Key` HTTP header — semantically equivalent to the URL query parameter, no behaviour change on the server side.  Swap two sites:
   - `application/cloud/googleSheetsConnector.cpp:170` — drop `url += "&key=" + std::string(credentials.m_Token.Get())`; add `AppendSecretHeader(headers, "X-Goog-Api-Key: ", credentials.m_Token, authScratch)` (helper from 8b already used in this file at line 191 for the OAuth-Bearer path).
   - `application/cloud/googleSheetsCloudTaskExecutor.cpp:243` — same swap.  Verify the URL no longer carries a trailing `&` / `?key=` fragment after the change (drop the URL-builder branch that prepended the separator).

3. **Postgres conninfo → `PQconnectdbParams`.**  Replace `PQconnectdb(connStr.c_str())` at `application/cloud/postgresConnector.cpp:329` with the parallel-array variant:
   - Build `const char* keywords[] = {"host", "port", "dbname", "user", "password", "sslmode", "connect_timeout", nullptr};` (NULL-terminated array).
   - Build `const char* values[]` with the corresponding values; the `password` slot points directly at `credentials.m_Password.CStr()` (no `std::string` intermediate).  Omit the `password` entry (or pass an empty string — verify libpq behaviour) when `m_Password.IsEmpty()`.
   - Call `PQconnectdbParams(keywords, values, /*expand_dbname=*/0)`.
   - libpq still makes its own internal copy of the password into a `PGconn`-owned buffer (the libpq floor, analogous to libcurl's strdup floor) — outside our threat-model boundary.
   - Delete `BuildConnectionString` + the `escape` helper if no other caller remains.
   - Per existing `feedback_secrets_only_via_redactor`: the `SecretRedactor` already registers `m_Password` at credential-load time, so the only logging-exposure risk was the `connStr` itself (which never appeared in any log line); no redactor change needed.

4. **OAuth refresh POST body → SecureString.**  Three components:
   - **Extend `SecureString`** with a `Build(std::span<std::string_view const> pieces)` method (or equivalent variadic-template / initializer-list signature) that concatenates N pieces into a single mlock'd allocation with the same strong exception guarantee as `Format` (copy-and-swap on a temporary, originals untouched on `bad_alloc`).  `Format(prefix, secret, suffix={})` is the existing 3-piece variant; `Build` is the N-piece variant.
   - **Extend `CurlEscapedString`** in `engine/keys/oauthTokenManager.cpp:64-83` with a `std::string_view view() const` accessor returning the libcurl-allocated buffer as a view (no `std::string` materialisation).  Fall back to the raw source view if `m_Ptr` is null (escape failure path).
   - **Refactor `PerformRefresh`** at `oauthTokenManager.cpp:576-583`: build `postBody` as a `SecureString` via `postBody.Build({"grant_type=refresh_token&refresh_token=", escRefreshToken.view(), "&client_id=", escClientId.view(), ...})`.  The conditional `&client_secret=...` branch becomes a second `Build` call (or use a `std::vector<std::string_view>` builder).  `curl_easy_setopt(CURLOPT_POSTFIELDS, postBody.CStr())` — `CURLOPT_POSTFIELDS` does NOT copy by default, so the SecureString must outlive `curl_easy_perform` (just keep `postBody` in scope until `curl_easy_cleanup`).
   - The `ToString(fallback)` method on `CurlEscapedString` can stay for any non-secret callers, or be removed if all callers migrate to `view()`.

5. **Generator return-by-value → out-parameter SecureString.**  Two interface changes + 5 caller updates.
   - **`engine/keys/oauthTokenManager.h:59`** — change `std::string GetAccessToken(std::string const& keyName, std::string& errorMessage)` to `[[nodiscard]] bool GetAccessToken(std::string const& keyName, SecureString& out, std::string& errorMessage)`.  Internally, the cached token already lives in a SecureString-equivalent buffer (verify via `m_Tokens` shape); writing into the caller's `out` becomes a `.Set(view)` instead of returning the underlying string.
   - **`engine/keys/jwtGenerator.h:50`** — change `static std::string GenerateSnowflakeJwt(...)` to `[[nodiscard]] static bool GenerateSnowflakeJwt(..., SecureString& out, std::string& errorMessage)`.  The signed JWT is constructed internally from OpenSSL primitives; writing the final base64-joined result into `out` is a single `.Set(...)`.
   - **Update 5 callers** (horizontal sweep per `feedback_horizontal_sweeps`):
     - `application/cloud/oneDriveConnector.cpp:65`
     - `application/cloud/azureBlobConnector.cpp:71`
     - `application/cloud/snowflakeConnector.cpp:164`
     - `application/cloud/gcsConnector.cpp:270`
     - `application/cloud/googleSheetsConnector.cpp:92`
   - Each caller swaps `credentials.m_Token.Set(generator.GetAccessToken(...))` → `if (!generator.GetAccessToken(name, credentials.m_Token, errorMessage)) { ... }` (or the JWT equivalent).  Error path: log + return `ConnectorError::CredentialMissing` / `::CredentialInvalid` per the existing typed-error pattern from Sittings 7a/7b/7c.

**Verification.**  After all five parts land, the 8c heap-scan audit gets extended + re-run:

1. **Re-run** `premake5 gmake --clang --heapscan && make config=debug && ./bin/Debug/jarvisAgent-studio`.  Expected delta from the 2026-05-22 close-out snapshot:
   - Scenarios `cloudSigV4::Sign` and `AzureSharedKey::Sign` flip from `expected_residual=true` to **`expected_residual=false`** — bytes genuinely don't materialise anywhere.  Both must report 0 hits in both smoke and deep (no longer "evaporated under churn" — actually never present).
   - All 5 original must-be-zero scenarios (Bearer / XGoogApiKey / AnthropicXApiKey / AzureApiKey / engineSigV4) still PASS unchanged.
   - `AppendSecretHeader` libcurl-strdup floor unchanged (1 hit — architectural, outside threat-model boundary).

2. **Add new audit scenarios** to `test/security/heapScan_test.cpp`:
   - **OAuth POST body** (`ScenarioOAuthPostBody`): builds a `SecureString postBody` via `Build` with a nonce in the `client_secret` position, hands `postBody.CStr()` to a dummy `CURLOPT_POSTFIELDS` setopt (no live HTTP call needed — the audit just needs `postBody` to destruct in scope), scans.  Expected: 0 / 0 hits.  Must-be-zero scenario.
   - **Postgres password value pointer** (`ScenarioPostgresParams`): synthesises a `keywords[]`/`values[]` array with a nonce as the password value, calls `PQconnectdbParams` against a fake unreachable host, immediately `PQfinish` on whatever `PGconn*` comes back, scans.  Expected: small bounded libpq-internal residue (analogous to libcurl floor — libpq copies the password into a `PGconn`-owned buffer).  Documented-residual scenario.  Update `doc/cyber security.md` to mention the libpq floor alongside the libcurl floor.

3. **Update `test/security/heapScan_test.cpp`** to flip the `expected_residual` flags for scenarios `cloudSigV4::Sign` and `AzureSharedKey::Sign` from `true` to `false`.  A regression that reintroduces the leak now FAILs the audit.

4. **Update `doc/cyber security.md`**:
   - "SecureString-only HTTP path" → "Empirical verification — heap-scan audit" sub-section: scenario table row for cloud SigV4 + Azure SharedKey flips from "known residual" to "0 / 0".  New rows for OAuth POST body (must-be-zero) and Postgres params (libpq floor).  Result snapshot updated with the new PASS counts (7/7 must-be-zero + 2 documented floors).
   - "Known residual leaks" list reduced to **2 items** (the architectural floors): libcurl strdup floor, libpq internal password copy.  The five 8b-documented hardening tracks marked closed inline at their original call sites (replace "deferred to separate hardening track" with "closed in Sitting 8d").

5. **Update `CLAUDE.md`** SecureString discipline rule if `SecureString::Build` is the canonical pattern for multi-piece secret-bearing strings (replacing or extending the existing `Format(prefix, secret, suffix)` mention).

6. **End-to-end live + mock smoke** against each touched cloud path:
   - Google Sheets connection test → `ok=true` (verifies the `X-Goog-Api-Key` header swap doesn't regress).
   - Postgres connection test → `ok=true` (verifies `PQconnectdbParams` migration).
   - OneDrive / Azure Blob / Snowflake / GCS connection tests → `ok=true` (each exercises the new `SecureString&`-output `GetAccessToken` / `GenerateSnowflakeJwt`).
   - Azure Blob Shared Key → `ok=true` (re-verifies `AzureSharedKeySigner::Sign` with the new byte-vector `rawKey`).
   - S3 connection test → `ok=true` (re-verifies cloud `SigV4Signer::Sign` with the new `ScopedSecretBytes` chain).
   - OAuth refresh round-trip (provider with a refresh-token configured, e.g. OneDrive after initial PKCE handshake) → 200 OK from the token endpoint, new access token lands in `m_Tokens`.

7. **All four binary configs build clean**: Studio Debug+Release, Engine Debug+Release.

**Out of scope (architectural floors — outside threat model).**

- **libcurl `curl_slist_append` strdup floor** — libcurl's internal copy of header strings, freed via `free()` (not `explicit_bzero+free`).  Architectural; would require forking libcurl.  Already an expected-residual cell in the 8c audit.
- **libpq internal password copy** — `PQconnectdbParams` copies the password into a `PGconn`-owned buffer that libpq frees via `free()` on `PQfinish`.  Same shape as the libcurl floor.  New expected-residual cell after Part 3.
- **`QueryData::m_Data` request body** — Anthropic prompts and Bedrock SigV4-signed bodies pass through this `std::string` field.  Not strictly a credential, but prompts often carry secret-equivalent material (PII, internal data).  Separate body-path hardening track; not in 8d scope.
- **Audit republish** — per `feedback_audit_republish_end_of_domain`, the `jarvisCpp{CyberSec,Safety}Audit` JCWFs run at domain close-out with Sonnet 4.6, not per-sitting.  8d's "closed five residual leaks" finding will surface naturally in the next end-of-domain audit republish.

**Acceptance.**

- All four binary configs build clean (Studio Debug+Release, Engine Debug+Release); CI matrix (Rocky 9 / Docker amd64+arm64 / macOS / Windows) green.
- 8c heap-scan audit binary: `5 + 2 = 7` must-be-zero scenarios PASS (5 from 8c + cloud SigV4 + Azure SharedKey now flipped to must-be-zero); 1 new must-be-zero scenario (OAuth POST body) PASS; 2 documented architectural floors (libcurl + libpq) — total 8 scenarios, 0 unexpected failures.
- Live connection tests green: Google Sheets, Postgres, OneDrive, Azure Blob, Snowflake, GCS, S3.  OAuth refresh round-trip green for at least one configured OAuth provider.
- `doc/cyber security.md` "Known residual leaks" list reduced from 5 items to 2 items (the architectural floors).  Inline `// known leak, separate fix surface` comments at the 5 fix sites replaced with `// closed in Sitting 8d`.
- `CLAUDE.md` SecureString discipline rule updated if `SecureString::Build` is now part of the canonical pattern.

**Effort.**

Estimated per-part:
- Part 1 (cloud HMAC scratch): 2–3h.  Engine sibling is the line-for-line template.
- Part 2 (Google Sheets header swap): ~30min.  Two-line change × 2 files; existing helper at hand.
- Part 3 (Postgres `PQconnectdbParams`): 2–3h.  libpq API migration + verify all 7 conninfo fields translate cleanly.
- Part 4 (OAuth POST body + `SecureString::Build`): 2–3h.  SecureString extension is the bulk; the body refactor is straightforward once `Build` exists.
- Part 5 (generator out-param sweep): 3–4h.  Two interface changes + 5 caller updates + verify each connection-test path.
- Audit extension + doc sweep + smoke verification: 2h.

**Total ~12–16h** of focused work — chunky sitting on par with 8b.  Order: 2 → 1 → 3 → 5 → 4 (Part 2 first as a 30-min warm-up; Part 4 last because it's the only one that extends `SecureString` itself, so it's cleaner to land the API change alone).  Each part is independent — JC may choose to land 8d as 1 bundled commit, 5 commits (one per part + verification), or split across multiple sittings if scope expands during implementation.

**Cross-reference.**  8b's documented residual-leak inventory: `doc/misc/hand-off.md` 2026-05-22 Sitting 8b entry "Documented residual leaks (deferred, not 8b scope)" + `doc/cyber security.md` "Known residual leaks" subsection.  8c audit baseline: `test/security/heapScan_test.cpp` scenario table + `doc/cyber security.md` "Empirical verification — heap-scan audit (Sitting 8c)" subsection.

---

### Sitting 8e — Close in-house SecureString residuals (post-8d cleanup) — **closed 2026-05-22**

Closed same day it was planned.  All four in-house residuals (R1 + R2 + R3 + R4) are now structurally absent — verified empirically by the existing 8c audit plus a new structural-check scenario (`engineSigV4(no-churn)`) that disables the 64 MiB churn pad and confirms R4 closed the canonical-headers session-token residue STRUCTURALLY rather than via allocator activity.  Result: **10 audit scenarios** (9 must-be-zero + 1 architectural floor); **9 PASS, 0 FAIL, 1 expected-residual (libcurl strdup floor)**.  SigV4 self-test (locked Bedrock KAT signature) still passes — R4's canonical-request refactor is byte-identical to the pre-refactor bytes.  See `doc/misc/hand-off.md` for the close-out entry.



**Problem.**  After 8d closed the five 8b-documented residual surfaces, a post-8d walkthrough of the SecureString-only HTTP path turned up four in-house residuals still bounded by function lifetime — each is "one SecureString-threading layer short of fully closed".  None of them is caught by the current 8c audit (per the post-8d audit-coverage matrix in the 2026-05-22 hand-off entry), but each is structurally present in the code:

1. **OAuth refresh snapshots** (`oauthTokenManager.cpp:262-265`).  `PerformRefresh` materialises `entry.m_ClientSecret` and `entry.m_RefreshToken` into request-scoped `std::string` snapshots (`snapClientSecret`, `snapRefreshToken`) so the network call can run lock-free.  These pass into `RefreshToken(..., string const& clientSecret, string const& refreshToken, ...)`.  The downstream `RefreshResult` struct (the response side) also holds the new tokens as `std::string` until `ApplyRefreshResult` writes them back to the SecureString fields — same residual shape on the response path.
2. **`JwtGenerator::Generate` local `std::string jwt`** (`jwtGenerator.cpp:194-201`).  `Generate` builds the JWT as a local `std::string jwt = signingInput + "." + Base64UrlEncode(signature)` for the existing `SecretRedactor::AddSecret(jwt)` registration, then copies into the SecureString out-param.  The local `std::string jwt` is bounded by `Generate()` lifetime but contains the full signed JWT (an auth-bearing credential until expiry).
3. **`GcsConnector::ExchangeJwtForAccessToken` postBody concat** (`gcsConnector.cpp:241-256` caller + `gcsConnector.cpp:70-87` implementation).  Still takes `std::string const& jwt`; the 8d.5 caller materialises the SecureString JWT into a `std::string(jwt.Get())` at the boundary.  Inside, `std::string postBody = "grant_type=...assertion=" + jwt` is the same shape as the OAuth refresh body was before 8d.4.  Same pattern, different file.
4. **Engine awsSigV4 canonical-headers `std::string`** (`engine/curlWrapper/awsSigV4.cpp:240-249`).  The canonical-request build materialises the STS session token into a `std::string` value inside `headerMap["x-amz-security-token"] = std::string(in.m_SessionToken.Get())` for canonical-request string-mangling.  Pre-existing residual the 8b refactor deliberately deferred; the comment at line 240 documents it as "out of scope here".

Defense in depth, not current vulnerabilities — but the same defense-in-depth argument that motivated 8a–8d applies here.  Closing them brings the SecureString-only HTTP path to genuinely-zero in-house heap residue (only the two architectural floors — libcurl + libpq — remain after 8e).

**Fix.**  Four parts.  Order from smallest to largest: 2 → 3 → 1 → 4.

1. **R1 — OAuth refresh snapshots + RefreshResult → SecureString.**  Widen the request-path threading one layer deeper:
   - `oauthTokenManager.cpp::PerformRefresh` (lines 262-265): change `snapClientSecret` and `snapRefreshToken` from `std::string` to `SecureString`.  `snapTokenEndpoint` and `snapClientId` stay `std::string` — they're non-secret.
   - `oauthTokenManager.cpp::RefreshToken` signature: change `std::string const& clientSecret` and `std::string const& refreshToken` to `SecureString const& clientSecret` and `SecureString const& refreshToken`.  Inside, `CurlEscapedString(curl, refreshToken)` currently takes `std::string const& src` — widen to `std::string_view` (libcurl's `curl_easy_escape` takes `char const*, int`; trivial swap).
   - Update the existing `SecureString::Build` postBody call (from 8d.4) to consume `.view()` from the new SecureString-typed snapshots — `escRefreshToken.view(snapRefreshToken.Get())` instead of `.view(refreshToken)`.
   - `RefreshResult` struct (response side): change `m_AccessToken` and `m_RefreshToken` from `std::string` to `SecureString`.  Touch the parser sites that fill these.  `ApplyRefreshResult` becomes `outField.Set(result.m_AccessToken.Get())` — no `std::string` materialisation.
   - `SecretRedactor::Get().AddSecret(...)` call sites in `oauthTokenManager.cpp:343-344` continue to work — `AddSecret` already takes `std::string_view`, so `.Get()` views feed directly without materialising.

2. **R2 — `JwtGenerator::Generate` local std::string → `SecureString::Build`.**
   - `jwtGenerator.cpp:194-201`: replace `std::string jwt = signingInput + "." + Base64UrlEncode(signature); SecretRedactor::Get().AddSecret(jwt); outJwt.Set(jwt);` with `outJwt.Build({signingInput, ".", Base64UrlEncode(signature)}); SecretRedactor::Get().AddSecret(outJwt.Get());` — same registration semantics, no local `std::string` heap allocation holding the JWT bytes.
   - Verify `Base64UrlEncode` returns `std::string` (it does, and the result is non-secret derived material).  If a future refactor decides that the Base64-encoded signature itself is secret-bearing enough to mlock, that's a separate concern outside 8e scope.

3. **R3 — `GcsConnector::ExchangeJwtForAccessToken` postBody → `SecureString::Build`.**
   - Header: change `static bool ExchangeJwtForAccessToken(std::string const& jwt, std::string const& endpoint, std::string& accessToken, std::string& errorMessage)` → `(SecureString const& jwt, std::string const& endpoint, SecureString& accessToken, std::string& errorMessage)`.  Both the input JWT and the output access token become SecureString.
   - Implementation: drop the early-return `accessToken = jwt` branch's `std::string(jwt.Get())` — write directly via `accessToken.Set(jwt.Get())`.
   - Build `postBody` as a `SecureString` via `Build({"grant_type=...assertion=", jwt.Get()})` — same pattern as the 8d.4 OAuth refresh body.  Hand `postBody.CStr()` + `postBody.Size()` to `CURLOPT_POSTFIELDS` + `CURLOPT_POSTFIELDSIZE`.
   - Update gcsConnector caller (`gcsConnector.cpp:241-256`): drop the `std::string(jwt.Get())` materialisation; pass the SecureString jwt directly.  Caller's `accessToken` local changes from `std::string` to `SecureString`; the token cache (`m_TokenCache`'s `CachedToken::m_AccessToken`) likewise becomes `SecureString` — touches `gcsConnector.h::CachedToken` definition + 1-2 cache-access sites.
   - The final `credentials.m_Token.Set(accessToken)` becomes `credentials.m_Token.Set(accessToken.Get())` — same shape, different argument type.

4. **R4 — Engine awsSigV4 canonical-headers session-token → ScopedSecretBytes.**  The biggest of the four parts; non-trivial canonical-request refactor.
   - **Problem.**  Canonical-request assembly today uses `std::map<std::string, std::string> headerMap` keyed by lowercase header name, value-stringified.  `headerMap["x-amz-security-token"] = std::string(in.m_SessionToken.Get())` is the leak — the std::string sits in the map until the canonical-request build completes.
   - **Approach.**  Split the canonical-request build into two passes:
     - Pass 1: build a sorted list of `(header_name, header_value_kind)` where `value_kind` is either a `std::string_view` into a non-secret buffer (host, x-amz-date, x-amz-content-sha256) or a `std::string_view` into a caller-provided `ScopedSecretBytes` (the session token, when present).  Keep the secret in a `ScopedSecretBytes` for the duration of the Sign() call.
     - Pass 2: concatenate the canonical-headers string + signed-headers list from the sorted pairs.  Canonical-headers itself materialises into a `std::string` that contains the session-token bytes (`x-amz-security-token:<token>\n`).  Mitigation: keep this `std::string` as a tightly-scoped local; once `Sha256Hex(canonicalRequest)` runs the bytes are no longer needed and the local can be `OPENSSL_cleanse`'d before scope exit.  Alternatively, wrap the canonical-request string in a SecureString built via `SecureString::Build({...sorted pieces...})`.
   - **Effort risk.**  This is the largest 8e item.  If the refactor balloons during implementation (e.g. canonical-query reuse + UriEncode interplay), 8e may need to split into 8e.1 (R1+R2+R3, ~6h) and 8e.2 (R4 + audit + docs, ~5-6h).  Decide during implementation.
   - **Audit scenario tweak.**  `ScenarioEngineSigV4` currently plants distinct needles in `m_SecretKey` (XOR-shifted) and `m_SessionToken`.  Post-R4, the deep-scan should report 0/0 even WITHOUT the churn pad (i.e. structurally clean) — toggle the churn off briefly during R4 verification to confirm no residue depends on allocator activity.

**Verification.**  After all four parts land, the 8c audit gets four new scenarios + a churn-off variant:

1. **`ScenarioOAuthRefresh` (NEW, must-be-zero)** — exercises `PerformRefresh`'s snapshot path with a planted nonce in `entry.m_ClientSecret`.  Hits `RefreshToken(...)` with a fake unreachable endpoint (curl fails fast with `CURLE_COULDNT_RESOLVE_HOST`); scans after the SecureString snapshots destruct.  Expected: 0 / 0.
2. **`ScenarioJwtGenerate` (NEW, must-be-zero)** — calls `JwtGenerator::Generate(payloadJson, testRsaPrivateKeyPem, outJwt, errMsg)` with a synthesised RSA test key (or skip if key-generation overhead is too high — alternative: plant the nonce in payloadJson which Generate concatenates verbatim into the signing input).  Scans after outJwt destructs.  Expected: 0 / 0.
3. **`ScenarioExchangeJwt` (NEW, must-be-zero)** — calls `GcsConnector::ExchangeJwtForAccessToken(jwt, fakeEndpoint, outAccessToken, err)` with a planted nonce in `jwt`.  Endpoint is deliberately fake (curl fails fast); scans after the SecureString jwt + outAccessToken destruct.  Expected: 0 / 0.
4. **`ScenarioEngineSigV4StructuralCheck` (NEW, must-be-zero, no churn pad)** — same as the existing `ScenarioEngineSigV4` but skips the 64 MiB churn pad call.  Plants a nonce in `m_SessionToken` only; scans `[heap]` immediately.  Expected: 0 / 0 post-R4 (where today the residue would only be neutralised by the churn).  Toggle implemented via an audit-internal flag passed to `RunScenario`; doesn't change behaviour for the other scenarios.
5. **Re-run** `premake5 gmake --clang --heapscan && make config=debug && ./bin/Debug/jarvisAgent-studio`.  Expected: **12 scenarios total** — 11 must-be-zero PASS + 1 architectural floor (libcurl `AppendSecretHeader`).
6. **Update `doc/cyber security.md`** "Empirical verification — heap-scan audit (Sittings 8c + 8d + 8e)" sub-section: scenario table grows from 9 rows to 12 rows; result snapshot updated; "Architectural residue floors" section unchanged (still 2 floors); inline `// known residual` comments at the 4 fix sites (R1-R4) replaced with `// closed in Sitting 8e`.
7. **Update `CLAUDE.md`** SecureString discipline rule if any new canonical pattern emerges from R4 (e.g. `ScopedSecretBytes` in canonical-headers maps).
8. **End-to-end live smoke** against each touched cloud path:
   - OAuth refresh round-trip (any configured OAuth provider — OneDrive after initial PKCE, Google Sheets via service-account refresh, etc.) → 200 OK from the token endpoint, fresh access_token lands in `m_Tokens`.
   - Snowflake connection test → `ok=true` (verifies `JwtGenerator::Generate` + the new path through R2 + R3).
   - GCS connection test → `ok=true` (verifies the `ExchangeJwtForAccessToken` widening from R3).
   - Bedrock dispatch via `test/dispatch/test_bedrock_sigv4.py` — locked SigV4 KAT signature unchanged (verifies R4 didn't break canonical-request determinism); plus a live Bedrock smoke if STS session tokens are configurable (verifies the X-Amz-Security-Token path under the new ScopedSecretBytes-backed canonical-headers).

**Out of scope (deferred to post-1.0).**

- **`QueryData::m_Data`** — request body is `std::string`.  Anthropic prompts and Bedrock SigV4-signed bodies pass through it.  Not strictly a credential, but prompts often carry secret-equivalent material (PII, internal data).  Separate body-path hardening track.  Decision: post-1.0 — closing requires a wider refactor of every API parser/builder that touches request body bytes, well beyond the credential-leak scope of 8a-8e.

**Architectural floors (unchanged — still outside threat-model boundary).**

- libcurl `curl_slist_append` strdup floor.
- libpq `PQconnectdbParams` internal password copy.

**Acceptance.**

- All four binary configs build clean (Studio Debug+Release, Engine Debug+Release); CI matrix (Rocky 9 / Docker amd64+arm64 / macOS / Windows) green.
- 8c heap-scan audit binary: **11 must-be-zero scenarios PASS** (8 pre-existing from 8d + 3 new from 8e — OAuthRefresh / JwtGenerate / ExchangeJwt) + **1 architectural floor PASS (known residual)** + **1 churn-off structural-check scenario PASS** = 12 audit scenarios, 0 unexpected failures.
- Live smokes green: OAuth refresh round-trip, Snowflake connection test, GCS connection test, Bedrock SigV4 KAT (signature unchanged), optional Bedrock STS smoke.
- `doc/cyber security.md` inline `// known residual` comments at the 4 fix sites (R1-R4) replaced with `// closed in Sitting 8e`.
- `pre-1_0_follow-ups.md` updated: 8e marked closed; remaining-sittings count drops by 1.

**Effort.**

Estimated per-part:
- R1 (OAuth snapshots + RefreshResult): 2-3h.  RefreshResult struct touch is the wildcard.
- R2 (JwtGenerator::Generate local std::string): 1h.  Mechanical Build swap + redactor view-call.
- R3 (ExchangeJwtForAccessToken): 1-2h.  Signature widening + caller cache-type touch + small body refactor.
- R4 (engine awsSigV4 canonical-headers): 3-4h.  Largest item; canonical-request refactor.  Possible split point if scope expands.
- Audit extension (4 new scenarios incl. churn-off variant) + doc sweep + smoke verification: 2-3h.

**Total ~9-13h** of focused work.  Order: R2 → R3 → R1 → R4 (smallest first as warm-up; R4 last because it's the only canonical-request structural change).  Each part is independent — JC may choose to land 8e as 1 bundled commit, 4 commits (one per residual), or split into 8e.1 (R1+R2+R3) + 8e.2 (R4 + verification) if R4 scope expands.

**Cross-reference.**  Post-8d residual inventory: `doc/misc/hand-off.md` 2026-05-22 Sitting 8d entry "Residual hardening tracks" + the audit-coverage matrix in the same entry.  Sitting added mid-plan 2026-05-22 (same pattern as Sittings 15 and 8d were added mid-plan) under the pre-1.0 closing-list policy — items belonging in 1.0 get a sitting slot rather than drifting to post-1.0.

---

### Sitting 9 — `SanitizeUserSlug` collision fix + migration — **closed 2026-05-23**

Closed same day it was planned.  `SanitizeUserSlug` now appends `_<8 hex chars of SHA-256(original_user)>` (body capped at 55 chars; total slug ≤ 64).  Two webServer.cpp authz sites flipped from `SanitizeUserSlug(auth.m_User) != info->m_OwnerSlug` to `auth.m_User != info->m_User` — authz is now on user identity, slug is a filesystem-naming primitive only.  `ReadMeta` backfill switched to deriving from `folder.parent_path().filename()` (the canonical truth for legacy folders).  As a horizontal sweep, the two existing file-local `Sha256Hex` copies (`engine/curlWrapper/awsSigV4.cpp` + `application/web/mcpKeyManager.cpp`) consolidated into `engine/auxiliary/sha256.{h,cpp}` ahead of the third site appearing (per `feedback_cpp_discipline`).  Live e2e collision-repro test at `test/security/test_adhoc_user_slug_collision.py` provisions two MCP keys with users that collapse to the same body (`bob+admin@x` / `bob_admin@x`) and verifies: distinct on-disk slug dirs, distinct hex suffixes, identical bodies pre-suffix (proving the suffix is what separates them), cross-user 403 + `not_owner`, admin 200 + `admin_cross_user_read` INFO audit line, self-access 200.  14/14 checks PASS.  All 4 binary configs build clean; existing adhoc empty-body regression test still passes; `inputResolutionTest` non-adhoc workflow passes.  See `doc/misc/hand-off.md` 2026-05-23 entry.

---

### Sitting 10 — `ConfigParser` 36-field boilerplate refactor — **closed 2026-05-23**

Closed same day it was planned.  Six file-local helpers (one more than the planned four — `ParseUint64Field` and `ParseStringFieldLogOnly` came out of enumerating the actual sites: `MaxRequestBodyMB` and `max_context_tokens` need uint64, `description` + `author` log only without storing).  `NumericPolicy` enum (`AcceptAny` / `RejectNegative` / `ClampNegativeToZero` / `StoreOnlyIfPositive`) captures the per-site post-extract checks declaratively at the call site.  29 of 38 field sites became one-line calls; 9 outliers stay inline with reasons documented at each site (file format identifier counter-only, API interfaces array dispatch, use_bash platform-conditional log, max_context_tokens / default_output_tokens silent-on-failure, rate_limit nested object, API enum-mapping with "Test" migration error, is_mock / fixture_path with different error log shape).  Helpers take a raw `uint32_t*` counter pointer (nullable) rather than the private `ConfigFields` enum so they live in the anonymous namespace without needing `friend ConfigParser`.  Byte-identical behaviour verified two ways: (a) happy-path ConfigParser-startup log diff produced ZERO lines aside from timestamps; (b) `test/config/fixtures/malformed_types.json` produced 23 ERROR lines + `test/config/fixtures/out_of_range.json` produced 2 WARN lines + 2 INFO lines — every line matched the pre-refactor format exactly (`ConfigParser: 'description' must be a string`, `port 99999 out of range [0, 65535], defaulting to 0 (auto)`, `session_timeout_hours 500 out of range [1, 168], defaulting to 8`).  4 fixture files left under `test/config/fixtures/` for Sitting 11 to wire into a proper Python test harness.  All 4 binary configs build clean; Sitting 9 collision repro + adhoc envelope reject + 2 non-adhoc workflows (inputResolutionTest, make-example) all pass.  See `doc/misc/hand-off.md` 2026-05-23 entry.

---

### Sitting 11 — Verification: malformed config.json tests + D1 negative-path fixtures — **closed 2026-05-23**

Closed same day it was planned.  Two new test files landed:

- `test/config/test_malformed_configs.py` — subprocess harness that spawns the engine binary in `/tmp/claude/j9t-cfg-sandbox/<fixture>/` against each fixture under `test/config/fixtures/*.json`.  Verifies expected ERROR/WARN substrings appear in the sandboxed `log/log.txt` AND that the engine either stays alive or exits cleanly (rc ≥ 0; negative rc would be a signal-driven crash regression).  46 passing checks across 7 fixtures: `malformed_types`, `out_of_range`, `negative_rejected`, `clamped_negatives` (carried over from Sitting 10) + 3 new — `unknown_API` (API="API7" → InvalidAPI + ConfigChecker rejection), `oob_API_index` (API index 50 with 1 interface → ConfigChecker rejection), `url_substring_attack` (URL with embedded `..` stored verbatim by parser — downstream validators are responsible).  Each fixture carries an `_expected_errors` list inside the JSON itself so the harness has a uniform contract.

- `test/hardening/test_negative_paths.py` — REST-driven against the running j9t (https://localhost:8443).  28 passing checks across 4 groups: **Group 1** size caps (adhoc JCWF > 4 MB → 400 `stage_failed` + `jcwf_too_large` in message); **Group 2** path-traversal on three JCWF surfaces (absolute cntx_files path → 400 at parse, workflow id containing `../` → ≥ 400, file_watch trigger path = `/etc/...` → trigger silently dropped at registration); **Group 3** concurrency (8-way parallel reload+list completes in < 1 s — no deadlock, ai_calls_inflight returns to baseline after batch — no leak); **Group 4** db_query caps against `local-pg` (output_file path-separator rejection, max_rows=5 with 100-row query → "exceeds max_rows=5", statement_timeout_ms=100 + pg_sleep(2) → "canceling statement due to statement timeout").  Group 4 uses a connection-breaker warmup helper (`/api/connections/local-pg/test`) to neutralise the per-connection circuit breaker that counts every failed task — including expected-app-level cap rejections — against the connection.

Out-of-scope-deferred to **Sitting 16** (not cleanly REST-driveable from a shared host instance): polarion WriteItemFile path-traversal (needs configured polarion fixture), AI output 64 KB size cap (internal-truncation not REST-observable), reaper CV wake-on-stop (shutdown-timing test that requires a sandboxed-spawn approach — could be added as a perf-only check using the malformed-config harness's spawn infrastructure).  Each deferred item now has a Sitting 16 entry.

**Mid-sitting Sitting 9 regression fix:** `test_auth_mcp.py::test_adhoc_folder_namespace` had a literal `f"/_adhoc/{user}/"` substring assertion that didn't accommodate the new `_<8hex>` slug suffix from Sitting 9.  Patched to `f"/_adhoc/{user}_"` (substring without trailing slash, since the next character is the hash-suffix separator).  Test suite back to 100/100.  Surfaced because Sitting 11's prep work ran the full test_auth_mcp suite for the first time post-Sitting-9 — Sitting 9's verification only ran the new collision-repro test + two dispatch regressions, violating `feedback_thorough_testing`.

**Sitting 16 grew by 3 items** during this sitting (the three deferred D1 items above).  Basket now carries 8 open items.  See `doc/misc/hand-off.md` 2026-05-23 entry.

---

### Sitting 12 — Cloud tail: persist `email_watch` watermark across restart — **closed 2026-05-23**

Closed same day it was planned.  `<queue_folder>/.email_watermarks.json` (single file, all triggers keyed by `<workflowId>|<triggerId>`).  `TriggerEngine` constructor loads at startup; `AddEmailWatchTrigger` restores the persisted UID on a key match; post-poll update site mirrors into the persisted map and atomic-writes the file via `EngineCore::AtomicWriteFile`.  Live end-to-end verified for load + restore; save path exercised by code review (mirror of load — same map, same JSON shape, same atomic-write helper).  Format: `{"format_version": 1, "watermarks": [{"workflow_id": ..., "trigger_id": ..., "connection_name": ..., "folder": ..., "last_seen_uid": ..., "updated_at": ...}, ...]}`.  Doc updated: `doc/cloud-integration.md` "email_watch Trigger" gained a one-line bullet about the persistence.  See `doc/misc/hand-off.md` 2026-05-23 entry.

---

### Sitting 13 — `tools/replayTranscript.py` — **cancelled 2026-05-23**

JC cancelled — feature not needed.  Trigger condition was "first real worked-yesterday-broke-today report"; the operational need hasn't materialised and a speculatively-built debugging tool would carry maintenance cost without proven demand.  Slot retired; numbering preserved.

---

### Sitting 14 — KeyManager hardening tail (4 small items) — **closed 2026-05-23**

Closed same day it was planned.  All four items landed: SetDefaultProvider empty/unknown rejection + ClearDefaultProvider separator; Unlock TOCTOU closed via dedicated path mutex; LoadPlaintext/SavePlaintext gated by `#ifdef J9T_STUDIO` (Engine binaries verifiably strip the symbols via `nm`); 4 MB file-size + 1024 provider-count caps in the parse path.  Behaviour neutral for legitimate keystores; defensive against tampered ones.  See `doc/misc/hand-off.md` 2026-05-23 entry.

---

### Sitting 15 — Plain-HTTP loopback policy + credentialed-HTTP refusal — **closed 2026-05-23**

Closed same day it was planned.  All three parts landed: (A) loopback-only `http://` allowlist + (B) credentialed-plaintext refusal + (C) dashboard pill + banner.  See `doc/misc/hand-off.md` 2026-05-23 Sitting 15 entry for the close-out detail.



**Problem.** The existing SSRF gates apply to outbound HTTP from workflow context (`workflowRuntimeManager.cpp::IsCallbackUrlAllowed`, `application/cloud/connectorHttp.cpp::ValidatePublicHttpEndpoint`) — neither gates the AI-dispatch URL set via `config.json::api_interfaces[].url`. A local-LLM interface (`http://localhost:11434/v1/chat/completions` for ollama / llama.cpp / vLLM) dispatches fine, which is the correct outcome for loopback-only test backends (qwen-7b validated as test AI on 2026-05-21). But the same path silently accepts `http://internal-llm.example.com/...` — a LAN-or-public plaintext endpoint that would carry prompts (potentially containing secrets via template substitution + dataflow propagation) over the wire in clear. And nothing currently prevents an interface from carrying a `key_name` whose resolved Bearer token would then go over plain HTTP.

j9t's cyber-security posture rejects plain HTTP everywhere else (callback URLs, cloud connectors). Making the AI-dispatch surface consistent unblocks "use ollama qwen-7b as the default AI" — currently the operator must accept a plain-HTTP default that has no defense-in-depth around what URLs that label can be pointed at, and no protection against a credentialed `http://` footgun.

**Fix (three parts, one sitting).**

**Part A — Loopback-only `http://` allowlist.**
1. New helper next to existing SSRF gates: `application/network/urlPolicy.{h,cpp}` (new module, low file count) exporting `[[nodiscard]] std::expected<void, UrlPolicyError> ValidateAiInterfaceUrl(std::string const& url)`. Reuses the resolved-address classifier already in `IsCallbackUrlAllowed` — `getaddrinfo` + per-address loopback check (`127.0.0.0/8`, `::1`, with IPv4-mapped-IPv6 unwrap). `https://` URLs pass without further checks; `http://` URLs pass only if EVERY resolved address is loopback; everything else (`ws://`, `file://`, malformed) is rejected.
2. `UrlPolicyError` is a small typed-error enum following the established pattern (`ConnectorError` / `ParserError` / `RegistryError`): `Code` enum (`SchemeRejected`, `NonLoopbackHttp`, `UnresolvedHost`, `MalformedUrl`) + `m_Details` string. No `default:` arm — `-Wswitch` is the enforcement.
3. Hook into two enforcement points:
   - **Config-load time** (`engine/json/configParser.cpp::ParseInterfaces`): validate each interface's URL. On rejection, `LOG_CORE_ERROR("ParseInterfaces: rejected interface '{}' url='{}': {}", name, url, Describe(err.m_Code))` and skip the entry — don't load it into `m_ApiInterfaces` (fail-closed; the system has fewer interfaces, but every loaded one is policy-compliant).
   - **REST POST/PUT time** (`application/web/webServer.cpp::HandleAiInterfacePost` / `HandleAiInterfacePut`): same validator. On rejection, HTTP 400 with `code: "url_policy_violation"`, `message: "<Describe(code)>: <m_Details>"`. Test handle also rejects (`HandleAiInterfaceTestPost`) so an operator can't probe a forbidden endpoint by misusing the Test button.
4. Audit log per-dispatch: `AiRequestPool::Submit` emits ONE entry per successfully dispatched plain-HTTP request: `LOG_SECURITY_INFO("ai_dispatch_plaintext_http host='{}' run='{}' workflow='{}' task='{}' interface='{}'", host, runId, workflowId, taskId, interfaceName)`. Goes through the existing `[Security]` channel so it's grep-able in audit reviews. INFO-level — this is "noted-and-allowed", not an error.

**Part B — Reject credentialed plain HTTP.**
1. In the same `ParseInterfaces` and `HandleAiInterfacePost/Put` validators: if interface URL is `http://` AND `key_name` is non-empty, reject with `UrlPolicyError::Code::CredentialedPlaintextHttp`. Local ollama doesn't require authentication (loopback); the only reason to attach a `key_name` to a plain-HTTP interface is operator confusion — likely copy-pasted from a cloud interface — and that confusion would silently leak the Bearer token in transit on every request.
2. REST response: HTTP 400 with `code: "credentialed_plaintext_http"`, `message: "interfaces with a key_name require https:// — plain http would expose the credential in transit"`.
3. Audit log entry at rejection: `LOG_SECURITY_INFO("credentialed_plaintext_http_rejected interface='{}' key_name='{}'", interfaceName, keyName)`. Counter on `/api/debug/signals`: `credentialed_plaintext_http_rejections`.

**Part C — Dashboard surface (small UI touch).**
1. AI Interfaces panel in `dashboard/ui/src/views/AiInterfacesView.tsx` (or its current home — confirm at start of sitting): yellow "loopback HTTP" pill next to interfaces whose URL is `http://`, similar to the existing "mock" / "default" pills.
2. AI Health card on the main dashboard: when the configured default (`api_index`) resolves to a plain-HTTP interface, add a one-line yellow banner: "Default AI uses plaintext loopback — fine for local LLMs, never use for remote backends." Not dismissible (security signal, not info popup); occupies one row of the AI Health card so it doesn't push other content around.

**Acceptance.**
- New `test/security/test_url_policy.py` (`test/security/` may need creation):
  - POST `http://example.com/v1/chat` → HTTP 400 + `code: "url_policy_violation"` + `dns_resolved_ip_rejections` counter increments.
  - POST `http://192.168.1.5/v1/chat` → HTTP 400 + `code: "url_policy_violation"`.
  - POST `http://localhost:11434/v1/chat` (no key_name) → HTTP 201 + interface loaded.
  - POST `http://localhost:11434/v1/chat` WITH `key_name: "ollama"` → HTTP 400 + `code: "credentialed_plaintext_http"`.
  - POST `https://api.openai.com/v1/chat/completions` with `key_name: "openai"` → success (existing path unchanged — verify no regression).
  - POST `http://[::1]:11434/v1/chat` → success (IPv6 loopback).
  - Malformed URLs (`htp://...`, `ws://...`, empty) → HTTP 400 + `code: "url_policy_violation"`.
- Config-fixture test: drop a `config.json` carrying `http://192.168.1.5/...` into a test working dir, launch j9t headless, verify (a) ERROR log line at load with the offending host + interface name, (b) interface omitted from `GET /api/settings/ai-interfaces`, (c) j9t alive and accepting requests on other interfaces.
- `jarvisCppDocu` end-to-end on `http://localhost:11434` qwen-7b interface still completes successfully (regression check — the legitimate loopback path must not break).
- Build all 5 targets clean.
- `feedback_no_legacy.md` applies: no "allow non-loopback http" escape hatch (no flag, no env var). The one and only safe `http://` case is loopback.

**Doc updates this sitting closes out.**
- `doc/cyber security.md` — new subsection "Plain-HTTP policy" describing the loopback-only allowlist + credentialed-HTTP refusal, with the rationale (defense-in-depth, audit posture) and reference impl pointers.
- `CLAUDE.md` discipline rules — add bullet: "**Plain `http://` is loopback-only and never with `key_name`.** Reference impl: `application/network/urlPolicy.h::ValidateAiInterfaceUrl` + the two enforcement hooks in `configParser.cpp::ParseInterfaces` and `webServer.cpp::HandleAiInterfacePost/Put`. Cross-ref the SSRF gate cluster (`IsCallbackUrlAllowed`, `ValidatePublicHttpEndpoint`) that this completes."
- `doc/jarvisagent.md` — note in the interface-configuration section that `http://` is loopback-only; cite the example use case (local ollama / llama.cpp).
- `doc/api-endpoints.md` — document the new `url_policy_violation` and `credentialed_plaintext_http` error codes on `POST /api/settings/ai-interfaces` + `PUT /api/settings/ai-interfaces/<id>` + `POST /api/settings/ai-interfaces/<id>/test`.

**Effort.** Medium (~1 day).
- Part A: ~3 hours (helper module + UrlPolicyError + 3 enforcement sites + audit log).
- Part B: ~2 hours (credentialed-HTTP check + error code + counter wiring).
- Part C: ~2 hours (dashboard pill + banner; React-side, no C++).
- Tests + verification: ~2 hours.
- Doc sweep: included in the effort estimate (4 files).

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
7. **Sittings 12 + 14** are the small tail (email watermark, KeyManager hardening; Sitting 13 cancelled). Sitting 14's four KeyManager items could also fold into Sitting 7's API-shape sweep opportunistically (the typed `Error` return on `SetDefaultProvider` / `ParseProvidersJson` lands naturally then) — keep them grouped if Sitting 7 happens close in time, otherwise ship Sitting 14 as a clean standalone.
8. **Sitting 15** is the plain-HTTP loopback policy + credentialed-HTTP refusal (Tier 1 + Tier 2 of the local-LLM cyber-sec follow-ups). Added at JC's direction 2026-05-21 after qwen-7b was validated as a test backend over `http://localhost:11434` — codifies "plain HTTP is only safe when it cannot leave the machine" and "never carry a credential over plaintext", which closes the cyber-sec gap that currently blocks "use ollama as the default AI" without weakening the existing posture. Naturally last among the substantive Pre-1.0 sittings because it depends on `feedback_expected_error_pattern` (the typed-error discipline from Sitting 7) and reuses the address-classification helper that the SSRF gate already exercises.
9. **Sitting 16** is the incidental-findings basket — the LAST sitting before 1.0 tagging.  Collects off-topic items that surface during Sittings 9–15 and the audit/review work afterwards; each item closed individually before tagging.  Doesn't depend on any specific predecessor (items can land here regardless of sequence), but executes after Sitting 15 so it sweeps up anything those sittings turned up.

Sittings 1, 2, 3, 4, 5, 12, 14 are each half-day to one day and can pair into combined sittings if time permits. Sittings 6, 7, 8, 10, 15 are full sittings on their own.

Total estimated effort: ~12–15 working days across the 15 substantive sittings + the Sitting 16 basket pass (variable; depends on what surfaces).

---

### Sitting 16 — Incidental-findings basket

**Purpose.**  Collection point for off-topic findings surfaced during regular sittings — items that don't fit the current sitting's scope but are too small to justify a dedicated sitting of their own, and that we don't want to silently lose track of.

**Workflow.**

- When a finding surfaces mid-sitting, **flag it to JC**: a short paragraph naming the finding + likely pre-1.0 vs post-1.0 classification + recommended action.  Do not silently add it to the current sitting's working tree.
- JC decides per item: (a) append to this sitting's open-items list below for a future cleanup pass, (b) fold into an existing sitting if scope-compatible, or (c) explicitly mark post-1.0.
- Items in the open-items list are tackled in a single pass near the end of the pre-1.0 closeout (after Sitting 15) — the basket is the LAST stop before 1.0 tagging.  If an item turns out to be urgent enough that "near the end" is too late, promote it to its own sitting at decision time.
- Items closed during the basket pass are crossed off below with a `(closed)` note; items deferred post-1.0 are crossed off with a `(post-1.0)` note + a brief rationale.

**Per-item shape.**  Each entry follows a compressed version of the regular sitting shape:

```
N. **Title** — short prose.
   - **Why it matters:** one sentence.
   - **Fix sketch:** one or two sentences.
   - **Effort:** small / small-medium / medium.
   - **Status:** open / closed (N sittings ago) / post-1.0 (rationale).
```

**Open items (initial seed; grow as findings surface).**

1. **Two `SigV4Signer` classes in `AIAssistant` namespace** — one in `engine/curlWrapper/awsSigV4.{h,cpp}` (Bedrock AI dispatch path), one in `application/cloud/sigV4Signer.{h,cpp}` (S3 connector path).  They link cleanly because no TU includes both headers, but the canonical-request logic is duplicated and the heap-scan audit had to split into a separate TU (`test/security/heapScan_cloud_scenarios.cpp`) precisely because of this.
   - **Why it matters:** Two parallel canonical-request implementations are a guaranteed drift surface — a future fix to one would need to be ported to the other, and the audit TU split is permanent maintenance overhead.
   - **Fix sketch:** Promote `engine/curlWrapper/awsSigV4.{h,cpp}` as the keeper (already used by both AI dispatch and the S3 connector path could route through it).  Delete `application/cloud/sigV4Signer.{h,cpp}`.  Update `s3Connector.cpp` + `s3CloudTaskExecutor.cpp` + `heapScan_cloud_scenarios.cpp` to use the engine signer.  Audit can then collapse `heapScan_cloud_scenarios.cpp` back into `heapScan_test.cpp`.
   - **Effort:** medium.
   - **Status:** open.

2. **`OAuthTokenManager::StoreTokens` takes `std::string const&` for three secret-bearing params** — `accessToken`, `refreshToken`, `clientSecret`.  Called once per OAuth-consent-completion handshake to seed `m_Tokens`; the entry's SecureString fields then take ownership via `.Set(view)`.
   - **Why it matters:** Same SecureString-threading opportunity as `PerformRefresh`'s prior snapshot leak.  The OAuth handler that calls `StoreTokens` receives the tokens from the consent flow as `std::string` (parsed out of the provider's JSON response), so the std::string materialisation is one layer further upstream than the snapshot was — but the function-boundary fix is the same shape.
   - **Fix sketch:** Widen the three secret params to `SecureString const&`.  Update the consent-handler caller (`webServer_studio.cpp` OAuth callback handler) to parse into a SecureString instead of `std::string`.  Same pattern as the OAuth refresh response parsing.
   - **Effort:** small-medium.
   - **Status:** open.

3. **Engine `awsSigV4.cpp::HmacSha256` `data` parameter is `std::string const&`** — not a leak (the data passed in is signed-not-secret material like the canonical-request string or the stringToSign), but inconsistent with the cloud-side sibling which takes `std::string_view`.  Small unification.
   - **Why it matters:** Minor consistency / maintenance.  Not a security issue.
   - **Fix sketch:** Widen the `data` param to `std::string_view`.  Call sites (the kDate/kRegion/kService/kSigning chain and the final signature) all pass `std::string` arguments that convert implicitly.
   - **Effort:** small.
   - **Status:** open.

4. **MCP-bridge heartbeat traps locked-keystore startup into auto-lockout** — the standalone node MCP bridge (`mcp/dist/index.js`) sends `POST /api/mcp/heartbeat` every 15 s with its stored Bearer MCP key.  When j9t starts with a locked keystore (the normal startup state — master password must be entered each time), the MCP-key cache is empty (keys are AES-encrypted with the master password) and every heartbeat fails with `mcp_auth_failure reason=invalid_key`.  10 failures in 5 min → automatic 15-min IP lockout.  Net effect: the admin has ~150 s after starting j9t to unlock the keystore before the system locks itself out for 15 min — and during that window REST shutdown is also blocked, so recovery requires SIGTERM.  Surfaced during the post-Sitting-8e regression-test build cycle when consecutive rebuilds + restarts exceeded the unlock-grace window.
   - **Why it matters:** Self-DoS during a routine workflow (any restart: code change, machine reboot, crash recovery).  The "10 failures" threshold is sized for genuine bruteforce, not for trusted-bridge polling against a known-not-yet-ready system state.  Security tradeoff is favourable: no real secrets are accessible during the locked window (the keys themselves aren't decoded yet), and the bridge is the local in-deployment process, not an external attacker.
   - **Fix sketch:** j9t-side is preferred.  In the heartbeat-handler auth gate, when the keystore is locked (`!m_KeyManager.IsUnlocked()`), short-circuit before incrementing the per-IP failure counter — return `423 Locked` (or `503 Service Unavailable`) with body `{"error":"keystore_locked","message":"keystore not yet unlocked"}` and DO NOT count this against the lockout window.  The bearer-key validation never runs because the cache it would check against is empty by construction.  Bridge-side hardening (poll `/api/status::keys_unlocked` first, skip heartbeat until true) is optional defence in depth.  Touch sites: `application/web/webServer.cpp` heartbeat handler + the per-IP lockout counter increment site in the auth gate.  Verify by restarting j9t and confirming the security log shows no `mcp_auth_failure` lines during the locked window (only after unlock).
   - **Effort:** small-medium.
   - **Status:** open.

5. **Polarion `WriteItemFile` / `WriteAttachmentFile` path-traversal coverage** — Sitting 11 (D1 hardening pass) deferred this because polarion fixtures aren't trivially driveable from the host's running j9t.  `polarionClient.cpp:669` (WriteItemFile) and `:893` (WriteAttachmentFile) both go through `ConfineUnderProjectRoot`, but neither has a REST-test that exercises the rejection branch against an out-of-root path.
   - **Why it matters:** completeness of the D1 hardening test matrix.  The existing `IsValidFilesystemId` (polarionClient.cpp:64-80) tests the id-shape allowlist but not the post-resolve path containment.  A test gap means a future regression in the containment gate would slip through CI.
   - **Fix sketch:** Configure a mock polarion endpoint (the existing `api6` mock-infrastructure pattern is the closest match — see `application/cloud/polarionClient.cpp::PolarionClient::IsValidFilesystemId` + `test/dispatch/test_api6_mock_errors.py`).  Submit a JCWF whose polarion-filter `output_path` resolves outside the project root.  Assert task fails + log line fires + escape file absent.
   - **Effort:** medium (depends on mock-infrastructure setup; if a usable mock exists for polarion already this is small).
   - **Status:** open.

6. **AI output 64 KB size cap (kMaxOutputBytes) coverage** — Sitting 11 deferred because the cap applies to output file content read AFTER an AI call completes (workflowRuntimeManager.cpp:764-785), and the truncation is internal — not REST-observable as a rejection.
   - **Why it matters:** completeness.  Internal truncation has a behaviour contract (cap at 64 KB) that no test exercises today.  A regression that raises/lowers the cap would go undetected.
   - **Fix sketch:** Write a Python task that emits > 64 KB to its output file (e.g. `print('A' * 100000)`).  Submit via adhoc, observe the truncation behaviour via the run's task output (read via `GET /api/workflow-runs/<id>/files/<output-file>` and assert length ≤ 65536).
   - **Effort:** small (~1h once a Python-output JCWF skeleton is in place).
   - **Status:** open.

7. **Reaper CV wake-on-stop shutdown-timing test** — Sitting 11 deferred because the test requires a controlled shutdown of a j9t instance and the host's running j9t can't be hit for this kind of timing measurement without disturbing other tests.
   - **Why it matters:** the reaper-thread CV fix (sittings prior) reduced shutdown-blocking from up to 60 s to near-zero.  A regression that re-introduces a `sleep_for(60s)` would silently undo that — currently only catchable by manual observation.
   - **Fix sketch:** Reuse Sitting 11's `test_malformed_configs.py` sandbox-spawn harness (subprocess + sandbox dir at `/tmp/claude/j9t-cfg-sandbox/`).  After the spawned j9t prints `LiveTransport: curl multi handle initialised` (server-up signal), POST `/api/shutdown` and measure the time-to-process-exit; assert < 5 s.  Mark non-merge-gating (perf check) so a flaky CI environment doesn't trip it.
   - **Effort:** small-medium (~2h; the harness already exists).
   - **Status:** open.

8. **Circuit breaker counts expected app-level failures as "connection failures"** — `CloudCircuitBreaker::RecordFailure` is called by every task-level rejection in the cloud-connector / db_query path, including expected-app-level outcomes like "Result set has 100 rows, exceeds max_rows=5" (db_query cap) or "exceeds statement_timeout".  Five consecutive such rejections trip the breaker open and subsequent requests short-circuit with `"Cloud connection 'X' circuit breaker is open"` instead of reaching the actual database.  Surfaced by Sitting 11's Group 4 hardening tests — they need a `_warm_up_connection` helper that calls `POST /api/connections/<name>/test` before each cap-rejection assertion, otherwise consecutive tests trip the breaker open and the assertions short-circuit.
   - **Why it matters:** Operational UX risk in production.  A user running a `db_query` workflow that legitimately hits the `max_rows` cap five times in a row would lock themselves out of the connection — the breaker thinks the connection is unhealthy when in fact the queries reached the database successfully and were rejected at the app layer.  Workaround (manual `POST /api/connections/<name>/test` reset) is operator-visible friction that shouldn't be needed for application-layer caps.
   - **Fix sketch:** Add a failure-class enum to `CloudCircuitBreaker::RecordFailure(ConnectorErrorCode)` (already takes the optional `ConnectorErrorCode` per Sitting 7b) — count `NetworkError` / `AuthFailure` / `CredentialMissing` etc. as connection failures, but treat `ValueOutOfRange` (the row/byte/timeout cap class) as a non-connection-failure that doesn't decrement the breaker's health budget.  Test sites: `dbQueryCloudTaskExecutor.cpp` row/byte/timeout cap branches, anywhere else that reports a known-good-connection app-level rejection.
   - **Effort:** small-medium (~half day).  The plumbing is already there; the change is in the breaker's policy on which `ConnectorErrorCode` values count.
   - **Status:** open.

9. **`AdhocWorkflowManager::ReadMeta` uses hand-rolled JSON `pluck` instead of simdjson** — `adhocWorkflowManager.cpp:373-394` extracts four fields from `meta.json` via `std::string::find`-based substring scans without JSON-unescape.  Theoretical-only weakness today (the corresponding `WriteMeta` uses `JsonHelper::EscapeJsonString` so escapes are introduced symmetrically, and the four field values are all upstream-validated by McpKeyManager / the policy whitelist), but it's a hand-rolled JSON parser in a security-critical attribution path — violates `feedback_simdjson_only`.  Surfaced during the Sitting 9 doc sweep on 2026-05-23.

10. **email_watch save-path end-to-end test** — the load + restore paths were verified live (planted sentinel → restart → load INFO + restore INFO fires) but the save path (poll completes → `.email_watermarks.json` atomic-rewritten with the new UID) was code-review only because the host's `my-email` connection has an empty endpoint; the IMAP target validator rejects before any successful poll fires the save.
   - **Why it matters:** Save is the mirror of load (same map + same JSON shape + `EngineCore::AtomicWriteFile`), but "mirror" is a code-review claim not a runtime claim — a future refactor that touches one without the other could silently break the round-trip and no test would catch it.
   - **Fix sketch:** Reuse Sitting 11's sandbox-spawn harness pattern; spin up mailpit (or a minimal IMAP mock) on a host:port that the connection validator accepts; submit a polling cycle; verify the watermark file gets atomic-rewritten with the expected UID.  Alternative: extract the save body into a testable static helper an in-process test can drive without going through the IMAP path.
   - **Effort:** small-medium (~half day; the IMAP-mock setup is the bulk).
   - **Status:** open.

11. **KeyManager hardening synthetic tests not written** — Sitting 14's plan listed two acceptance-criteria tests that were code-review verified only: (a) empty-string `SetDefaultProvider` → rejection + WARN log, (b) malformed 100k-provider keystore → parser rejection + LOG_CORE_ERROR.
   - **Why it matters:** Both are defensive paths that protect against tampering or programmer error.  A regression that softens either check — someone re-introduces the empty-name silent-clear, or raises `kMaxProviders` thinking the cap is a guideline — would go undetected.  The empirical `nm`-based verification of the J9T_STUDIO build-guard (item d in the original plan) DID land; these two didn't.
   - **Fix sketch:** Two small tests under `test/security/`: (a) synthesise a > 4 MB plaintext `keys.json` into a `/tmp/claude/j9t-...` sandbox dir + spawn studio binary + assert LOG_CORE_ERROR with `kMaxKeysFileBytes` substring + assert engine exits cleanly (rc ≥ 0).  (b) Empty-name needs harness work since the lone REST caller (`HandleProviderSetDefaultPost`) pre-filters via `HasCredential`; either expose a test-only endpoint or write a small C++ test TU that links against `engine/keys/keyManager.{h,cpp}` directly.
   - **Effort:** small-medium (~half day; (a) is straightforward sandbox-spawn, (b) needs harness work).
   - **Status:** open.
   - **Why it matters:** Consistency + defence in depth.  A future field added with characters that need JSON-escape (`"`, `\\`, control chars) would silently round-trip incorrectly on read; an attacker who somehow plants a hand-crafted `meta.json` could fool the reader into mis-attributing a run.  Both paths require additional bugs to be exploitable, but `feedback_simdjson_only` exists precisely so we don't depend on those bugs not happening.
   - **Fix sketch:** Rewrite `ReadMeta` to parse the file with simdjson DOM (same pattern as `AdhocWorkflowManager::RewriteWorkflowId` already uses in the same file — `dom::parser parser; parser.parse(content).get(root)`; then `root["user"].get_string()`, etc.).  Keep the parent-dir-name fallback for legacy meta.json files missing `owner_slug`.  Drop the `pluck` lambda + four `pluck("...")` calls.  Effort: small (~1h).
   - **Effort:** small.
   - **Status:** open.

**Acceptance** (when the basket pass happens):

- Each item closed: build clean across all 4 binary configs, heap-scan audit still PASSES, live smoke against any touched path.
- Each item deferred to post-1.0: rationale documented in the `(post-1.0)` note below the item.
- Basket pass closes the pre-1.0 plan — no open items remain that aren't explicitly marked post-1.0.

**Effort.**  Variable — depends on what surfaces.  Items 1-4 are the original seed (~half-day to one day).  Items 5-11 were added during Sittings 9 / 11 / 12 / 14 (Polarion path-traversal coverage, AI output size cap coverage, reaper CV shutdown-timing test, circuit-breaker failure-class policy, ReadMeta simdjson rewrite, email_watch save-path live test, KeyManager hardening synth tests — each small to medium).  If the basket grows significantly during the remaining sittings, consider splitting into per-theme sub-sittings (e.g. one for HTTP-layer items, one for OAuth-layer items, one for test-coverage items).

**Cross-reference.**  Items are added here under the pre-1.0 closing-list policy — pre-1.0 findings surface immediately to JC rather than silently deferring.

---

## Hand-off

Each sitting should follow the established hand-off log format (`doc/misc/hand-off.md`): prepend a new entry at session end with "What landed", "What's verified", "Architecture notes", "Open items / next-session candidates", "Gotchas", "Files in working tree".

Cross-references to update on plan completion:
- `todo.md` — remove the closed sections; preserve the "Loose follow-ups" entries that turn out to need further deferral.
- `MEMORY.md` — add a feedback memory for "new error-returning APIs use `std::expected`" after Sitting 7.
- `doc/misc/hand-off.md` — final entry notes plan completion + any deferred items.
