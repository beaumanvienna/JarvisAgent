# JarvisAgent TODO List

Last reviewed: Mar 2026

**Build command:** `make config=release && make config=debug`

---

## Go-live blockers (highest priority)

### Platform
- [x] ~~Fix **Windows build**~~ — guarded `<unistd.h>` behind `#ifndef _WIN32` in `terminalManager.cpp`; added `<io.h>`/`_write()`/`_fileno()` for MSVC via `RAW_STDERR` macro.
- [x] ~~Fix **macOS build**~~ — Apple Clang's libc++ lacks C++20 chrono timezone. Integrated Howard Hinnant's `date` library (`vendor/date`) which provides the same API cross-platform. `triggerEngine.cpp` now uses `date::` for timezone resolution on all platforms.

### ai_call architecture compliance
- [x] ~~Implement **per-request overrides** for `ai_call`~~ — per-task AI interface selection via `api_interface` field in editor inspector. Each interface specifies model, URL, API type, and `key_name`. Dropdown shows all configured interfaces plus "default (global API index)".
- [x] ~~Implement `queue_binding.prob_files` behavior~~ — already implemented in `BuildProbTextFromQueueBinding` + `WriteInlineQueueBindingFiles` in `aiCallTaskExecutor.cpp`. `prob_files` (inline or file ref) are consumed, concatenated, and written into the executor's `PROB_<id>_<ts>.txt`. No conflict — same pipeline.
- [x] ~~Finalize **ai_call output semantics**~~ — file-path-only mode. Output slots always contain file paths, never raw text in memory. When no explicit `file_outputs` are declared, the source `.output.txt` created by the core engine is used as the default. Implemented in `BuildCompletionOutputs` / `AiRequestPool`.

### Workflow graph validation (load-time)
- [x] ~~Enforce **version handling**~~ — parser now splits `"major.minor"`, rejects unknown major (only `1` accepted), warns on minor > known (`1.0`). Malformed or non-numeric versions are rejected. Validator retains defense-in-depth empty check.
- [x] ~~Add **cycle detection at load time**~~ — implemented in `workflowValidator.cpp`.
- [x] ~~Apply **root-level defaults** to tasks~~ — `defaults` parsed into `WorkflowDefaults` struct (`timeout_ms`, `retries`). Post-parse merge loop applies them to every task whose field is still zero. AI defaults deferred to dispatch time. Raw JSON kept for serialization.

### Required input correctness (fail-fast)
- [x] ~~Implement **required input validation**~~ — was already implemented in `DataflowResolver` + `workflowValidator`. Added `m_ErrorMessage` to `TaskResolvedInputs` so the specific missing input name propagates to `TaskInstanceState.m_LastErrorMessage` (previously only logged, not surfaced).

---

## Runtime execution gaps (core functionality)

### Modes, filters, and per_item expansion
- [x] ~~Implement `mode: "per_item"` task expansion with **filter nodes**~~ — full pipeline: `DispatchFilterEvaluation`, `FanOutPerItemChildren`, `AggregatePerItemResults` in `workflowRuntimeManager.cpp`. Filter engine supports CSV, text_lines, Lucene/Polarion. Includes manifest freshness and skip-if-fresh logic.
- [x] ~~**Create `text_lines` example workflow + documentation**~~ — `example/workflows/bookSummaryPipeline.jcwf`: shell→python→text_lines filter→per_item ai_call→python combine. Demonstrates the full per_item pipeline.
- [x] ~~Update **JC Workflow Specification** for filters + per_item~~ — bumped spec to v1.1: added §3.7 (Filters), `"filters"` root-level array, `"filter"` field on tasks, filter JSON Schema `$def`, query language reference, filter manifest freshness scheme, fan-out node description, security note for unbounded expansion.

### Dataflow and context resolution
- [x] ~~Implement `dataflow.mapping` evaluation~~ — mapping values are injected into `resolvedInputs.m_StringValues` in `DataflowResolver`. Fixed parser to strip surrounding JSON quotes from string values so they are stored clean.
- [x] ~~Implement **context-based input resolution**~~ — full 3-step resolution chain in `DataflowResolver`: (1) dataflow edges, (2) `workflowRun.m_Context` lookup, (3) input-level `"default"` fallback. Task outputs auto-publish to context as `taskId.outputName` keys. `POST /api/workflows/<id>/run` accepts optional `{"context": {...}}` body to seed initial values. `TaskIOField.m_Default` parsed from JCWF `"default"` field.

### Workflow housekeeping — "Clean" command
- [x] ~~Implement a **"Clean" command**~~ — `DELETE /api/workflows/<id>/clean` endpoint + `WorkflowRuntimeManager::CleanWorkflow()` + "Clean" button in editor toolbar with confirmation dialog. Deletes queue task folders, declared `file_outputs`, and empty working directories.

### Reliability features
- [x] ~~Implement **retries/backoff** from `RetryPolicy`~~ — `TryScheduleRetry` in `workflowRuntimeManager.cpp`: linear backoff (`m_BackoffMs * attempt`), `m_RetryAfterTime` respected by dispatch loop, deadlock detector accounts for retry-pending tasks.
- [x] ~~Enforce `timeout_ms` for **non-ai_call** tasks (`python` / `shell` / `internal`) at runtime~~ — inactivity watchdog: `TaskWatchdog` atomic struct, shell executor uses `fork()/exec()/poll()` with stdout as implicit heartbeat + process group kill on timeout; REST `POST /api/task/<id>/heartbeat` for explicit heartbeats; python/internal get post-execution inactivity check. Spec §3.3.3 updated, bookSummaryPipeline demo added.

### Error branching / controlflow
- [x] ~~Implement **error branching** with Branch nodes and `expose_error_signal`~~ — `control_nodes` array + `controlflow` edges parsed in `workflowJsonParser.cpp`, validated in `workflowValidator.cpp` (DAG constraint includes controlflow edges). Runtime: `FireBranchIfReady` in `workflowRuntimeManager.cpp` activates selected path and skips unselected path. Re-activation of previously-skipped tasks handled (Skipped→Pending reset). Rule A: handled failures don't fail the run. Verified with `exampleMakefile5`: ai_call generates code with deliberate error → shell fails → branch_1 error path → ai_call_fix → shell_retry succeeds → branch_2 → shell_2 runs hello.

---

## Executor completeness

- [x] ~~Implement JCWF I/O semantics for `PythonTaskExecutor`~~ — already implemented: resolved inputs passed as Python **kwargs** via `PythonEngine::ExecuteWorkflowTask`; return `dict` collected into `TaskInstanceState.m_OutputValues`; `file_inputs`/`file_outputs` injected as positional keys (`input[0]`, etc.); `BuildOutputSlotMap` fills missing output slots. Verified with `bookSummaryPipeline` (`extractChapters.py`, `combineSummaries.py`).
- [x] ~~Implement `InternalTaskExecutor`~~ — has working implementation in `internalTaskExecutor.cpp`.

---

## Refactor cleanup / safety

- [x] ~~**Unify template substitution syntax: `${...}` → `{{...}}`**~~ — created shared `templateEngine.h/.cpp` with `ExpandTemplate()` supporting strict (shell) and lenient (ai_call) modes. Migrated `ShellTaskExecutor`, `AiCallTaskExecutor`, and `DataflowResolver` to use the shared engine. Updated all 5 example JCWF files and 3 documentation files. No `${...}` template references remain in the codebase.

- [x] ~~**Port `web/index.html` to React**~~ — replaced legacy `web/index.html` with React 18 + Vite + TypeScript dashboard (`dashboard/ui/`). Live WebSocket monitoring, workflow run controls, session manager table, status LEDs. Old `web/` folder deleted.
- [x] ~~Remove old synchronous orchestrator fallback~~ — removed `WorkflowOrchestrator` usage from `jarvisAgent.cpp` and `webServer.cpp`. Trigger callback and API now require `WorkflowRuntimeManager`; null case logs error / returns 500.

---

## AI Keys & multi-provider support

- [x] ~~**Google AI integration**~~ — Google Gemini works via OpenAI-compatible endpoint (`/v1beta/openai/chat/completions`) with API1 parser, Bearer auth, model `gemini-2.5-flash`. No new parser needed.
- [x] ~~**Free-text key names**~~ — AI Keys "Name" field changed from interface-constrained dropdown to free-text input (e.g. "openai", "google", "anthropic").
- [x] ~~**Eye icon on API Key input**~~ — password visibility toggle added to AI Keys page, matching `MasterPasswordDialog`.
- [x] ~~**Master password prompt on Save Encrypted**~~ — `ProvidersSettingsView` shows password modal when no cached password; stores in page-level state (cleared on navigate away). App-level password passed from startup unlock dialog.
- [x] ~~**Password validation before save**~~ — backend decrypts existing `keys.json.enc` with provided password before overwriting; wrong password returns HTTP 403 `"wrong_password"`. Frontend shows error in modal, stays open for retry.
- [x] ~~**`key_name` persistence in config.json**~~ — each AI interface can reference a named key; saved to config.json, omitted when empty.
- [x] ~~**Key dropdown default fix**~~ — AI Manager dropdown shows "— not set —" instead of falsely defaulting to first key alphabetically.

---

## Bugs

- [x] ~~**`POST /api/workflows/{id}/run` accepts non-existent workflow IDs**~~ — fixed: handler now validates workflow ID against registry and returns HTTP 404 immediately.

---

## Bug: JA hangs on shutdown when cleaning auto-triggered workflows

**Repro:** Start JA with all 9 test JCWF files in `workflows/`. Before the auto-triggered
workflows finish, request clean via the Python test runner (`run_tests.py`). JA times out
on all JCWFs and hangs on shutdown (never exits cleanly).

**Suspected cause:** The clean endpoint returns 409 when a workflow is running, but the
combination of multiple concurrent auto-triggered runs timing out may deadlock the
shutdown sequence (two-phase parallel subsystem shutdown + 6s watchdog safety net).

**Root causes found (3) and fixed:**

1. **WaitingExternal tasks never timed out in the runtime manager** — the `AiRequestPool` had its
   own timeout, but the deadlock detector gave a free pass to any run with `WaitingExternal` tasks,
   masking the real deadlock.
   - **Fix:** `TimeoutWaitingExternalTasks()` runs every tick before the deadlock detector.
     Uses per-task `timeout_ms` (or 5 min default). Stamps `m_WaitingExternalSince` on transition.

2. **Failed tasks didn't propagate to downstream dependents** — `IsTaskReady()` only passes on
   `Succeeded`/`Skipped`, so when an upstream task failed, all downstream tasks stayed `Pending`
   forever. If another branch still had a `WaitingExternal` task, the deadlock detector wouldn't fire.
   - **Fix:** `SkipDownstreamOfFailed()` does a BFS from the failed task and immediately marks all
     transitive dependents as `Skipped`. Called at all three failure points (task future threw,
     task execution failed, AI completion failed) plus from `TimeoutWaitingExternalTasks()`.

3. **Shutdown didn't clean up WaitingExternal tasks** — `OnUpdate()` stops running after
   `m_IsFinished = true`, so `m_CancelRequested` (set by `SignalStop()`) was never processed by
   `TickActiveRun()`. Orphaned `WaitingExternal` tasks could block `AiRequestPool::Shutdown()`.
   - **Fix:** `WaitStop()` now iterates all active runs, fails any remaining `WaitingExternal` tasks,
     and calls `requestPool->Forget()` on their AI request handles before clearing.

**Remaining investigation:**
- [x] ~~Check if `CleanWorkflow` or the 409 rejection path leaves the runtime manager in a bad state~~
  — Verified 2026-03-12: the 409 path is read-only (mutex-guarded scan of `m_ActiveRuns`), no state mutation.
- [x] ~~Reproduce with logging and confirm the hang is fixed~~
  — Verified 2026-03-12: started JA with 9 JCWFs, pressed `q` ~1s after start while all workflows
  were running. Shutdown completed in ~1.56s total (68ms after phase-1 signal). WaitStop() failed
  76 WaitingExternal tasks across 5 runs, curl abort callback killed all in-flight requests.
  No watchdog timer needed, no deadlock, clean exit.

---

## Run control — pause / resume / stop

Backend support for fine-grained run control. UI buttons and `doc/api-endpoints.md`
documentation are already in place (buttons disabled until backend is ready).

- [x] ~~Add `RunControlState` enum (`Running`, `Paused`, `Stopping`) to `WorkflowRun` in `workflowTypes.h`~~ — added `Paused` and `Stopping` to `WorkflowRunState` enum
- [x] ~~Implement `PauseRun(runId)` in `workflowRuntimeManager.cpp`~~ — `RequestPauseRun` sets `m_PauseRequested`, dispatch loop returns early
- [x] ~~Implement `ResumeRun(runId)`~~ — `RequestResumeRun` clears `m_PauseRequested`, sets state back to `Running`
- [x] ~~Implement `StopRun(runId)`~~ — `RequestStopRun` sets `m_StopRequested`, in-flight tasks finish, remaining skipped
- [x] ~~Add three route handlers in `webServer.cpp`~~ — `HandleWorkflowRunPausePost`, `HandleWorkflowRunResumePost`, `HandleWorkflowRunStopPost`
- [x] ~~Add `pauseRun()`, `resumeRun()`, `stopRun()` API calls in `workflow-editor/ui/src/api/workflows.ts`~~
- [x] ~~Remove `disabled` from Pause/Resume buttons in `WorkflowEditorView.tsx`~~ — added Stop/Pause/Resume/Cancel button row

---

## E2E testing — padded indices

~~Staged but uncommitted changes to `FilterEngine::AddPaddedIndices()`. Needs live
verification before committing.~~

- [x] ~~Build project (`make config=release verbose=1 && make config=debug verbose=1`)~~
- [x] ~~Run `aiCarMaintenancePipeline` workflow — verify per_item CSV filter + padded index filenames~~
- [x] ~~Run `portfolioDividendAnalysis` workflow — verify per_item CSV filter + padded index filenames~~
- [x] ~~Commit staged changes after successful E2E~~

Verified 2026-03-11: `portfolioDividendAnalysis` output files use 3-digit zero-padded
row numbers (e.g. `PROB_BNDX_002.txt`, `PROB_BAC_007.txt`, `PROB_BA_053.txt`).

---

## Workflow editor — recent features (Feb 2026)

- [x] ~~**Script path validation**~~ — `GET /api/scripts/check?path=...` endpoint validates that shell task command scripts exist and are executable. Frontend caches results and shows inline warnings on shell task nodes. Lexical path normalization rejects `..` traversal.
- [x] ~~**Shell task stdout/stderr capture**~~ — `ExecuteCommandWithWatchdog` uses 2 separate pipes; `ExecuteCommandWithCapturedOutput` redirects stderr to temp file. Full output written to `stdout.txt`/`stderr.txt` in task working directory. First 1024 chars stored in `TaskInstanceState` and exposed via REST API + WebSocket snapshot. Frontend shows hover tooltip (stderr in red, stdout below) and side panel display.

---

## Workflow editor testing (open)

The manual test plan in `workflow-editor/workflow-editor-test.md` covers 3 test workflows:
1. **exampleMakefile** — ai_call → shell (make)
2. **stockAnalyzerTop6** — filter + per_item fan-out → summary
3. **techTermGlossary** — 3-task ai_call chain

These should be re-run periodically after editor changes to catch regressions.
Additional test scenarios to cover:
- [x] ~~Shell task with stderr output (verify red text in tooltip + side panel)~~ — Verified 2026-03-12: `exampleMakefile4` with deliberate syntax error. Failed shell task shows red node glow, red "F" badge, hover tooltip with compiler errors in red. Downstream "run hello" correctly skipped.
- [x] ~~Shell task with >1024 chars output (verify truncation)~~ — Verified 2026-03-12: `truncationTest` workflow with 50-line stdout/stderr. Stdout cuts at exactly byte 1024 (line 12 "jumps ov"). Also fixed tooltip UX: `pointer-events: auto` + padding bridge so popup is scrollable and stays visible on mouse-over.
- [x] ~~Watchdog timeout path (task with low `timeout_ms` that hangs)~~ — Verified 2026-03-12: `watchdogTimeoutTest` workflow with `sleep 3600` script and `timeout_ms: 5000`. Task killed after exactly 5s, run analyzer shows "Task timed out (inactivity > 5000ms)". Stdout captured: "Starting... will hang now."
- [x] ~~Pause / Resume / Stop controls during a multi-task run~~ — Verified 2026-03-12: `pauseResumeStopTest` workflow (3×10s chained shell tasks). Pause shows ❚❚ badges + PAUSED banner + ▶ Resume button. Stop during step 1 → "■ Run stopped", steps 2&3 skipped. Also fixed: stale "R" badge (fetch final state on run exit), `WorkflowRunState::Stopped` terminal state, normal completion path preserving Stopping state, auto-trigger prevention via explicit triggers array.
- [x] ~~Clean command after a run (verify queue folders are deleted)~~ — Verified 2026-03-12: `DELETE /api/workflows/exampleMakefile4/clean` returned `{"ok": true}`, `queue/exampleMakefile4/` and all task subdirectories fully removed.

---

## Bug: exampleMakefile4 dependency code uses stale inputs

**Repro:** Run `exampleMakefile4` with queue outputs still present from a previous run.
The shell task (`run command make`) materializes the old `PROB_hello.output.txt` before
the AI task for the current run has finished writing its new output. As a result, `make`
compiles the stale `hello.cpp` instead of the freshly generated one.

**Timeline observed (2026-02-28):**
- 10:56:43 — workflow run started, `ai_call` + `ai_call_2` dispatched
- 10:56:45 — shell task copied stale `PROB_hello.output.txt` → `hello.cpp` (old content)
- 10:56:45 — `make` compiled successfully (stale code, no syntax error)
- 10:56:46 — AI wrote new `PROB_hello.output.txt` with deliberate syntax error (too late)

**Root cause found:** The path-based AI completion routing in `jarvisAgent.cpp` `OnEvent()`
(line ~456) had **no stale file guard**. When an ai_call task registered its expected output
path in `m_PendingByOutputPath`, the existing stale `PROB_hello.output.txt` from a prior
run could trigger a file event that `OnOutputFileCreated` matched — reading the **old
content** and marking the ai_call as `Succeeded` before the real AI response arrived.
The PROB-based completion path (for `PROB_<id>_<ts>` naming) already had a stale guard
(`fileTimestamp < startupTimestamp`), but the path-based path did not.

**Fix (applied):** Added `last_write_time` < `m_StartupTime` check to the path-based
completion block in `jarvisAgent.cpp`, matching the pattern already used by
`suppressTriggerEvent`. Stale `.output` files are now logged and ignored.

---

## Bug: PROV file regression after API3 integration ✅

**Repro:** `vehicleTroubleshootingGuide` workflow configured for OpenAI generates `PROV_provider.json`
with Google's API URL after the Gemini API3 integration. All AI tasks fail with HTTP 404.

**Root causes found (3) and fixed (2026-03-12):**

1. **PROV URL/api_type resolved from stale ProviderConfig** — `aiCallTaskExecutor.cpp` used
   `KeyManager::GetProvider()` which stores a single endpoint per provider name, ambiguous when
   multiple interfaces share the same key. Fixed: look up config.json interface by **name** (the
   unique identifier). JCWF `provider` field now uses interface names (e.g.
   `"api.openai.com/gpt-4.1-mini/API1"`). PROV file stores `key_name` as `"provider"` for
   SessionManager API key resolution.

2. **ReplyParser type mismatch** — `sessionManager.cpp` used `Core::g_Core->GetInterfaceType()`
   (global default API2) instead of the per-session `m_ApiType` (API1 from PROV file). Fixed: use
   `m_ApiType` to select the correct parser.

3. **Prerequisite check blocked auto-triggers** — `CheckAiProviderPrerequisites` in
   `workflowRuntimeManager.cpp` called `keyManager.GetProvider("api.openai.com/gpt-4.1-mini/API1")`
   which doesn't exist as a key name. Fixed: resolve interface name → `key_name` before KeyManager
   lookup. Falls back to treating `providerName` as a key name for legacy JCWFs.

**Files changed:** `aiCallTaskExecutor.cpp`, `sessionManager.cpp`, `workflowRuntimeManager.cpp`,
5 JCWFs in `example/workflows/`.

**Verified:** All 7 workflows succeeded (dashboard: 7 succeeded, 0 failed), including full
PDF generation pipeline for `vehicleTroubleshootingGuide`.

---

## Shutdown hang — RESOLVED (2026-03-12)

**Root cause:** `DrainPendingBroadcasts()` was only called from the WebSocket `onmessage` handler
(triggered by browser pings), never from `JarvisAgent::OnUpdate()`. The broadcast queue grew
unbounded — **33,152 peak messages in 4 minutes** of normal operation. At shutdown, Crow's I/O loop
had to process/discard thousands of pending async writes, causing the ~5s delay that triggered the
watchdog.

**Fix:** Added `m_WebServer->DrainPendingBroadcasts()` to `JarvisAgent::OnUpdate()` (`jarvisAgent.cpp`).
Queue now stays at 0, peak limited to ~162 (startup burst only). Confirmed with 2.5-hour soak test —
zero queue growth, shutdown is instant.

**Diagnostics retained:**
- [x] Raw `stderr` diagnostic before/after `app->OnShutdown()` in `engine.cpp`
- [x] Raw `stderr` diagnostic at every step inside `JarvisAgent::OnShutdown()` in `jarvisAgent.cpp`
- [x] WebSocket accumulation stats (`totalConnects`, `totalDisconnects`, `peakClients`,
  `peakPendingBroadcasts`) in `webServer.h`, logged at shutdown + connect/disconnect
- [x] WebSocket stats exposed via `GET /api/status` for live monitoring
- [x] WebSocket forced close + `m_Server.stop()` + thread join logging in `webServer.cpp`

---

## ~~Bug: WebSocket log line buffer grows unbounded when no client is connected~~ ✅

~~`EnqueueLogLine()` in `webServer.cpp` pushes every log line into `m_PendingLogLines`
with no size cap. When no WebSocket client is connected, log lines accumulate in memory
indefinitely.~~

**Fix (applied):** Two-layer defense in `EnqueueLogLine()`:
1. `m_ClientCount` atomic (lock-free mirror of `m_Clients.size()`, updated in `onopen`/`onclose`) — skip buffering entirely when no WebSocket client is connected.
2. `kMaxPendingLogLines = 500` cap — if a client is connected but the drain lags, oldest lines are evicted.

Related: the prior shutdown hang (see "Shutdown hang — RESOLVED" above) was caused by
`m_PendingBroadcasts` growing unbounded — same pattern, different buffer.

---

## JCWF generation pipeline (AI → workflow)

- [x] ~~**`AiJcwfService` 5-stage pipeline**~~ — decompose → generate JCWF → generate Python scripts → validate → fix. Implemented in `aiJcwfService.cpp`. Each stage uses queue folder artifacts (STNG/TASK/CNTX/PROB files).
- [x] ~~**`WorkflowFileIndex`**~~ — scans `workflows/` at startup and before generation, indexes files by basename. Provides file inventory to the decompose prompt so the AI knows which input files exist on disk. Used by the validator to suggest path corrections for unreachable `file_inputs`.
- [x] ~~**Python hot-reload**~~ — `PythonEngine` evicts `sys.modules` entries for `scripts.*` before each import, ensuring AI-generated scripts are picked up immediately without restart.
- [x] ~~**`context` dict always passed**~~ — `PythonEngine::ExecuteWorkflowTask` always attaches the `context` dict (with `_file_input_0`, `_task_working_directory`, etc.) to kwargs. Previously only attached when the task explicitly declared a `context` input.
- [x] ~~**Generation prompts use `context` not `_context`**~~ — fixed hardcoded prompt strings in `aiJcwfService.cpp` and `jcwf_generation_guide.md` to match the actual kwarg name passed by the C++ executor.
- [x] ~~**Validator file_inputs path resolution**~~ — `ValidateFileInputReachability` resolves paths relative to `workflows/` base directory (via `TaskPathResolver::ResolveWorkflowBaseDirectory`), not bare `launchCwd`. Provides `WorkflowFileIndex` basename suggestions in fix hints.
- [x] ~~**`TaskPathResolver` extraction**~~ — `ResolveWorkflowBaseDirectory()` extracted from `workflowRuntimeManager.cpp` into shared `taskPathResolver.h/.cpp` for use by both the runtime and the validator.
- [x] ~~**E2E verified**~~ — `cyber2` workflow: OpenSSH log → Python parse → AI threat assessment. Generated, validated, fixed, and executed without manual edits. See `example/workflows/cyber2_e2e.md`.

---

## Notes / follow-ups (when the above is done)
- [x] ~~Update docs to match final behavior (JCWF spec + `aiCallArchitecture.md` alignment)~~:
  - [x] ~~Clarify `doc` field accepted types~~ — verified: root-level uses `ExtractRawJson` (handles string and array), task-level uses `ElementToString` (string only). Both match the spec.
  - [x] ~~Cron trigger timezone support~~ — implemented C++20 `std::chrono::zoned_time` in `ComputeNextFireTime`, parsed `params.timezone` in `WorkflowTriggerBinder`, added trigger config UI in editor.
  - [x] ~~README.md rewrite~~ — updated project description, added workflow editor screenshot, planned features (Docker, n8n).
