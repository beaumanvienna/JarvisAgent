# Sub-Workflow JCWF Generation Guide

This is a condensed reference for generating valid JSON files for **sub-workflow canvases**.
Sub-workflows are JSON files inside a `.jcwf` zip container, in a subfolder named after
the sub-workflow. They are invoked by a parent workflow's `sub_workflow` task.
They do NOT have triggers or `manual_start`.

Use this guide when generating tasks inside a sub-workflow canvas. For generating
top-level workflows (which may contain `sub_workflow` nodes), see `jcwf_generation_guide.md`.

---

## 1. Root Object

A sub-workflow JCWF file is valid JSON with extension `.jcwf`.

```jsonc
{
  "version": "1.1",
  "id": "cleanup-pipeline",
  "label": "Cleanup Pipeline",
  "doc": "Removes temporary files and archives results.",
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
| `tasks` | YES | object | Map from taskId to task definition. |
| `defaults` | no | object | Default timeout, retry, AI provider settings. |
| `filters` | no | array | Filter nodes for per_item expansion (v1.1). |
| `dataflow` | no | array | Explicit output→input wiring between tasks. |
| `control_nodes` | no | array | Branch nodes for conditional execution (v1.1). |
| `controlflow` | no | array | Edges between tasks and control nodes (v1.1). |

**Not applicable to sub-workflows:** `manual_start`, `triggers`, `concurrency`. Sub-workflows are
always invoked by a parent — they never start on their own, and concurrency is owned by the parent workflow.

---

## 2. Tasks

Each key in `tasks` is the `taskId` (must match the task's `"id"` field).

```jsonc
"tasks": {
  "archive": {
    "id": "archive",
    "type": "shell",
    "label": "Archive results",
    "working_directory": "cleanup/01_archive",
    "params": { "command": "scripts/archive.sh" },
    "file_inputs": ["results/output.txt"],
    "file_outputs": ["cleanup/01_archive/archive.tar.gz"]
  },
  "notify": {
    "id": "notify",
    "type": "python",
    "label": "Send notification",
    "depends_on": ["archive"],
    "working_directory": "cleanup/02_notify",
    "params": { "module": "scripts.notify", "function": "send_email" }
  }
}
```

### Common Task Fields

| Field | Required | Type | Description |
|-------|----------|------|-------------|
| `id` | YES | string | Must match the key in `tasks`. |
| `type` | YES | string | `python`, `shell`, `ai_call`, `internal`, `sub_workflow`, or a cloud task type (see `jcwf_generation_guide.md` §2 for the full list). |
| `label` | no | string | Human-readable task name. |
| `doc` | no | string | Documentation. |
| `mode` | no | string | `"single"` (default) or `"per_item"`. |
| `depends_on` | no | array | Task IDs that must complete before this runs. |
| `working_directory` | no | string | Relative to the sub-workflow file directory. Use `""` for the workflow file directory itself. |
| `file_inputs` | no | array | Input file paths (relative to working_directory). |
| `file_outputs` | no | array | Output file paths (relative to working_directory). |
| `materialize` | no | object | Copy file_inputs to local names: `{"{{input[0]}}": "local.txt"}` |
| `params` | no | object | Type-specific parameters. |
| `inputs` | no | object | Named input slots (for dataflow wiring). |
| `outputs` | no | object | Named output slots (for dataflow wiring). |
| `timeout_ms` | no | integer | Task-level timeout in milliseconds. |
| `retries` | no | object | `{ "max_attempts": N, "backoff_ms": M }` |

---

## 3. Task Types

### 3.1 `shell` — Execute a script

```jsonc
{
  "id": "compile",
  "type": "shell",
  "params": {
    "command": "scripts/build.sh",
    "args": ["{{input[0]}}", "--output", "{{output[0]}}"]
  },
  "working_directory": "cleanup/01_compile",
  "file_inputs": ["source.cpp"],
  "file_outputs": ["output/binary"]
}
```

- `params.command` (REQUIRED): Must start with `scripts/`. Resolved relative to JarvisAgent launch directory.
- `params.args` (optional): Array of strings. May use `{{input[i]}}` and `{{output[i]}}` macros.
- `working_directory`: Where the script runs. Relative to sub-workflow file directory.

### 3.2 `ai_call` — Call an AI model

AI calls work through a queue folder mechanism via `queue_binding`.

```jsonc
{
  "id": "summarize",
  "type": "ai_call",
  "queue_binding": {
    "stng_files": ["stng_tone.txt"],
    "task_files": ["task_instructions.txt"],
    "prob_files": [{ "path": "prob_request.txt", "content": "Summarize this: {{inputs.text}}" }]
  },
  "working_directory": "",
  "file_outputs": ["output/summary.txt"]
}
```

- `queue_binding.prob_files` (REQUIRED): At least one PROB file must be present.
- `stng_files`, `task_files`, `cntx_files`: Optional context files.
- Inline content: Use `{ "path": "...", "content": "..." }` for generated content.

### 3.3 `python` — Call a Python function

```jsonc
{
  "id": "process",
  "type": "python",
  "params": {
    "module": "scripts.process_data",
    "function": "run"
  },
  "working_directory": "cleanup/02_process",
  "file_inputs": ["data.csv"],
  "file_outputs": ["processed.csv"]
}
```

- `params.module` (REQUIRED): Python module path (dot-separated). Must start with `scripts.`.
- `params.function` (REQUIRED): Function name to call.
- The function receives a context dict with `_file_input_0`, `_task_working_directory`, etc.

### 3.4 `internal` — Built-in C++ action

```jsonc
{
  "id": "cleanup",
  "type": "internal",
  "params": { "action": "update_status" }
}
```

Used for built-in operations like status updates or queue coordination.

---

## 4. Dependencies

```jsonc
"depends_on": ["task_a", "task_b"]
```

- Forms a **DAG** — cycles are rejected at load time.
- A task runs only when all `depends_on` tasks have succeeded (or been skipped).
- Tasks with no `depends_on` are root tasks (run immediately when the sub-workflow starts).

---

## 5. Dataflow

```jsonc
"dataflow": [
  { "from_task": "extract", "from_output": "result", "to_task": "transform", "to_input": "data" }
]
```

- Wires a named output slot of one task to a named input slot of another.
- Implicitly adds a dependency from `from_task` to `to_task`.

---

## 6. Defaults

```jsonc
"defaults": {
  "timeout_ms": 60000,
  "retries": { "max_attempts": 2, "backoff_ms": 1000 },
  "ai": { "provider": "openai_gpt4", "model": "gpt-4.1-mini" }
}
```

Applied to tasks that don't override these fields individually.

---

## 7. Control Nodes & Controlflow (v1.1)

Branch nodes enable conditional execution based on task success/failure.

```jsonc
"control_nodes": [
  { "id": "branch_1", "type": "branch", "label": "Check compile" }
],
"controlflow": [
  { "from": "compile", "to": "branch_1", "kind": "normal", "from_port": "dep-source", "to_port": "cf-in-normal" },
  { "from": "compile", "to": "branch_1", "kind": "error_signal", "from_port": "error-signal", "to_port": "cf-in-error" },
  { "from": "branch_1", "to": "notify_success", "kind": "normal", "from_port": "cf-out-normal", "to_port": "dep-target" },
  { "from": "branch_1", "to": "handle_error", "kind": "on_error", "from_port": "cf-out-error", "to_port": "dep-target" }
]
```

- Tasks that feed a branch must set `"expose_error_signal": true`.
- Tasks activated by controlflow edges have no `depends_on` — they are gated by the branch.

---

## 8. Filters (v1.1)

```jsonc
"filters": [
  {
    "id": "items",
    "source": { "kind": "text_lines", "path": "items.txt" },
    "binding": "item"
  }
]
```

- A task with `"mode": "per_item"` and `"filter": "items"` runs once per item.
- `binding` is the prefix for injected input values (e.g., `item.line`).

---

## 9. Key Differences from Top-Level Workflows

1. **No triggers**: Sub-workflows cannot have `triggers`. They are always started by a parent.
2. **No `manual_start`**: This field is not applicable.
3. **File paths**: `working_directory` is relative to the sub-workflow's own file directory, not the parent's.
4. **Scripts**: `params.command` and `params.module` paths are still relative to JarvisAgent launch directory (same as top-level).
5. **Scope**: AI generation operates on this canvas level only — do not generate tasks for parent or child canvases.

---

## 10. Common Pitfalls

1. **Do not add `triggers` or `manual_start`** — these are top-level-only features.
2. **`working_directory` must be relative to the sub-workflow file**, not the parent workflow.
3. **`scripts/` paths** are always relative to the JarvisAgent launch directory, regardless of nesting level.
4. **Task IDs must be unique within this sub-workflow** but are independent of parent workflow task IDs.
5. **Do not reference parent workflow tasks** in `depends_on` — dependencies are local to this sub-workflow.
6. **Queue folder paths** for `ai_call` tasks follow the same `../queue/` convention, resolved from this sub-workflow's working directory.
7. Every task must have a `type` field.
8. `depends_on` must only reference task IDs within this sub-workflow's `tasks` map.
