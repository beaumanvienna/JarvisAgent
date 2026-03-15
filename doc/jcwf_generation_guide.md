# JCWF Generation Guide

This is a condensed reference for generating valid JC Workflow Files (JCWF).
It covers the JSON structure, task types, dependencies, data flow, controlflow,
and common patterns. For the full specification, see `JC_Workflow_Specification.md`.

---

## 1. Root Object

A JCWF file is valid JSON with extension `.jcwf`.

```jsonc
{
  "version": "1.1",
  "id": "my-workflow",
  "label": "Human-readable name",
  "doc": "Description of what this workflow does.",
  "manual_start": true,
  "triggers": [ /* optional */ ],
  "tasks": { /* REQUIRED — map of taskId → task object */ },
  "defaults": { /* optional */ },
  "filters": [ /* optional, v1.1 */ ],
  "dataflow": [ /* optional */ ],
  "control_nodes": [ /* optional, v1.1 */ ],
  "controlflow": [ /* optional, v1.1 */ ]
}
```

### Root Fields

| Field | Required | Type | Description |
|-------|----------|------|-------------|
| `version` | YES | string | `"1.0"` or `"1.1"`. Use `"1.1"` if using filters, control_nodes, or controlflow. |
| `id` | YES | string | Unique workflow identifier (slug or UUID). |
| `label` | no | string | Human-friendly display name. |
| `doc` | no | string | Documentation/comments. |
| `manual_start` | no | boolean | Default `true`. Set `false` to prevent manual start (trigger-only). |
| `triggers` | no | array | When/how the workflow starts. If omitted, implicit auto-trigger. |
| `tasks` | YES | object | Map from taskId to task definition. |
| `defaults` | no | object | Default timeout, retry, AI provider settings. |
| `filters` | no | array | Filter nodes for per_item expansion (v1.1). |
| `dataflow` | no | array | Explicit output→input wiring between tasks. |
| `control_nodes` | no | array | Branch nodes for conditional execution (v1.1). |
| `controlflow` | no | array | Edges between tasks and control nodes (v1.1). |

---

## 2. Tasks

The `tasks` object maps each `taskId` (string key) to a task definition:

```jsonc
"tasks": {
  "my_task": {
    "id": "my_task",
    "type": "shell",
    "label": "Build project",
    "depends_on": ["generate_code"],
    "working_directory": "myWorkflow/01_build",
    "params": { "command": "scripts/build.sh" },
    "file_inputs": ["path/to/input.txt"],
    "file_outputs": ["path/to/output.txt"],
    "materialize": { "{{input[0]}}": "local_name.txt" },
    "timeout_ms": 60000,
    "expose_error_signal": false
  }
}
```

### Task Fields

| Field | Required | Type | Description |
|-------|----------|------|-------------|
| `id` | YES | string | Must match the key in the `tasks` map. |
| `type` | YES | string | `"shell"`, `"ai_call"`, `"python"`, or `"internal"`. |
| `label` | no | string | Display name. |
| `doc` | no | string | Documentation. |
| `depends_on` | no | array of strings | Task IDs that must complete before this task runs. Forms a DAG (no cycles). |
| `working_directory` | no | string | Task working dir, relative to workflow file directory. |
| `params` | no | object | Type-specific parameters (see below). |
| `file_inputs` | no | array of strings | Files this task reads (for freshness checks and positional args). |
| `file_outputs` | no | array of strings | Files this task produces. |
| `materialize` | no | object | Copy files into working_directory before execution. Keys are source templates like `{{input[0]}}`, values are target filenames. |
| `queue_binding` | no | object | For `ai_call` tasks: declares queue folder files (STNG, TASK, CNTX, PROB). |
| `inputs` | no | object | Named data slots for validation and dataflow wiring. |
| `outputs` | no | object | Named output slots. |
| `timeout_ms` | no | integer | Inactivity watchdog timeout in ms. 0 = no watchdog. |
| `retries` | no | object | `{ "max_attempts": N, "backoff_ms": N }` |
| `expose_error_signal` | no | boolean | If `true`, failure can be consumed by a branch node (see §7). |
| `mode` | no | string | `"single"` (default) or `"per_item"`. |
| `filter` | no | string | Filter ID for per_item expansion (v1.1). |

---

## 3. Task Types

### 3.1 `shell` — Execute a script

```jsonc
{
  "id": "compile",
  "type": "shell",
  "params": {
    "command": "scripts/build.sh",
    "args": ["${input[0]}", "--output", "${output[0]}"]
  },
  "working_directory": "myWorkflow/01_compile",
  "file_inputs": ["source.cpp"],
  "file_outputs": ["output/binary"]
}
```

- `params.command` (REQUIRED): Must start with `scripts/`. Resolved relative to JarvisAgent launch directory.
- `params.args` (optional): Array of strings. May use `${input[i]}` and `${output[i]}` to reference resolved `file_inputs`/`file_outputs` paths.
- `working_directory`: Where the script runs. Relative to workflow file directory.

### 3.2 `ai_call` — Call an AI model

AI calls work through a **queue folder** mechanism. The task declares files to write into a queue folder via `queue_binding`. A SessionManager watches the folder and dispatches AI queries.

```jsonc
{
  "id": "generate_code",
  "type": "ai_call",
  "label": "Generate hello.cpp",
  "working_directory": "../queue/myWorkflow/01_generate",
  "queue_binding": {
    "stng_files": [
      { "path": "STNG_style.txt", "content": "Be precise. Output only raw code." }
    ],
    "task_files": [
      { "path": "TASK_gen.txt", "content": "Generate a C++ hello world program." }
    ],
    "cntx_files": [
      { "path": "CNTX_style.txt", "content": "Use Allman brace style." }
    ],
    "prob_files": [
      { "path": "PROB_hello.txt", "content": "Please generate hello.cpp" }
    ]
  }
}
```

#### Queue Folder File Categories

| Prefix | Role | Sent to AI? |
|--------|------|-------------|
| `STNG_*` | Settings — tone, style, constraints | YES (environment) |
| `TASK_*` | Task instructions — what to produce | YES (environment) |
| `CNTX_*` | Context — background information | YES (environment) |
| `PROB_*` | Problem/requirement — each triggers one AI query | YES (appended to environment) |
| `PROV_*` | Provider config — endpoint, model, API type | NO (never sent to AI) |

**The full AI prompt is:** `[all STNG content] + [all CNTX content] + [all TASK content] + [PROB file content]`

#### queue_binding entries

Each entry is either:
- A **string** (path to an existing file): `"../other_task/PROB_foo.output.txt"`
- An **inline object**: `{ "path": "STNG_style.txt", "content": "Be concise." }`

#### AI Output

The SessionManager writes the AI response to `<stem>.output.<ext>`:
- `PROB_hello.txt` → `PROB_hello.output.txt`

#### Consuming upstream AI outputs as context

Use `cntx_files` to reference upstream task outputs:

```jsonc
"cntx_files": [
  "../01_generate/PROB_hello.output.txt"
]
```

The runtime copies the file as `CNTX_PROB_hello.output.txt` in the current task's queue folder. Glob patterns are supported: `"../01_generate/PROB_*.output.txt"`.

#### working_directory for ai_call

Convention: `"../queue/<workflowId>/<NN>_<taskId>"` (e.g., `"../queue/myWorkflow/01_generate"`). The `../queue/` prefix places the folder in the queue directory where SessionManagers watch.

### 3.3 `python` — Execute a Python function

```jsonc
{
  "id": "process_data",
  "type": "python",
  "params": {
    "module": "workflows.data_processor",
    "function": "run"
  }
}
```

- `params.module`: Python import name. Scripts in `scripts/` are on sys.path.
- `params.function`: Callable name in the module.

### 3.4 `internal` — Built-in C++ action

```jsonc
{
  "id": "cleanup",
  "type": "internal",
  "params": {
    "action": "update_status"
  }
}
```

Used for built-in operations like status updates or queue coordination.

---

## 4. Dependencies

### depends_on

```jsonc
"depends_on": ["task_a", "task_b"]
```

- Forms a **DAG** — cycles are rejected at load time.
- A task runs only when all `depends_on` tasks have succeeded (or been skipped as up-to-date).
- Tasks with no `depends_on` are root tasks (run immediately or on trigger).

### Freshness / Up-to-Date Checks

A task MAY be skipped if:
- All `file_outputs` exist, AND
- Each `file_output` is newer than every `file_input` and upstream output.

If `file_inputs` or `file_outputs` are omitted, the task always runs.

### materialize

Copies upstream outputs into the task's working directory under specific names:

```jsonc
"file_inputs": [
  "../../../queue/myWorkflow/01_generate/PROB_hello.output.txt",
  "../../../queue/myWorkflow/02_makefile/PROB_Makefile.output.txt"
],
"materialize": {
  "{{input[0]}}": "hello.cpp",
  "{{input[1]}}": "Makefile"
}
```

This lets scripts expect fixed filenames (e.g., `hello.cpp`) regardless of the actual upstream output path.

---

## 5. Dataflow

Explicit wiring of task outputs to task inputs:

```jsonc
"dataflow": [
  {
    "from_task": "extract",
    "from_output": "data",
    "to_task": "summarize",
    "to_input": "text"
  }
]
```

- `from_task` / `to_task`: Task IDs.
- `from_output` / `to_input`: Named slot names from the tasks' `outputs` / `inputs`.

---

## 6. Defaults

```jsonc
"defaults": {
  "timeout_ms": 60000,
  "retries": { "max_attempts": 2, "backoff_ms": 1000 },
  "ai": {
    "provider": "openai_gpt4",
    "model": "gpt-4.1-mini",
    "request_params": { "temperature": 0.7 }
  }
}
```

Task-level fields override defaults.

---

## 7. Control Nodes & Controlflow (v1.1)

### Branch Nodes

Branch nodes route execution based on whether a driving task succeeded or failed.

```jsonc
"control_nodes": [
  { "id": "branch_1", "type": "branch", "label": "error recovery" }
]
```

- `type`: Currently only `"branch"` is supported.
- Branch nodes are NOT tasks — no working_directory, no params, no artifacts.

### Controlflow Edges

```jsonc
"controlflow": [
  { "from": "shell", "to": "branch_1", "kind": "normal",       "from_port": "dep-source",    "to_port": "cf-in-normal" },
  { "from": "shell", "to": "branch_1", "kind": "error_signal",  "from_port": "error-signal",  "to_port": "cf-in-error" },
  { "from": "branch_1", "to": "next_task",  "kind": "normal",   "from_port": "cf-out-normal", "to_port": "dep-target" },
  { "from": "branch_1", "to": "fix_task",   "kind": "on_error", "from_port": "cf-out-error",  "to_port": "dep-target" }
]
```

#### Edge kinds

| Kind | Direction | Meaning |
|------|-----------|---------|
| `normal` | task → branch | Success/completion signal into branch |
| `error_signal` | task → branch | Failure signal into branch. Source task MUST have `expose_error_signal: true`. |
| `normal` | branch → task | Activates task on success path |
| `on_error` | branch → task | Activates task on failure path |

#### Port names (for the visual editor)

| Port | Location |
|------|----------|
| `dep-source` | Task output (right side) |
| `error-signal` | Task error output (bottom) |
| `cf-in-normal` | Branch left input (normal) |
| `cf-in-error` | Branch left input (error) |
| `cf-out-normal` | Branch right output (normal) |
| `cf-out-error` | Branch right output (error) |
| `dep-target` | Task input (left side) |

### expose_error_signal

Set `"expose_error_signal": true` on a task to enable error branching. When the task fails:
- The branch fires its `on_error` outputs.
- The branch's `normal` outputs are Skipped.
- The failure is **handled** — it does NOT fail the overall workflow run (Rule A).

When the task succeeds:
- The branch fires its `normal` outputs.
- The branch's `on_error` outputs are Skipped.

### Rule A (Handled vs. Unhandled Failures)

A workflow run fails **only** if it has at least one Failed task whose failure is **unhandled**. A failure is handled if the task has `expose_error_signal: true` AND there's an `error_signal` edge to a branch node.

---

## 8. Triggers

```jsonc
"triggers": [
  { "type": "auto", "id": "auto", "enabled": true },
  { "type": "manual", "id": "manual", "enabled": true },
  { "type": "cron", "id": "daily", "enabled": true, "params": { "expression": "0 8 * * *" } },
  { "type": "file_watch", "id": "on-change", "enabled": true, "params": { "path": "data/input.csv" } }
]
```

- If `triggers` is omitted: implicit auto-trigger (starts on registration).
- `manual_start: true` (default) allows manual start regardless of triggers.

---

## 9. Filters (v1.1)

Filters define data sources for `per_item` task expansion.

```jsonc
"filters": [
  {
    "id": "items",
    "source": { "kind": "csv", "path": "data/items.csv", "has_header": true },
    "binding": "item",
    "max_items": 10000
  }
]
```

### Source kinds

- `csv` — Iterate CSV rows. Fields: `path`, `delimiter`, `has_header`, `range`.
- `text_lines` — Iterate text file lines. Fields: `path`, `skip_empty`.
- `query` — Lucene-style query. Fields: `index_path`, `query`, `fields`.

### Template substitution in queue_binding

For `per_item` AI tasks, use `{{binding.field}}` in `queue_binding` content/path:

```jsonc
"prob_files": [
  { "path": "PROB_{{item.Name}}.txt", "content": "Process: {{item.Name}}" }
]
```

---

## 10. Common Patterns

### Pattern A: AI generates code, shell compiles and runs

```
ai_call (generate code) ──┐
ai_call_2 (generate Makefile) ──┤
                                ├── shell (compile) ── shell_2 (run)
```

### Pattern B: Error recovery with branch

```
shell (compile) [expose_error_signal: true]
    ├── normal → branch_1 → normal → shell_2 (run)
    └── error_signal → branch_1 → on_error → ai_call_fix (fix code)
                                                 └── shell_retry (retry compile)
                                                        └── branch_2 → normal → shell_2 (run)
```

### Pattern C: AI pipeline (summarize many items)

```
filter (csv) → ai_call (per_item, summarize each) → ai_call_2 (single, combine all summaries)
```

---

## 11. Complete Examples

### Example A: AI-Generated C++ Build (exampleMakefile4)

Two AI calls generate C++ code and a Makefile in parallel. A shell task compiles them, another runs the binary.

```json
{
  "version": "1.0",
  "id": "exampleMakefile4",
  "label": "example Makefile 4",
  "doc": "AI creates C++ code. another ai call creates Makefile",
  "defaults": { "timeout_ms": 30000 },
  "tasks": {
    "ai_call": {
      "id": "ai_call",
      "type": "ai_call",
      "label": "generate hello.cpp",
      "params": {},
      "working_directory": "../queue/exampleMakefile4/01_ai_call",
      "queue_binding": {
        "stng_files": [{ "path": "STNG_new_1.txt", "content": "Be precise. Output only raw C++ code. No markdown fences, no explanations." }],
        "task_files": [{ "path": "TASK_new_1.txt", "content": "Generate one file: hello.cpp — a minimal C++ program that prints \"Hello from JarvisAgent!\" five times." }],
        "cntx_files": [{ "path": "CNTX_new_1.txt", "content": "For C++ code, use Allman brace style." }],
        "prob_files": [{ "path": "PROB_hello.txt", "content": "Please generate hello.cpp" }]
      }
    },
    "ai_call_2": {
      "id": "ai_call_2",
      "type": "ai_call",
      "label": "generate Makefile",
      "params": {},
      "working_directory": "../queue/exampleMakefile4/02_ai_call_2",
      "queue_binding": {
        "stng_files": [{ "path": "STNG_new_1.txt", "content": "Output only a raw Makefile. No markdown fences, no explanations." }],
        "task_files": [{ "path": "TASK_new_1.txt", "content": "Generate the requested Makefile." }],
        "cntx_files": [{ "path": "CNTX_new_1.txt", "content": "This is for a simple C++ project. Use proper Makefile syntax with TABS." }],
        "prob_files": [{ "path": "PROB_Makefile.txt", "content": "Write a Makefile that compiles hello.cpp into an executable called \"hello\". Use g++." }]
      }
    },
    "shell": {
      "id": "shell",
      "type": "shell",
      "label": "run command make",
      "params": { "command": "scripts/runMake.sh" },
      "working_directory": "exampleMakefile4/01_runMake",
      "file_inputs": [
        "../../../queue/exampleMakefile4/01_ai_call/PROB_hello.output.txt",
        "../../../queue/exampleMakefile4/02_ai_call_2/PROB_Makefile.output.txt"
      ],
      "materialize": { "{{input[0]}}": "hello.cpp", "{{input[1]}}": "Makefile" },
      "depends_on": ["ai_call", "ai_call_2"]
    },
    "shell_2": {
      "id": "shell_2",
      "type": "shell",
      "label": "run hello",
      "params": { "command": "scripts/run.sh", "args": ["exampleMakefile4/01_runMake/hello"] },
      "working_directory": "exampleMakefile4/02_runHello",
      "depends_on": ["shell"]
    }
  }
}
```

**Key points:**
- `ai_call` and `ai_call_2` run in parallel (no dependency between them).
- `shell` depends on both AI tasks and uses `materialize` to copy AI outputs as `hello.cpp` and `Makefile`.
- `shell_2` depends on `shell` and runs the compiled binary.
- `working_directory` for ai_call tasks points to queue folder (`../queue/...`).
- `working_directory` for shell tasks is relative to workflow file directory.

### Example B: Error Recovery with Branching (exampleMakefile5)

Same as Example A but the AI deliberately introduces a syntax error. A branch node detects the compilation failure and routes to an AI fix task.

```json
{
  "version": "1.1",
  "id": "exampleMakefile5",
  "manual_start": true,
  "triggers": [{ "type": "manual", "id": "manual", "enabled": true }],
  "label": "example Makefile 5",
  "doc": "AI creates C++ code (with deliberate syntax error). Make fails, error branch fixes code, retries make, then runs hello.",
  "defaults": { "timeout_ms": 30000 },
  "tasks": {
    "ai_call": {
      "id": "ai_call", "type": "ai_call", "label": "generate hello.cpp",
      "working_directory": "../queue/exampleMakefile5/01_ai_call",
      "queue_binding": {
        "stng_files": [{ "path": "STNG_new_1.txt", "content": "Be precise. Output only raw C++ code." }],
        "task_files": [{ "path": "TASK_new_1.txt", "content": "Generate hello.cpp with a deliberate syntax error." }],
        "cntx_files": [{ "path": "CNTX_new_1.txt", "content": "Use Allman brace style." }],
        "prob_files": [{ "path": "PROB_hello.txt", "content": "Please generate hello.cpp" }]
      }
    },
    "ai_call_2": {
      "id": "ai_call_2", "type": "ai_call", "label": "generate Makefile",
      "working_directory": "../queue/exampleMakefile5/02_ai_call_2",
      "queue_binding": {
        "stng_files": [{ "path": "STNG_new_1.txt", "content": "Output only a raw Makefile." }],
        "task_files": [{ "path": "TASK_new_1.txt", "content": "Generate the requested Makefile." }],
        "cntx_files": [{ "path": "CNTX_new_1.txt", "content": "Simple C++ project. Proper Makefile syntax with TABS." }],
        "prob_files": [{ "path": "PROB_Makefile.txt", "content": "Makefile for hello.cpp → hello binary. Use g++." }]
      }
    },
    "shell": {
      "id": "shell", "type": "shell", "label": "run command make",
      "params": { "command": "scripts/runMake.sh" },
      "working_directory": "exampleMakefile5/01_runMake",
      "file_inputs": [
        "../../../queue/exampleMakefile5/01_ai_call/PROB_hello.output.txt",
        "../../../queue/exampleMakefile5/02_ai_call_2/PROB_Makefile.output.txt"
      ],
      "materialize": { "{{input[0]}}": "hello.cpp", "{{input[1]}}": "Makefile" },
      "expose_error_signal": true,
      "depends_on": ["ai_call", "ai_call_2"]
    },
    "ai_call_fix": {
      "id": "ai_call_fix", "type": "ai_call", "label": "fix hello.cpp",
      "working_directory": "../queue/exampleMakefile5/03_ai_call_fix",
      "queue_binding": {
        "stng_files": [{ "path": "STNG_new_1.txt", "content": "Output only raw C++ code." }],
        "task_files": [{ "path": "TASK_new_1.txt", "content": "Fix hello.cpp based on the compiler error output." }],
        "cntx_files": [
          "../01_ai_call/PROB_hello.output.txt",
          "../../../workflows/exampleMakefile5/01_runMake/stderr.txt"
        ],
        "prob_files": [{ "path": "PROB_fix.txt", "content": "Fix the C++ code so it compiles. Output only corrected hello.cpp." }]
      }
    },
    "shell_retry": {
      "id": "shell_retry", "type": "shell", "label": "retry make",
      "params": { "command": "scripts/runMake.sh" },
      "working_directory": "exampleMakefile5/01_runMake",
      "file_inputs": [
        "../../../queue/exampleMakefile5/03_ai_call_fix/PROB_fix.output.txt",
        "../../../queue/exampleMakefile5/02_ai_call_2/PROB_Makefile.output.txt"
      ],
      "materialize": { "{{input[0]}}": "hello.cpp", "{{input[1]}}": "Makefile" },
      "depends_on": ["ai_call_fix", "ai_call_2"]
    },
    "shell_2": {
      "id": "shell_2", "type": "shell", "label": "run hello",
      "params": { "command": "scripts/run.sh", "args": ["exampleMakefile5/01_runMake/hello"] },
      "working_directory": "exampleMakefile5/02_runHello"
    }
  },
  "control_nodes": [
    { "id": "branch_1", "type": "branch", "label": "branch" },
    { "id": "branch_2", "type": "branch", "label": "branch" }
  ],
  "controlflow": [
    { "from": "shell", "to": "branch_1", "kind": "normal", "from_port": "dep-source", "to_port": "cf-in-normal" },
    { "from": "shell", "to": "branch_1", "kind": "error_signal", "from_port": "error-signal", "to_port": "cf-in-error" },
    { "from": "branch_1", "to": "shell_2", "kind": "normal", "from_port": "cf-out-normal", "to_port": "dep-target" },
    { "from": "branch_1", "to": "ai_call_fix", "kind": "on_error", "from_port": "cf-out-error", "to_port": "dep-target" },
    { "from": "branch_1", "to": "shell_retry", "kind": "on_error", "from_port": "cf-out-error", "to_port": "dep-target" },
    { "from": "shell_retry", "to": "branch_2", "kind": "normal", "from_port": "dep-source", "to_port": "cf-in-normal" },
    { "from": "branch_2", "to": "shell_2", "kind": "normal", "from_port": "cf-out-normal", "to_port": "dep-target" }
  ]
}
```

**Key points:**
- `shell` has `expose_error_signal: true` — its failure is handled by `branch_1`.
- `branch_1` receives both `normal` and `error_signal` edges from `shell`.
- On success: `branch_1` activates `shell_2` (run hello).
- On failure: `branch_1` activates `ai_call_fix` and `shell_retry` (both gated by on_error).
- `ai_call_fix` uses `cntx_files` to read the broken code AND the compiler stderr.
- `shell_retry` re-compiles with the fixed code, then `branch_2` activates `shell_2`.
- `shell_2` has NO `depends_on` — it's gated purely by controlflow from `branch_1` or `branch_2`.

---

## 12. Common Pitfalls

1. **Missing `id` field on tasks** — The `id` field must match the key in the `tasks` map.
2. **Cycles in `depends_on`** — The task graph must be a DAG. Cycles are rejected.
3. **Missing queue files for ai_call** — Every `ai_call` must have at least `prob_files` in `queue_binding`. The runtime auto-fills missing `stng_files`/`task_files`/`cntx_files` with defaults, but `prob_files` triggers the AI query.
4. **Wrong `working_directory` for ai_call** — Must point to a queue folder (convention: `../queue/<workflowId>/<NN>_<taskId>`).
5. **Shell command not starting with `scripts/`** — Security requirement: all shell commands must start with `scripts/`.
6. **Controlflow without `expose_error_signal`** — An `error_signal` edge requires the source task to have `expose_error_signal: true`.
7. **Version mismatch** — Use `"1.1"` if using `filters`, `control_nodes`, or `controlflow`. Otherwise `"1.0"` is fine.
8. **Controlflow-gated tasks with `depends_on`** — Tasks activated by controlflow edges should generally NOT also be in `depends_on` of the branching task, as controlflow gating is independent.
