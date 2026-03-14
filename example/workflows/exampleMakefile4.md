# exampleMakefile4 Workflow – AI‑Generated C++ Build Pipeline

## Executive Summary

The **exampleMakefile4** workflow demonstrates a straightforward **AI → compile → run** pipeline: two AI tasks generate C++ source code and a Makefile, a shell task compiles the project, and a final shell task runs the resulting executable.

At its core, this workflow shows:

- how parallel `ai_call` tasks produce source artifacts via `queue_binding`,
- how `materialize` copies AI outputs into the shell task's working directory,
- how `depends_on` enforces execution order between AI and shell tasks,
- and how the full cycle (generate → compile → run) executes end‑to‑end.

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
       │  shell          │
       │  run make       │
       │  (01_runMake)   │
       └────────┬───────┘
                ▼
       ┌────────────────┐
       │  shell_2        │
       │  run hello      │
       │  (02_runHello)  │
       └────────────────┘
```

---

## Task Details

### 1. ai_call – generate hello.cpp

Generates a minimal C++ program that prints "Hello from JarvisAgent!" five times.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Working dir | `../queue/exampleMakefile4/01_ai_call` |
| Output | `PROB_hello.output.txt` |

#### Queue Binding

| File | Content |
|------|---------|
| `STNG_new_1.txt` | Raw C++ only, no markdown fences or explanations |
| `TASK_new_1.txt` | Generate `hello.cpp` — minimal iostream program |
| `CNTX_new_1.txt` | Use Allman brace style |
| `PROB_hello.txt` | "Please generate hello.cpp" |

### 2. ai_call_2 – generate Makefile

Generates a Makefile that compiles `hello.cpp` into an executable called `hello`.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Working dir | `../queue/exampleMakefile4/02_ai_call_2` |
| Output | `PROB_Makefile.output.txt` |

#### Queue Binding

| File | Content |
|------|---------|
| `STNG_new_1.txt` | Raw Makefile only, no markdown |
| `TASK_new_1.txt` | Generate the requested Makefile |
| `CNTX_new_1.txt` | Simple C++ project, proper Makefile syntax with TABS |
| `PROB_Makefile.txt` | "Write a Makefile that compiles hello.cpp into 'hello'. Use g++." |

### 3. shell – run make

Compiles the AI‑generated source using `make`.

| Field | Value |
|-------|-------|
| Type | `shell` |
| Script | `scripts/runMake.sh` |
| Working dir | `exampleMakefile4/01_runMake` |
| Depends on | `ai_call`, `ai_call_2` |
| Timeout | 30 s (from defaults) |

#### Materialize

The `materialize` map copies AI outputs into the working directory before execution:

| Source (AI output) | Target (local file) |
|--------------------|---------------------|
| `PROB_hello.output.txt` | `hello.cpp` |
| `PROB_Makefile.output.txt` | `Makefile` |

This ensures `make` sees standard filenames in its working directory regardless of how the AI outputs are named.

### 4. shell_2 – run hello

Runs the compiled executable.

| Field | Value |
|-------|-------|
| Type | `shell` |
| Script | `scripts/run.sh` |
| Args | `exampleMakefile4/01_runMake/hello` |
| Working dir | `exampleMakefile4/02_runHello` |
| Depends on | `shell` |

---

## Running

```bash
# exampleMakefile4 has no triggers (auto-starts on load)
# To run manually after loading:
curl -s -X POST http://localhost:8080/api/workflows/exampleMakefile4/run

# Clean before re-run
curl -s -X DELETE http://localhost:8080/api/workflows/exampleMakefile4/clean
```

---

## Expected Output

On a successful run, the shell task compiles `hello.cpp` and the final task prints:

```
Hello from JarvisAgent!
Hello from JarvisAgent!
Hello from JarvisAgent!
Hello from JarvisAgent!
Hello from JarvisAgent!
```

---

## Key Concepts Demonstrated

- **Parallel AI tasks** — `ai_call` and `ai_call_2` run concurrently (no dependency between them)
- **queue_binding** — inline STNG/TASK/CNTX/PROB files materialized into task working directories
- **materialize** — AI output files copied and renamed into the shell task's working directory
- **depends_on** — shell task waits for both AI tasks to complete before starting
- **Makefile semantics** — freshness checking enables fast re‑runs when outputs are up to date
