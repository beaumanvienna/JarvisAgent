# aiZipDemo Workflow – AI‑centric Architecture & Execution

## Executive Summary

The **aiZipDemo** workflow demonstrates how JarvisAgent dispatches **AI‑driven tasks** and **shell tasks** through a unified, event‑driven workflow engine.

At its core, this workflow shows:

- how JCWF workflows are parsed and scheduled,
- how AI tasks are translated into **STNG / CNTX / TASK / PROB** artifacts,
- how asynchronous AI execution integrates with synchronous shell tooling,
- and how freshness checking ensures Makefile‑like behavior.

The focus of this document is **not** basic configuration, but the **AI call execution pipeline and runtime behavior**.

---

## AI Call Execution Pipeline (Center Stage)

### High‑level Flow

1. **WorkflowRuntimeManager** starts a workflow run.
2. **WorkflowOrchestrator** determines which tasks are ready.
3. **AiCallTaskExecutor** materializes AI tasks into files.
4. **AI requests** are dispatched asynchronously.
5. **Completions** are merged back into the workflow.
6. **ShellTaskExecutor** consumes AI outputs and produces derived artifacts.

This entire process is **event‑driven** and **non‑blocking**.

---

## AI Task Materialization (STNG / CNTX / TASK / PROB)

Each `ai_call` task produces a concrete file‑based environment before sending a request.

### Environment Files

For every AI task, the executor assembles:

| File Type | Purpose |
|----------|--------|
| `STNG_*.txt` | Style / behavior constraints |
| `CNTX_*.txt` | Contextual information |
| `TASK_*.txt` | The actual instruction |
| `PROB_<id>_<timestamp>.txt` | Per‑request payload |

These files are written into the **task working directory**:

```
../queue/aiZipDemo/
```

### Why files?

- Full traceability
- Easy debugging
- Deterministic re‑runs
- Offline inspection

This design intentionally mirrors **Makefile transparency** rather than opaque in‑memory pipelines.

---

## Unique File Naming

Each AI request generates a unique PROB filename:

```
PROB_<taskIndex>_<monotonicTimestamp>.txt
```

This ensures:

- no collisions during parallel execution,
- deterministic pairing between input and output,
- safe re‑runs across application restarts.

Corresponding outputs:

```
PROB_<taskIndex>_<timestamp>.output.txt
```

---

## Asynchronous AI Dispatch

### Dispatch

`AiCallTaskExecutor` submits AI requests via the **AI request pool**.  
Requests are non‑blocking and immediately return control to the runtime manager.

### Completion

When an AI response arrives:

1. A completion event is queued.
2. `WorkflowRuntimeManager::DrainAiRequestCompletions()` applies it.
3. Output files are written.
4. The task transitions to **Succeeded**.

Timeouts and failures are detected per request and propagated as task failures.

---

## Synchronization With Shell Tasks

Shell tasks (e.g. `zip_responses`) depend on AI outputs via `depends_on`.

### Guarantees

- Shell tasks **never start** until all required AI tasks have completed.
- Shell tasks operate in the same working directory.
- Inputs and outputs follow the same freshness rules as AI tasks.

This allows seamless mixing of:
- async AI tasks
- deterministic shell tooling

---

## Freshness Checking (Makefile Semantics)

Before executing **any** task:

- `TaskFreshnessChecker` compares:
  - `file_inputs`
  - `file_outputs`

### Outcomes

| Condition | Result |
|--------|--------|
| Outputs newer than inputs | Task is **Skipped** |
| Any input newer | Task executes |
| Missing output | Task executes |

Skipped tasks are logged as **info**, not errors.

This enables:

- fast re‑runs,
- safe auto‑triggers,
- idempotent workflows.

---

## Deadlock Detection (Corrected Behavior)

The runtime manager only logs **critical deadlock/cycle** when:

- no task is running,
- no task was skipped or dispatched,
- and pending tasks still exist.

Pure “everything up‑to‑date” runs **do not** trigger this condition anymore.

---

## Shell Task Execution

Shell tasks:

- run in the task working directory,
- use scripts from `./scripts/`,
- treat scripts as **tools**, not dependencies.

Example:

```
zipTool.sh aiResponses.zip file1.md file2.md file3.md
```

Shell tasks obey the same dependency and freshness logic as AI tasks.

---

## Key C++ Classes Involved

### WorkflowRuntimeManager
- Owns active runs
- Applies AI completions
- Detects real deadlocks
- Advances workflow state

### WorkflowOrchestrator
- Evaluates task readiness
- Applies dependency logic

### AiCallTaskExecutor
- Writes STNG / CNTX / TASK / PROB files
- Dispatches async AI requests
- Handles timeouts and errors

### ShellTaskExecutor
- Executes deterministic tools
- Operates on AI‑generated artifacts

### TaskFreshnessChecker
- Implements Makefile‑style timestamp logic

---

## Observability Through Logs

Typical AI task lifecycle:

```
[info] Writing STNG_*.txt
[info] Writing CNTX_*.txt
[info] Writing TASK_*.txt
[info] Scheduling AI request
[info] AI response received
[info] Wrote PROB_*.output.txt
```

Skipped task:

```
[info] Task 'ai_python_trivia_random' is up to date -> skipped
```

---

## Appendix: Runtime Paths & Configuration

### Where JarvisAgent Is Started

```
~/dev/jarvisAgent
./bin/Release/jarvisAgent
```

### config.json Paths

- Workflows folder: `../workflows`
- Queue folder: `../queue`

### Workflow Base Directory

- Defaults to the directory containing the `.jcwf`
- For aiZipDemo: `../workflows`

### Task Working Directory

```
working_directory: "../queue/aiZipDemo"
```

Resolved relative to workflow base directory.

---

## Final Notes

- aiZipDemo is a **reference workflow** for AI + shell orchestration.
- All behavior is deterministic, observable, and restart‑safe.
- The design intentionally favors clarity over hidden magic.

