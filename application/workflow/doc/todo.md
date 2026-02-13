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
- [ ] Implement `mode: "per_item"` task expansion with **filter nodes** (CSV, text lines, Lucene query via Python bridge). See `application/workflow/doc/perItem_structureTriggers.md` for dev plan.
- [ ] **Create `text_lines` example workflow + documentation** — `EvaluateTextLines` is fully implemented in `filterEngine.cpp` but has no demo JCWF exercising it. Create an example workflow with a `.txt` input file, a `text_lines` filter, a per-item AI task, and a corresponding `.md` doc.
- [x] ~~Update **JC Workflow Specification** for filters + per_item~~ — bumped spec to v1.1: added §3.7 (Filters), `"filters"` root-level array, `"filter"` field on tasks, filter JSON Schema `$def`, query language reference, filter manifest freshness scheme, fan-out node description, security note for unbounded expansion.

### Dataflow and context resolution
- [ ] Implement `dataflow.mapping` evaluation (mapping object is parsed/stored but currently ignored).
- [ ] Implement **context-based input resolution** (from run context / params / defaults) where `DataflowResolver` has TODOs.

### Workflow housekeeping — "Clean" command
- [ ] Implement a **"Clean" command** for a given JCWF that deletes all **output artifacts** produced by running the workflow. Only output artifacts are deleted; source/input files are never touched.

  **What to delete:**
  1. **Queue task folders** — everything under `queue/<workflowId>/` (per-task subdirectories like `01_classifyQuestion/`).
  2. **Explicitly declared output files** — all `file_outputs` declared in shell/ai_call/python tasks, resolved from the JCWF definition (e.g. `myapp`, `*.o`, `*.a`).
  3. **AI call `.output.txt` files** — predictable naming convention (`<probfile>.output.txt`).
  4. **Per-item / filter output artifacts** — fan-out generated files, matched via wildcard (e.g. `PROB_*_001.txt`, per-item output files).
  5. **Working directory artifacts** — if `working_directory` is set and differs from `queue/<workflowId>`, clean that too.

  **What to NOT delete:**
  - Source files referenced as inputs (e.g. `lib1.cpp`, `main.cpp`, `message.txt`).
  - The JCWF file itself.
  - The `scripts/` folder or any read-only assets.
  - Filter source files (e.g. the CSV input, not the generated per-item outputs).

  **Implementation:**
  - **Backend**: `DELETE /api/workflows/<id>/clean` endpoint in `webServer.cpp`.
  - **Logic**: `WorkflowRuntimeManager::CleanWorkflow(workflowId)` — reads the JCWF definition, enumerates task output paths and queue folders, deletes them.
  - **UI**: "Clean" button (broom icon) in the editor toolbar; shows confirmation dialog before executing.

### Reliability features
- [ ] Implement **retries/backoff** from `RetryPolicy` in `WorkflowRuntimeManager`.
- [ ] Enforce `timeout_ms` for **non-ai_call** tasks (`python` / `shell` / `internal`) at runtime.

---

## Executor completeness

- [ ] Implement JCWF I/O semantics for `PythonTaskExecutor`:
  - [ ] Pass resolved inputs into Python execution.
  - [ ] Collect outputs back into workflow output slots for downstream dataflow.
- [x] ~~Implement `InternalTaskExecutor`~~ — has working implementation in `internalTaskExecutor.cpp`.

---

## Refactor cleanup / safety

- [x] ~~**Unify template substitution syntax: `${...}` → `{{...}}`**~~ — created shared `templateEngine.h/.cpp` with `ExpandTemplate()` supporting strict (shell) and lenient (ai_call) modes. Migrated `ShellTaskExecutor`, `AiCallTaskExecutor`, and `DataflowResolver` to use the shared engine. Updated all 5 example JCWF files and 3 documentation files. No `${...}` template references remain in the codebase.

- [ ] **Port `web/index.html` to React** — replace the legacy dashboard with the React frontend.
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

## Notes / follow-ups (when the above is done)
- [x] ~~Update docs to match final behavior (JCWF spec + `aiCallArchitecture.md` alignment)~~:
  - [x] ~~Clarify `doc` field accepted types~~ — verified: root-level uses `ExtractRawJson` (handles string and array), task-level uses `ElementToString` (string only). Both match the spec.
  - [x] ~~Cron trigger timezone support~~ — implemented C++20 `std::chrono::zoned_time` in `ComputeNextFireTime`, parsed `params.timezone` in `WorkflowTriggerBinder`, added trigger config UI in editor.
  - [x] ~~README.md rewrite~~ — updated project description, added workflow editor screenshot, planned features (Docker, n8n).
