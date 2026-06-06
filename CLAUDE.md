# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

JarvisAgent is a C++ backend + React frontend application for parallel AI-driven automation. It is a workflow engine that coordinates concurrent AI requests with visual workflow editing and multi-platform deployment.

## Build System

The project uses **Premake5** to generate platform-specific build files.

Two editions are available, selected at build time:

| Edition | Flag | Output | Purpose |
|---------|------|--------|---------|
| **j9t Studio** (default) | *(none)* or `--studio` | `bin/Release/jarvisAgent-studio` | Full developer IDE — workflow editor, AI JCWF generation, AI assistant, config editing |
| **j9t Engine** | `--engine` | `bin/Release/jarvisAgent-engine` | Lean production server — no workflow CRUD, no AI tooling, no script writing |

```bash
# Studio edition (default — full developer IDE)
premake5 gmake
make config=release    # → bin/Release/jarvisAgent-studio

# Engine edition (lean production server)
premake5 gmake --engine
make config=release    # → bin/Release/jarvisAgent-engine

# Optional: Enable Tracy profiler
premake5 gmake --tracy            # Studio + Tracy
premake5 gmake --engine --tracy   # Engine + Tracy

# Debug build
make config=debug verbose=1
```

Each edition has its own objdir (`bin-int/studio/` vs `bin-int/engine/`) so switching editions triggers a full rebuild automatically.

For Windows, use `premake5 vs2022` (Studio) or `premake5 vs2022 --engine` (Engine) to generate a Visual Studio 2022 solution.

## React UIs

Two separate React apps that build into the C++ binary's served static files:

```bash
cd code/frontend/dashboard/ui && npm install && npm run build
cd code/frontend/workflow-editor/ui && npm install && npm run build
```

## Running

```bash
# Recommended: use the launcher (auto-manages Python venv):
./jarvisagent.sh              # Studio edition
./jarvisagent.sh --engine     # Engine edition

# Dashboard: https://localhost:8443
# Workflow Editor: https://localhost:8443/editor

# Or run the binary directly:
./bin/Release/jarvisAgent-studio
./bin/Release/jarvisAgent-engine
```

## Testing

Tests are Python scripts that hit the REST API of a running JarvisAgent instance:

```bash
# Terminal 1: start the server
./bin/Release/jarvisAgent-studio

# Terminal 2: run tests
python3 test/run_tests.py --all             # all workflows
python3 test/run_tests.py --workflow NAME   # single workflow
python3 test/run_tests.py --list            # list available workflows

# AI assistant tests
python3 test/assistant/test_assistant.py                          # non-AI
python3 test/assistant/test_assistant.py --with-ai
python3 test/assistant/test_assistant.py --with-ai --auto-approve # includes mutating tools

# Auth + MCP integration
python3 test/test_auth_mcp.py --admin-key "$J9T_TOKEN"

# Adhoc slug collision e2e
python3 test/security/test_adhoc_user_slug_collision.py --admin-key "$J9T_TOKEN"

# ConfigParser malformed-input fixtures (subprocess sandbox)
python3 test/config/test_malformed_configs.py

# D1 negative-path: size caps, path confinement, mutex stress, inflight leak, db_query caps
python3 test/hardening/test_negative_paths.py --admin-key "$J9T_TOKEN"

# email_watch persistence + integrity guards (needs greenmail + JARVIS_MASTER_PASSWORD for restart cycles)
python3 test/hardening/test_email_watch_persistence.py --admin-key "$J9T_TOKEN"

# KeyManager keystore parse caps (subprocess sandbox; crafts encrypted keys.json.enc fixtures)
python3 test/hardening/test_keymanager_caps.py

# Live S3 round-trip via the consolidated SigV4 signer (needs an S3 connection online, e.g. minio)
python3 test/dispatch/test_s3_roundtrip.py --admin-key "$J9T_TOKEN"

# Note: Use 'python' instead of 'python3' on Windows and Arch/Manjaro
```

## Code Style

- **Allman** brace style (braces on their own lines after functions/classes/control flow)
- **Member fields**: `m_` prefix + PascalCase (e.g., `m_ThreadPool`, `m_Config`)
- **Column limit**: 125 characters
- **Indent**: 4 spaces
- **Pointer alignment**: Left (`int* ptr`, not `int *ptr`)
- C++23 standard (`premake5.lua::cppdialect "C++23"`)

Format with clang-format using the `.clang-format` config at the root. No automated linting in CI — formatting is manual/IDE-driven.

Local clang ≤18 dev: build via `premake5 gmake --clang` then `make` — this routes through libc++ (`-stdlib=libc++`) because libstdc++'s `<expected>` requires `__cpp_concepts >= 202002L` and clang 18 reports `201907L` (fixed in clang 19+).  CI Linux + Docker use gcc 13+ (stock libstdc++ works); Rocky 9's RPM job activates `gcc-toolset-13` via `scl enable`.  See `DEVELOPMENT.md` "C++23 toolchain notes" for the full build-matrix table.

### Discipline rules (each one came from a real bug — don't relax them)

- **No `default:` arms in `switch` over closed enums we own** (`InterfaceType`, `AuthStyle`, etc.). Either enumerate every case (the `-Wswitch` warning catches missing arms when a variant is added) or `static_assert(NumVariants == N, "extend this switch")`. A `default:` that silently absorbs unknown variants is anti-debugging armor — see the silent-Bearer-fallback bug in `CurlMultiDispatcher` for a real example.
- **Don't duplicate complex-struct construction across files.** If two places build the same `QueryData` / envelope / similar struct from the same inputs, extract a `BuildXxx(...)` helper before a third site appears. Parallel construction sites are guaranteed to skew when fields are added.
- **Failure-path logs are ERROR-level AND mention the runId or workflowId as a literal substring.** The dashboard's Run Analysis filters issues to lines containing this run's identifiers; a fail-path log without an id (or at WARN level) is invisible to it. Subsystems without run context (parsers, signers) return errors via their data types and let the upstream caller — which has the runId in scope — emit the ERROR log. Concrete: `LOG_APP_ERROR("AiRequestPool::Submit: ... run='{}' workflow='{}' task='{}': ...", ...)` not `LOG_APP_WARN("operation failed: %s", err)`.
- **All C++ output goes through `LOG_*` macros**, never `std::cout` / `std::cerr`. The macros land in both the ncurses TUI and `log/log.txt`; raw stream writes only land in one (or are silently swallowed in TUI mode).
- **simdjson is the only JSON library**. Don't add nlohmann or RapidJSON for new capabilities — extend on top of simdjson.
- **Filesystem-touching paths from external strings go through `code/backend/application/file/pathConfinement.h::ConfineUnderProjectRoot()`**, not local re-implementations. Fail-closed; rejection logs at ERROR with the offending input. Use sites: Python `sys.path` entries, Python `taskWorkingDirectory`, `PythonEnginePool::Initialize`'s `scriptPath`, `WorkflowRuntimeManager::CleanWorkflow`'s 5 `fs::remove*` sites. When a 6th site appears — extend the helper's use-site list, don't write a fresh local copy.
- **Outbound HTTP from workflow context (e.g. completion-callback `callbackUrl`) goes through an SSRF gate**: scheme allowlist (`https://` only), DNS resolution + per-address rejection of loopback / RFC 1918 / link-local / unique-local / multicast / cloud-metadata ranges (incl. IPv4-mapped IPv6 unwrap), TLS verify + no-redirect + protocol allowlist. The reference implementation is `IsCallbackUrlAllowed` in `workflowRuntimeManager.cpp`; cloud-connector requests use `ConnectorHttp` which has its own equivalent. Don't add a new outbound-HTTP surface without one.
- **AI-dispatch URLs (`api_interfaces[].url`) are loopback-only for plain `http://` and never carry a `key_name` over plaintext.** Reference impl: `AIAssistant::UrlPolicy::ValidateAiInterfaceUrl` in `code/backend/application/network/urlPolicy.{h,cpp}`.  Enforcement hooks: `ApiInterfaceManager::ValidateAndNormalize` (store-load + persist fail-closed — non-compliant interface dropped before it reaches `m_ApiInterfaces`) and `HandleAiInterfaceCreatePost` / `HandleAiInterfaceUpdatePut` (REST 400 + `code=url_policy_violation` / `credentialed_plaintext_http`).  `https://` URLs pass unchanged.  No env-var or flag escape hatch; the one safe `http://` case is loopback.  Companion: `AiRequestPool::Submit` emits a `LOG_SECURITY_INFO("[security] ai_dispatch_plaintext_http ...")` line per successful plain-HTTP dispatch so every plaintext byte that leaves j9t is greppable.  This completes the outbound-HTTP SSRF cluster (callback gate + connector gate + AI-interface gate).
- **"AI is available" is gated on interface *usability*, never on credential count.**  A keyless loopback interface (ollama / llama.cpp / vLLM) needs no credential, so the run gate (`WorkflowRuntimeManager::CheckAiPrerequisites`), the dashboard `has_providers` banner signal (`/api/settings/keys/status`), and the status snapshot all go through `Core::HasUsableAiInterface()` / `Core::IsAiProviderAvailable(name)` — usable = keyless OR the interface's `key_name` has a stored credential.  A `!m_Credentials.empty()` credential-count proxy is the anti-pattern: it silently blocked **every** `ai_call` workflow on local-LLM-only setups (the keyless-provider bug), masked for ages because the keystore always carried ≥1 cloud key and "keyless" ollama dispatch was actually *borrowing* it.  Companion dispatch rules: `AiRequestPool::ResolveApiKey` returns empty for a keyless interface (never borrows the default credential — that both masks a missing provider and would attach an unrelated secret to a plaintext loopback request); `AiRequestPool::Submit` treats an empty key as fatal only when `key_name` is non-empty (declared-but-unresolvable); `LiveTransport` skips the auth signer entirely when the key is empty (keyless → no auth header).  (The removed `KeyManager::HasProviders()` / `HasDefaultCredential()` were the credential-count checks this superseded — don't reintroduce them.)
- **SHA-256 hex digests go through `EngineCore::Sha256Hex(std::string_view)`** (`code/backend/engine/auxiliary/sha256.h`), not file-local OpenSSL `SHA256()` + hex-encode loops.  Returns empty on the impossible OpenSSL-NULL path (logs LOG_CORE_ERROR internally).  `string_view` input accepts `SecureString::Get()` views without a `std::string` materialisation step — required for digest-of-secret use cases.
- **Hand-built file writes go through `EngineCore::AtomicWriteFile()`** (`code/backend/engine/auxiliary/file.h`), not local `std::ofstream` + `fs::rename` re-implementations. The helper creates parent directories, opens `<path>.tmp.<atomic-counter>` with `ofstream::exceptions(failbit | badbit)`, writes, closes, then atomically renames over the destination. Returns `bool` + populated `errorMessage`; does not log internally (callers with run/workflow context emit the ERROR line). Opt-outs are limited to streaming writers with running caps (`dbQueryCloudTaskExecutor`), append-mode logs (`assistantSession.cpp`), and operator-diagnostic stdout/stderr dumps — those get `out.exceptions(failbit | badbit)` + try/catch instead. See `code/backend/application/file/README.md` "Atomic-rename writes" for the use-site list.
- **Long-lived subsystem threads must be stopped inside the shutdown watchdog window** — joined via an explicit Stop call routed through `WebServer::SignalStop` / `WebServer::WaitStop` / the engine's own subsystem-shutdown chain in `Core::Shutdown`, NOT only via the owning object's destructor.  The 3-second shutdown watchdog in `engine.cpp:297-311` covers `app->OnShutdown()` + `engine->Shutdown()` but is diffused right before `main` returns — the subsequent `unique_ptr<Application>` destruction (where destructor-only thread joins fire) is NOT covered.  A subsystem-thread stall introduced there (e.g. a `sleep_for(60s)` regression in a reaper loop) would silently delay process exit with no `[shutdown watchdog] timeout expired` line and no `_exit(EXIT_FAILURE)` — exactly the kind of regression that's invisible to tests and to operators.  Discipline: every `std::thread` / `std::future` / threadpool-worker subsystem with a blocking `wait_for` / `sleep_for` / poll loop has an explicit Stop method invoked from one of the watchdog-covered phases; the destructor remains an idempotent safety-net call to the same Stop.  Reference impl: `AdhocWorkflowManager::StopReaperThread()` invoked from `WebServer::SignalStop` (right after the WRM observer detach).  Companion audit (all `std::thread` members across `application/` + `engine/`): every long-lived thread is currently inside the window — adding a new threaded subsystem MUST extend the same pattern.
- **Never fire user callbacks while holding a fundamental data-structure lock.**  When a user callback runs inside a critical section (e.g. `m_DebugMutex`, `m_Mutex`, the inbox mutex), and that callback can re-enter the same subsystem (via a getter, an event push, a state-transition call), you have set up an AB-BA deadlock the moment any other thread acquires the locks in the opposite order.  Discipline: under the lock, **collect** the callbacks (move them into a local vector along with the payload they need), release the lock, then fire them.  Reference impls: `CurlMultiDispatcher::DrainPendingCancellations` and `CurlMultiDispatcher::OnTransportComplete` — both stage `std::vector<Callback>` (or a single `Callback callbackToFire`) out of the locked region and invoke after release.  Companion: when a callback needs a tiny piece of subsystem state (e.g. the AIMD cap for one quotaKey), expose a lightweight getter that locks only ONE of the involved mutexes (e.g. `CurlMultiDispatcher::GetCurrentConcurrencyCap`) so re-entry doesn't pull in a second lock.  Routing through `GetDebugSnapshot` is the wrong move — it locks `m_InboxMutex` first then `m_DebugMutex`, the exact inversion of the path that holds `m_DebugMutex` while invoking the callback.
- **Secrets never materialise into a plain `std::string` between `SecureString` and `curl_slist_append`.**  The credential hierarchy in `code/backend/engine/keys/credential.h` (ApiKeyCredential / OAuthCredential / AwsCredential / BasicAuthCredential / KeyPairCredential) holds every secret in mlock'd, zero-on-destruct `SecureString`.  The dispatch-time transport bundle `CloudCredentials` (`code/backend/application/cloud/cloudConnector.h`) does the same for `m_Token` / `m_SecretKey` / `m_Password`.  Wrapping `SecureString::Get()` in `std::string(view)` or concatenating `"Authorization: Bearer " + view` allocates a non-mlock'd heap copy that survives uninitialised after the string destructs and is recoverable from heap residue (debugger, core dump, allocator fastbin replay).  Discipline: thread `SecureString` through the HTTP-build path; build the secret-bearing header with `SecureString::Format(prefix, secretView, suffix)` for the three-piece case or `SecureString::Build({pieces...})` for the N-piece case (form-urlencoded POST bodies — both share the same single-mlock'd-allocation + copy-and-swap-on-`bad_alloc` posture); hand it to libcurl via `SecureString::CStr()` + `CurlSlist::AppendCStr(...)` (or `curl_slist_append` / `CURLOPT_POSTFIELDS` directly) so no intermediate `std::string` exists.  The canonical helper is `AppendSecretHeader(curl_slist*&, prefix, SecureString const&, SecureString& scratch)` in `code/backend/engine/curlWrapper/curlSlistHelper.h` — every cloud-connector / workflow-filter `"Authorization: Bearer " + token` site routes through it; new sites do the same.  `IAuthSigner::Apply` separates output into `std::vector<std::string>& publicHeaders` (Content-Type, version, signed-not-secret headers) and `SecureString& secretHeader` (caller-owned; filled for Bearer / x-api-key / x-goog-api-key / api-key, plus SigV4 when an STS session-token X-Amz-Security-Token is present).  HMAC-input signers (`AzureSharedKeySigner::Sign`, `SigV4Signer::Sign` in both `code/backend/application/cloud/` and `code/backend/engine/curlWrapper/`) take `SecureString const&` for the secret key and consume via `Get()` views — the HMAC chain itself derives intermediates inside `ScopedSecretBytes` (shared definition at `code/backend/engine/keys/scopedSecretBytes.h`; both engine SigV4 and cloud SigV4 / AzureSharedKey use it).  Reference impls: `BearerSigner::Apply` in `code/backend/engine/curlWrapper/authSigner.cpp`; `LiveTransport::SetupEasyHandle` + `CurlWrapper::Query` for the slist boundary; `polarionClient.cpp` for a helper-class-with-bearer-token pattern.  `QueryData::m_ApiKey` is `SecureString` (the dispatcher's retry path uses an explicit `QueryData::Clone()` for legitimate deep copies; the default copy ctor is deleted).  The two architectural residue floors are libcurl's own `strdup` inside `curl_slist_append` (cleared via `curl_slist_free_all`'s `free()`, not `explicit_bzero+free`) and libpq's `PQconnectdbParams` internal password copy (freed via `PQfinish` without zeroing) — both outside the threat-model boundary; would require forking the upstream library to fix.  Empirical verification: `test/security/heapScan_test.cpp` plants a nonce as the secret, drives each auth style end-to-end, churns the allocator, then scans `/proc/self/mem` — 10 scenarios (9 must-be-zero plus the documented libcurl floor).  Opt-in via `premake5 gmake --heapscan` (`J9T_HEAPSCAN_BUILD`); the audit binary runs in place of normal startup and `std::exit(0)` on PASS / 1 on FAIL.  Re-run after touching anything in the SecureString → `curl_slist_append` path.  See `doc/cyber security.md` "SecureString-only HTTP path" + "Empirical verification — heap-scan audit".
- **New error-returning APIs use `[[nodiscard]] std::expected<T, SubsystemError>`** — not the legacy `bool DoX(..., std::string& errorMessage)` shape.  Subsystem-scoped error enums (`ConnectorError` in `code/backend/application/cloud/connectorError.h`, `ParserError` + `RegistryError` in `code/backend/application/workflow/`) carry a `Code` enum (no `default:` arm — `-Wswitch` is the enforcement) plus a `std::string m_Details` for the human-readable message.  Caller logs `Describe(err.m_Code)` (typed category) + `err.m_Details` (variable detail) at ERROR with run/workflow context.  All three subsystems use the pattern: `WorkflowRegistry::RemoveWorkflow` → `RegistryError`; `ICloudConnector::TestConnection` + 13 overrides + `ValidatePublicHttpEndpoint` + `ValidatePostgresParams` → `ConnectorError`; 23 `WorkflowJsonParser` methods + 2 `Require*` helpers → `ParserError`.  When extending: pick `Code` per site (TypeMismatch / MissingField / ValueOutOfRange / InvalidConfig / CredentialMissing / CredentialInvalid / NetworkError / AuthFailure / HttpError / etc.) using the existing impls as the per-category template.  Utility helpers (e.g. parser's `ExtractRawJson` / `ElementToString`) stay on the legacy bool shape — chain methods bridge their failures into typed errors at the call site.

## Architecture

### Major Components

| Component | Responsibility | Key Files |
|-----------|---------------|-----------|
| **Engine Core** | Thread pool, logging, event queue, JSON parsing | `code/backend/engine/engine.h`, `code/backend/engine/event/` |
| **Workflow Runtime** | DAG-based task execution, JCWF parsing | `code/backend/application/workflow/workflowJsonParser.h`, `triggerEngine.h` |
| **AI Request Pool** | Parallel AI dispatch over six adapters (OpenAI Chat / OpenAI Responses / Gemini native / Anthropic Messages / Azure OpenAI / AWS Bedrock); auth uniformly via `IAuthSigner` (Bearer / x-api-key / x-goog-api-key / api-key / SigV4) | `code/backend/application/workflow/aiRequestPool.h`, `aiCallTaskExecutor.h`, `code/backend/engine/curlWrapper/authSigner.h` |
| **Session Manager** | Queue file monitoring, STNG/CNTX/TASK file assembly | `code/backend/application/session/` |
| **File Watcher** | Real-time queue folder change detection | `code/backend/application/file/fileWatcher.h` |
| **Web Server** | REST API + WebSocket, React UI serving (Crow framework) | `code/backend/application/web/webServer.h` |
| **Workflow Registry** | JCWF workflow CRUD, versioning | `code/backend/application/workflow/workflowRegistry.h` |
| **Python Engine** | Executes Python scripts embedded in workflows | `code/backend/application/python/pythonEngine.h` |
| **AI Assistant** | Natural language workflow generation | `code/backend/application/assistant/` |
| **AI JCWF Service** | AI-driven JCWF generation pipeline (decompose, generate, review) | `code/backend/application/web/aiJcwfService.h` |
| **JCWF Container** | Zip container read/write for `.jcwf` files (miniz) | `code/backend/application/workflow/jcwfContainer.h` |
| **Trigger Engine** | Cron, file-watch, webhook, manual triggers | `code/backend/application/workflow/triggerEngine.h` |
| **Task Executors** | Pluggable executors: shell/Python/AI/internal tasks | `code/backend/application/workflow/taskExecutor*.h` |

### Data Flow

1. **Trigger Engine** fires workflows on cron / webhook / file-watch / manual events
2. **Workflow Runtime** runs the task DAG, materializing each `ai_call` task's queue folder with **STNG** (settings), **CNTX** (context), **TASK** (instruction), **PROB** (one prompt per fan-out item) inputs
3. **AI Request Pool** dispatches the assembled `AiInvocation` envelope in parallel — disk-first stays preserved (PROV sidecar + transcript still written), but the envelope is the authoritative source of truth, not a file-watcher round-trip
4. The reply lands at `<prob>.output.{txt,json}`; the runtime advances the DAG to dependent tasks
5. **Web Server** exposes REST + WebSocket for the React UI to monitor execution

See `doc/architecture.md` "AI Dispatch Pipeline" for the full diagram.

### Workflow Format (JCWF)

Workflows are `.jcwf` zip containers holding `global.json` (metadata), canvas JSONs, and sub-workflow folders. Key concepts:
- **Template variables**: `{{binding.field}}` substitution in task inputs
- **Per-item fan-out**: CSV/text filters spawn parallel AI calls per item
- **Task dependencies**: Explicit `dependsOn` edges enabling serial + parallel execution
- **Sub-workflows**: Nested canvas pages with their own task DAGs
- **Disk-first design**: All inputs, outputs, and intermediates stored on disk

See `doc/JC_Workflow_Specification.md` for the full format definition.

### Key Documentation

- `doc/JC_Workflow_Specification.md` — Complete JCWF format and execution model
- `doc/api-endpoints.md` — REST API reference
- `doc/architecture.md` — Detailed architecture overview, including the **Key Design Decisions** table (the "why" behind non-obvious choices)
- `doc/jarvisagent.md` — User manual / `config.json` reference
- `doc/cyber security.md` — Threat model, MCP key lifecycle, master-password handling
- `code/mcp/README.md` — MCP tool surface (run, configure, artifact retrieval)
- `integration/README.md` — Webhook triggers, n8n integration, HMAC signing
- `docker-compose.example.yml` — Optional dev mocks (`aoai-api-simulator` for Azure OpenAI testing, LocalStack Hobby tier for Bedrock testing) — both kept commented; uncomment to run

### Vendored Dependencies

All third-party libraries are in `vendor/`: libcurl + openssl (HTTP/HTTPS), spdlog (logging), simdjson (JSON parsing), pdcursesmod (terminal UI), crow (REST framework), asio (async I/O), thread-pool, tracy (optional profiler), date (timezone).
