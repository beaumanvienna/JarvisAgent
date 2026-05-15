# AI Dispatch Refactor — Dev Plan

**Status:** dev plan, rev 1 — **MERGE-READY** as of 2026-04-24, follow-up throttle work in flight 2026-04-25
**Target:** j9t 1.0
**Tracking:** `JarvisAgent TODO List.md` §5g (pre-1.0 priority). Subsumes §5c.

> ⚠️ **Historical record — `TestInterface` removed in Sitting 2 of `ai-provider-error-visibility-dev-plan.md` (2026-05-14).**  Every reference below to `InterfaceType::Test`, `api_type: "Test"`, or "TestInterface" describes how the system worked through 2026-04 / early 2026-05.  Current dispatch architecture replaces TestInterface with the `is_mock: true` + `fixture_path` pair on real `api_type` entries (`API1..API6`) — the dispatcher routes mock-flagged calls through `MockTransport` (fixture replay) while everything else goes through `LiveTransport` (real curl).  Instructions in this doc that POST `api_type: "Test"` will now return 400 with `api_type_test_removed` — see `doc/jarvisagent.md` "API interfaces" + `doc/api-endpoints.md` `POST /api/settings/ai-interfaces` for the supported migration path.

---

> ## Session end — 2026-04-25 (TPM throttle + retry budget + dashboard live progress — fresh start tomorrow)
>
> **Trigger:** trying to run `jarvisCppDocu` (137 ai_call tasks) against Anthropic Sonnet 4.6 (`API index: 9`) for a one-shot security review of every C++ unit. Initial run on Opus (`API index: 8`) blew through tier limits in seconds (11 succeeded / 126 × `HTTP 429 retries exhausted`). Switched to Sonnet and peeled back successive failure layers.
>
> **Run-by-run progress** (best result per change):
>
> | Run | Build | Succeeded / Failed | Cause of failures |
> |---|---|---:|---|
> | 1 (Opus #8) | baseline | 11 / 126 | 429 storm, no in-process throttle |
> | 2 (Sonnet #9) | baseline | 14 / 123 | same |
> | 3 | + initial-burst cap (4) + Anthropic header parsing | 12 / 125 | retries bypassed gate |
> | 4 | + retries routed through inbox | 41 / 96 | gate only checked `requests`, not `tokens` |
> | 5 | + token quota + separate req/tok reset times | 41 / 96 | `kDefaultTimeoutMs` armed pre-dispatch |
> | 6 | + deferred deadline arm (`onDispatched` callback) | 41 / 96 | retry chains > 5 min wall-clock |
> | 7 | + Option B (deadline reset on dispatch) + 50-retry budget | **50 / 87** | second timeout layer (`WorkflowRuntimeManager::TimeoutWaitingExternalTasks`) |
>
> Each run cleared one failure mode and exposed the next.
>
> **What landed (debug build, on top of the merge-ready 2026-04-24 work — NOT yet committed):**
> 1. **Throttle gate in `CurlMultiDispatcher::DrainInbox`** — per-host gate consults `m_HostRateLimits`. Throttles on (a) HTTP/2 stream cap (existing `kMaxActivePerHost = 48`), (b) provider request quota exhausted, (c) provider token quota exhausted (held until each quota's own reset elapses), (d) initial-burst cap of 4 while waiting for first response headers. Throttle cause logged once per 5 s per host with full state.
> 2. **Anthropic + retry-after header parsing** in `ParseRateLimitHeaders` — `anthropic-ratelimit-{requests,tokens,input-tokens,output-tokens}-{remaining,reset}` plus `retry-after`. Reset times tracked **separately** as `m_RequestsResetAt` and `m_TokensResetAt` (was a single `m_ResetAt`); token resets use the LATEST across in/out/combined. ISO 8601 reset format parsed via `sscanf` + `timegm`.
> 3. **Retries routed through inbox** — `DrainRetryQueue` previously bypassed the gate by adding directly to `m_Active`. Now retries are pushed back to `m_Inbox` (preserving `m_RetryCount`, `m_OnDispatched`, `m_OnRetryQueued`) so `DrainInbox`'s gate applies uniformly to first attempts and retries.
> 4. **Deferred deadline arm (Option B)** — `AiRequestPool::Submit` no longer arms `m_Deadline` immediately after `dispatcher->Submit`. Instead it passes a `DispatchedCallback` that the dispatcher fires when `curl_multi_add_handle` actually puts the request on the wire. The `!m_HasDeadline` guard was removed from `ActivateDeadlineForOutputPath` so each retry's actual dispatch resets the deadline. `m_OnDispatched` is preserved across the retry → inbox round-trip via `ActiveRequest` and `PendingRequest`.
> 5. **Retry-queue wait extension (Option A)** — new `RetryQueuedCallback` on `CurlMultiDispatcher::Submit` fires from the 429 + transient retry handlers with the chosen `delayMs`. `AiRequestPool::ExtendDeadlineForOutputPath` adds it to `m_Deadline` so quota-wait time we deliberately impose isn't charged against the per-attempt timeout.
> 6. **`kMaxRetries` for 429 bumped from 5 to 50** — TPM-throttled large workflows have queue depths that legitimately need dozens of `wait → retry → 429 → wait` cycles. 5 was right when retries were exceptional; 50 is right when retries are the steady state for a back-of-queue task. `kMaxRetriesTransient = 2` unchanged.
> 7. **Debug signals at `/api/debug/signals`** (debug builds only): lifetime atomics `dispatcher_total_{dispatched,throttled,429s,retries_exhausted,completed}`, live `dispatcher_{inbox_size,active_count,retry_queue_size}`, and per-host `{remaining_requests, remaining_tokens, req_reset_in_sec, tok_reset_in_sec, active_count}`. Backed by a `std::recursive_mutex m_DebugMutex` covering `m_Active`, `m_HostRateLimits`, `m_RetryQueue` so the API thread can snapshot consistently while the I/O thread mutates.
> 8. **Dashboard live progress fix** — `WorkflowRuntimeManager::Update()` previously returned `true` (→ `BroadcastWorkflowRunsSnapshot`) only on run start and run termination. Long-running workflows showed the start snapshot until they ended. Fix: fingerprint `{ task states, attempt counts, completion flag }` per `ActiveRun` before/after `TickActiveRun`; if changed, set `stateChanged = true`. Verified live during run 7 (counter ticked `5/138` → `108` → accurate `queued` state).
> 9. **Diagnostic instrumentation** — `ActivateDeadlineForOutputPath` logs `armed`/`RESET deadline` per call; `ExtendDeadlineForOutputPath` logs `extended deadline ... by Nms (retry-queue wait)`. Easy to grep.
>
> **What's still broken — the next failure mode (line 14905 in `log/log.txt`):**
>
> A SECOND timeout layer at `application/workflow/workflowRuntimeManager.cpp:2131` — `WorkflowRuntimeManager::TimeoutWaitingExternalTasks` enforces `kDefaultWaitingExternalTimeoutMs = 300000` (5 min) on tasks in `WaitingExternal` state, **independent of `AiRequestPool`'s `m_Deadline`**. When this fires it calls `requestPool->Forget(handle)` and marks the task failed. 104 tasks killed by this layer in run 7. AiRequestPool's deadline was correctly extended past 5 min by Option A; the workflow runtime didn't know.
>
> Log signature:
> ```
> [19:06:02.730] [warning] [workflow] WaitingExternal timeout for task '...' in run '...' (300004ms elapsed, limit 300000ms)
> ```
>
> **Tomorrow's plan (fresh start):**
> 1. **Plumb the same extension through `WorkflowRuntimeManager`.** Either:
>    - **(a)** Have `AiRequestPool` push deadline updates back to `taskState.m_WaitingExternalSince` on each dispatch/retry (i.e., reset it the same moments AiRequestPool resets `m_Deadline`). Cleanest separation of concerns: workflow runtime no longer owns this clock.
>    - **(b)** Remove the runtime-level `WaitingExternal` timeout for `ai_call` tasks entirely and trust AiRequestPool's deadline (which now self-extends). Keep the runtime timeout for non-AI external waits.
>
>    (b) is simpler and "one watchdog per concern." Verify what other task types use `WaitingExternal` before removing.
> 2. **Smoke-test on a small workflow first** (e.g. `cyber2`, < 20 ai_call tasks). Confirm throttle + deadline behaviour end-to-end without burning Sonnet credits.
> 3. **Then re-run `jarvisCppDocu`** for the actual security review (the original goal — this whole detour started there).
> 4. **Streaming as follow-up** (not blocking). With SSE, idle-timeout becomes the natural watchdog; replaces the deadline-extension bookkeeping. Big lift; defer until everything else is stable.
> 5. **Consider config exposure.** `kInitialBurstCap` (4), `kMaxRetries` (50), `kDefaultTimeoutMs` (5 min), `kAiCallMinWaitingExternalTimeoutMs` (2 min) are all hardcoded; correct defaults but config-driven would let tier-1 vs tier-3 Anthropic accounts tune without rebuilding.
>
> **Files touched (uncommitted):**
> - `engine/curlWrapper/curlMultiDispatcher.h` + `.cpp` — gate, header parsing, retry routing, callbacks, debug snapshot, recursive mutex, atomic counters
> - `application/workflow/aiRequestPool.h` + `.cpp` — deferred deadline arm, deadline reset on retry, `ExtendDeadlineForOutputPath`, `RESET`/`extended deadline` instrumentation
> - `application/web/webServer.cpp` — `dispatcher_*` signals into `/api/debug/signals`
> - `application/workflow/workflowRuntimeManager.cpp` — fingerprint `ActiveRun` to drive dashboard broadcasts on task state changes
> - `scripts/buildJarvisCppDocu.py` — STNG/TASK/PROB rewritten to "security review" prompts (CRITICAL/HIGH/MEDIUM/LOW severity output schema, `### NONE\nNo security issues identified.` exit clause); label prefix `Sec:` instead of `Docu:`. Run with `python3 scripts/buildJarvisCppDocu.py` (no `--pack` so the `.jcwf` zip + `example/workflows/` mirror stay untouched).
> - `workflows/jarvisCppDocu/jarvisCppDocu.json` — regenerated from above
> - `config.json` — switched `API index` from 8 (Opus) to 9 (Sonnet) for the tests
>
> **Anthropic credit burn:** ~$2-5 across all 7 runs. Most failed requests were 429s (no compute charged); the 50-task partial run on Sonnet is the bulk of the spend.
>
> **Counter sanity check (final failed run 7):** `dispatcher_total_dispatched: 683`, `_completed: 135`, `_429s: 548`, `_retries_exhausted: 85` (cumulative across runs in this j9t lifetime), `_throttled: 50244` (cycle-rate count, not request count — every push-back per IO cycle).
>
> **Side find — dashboard WebSocket update bug** (separate from the live-progress C++ fix above): `DrainPendingBroadcasts()` only runs inside Crow's `onmessage` handler (`webServer.cpp:3720`), i.e. only when a client *sends* a message. The editor sends a 500ms poll which keeps it alive; the dashboard doesn't, so server-pushed broadcasts pile up in `m_PendingBroadcasts` and only flush when the dashboard sends something (e.g., manual refresh round-tripping through HTTP). **Comment in `jarvisAgent.cpp:599` explains why server-side timer drain wasn't done — Crow `send_text` overlap with the IO-thread drain caused silent message loss.** Fix needs to be on the dashboard side: add a 500ms `setInterval` in `dashboard/ui/src/hooks/useWebSocket.ts` that sends a ping (matches the editor pattern). Tracked in `JarvisAgent TODO List.md`. ~1 min change + dashboard `npm run build`.
>
> --- (prior session handoffs preserved below for historical reference) ---
>
> ## Session end — 2026-04-24 (merge-ready hand-off)
>
> **State:** branch `refactor/ai-dispatch`. All refactor acceptance criteria met, full regression green, stress test green, new cross-workflow concurrency test green. Safe to merge to `main` once CI is green. Delete this file as part of the merge commit (per JC's earlier instruction — all long-term content already in `doc/architecture.md`, `doc/JC_Workflow_Specification.md`, `doc/jcwf_generation_guide.md`).
>
> **Fixes landed today (on top of yesterday's uncommitted work):**
> 1. **Concurrency bug** (the only real merge blocker): `WorkflowRuntimeManager::m_ActiveRuns` switched from `std::vector<ActiveRun>` to `std::vector<std::unique_ptr<ActiveRun>>` — stable element addresses across `push_back` keep `&workflowDefinition` references inside worker lambdas alive. Mechanical change across ~15 access sites. Locked down with new `test/dispatch/test_cross_workflow_parallel.py` (fires N=12 parallel adhoc runs and verifies each lands its own output in its own folder — green at N=25 in local stress).
> 2. **TUI paint regression + Crow noise:** Crow's default logger wrote straight to stderr, bypassing ncurses and overpainting the status rows, and produced 3×/minute "Could not start adaptor: ssl/tls alert certificate unknown" warnings from untrusted-cert clients (browser tabs). Installed a `crow::ILogHandler` shim in `webServer.cpp` that routes Crow through `LOG_CORE_*` (so entries land in log.txt + ncurses LOG window like everything else) and silences the specific benign-but-noisy adaptor warning. Defensive: `SanitizeForCurses` now maps C0 control chars (newline/CR/etc.) to spaces so embedded tracebacks in log payloads can't reposition the ncurses cursor.
> 3. **Mermaid fence-strip regression:** `StripWholeReplyFence` had eaten the ```mermaid fence around AI-generated flowcharts in `vehicleTroubleshootingGuide`, leaving raw Mermaid source to render as text in the PDF. Added a language-tag keep-list (`mermaid`, `dot`, `plantuml`, `graphviz`, `latex`, `tex`, `markdown`, `md`) that preserves the fence when the content is a diagram/markdown-authored block — Haiku's accidental `cpp`/`python`/bare-fence wrapping is still stripped.
> 4. **Structured-output template wiring:** `InjectUpstreamOutputs` extended to parse any `.json` file in an upstream ai_call's `m_OutputValues` and flatten it into `{{A.json.PATH}}`, symmetric with cloud tasks' `response.json` handling. Unlocks structured output as a first-class template source for downstream tasks.
>
> **JCWF upgrades showcasing structured output:**
> - `vehicleTroubleshootingGuide` — 3 ai_call tasks now emit `{title, mermaid}` JSON validated by schema; combiner script owns the ```mermaid fence wrapping so the fence-strip regression above can't recur for this workflow regardless of heuristic. Tightened TASK instructions + defensive regex normalization in the python combiner for Haiku's occasional `--|` (missing `>` arrow) quirk.
> - `redmineTriageBot` — `ai_classify` emits `{user_id: enum["5","6"]}`, `update_issue.assigned_to_id` reads `{{ai_classify.json.user_id}}` via the new `.json.PATH` resolver.
> - `snowflakeQueryDemo` — `ai_analyze` emits `{verdict: enum[STRONG|MODERATE|WEAK]}`, SQL INSERT uses `{{ai_analyze.json.verdict}}`.
> - `goKartComplianceCheck` — `assessRequirement` emits `{verdict, summary, cost_estimate_eur, difficulty}`, Polarion update uses `[{{json.verdict}}] {{json.summary}}`.
> - `gitHubIssueDemo` — `ai_triage` emits `{category, priority, next_action, labels_ok}`, comment body composed from the fields.
> - `aiCarMaintenancePipeline` — already structured; verified end-to-end.
>
> All 5 upgraded JCWFs plus `.md` docs copied back to `example/workflows/`.
>
> **Test results (2026-04-24):**
> - 31-call stress burst (27 example workflows + 4 adhoc generate/build pipeline) in one tool-call message: **0 unexpected failures** (1 deliberate exampleMakefile5 shell failure handled by Rule A; 1 expected negative-path inputResolutionTest without context, re-run green with context).
> - Hermetic dispatch suite: `test_schema_covers_parser`, `test_direct_dispatch_signals`, `test_envelope_empty_body_rejected`, `test_testinterface_hermetic`, `test_relaxed_env_warnings`, new `test_cross_workflow_parallel` (N=25 parallel) — **all green**.
> - Peak WebSocket broadcast burst: 115 queued, drained cleanly.
> - `ai_structured_submissions: 1+`, `ai_schema_validation_retries: 0`, `ai_schema_validation_failures: 0` — schema validator never needed to retry in any of the runs.
>
> **TODO updates (same commit as the merge):**
> - `JarvisAgent TODO List.md` §5g: 60-second `WaitingExternal` timeout entry marked resolved (the refactor already used 300 s default + 120 s ai_call floor); Haiku fence quirk entry marked with mitigation-in-place, contract-tests entry annotated with today's additions.
> - §5g contract-tests checkbox kept open for post-1.0 slices (live-backed retry/chunking/markitdown, automated assistant tool-call tests).
>
> **Not touched (deliberate, post-merge or post-1.0):**
> - §5d repo-layout hygiene (`code/` subtree move, `.npm-tools/` + `jarvis_agent.example.env` deletion, Docker-file relocation).
> - §5h Bedrock + Azure OpenAI adapters.
> - §5e / §5f tool-calling + Claude Code PoC.
> - Open §5g follow-ups: chunking fan-out, JCWF schema gap-close, GenerateAsync wire-up to `kJcwfSchemaJson`, EventCategoryAi consumers beyond the aggregate LED.
>
> --- (prior session handoffs preserved below for historical reference) ---
>
> ## Session handoff — 2026-04-23 → 2026-04-24 (fresh session read this first)
>
> **Where we are:** branch `refactor/ai-dispatch`, several uncommitted changes in the working tree (see "Uncommitted work" below). j9t is stopped. Merge to main has NOT happened yet (blocked on resolving the concurrency bug below or deciding to defer it).
>
> **Today's wins (2026-04-23):**
> - Closed §5g follow-ups: §5g tasks 1–10 resolved (most were stale — already in 62a55be), `#9 replayTranscript.py` cancelled per JC.  Task #5 shipped the TUI status window (2-line: edition + LEDs + last-runs; sealed-keys hint appended).  Task #6 added 5 new `test/dispatch/` contract tests (signals, hermetic TestInterface, relaxed env, output-schema roundtrip, chunking, markitdown).  Task #8 shipped 3 live-backed E2E tests.
> - Side task: UI/TUI revision — dashboard-style signals, 2-banner variants in WorkflowsPanel, Cloud LED rework (only green when a connection has been confirmed via Test button or JCWF success), hidden Studio anon "admin" pill, auto-open MasterPasswordDialog on reconnect, consistent button spacing.
> - Side task: ran full connection regression across 14 cloud connectors. 12 green immediately, fixed 4 (greenmail creds, jira API token, azurite container start, sheets OAuth re-registration). OneDrive OAuth tokens survive restart now (added `OAuthTokenManager::HydrateFromKeyManager()` call after unlock in `HandleKeysUnlockPost`).
> - Side task: ran full example-workflow regression sweep. **25/27 green** (1 skipped — `hamburg-tourist-day-planner` is n8n-driven, not manual).
>
> **Bugs surfaced + fixed during testing today (all committed-to-worktree, not yet committed to git):**
> - `FileWatcher::WaitStop` logged "File watcher stopped" on every call; second call (from `~FileWatcher` after OnShutdown returns) fired after Core::Shutdown had torn down the TUI → stray log line on raw terminal. Fix in `application/file/fileWatcher.cpp`: invalidate `m_WatchTask` after first wait so subsequent `Stop()` is a no-op.
> - `WorkflowRuntimeManager::TickActiveRun` re-entered the "[workflow] run '{}' completed" log ~120×/run during the 2 s minimum-visibility hold (make-example auto-trigger made this very visible). Fix: early return from TickActiveRun when `m_Run.m_IsCompleted`.
> - `POST /api/settings/ai-interfaces` didn't populate `max_context_tokens` — stayed 0, chunking never fired for REST-created Test interfaces. Fix: accept optional `max_context_tokens` in body, else fall back to newly-public `ConfigParser::EngineConfig::ResolveMaxContextTokensFromModel`.
> - OAuth token persistence across restart was broken (`HydrateFromKeyManager` ran at startup before unlock, saw empty provider map, never re-ran). Fix: call `HydrateFromKeyManager` from `HandleKeysUnlockPost` after successful unlock.
> - OAuth error handling truncated Microsoft/Google error bodies below 500 chars — sometimes the actionable error was missed. Fix: include up to 1500 chars of response body in the logged error.
> - Dashboard: `AdminLoginDialog` could appear in Studio on transient 401s. Fix: guard `j9t-auth-required` handler and dialog condition with `isEngine`.
> - Dashboard Cloud LED was meaningless (read circuit-breaker map, empty until connections were exercised). Now keyed on `confirmed_healthy` per connection (set via `RecordSuccess` — both Test button and JCWF success increment it). Grey until ≥1 connection proved, then "Cloud: N healthy".
>
> **CONCURRENCY BUG found during regression sweep — serious, pre-existing (not a §5g regression):**
> - `WorkflowRuntimeManager::TickActiveRun` at line ~1841 captures `&workflowDefinition` **by reference** into a thread-pool lambda. `workflowDefinition` refs an element of `std::vector<ActiveRun> m_ActiveRuns`. When a new run is added while another is executing, `vector::push_back` can reallocate → all references dangle → thread pool worker reads the wrong workflow's base directory.
> - Evidence: during batch-4 of the regression sweep, jira's `create_issue` task wrote `response.json` to `workflows/cyber2/jiraIssueDemo/02_create/` (cyber2 happened to occupy the slot jiraIssueDemo used to). github's `list_issues` similarly scattered. Log line that proves it: `TaskPathResolver::ResolveTaskWorkingDirectoryPath … baseDirectoryAbsolute='workflows/cyber2' taskWorkingDirectoryRelative='jiraIssueDemo/02_create'`.
> - `gitHubIssueDemo` + `jiraIssueDemo` **run green when executed serially** (not yet verified — the solo rerun was cut short). Everything else in the sweep was green.
> - Fix options discussed, none applied yet:
>   - (a) `std::vector<std::unique_ptr<ActiveRun>>` — stable element addresses, smallest blast radius, touches every `m_ActiveRuns[idx].m_X` call site.
>   - (b) `std::list<ActiveRun>` — stable refs but breaks `m_ActiveRuns[idx]` API.
>   - (c) `WorkflowRun const workflowRunSnapshot = workflowRun;` pattern is already used for the RUN; extend it to WorkflowDefinition. But `WorkflowDefinition` is heavy (tasks map, dataflow, filters, control nodes) — copying per task submission costs cycles. Likely fine for human-scale workflows but worth measuring.
>   - (d) Pre-resolve everything the worker needs (workflowBaseDir, etc.) at submission time, capture those small derived values by value. Least-invasive but doesn't fix other uses of the dangling ref.
> - Recommendation: **(a) `std::vector<std::unique_ptr<ActiveRun>>`**. Stable addresses, minimal overhead, explicit about ownership. ~30–50 edit sites but mechanical.
>
> **Uncommitted changes on disk (all in `refactor/ai-dispatch` worktree):**
> - `application/file/fileWatcher.cpp` — WaitStop idempotency
> - `application/workflow/workflowRuntimeManager.cpp` — TickActiveRun early-return on completed
> - `application/web/webServer.cpp` — improved OAuth error body logging, `IsMcpConnected()` getter, `max_context_tokens` in POST /api/settings/ai-interfaces, OAuth rehydrate after unlock, `confirmed_healthy` in connection_health, Test-endpoint RecordSuccess/Failure wiring, two-banner Workflows panel, tab-bar button hiding in StatusBar
> - `application/web/webServer.h` — `IsMcpConnected()` declaration
> - `application/cloud/cloudCircuitBreaker.{h,cpp}` — `m_EverSucceeded` flag + `HasEverSucceeded()` accessor
> - `application/log/statusRenderer.{h,cpp}` — new 2-line dashboard-style status (rewrite)
> - `application/log/terminalManager.*` — (if touched — check)
> - `application/jarvisAgent.{cpp,h}` — StatusRenderer wiring, RuntimeSnapshot + LastRuns providers, EventCategoryAi wiring
> - `application/logging.md`, `application/README.md` — doc sweep for StatusRenderer rewrite
> - `engine/json/configParser.{h,cpp}` — `EngineConfig::ResolveMaxContextTokensFromModel` now public
> - `dashboard/ui/src/App.tsx`, `components/StatusBar.tsx`, `components/WorkflowsPanel.tsx`, `types.ts`, CSS — dashboard UI rework (all described above)
> - `workflow-editor/ui/src/App.tsx` — hide Workflows button when on the Workflows view
> - `test/dispatch/`: 5 new Python tests + fixture + README update
> - Memory updates: added `feedback_build_studio_debug.md`.
>
> **Regression sweep state (2026-04-23):**
> - 25/27 workflows pass. 2 failing (`gitHubIssueDemo`, `jiraIssueDemo`) — both blocked by the concurrency bug above. Symptom: `response.json` from a `jira_issue create` / `github_issue list_issues` task lands in the wrong workflow's folder, so downstream `{{taskId.json.PATH}}` template resolution fails.
> - Not investigated: test pass/fail when the same two run serially instead of in-batch.
> - `hamburg-tourist-day-planner` skipped per JC ("I need to run that one from n8n").
>
> **Tomorrow's plan (rough, JC to confirm):**
> 1. Decide on concurrency-bug fix (recommend option a) and apply it.
> 2. Re-run gitHubIssueDemo + jiraIssueDemo; confirm 27/27 green.
> 3. Walk uncommitted-changes list, review, commit in logical chunks.
> 4. CI on green → merge `refactor/ai-dispatch` → `main`. Watch `package-*` jobs fire on main for the first time.
> 5. Consider adding a regression test that exercises cross-workflow parallel runs with template resolution, to lock this class of bug down.
>
> **Local-env state:**
> - j9t is stopped.
> - `workflows/` has 27 JCWFs copied from `example/workflows/` (clean-slate regression baseline), plus `in.pdf`, plus `.history/` (stale auto-backups).
> - `queue/` empty. `_adhoc/` empty.
> - 14 cloud connections all registered and (previously confirmed) healthy — sheets/onedrive will survive restart thanks to the OAuth rehydrate fix.
> - MCP admin token used today: `mcp_8bd14deb8d4353ef39622d47b07d3f991e593efedd6e934328717b9cacda28b8` (may have rotated; ask JC if auth fails).
>
> --- (pre-today handoff preserved below for reference) ---
>
> ## Session handoff — 2026-04-21 → 2026-04-22 (fresh session read this first)
>
> **Where we are:** branch `refactor/ai-dispatch`, HEAD `62a55be` ("AI dispatch refactor (second part)") pushed to origin. Clean working tree.
>
> **Tomorrow's plan (JC):** merge `refactor/ai-dispatch` → `main` once CI is green.
>
> **Before merging, do this:**
> 1. **Delete this file.** JC's instruction: "we can remove it before we move everything onto the main branch." All long-term info has already been merged into `doc/architecture.md`, `doc/JC_Workflow_Specification.md`, and `doc/jcwf_generation_guide.md`.
> 2. Check CI on `62a55be` — feature-branch smoke builds only (`build-linux` / `build-macOS` / `build-windows`). The `package-*` jobs are gated to `refs/heads/main` via `if: github.ref == 'refs/heads/main'` and will only fire on the post-merge push.
> 3. After merge, the `package-*` jobs will run on `main` for the first time with the refactor's changes — watch them.
>
> **Refactor wins that are fully tested:**
> - Envelope-direct dispatch (`AiRequestPool::Submit(env, cb)`) — automated: `test/dispatch/test_envelope_empty_body_rejected.py` + `test_api4_anthropic_live.py`.
> - API4 Anthropic adapter — live Opus round-trip.
> - API1 OpenAI, chunking + reduce pass, auto-markitdown, fence-strip, debug signals, event broadcasting, model-name fallback table, 120 s `WaitingExternal` floor, late `RegisterPendingWorkflowTask` — manually validated this session.
> - **Structured output E2E on a workflow-bound task** — validated via `example/workflows/aiCarMaintenancePipeline.jcwf` (classify step declares `output_schema` with enum {engine, tires, rephrase}, downstream `CarMaintenanceTask` parses the JSON via simdjson). End-to-end green: `ai_structured_submissions: 1`, zero retries, zero failures.
> - **TestInterface hermetic fixture** — `POST /api/settings/ai-interfaces` + adhoc JCWF routed via `params.provider`, canned reply from `test/dispatch/fixtures/hermetic_reply.txt` landed byte-exact. Zero network calls.
>
> **Bugs surfaced + fixed during testing (all in `62a55be`):**
> - `expectedOutputPath` used `.output.txt` even when `output_schema` was declared, but Submit writes `.output.json` → workflow-bound structured tasks would have hung in `waiting_external` forever. Fix: `aiCallTaskExecutor.cpp` derives `.json` when `m_OutputSchemaJson` is non-empty.
> - TestInterface short-circuit wrote `<stem>.output.txt` but never called `OnOutputFileCreated` → workflow-bound tasks using Test interface hung. Fix: `aiRequestPool.cpp` Test branch now signals completion symmetrically with the real-provider path.
> - Four `webServer.cpp` handlers (create / update / list / **save**) silently downgraded `api_type: "Test"` and `api_type: "API4"` to `"API1"`. The save handler was the nastiest — persisted the corruption to disk, so a restart loaded API4 entries back as API1 and breakage only surfaced on the second run. Fix: all four handlers extended with explicit cases.
>
> **Open follow-ups to add to `JarvisAgent TODO List.md` after merge:**
> - Live-backed E2E tests for schema-validation retry, chunking, and markitdown (currently manual-only).
> - Automated hermetic test driving `InterfaceType::Test` (the interface exists, validated by hand, but no regression test asserts the path).
> - Bedrock (SigV4, `InterfaceType::API5`) + Azure OpenAI — already in TODO §5h.
> - `replayTranscript.py` tool — already in TODO.
>
> **Local-env gotchas worth remembering:**
> - `config.json` in the working tree matches upstream (reverted pre-commit — the local Test interface addition and the API4-corrupted entries were left out of the commit). If you want to re-exercise the Test interface in a fresh session, POST it via `/api/settings/ai-interfaces` with `api_type: "Test"` and `url: /home/beaumanvienna/dev/jarvisAgent/test/dispatch/fixtures/hermetic_reply.txt`, then `/save`. The save-handler fix in `62a55be` means it will persist as `"API": "Test"` correctly.
> - Use `python3` on the CLI. `make` has `MAKEFLAGS=-j32` set; never pass `-j`.
> - The previous session's MCP admin token may have rotated — ask for a fresh one if auth fails.

---

This document is the implementation plan for a typed, schema-validated, retry-aware AI dispatch layer on top of the existing libcurl multi + HTTP/2 transport. Transport is not modified.

---

## 1. Decisions (locked)

| Id | Decision |
|---|---|
| A | `AiInvocation` is a pure data struct; no inheritance. |
| B | `IRequestBuilder` is introduced in this refactor, symmetric to `ReplyParser`. |
| C | `ReplyParser` base class is extended with five virtuals (§5). |
| D | Structured output ships both modes: native `json_schema` (OpenAI / Gemini) and forced-tool shim (Anthropic). Per-interface selection. |
| E | Full slice for 1.0: envelope, structured output, transcript, determinism, TestInterface. |
| F | JCWF schema fully gap-closed in this refactor. Contract test enforces *schema ⊇ parser*. |
| G | Direct dispatch. Queue-folder `FileWatcher` retired from runtime-initiated `ai_call`. |
| H | New `EventCategoryAi` bitfield flag. |
| +1 | STNG / CNTX / TASK are optional. Dispatch if combined body has ≥1 non-whitespace char; PROB remains required. PROV (provider sidecar) is optional — when absent the default interface from `config.json` applies. |
| +2 | Markitdown auto-conversion of office files preserved as-is. |
| +3 | Chunking becomes structure-aware (markdown-section boundaries); per-interface `max_context_tokens` config. |

---

## 2. End-state architecture

```
JCWF ai_call task
  ↓  AiCallTaskExecutor
       ├─ template-resolve {{...}} vars
       ├─ write STNG / CNTX / TASK / PROB / PROV files to queue folder (disk-first)
       │    PROV = provider sidecar (interface name, model, url, key_name, api_type)
       ├─ markitdown-convert any office files in queue folder
       ├─ structure-aware chunker (if body > interface max_context_tokens)
       └─ construct 1 AiInvocation per PROB × chunk
            (m_InterfaceName mirrors PROV content; envelope is load-bearing, PROV stays for replay/debug)
  ↓  AiRequestPool::Submit(envelope)          [direct in-process call]
  ↓  IRequestBuilder::BuildBody(envelope)      [per-provider]
  ↓  CurlMultiDispatcher                       [HTTP/2, unchanged]
  ↑  ReplyParser                               [per-provider, extended]
  ↑  AiReply
  ↓  AiRequestPool::OnReply(handle, reply)     [direct callback]
       ├─ if output_schema: parse+validate with simdjson; retry on failure (≤ output_retries)
       ├─ write <prob>.output.{json|txt}
       └─ write <prob>.transcript.json
  ↓  AiRequestPool::TryPopCompletion()         [existing non-blocking poll]
  ↓  workflow runtime tick picks up the completion

Event bus (fire-and-forget, observability only):
  AiCallStartedEvent → AiCallRetryingEvent → AiCallCompletedEvent | AiCallFailedEvent
  Category: EventCategoryAi
```

---

## 3. Scope

**In scope**
- `AiInvocation` envelope + `AiReply` typed result.
- Direct dispatch (`AiCallTaskExecutor → AiRequestPool::Submit`); retire queue-folder `FileWatcher`.
- Relaxed env rule (STNG / CNTX / TASK optional).
- `IRequestBuilder` abstraction + 4 concrete builders (API1, API2, API3, API4).
- `ReplyParser` base extended with 5 virtuals; API4 (Anthropic) reply parser added.
- Structured output via `output_schema` / `output_retries` on `ai_call`. Two modes (native + shim).
- simdjson-based Draft 2020-12 subset schema validator.
- Output file contract: `<prob>.output.json` when schema set, else `<prob>.output.txt`.
- Editor-side `AiJcwfService::GenerateAsync` uses schema-enforced output against compiled-in JCWF schema.
- Extract `doc/jcwf.schema.json`; fully gap-close against parser; contract test.
- Premake prebuild step compiles schema + generation guide into generated headers.
- Determinism defaults (`temperature=0`, optional `seed`, `system_fingerprint` logging).
- `<prob>.transcript.json` per call.
- Structure-aware chunking + per-interface `max_context_tokens`.
- `TestInterface` as `InterfaceType::Test` for no-network integration tests.
- `EventCategoryAi` + 4 lifecycle events.

**Out of scope** (deliberate)
- Native LLM tool-calling (Assistant + JCWF `ai_call`) — tracked in `JarvisAgent TODO List.md` §5e, post-1.0.
- Claude Code / agent-of-agents PoC — §5f, post-1.0.
- Any change to libcurl multi / HTTP/2 transport, adaptive rate limiting, 429 backoff, or `CurlMultiDispatcher` internals.
- Streaming responses.

**Preserved unchanged**
- Disk-first philosophy: every input and output on disk, every call replayable.
- Queue-folder convention: one STNG / one CNTX / one TASK / many PROB / one PROV per `ai_call` task folder. Fan-out = one envelope per PROB.
- PROV sidecar: written per dispatch, captures which interface / model / URL / key_name / api_type was used. Envelope carries the same info; PROV is kept for debugging and transcript replay. **PROV is write-only from the dispatch code path** — it is never read back during execution. Only replay tooling (`tools/replayTranscript.py`, §9) reads PROV to reconstruct an envelope from disk. This keeps the envelope as the unambiguous source of truth at runtime.
- `max inflight ai calls` throttle (existing config field, default 1000) — preserved. Applies to `AiRequestPool::Submit` the same way it applied to the old file-event-driven path; envelope construction and dispatch are separate, so the throttle's queueing and rejection behavior are unchanged.
- Markitdown auto-conversion of office files in queue folders.
- Script-file watcher (`m_ScriptFileWatcher`) — independent, unchanged.
- JCWF `file_watch` triggers — continue to work on arbitrary paths declared in the JCWF. **Implementation change**: they gain their own dedicated `FileWatcher` instance (`m_TriggerFileWatcher`) owned by `TriggerEngine`, populated via `AddPath` / `RemovePath` on trigger bind/unbind. Previously rode on the queue-folder watcher de-facto — so this is a correctness improvement as well as a decoupling.
- `FileWatcher` class itself stays; only the queue-folder *instance* (`m_FileWatcher`) is retired.
- `AiRequestPool::TryPopCompletion` non-blocking polling model consumed by the workflow runtime tick.

---

## 4. Types

```cpp
// application/workflow/aiInvocation.h — new
namespace AIAssistant
{
    enum class MessageRole { System, User, Assistant };

    struct Message
    {
        MessageRole m_Role;
        std::string m_Content;
    };

    struct AiSettings
    {
        double m_Temperature = 0.0;
        std::optional<int64_t> m_Seed;
        std::optional<int32_t> m_MaxTokens;
    };

    struct RetryPolicy
    {
        int m_HttpMaxAttempts = 3;             // transport flakiness (network, 5xx, 429)
        int m_OutputSchemaMaxAttempts = 3;     // output quality (schema mismatch) — per PROB
        int m_TotalMaxAttempts = 10;           // hard ceiling across both budgets, prevents runaway compounding
        std::chrono::milliseconds m_BackoffMs{500};
    };

    enum class StructuredMode { None, NativeJsonSchema, ForcedToolShim };

    struct AiInvocation
    {
        std::string m_InterfaceName;                         // resolves to ApiInterface in config.json
        std::optional<std::string> m_ModelOverride;
        AiSettings m_Settings;
        std::vector<Message> m_Messages;
        std::optional<std::string> m_OutputSchemaJson;       // raw schema source
        StructuredMode m_StructuredMode = StructuredMode::None;
        std::chrono::milliseconds m_Timeout{120000};
        RetryPolicy m_Retry;
        std::filesystem::path m_QueueFolder;
        std::string m_ProbName;
        std::string m_ProvName;                              // PROV sidecar filename (for replay; optional)
        std::optional<int32_t> m_ChunkIndex;                 // when chunked, 0..N-1; nullopt otherwise
        std::optional<int32_t> m_ChunkCount;
    };

    struct AiUsage
    {
        int32_t m_InputTokens = 0;
        int32_t m_OutputTokens = 0;
        int32_t m_TotalTokens = 0;
    };

    struct AiError
    {
        enum class Kind { None, Http, Parse, SchemaValidation, Timeout, Transport, Provider };
        Kind m_Kind = Kind::None;
        int m_HttpStatus = 0;
        std::string m_Message;
    };

    struct AiReply
    {
        enum class Kind { Text, Structured, Error };
        Kind m_Kind = Kind::Error;
        std::string m_Text;
        std::string m_StructuredJson;           // when Kind == Structured; store raw for re-parse
        AiError m_Error;
        AiUsage m_Usage;
        std::string m_FinishReason;
        std::string m_SystemFingerprint;
    };
}
```

---

## 5. Extended `ReplyParser` base

```cpp
// application/json/replyParser.h — modified
class ReplyParser
{
public:
    virtual ~ReplyParser() = default;
    virtual size_t HasContent() const = 0;
    virtual std::string GetContent(size_t index = 0) const = 0;

    // New virtuals — providers that lack a concept return empty/default.
    virtual AiError GetError() const = 0;
    virtual AiUsage GetUsage() const = 0;
    virtual std::string GetFinishReason() const = 0;
    virtual std::string GetSystemFingerprint() const = 0;
    virtual std::optional<std::string> GetStructuredOutput() const = 0;

    static std::unique_ptr<ReplyParser> Create(InterfaceType const&, std::string const& json);
};
```

All four concretes (`ReplyParserAPI1..4`) implement all five. Providers with no native concept:
- `GetSystemFingerprint()` — returns `""` for API2 (when absent), API3, API4.
- `GetStructuredOutput()` — returns `std::nullopt` unless the provider emitted structured content (native json_schema response, forced-tool call).

---

## 6. `IRequestBuilder` abstraction

```cpp
// application/json/requestBuilder.h — new
class IRequestBuilder
{
public:
    virtual ~IRequestBuilder() = default;
    virtual std::string BuildBody(AiInvocation const&) const = 0;
    virtual std::string GetEndpointPath() const = 0;
    virtual std::unordered_map<std::string, std::string> GetExtraHeaders() const = 0;
    virtual bool SupportsNativeJsonSchema() const = 0;
    virtual bool SupportsForcedToolShim() const = 0;
    static std::unique_ptr<IRequestBuilder> Create(InterfaceType const&);
};
```

Concrete classes:
- `RequestBuilderAPI1` — OpenAI chat.completions. Native schema supported.
- `RequestBuilderAPI2` — OpenAI Responses API. Native schema supported.
- `RequestBuilderAPI3` — Gemini `generateContent`. Native schema via `responseSchema`.
- `RequestBuilderAPI4` — Anthropic `/v1/messages`. Forced-tool shim only (no native schema).

Structured-mode selection rule: when `AiInvocation.m_OutputSchemaJson` is set, builder picks the best supported mode for its provider (native > shim); if neither is supported, dispatcher falls back to prompted-and-validate.

---

## 7. Schema validator

`application/json/schemaValidator.{h,cpp}` — new. simdjson-based Draft 2020-12 subset:

Supported keywords:
- `type` (`string` | `number` | `integer` | `boolean` | `object` | `array` | `null`)
- `properties`, `required`, `additionalProperties`
- `items`
- `enum`
- `minimum`, `maximum`, `minLength`, `maxLength`, `pattern`
- `oneOf`, `anyOf`
- `$ref`, `$defs`

Rejected keywords (explicit error, so gaps don't fail silently): anything not in the list above.

Public API:

```cpp
struct ValidationError
{
    std::string m_Path;        // JSON pointer
    std::string m_Message;
};

struct ValidationResult
{
    bool m_Ok;
    std::vector<ValidationError> m_Errors;
};

class SchemaValidator
{
public:
    explicit SchemaValidator(std::string schemaJson);
    ValidationResult Validate(std::string const& documentJson) const;
    ValidationResult Validate(simdjson::dom::element const& doc) const;
    static std::string FormatErrorsForModel(std::vector<ValidationError> const&);
};
```

Used both on the AI reply path (`AiRequestPool::OnReply`) and on the editor-side JCWF generation path (`AiJcwfService::GenerateAsync`).

---

## 8. Phases

Phases are ordered by dependency. Each phase ends with a docs sweep.

### Phase 1 — Envelope + direct dispatch + relaxed env

**Goal.** Introduce `AiInvocation` / `AiReply`, route runtime-initiated `ai_call` through `AiRequestPool::Submit(envelope)` directly, retire queue-folder `FileWatcher`, relax env completeness. PROV sidecar writing preserved (for replay/debug); no longer load-bearing for dispatch.

**Files — add**
- `application/workflow/aiInvocation.h` (types from §4).
- `application/workflow/aiReply.h` (if split from above).

**Files — modify**
- `application/workflow/aiCallTaskExecutor.{h,cpp}` — construct `AiInvocation` after writing queue files; call `AiRequestPool::Submit(envelope)` directly. Warn (not error) on missing STNG / CNTX / TASK. Error only if combined body has no non-whitespace char or PROB missing. PROV sidecar: continue writing it (resolved from task's `api_interface` field, or the global default); envelope's `m_InterfaceName` is the source of truth for dispatch — PROV is the disk mirror.
- `application/workflow/aiRequestPool.{h,cpp}` — new `Submit(AiInvocation)` and `OnReply(handle, AiReply)`. Remove `OnProbFileEvent` / `OnOutputFileCreated` entry points. Completion queue + `TryPopCompletion` unchanged.
- `application/session/sessionManager.{h,cpp}` — remove `FileAddedEvent` consumer; remove silent-abort branch (current `:742-747`). Becomes an in-process helper called from `AiCallTaskExecutor`, not a file-event consumer. File-categorizer logic moves with it.
- `jarvisAgent.cpp` — remove queue-folder `FileWatcher` (`m_FileWatcher`) construction (`:161-162`), shutdown (`:800-867`), and the `OnEvent` → `TriggerEngine::NotifyFileEvent` bridge at `:616` (events now originate from the new `TriggerEngine`-owned watcher, not from a shared queue watcher). `m_ScriptFileWatcher` untouched. `GetQueueFileWatcher()` getter at `jarvisAgent.h:78` removed.
- `application/workflow/triggerEngine.{h,cpp}` — own a new `std::unique_ptr<FileWatcher> m_TriggerFileWatcher`. On `AddFileWatchTrigger`, `AddPath(normalizedPath)`; on removal, `RemovePath`. `NotifyFileEvent` is now called from a subscription *to this* watcher, not from `JarvisAgent::OnEvent`. File-watch triggers work on any declared path — no longer limited to `queue/`.
- `application/workflow/adhocWorkflowManager.{h,cpp}` — drop the `FileWatcher* m_QueueWatcher` member, the constructor parameter, `AddPath`/`RemovePath` calls, and `Stage()`/`OnRunCompleted()`/`Reap()`/`Init()` watcher bookkeeping. Adhoc `ai_call` goes through the same direct-dispatch path as runtime-initiated calls.
- `application/web/webServer.cpp:596` — no longer reads `GetQueueFileWatcher()`; adhoc manager constructed without it.

**Files — delete (or inline)**
- File-categorizer free-standing event consumer, if it becomes dead after SessionManager collapses.

**Contract tests — add to `test/dispatch/`**
- `test_envelope_construction.py` — ai_call task with all four env files → envelope populated correctly; single warning-free log.
- `test_relaxed_env_warnings.py` — ai_call with missing STNG / CNTX / TASK (each combination) → one warning per missing category, dispatch proceeds, succeeds.
- `test_empty_body_rejected.py` — ai_call with only whitespace in every file → envelope construction fails with explicit error, no HTTP dispatch.
- `test_prov_sidecar_written.py` — ai_call with explicit `api_interface` → PROV file on disk matches envelope's `m_InterfaceName`; ai_call with no `api_interface` → PROV reflects global default interface.
- `test_prov_absent_falls_back.py` — delete PROV from queue folder before dispatch — envelope's `m_InterfaceName` drives dispatch regardless (PROV is not load-bearing).
- `test_direct_dispatch_no_filewatcher.py` — start j9t; verify `queue/` folder is not under any watch (`/api/debug/signals` reports no queue watchers); run a workflow; completes end-to-end.
- `test_manual_drop_retired.py` — drop STNG/CNTX/TASK/PROB files manually into `queue/` — no dispatch happens (feature retired). Run same content via `ai_call` task — dispatches normally.
- `test_file_watch_trigger_arbitrary_path.py` — JCWF with `file_watch` trigger on a path *outside* `queue/` (e.g. `/tmp/j9t-trigger-test/`). Touch a file at that path → workflow fires. Previously would not have worked (trigger received no events).
- `test_file_watch_trigger_inside_queue.py` — regression: JCWF with `file_watch` trigger on a path inside `queue/` still fires. No behavioral change for existing users.
- `test_adhoc_direct_dispatch.py` — adhoc `ai_call` submitted via MCP → dispatches directly, no `FileWatcher::AddPath` involvement (adhoc manager no longer holds a watcher reference).

Both editions (Studio + Engine) run all tests.

**Docs — update at end of phase**
- `doc/JC_Workflow_Specification.md` §3.3.6 — STNG / CNTX / TASK marked optional. Add note: "Richer context produces better results; a warning is logged when these are omitted." PROV sidecar documented: generated per dispatch, optional (default interface applies when absent), kept on disk for replay — not load-bearing for dispatch.
- `doc/JC_Workflow_Specification.md` — removal of manual queue-drop semantics (if documented anywhere).
- `doc/architecture.md` — diagram + prose updated for direct dispatch.
- `doc/api-endpoints.md` — no change expected; verify.
- `application/workflow/doc/todo.md` "future refactors" section — close §5c (Option E) entry with pointer to §5g / this doc.
- `JarvisAgent TODO List.md` §5c — mark as landed as part of §5g.

**Acceptance**
- All existing JCWF end-to-end tests pass unchanged.
- Manual drop no longer triggers dispatch; logged once at startup that feature is retired.
- `python3 test/run_tests.py --all` green on Studio + Engine.

---

### Phase 2 — Provider abstractions + API4 (Anthropic)

**Goal.** Symmetric `IRequestBuilder` for all four providers; extended `ReplyParser` base; Anthropic `/v1/messages` integration.

**Files — add**
- `application/json/requestBuilder.h` (interface from §6).
- `application/json/requestBuilderAPI1.{h,cpp}`
- `application/json/requestBuilderAPI2.{h,cpp}`
- `application/json/requestBuilderAPI3.{h,cpp}`
- `application/json/requestBuilderAPI4.{h,cpp}`
- `application/json/replyParserAPI4.{h,cpp}` — Anthropic parser.

**Files — modify**
- `application/json/replyParser.h` — add 5 virtuals from §5.
- `application/json/replyParserAPI1.{h,cpp}` — implement new virtuals.
- `application/json/replyParserAPI2.{h,cpp}` — implement new virtuals.
- `application/json/replyParserAPI3.{h,cpp}` — implement new virtuals.
- `application/json/replyParser.cpp` — `Create()` dispatches for API4.
- `engine/json/configParser.{h,cpp}` — add `InterfaceType::API4`; recognize in parser.
- `application/session/sessionManager.cpp` — route through `IRequestBuilder::Create(...)` instead of inline body assembly.
- `vendor/curl/curlWrapper.{h,cpp}` — add `AuthStyle::AnthropicXApiKey` (`x-api-key:` + `anthropic-version:` headers).
- `workflow-editor/ui/src/views/AiManagerView.tsx` — add "API4 (Anthropic Messages)" option in the interface dropdown.
- `packaging/config.json.example` — add commented-out API4 interface example.

**Contract tests**
- `test_request_builder_api1..4.py` — known-good envelope → expected HTTP body shape per provider (snapshot tests).
- `test_reply_parser_virtuals.py` — all four parsers return non-crashing values for GetError / GetUsage / GetFinishReason / GetSystemFingerprint / GetStructuredOutput on canned responses.
- `test_api4_anthropic_live.py` — gated behind `--with-ai`; round-trip exampleMakefile against Claude Sonnet.

**Docs**
- `doc/JC_Workflow_Specification.md` — add API4 to the supported interface list.
- `doc/jarvisagent.md` / `.1` / `.html` — add API4 to AI setup section.
- `doc/api-endpoints.md` — AI interface test endpoint lists API4 as valid.
- `README.md` — "supported providers" list gains Anthropic.

**Acceptance**
- All four providers exercisable end-to-end through an unchanged JCWF (only `api_interface` changes).
- Regression suite green; no API1/2/3 behavioral changes.

---

### Phase 3 — Structured output + schema validator

**Goal.** `output_schema` / `output_retries` on `ai_call`; simdjson validator; native schema mode + forced-tool shim; `.output.json` contract; per-PROB retry.

**Files — add**
- `application/json/schemaValidator.{h,cpp}` (from §7).

**Files — modify**
- `application/workflow/workflowJsonParser.{h,cpp}` — parse `output_schema` (raw JSON preserved) + `output_retries` on ai_call tasks.
- `application/workflow/workflowTypes.h` — fields on `TaskDef` for the above.
- `application/workflow/workflowValidator.{h,cpp}` — (a) reject dataflow references of the form `{{tasks.X.output.field}}` when task X has no `output_schema`; (b) pre-validate that the declared `output_schema` parses and uses only supported keywords.
- `application/json/requestBuilderAPI{1,2,3}.cpp` — inject `response_format` / `responseSchema` when `m_StructuredMode == NativeJsonSchema`.
- `application/json/requestBuilderAPI4.cpp` — forced-tool shim: define `output` tool with schema as parameters; `tool_choice: {"type":"tool","name":"output"}`.
- `application/workflow/aiRequestPool.cpp` — `OnReply` path: if `m_OutputSchemaJson` set, parse with simdjson, validate, on failure append user message with error and re-submit (count against `m_OutputSchemaMaxAttempts`). Write `<prob>.output.json` on success; write `<prob>.output.txt` on free-text path.
- `application/workflow/aiCallTaskExecutor.cpp` — pick structured mode based on interface capabilities + presence of schema.

**Example workflow update**
- `example/workflows/aiCarMaintenancePipeline/*` — classify step declares `output_schema: {type:"string", enum:[...]}`. Downstream `buildManual` consumes `<prob>.output.json`.

**Contract tests**
- `test_schema_validator_positive.py` / `test_schema_validator_negative.py` — Draft 2020-12 subset correctness (all supported keywords).
- `test_schema_validator_unsupported_keyword.py` — explicit error on `allOf`, `not`, `format`, etc.
- `test_output_schema_retry.py` — mock provider emits invalid JSON → second attempt succeeds → exactly 2 dispatches logged, `.output.json` written.
- `test_output_schema_retry_exhausted.py` — provider keeps failing → task fails with `AiError{kind=SchemaValidation}`, N+1 dispatches, `.transcript.json` has full retry chain.
- `test_output_file_contract.py` — ai_call with schema → `.output.json`; without schema → `.output.txt`; never both.
- `test_structured_field_ref_rejected.py` — workflow validator rejects `{{tasks.foo.output.bar}}` when foo has no schema.
- `test_forced_tool_shim_api4.py` — API4 request body uses forced-tool shape when schema set.

**Docs**
- `doc/JC_Workflow_Specification.md` — new section under §3.3.6 for `output_schema` / `output_retries` (declaration, scope = task-level applied per PROB, validation behavior, retry budget). Explicitly note: "Schemas use a Draft 2020-12 *subset*; unsupported keywords (`allOf`, `not`, `format`, `dependencies`, ...) are rejected at JCWF load time, not at AI-reply time — so authors see errors before a run starts, never during one."
- `doc/jcwf_generation_guide.md` — "JSON unless told otherwise" convention for new `ai_call` tasks; enum example.
- `example/workflows/aiCarMaintenancePipeline.md` — updated narrative reflects schema-enforced classify step.

**Acceptance**
- Existing free-text `ai_call` tasks unaffected (no `output_schema` → legacy behavior).
- Structured ai_call tasks produce `.output.json` with validated content.
- Per-PROB retry budget independent; one bad PROB does not starve siblings.

---

### Phase 4 — JCWF schema gap-close + editor generator

**Goal.** Extract embedded schema, diff against parser, close all gaps, add parser↔schema contract test, compile schema + generation guide into headers, wire `AiJcwfService::GenerateAsync` to schema-enforced output.

**Files — add**
- `doc/jcwf.schema.json` — extracted from `doc/JC_Workflow_Specification.md` §9, updated to current state.
- `tools/generateEmbeddedHeaders.py` — reads `doc/jcwf.schema.json` + `doc/jcwf_generation_guide.md`, writes `application/json/jcwfSchema.generated.h` and `application/json/jcwfGenerationGuide.generated.h`. Both generated headers contain `inline constexpr char const* kJcwfSchemaJson = R"JSON(...)JSON";` / `kJcwfGenerationGuide`.
- `test/dispatch/test_schema_covers_parser.py` — contract test. Walks every field read by `workflowJsonParser` (via a documented allowlist file `test/dispatch/parser_fields.txt` generated/audited once) and asserts each is declared in `jcwf.schema.json`. Fails if the parser reads a field the schema doesn't describe.

**Files — modify**
- `premake5.lua` — prebuild step invoking `tools/generateEmbeddedHeaders.py`. Outputs gitignored. Runs before C++ compile.
- `.gitignore` — `application/json/jcwfSchema.generated.h`, `application/json/jcwfGenerationGuide.generated.h`.
- `doc/JC_Workflow_Specification.md` §9 — replace the embedded schema with a one-line reference: "See `doc/jcwf.schema.json` for the full JCWF JSON Schema."
- `doc/jcwf.schema.json` — (generated at extraction time) brought up to match parser:
  - Add `sub_workflow` task type.
  - Add `output_schema` / `output_retries` on `ai_call`.
  - Audit every field: `queue_binding`, `filters`, `control_nodes`, `controlflow`, `defaults`, `base_directory`, `file_inputs`/`file_outputs`, dataflow `mapping`, webhook trigger params, cron `timezone`, retry policy, etc.
- `application/web/aiJcwfService.{h,cpp}`:
  - Remove `LoadGenerationGuide()` file-search + placeholder fallback; use `kJcwfGenerationGuide` constant.
  - `GenerateAsync()` sets `AiInvocation.m_OutputSchemaJson = kJcwfSchemaJson` and `m_StructuredMode = NativeJsonSchema` (or shim, per interface capability).
  - Multi-stage pipeline (decompose → generate → review) — each stage declares its own schema (decompose → `{tasks:[...]}`; generate → full JCWF schema; review → `{issues:[...]}`).
  - `ExplainAsync` and `FixFailedScriptAsync` stay free-text; migrate them to envelope only.
- `application/json/schemaValidator.cpp` — add `$ref` / `$defs` resolution (required by JCWF schema).

**Contract tests — add**
- `test_schema_covers_parser.py` (described above).
- `test_generate_with_schema.py` — `GenerateAsync("a simple one-task shell workflow")` → produced JCWF validates against `kJcwfSchemaJson` and passes `WorkflowValidator`.
- `test_generate_retry_on_schema_mismatch.py` — mock AI first returns malformed JCWF → retry → second attempt valid → saved.
- `test_prebuild_generates_headers.py` — clean build, inspect that both `.generated.h` files are produced and non-empty.
- `test_schema_validator_refs.py` — `$ref` / `$defs` resolution correctness.

**Docs**
- `doc/JC_Workflow_Specification.md` — §9 replaced; the parser-gap entries audited during extraction get their own sections if missing.
- `doc/jcwf_generation_guide.md` — reviewed against new schema-driven generation flow; update any passages that assume free-text output.
- `DEVELOPMENT.md` — add short section on the prebuild step + how to regenerate after editing schema/guide.
- `application/workflow/doc/todo.md` — note schema-covers-parser contract test added.

**Acceptance**
- Clean build from scratch produces both generated headers.
- `test_schema_covers_parser.py` green.
- Editor "Generate" produces a schema-valid JCWF on canonical prompts, and recovers via retry on one deliberate malform.

---

### Phase 5 — Determinism + transcripts

**Goal.** Config-driven determinism defaults; `<prob>.transcript.json` per call.

**Files — add**
- `application/workflow/aiTranscript.{h,cpp}` — tiny helper for appending request/response turns to a JSON array on disk.

**Files — modify**
- `engine/json/configParser.{h,cpp}` — add fields:
  - `determinism.temperature` (double, default 0.0)
  - `determinism.seed` (optional int64)
  - `determinism.record_system_fingerprint` (bool, default true)
- `application/workflow/aiCallTaskExecutor.cpp` — apply defaults to `AiInvocation.m_Settings` unless the task overrides.
- `application/workflow/aiRequestPool.cpp` — write transcript entry at `Submit` (request turn) and at `OnReply` (response turn; include `usage`, `system_fingerprint`, `finish_reason`). One file per PROB: `<prob>.transcript.json`.
- `packaging/config.json.example` — include a `determinism` block with defaults.

**Contract tests**
- `test_determinism_defaults_applied.py` — config sets temp=0, seed=42 → sent in request body.
- `test_per_task_override.py` — JCWF `settings.temperature: 0.7` overrides config default.
- `test_transcript_written.py` — `<prob>.transcript.json` exists, contains ≥2 turns (request + response), valid JSON.
- `test_transcript_retry_trail.py` — schema retry produces a transcript with all attempt turns in order.

**Docs**
- `doc/JC_Workflow_Specification.md` — new section: "Determinism and transcript" (config fields, per-task override, transcript format).
- `doc/architecture.md` — note the transcript artifact alongside `.output.{json|txt}`.
- `packaging/config.json.example` — inline comments document determinism block.

**Acceptance**
- Re-running the same workflow twice with `seed` set produces identical replies (provider permitting — OpenAI honors it; Gemini/Anthropic approximate).
- Every completed `ai_call` run produces one transcript per PROB.

---

### Phase 6 — Structure-aware chunking

**Goal.** Replace the current context-blind chunker with a markdown-structure-aware splitter. Each provider advertises a context limit in config.

**Placement rationale.** Chunking lives at the executor layer (called from `AiCallTaskExecutor`, not from `AiRequestPool`). The executor already owns the 1:N fan-out contract for per-PROB dispatch; extending it to per-chunk keeps `AiRequestPool::Submit` strictly 1:1 (one envelope → one HTTP call → one reply). Making chunking AI-aware (reads `max_context_tokens` per interface) is deliberate — chunk sizing is provider-dependent, so the layer that holds the interface selection is the right place.

**Files — add**
- `application/content/markdownSectionSplitter.{h,cpp}` — parse markdown, build a tree of sections by heading level (`#` / `##` / `###`), produce leaf sections in document order. No external dependencies — hand-rolled scanner over line starts.
- `application/content/chunkPlanner.{h,cpp}` — given (body, max_tokens, prompt_overhead), return N chunks preferring whole-section boundaries; subdivide a section only when it alone exceeds budget (fall through: `#` → `##` → `###` → paragraph → sentence).

**Files — modify**
- `engine/json/configParser.{h,cpp}` — `ApiInterface` gains `max_context_tokens` (int, default provider-specific; required on user-added interfaces). Documented defaults:
  - OpenAI GPT-4-family: 128000
  - OpenAI GPT-5-family: 200000 (conservative until provider publishes)
  - Gemini 2.5: 1000000
  - Anthropic Claude Sonnet/Opus: 200000
- `application/workflow/aiCallTaskExecutor.cpp` — after markitdown conversion, estimate body size (chars ÷ 4 → tokens). If over budget, call `chunkPlanner`; emit one `AiInvocation` per chunk with `m_ChunkIndex` / `m_ChunkCount` set. All chunks share the prompt template.
- `application/session/sessionManager.cpp` — chunk aggregation logic: chunked responses concatenated into a single `.output.txt` (or for schema case, require schema to be `{type: array}` and concatenate; else chunking is rejected with clear error for that task).
- Existing chunking path in `sessionManager.cpp` (the line-based splitter) — removed.
- `application/workflow/aiTranscript.cpp` (from Phase 5) — when a PROB produces multiple dispatches due to chunking, write `<prob>_chunk<N>.transcript.json` per dispatch plus a `<prob>.transcript.json` summary listing the chunk trails. If this phase lands before Phase 5, capture the filename convention as a Phase-5 follow-up note.
- `workflow-editor/ui/src/views/AiManagerView.tsx` — `max_context_tokens` field with sensible provider-based defaults auto-filled on select.

**Contract tests**
- `test_section_splitter.py` — fixtures: small doc, h1-only, h1+h2 mix, malformed headings, code fences (headings inside fenced blocks ignored).
- `test_chunk_planner_whole_section.py` — body fits → one chunk, unchanged.
- `test_chunk_planner_split_at_h1.py` — body over budget, h1 sections each fit → one chunk per h1.
- `test_chunk_planner_subdivide.py` — one h1 alone over budget → subdivided at h2; if still over, h3; etc.
- `test_chunk_context_limit_per_interface.py` — same body, different interface max → different chunk count.
- `test_markitdown_preserved.py` — `.docx` file in queue folder → auto-converted to markdown → chunked → dispatched.

**Docs**
- `doc/JC_Workflow_Specification.md` — new section: "Chunking." Describe section-aware behavior, context-limit config, fan-out model for chunked dispatch.
- `doc/architecture.md` — content pipeline block (markitdown → chunker → envelope) diagrammed.
- `README.md` — bullet update if the "supported formats" list mentions chunking.

**Acceptance**
- Large-document workflows (>context limit) run successfully and produce coherent concatenated output.
- No regression on small-document workflows (single envelope, same as today).

---

### Phase 7 — TestInterface

**Goal.** A no-network `InterfaceType::Test` backend for JCWF integration tests.

**Files — add**
- `application/json/requestBuilderTest.{h,cpp}` — stores envelope for inspection; returns a deterministic body.
- `application/json/replyParserTest.{h,cpp}` — no-op parser over pre-canned replies.
- `application/workflow/testInterfaceBackend.{h,cpp}` — routes dispatches to either:
  - **Canned mode:** fixture map `(interface, model, hash(messages)) → AiReply` loaded from `test/fixtures/testInterface/*.json`.
  - **Schema-driven mode:** when envelope carries `m_OutputSchemaJson`, synthesize a minimal schema-valid JSON reply.

**Files — modify**
- `engine/json/configParser.{h,cpp}` — recognize `InterfaceType::Test`; interface fields `fixture_path` (optional) and `schema_auto` (bool).
- `application/workflow/aiRequestPool.cpp` — short-circuit: when interface type is `Test`, bypass `CurlMultiDispatcher`, call `testInterfaceBackend::Dispatch(envelope)` synchronously on a worker thread (preserving the async callback contract).
- `test/run_tests.py` — accept `--test-interface` flag that rewrites config to swap the declared interface for a `Test` interface backed by fixtures.

**Contract tests**
- `test_canned_mode.py` — envelope matches fixture key → canned reply returned; mismatched key → clear error.
- `test_schema_auto_mode.py` — no fixture, schema set → synthesized reply is schema-valid.
- `test_suite_runs_without_network.py` — run the full JCWF test suite under `--test-interface` with no network egress; all pass.

**Docs**
- `doc/JC_Workflow_Specification.md` — brief note under "Supported interfaces" that `Test` exists, intended for integration tests only.
- `test/README.md` — how to use fixtures, how to regenerate canned replies from a real run.
- `DEVELOPMENT.md` — add the `--test-interface` flow to the contributor onboarding.

**Acceptance**
- `python3 test/run_tests.py --all --test-interface` green.
- Studio + Engine CI gains a no-network job that uses `TestInterface`.

---

### Phase 8 — Observability events

**Goal.** AI lifecycle events on the global `EventQueue` under a dedicated category. Consumers: TUI, dashboard WebSocket, Python hooks, debug signals.

**Files — add**
- `application/workflow/aiCallEvents.{h,cpp}` — four event types with the following payloads:
  - `AiCallStartedEvent` — task id, prob name, interface name, chunk index/count (if chunked).
  - `AiCallRetryingEvent` — task id, prob name, attempt number, last error (kind + message).
  - `AiCallCompletedEvent` — task id, prob name, interface, `AiUsage`, system fingerprint, finish reason.
  - `AiCallFailedEvent` — task id, prob name, `AiError` (kind + http status + message).

**Files — modify**
- `engine/event/event.h` — add `EventCategoryAi` to the category bitfield.
- `application/workflow/aiRequestPool.cpp` — post each event at its lifecycle moment. Fire-and-forget; never block dispatch on event delivery.
- `application/terminal/terminalManager.cpp` — status row consumer: subscribes to `EventCategoryAi`, updates "queries in flight" LED and per-task state.
- `application/web/webServer.cpp` — dashboard WebSocket consumer: forwards events as JSON to connected clients.
- `application/python/pythonEngine.cpp` — `BuildEventDict` handles the four new event types, matching the existing `FileAddedEvent` bridge pattern.
- `application/web/debugSignals.cpp` — expose counters per event type on `/api/debug/signals`.

**Contract tests**
- `test_ai_events_emitted.py` — run a workflow, tap `EventQueue` → observe Started → Completed in order per task; retry scenario observes Retrying.
- `test_ai_events_on_failure.py` — forced failure → Failed emitted with correct error kind.
- `test_ai_events_fire_and_forget.py` — slow consumer does not stall the dispatcher.
- `test_python_hook_ai_event.py` — Python hook registered on `EventCategoryAi` receives events with correct payload dict.

**Docs**
- `engine/event/event_system.md` — document `EventCategoryAi` and the four event types.
- `doc/architecture.md` — add the event bus row to the pipeline diagram.
- `doc/api-endpoints.md` — `/api/debug/signals` new fields.

**Acceptance**
- Every `ai_call` task produces at least Started + (Completed | Failed).
- TUI and dashboard both show live task state during an AI-heavy workflow run.

---

## 9. Global testing + acceptance

**Regression suite.** After every phase, the full existing test matrix must pass on Studio + Engine builds:
- `python3 test/run_tests.py --all`
- `python3 test/assistant/test_assistant.py --with-ai`
- Contract tests for all previous phases.

**Performance baseline.** Before Phase 1 lands, capture a baseline run of `portfolioDividendAnalysis` (60 parallel ai_calls). After each phase, the same run must complete within +5% of baseline wall-clock time. HTTP/2 multiplexing is the guarantee; any regression indicates a serialization leak in the new layer.

**Transcript replay tool.** By end of Phase 5, a `tools/replayTranscript.py` script takes a `<prob>.transcript.json` and re-emits the request body exactly, for debugging drift.

---

## 10. Post-landing

- Close `JarvisAgent TODO List.md` §5c (subsumed), §5g (landed).
- Update `JarvisAgent TODO List.md` §5e to note: "dispatch refactor shipped; tool-calling can now ride on top via `m_Tools` field on `AiInvocation` (to be added post-1.0)."
- Close `application/workflow/doc/todo.md` future-refactor entries for dispatch + chunker + envelope.
- Update `README.md` changelog.
- The `AiInvocation` envelope is the seam for future tool-calling, multi-turn, Claude Code orchestration — document this in `doc/architecture.md` so the extension points are visible.

---

## 11. File inventory (quick reference)

| Phase | Added | Modified | Deleted |
|---|---|---|---|
| 1 | aiInvocation.h, aiReply.h | aiCallTaskExecutor, aiRequestPool, sessionManager, jarvisAgent, triggerEngine (owns new file-watch instance), adhocWorkflowManager (drops watcher ref), webServer (drops GetQueueFileWatcher call) | queue-folder FileWatcher construction, silent-abort branch, GetQueueFileWatcher getter |
| 2 | requestBuilder + 4 concretes, replyParserAPI4 | replyParser + API1/2/3, configParser, sessionManager, curlWrapper, AiManagerView, config.example | — |
| 3 | schemaValidator | workflowJsonParser, workflowTypes, workflowValidator, requestBuilderAPI1/2/3/4, aiRequestPool, aiCallTaskExecutor, aiCarMaintenancePipeline | — |
| 4 | jcwf.schema.json, generateEmbeddedHeaders.py, parser_fields.txt | premake5.lua, JC spec §9, aiJcwfService, schemaValidator ($ref), .gitignore | `LoadGenerationGuide()` file-search path |
| 5 | aiTranscript | configParser, aiCallTaskExecutor, aiRequestPool, config.example | — |
| 6 | markdownSectionSplitter, chunkPlanner | configParser, aiCallTaskExecutor, sessionManager, AiManagerView | legacy line-based chunker |
| 7 | requestBuilderTest, replyParserTest, testInterfaceBackend, fixtures/ | configParser, aiRequestPool, run_tests.py | — |
| 8 | aiCallEvents | event.h, aiRequestPool, terminalManager, webServer, pythonEngine, debugSignals | — |

---

## 12. Dependencies between phases

```
Phase 1 (envelope + direct dispatch)
  ├─→ Phase 2 (provider abstractions + API4)
  │     └─→ Phase 3 (structured output)
  │           ├─→ Phase 4 (JCWF schema + editor)
  │           └─→ Phase 7 (TestInterface — IRequestBuilder + schema validator for auto-synth mode)
  ├─→ Phase 5 (determinism + transcript)
  ├─→ Phase 6 (chunking)
  └─→ Phase 8 (events)
```

Parallelizable: 5, 6, 8 all depend only on Phase 1 and can be developed concurrently with 2/3/4 if needed. Phase 7's canned-mode can land after Phase 2; full schema-driven auto-synth needs Phase 3.

Critical path: 1 → 2 → 3 → 4.
