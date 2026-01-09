# JarvisAgent TODO List

Source: the open points you provided (focused on `jarvisAgent.cpp/.h` and the `workflow/` folder).

---

## Go-live blockers (highest priority)

### ai_call architecture compliance
- [ ] Implement **per-request overrides** for `ai_call` (model / API index / request params) instead of only `prompt_template`.
- [ ] Fix **queue-binding path anchoring** so `STNG_` / `TASK_` / `CNTX_` files land under the **queue folder / subsystem directory**, not written “as-is” via `QueueFileRef.path`.
- [ ] Implement `queue_binding.prob_files` behavior:
  - [ ] If JCWF provides `prob_files` (inline content or string ref), **consume/write** them.
  - [ ] Decide how this interacts with the executor’s `PROB_<id>_<ts>.txt` generation (avoid conflicting sources of truth).
- [ ] Finalize **ai_call output semantics**:
  - [ ] Decide whether output slots contain **text** or **file paths** by default.
  - [ ] Make pool + runtime behavior match the chosen JCWF rule (and document it).

### Workflow graph validation (load-time)
- [ ] Enforce **version handling**: reject unknown **major** versions.
- [ ] Add **cycle detection at load time** (reject workflows with dependency cycles instead of waiting for runtime deadlock/no-progress).
- [ ] Parse and apply **root-level defaults** (currently not parsed; not merged into tasks).

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
- [ ] Implement `InternalTaskExecutor` (currently placeholder / unsupported).

---

## Refactor cleanup / safety

- [ ] Remove or hard-disable the old synchronous orchestrator fallback:
  - [ ] `JarvisAgent::OnUpdate()` fallback calling `WorkflowOrchestrator::RunWorkflowOnce()` when runtime manager is null.
  - [ ] Ensure no accidental reversion path that could reintroduce the `ai_call` deadlock risk.

---

## Notes / follow-ups (when the above is done)
- [ ] Update docs to match final behavior (JCWF spec + `aiCallArchitecture.md` alignment):
  - [ ] Clarify `doc` field accepted types (string vs array-of-strings) and implement parser support if required by the spec.
  - [ ] Clarify cron trigger timezone support and implement binder parsing/usage for `params.timezone`.
