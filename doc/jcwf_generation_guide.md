# JCWF Generation Guide

This is a condensed reference for generating valid JC Workflow JSON files.
It covers the JSON structure, task types, dependencies, data flow, controlflow,
and common patterns. For the full specification, see `JC_Workflow_Specification.md`.

**Note:** `.jcwf` files are now zip containers. The JSON files inside use `.json` extension.
This guide covers the root canvas JSON content. For sub-workflow canvases, see
`sub-jcwf_generation_guide.md`.

---

## 1. Root Object

A workflow canvas is valid JSON with extension `.json` (inside a `.jcwf` container).
For the root canvas, global metadata (version, id, triggers, defaults) comes from
`global.json` in the container. The canvas JSON itself focuses on tasks and dataflow.

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
| `type` | YES | string | `"shell"`, `"ai_call"`, `"python"`, `"internal"`, `"sub_workflow"`, or a cloud task type (see §3 and `JC_Workflow_Specification.md` §3.3.1). Cloud types: `"polarion_write"`, `"s3"`, `"db_query"`, `"onedrive_upload"`, `"onedrive_download"`, `"snowflake_query"`, `"slack_message"`, `"email_send"`, `"email_read"`, `"github_issue"`, `"jira_issue"`, `"sheets_read"`, `"sheets_write"`, `"azure_blob_upload"`, `"azure_blob_download"`, `"gcs_upload"`, `"gcs_download"`. |
| `label` | no | string | Display name. |
| `doc` | no | string | Documentation. |
| `depends_on` | no | array of strings | Task IDs that must complete before this task runs. Forms a DAG (no cycles). |
| `working_directory` | no | string | Task working dir, relative to workflow file directory. |
| `params` | no | object | Type-specific parameters (see below). |
| `file_inputs` | no | array of strings | Files this task reads (for freshness checks and positional args). Values are **relative to `working_directory`** — use bare filenames (e.g. `"input.log"`), NEVER prefix with the working_directory path. |
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
    "args": ["{{input[0]}}", "--output", "{{output[0]}}"]
  },
  "working_directory": "myWorkflow/01_compile",
  "file_inputs": ["source.cpp"],
  "file_outputs": ["output/binary"]
}
```

- `params.command` (REQUIRED): Must start with `scripts/`. Resolved relative to JarvisAgent launch directory.
- `params.args` (optional): Array of strings. May use `{{input[i]}}` and `{{output[i]}}` to reference resolved `file_inputs`/`file_outputs` paths.
- **Auto-injection (Option B)**: If `args` is omitted or contains NO `{{input[`/`{{inputs}}`/`{{output[`/`{{outputs}}` macros, the executor auto-injects individual `{{input[0]}}`, `{{input[1]}}`, … as the first positional args, followed by `{{output[0]}}`, `{{output[1]}}`, … So `file_inputs` become `$1`, `$2`, … and `file_outputs` become the next positional args. **Do NOT put literal file paths in `args` if they duplicate `file_inputs`/`file_outputs`** — that causes doubled arguments. Prefer omitting `args` entirely (recommended for simple scripts) and letting auto-injection handle it, or use explicit `{{input[i]}}`/`{{output[i]}}` macros.
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

#### Exposing the AI response to downstream non-ai_call tasks

When a downstream `cloud_*`, `shell`, or `python` task needs the AI response as a file path (via `{{taskId.output_file}}` or `{{taskId.<slot>}}`), declare an `outputs` slot on the `ai_call` task — do **NOT** use `file_outputs`:

```jsonc
"ai_bug_report": {
  "type": "ai_call",
  "working_directory": "../../queue/myWorkflow/01_generate",
  "outputs": { "bug_report": { "type": "string" } },  // ✓ slot auto-maps to PROB_*.output.txt
  // NOT: "file_outputs": ["bug_report.txt"]            // ✗ writes a second file inside queue/
  "queue_binding": { /* STNG + CNTX + TASK + PROB */ }
}
```

The runtime maps the declared slot to the natural `PROB_*.output.txt` file the SessionManager produces. Downstream tasks then use `{{ai_bug_report.output_file}}` or `{{ai_bug_report.bug_report}}` — both resolve to the absolute path of the AI response.

**Why not `file_outputs` on `ai_call`?** `file_outputs` paths are resolved against the task's `working_directory`. For `ai_call` that directory is inside `queue/`, so the output file lands inside a watched queue folder. The file categorizer treats any file in a queue folder that doesn't match STNG/CNTX/TASK/PROB/PROV/`*.output.*` as a new requirements file and fires an **extra AI call** — wasted tokens and latency. Always use the `outputs` slot pattern on `ai_call`.

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
  "id": "parse_log",
  "type": "python",
  "working_directory": "myWorkflow/01_parse",
  "params": {
    "module": "scripts.parseLog",
    "function": "extract_stats"
  },
  "file_inputs": ["input_data.log"],
  "file_outputs": ["parsed_stats.json"]
}
```

- `params.module`: Python import path. New scripts MUST be placed in `scripts/` (e.g. `"scripts.parseLog"`). The `scripts/` directory is on sys.path.
- `params.function`: Callable name in the module. **The function MUST exist** in the script with this exact name.

#### Calling Convention (IMPORTANT)

The runtime calls `module.function(**kwargs)` **programmatically** — scripts are NOT invoked via CLI. Do NOT use `sys.argv`, `argparse`, or `main()` as the entry point.

The function receives:
- **Keyword arguments** from upstream task outputs (wired through the DAG).
- **`context` dict** (optional kwarg) containing resolved file paths and metadata:
  - `context["_file_input_0"]`, `context["_file_input_1"]`, … — absolute paths to `file_inputs`
  - `context["_task_working_directory"]` — absolute path to the task's working directory
  - `context["_workflow_base_directory"]` — absolute path to the workflow base

#### Python Script Pattern

```python
def extract_stats(context=None, **kwargs):
    import json, os
    input_path = context["_file_input_0"]      # resolved from file_inputs[0]
    work_dir = context["_task_working_directory"]
    output_path = os.path.join(work_dir, "parsed_stats.json")

    with open(input_path) as f:
        data = f.read()
    # ... process data ...
    result = {"key": "value"}

    with open(output_path, "w") as f:
        json.dump(result, f, indent=2)

    return {"parsed_stats": output_path}  # optional: expose as output slot
```

#### Wiring python output → ai_call cntx_files

To feed a python task's `file_outputs` into a downstream `ai_call`, reference the file relative to the ai_call's `working_directory`. Since ai_call working directories are in `../queue/` (3 levels from the JarvisAgent root) and python outputs are in `workflows/` (also relative to root), you need **3 levels up** then back down into `workflows/`:

```jsonc
// ai_call working_directory: "../queue/myWorkflow/02_generate"
// python working_directory:  "myWorkflow/01_parse"
// python file_outputs:       ["parsed_stats.json"]
//
// Path from ai_call dir:  queue/myWorkflow/02_generate
//   ../../../              → <jarvisAgent root>
//   workflows/myWorkflow/01_parse/parsed_stats.json
"cntx_files": [
  "../../../workflows/myWorkflow/01_parse/parsed_stats.json"
]
```

The runtime copies the file into the ai_call's queue folder as `CNTX_parsed_stats.json`.

**Important:** Do NOT use only `../../` — that only reaches the `queue/` parent, not the `workflows/` directory. Always use `../../../workflows/` to cross from queue to workflows.

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

### 3.5 `sub_workflow` — Execute a child workflow

```jsonc
{
  "id": "run_cleanup",
  "type": "sub_workflow",
  "label": "Run cleanup sub-workflow",
  "workflow_file": "subworkflows/cleanup.jcwf",
  "depends_on": ["prepare_data"]
}
```

- `workflow_file` (REQUIRED): Path to a child `.jcwf` file, relative to the parent workflow's file directory.
- The child workflow is a standalone JCWF file loaded from the `workflows/` directory (scanned recursively).
- The parent task enters `WaitingExternal` state until the child workflow completes.
- If the child succeeds, the parent task succeeds; if the child fails or is cancelled, the parent task fails.
- Sub-workflows do **not** have triggers — they are always invoked by a parent.
- Multiple parents can reference the same sub-workflow file (reuse).
- Sub-workflows may contain nested `sub_workflow` tasks (max depth: 10).

**When to generate a `sub_workflow` node:** When the user's prompt describes a group of tasks that should be encapsulated as a reusable unit. Generate the `sub_workflow` node at the parent canvas level — the child workflow content is generated separately when the user edits that canvas.

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
- Control node IDs MUST NOT collide with any task ID. Do NOT place branch nodes in the `tasks` map — they belong only in `control_nodes`.

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
  { "type": "file_watch", "id": "on-change", "enabled": true, "params": { "path": "data/input.csv" } },
  { "type": "webhook", "id": "wh", "enabled": true, "params": { "secret": "my-shared-secret" } }
]
```

- If `triggers` is omitted: implicit auto-trigger (starts on registration).
- `manual_start: true` (default) allows manual start regardless of triggers.
- **Webhook triggers** expose the workflow at `POST /api/webhook/<workflowId>`. The request body may include `runId`, `callbackUrl`, and a `context` object (key-value pairs injected into run context). If `params.secret` is set, the caller must send `X-Webhook-Signature: sha256=<hex>` (HMAC-SHA256 of the body). Empty/missing secret = open webhook. If `callbackUrl` is provided, JarvisAgent POSTs a completion payload (`workflowId`, `runId`, `state`, `ok`, `completedAt`, per-task `tasks`) to that URL when the run finishes (fire-and-forget, 15 s timeout).

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

### Per-item output piping

When a downstream `per_item` task depends on an upstream `per_item` task (same filter),
the runtime injects the upstream's per-instance outputs as template variables:

| Variable | Resolves to |
|----------|-------------|
| `{{upstream.output_file}}` | Absolute path to the upstream instance's first output file |
| `{{upstream.captured_stdout}}` | Captured stdout (up to 1024 chars) from the matching instance |
| `{{upstream.<slotName>}}` | Named output slot value from the matching instance |

These are available in `params`, `queue_binding` content, shell args, and any other
template-expanded context. For cloud task params (JSON), values are JSON-escaped automatically.

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

### Pattern D: Per-item AI + cloud write-back (output piping)

When a `per_item` task B depends on `per_item` task A (same filter), B's instances
can reference A's outputs via `{{A.output_file}}`, `{{A.captured_stdout}}`, or
`{{A.<slotName>}}`. The runtime matches by item index automatically.

```
filter (csv) → ai_call (per_item, analyze each) → cloud_write (per_item, write AI output back)
                                                        ↑
                                        {{ai_call.captured_stdout}} piped from matching instance
```

Example — query departments, AI analyzes each, INSERT analysis back:

```jsonc
"ai_analyze": {
  "type": "ai_call",
  "mode": "per_item",
  "filter": "dept-stats",
  "file_outputs": ["analysis.txt"],
  "depends_on": ["query_departments"],
  "queue_binding": {
    "stng_files": [{ "path": "STNG_analyst.txt", "content": "You are a data analyst..." }],
    "task_files": [{ "path": "TASK_analyze.txt", "content": "Analyze the department..." }],
    "cntx_files": [{ "path": "CNTX_context.txt", "content": "Scoring context..." }],
    "prob_files": [{ "path": "PROB_{{dept.department}}.txt", "content": "Department: {{dept.department}}..." }]
  }
},
"write_analysis": {
  "type": "db_query",
  "mode": "per_item",
  "filter": "dept-stats",
  "depends_on": ["ai_analyze"],
  "params": {
    "connection": "local-pg",
    "query": "INSERT INTO analysis (dept, text) VALUES ('{{dept.department}}', $j9t${{ai_analyze.captured_stdout}}$j9t$)"
  }
}
```

Cloud task params are JSON-escaped automatically — `{{ai_analyze.captured_stdout}}` content
with quotes, newlines, or backslashes will not break the JSON structure.

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
9. **Missing 'No markdown fences' in ai_call STNG** — AI output from `ai_call` tasks is consumed directly by compilers, tools, or downstream tasks — NOT by humans. Every `stng_files` content MUST include `"No markdown fences, no explanations."` to prevent the AI from wrapping output in ` ```lang ` blocks.
10. **file_inputs values prefixed with working_directory** — `file_inputs` are resolved relative to `working_directory`. Use bare filenames only (e.g. `"input.log"`). NEVER repeat the working_directory path inside file_inputs — that doubles the path at runtime.
11. **Over-decomposed outputs** — Prefer a single combined JSON output file over splitting into multiple files. If a python task extracts statistics, write everything into ONE JSON file, not separate files per category. This simplifies downstream wiring and cntx_files references.
12. **Wrong cntx_files path crossing queue↔workflows** — To reach a python/shell task's output from an ai_call's working directory, you need `../../../workflows/<taskWorkDir>/<outputFile>` (3 levels up from `queue/X/Y` to the JarvisAgent root, then into `workflows/`). Using only `../../` reaches `queue/` — not `workflows/`.
13. **Duplicated literal paths in `args` + `file_inputs`/`file_outputs`** — If a shell task has `file_inputs` and `file_outputs`, do NOT also put the same literal paths in `args`. The executor auto-injects individual `{{input[0]}}`, `{{input[1]}}`, …, `{{output[0]}}`, `{{output[1]}}`, … when no macros are present in args. Either omit `args` entirely (recommended for simple scripts) or use explicit `{{input[i]}}` / `{{output[i]}}` macros.
14. **`file_outputs` on `ai_call` tasks** — NEVER declare `file_outputs` on an `ai_call`. The paths resolve against the task's `working_directory`, which is inside `queue/` for ai_call tasks, so the output file lands in a watched queue folder. The file categorizer then treats it as a new requirements file and fires a second wasted AI call. Use an `outputs` slot instead — it auto-maps to the natural `PROB_*.output.txt` file the SessionManager produces, and `{{taskId.output_file}}` / `{{taskId.<slot>}}` still resolve correctly for downstream consumers. See §3.2 "Exposing the AI response to downstream non-ai_call tasks" for the canonical pattern.
