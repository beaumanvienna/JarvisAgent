# JarvisAgent TODO List

Last reviewed: Feb 2026

---

## Go-live blockers (highest priority)

### Platform
- [x] ~~Fix **Windows build**~~ — guarded `<unistd.h>` behind `#ifndef _WIN32` in `terminalManager.cpp`; added `<io.h>`/`_write()`/`_fileno()` for MSVC via `RAW_STDERR` macro.
- [x] ~~Fix **macOS build**~~ — Apple Clang's libc++ lacks C++20 chrono timezone. Integrated Howard Hinnant's `date` library (`vendor/date`) which provides the same API cross-platform. `triggerEngine.cpp` now uses `date::` for timezone resolution on all platforms.

### ai_call architecture compliance
- [ ] Implement **per-request overrides** for `ai_call` (model / API index / request params) instead of only `prompt_template`.
- [x] ~~Implement `queue_binding.prob_files` behavior~~ — already implemented in `BuildProbTextFromQueueBinding` + `WriteInlineQueueBindingFiles` in `aiCallTaskExecutor.cpp`. `prob_files` (inline or file ref) are consumed, concatenated, and written into the executor's `PROB_<id>_<ts>.txt`. No conflict — same pipeline.
- [x] ~~Finalize **ai_call output semantics**~~ — file-path-only mode. Output slots always contain file paths, never raw text in memory. When no explicit `file_outputs` are declared, the source `.output.txt` created by the core engine is used as the default. Implemented in `BuildCompletionOutputs` / `AiRequestPool`.

### Workflow graph validation (load-time)
- [ ] Enforce **version handling**: reject unknown **major** versions (currently only checks empty).
- [x] ~~Add **cycle detection at load time**~~ — implemented in `workflowValidator.cpp`.
- [ ] Apply **root-level defaults** to tasks (parsed into `m_DefaultsJson` but not merged into tasks at runtime).

### Required input correctness (fail-fast)
- [ ] Implement **required input validation**: if `TaskIOField.required` is true and not resolved, fail before dispatch.

---

## Runtime execution gaps (core functionality)

### Modes and triggers
- [ ] Implement `mode: "per_item"` task expansion (iterator/instance expansion pipeline in `WorkflowRuntimeManager`).
- [ ] Implement **structure triggers** semantics (iterator extraction + task expansion and triggering).

### Dataflow and context resolution
- [ ] Implement `dataflow.mapping` evaluation (mapping object is parsed/stored but currently ignored).
- [ ] Implement **context-based input resolution** (from run context / params / defaults) where `DataflowResolver` has TODOs.

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

- [ ] **Port `web/index.html` to React** — replace the legacy dashboard with the React frontend.
- [x] ~~Remove old synchronous orchestrator fallback~~ — removed `WorkflowOrchestrator` usage from `jarvisAgent.cpp` and `webServer.cpp`. Trigger callback and API now require `WorkflowRuntimeManager`; null case logs error / returns 500.

---

## Notes / follow-ups (when the above is done)
- [x] ~~Update docs to match final behavior (JCWF spec + `aiCallArchitecture.md` alignment)~~:
  - [x] ~~Clarify `doc` field accepted types~~ — verified: root-level uses `ExtractRawJson` (handles string and array), task-level uses `ElementToString` (string only). Both match the spec.
  - [x] ~~Cron trigger timezone support~~ — implemented C++20 `std::chrono::zoned_time` in `ComputeNextFireTime`, parsed `params.timezone` in `WorkflowTriggerBinder`, added trigger config UI in editor.
  - [x] ~~README.md rewrite~~ — updated project description, added workflow editor screenshot, planned features (Docker, n8n).
