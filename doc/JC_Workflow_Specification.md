# JC Workflow File Format™ (extension jcwf)

Copyright (c) 2025 JC Technolabs<br>
License: GPL-3.0

---

## Abstract

This document specifies the **JC Workflow File Format™ (JCWF)** and the corresponding execution model for **JarvisAgent**.  
JCWF is a **JSON-based** workflow description language that allows JarvisAgent to:

- Define workflows as graphs of tasks (a workflow pipeline).
- Express dependencies between tasks, including file freshness checks.
- Attach triggers (cron-like, file-based, structure-based).
- Execute tasks using C++ modules and Python scripts, including AI assistant calls.
- Monitor execution and propagate data from one task to another.
- Integrate with the JarvisAgent web UI for visualization and control.

This specification focuses on:

1. Defining Workflows (JSON schema and semantics)  
2. Managing Dependencies (task- and file-based)  
3. Handling Triggers  
4. Executing Tasks (C++ and Python responsibilities)  
5. Monitoring & Reporting  
6. Data Flow between tasks  
7. Integration with JarvisAgent’s architecture (C++, Python, Web UI)  

---

## 1. Introduction

JarvisAgent orchestrates a variety of automation tasks: document-to-markdown conversion (PDF, DOCX, XLSX, PPTX), AI assistant queries, file watching, and more.  
The JC Workflow File (JCWF) provides a declarative way to describe these tasks and their relationships, so that JarvisAgent can:

- Load workflows at startup or on demand.  
- Run them automatically upon registration, reactively (on triggers), or explicitly (manual trigger from CLI / web UI).  
- Parallelize non-dependent tasks.  
- Track progress and expose status to the web dashboard.

You can think of each workflow as a pipeline: a sequence/DAG of stages for automation tasks.

This document defines:

- The JSON structure of a `.jcwf` file.  
- The execution semantics inside JarvisAgent.  
- The interactions between C++ core, Python scripting engines, and the web UI.

### 1.1 Requirements Language

The key words **"MUST"**, **"MUST NOT"**, **"REQUIRED"**, **"SHALL"**, **"SHALL NOT"**, **"SHOULD"**, **"SHOULD NOT"**, **"RECOMMENDED"**, **"MAY"**, and **"OPTIONAL"** in this document are to be interpreted as described in [RFC 2119](https://datatracker.ietf.org/doc/html/rfc2119).

---

## 2. Terminology

- **Workflow**: A named collection of tasks, triggers, and global configuration.  
- **Task**: A unit of work (e.g., convert a document, call an AI assistant, run a Python function, invoke a shell command).  
- **Dependency**: A requirement that one or more tasks MUST complete successfully (and be up to date) before another task starts.  
- **Trigger**: A condition that starts a workflow or a task:  
  - Time-based (cron-like)  
  - File-based (XLS/CSV/Markdown changes)  
  - Structure-based (e.g., “for each subsection in chapter 3”)  
  - Manual (explicit command or UI action)
  - Auto (default trigger if no other trigger is provided)
- **Data Slot**: A named input or output field associated with a task.  
  - Example: task `convert_document` with input slot `source_path` and output slot `markdown_path`.  
- **Context / State**: A key-value store that persists data across tasks within a workflow run.  
  - Example: `context["today"] = "2025-12-01"` or `context["report_url"] = "https://..."`.  
- **Run**: A single execution instance of a workflow with its own state and logs.  
- **Workflow File Path**: The filesystem path of the loaded `.jcwf` file.
- **Workflow File Directory**: The directory that contains the loaded `.jcwf` file.
- **Workflow Base Directory**: The base directory used for resolving workflow-level relative paths.
  - If the root field `base_directory` is present and starts with `/`, it is treated as an absolute path.
  - If `base_directory` is present and is relative, it MUST be resolved relative to the Workflow File Directory.
  - If `base_directory` is omitted, the Workflow Base Directory defaults to the Workflow File Directory.
  - Relative paths MAY contain `..` segments; JarvisAgent MUST resolve them after lexical normalization.
- **Path Syntax**: This specification uses Unix-style paths with forward slashes (`/`). An absolute path MUST begin with `/`.

- **JarvisAgent Launch Working Directory**: The process current working directory at the time JarvisAgent starts (for example, the project root when launching `./bin/Release/jarvisAgent`). This directory is used to resolve `scripts/` paths for `shell` tasks.

- **JCWF Runtime**: The JarvisAgent orchestration layer that loads, validates, and runs JCWF workflows.  
- **Environment**: Optional metadata and variables attached to a task (for example, environment variables for shell tasks, or an assistant environment for AI tasks).  
- **Queue Files**: Optional STNG_, TASK_, CNTX_, PROB_ artifacts used by JarvisAgent’s queue-based execution; JCWF can reference these explicitly per task.
  - In addition to file paths, a JCWF MAY embed queue file content inline (see 3.3.6) to make the workflow self-contained.

---

## 3. JCWF JSON Specification

JCWF files MUST be valid JSON documents.  
The file extension SHOULD be `.jcwf`.

### 3.1 Root Object

The root object has the following top-level fields:

```jsonc
{
  "version": "1.0",
  "id": "daily-report",
  "label": "Daily Reporting Workflow",
  "doc": "Generates a daily report from XLS and sends it to an AI assistant for summarization.",
  "base_directory": ".",
  "triggers": [ /* see 3.2 */ ],
  "tasks": { /* see 3.3 */ },
  "dataflow": [ /* see 3.5 */ ],
  "defaults": { /* see 3.6 */ }
}
```

#### 3.1.1 Fields

- `version` (REQUIRED, string)  
  - The JCWF spec version. For this document, `"1.0"` is assumed.  
  - Implementations MUST reject unknown major versions.

- `id` (REQUIRED, string)  
  - Unique identifier for this workflow within the JarvisAgent environment.  
  - When workflows are created externally (e.g., from the web UI), the UI SHOULD generate an ID that is unique and stable (for example, a UUID or a slug).  
  - JarvisAgent MUST reject a new workflow whose `id` collides with an already loaded workflow.

- `label` (OPTIONAL, string)  
  - Human-friendly name for UI display.

- `doc` (OPTIONAL, string or array of strings)  
  - Documentation or comments about the workflow.

- `base_directory` (OPTIONAL, string)  
  - Workflow base directory override used for resolving workflow-level relative paths (see 3.1.2).  
  - If relative, it MUST be resolved relative to the Workflow File Directory. If omitted, it defaults to the Workflow File Directory.
- `triggers` (OPTIONAL, array of trigger objects)  
  - Defines when and how the workflow starts. See 3.2.

- `tasks` (REQUIRED, object)  
  - Map from taskId to a task specification. See 3.3.

- `dataflow` (OPTIONAL, array)  
  - Explicit data wiring between task outputs and inputs. See 3.5.

- `defaults` (OPTIONAL, object)  
  - Default settings for tasks, retries, timeouts, etc. See 3.6.

#### 3.1.2 Path Resolution

JarvisAgent MUST resolve paths deterministically and independent of the process current working directory.

**Workflow-level paths**

1. Determine the **Workflow File Directory** as the directory containing the loaded `.jcwf` file.
2. Determine the **Workflow Base Directory** as follows:
   - If `base_directory` is present:
     - If it starts with `/`, treat it as an absolute path.
     - Otherwise, resolve it relative to the Workflow File Directory.
   - If `base_directory` is omitted, the Workflow Base Directory MUST default to the Workflow File Directory.
3. Unless explicitly stated otherwise, any **workflow-level** relative path (for example trigger paths) MUST be resolved relative to the Workflow Base Directory.

**Task-level paths**

1. Each task MAY define `working_directory`.
   - If `working_directory` is omitted, it MUST default to the Workflow Base Directory.
   - If `working_directory` is relative, it MUST be resolved relative to the Workflow Base Directory.
   - Task working directories MAY contain `..` segments; JarvisAgent MUST resolve them after lexical normalization (escaping upward is allowed).
2. Unless explicitly stated otherwise for a specific field, any **task-scoped** relative file path MUST be resolved relative to the task `working_directory`, including (but not limited to):
   - `file_inputs` and `file_outputs`
   - `queue_binding` file references (`stng_files`, `task_files`, `cntx_files`, `prob_files`), including inline `{ "path": "...", "content": "..." }`

**Exceptions**

- For `shell` tasks, `params.command` MUST start with `scripts/` and MUST be resolved relative to the JarvisAgent Launch Working Directory (not the workflow/task directories).

**Directory creation**

- Before executing a task, JarvisAgent MUST ensure the resolved task `working_directory` exists (create directories as needed).
- If JarvisAgent writes a file on behalf of the workflow (for example inline queue files with `content`, or other runtime-generated artifacts), it MUST create the parent directory of the target path if it does not exist.

This enables workflows to be run from any current working directory without writing artifacts into the launch directory by accident.

---

### 3.2 Triggers

A workflow MAY be started by one or more triggers. Manual start is always allowed unless explicitly disabled.

Each trigger has:

- If no trigger is provided in the JCWF file, 'auto' is assumed as the default trigger.

```jsonc
{
  "type": "auto | cron | file_watch | structure | manual",
  "id": "morning-run",
  "enabled": true,
  "params": { /* type dependent */ }
}
```

#### 3.2.1 Cron Triggers

```jsonc
{
  "type": "cron",
  "id": "every-morning",
  "enabled": true,
  "params": {
    "expression": "0 8 * * *",
    "timezone": "America/Los_Angeles"
  }
}
```

- `expression` (REQUIRED) is a standard 5-field cron expression.  
- `timezone` (OPTIONAL) defaults to system time.

#### 3.2.2 File-Watch Triggers

```jsonc
{
  "type": "file_watch",
  "id": "on-xls-change",
  "enabled": true,
  "params": {
    "path": "data/report.xlsx",
    "events": ["modified", "created"],
    "debounce_ms": 2000
  }
}
```

- Tied to JarvisAgent’s existing FileWatcher.  
- MUST map events to JarvisAgent event types.

#### 3.2.3 Structure-Based Triggers

Used for “for each row / section” style operations. These do not schedule time; rather, they define how to expand tasks when the workflow is triggered.

```jsonc
{
  "type": "structure",
  "id": "each-row-in-xls",
  "enabled": true,
  "params": {
    "source": {
      "kind": "xls",
      "path": "data/tasks.xlsx",
      "sheet": "Sheet1"
    },
    "iterator": {
      "mode": "rows",
      "range": "A2:D100",
      "binding": "row"
    }
  }
}
```

At runtime, the engine will expand designated tasks from this iterator (see tasks with `"mode": "per_item"` in 3.3.2).

#### 3.2.4 Auto Triggers

```jsonc
{
  "type": "auto",
  "id": "auto-trigger",
  "enabled": true,
  "params": {}
}
```

- This trigger type starts the workflow automatically upon registration without any additional parameters.


```jsonc
{
  "type": "manual",
  "id": "manual-run",
  "enabled": true,
  "params": {
    "exposed_in_ui": true
  }
}
```

Manual triggers are exposed in the web UI and/or CLI.

---

### 3.3 Tasks

The `tasks` field is a mapping from taskId to a task object:

```jsonc
"tasks": {
  "load_xls": { ... },
  "summarize": { ... },
  "notify": { ... }
}
```

Each task has:

```jsonc
{
  "id": "summarize",
  "type": "python | shell | ai_call | internal",
  "label": "Summarize report with AI",
  "doc": "Sends the prepared text to an AI assistant and stores the answer.",
  "mode": "single | per_item",
  "depends_on": ["load_xls"],
  "working_directory": "output",
  "file_inputs": ["data/report.xlsx"],
  "file_outputs": ["output/report.summary.txt"],
  "environment": { /* see 3.3.6 */ },
  "queue_binding": { /* see 3.3.6 */ },
  "params": { /* type-specific */ },
  "inputs": { /* named inputs */ },
  "outputs": { /* named outputs */ },
  "timeout_ms": 600000,
  "retries": {
    "max_attempts": 3,
    "backoff_ms": 1000
  }
}
```

#### 3.3.1 Task Types

- `python`  
  Executes a function or script via the PythonEngine.

  ```jsonc
  {
    "id": "convert_document",
    "type": "python",
    "params": {
      "module": "workflows.doc_tasks",
      "function": "convert_to_markdown"
    },
    "inputs": {
      "source_path": { "type": "string" },
      "output_dir": { "type": "string" }
    },
    "outputs": {
      "markdown_path": { "type": "string" }
    },
    "file_inputs": ["${inputs.source_path}"],
    "file_outputs": ["${outputs.markdown_path}"]
  }
  ```

**Python module/function resolution**

- Python tasks **MUST** specify `params.module` and `params.function`.
- `params.module` is a normal Python import name.
  JarvisAgent adds the `scripts/` directory (relative to the Launch Working Directory)
  to `sys.path`.
- A script file like `scripts/combineJarvisDocumentation.py` is referenced as:
  - `"module": "combineJarvisDocumentation"`
  - `"function": "<callable name inside the file>"`
- If the module cannot be imported, or the function is missing / not callable,
  the task fails and the error message is recorded and logged.


- `shell`  
  Executes a command on the host (JarvisAgent SHOULD restrict/whitelist this).  
  Security rule: shell commands MUST start with `scripts/` (relative to the JarvisAgent Launch Working Directory).

  ```jsonc
  {
    "id": "run_script",
    "type": "shell",
    "params": {
      "command": "scripts/run_something.sh",
      "args": ["--flag", "value"]
    }
  }
  ```

  **Argument templating (positional file arguments):**

  - If a shell task declares `file_inputs` and/or `file_outputs`, the runtime MUST provide positional template variables:
    - `${input[i]}` expands to the resolved path of `file_inputs[i]`
    - `${output[i]}` expands to the resolved path of `file_outputs[i]`
  - `args` MAY include additional literal flags (for example `"-O3"`) alongside `${input[i]}` / `${output[i]}`.

- `ai_call`  
  A high-level “call AI” task that routes via Python or C++ backend.

  ```jsonc
  {
    "id": "ask_ai",
    "type": "ai_call",
    "params": {
      "provider": "openai",
      "model": "gpt-4.1-mini",
      "mode": "one_shot",  // or "assistant"
      "prompt_template": "Summarize the following report:
{{report_text}}"
    },
    "inputs": {
      "report_text": { "type": "string" }
    },
    "outputs": {
      "summary": { "type": "string" }
    }
  }
  ```

  - `mode: "one_shot"` means a single request/response using the provided inputs.  
  - `mode: "assistant"` means using a long-lived assistant environment (see 3.3.6) where less context may be passed each time because conversation state is maintained externally.

- `internal`  
  Built-in C++ actions, such as updating status, writing to ChatMessagePool, or coordinating queue artifacts.

#### 3.3.2 Mode: `single` vs `per_item`

- `mode: "single"` (default)  
  Task executes once per workflow run.

- `mode: "per_item"`  
  Task is expanded per iterator item (for example, each row in XLS, each subsection in a document).  
  The expansion is driven by a structure trigger (3.2.3) or by explicit dataflow list sources.

Example:

```jsonc
{
  "id": "summarize_section",
  "type": "ai_call",
  "mode": "per_item",
  "params": {
    "provider": "openai",
    "model": "gpt-4.1-mini",
    "mode": "assistant"
  },
  "inputs": {
    "section_text": { "type": "string" },
    "section_title": { "type": "string" }
  },
  "outputs": {
    "section_summary": { "type": "string" }
  },
  "file_inputs": ["${inputs.section_text}"],
  "file_outputs": ["output/sections/${inputs.section_title}.summary.txt"]
}
```

#### 3.3.3 Timeouts and Retries

- `timeout_ms` (OPTIONAL, integer)  
  Maximum execution time per task instance. If exceeded, the task is considered failed.

- `retries` (OPTIONAL, object)  
  - `max_attempts` (integer)  
  - `backoff_ms` (integer) linear backoff between retries.  
  Implementations MAY extend this with exponential strategies later.

#### 3.3.4 Inputs & Outputs (Data Slots)

Inputs and outputs are declared to aid validation and UI:

```jsonc
"inputs": {
  "source_path": { "type": "string", "required": true },
  "config": { "type": "object", "required": false }
},
"outputs": {
  "markdown_path": { "type": "string" }
}
```

- Each key is a data slot name.  
- Types are advisory but useful for sanity checks and editor tooling.

**Relationship to `file_inputs` / `file_outputs`:**

- `inputs` / `outputs` describe **named data slots** for validation and `dataflow` wiring.  
- `file_inputs` / `file_outputs` describe **file dependencies** for freshness checks and (for shell tasks) positional argument templating via `${input[i]}` / `${output[i]}`.
- A shell task MAY omit `inputs` entirely and still run, as long as its `params` are resolvable (for example, only using `${input[i]}` / `${output[i]}`).
- When a task defines named `outputs` and also defines `file_outputs`, the runtime MAY populate output slot values with the corresponding `file_outputs` paths (commonly the first output slot maps to `file_outputs[0]`) to support `dataflow` wiring in Makefile-style workflows.

#### 3.3.5 Clean Tasks

A workflow MAY define a dedicated `clean` task that removes generated artifacts. For example:

```jsonc
"tasks": {
  "clean": {
    "id": "clean",
    "type": "shell",
    "label": "Clean artifacts",
    "params": {
      "command": "scripts/clean_artifacts.sh"
    }
  }
}
```

The orchestrator or UI MAY expose a “clean” action that simply runs this task (ignoring usual dependency checks).

#### 3.3.6 Environment and Queue Integration (STNG_, TASK_, CNTX_, PROB_)

Tasks MAY describe additional environment and queue-related details:

```jsonc
"environment": {
  "name": "assistant_env_daily_reports",
  "variables": {
    "PROJECT": "DailyReports",
    "LOCALE": "en-US"
  },
  "assistant_id": "daily-report-assistant"
},
"queue_binding": {
  "stng_files": ["STNG_daily.txt"],
  "task_files": ["TASK_summarize.txt"],
  "cntx_files": ["CNTX_daily.txt"],
  "prob_files": ["PROB_daily.txt"]
}
```

- `environment` (OPTIONAL, object)  
  - `name` (OPTIONAL, string): Logical name for this environment.  
  - `variables` (OPTIONAL, object): Key-value environment variables for shell or Python tasks.  
  - `assistant_id` (OPTIONAL, string): Identifier for an AI assistant environment.  
    - For `ai_call` tasks with `mode: "assistant"`, this MAY reference a preconfigured assistant that keeps its own long-lived context. In that case, JCWF does not need to send full context on every call; the backend can rely on the assistant’s stored state.

- `queue_binding` (OPTIONAL, object)  
  - `stng_files` (OPTIONAL, array): STNG_ settings files associated with this task, for example, the tone of the AI.  
  - `task_files` (OPTIONAL, array): TASK_ files representing work instructions.  
  - `cntx_files` (OPTIONAL, array): CNTX_ context files (background information).  
- `prob_files` (OPTIONAL, array): PROB_ files representing problems/requests (the concrete work item) that the task should address.  

Each entry in `stng_files` / `task_files` / `cntx_files` / `prob_files` MAY be either:

- A string (path to an existing file), or
- An inline object with file content:

```jsonc
{ "path": "TASK_summarize.txt", "content": "Summarize the report in 5 bullets." }
```

If `content` is present, the runtime MUST write (or overwrite) the file at `path` before the task executes.
This inline form is RECOMMENDED when a workflow is generated automatically and should be runnable without additional files.


**Path resolution for `queue_binding`:**

- If a `queue_binding` entry is a relative path string, it MUST be resolved relative to the task `working_directory`.
- If a `queue_binding` entry is an inline object `{ "path": "...", "content": "..." }` and `path` is relative, it MUST be resolved relative to the task `working_directory`.
- When writing inline `content`, the runtime MUST create the parent directory of the target `path` if it does not exist.

JarvisAgent MAY use `queue_binding` to map between high-level tasks and the low-level queue directories. A task can thus have an explicit array of associated STNG_/TASK_/CNTX_/PROB_ files when it is an AI or queue-integrated task.

---

### 3.4 Dependency Semantics and Up-to-Date Checks

JCWF models dependencies per task. Each task can declare:

- `depends_on` (OPTIONAL, array of task IDs)  
  Other tasks that must succeed before this task is considered ready.  

- `file_inputs` (OPTIONAL, array of strings)  
  Files or patterns this task reads from.  

- `file_outputs` (OPTIONAL, array of strings)  
  Files or patterns this task produces or updates.

Example for “chunk an MD file if the output is missing or stale”:

```jsonc
"tasks": {
  "chunk_book": {
    "id": "chunk_book",
    "type": "python",
    "label": "Chunk MD file book.md",
    "params": {
      "module": "workflows.chunking",
      "function": "chunk_markdown_file"
    },
    "inputs": {
      "input_path": { "type": "string", "required": true },
      "output_path": { "type": "string", "required": true }
    },
    "file_inputs": ["book.md"],
    "file_outputs": ["book.output.md"]
  }
}
```

Conceptually, this is similar to how a Makefile decides whether a target is up to date.

Rules:

1. Task graph  
   - `depends_on` defines a task-level DAG.  
   - If a task has no `depends_on`, it is considered a root task (subject to triggers).  
   - The workflow MUST NOT contain cycles in `depends_on`. Cycles SHOULD be detected and rejected at load time.

2. Up-to-date check  
   - A task MAY be skipped as “up to date” if all of the following are true:  
     - All `file_outputs` exist, and  
     - Each `file_output` has a modification time newer than or equal to every `file_input` and all upstream outputs from `depends_on` tasks.  
   - If any `file_output` is missing, or any `file_input` or upstream output is newer, the task is considered stale and MUST run.  
   - If `file_inputs` or `file_outputs` are omitted, the engine MUST assume the task is not up to date and SHOULD run it whenever its dependencies are satisfied.
   - If a task is skipped as “up to date”, the runtime SHOULD treat it as successful and its `file_outputs` MUST be considered available to downstream tasks (for both readiness and freshness comparisons).

3. Per-item mode  
   - For `mode: "per_item"` tasks, the same freshness rules apply per item.  
   - `file_inputs` and `file_outputs` may use templates (for example, `"output/${inputs.section_title}.summary.txt"`). The runtime evaluates templates per item before checking timestamps.

4. Interaction with triggers  
   - A trigger (cron, file, structure, manual) creates a new workflow run.  
   - Within that run, each task is examined for readiness and freshness as above.  
   - It is valid to model a “no-op” run where all tasks are up to date and thus skipped.

---

### 3.5 Data Flow

Optional explicit wiring of outputs to inputs.

```jsonc
"dataflow": [
  {
    "from_task": "load_xls",
    "from_output": "rows",
    "to_task": "summarize_section",
    "to_input": "section_text",
    "mapping": {
      "use_field": "A"  // e.g., from XLS column A
    }
  },
  {
    "from_task": "summarize_section",
    "from_output": "section_summary",
    "to_task": "notify",
    "to_input": "body"
  }
]
```

Semantics:

- The runtime MUST ensure that the source task has completed (or was skipped as up to date) and produced the referenced output before starting the target task.  
- For `per_item` tasks, dataflow can create fan-out: one `load_xls` task to many `summarize_section` tasks.

If `dataflow` is omitted, tasks may rely purely on the workflow state (context) or external files.

---

### 3.6 Defaults

`defaults` supplies common configuration inherited by tasks unless overridden.

```jsonc
"defaults": {
  "timeout_ms": 600000,
  "retries": {
    "max_attempts": 2,
    "backoff_ms": 1000
  },
  "ai": {
    "provider": "openai",
    "model": "gpt-4.1-mini"
  }
}
```

Task-specific fields override defaults at the same key path.

---

## 4. Execution Model

This section describes how JarvisAgent should execute JCWF workflows across the C++ core, Python engines, and the web UI.

### 4.1 High-Level Flow

1. Load `.jcwf` files from a configured directory.  
2. Validate JSON structure, triggers, and the `depends_on` DAG (no cycles).  
3. Register workflows and triggers with the JarvisAgent core.  
4. On trigger activation (cron, file, structure, manual):  
   - Create a workflow run instance with its own ID and context.  
   - Resolve ready tasks:  
     - All `depends_on` tasks succeeded (or were skipped as up to date), and  
     - Inputs are resolvable, and  
     - The task is not up to date (or up-to-date checking is disabled).  
   - Schedule tasks on worker pools and/or Python engines.  
5. Monitor task states, store outputs in a run-local state store (context).  
6. Propagate data as specified by `dataflow`.  
7. Update the web UI with real-time status (pending, running, skipped, success, failed).  
8. Mark the workflow run completed when no further tasks can run.

---

### 4.2 C++ Side: Core Orchestrator

Responsibilities:

- Parse and hold in-memory representation of workflows:  
  - `WorkflowDefinition` (id, label, triggers, tasks, dataflow).  
- Store the workflow **file path**, **Workflow File Directory**, and **Workflow Base Directory** when loading a `.jcwf`, and resolve workflow/task relative paths as specified in 3.1.2.
- Listen to cron and file events and map them to workflow triggers.  
- Maintain a `WorkflowRun` object per workflow execution.  
- Perform dependency resolution and ready-task scheduling using `depends_on` and file freshness.  
- Assign tasks to:  
  - PythonEngine instances (for `python` and `ai_call` tasks using Python).  
  - Internal handlers (for `internal` tasks).  
  - Shell executor (for `shell` tasks, if allowed).  
- Track task status (`Pending`, `Ready`, `Running`, `Skipped`, `Succeeded`, `Failed`).  
- Emit events for UI and logging (for example, `WorkflowRunStartedEvent`, `TaskStatusChangedEvent`).

Recommended data structures (conceptual):

- `WorkflowDefinition`  
  - `std::string id;`  
  - `std::string filePath;`  
  - `std::string fileDirectory;`  
  - `std::string baseDirectory;`  
  - `std::unordered_map<std::string, TaskDef> tasks;`  
  - `std::vector<DataflowDef> dataflows;`  
  - `std::vector<TriggerDef> triggers;`

- `TaskDef`  
  - `std::string id;`  
  - `std::vector<std::string> dependsOn;`  
  - `std::vector<std::string> fileInputs;`  
  - `std::vector<std::string> fileOutputs;`  
  - `TaskType type;`  
  - `TaskMode mode;`  
  - `JsonLike params;`  
  - `TaskIO inputs;`  
  - `TaskIO outputs;`  
  - `EnvironmentDef environment;`  
  - `QueueBindingDef queueBinding;`

- `WorkflowRun`  
  - `std::string runId;`  
  - `std::string workflowId;`  
  - `RunState state;`  
  - `std::unordered_map<std::string, TaskInstanceState> taskStates;`  
  - `JsonLikeState context;`

The core orchestrator SHOULD be deterministic and thread-safe. It MAY use a task queue and worker threads, but heavy work (document conversion, AI calls) is delegated to Python or external processes.

For tasks that integrate with the existing queue system, the orchestrator MAY map task execution to creation or consumption of STNG_, TASK_, CNTX_, and PROB_ files in the queue directories according to `queue_binding`.

---

### 4.3 Python Side: Task Executors

Python is responsible for:

- Implementing `python` tasks’ logic.  
- Acting as a bridge for `ai_call` tasks (HTTP requests to AI providers).  
- Optional helpers for reading XLS, documents, and other sources for structure-based iteration.

Python interface expectations:

- For `python` tasks:

  ```python
  # Example module: workflows/doc_tasks.py

  def convert_to_markdown(source_path: str, output_dir: str, context: dict) -> dict:
      # context: workflow run state (read-only or partial write)
      # return: dict of outputs, e.g. {"markdown_path": "..."}
      ...
  ```

- For `ai_call` tasks, either:  
  - A generic function that takes provider/model/mode/prompt and optional assistant/environment identifiers, or  
  - A specific module for workflow-specific logic.

Python tasks SHOULD avoid blocking the GIL unnecessarily (for example, use I/O-bound calls, async HTTP). Long-running CPU tasks MAY be done via separate processes if needed.

#### 4.3.1 Python logging and subprocess output

JarvisAgent embeds Python via `PythonEngine` and installs an in-process redirection for Python’s `sys.stdout` and `sys.stderr` so that Python output is routed through `JarvisRedirectPython(...)` into the ncurses terminal UI.

That means **regular Python output is safe** (for example `print(...)`, and the standard `logging` module when configured to write to `sys.stderr`).

However, **child processes** (anything started via `subprocess`, `os.system`, etc.) write to the **OS-level** stdout/stderr file descriptors by default and can bypass Python’s `sys.stdout`/`sys.stderr` redirection. If that output reaches the terminal directly, it can corrupt the ncurses UI.

**Rule for Python task scripts:** If you invoke external tools, you MUST capture their stdout/stderr and forward it explicitly via Python (which is redirected by `PythonEngine`) or write it to files in the task working directory.

Minimal example (capture + forward):

```python
import subprocess
import sys

completed_process = subprocess.run(
    ["some_tool", "--flag"],
    text=True,
    capture_output=True,
)

if completed_process.stdout:
    print(completed_process.stdout, end="")

if completed_process.stderr:
    print(completed_process.stderr, end="", file=sys.stderr)

completed_process.check_returncode()
```

If you need streaming output (long-running tools), use `subprocess.Popen(..., stdout=PIPE, stderr=PIPE, text=True)` and forward line-by-line.

---

### 4.4 Web UI: Monitoring and Control

The web interface SHOULD:

- List registered workflows (id, label, doc, triggers).  
- Allow manual trigger of workflows and manual cancel of runs.  
- Visualize each workflow run as:  
  - A DAG of tasks (nodes) with statuses.  
  - A table or card view of all task instances.  
- Show logs and outputs for each task (where practical).  
- Permit per-task or per-workflow debugging (for example, view context, last errors).

Data exchange between C++ and the web UI is via:

- Status snapshots broadcast by the web server.  
- HTTP routes such as `/workflows`, `/workflow/:id/runs`, `/workflow/:id/run/:runId/tasks`.

---

## 5. Managing Dependencies

### 5.1 Readiness Rule

A task instance is ready to execute when:

1. All tasks in its `depends_on` list have succeeded (or were skipped as up to date).  
2. All its declared required inputs are resolvable from one of:  
   - Dataflow links,  
   - Workflow context, or  
   - Static literals/defaults.  
3. The up-to-date check (3.4) either:  
   - Determines the task is stale, or  
   - Is explicitly disabled (implementation option).

Tasks with no `depends_on` and no missing required inputs MAY start immediately after the workflow is triggered.

### 5.2 Parallel Execution

If multiple tasks become ready at the same time and do not depend on each other (no path between them in the `depends_on` DAG), the orchestrator SHOULD run them in parallel, subject to resource limits (thread pool size, number of Python engines).

### 5.3 Failure Propagation

If a task fails (after all retries):

- Tasks that depend on it MUST NOT run, unless a future version introduces an explicit `allow_failed_prereqs: true` override.  
- The workflow run status SHOULD be marked as `Failed`, unless the failure is confined to optional branches (implementation-defined policy).

---

## 6. Handling Triggers

### 6.1 Time-Based (Cron)

- The C++ side integrates with a scheduler to check cron expressions.  
- When a cron fires, it creates a new workflow run.

### 6.2 File-Based

- Integrated with the existing FileWatcher.  
- When a relevant event is received (path and event type), the orchestrator checks matching triggers and starts a run.

### 6.3 Structure-Based

- The runtime uses the structure trigger parameters to preprocess input sources (XLS, documents, etc.).  
- It builds an iterator collection (for example, list of rows or sections).  
- For `per_item` tasks, the orchestrator creates one task instance per item, injecting the k-th item as inputs.

### 6.4 Manual

- The web UI or CLI sends a “start workflow” command to the core, referencing workflow `id` and optional initial context.

---

## 7. Monitoring and Reporting

JarvisAgent SHOULD maintain for each workflow run:

- Start time, end time, duration.  
- Task instance statuses and timestamps.  
- Error messages, if any.  
- Key outputs (as configured).

The web UI SHOULD offer:

- A summary of recent runs (success/failure).  
- Drill-down views per run.  
- Log panels for debugging tasks.

The C++ core MAY expose a compact JSON representation of workflows and runs over an HTTP endpoint for external monitoring.

---

## 8. Data Flow

Data flow is a combination of:

1. Explicit `dataflow` wiring.  
2. Shared run-level context (key-value).  
3. External files and side effects (for example, generated markdown, XLS).

### 8.1 Task Input Resolution

When a task starts, its inputs are resolved from:

1. Dataflow links (`from_task` to `to_task`).  
2. Context fields (for example, `context["config"]`).  
3. Static literals provided in `params` or `defaults` (such as `provider`, `model`).

If a required input cannot be resolved, the task MUST fail fast with a validation error.

**Note (shell tasks):** `${input[i]}` / `${output[i]}` template expansion is based on `file_inputs` / `file_outputs` and does not require named `inputs` / `outputs` declarations.

### 8.2 Outputs and Context

Task outputs MAY be written to:

- Local task outputs (for dataflow).  
- Shared context, if configured (for example, `write_to_context: true`, which may be added in a future revision).

The exact policy is left to the implementation, but JCWF is designed so that future versions can specify context writes explicitly.

---

## 9. JSON Schema (Draft)

Below is a simplified JSON Schema for JCWF v1.0. It is not exhaustive but is suitable for validation and editor tooling.

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "JC Workflow File (JCWF) v1.0",
  "type": "object",
  "required": ["version", "id", "tasks"],
  "properties": {
    "version": {
      "type": "string",
      "pattern": "^1\.0$"
    },
    "id": { "type": "string" },
    "label": { "type": "string" },
    "doc": {
      "anyOf": [
        { "type": "string" },
        { "type": "array", "items": { "type": "string" } }
      ]
    },
    "base_directory": { "type": "string" },
    "triggers": {
      "type": "array",
      "items": { "$ref": "#/$defs/trigger" }
    },
    "tasks": {
      "type": "object",
      "additionalProperties": { "$ref": "#/$defs/task" }
    },
    "dataflow": {
      "type": "array",
      "items": { "$ref": "#/$defs/dataflow" }
    },
    "defaults": {
      "type": "object",
      "additionalProperties": true
    }
  },

  "$defs": {
    "trigger": {
      "type": "object",
      "required": ["type", "id"],
      "properties": {
        "type": {
          "type": "string",
          "enum": [
            "auto",
            "cron",
            "file_watch",
            "structure",
            "manual"
          ]
        },
        "id": { "type": "string" },
        "enabled": { "type": "boolean" },
        "params": { "type": "object" }
      }
    },

    "task": {
      "type": "object",
      "required": ["id", "type"],
      "properties": {
        "id": { "type": "string" },
        "type": {
          "type": "string",
          "enum": ["python", "shell", "ai_call", "internal"]
        },
        "label": { "type": "string" },
        "doc": { "type": "string" },
        "mode": {
          "type": "string",
          "enum": ["single", "per_item"],
          "default": "single"
        },
        "working_directory": { "type": "string" },
        "depends_on": {
          "type": "array",
          "items": { "type": "string" }
        },
        "file_inputs": {
          "type": "array",
          "items": { "type": "string" }
        },
        "file_outputs": {
          "type": "array",
          "items": { "type": "string" }
        },
        "environment": {
          "type": "object",
          "properties": {
            "name": { "type": "string" },
            "assistant_id": { "type": "string" },
            "variables": {
              "type": "object",
              "additionalProperties": { "type": ["string", "number", "boolean"] }
            }
          }
        },
        "queue_binding": {
          "type": "object",
          "properties": {
            "stng_files": {
              "type": "array",
              "items": { "$ref": "#/$defs/queue_file_ref" }
            },
            "task_files": {
              "type": "array",
              "items": { "$ref": "#/$defs/queue_file_ref" }
            },
            "cntx_files": {
              "type": "array",
              "items": { "$ref": "#/$defs/queue_file_ref" }
            },
            "prob_files": {
              "type": "array",
              "items": { "$ref": "#/$defs/queue_file_ref" }
            }
          }
        },
        "params": { "type": "object" },
        "inputs": {
          "type": "object",
          "additionalProperties": {
            "type": "object",
            "properties": {
              "type": { "type": "string" },
              "required": { "type": "boolean" }
            }
          }
        },
        "outputs": {
          "type": "object",
          "additionalProperties": {
            "type": "object",
            "properties": {
              "type": { "type": "string" }
            }
          }
        },
        "timeout_ms": { "type": "integer" },
        "retries": {
          "type": "object",
          "properties": {
            "max_attempts": { "type": "integer" },
            "backoff_ms": { "type": "integer" }
          }
        }
      }
    },

    "queue_file_ref": {
      "anyOf": [
        { "type": "string" },
        {
          "type": "object",
          "required": ["path", "content"],
          "properties": {
            "path": { "type": "string" },
            "content": { "type": "string" }
          }
        }
      ]
    },

    "dataflow": {
      "type": "object",
      "required": ["from_task", "to_task"],
      "properties": {
        "from_task": { "type": "string" },
        "from_output": { "type": "string" },
        "to_task": { "type": "string" },
        "to_input": { "type": "string" },
        "mapping": { "type": "object" }
      }
    }
  }
}

```

---

## 10. Security Considerations

- `shell` tasks can be dangerous; JarvisAgent SHOULD provide configuration flags to disable or restrict them and MUST enforce the `scripts/` prefix rule.  
- `ai_call` tasks send data to external services; sensitive data MUST be handled carefully.  
- JCWF files SHOULD be sourced from trusted locations; tampering can change automation behavior.  
- Structure-based iteration over external documents and XLS files SHOULD validate inputs to avoid unexpected expansion or injection.

---

*End of JC Workflow File (JCWF) Specification v1.0*
---

## Project conventions and runtime enforcement

The JCWF **file format** is intentionally generic. Some path rules are **project/runtime policies enforced by JarvisAgent**, not JCWF schema requirements.

### Task folders

**Policy (JarvisAgent runtime enforcement):** each task MUST use its own unique `working_directory` so every task writes its artifacts into its own folder.

JarvisAgent enforces these conventions:

- **AI tasks (`type: "ai_call"`):** `working_directory` MUST be a per-task folder under the workflow queue root (e.g. `../queue/<workflowId>/<NN_taskName>/`).
- **Non-AI tasks (`type: "internal" | "shell" | "python"`):** `working_directory` MUST be a per-task folder under the workflow base directory (e.g. `<workflowId>/<NN_taskName>/`).

This keeps queue artifacts isolated from workflow-owned artifacts and makes debugging much easier.

### Queue folder policy (JarvisAgent runtime)

**Policy:** _Only AI tasks may write into the queue folder._

This is a **JarvisAgent enforcement rule (engine/runtime)**, not a JCWF spec rule. JCWF merely provides file paths; JarvisAgent decides whether a task is allowed to create/write a given path.

### Scripts folder policy (JarvisAgent runtime)

**Policy:** _Workflow tasks must not create or overwrite files inside the scripts folder._

Scripts may be referenced as **inputs** (e.g. a shell script path), but outputs must be written into the task working directory (or another workflow-owned location).

### Missing source files

**Policy:** missing input sources are a **hard error** for all executors (`ai_call`, `internal`, `shell`, `python`).

If any required source file (e.g. `file_inputs` or non-inline `queue_binding` files) cannot be found/read, the task fails and subsequent tasks do not start.
