# JarvisAgent `ai_call` Architecture (Event‑Driven, Async Completion)

This document captures the **strategy decisions** we agreed on for reworking the workflow `ai_call` task so it uses the **JarvisAgent core + event system** (filesystem-driven), **not Python**, and completes asynchronously when the corresponding `*.output.txt` file arrives.

It is written to be a “single source of truth” for the design, so we can pick up later without re-deriving decisions.

---

## Strategy decisions (explicit)

1. **`ai_call` is asynchronous by nature**
   - The executor may **synchronously submit** work (write files + `PROB_*`), but **completion happens later** when the `.output.txt` file is created/modified.

2. **Completion is driven by the existing event system**
   - `FileWatcher → EventQueue → Core::Run → JarvisAgent::OnEvent`.
   - We route AI completion through **events**, not callbacks.

3. **Dedicated pool for AI requests**
   - Create a dedicated `AiRequestPool` (similar in spirit to `ChatMessagePool`), rather than reusing the chat pool.

4. **Unique filename correlation**
   - `PROB_<id>_<timestamp>.txt` for submission
   - `PROB_<id>_<timestamp>.output.txt` for completion
   - `<id>` is a monotonic counter that restarts each JarvisAgent start.
   - `<timestamp>` is a Unix/system clock timestamp (consistent with JarvisAgent startup timestamp filtering).

5. **Per-task overrides are allowed**
   - `ai_call` may specify overrides (model, API index, request parameters).
   - Overrides replace the default config **for that request only**.

6. **Workflow execution must not block main-thread event processing**
   - Avoid a design where a workflow run blocks inside `OnUpdate()` while waiting for file events (deadlock risk).
   - Preferred approach: a **tick-based workflow runtime** (see below).

---

## Why the current synchronous workflow loop cannot handle `ai_call`

### Current engine event flow (as implemented)
In `Core::Run()` the loop is effectively:

1. `app->OnUpdate()`
2. Pop events from `EventQueue`
3. Dispatch engine-level events
4. Forward unhandled events to `app->OnEvent(eventPtr)`
5. Render terminal, sleep

**Consequence:** If `app->OnUpdate()` blocks, **event delivery to `OnEvent()` is delayed**.

### Current workflow triggering path (as implemented)
`JarvisAgent::OnUpdate()` ticks `TriggerEngine`, and the trigger callback calls:

- `WorkflowOrchestrator::RunWorkflowOnce(...)` which runs synchronously and waits on task futures.

That is fine for tasks that complete on worker threads quickly.  
But `ai_call` completion depends on a **filesystem event** that is only delivered after `OnUpdate()` returns.

If a workflow run blocks inside `OnUpdate()` waiting for `ai_call` to “finish”, events cannot be processed → **deadlock**.

---

## Target behavior: `ai_call` task instance state machine

Within a workflow run, a single `ai_call` task instance goes through:

1. **Prepare environment**
   - Create per-request files:
     - `STNG_*`
     - `CNTX_*`
     - `TASK_*`
     - `PROV_*` (optional — provider override, never sent to AI)

2. **Submit**
   - Create `PROB_<id>_<timestamp>.txt` (the “go” signal)

3. **Wait for completion**
   - `FileWatcher` emits `FileAdded` / `FileModified`
   - JarvisAgent receives them in `OnEvent()`
   - The AI pool matches `*.output.txt` to the pending request

4. **Finalize**
   - Read `PROB_<id>_<timestamp>.output.txt`
   - Populate `taskState.m_OutputValues`
   - Mark task **Succeeded**

5. **Timeout / fail**
   - If output doesn’t appear by `timeout_ms`, mark **Failed**

**Key point:** steps (3)–(5) are driven by **events + periodic update**, not by blocking.

---

## Components and responsibilities

### 1) `AiCallTaskExecutor` (workflow layer)

**Role:** submission-only (synchronous “kickoff”), plus minimal bookkeeping.

**Responsibilities:**
- Apply **task overrides** (model/API index/parameters) if provided in the task definition
- Write STNG/CNTX/TASK files into the appropriate queue folder / subsystem directory
- Generate `{id, timestamp}` and write `PROB_<id>_<timestamp>.txt`
- Register the pending request in `AiRequestPool`
- Mark the task as “waiting for external completion”

**Required workflow state support:**  
Today you use `Pending / Ready / Running / Succeeded / Skipped / Failed` (based on usage in `WorkflowOrchestrator`).
To support `ai_call`, you need a **non-terminal “waiting” state** (name up to you, e.g. `WaitingExternal`), or another mechanism that prevents:
- the orchestrator from treating it as complete, and
- the workflow run from deadlocking.

This document does not assume the enum already exists; it’s a required addition.

---

### 2) `AiRequestPool` (new, application-level manager)

**Role:** the “truth” for pending AI requests and their completion.

A dedicated pool “similar to `ChatMessagePool`” but with workflow metadata.

**Responsibilities:**
- Allocate unique request IDs (monotonic counter reset on app start)
- Capture timestamps (system clock; consistent with startup timestamp filtering)
- Compute and store:
  - submission filename `PROB_<id>_<ts>.txt`
  - expected completion filename `PROB_<id>_<ts>.output.txt`
  - the relevant queue/subsystem directory
- Store request metadata needed to complete the workflow task:
  - workflow id + run id + task id (and later per-item index if needed)
  - a key/handle sufficient to locate the correct `TaskInstanceState` to finalize
- Handle **file events**:
  - if a file event matches an expected `*.output.txt`:
    - read output
    - mark request complete
    - finalize workflow task outputs/state
- Handle **timeouts**:
  - `Update()` called periodically from `JarvisAgent::OnUpdate()`
  - expire entries and mark corresponding tasks failed

**Relationship to `ChatMessagePool`:**
- `ChatMessagePool` tracks chat submissions and answers and broadcasts to websocket clients.
- `AiRequestPool` tracks workflow `ai_call` requests (broadcast optional, not required).

---

### 3) Workflow execution: tick-based runtime (required)

**Goal:** allow workflow progress **without blocking** the engine loop, so events can be delivered.

**Recommended approach:** introduce a `WorkflowRuntimeManager` (name flexible) ticked from `JarvisAgent::OnUpdate()`.

**Responsibilities:**
- Start a workflow run when a trigger fires (do not block)
- Each tick:
  - find ready tasks
  - dispatch tasks to the thread pool
  - collect completed futures without blocking the main thread for long durations
  - allow `ai_call` tasks to remain in waiting state until events complete them
- Mark workflow run completed when all tasks are in terminal states

**Trigger integration change:**
Instead of calling `WorkflowOrchestrator::RunWorkflowOnce()` directly from the trigger callback, the callback should request: **start run**.

---

## Event-system integration points (existing + additions)

### Existing (implemented)
- `FileWatcher` produces `FileAddedEvent`, `FileModifiedEvent`, `FileRemovedEvent`.
- Events flow: `EventQueue` → `Core::Run()` → `JarvisAgent::OnEvent()`.

### Existing `JarvisAgent::OnEvent()` routing (relevant)
Simplified order:

1. Shutdown `EngineEvent`
2. Extract file path + file event type
3. Notify `TriggerEngine` for file_watch triggers
4. Chat PROB output handling via `ProbUtils::ParseProbFilename(...)` and `ChatMessagePool::MarkAnswered(...)`
5. Forward remaining file events to `SessionManager`
6. Forward to `PythonEngine`

### Proposed addition for workflow `ai_call`
Add a step like the chat handling:

- If the file matches `PROB_<id>_<ts>.output.txt`:
  - `AiRequestPool->OnFileEvent(...)`
  - if consumed, return early

This keeps the pattern consistent: **file event → pool → finalize**.

---

## File naming and stale-file suppression (consistency)

You already suppress stale PROB files in `JarvisAgent::OnEvent()` by comparing the embedded timestamp with the app startup timestamp.

To keep this reliable:
- Use system clock time-since-epoch for `<timestamp>`.
- Prefer explicitly using **nanoseconds** (`duration_cast<nanoseconds>(...).count()`) so it matches startup timestamp logic.

---

## Overrides and configuration layering

- Base defaults come from `config.json` (API interfaces + selected API index).
- `ai_call` task may override model / API index / request params.

**Rule (agreed):** overrides apply **only to that request**, not globally.

---

## Output handling (guarantees)

When `*.output.txt` arrives:
- correlate by `{id, timestamp}`
- read output
- populate `TaskInstanceState.m_OutputValues`
- mark task terminal (Succeeded/Failed)

**Note:** whether outputs store **text**, **file path**, or **both** is a workflow semantics choice. This architecture supports either; the JCWF schema should be consistent.

---

## Minimal end-to-end sequence

1. Trigger fires → runtime manager starts workflow run
2. Tick finds ready `ai_call` → `AiCallTaskExecutor` writes env + PROB and registers request
3. JarvisAgent core pipeline (SessionManager etc.) processes PROB asynchronously and creates `*.output.txt`
4. FileWatcher detects output file → pushes event
5. Core delivers event → `JarvisAgent::OnEvent`
6. `AiRequestPool` matches, reads output, finalizes the task
7. Workflow ticks to completion when all tasks terminal

---

## Files / modules referenced (current tree)

- Event system:
  - `engine/event/event.h`
  - `engine/event/eventQueue.h/.cpp`
  - `engine/event/filesystemEvent.h`
  - `engine/event/events.h`
  - `engine/event/event_system.md`
  - `engine/core.cpp` (`Core::Run`, event pop/dispatch)

- JarvisAgent core routing:
  - `application/jarvisAgent.cpp` (`OnStart`, `OnUpdate`, `OnEvent`)
  - `application/jarvisAgent.md`

- Chat pool precedent:
  - `application/web/chatMessages.h/.cpp`

- Workflow system:
  - `application/workflow/workflowOrchestrator.h/.cpp` (current synchronous implementation)
  - `application/workflow/taskExecutor.h/.cpp`
  - `application/workflow/taskExecutorRegistry.h/.cpp`
  - `application/workflow/aiCallTaskExecutor.h/.cpp` (to be refactored away from Python dependency)

---

## Next implementation steps (no code)

1. Add `AiRequestPool` (application-layer)
2. Refactor `AiCallTaskExecutor` to submit via files + register request (no Python dependency)
3. Add tick-based workflow runtime manager (non-blocking)
4. Route `*.output.txt` events in `JarvisAgent::OnEvent()` into `AiRequestPool`
5. Implement timeouts via `AiRequestPool::Update()` from `JarvisAgent::OnUpdate()`
