# JarvisAgent TODO

Open items consolidated 2026-04-28.  Closed-items history archived in `doc/misc/`:
- `doc/misc/JarvisAgent TODO List.md` — global archive
- `doc/misc/application-workflow-todo.md` — backend workflow archive
- `doc/misc/workflow-editor-todo.md` — frontend archive

The two scope-specific live files (`application/workflow/doc/todo.md`, `workflow-editor/todo.md`) currently hold only headers — no open items.

**See also:**
- `doc/misc/hand-off.md` — session hand-off log; read latest entry first when picking up.
- `doc/misc/cybersec-hardening-dev-plan.md` — §18 plan (4-domain split, 4 sessions combined with §19).
- `doc/misc/cpp-safety-hardening-dev-plan.md` — §19 plan (Rust-emulating C++ defaults).
- `doc/misc/AI call performance optimization.md` — §5g design reference (refactor complete).

---

## Pre-1.0

### Concurrent-run policy for JCWFs (serialize by default)
Surfaced 2026-04-29 during sitting-4 verification.  When the `--with-ai --auto-approve` test suite exercised `run_workflow`, the AI picked `jarvisCppDocu` (141 tasks) and started it multiple times — the runs raced on the shared queue folder, each run's `OnOutputFileCreated` callback consumed events the other was waiting for, and 82 tasks ended up stranded in `waiting_external` with output files already on disk that no one would transition.  Today the engine has no concurrency guard: there's no single-instance lock, no serialization, nothing.  The dashboard greys out the Run button while a run is active, which **looks** like a guard but only blocks the UI path — REST/MCP/test callers bypass it entirely.  That UX-vs-engine gap is itself a finding to fix.

Three policies, picked in `global.json`:
- `parallel` — current behaviour; both runs proceed concurrently.  Right for genuinely fan-out-by-design workflows but they need per-run queue folders to be safe (separate work).
- `serialize` — second run accepted, sits in `pending` until the first completes; FIFO drain.  Right for "I clicked Run twice / cron stacked / test fired Run twice" — preserves runId observability without overlap.
- `reject` — second run returns 409 Conflict; caller decides retry/fail/log.  Right for IO-exclusive jobs.

**Default policy:** `serialize` — safer surprise.  Workflows that legitimately want parallel execution opt in via `"concurrency": "parallel"` in `global.json` and pair it with the per-run queue folder work below.

Implementation lives in `WorkflowRuntimeManager`:
- New `m_PendingByWorkflow: unordered_map<workflowId, deque<RunRequest>>`, mutex-guarded.
- `EnqueueWorkflowRunAndGetRunId` checks the JCWF's policy.  If `serialize` and there's already an active run for `workflowId`, push to the deque and return the new runId in `pending` state.
- On run completion (succeeded / failed / cancelled / stopped), pop the front of the deque and start the next run.
- Cap the deque (e.g., 32).  Overflow either rejects or drops oldest with `LOG_APP_ERROR`.

Dashboard work pairs with this:
- Render `pending` runs distinctly from `running` runs (different badge / state column).
- Stop button on a pending run dequeues cleanly — no AI calls dispatched yet, just remove from queue.
- Match the dashboard's existing "grey out Run button" UX to the actual engine behaviour: with serialize-default, the Run button can stay enabled and queue the request rather than reject it.

Per-run queue folder (separate, deeper work, not blocking this entry): `queue/<workflowId>/<runId>/<NN>_<task>/` instead of `queue/<workflowId>/<NN>_<task>/`.  Required for `parallel`-opted workflows to be hermetic.  Tracked here as a follow-up rather than its own entry because it only matters once `parallel` is opt-in.

### Per-interface mock transport for parser fault injection
Sequenced after the 4 hardening sessions.  New `IInterfaceTransport` abstraction with two impls per real `InterfaceType` (API1–API5 + API6 reusing API1's parser): `LiveTransport` (real curl + auth signer, current behavior) and `MockTransport` (canned responses from disk fixtures).  Switch is dispatch-time, driven by the request: if the request carries a `X-J9T-Mock-Fixture: <name>` header (and a hermetic-mode flag), `MockTransport` is selected and serves bytes from `test/dispatch/fixtures/<api>/<name>.json` (or similar); otherwise `LiveTransport` runs unchanged.  Match strategy is **InterfaceType + fixture-name header only** — no URL or full-header matching, keeping the mock cheap to maintain.

Goal trio: (1) byte-level fault injection through real parsers (malformed UTF-8, surrogate halves, truncated multi-byte, overlong encodings) — closes the §19 SanitizeUtf8 verification gap that today's hermetic dispatcher can't reach; (2) per-interface contract tests catching response-shape drift (provider adds/renames fields) without burning quota; (3) reproducible parser regression fixtures.

**Complementary to existing HTTP mocks** (`aoai-api-simulator` for API6, LocalStack for API5) — those keep covering auth + curl + multi-dispatcher behavior with realistic well-formed bodies.  The routing-layer mock focuses on parser/byte pathology where the HTTP mocks are weak.  Not redundant: different layers, different bug classes.

Implementation skeleton:
- New header `engine/curlWrapper/interfaceTransport.h` defines `IInterfaceTransport` (one virtual dispatch method matching today's `CurlMultiDispatcher` request shape).
- `LiveTransport` wraps the existing curl path verbatim (refactor, not rewrite).
- `MockTransport` reads `<fixture>.json` (or `.bin` for binary-pathology fixtures) plus an optional `<fixture>.meta.json` for HTTP status / headers / latency injection.
- Selection at `AiRequestPool::Submit` time: if `m_MockFixture` is set on the envelope, use Mock; else Live.
- Fixtures committed under `test/dispatch/fixtures/api{1..6}/`.  Each interface gets a baseline `golden_response.json` + a battery of pathology fixtures (`malformed_utf8.json`, `truncated_multibyte.json`, `surrogate_half.json`, `overlong.json`, `empty_choices.json`, `missing_finish_reason.json`, etc.).
- New test files `test/dispatch/test_api{1..6}_mock_*.py` per interface drive the dispatcher with fixture names and assert downstream invariants.

### Dogfood the workflow editor (JC)
Write a few non-trivial JCWFs **directly in the editor** rather than as raw JSON: sub-workflow nesting, per-item fan-out, mixed task types (ai_call + python + shell + cloud), file_watch trigger, error-branching edges.  The editor exists and has a 70-test suite, but it's never been driven by JC in anger.  Goal: surface UX gaps, validation-surprise messaging, broken-state visibility, inspector quirks.  Findings inform 1.0 polish or post-1.0 backlog depending on severity.

### Dogfood the AI assistant (JC)
Drive a real conversation through the assistant chat surface: multi-turn tool-use loop, approval flow for mutating tools, the eight `jcwf_*` tools (read / explain / validate / read_plan / write_plan / generate / fix_task / write_script), runtime-control tools (`workflow_pause/resume/stop`, `get_dashboard_status`), slash commands, ghost-text auto-completion, history search, persistent session save/load.  Cross-references §18 D2 hardening triage: the assistant is exactly where the cyber-sec audit found its densest CRITICAL cluster (`assistantTools.h` has five shell-injection findings in a single file + the tool-approval bypass in `assistantController.h`).  Findings reachable in real use should jump the §18 D2 queue; findings unreachable in any plausible workflow get a "skip with reason" entry.  Two-for-one: dogfood validation **and** sharper hardening triage.

### Repository layout + root-folder hygiene
Group sources under `code/{backend,frontend,mcp}`; gitignore runtime folders (`queue/`, `workflows/`); prune root artefacts (`.npm-tools/`, `jarvis_agent.example.env`); consider moving Docker files to `packaging/Docker/`.  Rollout in 4 phases (runtime folders → root cleanup → Docker relocation → source tree reorg).  GitHub first impression matters before 1.0.

### Landing page for new users
Welcoming landing page / website explaining what JarvisAgent is, key features, screenshots, download links.  Target: first-time visitors who discover the project.

### Promotion video
Demo / promotion video covering workflow creation in the editor, running workflows, dashboard monitoring, multi-platform support.  Target: GitHub README embed, YouTube, social.

---

## §5i follow-ups (post-implementation)

The main §5i work landed 2026-04-25 (see `doc/misc/engine-studio-capability-review.md` lines 333-339).  These are the small tail items the review doc tracked at end-of-session:

- **`HandleAiInterfaceTestPost` Engine fallback** — today returns `ai_test_not_available_in_engine` and lives in `webServer.cpp` (not `webServer_studio.cpp`) with one inline `#ifdef J9T_STUDIO` around the `m_AiJcwfService.TestAiInterface(...)` call.  Two options: (a) move the test path into `aiRequestPool` so both editions support it (more useful for Engine admins debugging interface config); (b) make the route Studio-only and hide the dashboard's Test button on Engine.  Either is fine — pick one.
- **AI-WebSocket dispatch extraction** — ~150-line `#ifdef J9T_STUDIO` block in `webServer.cpp`'s `/ws` `.onmessage` lambda dispatching `ai-explain-jcwf` / `ai-generate-jcwf` / `ai-write-scripts` / `ai-fix-failed-script` / `chat`.  Extract into a helper (`WebServer::HandleAssistantWebSocketMessage`) and move into `webServer_studio.cpp`.  Mechanical, drops the `#ifdef` count in `webServer.cpp` from 10 to ~7.
- **Bootstrap admin user collides with role name** — first-run admin's user is literally `admin`, which renders as `admin / admin` in the dashboard StatusBar (user / role badge).  Either rename the bootstrap user (e.g. `j9t-bootstrap-admin`) at MCP key activation time, or have the React StatusBar suppress the role badge when user equals role.  Cosmetic only — no security impact.

---

## §5g remaining follow-ups (post-1.0)

The AI dispatch refactor's main work is committed; these are slice items the §5g handoff explicitly tracks for later:

- **JCWF schema gap-close** — close gaps in `doc/jcwf.schema.json` vs. `workflowJsonParser`; add a parser↔schema contract test.  Embedded `kJcwfSchemaJson` already compiled into the binary.
- **Schema-enforced JCWF generation** — wire `AiJcwfService::GenerateAsync` to set `AiInvocation.m_OutputSchemaJson = kJcwfSchemaJson` with validator-error retry.
- **`EventCategoryAi` consumers** — events posted from the dispatch lifecycle; only the aggregated "in flight" LED reads them today.  TUI / dashboard could surface per-call AI events.
- **More `test/dispatch/` contract test slices** — many landed (hermetic, relaxed env, output-schema roundtrip, chunking, markitdown, cross-workflow concurrency, Tier A, Tier B, TUI stress).  Remaining slices tracked as follow-ups; no specific list yet.
- **Live-backed E2E tests for schema-validation retry, chunking, and markitdown** — flagged in `doc/misc/AI dispatch refactor.md` post-merge follow-ups: today these paths are hermetic-only (TestInterface).  Live-backed versions hitting real providers would catch tokenizer / quirk drift the hermetic tests don't.

---

## Post-1.0

### Native LLM tool-calling
- **§5e.1 Assistant** — replace `<tool_call>` regex parser in `assistantTools.h::ParseToolCalls()` with native tool-calling (OpenAI `tool_calls[]`, Gemini `functionCall`, Anthropic `tool_use`).  Extend `ReplyParser` with `GetToolCalls()`; tag parser stays as fallback for interfaces that don't expose tools.
- **§5e.2 JCWF `ai_call`** — JCWF `ai_call` gains optional `tools: [{name, description, parameters_schema, handler_task}]`.  `AiCallTaskExecutor` enqueues handler synchronously, awaits `.output.txt`, appends `ToolReturn`, re-dispatches.  New design needed: bounded `max_tool_turns`, deterministic-replay story, freshness-model interaction, transcript format extension.

### j9t as orchestrator of other AI tools — Claude Code PoC
[from §5f] New `InterfaceType::ClaudeCode` that spawns `claude -p "<prompt>"` instead of HTTP.  Auth via user's Claude Max/Pro subscription (no API key in j9t).  Concurrency cap, default `--permission-mode=plan` (read-only), Engine refuses mutation.  Longer-term candidates: cross-instance j9t→j9t via existing MCP sidecar, Cursor Agent (headless), Aider, OpenAI Codex CLI.

---

## Cloud integration tail (post-implementation)

The bulk of the cloud-integration work landed (Phases 0-12 done; see `doc/misc/cloud-integration-dev-plan.md`).  One open item remains:

- **`email_watch` trigger doesn't actually check IMAP** — currently fires on the polling timer regardless of whether new mail arrived.  Wire the trigger to perform an actual IMAP UID check before firing so it only triggers on genuinely-new messages.

(The two cloud-integration-dev-plan checkboxes for Redmine frontend and Snowflake round-trip E2E were both stale — Redmine has full frontend coverage in `ConnectionsView.tsx` + `WorkflowEditorView.tsx`, and `snowflakeQueryDemo.md` documents the verified round-trip end-to-end.  Plan checkboxes never got flipped; closed here.)

---

## Loose follow-ups

- **`tools/replayTranscript.py`** — nominally-planned dispatch debugging tool from §5g.  Reads a `<prob>.transcript.json` and re-emits the exact request body against the same provider, for reproducing drift.  Not built; add when first real replay need arises.

- **`HandleWorkflowVersionRestorePost` is broken since JCWF moved to zip containers** — Surfaced 2026-04-30 during sitting 8 verification.  `POST /api/workflows/<id>/versions/<ts>/restore` reads the version file as raw bytes via `std::ifstream` then passes those bytes to `WorkflowRegistry::SaveOrUpdateWorkflowFromJson`, which expects plain JSON.  Since `.jcwf` files are always zip containers (per `feedback_no_legacy_jcwf`), the parser fails immediately with `restore_failed: UNCLOSED_STRING`.  The handler's auth check, path validation, TOCTOU-safe read, and best-effort backup all run correctly — only the final write step is wrong.  Fix: replace the `SaveOrUpdateWorkflowFromJson` call with the registry's container-aware write path (likely `UpsertJcwfFromZipBytes` or equivalent — check `workflowRegistry.h` for the right entry point).  Live-verified the failure with `exampleMakefile4`'s most-recent version; verified the live workflow is left untouched on failure (md5 unchanged), so this is a "broken feature, not a corrupting feature".  Rarely exercised — the dashboard's version history UI lists versions but the Restore button is the only client and may not have been tested end-to-end since the zip-container migration.

- **Editor needs master-password unlock + MCP login parity with the dashboard** — Surfaced 2026-04-30 during sitting 8 wrap.  Today the dashboard prompts for the master password whenever j9t restarts (encrypted key store locked → `MasterPasswordDialog` pops up → user enters password → dashboard calls `POST /api/settings/keys/unlock`).  The workflow editor has **no equivalent** — when j9t restarts, the editor either silently fails on locked-key requests or shows a broken state, with no UI affordance to unlock from the editor side.  Same gap for MCP login: the dashboard exchanges an MCP key for a session cookie via `POST /api/auth/login`; the editor has no login surface.  **Fix shape:** lift the dashboard's auth-gating components (`dashboard/ui/src/components/MasterPasswordDialog.tsx` + the login flow) into a shared frontend location, or duplicate them into the editor — pick whichever fits existing repo conventions for shared frontend code.  Backend endpoints are already in place and edition-agnostic (see `api-endpoints.md` §"Settings — Key Management" + §"Auth — MCP keys + sessions"); this is pure frontend work.  **Acceptance:** restart j9t → open editor → master-password prompt appears with the same UX as the dashboard; unauthenticated editor session presents a login affordance; both flows handle the same error states (`no_password` / `wrong_password` / `no_keys_file` / gateway-injected identity headers).  Cross-ref `workflow-editor/todo.md` if a more detailed editor-scoped entry is wanted later.

- **`EventQueue` bounded-cap policy** — Surfaced 2026-05-04 during S2 sitting 14 (`engine/event/eventQueue.{h,cpp}`).  The queue has no cap — a wedged main loop (e.g. a long-running Python script in `OnEvent` / `OnUpdate`) lets producers (fileWatcher, AI dispatch workers, webServer, pythonEngine) push unboundedly.  Producers ARE rate-limited in practice today (FS events, AI lifecycle from worker pool, keyboard, SIGINT poll), so this isn't biting in production — but it's a latent OOM vector under burst conditions.  **Fix shape:** soft cap (configurable, default e.g. 10 000 events) + `LOG_CORE_WARN` when crossed.  Drop policy is the design call — drop-newest preserves history but loses fresh signal; drop-oldest preserves freshness but loses backlog.  Behaviour change, so JC's call before implementation.  No drop policy without explicit decision.  **Acceptance:** synthetic stress test — push 50k events while main loop is held for 5s; verify cap hit log line + bounded memory + queue drains correctly when main loop resumes.

- **`ConfigParser` ~30-field boilerplate refactor** — Surfaced 2026-05-04 during S2 sitting 15.  After the horizontal sweep replacing `CORE_ASSERT` with `.get(target)` + `LOG_CORE_ERROR + continue`, each of the ~30 fields in `Parse()` + `ParseInterfaces()` is now ~5-7 lines of near-identical boilerplate (`get_X().get(target)` → `LOG` → store → `++count`).  Could collapse to ~30 single-line helper calls via small functions: `ParseStringField(value, key, target, fieldEnum, occurrences)`, `ParseInt64Field(...)`, `ParseBoolField(...)`, plus a numeric-with-bounds variant.  **Behaviour-neutral polish only** — the safety/correctness work is already in.  Deferred from sitting 15 to keep that scope safety-first.  Cross-ref `feedback_cpp_discipline` "extract a helper before a third site appears" — we now have ~30 sites, well past the threshold.

- **Hardening test for malformed `config.json`** — Surfaced 2026-05-04 during S2 sitting 15.  All the right error paths exist post-sitting-15 (negative numerics → ERROR + continue; type mismatches → ERROR + continue; unknown API → `InvalidAPI` sentinel; out-of-bounds API index → ConfigChecker reject; URL prefix-not-substring; etc.) but they're code-review-only.  No synthetic test feeds malformed input + asserts (a) ERROR logs fire, (b) `Parse()` returns the right `State`, (c) `ConfigChecker::Check()` rejects, (d) NO crash / abort / `std::terminate`.  **Fix shape:** `test/config/test_malformed_configs.py` (or C++ unit test if a test harness exists) with a fixtures dir of bad configs (negative-numeric.json, type-mismatch.json, unknown-API.json, oob-API-index.json, url-substring-attack.json, etc.) + assertions.  **Acceptance:** every fixture loads + produces the expected error pattern + leaves the engine alive.

- **`RedactingFormatter::format` per-line allocation when secrets are registered** — Surfaced 2026-05-04 during S2 sitting 16 (`engine/log/log.cpp` + `secretRedactor.cpp`).  When `HasSecrets()` returns true (i.e., at least one secret registered), the formatter does TWO heap allocations per log line: (1) `std::string(msg.payload.data(), msg.payload.size())` to feed the redactor, (2) inside `Redact()`, `std::string result = message;` whether or not any replacement happens.  Hot path under busy logging.  No-secrets fast path (the common case at server startup) is already lock-free post-sitting-16, so this ONLY matters once secrets are registered.  **Fix shape:** redact in-place into a thread-local buffer, OR have `Redact()` return early without allocating when no secret matches the message.  Pure perf, not safety — defer until profiling shows it as a hot spot under realistic logging load.

- **`KeyManager::GetCredential` TOCTOU race** — Surfaced 2026-05-04 during sitting 17-20 review.  `KeyManager::GetCredential(name)` and `GetDefaultCredential()` return a non-owning `ICredential const*` after releasing their internal `shared_lock`.  Every consumer pattern is `auto const* cred = km.GetCredential(name); /* read cred->fields */; /* maybe call km.UpdateCredential(...) */`.  Between the lock release and the next operation, a concurrent `RemoveProvider` would delete the credential, leaving `cred` dangling — subsequent reads are undefined behaviour.  **This is pre-existing, NOT a sitting-17-20 regression** — the legacy `GetProvider` had the same shape.  Bounded risk in practice: REST credential CRUD is admin-only and rare; OAuthTokenManager's background refresh doesn't directly mutate KeyManager.  Affected sites: every cloud connector's `ResolveCredentials`, `webServer::HandleProviderUpdatePut` (read existing → CloneAndPatch → UpdateCredential), the OAuth callback path (~line 6886), `aiRequestPool::ResolveApiKey` / `ResolveProviderParams`.  **Fix shape (deferred):** add a `template<typename F> bool ModifyCredential(name, F&& mutator)` that reads existing under unique-lock, runs `mutator(*existing)` to produce a new credential, writes it back — all inside one critical section.  Migrate the UPDATE path + OAuth callback path to use this; reads-only sites (cloud connectors) can stay as-is or be converted to `WithCredential(name, lambda)` for stricter safety.  Belongs in a future "concurrent CRUD safety" pass.

- **SigV4 signer: read directly from `AwsCredential` instead of `QueryData::m_Params` reinjection** — Surfaced 2026-05-04 during S2 sitting 19c, deferred at sitting 20 close.  Today's AI-dispatch SigV4 signer (engine/curlWrapper authSigner family for API5 Bedrock) reads `q.m_Params["secret_access_key"]` / `["session_token"]` / `["region"]` from `QueryData`.  `aiRequestPool::ResolveProviderParams` rebuilds those params by reinjecting the AWS SecureString fields from the typed `AwsCredential` after they were stripped at credential-load time — a transitional shim documented in the helper's comment.  **Fix shape:** add a typed credential reference / snapshot to `QueryData` (e.g. `std::shared_ptr<AwsCredential const>` or a small POD copy of the three fields), update the SigV4 signer's `Apply()` to read from there, delete the reinjection in `ResolveProviderParams`.  Touches: `QueryData` struct, `aiRequestPool::Submit` build site, `aiJcwfService.cpp` build site, every cloud connector that constructs QueryData for SigV4, the SigV4 signer's Apply().  No Bedrock provider configured today so the path isn't exercised end-to-end; sitting 19b's `s3Connector` already takes the typed `AwsCredential` path for SigV4 outside the AI-dispatch pipeline.  **Acceptance:** Bedrock model dispatch via aiRequestPool with a real `AwsCredential` produces a valid SigV4 signature and the request succeeds; the reinjection in `ResolveProviderParams` is gone.  Either bundle this with adding a Bedrock test fixture, or wait until first real Bedrock customer.

- **`SanitizeUserSlug` collision risk** — Surfaced 2026-05-06 during S3 sitting 8 (`adhocWorkflowManager.cpp::SanitizeUserSlug`).  The slug strips disallowed characters to `_`, so two distinct user names can collapse to the same slug (`"bob+admin@example.com"` and `"bob_admin@example.com"` both → `"bob_admin@example.com"`).  An adhoc folder lives under `_adhoc/<user_slug>/<run>/` — collision lets one user enumerate or interfere with another's adhoc artefacts via the shared parent directory.  **Fix shape:** append a stable hash suffix derived from the original user (e.g., the first 8 hex chars of SHA-256(user)) so distinct names stay distinct after sanitisation.  Needs a migration path for existing meta.json files that already reference legacy slugs — read the slug from meta.json on boot and don't re-derive.  **Acceptance:** two user names that pre-fix collapsed to the same slug produce distinct slugs; existing adhoc folders remain reachable via their stored owner_slug.

- **SecureString-only path through HTTP layer** — Surfaced 2026-05-06 during S3 sitting 9 (PolarionClient `FetchAll` materialises `api->m_ApiKey.Get()` into a request-scoped `std::string` that doesn't zero on destruction).  Same shape applies to bearer-token / Personal Access Token / OAuth access-token consumers across the cloud surface — every connector that calls `*Credential::m_ApiKey.Get()` to build an `Authorization: Bearer ...` header.  `std::string` is fine for correctness (lifetime is bounded by the HTTP call) but on a compromised process the secret can be recovered from heap residue after the call returns.  **Fix shape:** thread `SecureString const&` (or a non-copying view) through the HTTP-build path so the secret is only materialised into the slist header buffer at the last moment, then wiped.  Touches: `IRequestBuilder`, every Bearer-style cloud connector, the curl auth-header construction site.  Defense in depth, not a current vulnerability.  **Acceptance:** post-call heap scan with a known-token marker doesn't surface the token bytes after the request completes.

- **Atomic-write pattern (write-to-temp + rename) for hand-built JSON writers** — Surfaced 2026-05-06 during S3 sitting 9 (PolarionClient `WriteItemFile`); same pattern applies to `AdhocWorkflowManager` `WriteMeta` / `WriteManifest`, `WorkflowRegistry::SaveOrUpdateWorkflowFromJson`'s global.json/canvas writes, `AiTranscript` appends, and most other hand-built JSON outputs.  Today: open final path → write → close.  Disk-full or process-kill mid-write leaves a truncated/empty file that downstream readers parse as malformed.  **Fix shape:** open `<final>.tmp.<pid>` → write → close → `fs::rename(<tmp>, <final>)`.  Atomic on POSIX (rename within same fs); on Windows requires `MOVEFILE_REPLACE_EXISTING`.  Cross-cuts ~10-15 writer sites across `application/`.  **Acceptance:** SIGKILL during a workflow run mid-write leaves the previous version of the final file intact, not a corrupted current version.

- **`std::ofstream` exception-safety enable across writer sites** — Surfaced during S3 sittings 7+9.  Standard pattern in writer code is `std::ofstream out(path); if (!out.is_open()) return false; out << ...;` with no `out.exceptions(failbit|badbit)` enable.  Disk-full / quota / permission-loss mid-write either silently truncates (CSV/JSON outputs) OR throws on the next `<<` (with the `<<` operator's exception spec).  Pre-existing — not a regression — but a tail-sweep correctness improvement.  **Fix shape:** Either (a) `out.exceptions(std::ios::failbit | std::ios::badbit)` immediately after open + try/catch around the write block, OR (b) per-write `if (!out.good()) return false` after every `<<`-block.  Sitting 9 added pattern (b) to PolarionClient::WriteItemFile.  Cross-cuts ~20+ writer sites.  **Acceptance:** simulated disk-full mid-write produces a logged ERROR with workflow context AND returns false, instead of silent truncation or `std::terminate`.

- **API-shape sweep: `std::optional<T>` / `std::expected<T,E>` for error-returning APIs** — Surfaced 2026-05-06 across S3 sittings 7+8+9.  Many public methods use `bool` + out-param (`bool DoX(..., std::string& errorMessage)`) instead of `std::optional<T>` / `std::expected<T,E>`.  Caller-side: easy to forget the `if (!ok) handle(errorMessage)` check.  Compiler can't enforce.  **Fix shape:** API-shape change with caller-side fanout — convert each `bool + out-param` site to `std::expected<T,E>` (C++23, available with our toolchain) or `std::optional<T>` for nullable success.  Mechanical per-site but big aggregate diff.  Touches the cloud connector surface (Polarion HTTP methods), the registry (`TryGet*`, `SaveOrUpdate`, `RemoveWorkflow`), the request pool (`TryConsumeResult`, `WaitForCompletion`, `OnOutputFileCreated`, `OnRequestFailed`), AdhocWorkflowManager (`Stage` already returns `std::variant`).  **Acceptance:** all D1 + D2 public methods that report errors do so via a typed return; `[[nodiscard]]` on every one; no `std::string& errorMessage` out-params remain at public API boundaries.

- **Negative-path verification fixtures for D1 hardening** — Surfaced 2026-05-06 across S3 sittings 6+7+8+9 "What's not directly verified" lists.  Most rejection branches (path-traversal rejects in file_watch / aiRequestPool / registry / adhoc / db_query / polarion; row+byte+timeout caps in db_query; size cap on adhoc JCWF + AI output; reaper CV wake-on-stop timing; WorkflowRegistry mutex stress under concurrent reload+PUT; inflight-counter race under sync curlCallback) are code-review-only.  Structural shape is right; live exercise is missing.  **Fix shape:** dedicated test sitting building a fixture set that hits each rejection branch + each cap with assertions on log lines + state transitions.  Two-thirds of these would be small Python tests against a running j9t (hostile JCWF construction); the inflight-counter race needs a mock dispatcher (folds into the existing "Per-interface mock transport for parser fault injection" entry above — `MockTransport` configured to return synchronously is the verification vector).  **Acceptance:** every rejection branch and cap has at least one negative-path test that verifies (a) the operation fails with the documented error code/message, (b) the corresponding ERROR log line fires, (c) the engine remains alive afterwards.  Defense in depth on the hardening pass — not a current bug, but proves the gates work as documented.
