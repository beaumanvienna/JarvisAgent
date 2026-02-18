# JarvisAgent TODO List

Last reviewed: Feb 2026

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

## Notes / follow-ups (when the above is done)
- [x] ~~Update docs to match final behavior (JCWF spec + `aiCallArchitecture.md` alignment)~~:
  - [x] ~~Clarify `doc` field accepted types~~ — verified: root-level uses `ExtractRawJson` (handles string and array), task-level uses `ElementToString` (string only). Both match the spec.
  - [x] ~~Cron trigger timezone support~~ — implemented C++20 `std::chrono::zoned_time` in `ComputeNextFireTime`, parsed `params.timezone` in `WorkflowTriggerBinder`, added trigger config UI in editor.
  - [x] ~~README.md rewrite~~ — updated project description, added workflow editor screenshot, planned features (Docker, n8n).
