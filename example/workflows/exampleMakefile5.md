# exampleMakefile5 Workflow – Error Branching & AI‑Driven Recovery

## Executive Summary

The **exampleMakefile5** workflow extends `exampleMakefile4` with **error branching**: the AI deliberately introduces a C++ syntax error, `make` fails, a branch node routes the failure to an AI fix task, the fixed code is recompiled, and the executable runs successfully.

At its core, this workflow shows:

- how `expose_error_signal` marks a task as recoverable,
- how **Branch nodes** route execution based on task success or failure,
- how error‑path tasks receive failure context (compiler stderr + original source),
- how convergence works when a task (`shell_2`) is reachable from both branch paths,
- and how **Rule A** treats handled failures — the run succeeds despite `shell` failing.

---

## Pipeline Overview

```
┌──────────────┐     ┌──────────────┐
│  ai_call      │     │  ai_call_2    │
│  generate     │     │  generate     │
│  hello.cpp    │     │  Makefile     │
│  (01_)        │     │  (02_)        │
└──────┬───────┘     └──────┬───────┘
       │                     │
       └────────┬────────────┘
                ▼
       ┌────────────────┐
       │  shell          │  ← expose_error_signal: true
       │  run make       │
       │  (01_runMake)   │
       └────────┬───────┘
                ▼
          ┌──────────┐
          │ branch_1  │
          └──┬────┬──┘
    success  │    │  error
             ▼    ▼
             │   ┌────────────────┐
             │   │  ai_call_fix    │  ← receives stderr + original source
             │   │  fix hello.cpp  │
             │   │  (03_)          │
             │   └────────┬───────┘
             │            ▼
             │   ┌────────────────┐
             │   │  shell_retry    │
             │   │  retry make     │
             │   │  (01_runMake)   │
             │   └────────┬───────┘
             │            ▼
             │      ┌──────────┐
             │      │ branch_2  │
             │      └─────┬────┘
             │            │ success
             └──────┬─────┘
                    ▼
           ┌────────────────┐
           │  shell_2        │
           │  run hello      │
           │  (02_runHello)  │
           └────────────────┘
```

---

## Task Details

### 1. ai_call – generate hello.cpp (with deliberate error)

Generates a minimal C++ program with an **intentionally introduced syntax error**.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Working dir | `../queue/exampleMakefile5/01_ai_call` |
| Output | `PROB_hello.output.txt` |

The TASK prompt explicitly requests: *"Introduce deliberately a C++ syntax error"*.

### 2. ai_call_2 – generate Makefile

Generates a Makefile that compiles `hello.cpp` into an executable called `hello`.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Working dir | `../queue/exampleMakefile5/02_ai_call_2` |
| Output | `PROB_Makefile.output.txt` |

### 3. shell – run make (expected to fail)

Compiles the AI‑generated source. Fails because of the deliberate syntax error.

| Field | Value |
|-------|-------|
| Type | `shell` |
| Script | `scripts/runMake.sh` |
| Working dir | `exampleMakefile5/01_runMake` |
| Depends on | `ai_call`, `ai_call_2` |
| **expose_error_signal** | **`true`** |

The `expose_error_signal: true` field tells the runtime that this task's failure can be handled by a downstream Branch node. Compiler errors are captured in `stderr.txt` in the working directory.

#### Materialize

| Source (AI output) | Target (local file) |
|--------------------|---------------------|
| `PROB_hello.output.txt` | `hello.cpp` |
| `PROB_Makefile.output.txt` | `Makefile` |

### 4. ai_call_fix – fix hello.cpp

Activated only on the **error path** of `branch_1`. Receives the broken source and compiler errors as context, then generates corrected code.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Working dir | `../queue/exampleMakefile5/03_ai_call_fix` |
| Output | `PROB_fix.output.txt` |

#### Context Files (error artifacts)

| File | Source |
|------|--------|
| `../01_ai_call/PROB_hello.output.txt` | The original broken `hello.cpp` |
| `../../../workflows/exampleMakefile5/01_runMake/stderr.txt` | Compiler error output from the failed `make` |

This gives the AI model both the broken code and the exact compiler diagnostics to produce a fix.

### 5. shell_retry – retry make

Recompiles using the AI‑fixed source code.

| Field | Value |
|-------|-------|
| Type | `shell` |
| Script | `scripts/runMake.sh` |
| Working dir | `exampleMakefile5/01_runMake` |
| Depends on | `ai_call_fix`, `ai_call_2` |

#### Materialize

| Source (AI output) | Target (local file) |
|--------------------|---------------------|
| `PROB_fix.output.txt` | `hello.cpp` (overwrites the broken version) |
| `PROB_Makefile.output.txt` | `Makefile` |

### 6. shell_2 – run hello

Runs the compiled executable. Reachable from **both** branch paths:
- **Success path**: `shell` succeeds → `branch_1` normal → `shell_2`
- **Error recovery path**: `shell_retry` succeeds → `branch_2` normal → `shell_2`

| Field | Value |
|-------|-------|
| Type | `shell` |
| Script | `scripts/run.sh` |
| Args | `exampleMakefile5/01_runMake/hello` |
| Working dir | `exampleMakefile5/02_runHello` |

---

## Controlflow

### Branch Nodes

| Node | Driving Task | Normal Path | Error Path |
|------|-------------|-------------|------------|
| `branch_1` | `shell` | → `shell_2` | → `ai_call_fix`, `shell_retry` |
| `branch_2` | `shell_retry` | → `shell_2` | (none) |

### Controlflow Edges

| From | To | Kind |
|------|----|------|
| `shell` | `branch_1` | `normal` |
| `shell` | `branch_1` | `error_signal` |
| `branch_1` | `shell_2` | `normal` |
| `branch_1` | `ai_call_fix` | `on_error` |
| `branch_1` | `shell_retry` | `on_error` |
| `shell_retry` | `branch_2` | `normal` |
| `branch_2` | `shell_2` | `normal` |

### Convergence

`shell_2` appears on both `branch_1`'s normal output and `branch_2`'s normal output. When `branch_1` takes the error path, it initially skips `shell_2`. Later, when `branch_2` fires after `shell_retry` succeeds, it **re‑enables** `shell_2` (resets its state from Skipped to Pending), allowing it to execute.

---

## Rule A — Handled Failures

At run completion, the runtime checks whether any failed tasks are **unhandled**:

- `shell` has `expose_error_signal: true` and its error signal is connected to `branch_1`.
- `branch_1` selected the error recovery path, which completed successfully.
- Therefore `shell`'s failure is **handled** and the overall run reports **completed**, not failed.

---

## Running

```bash
# Manual start only (manual_start: true)
curl -s -X POST http://localhost:8080/api/workflows/exampleMakefile5/run

# Clean before re-run
curl -s -X DELETE http://localhost:8080/api/workflows/exampleMakefile5/clean
```

---

## Expected Execution

### Task States at Completion

| Task | Final State | Notes |
|------|-------------|-------|
| `ai_call` | Succeeded | Generated hello.cpp with deliberate error |
| `ai_call_2` | Succeeded | Generated Makefile |
| `shell` | **Failed** | `make` failed (syntax error) — handled by branch_1 |
| `ai_call_fix` | Succeeded | AI fixed the code using compiler stderr |
| `shell_retry` | Succeeded | Recompiled successfully |
| `shell_2` | Succeeded | Ran hello executable |

### Expected Output

```
Hello from JarvisAgent!
Hello from JarvisAgent!
Hello from JarvisAgent!
Hello from JarvisAgent!
Hello from JarvisAgent!
```

---

## Key Concepts Demonstrated

- **expose_error_signal** — marks a task as recoverable, enabling downstream Branch nodes to handle its failure
- **Branch nodes** — `control_nodes` with `"type": "branch"` that route execution based on driving task outcome
- **Controlflow edges** — `"kind": "normal"`, `"error_signal"`, and `"on_error"` edges define the branch wiring
- **Error context propagation** — `ai_call_fix` receives `stderr.txt` + original source as `cntx_files`
- **Convergence** — `shell_2` is reachable from both the success and error recovery paths
- **Rule A** — handled failures don't fail the run; `shell` stays Failed but the run completes successfully
- **DAG constraint** — no loops; the "retry" is modeled as a separate task (`shell_retry`), keeping the graph acyclic
