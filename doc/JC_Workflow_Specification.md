# JC Workflow File Format™ (extension jcwf)

Copyright (c) 2025 JC Technolabs<br>
License: GPL-3.0

---

> **IMPORTANT — Condensed Guide Sync:**  A condensed version of this specification
> lives at `doc/jcwf_generation_guide.md` and is used as AI context for the JCWF
> generation assistant.  Any change to this specification **MUST** be reflected in the
> condensed guide so the AI always works from an up-to-date reference.

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

## Table of Contents

- [1. Introduction](#1-introduction)
  - [1.1 Requirements Language](#11-requirements-language)
- [2. Terminology](#2-terminology)
- [3. JCWF JSON Specification](#3-jcwf-json-specification)
  - [3.1 Root Object](#31-root-object)
    - [3.1.1 Fields](#311-fields)
    - [3.1.2 Path Resolution](#312-path-resolution)
  - [3.2 Triggers](#32-triggers)
    - [3.2.1 Cron Triggers](#321-cron-triggers)
    - [3.2.2 File-Watch Triggers](#322-file-watch-triggers)
    - [3.2.3 Structure-Based Triggers](#323-structure-based-triggers)
    - [3.2.4 Auto Triggers](#324-auto-triggers)
    - [3.2.5 Manual Triggers](#325-manual-triggers)
    - [3.2.6 Webhook Triggers](#326-webhook-triggers)
    - [3.2.7 S3-Watch Triggers](#327-s3-watch-triggers)
    - [3.2.8 Azure Blob-Watch Triggers](#328-azure-blob-watch-triggers)
    - [3.2.9 GCS-Watch Triggers](#329-gcs-watch-triggers)
  - [3.3 Tasks](#33-tasks)
    - [3.3.1 Task Types](#331-task-types)
    - [3.3.2 Mode: single vs per_item](#332-mode-single-vs-per_item)
    - [3.3.3 Timeouts and Retries](#333-timeouts-and-retries)
    - [3.3.4 Inputs & Outputs (Data Slots)](#334-inputs--outputs-data-slots)
    - [3.3.5 Clean Tasks](#335-clean-tasks)
    - [3.3.6 Environment and Queue Integration](#336-environment-and-queue-integration-stng_-task_-cntx_-prob_-prov_)
    - [3.3.7 AI Provider Configuration](#337-ai-provider-configuration)
  - [3.4 Dependency Semantics and Up-to-Date Checks](#34-dependency-semantics-and-up-to-date-checks)
  - [3.5 Data Flow](#35-data-flow)
  - [3.6 Defaults](#36-defaults)
  - [3.7 Filters *(new in v1.1)*](#37-filters-new-in-v11)
    - [3.7.1 Filter Fields](#371-filter-fields)
    - [3.7.2 Source Kind: csv](#372-source-kind-csv)
    - [3.7.3 Source Kind: text_lines](#373-source-kind-text_lines)
    - [3.7.4 Source Kind: query](#374-source-kind-query)
    - [3.7.5 Source Kind: polarion_query](#375-source-kind-polarion_query)
    - [3.7.6 Filter Manifest](#376-filter-manifest)
    - [3.7.7 Filter Builder (Frontend)](#377-filter-builder-frontend)
  - [3.8 Control Nodes & Controlflow *(new in v1.1)*](#38-control-nodes--controlflow-new-in-v11)
    - [3.8.1 Control Nodes](#381-control-nodes)
    - [3.8.2 Controlflow Edges](#382-controlflow-edges)
    - [3.8.3 Task Field: expose_error_signal](#383-task-field-expose_error_signal)
    - [3.8.4 Runtime Semantics (Rule A)](#384-runtime-semantics-rule-a)
- [4. Execution Model](#4-execution-model)
  - [4.1 High-Level Flow](#41-high-level-flow)
  - [4.2 C++ Side: Core Orchestrator](#42-c-side-core-orchestrator)
  - [4.3 Python Side: Task Executor](#43-python-side-task-executor)
    - [4.3.1 Python logging and subprocess output](#431-python-logging-and-subprocess-output)
  - [4.4 Web UI: Monitoring and Control](#44-web-ui-monitoring-and-control)
- [5. Managing Dependencies](#5-managing-dependencies)
  - [5.1 Readiness Rule](#51-readiness-rule)
  - [5.2 Parallel Execution](#52-parallel-execution)
  - [5.3 Failure Propagation (Rule A)](#53-failure-propagation-rule-a)
- [6. Handling Triggers](#6-handling-triggers)
  - [6.1 Time-Based (Cron)](#61-time-based-cron)
  - [6.2 File-Based](#62-file-based)
  - [6.3 Filter-Driven Expansion](#63-filter-driven-expansion)
  - [6.4 Manual](#64-manual)
- [7. Monitoring and Reporting](#7-monitoring-and-reporting)
- [8. Data Flow](#8-data-flow)
  - [8.1 Task Input Resolution](#81-task-input-resolution)
  - [8.2 Outputs and Context](#82-outputs-and-context)
- [9. JSON Schema (Draft)](#9-json-schema-draft)
- [10. Security Considerations](#10-security-considerations)
- [11. Script Metadata Format](#11-script-metadata-format)
  - [11.1 Marker](#111-marker)
  - [11.2 Metadata Fields](#112-metadata-fields)
  - [11.3 Bash Script Example](#113-bash-script-example)
  - [11.4 Python Script Example](#114-python-script-example)
  - [11.5 Parsing Rules](#115-parsing-rules)
  - [11.6 Registry Table Format](#116-registry-table-format)

---

## 1. Introduction

JarvisAgent orchestrates a variety of automation tasks: document-to-markdown conversion (PDF, DOCX, XLSX, PPTX), AI assistant queries, file watching, and more.  
The JC Workflow File (JCWF) provides a declarative way to describe these tasks and their relationships, so that JarvisAgent can:

- Load workflows at startup or on demand.  
- Run them automatically upon registration, reactively (on triggers), or explicitly (manual trigger from CLI / web UI).  
- Parallelize non-dependent tasks.  
- Track progress and expose status to the web dashboard.

You can think of each workflow as a pipeline: a sequence/DAG () of stages for automation tasks.

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
- **Workflow File Path**: The filesystem path of the loaded `.jcwf` file, **including both the directory and the filename**.
  - Equivalently: `Workflow File Path = Workflow File Directory + <workflow filename>` (after resolution and normalization).
  - The Workflow File Path MAY be provided as either an **absolute** or **relative** path.
  - If provided as a relative path, it MUST be resolved relative to the **JarvisAgent Launch Working Directory**.
  - After resolution, the Workflow File Path MUST be normalized and stored internally as an **absolute path**.
  - Example (canonical form after resolution): `/home/jc/dev/jarvisAgent/workflows/aiZipDemo.jcwf`.
- **Workflow File Directory**: The directory that contains the loaded `.jcwf` file.
- **Workflow Base Directory**: The base directory used for resolving workflow-level relative paths.
  - If the root field `base_directory` is present and starts with `/`, it is treated as an absolute path.
  - If `base_directory` is present and is relative, it MUST be resolved relative to the Workflow File Directory.
  - If `base_directory` is omitted, the Workflow Base Directory defaults to the Workflow File Directory.
  - Relative paths MAY contain `..` segments; JarvisAgent MUST resolve them after lexical normalization.
- **Path Syntax**: This specification uses Unix-style paths with forward slashes (`/`). An absolute path MUST begin with `/`.

- **JarvisAgent Launch Working Directory**: The process current working directory at the time JarvisAgent starts (for example, the project root when launching `./bin/Release/jarvisAgent`). This directory is used to resolve `scripts/` paths for `shell` tasks.
  - JarvisAgent MUST capture this directory at process startup and treat it as immutable for the lifetime of the process.
  - JarvisAgent MUST NOT change the process current working directory (CWD) at any time.
  - The per-task `working_directory` concept in JCWF is a **path-resolution and file-placement rule only**; it MUST NOT be implemented by calling `chdir()` / `std::filesystem::current_path(...)` on the process.
  - Executors that need a task-specific directory MUST use absolute paths and/or per-call process-spawn parameters (for example, `cwd` in Python `subprocess`), without affecting JarvisAgent's own CWD.

- **JCWF Runtime**: The JarvisAgent orchestration layer that loads, validates, and runs JCWF workflows.  
- **Environment**: Optional metadata and variables attached to a task (for example, environment variables for shell tasks, or an assistant environment for AI tasks).  
- **Queue Files**: Optional STNG_, TASK_, CNTX_, PROB_, PROV_ artifacts used by JarvisAgent’s queue-based execution; JCWF can reference these explicitly per task.
  - In addition to file paths, a JCWF MAY embed queue file content inline (see 3.3.6) to make the workflow self-contained.

---

## 3. JCWF JSON Specification

JCWF files MUST be valid JSON documents.  
The file extension SHOULD be `.jcwf`.

### 3.1 Root Object

The root object has the following top-level fields:

```jsonc
{
  "version": "1.1",
  "id": "daily-report",
  "label": "Daily Reporting Workflow",
  "doc": "Generates a daily report from XLS and sends it to an AI assistant for summarization.",
  "base_directory": ".",
  "manual_start": true,
  "triggers": [ /* see 3.2 */ ],
  "filters": [ /* see 3.7 */ ],
  "tasks": { /* see 3.3 */ },
  "dataflow": [ /* see 3.5 */ ],
  "control_nodes": [ /* see 3.8 */ ],
  "controlflow": [ /* see 3.8 */ ],
  "defaults": { /* see 3.6 */ }
}
```

#### 3.1.1 Fields

- `version` (REQUIRED, string)  
  - The JCWF spec version. For this document, `"1.1"` is assumed.  
  - Implementations MUST reject unknown major versions.  
  - Implementations SHOULD warn on unknown minor versions but continue parsing.

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
- `manual_start` (OPTIONAL, boolean, default: `true`)  
  - Controls whether the workflow can be started manually (via UI or CLI).  
  - If `false`, the workflow can only be started by its defined triggers (cron, file_watch, etc.). See 3.2.

- `triggers` (OPTIONAL, array of trigger objects)  
  - Defines when and how the workflow starts. See 3.2.

- `tasks` (REQUIRED, object)  
  - Map from taskId to a task specification. See 3.3.

- `dataflow` (OPTIONAL, array)  
  - Explicit data wiring between task outputs and inputs. See 3.5.

- `filters` (OPTIONAL, array) *(new in v1.1)*  
  - Declares filter nodes for per_item task expansion. See 3.7.

- `control_nodes` (OPTIONAL, array) *(new in v1.1)*  
  - Declares control-flow graph nodes (e.g., branch nodes). See 3.8.

- `controlflow` (OPTIONAL, array) *(new in v1.1)*  
  - Declares control-flow edges between tasks and control nodes. See 3.8.

- `defaults` (OPTIONAL, object)  
  - Default settings for tasks, retries, timeouts, etc. See 3.6.

#### 3.1.2 Path Resolution

JarvisAgent MUST resolve paths deterministically and independent of the process current working directory.

As soon as a path can be resolved under these rules (i.e., once the relevant base/working directory context is known), JarvisAgent MUST convert it to an **absolute** path and store/use it internally in absolute form (after lexical normalization).


JarvisAgent MUST NOT change the process current working directory (CWD) at any time; all path resolution and task execution MUST be implemented without mutating the process CWD.

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
   - `queue_binding` file references (`stng_files`, `task_files`, `cntx_files`, `prob_files`, `prov_files`), including inline `{ "path": "...", "content": "..." }`

**Exceptions**

- For `shell` tasks, `params.command` MUST start with `scripts/` and MUST be resolved relative to the JarvisAgent Launch Working Directory (not the workflow/task directories). The raw path MAY contain `..` segments (for example `scripts/helpers/../run.sh`), but after lexical normalization the resolved path MUST still begin with `scripts/`. If the normalized path escapes the `scripts/` directory tree, JarvisAgent MUST reject the command. This ensures that shell tasks can only invoke scripts that reside inside the `scripts/` folder or any subfolder thereof.

**Directory creation**

- Before executing a task, JarvisAgent MUST ensure the resolved task `working_directory` exists (create directories as needed).
- If JarvisAgent writes a file on behalf of the workflow (for example inline queue files with `content`, or other runtime-generated artifacts), it MUST create the parent directory of the target path if it does not exist.

This enables workflows to be run from any current working directory without writing artifacts into the launch directory by accident.

---

### 3.2 Triggers

A workflow MAY be started by one or more triggers.

**Default behavior:**

- If no `triggers` array is provided in the JCWF file, an implicit `auto` trigger is assumed (the workflow starts automatically upon registration).
- Manual start (via UI or CLI) is enabled by default for all workflows.

**Disabling triggers:**

- To prevent automatic startup: either omit the `auto` trigger from the `triggers` array, or include it with `"enabled": false`.
- To prevent manual start: set `"manual_start": false` at the workflow root level. When `manual_start` is `false`, the workflow can only be started by its defined triggers (cron, file_watch, etc.).
- To disable any individual trigger: set its `"enabled"` field to `false`.

Each trigger has:

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

**Note:** Structure triggers are a placeholder that was never implemented. Per_item task expansion is driven by **filter nodes** (see 3.7), which provide CSV files, text file lines, and Lucene-style queries with complex boolean expressions, range selections, and a manifest-based freshness scheme.

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
- To suppress the default auto-start behavior, provide an explicit `triggers` array that does not include an `auto` trigger, or include one with `"enabled": false`.

#### 3.2.5 Manual Triggers

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

- Manual triggers are exposed in the web UI and/or CLI.
- A manual trigger entry is not required to allow manual start; manual start is enabled by default (see above).
- An explicit manual trigger is useful to control parameters such as `exposed_in_ui`.

#### 3.2.6 Webhook Triggers

```jsonc
{
  "type": "webhook",
  "id": "webhook-trigger",
  "enabled": true,
  "params": {
    "secret": "my-shared-secret"   // optional HMAC-SHA256 shared secret
  }
}
```

- Webhook triggers expose the workflow at `POST /api/webhook/<workflowId>`.
- When a POST request is received, the runtime enqueues a new workflow run.
- The request body MAY contain optional fields:
  - `runId` (string) — caller-chosen run identifier (auto-generated if omitted).
  - `callbackUrl` (string) — URL to POST completion results to when the run finishes.
  - `context` (object) — key-value pairs injected into the workflow run context.
- If `params.secret` is set, the caller MUST include an `X-Webhook-Signature` header
  with the value `sha256=<hex>`, where `<hex>` is the HMAC-SHA256 of the raw request
  body using the shared secret. Requests with missing or invalid signatures are
  rejected with HTTP 401.
- If `params.secret` is empty or omitted, the webhook is **open** (no signature check).
- The raw request body is persisted to `workflows/<workflowId>/webhook/<runId>/request.json`
  for disk-first traceability.
- **Completion callback:** When the run finishes, if `callbackUrl` was provided in the
  request body, JarvisAgent POSTs a JSON payload to that URL containing `workflowId`,
  `runId`, `state` (`succeeded`/`failed`/`cancelled`/`stopped`), `ok` (boolean), `completedAt`
  (ISO-8601), and a `tasks` object with per-task state and optional error messages.
  The callback is fire-and-forget (15 s timeout, failures are logged but do not affect
  the run result).

#### 3.2.7 S3-Watch Triggers

```jsonc
{
  "type": "s3_watch",
  "id": "watch-uploads",
  "enabled": true,
  "params": {
    "connection": "my-s3",         // named CloudConnection (required)
    "bucket": "my-bucket",         // optional, defaults to connection bucket
    "prefix": "inbox/",            // optional key prefix filter
    "poll_interval_seconds": 300   // optional, minimum 60, default 300
  }
}
```

- S3-watch triggers poll an S3 bucket at a configurable interval.
- On each poll interval, the trigger fires and the workflow runs.
- The workflow itself is responsible for detecting new or changed objects (e.g., by comparing against a previous listing stored on disk).
- The `connection` param references a named CloudConnection of type `s3` configured in the Connections tab.

#### 3.2.8 Azure Blob-Watch Triggers

```jsonc
{
  "type": "azure_blob_watch",
  "id": "watch-blobs",
  "enabled": true,
  "params": {
    "connection": "my-azure-blob",   // named CloudConnection (required)
    "container": "uploads",          // optional, defaults to connection container
    "prefix": "inbox/",             // optional blob name prefix filter
    "poll_interval_seconds": 300    // optional, minimum 60, default 300
  }
}
```

- Azure Blob-watch triggers poll an Azure Blob container at a configurable interval.
- On each poll interval, the trigger fires and the workflow runs.
- The `connection` param references a named CloudConnection of type `azure_blob`.

#### 3.2.9 GCS-Watch Triggers

```jsonc
{
  "type": "gcs_watch",
  "id": "watch-objects",
  "enabled": true,
  "params": {
    "connection": "my-gcs",          // named CloudConnection (required)
    "bucket": "my-bucket",           // optional, defaults to connection bucket
    "prefix": "inbox/",             // optional object name prefix filter
    "poll_interval_seconds": 300    // optional, minimum 60, default 300
  }
}
```

- GCS-watch triggers poll a GCS bucket at a configurable interval.
- On each poll interval, the trigger fires and the workflow runs.
- The `connection` param references a named CloudConnection of type `gcs`.

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
  "type": "python | shell | ai_call | internal | sub_workflow | polarion_write | s3 | db_query | onedrive_upload | onedrive_download | snowflake_query | slack_message | slack_read | email_send | email_read | github_issue | jira_issue | redmine_issue | sheets_read | sheets_write | azure_blob_upload | azure_blob_download | gcs_upload | gcs_download",
  "label": "Summarize report with AI",
  "doc": "Sends the prepared text to an AI assistant and stores the answer.",
  "mode": "single | per_item",
  "depends_on": ["load_xls"],
  "working_directory": "output",
  "file_inputs": ["data/report.xlsx"],
  "file_outputs": ["output/report.summary.txt"],
  "materialize": { /* see below */ },
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

**Input materialization (`materialize`):**

When a task consumes upstream outputs, the upstream filenames are determined by the
producing task (e.g., `PROB_new_1.output.txt` from a queue folder) and cannot be
controlled by the consuming task.  Tools like `make` expect specific filenames in the
current directory (e.g., `hello.c`, `Makefile`), and Python scripts may expect specific
input filenames — creating a mismatch.

To bridge this gap declaratively, any task MAY declare a `materialize` object:

```jsonc
{
  "id": "run_make",
  "type": "shell",
  "depends_on": ["generate_hello_c", "generate_makefile"],
  "file_inputs": [
    "../../../queue/myWorkflow/01_generateCode/PROB_code.output.txt",
    "../../../queue/myWorkflow/02_generateMakefile/PROB_makefile.output.txt"
  ],
  "materialize": {
    "{{input[0]}}": "hello.c",
    "{{input[1]}}": "Makefile"
  },
  "params": {
    "command": "scripts/runMake.sh"
  }
}
```

- `materialize` (OPTIONAL, object) — A mapping from source path (typically a
  `{{input[i]}}` or `{{output[i]}}` template) to a target filename.
- Applies to **all task types** (`shell`, `python`, `ai_call`, `internal`).
- Before executing the task, the runtime MUST:
  1. Expand all `{{...}}` templates in the keys using the standard template engine.
  2. For each entry, copy the resolved source file into the task's `working_directory`
     under the specified target filename.
  3. If the source file does not exist, the task MUST fail with a descriptive error.
  4. If the target filename contains path separators, create parent directories as needed.
- The materialized files are **copies**, not symlinks.  The originals are not modified.
- Materialization happens **after** `file_inputs` existence checks and **before**
  task execution.
- The target filenames are relative to the task's `working_directory`.
- This mechanism mirrors the `cntx_files` materialization that the runtime already
  performs for `ai_call` tasks (see §3.3.6.5), extending the same concept to all
  task types as a first-class feature.

With `materialize`, a shell script that wraps `make` becomes trivially simple:

```bash
#!/usr/bin/env bash
set -euo pipefail
make
```

Without `materialize`, the script must handle copying explicitly:

```bash
#!/usr/bin/env bash
set -euo pipefail
cp "$1" hello.c
cp "$2" Makefile
make
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
  JarvisAgent adds both the `scripts/` directory **and** the Launch Working Directory
  to `sys.path`, enabling two import styles:
  - **Bare import:** `"module": "combineJarvisDocumentation"` (resolves via `scripts/` on sys.path)
  - **Dotted import (preferred):** `"module": "scripts.combineJarvisDocumentation"` (resolves via launch CWD on sys.path; requires `scripts/__init__.py`)
- New scripts generated by the AI assistant MUST use the dotted `scripts.` prefix.
- If the module cannot be imported, or the function is missing / not callable,
  the task fails and the error message is recorded and logged.

**Python calling convention**

- The runtime calls `module.function(**kwargs, context=dict)` **programmatically** — scripts are NOT invoked via CLI.
- The `context` dict is **always** passed and contains:
  - `context["_file_input_0"]`, `context["_file_input_1"]`, … — absolute resolved paths from `file_inputs`
  - `context["_task_working_directory"]` — absolute path to the task's working directory
  - `context["_task_id"]` — the task ID string
- Scripts SHOULD declare `def my_function(context=None, **kwargs):` as their signature.
- Before each import, the runtime evicts the module from `sys.modules` to support hot-reload of modified scripts without restarting JarvisAgent.


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
    - `{{input[i]}}` expands to the resolved path of `file_inputs[i]`
    - `{{output[i]}}` expands to the resolved path of `file_outputs[i]`
  - `args` MAY include additional literal flags (for example `"-O3"`) alongside `{{input[i]}}` / `{{output[i]}}`.
  - **Auto-injection (Option B):** If `args` is omitted or contains NO `{{input[`/`{{inputs}}`/`{{output[`/`{{outputs}}` macros, the runtime MUST auto-inject individual `{{input[0]}}`, `{{input[1]}}`, … as the first positional arguments, followed by `{{output[0]}}`, `{{output[1]}}`, … as the remaining arguments. This means `file_inputs` become `$1`, `$2`, … and `file_outputs` become the next positional arguments. JCWF authors MUST NOT place literal file paths in `args` that duplicate `file_inputs`/`file_outputs` — that causes doubled arguments at runtime.

- `ai_call`  
  A high-level “call AI” task that routes via Python or C++ backend.

  ```jsonc
  {
    "id": "ask_ai",
    "type": "ai_call",
    "params": {
      "provider": "openai_gpt4",
      "model": "gpt-4.1-mini",
      "request_params": { "temperature": 0.3, "max_tokens": 4096 },
      "mode": "one_shot",  // or "assistant"
      "prompt_template": "Summarize the following report:\n{{report_text}}"
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

  **AI provider fields** (all OPTIONAL, within `params`):

  - `provider` (string) — Logical name of a registered AI provider (e.g., `"openai_gpt4"`, `"anthropic_claude"`, `"local_ollama"`). See 3.3.7 for the full provider resolution model.
  - `model` (string) — Overrides the provider's default model for this task (e.g., `"gpt-4.1-mini"`, `"claude-sonnet-4-20250514"`).
  - `request_params` (object) — Additional request parameters merged into the API call (e.g., `temperature`, `max_tokens`, `top_p`). Provider-specific; unknown keys are passed through verbatim.

- `internal`  
  Built-in C++ actions, such as updating status or coordinating queue artifacts.

- `sub_workflow`  
  Executes a referenced child JCWF workflow as a nested run. The parent task enters
  `WaitingExternal` state until the child workflow completes. If the child succeeds,
  the parent task succeeds; if the child fails or is cancelled, the parent task fails.

  **Required field:** `workflow_file` (string) — path to the child `.jcwf` file,
  relative to the parent workflow's file directory (or absolute).

  ```jsonc
  {
    "id": "run_cleanup",
    "type": "sub_workflow",
    "label": "Run cleanup sub-workflow",
    "workflow_file": "subworkflows/cleanup.jcwf",
    "depends_on": ["prepare_data"]
  }
  ```

  Sub-workflows are standalone JCWF files with their own `id`, `tasks`, and structure.
  They are loaded from the workflow registry (the `workflows/` directory is scanned
  recursively). Sub-workflows may themselves contain `sub_workflow` tasks (nested
  sub-workflows), subject to a maximum nesting depth of 10.

  Sub-workflows do **not** have triggers — they are always invoked by a parent workflow.
  The child workflow runs as an independent `WorkflowRun` with its own task states.

  **Cancellation:** If the parent run is cancelled, all active child sub-workflow runs
  are cancelled as well.

  **Reuse:** Multiple parent workflows (or multiple tasks within the same workflow) can
  reference the same sub-workflow file.

- `polarion_write`  
  Executes a Polarion ALM write operation (update, create, attachment upload/download, linked items).
  Uses the `ICloudTaskExecutor` base class for automatic connection/credential resolution.

  ```jsonc
  {
    "id": "update_status",
    "type": "polarion_write",
    "params": {
      "connection": "my-polarion",
      "operation": "update",
      "work_item_id": "REQ-003",
      "body": "{\"data\":{\"type\":\"workitems\",\"attributes\":{\"status\":{\"id\":\"approved\"}}}}"
    },
    "working_directory": "my-workflow/update_status"
  }
  ```

  Operations: `update`, `create`, `upload_attachment`, `download_attachment`, `linked_items`.
  See `doc/cloud-integration.md` §8 for full param reference.

- `s3`  
  Executes an S3-compatible object storage operation (upload, download, list, delete).
  Requests are authenticated with AWS Signature V4 (HMAC-SHA256).

  ```jsonc
  {
    "id": "upload_results",
    "type": "s3",
    "params": {
      "connection": "my-s3",
      "operation": "upload",
      "key": "reports/output.csv",
      "file_path": "my-workflow/output.csv"
    },
    "working_directory": "my-workflow/upload"
  }
  ```

  Operations: `upload`, `download`, `list`, `delete`.
  See `doc/cloud-integration.md` §9 for full param reference.

- `db_query`  
  Executes a SQL query against a PostgreSQL database and writes results to CSV or JSON.

  ```jsonc
  {
    "id": "fetch_users",
    "type": "db_query",
    "params": {
      "connection": "my-postgres",
      "query": "SELECT id, name, email FROM users WHERE active = true",
      "format": "csv",
      "output_file": "users.csv"
    },
    "working_directory": "my-workflow/fetch_users"
  }
  ```

  See `doc/cloud-integration.md` §10 for full param reference.

- `azure_blob_upload` / `azure_blob_download`  
  Uploads or downloads a blob to/from Azure Blob Storage via the Azure Blob REST API.
  Authenticated with Azure Storage Shared Key (HMAC-SHA256) or Azure AD OAuth2.

  ```jsonc
  {
    "id": "upload_report",
    "type": "azure_blob_upload",
    "params": {
      "connection": "my-azure-blob",
      "operation": "upload",
      "container": "reports",
      "blob_name": "output/{{run_id}}/report.pdf",
      "local_path": "report.pdf"
    },
    "working_directory": "my-workflow/upload"
  }
  ```

  See `doc/cloud-integration.md` §17 for full param reference.

- `gcs_upload` / `gcs_download`  
  Uploads or downloads an object to/from Google Cloud Storage via the GCS JSON API.
  Authenticated with service account JWT → OAuth2 access token.

  ```jsonc
  {
    "id": "upload_data",
    "type": "gcs_upload",
    "params": {
      "connection": "my-gcs",
      "operation": "upload",
      "bucket": "workflow-data",
      "object_name": "output/{{run_id}}/data.csv",
      "local_path": "data.csv"
    },
    "working_directory": "my-workflow/upload"
  }
  ```

  See `doc/cloud-integration.md` §17 for full param reference.

#### 3.3.2 Mode: `single` vs `per_item`

- `mode: "single"` (default)  
  Task executes once per workflow run.

- `mode: "per_item"` *(updated in v1.1)*  
  Task is expanded per iterator item (for example, each row in a CSV, each line in a text file, each query hit from a Lucene-style index).  
  The expansion is driven by a **filter node** (3.7) referenced via the task's `"filter"` field. For backward compatibility, expansion may also be driven by a structure trigger (3.2.3) or by explicit dataflow list sources.

- `filter` (OPTIONAL, string) *(new in v1.1)*  
  References a filter node by ID (see 3.7). Required when `mode` is `"per_item"` and no structure trigger drives the expansion.

**Fan-out behavior:** When a `per_item` task references a filter, the runtime creates one task instance per matched item. In the workflow editor, an auto-generated **fan-out node** visually represents this parallel expansion between the filter node and the task node. The fan-out node displays item count and progress at runtime.

**Output file naming:** Each per_item instance produces an output file named `<filterID>-<itemNumber>.txt` in the task's `working_directory`.

**Error policy:** If an item instance fails, remaining items continue execution (continue-on-error). The parent task reports partial failure with per-item error details.

**Max items:** Filters support a configurable `max_items` limit (default: 10000). Set to 0 for no limit.

**Upstream output piping** *(v1.1)*: For any task B that declares a `depends_on` on an upstream task A, the runtime automatically injects A's outputs into B's template context. This works for both regular (single-instance) chains and `per_item` chains — in the per_item case, the matching-index child outputs are injected.

Available template variables on a declared dependency:

- `{{A.output_file}}` — absolute path to A's first output file (sorted alphabetically by slot name).
- `{{A.captured_stdout}}` — captured stdout of A (up to 1024 characters).
- `{{A.<slotName>}}` — each named output slot from A individually.
- `{{A.json.<path>}}` — JSON response from upstream A is parsed and flattened into dotted-path entries. Works for two upstream types:
  - **Cloud tasks** that write `response.json` (regular) or `response_<N>.json` (per-item child). Example: `{{create_issue.json.key}}`, `{{create_issue.json.fields.summary}}`, `{{list_issues.json.items[0].id}}`.
  - **Schema-validated `ai_call` tasks** whose validated reply lands in an upstream `.output.json` file (any `.json` path registered in A's output values). Example: `{{ai_classify.json.user_id}}` resolves from the structured `{"user_id":"5"}` reply.

**Per-child response files:** Cloud task executors write their HTTP response to `response.json` for regular tasks and to `response_<N>.json` for per-item children (where `N` is the item index). The JSON-path resolver reads the appropriate file based on the caller's instance — so chained per_item cloud operations get their own matching-index response without races between concurrent children. Structured-output `ai_call` children get the same per-instance treatment via their per-child `<prob>.output.json` files.

This enables chained pipelines where each downstream task consumes structured fields from its corresponding upstream reply:

```
filter(N items) → per_item ai_call(N, output_schema) → per_item cloud_write(N)
                                                           ↑
                                    {{ai_call.json.FIELD}} from matching instance
```

For cloud task executors, template variables in `params` JSON are expanded with JSON-safe escaping (double quotes, backslashes, and newlines are escaped before substitution) so that piped output text does not break JSON structure.

Example — per_item AI analysis piped into per_item DB write-back:

```jsonc
// Task A: AI analyzes each department (per_item)
"ai_analyze": {
  "type": "ai_call",
  "mode": "per_item",
  "filter": "dept-stats",
  "file_outputs": ["analysis.txt"],
  // ... queue_binding with {{dept.department}} etc.
}

// Task B: INSERT each AI analysis back to DB (per_item, same filter)
"write_analysis": {
  "type": "db_query",
  "mode": "per_item",
  "filter": "dept-stats",
  "depends_on": ["ai_analyze"],
  "params": {
    "connection": "local-pg",
    "query": "INSERT INTO analysis (department, text) VALUES ('{{dept.department}}', $j9t${{ai_analyze.captured_stdout}}$j9t$)"
  }
}
```

Example (v1.1 — filter-driven):

```jsonc
{
  "id": "summarize_req",
  "type": "ai_call",
  "mode": "per_item",
  "filter": "reviewed-reqs",
  "params": {
    "provider": "openai",
    "model": "gpt-4.1-mini",
    "mode": "one_shot"
  },
  "inputs": {
    "req_id":   { "type": "string" },
    "req_body": { "type": "string" }
  },
  "outputs": {
    "summary": { "type": "string" }
  }
}
```

Example (v1.0 — structure trigger / template-driven):

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

#### 3.3.3 Timeouts, Heartbeat Watchdog, and Retries

- `timeout_ms` (OPTIONAL, integer)  
  **Inactivity watchdog timeout** per task instance. The runtime starts a watchdog timer when the task begins. Any **heartbeat** resets the timer. If no heartbeat is received within `timeout_ms` milliseconds, the runtime considers the task hung and terminates it.

  **Implicit heartbeats (automatic):**  
  For `shell` tasks, every line of stdout/stderr output counts as a heartbeat. A script that produces regular output will never trigger the inactivity watchdog, even if it runs for hours.

  **Explicit heartbeats (from task code):**  
  Tasks can send a heartbeat via the REST API to reset the watchdog timer:

  ```
  POST /api/task/<task_id>/heartbeat
  ```

  The runtime sets two environment variables in shell child processes:
  - `JARVIS_PORT` — the web server port (e.g. `8080`)
  - `JARVIS_TASK_ID` — the current task instance ID

  **Shell (bash):**
  ```bash
  #!/usr/bin/env bash
  for item in "$@"; do
      process_item "$item"
      # Keep watchdog alive in long loops
      curl -s -X POST "http://localhost:${JARVIS_PORT}/api/task/${JARVIS_TASK_ID}/heartbeat" > /dev/null
  done
  ```

  **Python:**
  ```python
  import os, requests

  port = os.environ.get("JARVIS_PORT", "8080")
  task_id = os.environ.get("JARVIS_TASK_ID", "")

  for item in items:
      result = process(item)
      # Keep watchdog alive
      requests.post(f"http://localhost:{port}/api/task/{task_id}/heartbeat")
  ```

  If `timeout_ms` is omitted or `0`, no watchdog is active and the task may run indefinitely.

  **AI code-generation guidance:** When generating shell or Python code for tasks that contain loops or long-running operations, always include a heartbeat call inside each iteration. This ensures the watchdog timer resets on every loop pass, preventing false timeout kills for legitimately long-running tasks while still catching truly hung processes.

- `retries` (OPTIONAL, object)  
  - `max_attempts` (integer)  
  - `backoff_ms` (integer) linear backoff between retries.  
  Implementations MAY extend this with exponential strategies later.

#### 3.3.4 Inputs & Outputs (Data Slots)

Inputs and outputs are declared to aid validation and UI:

```jsonc
"inputs": {
  "source_path": { "type": "string", "required": true },
  "config": { "type": "object", "required": false },
  "greeting": { "type": "string", "required": true, "default": "Hello World" }
},
"outputs": {
  "markdown_path": { "type": "string" }
}
```

- Each key is a data slot name.  
- Types are advisory but useful for sanity checks and editor tooling.
- `default` (OPTIONAL, string): Fallback value used when neither a dataflow edge nor the run context provides a value for this input. See §8.1 for the full resolution chain.

**Relationship to `file_inputs` / `file_outputs`:**

- `inputs` / `outputs` describe **named data slots** for validation and `dataflow` wiring.  
- `file_inputs` / `file_outputs` describe **file dependencies** for freshness checks and (for shell tasks) positional argument templating via `{{input[i]}}` / `{{output[i]}}`.
- A shell task MAY omit `inputs` entirely and still run, as long as its `params` are resolvable (for example, only using `{{input[i]}}` / `{{output[i]}}`).
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

#### 3.3.7 AI Provider Configuration

`ai_call` tasks target a named **provider** from JarvisAgent's provider registry. The provider registry maps logical names to connection details (endpoint URL, API key, default model, API type). Providers are configured outside the JCWF file — either via JarvisAgent's encrypted key store (`keys.json.enc`) or the Settings UI. See `engine/keys.md` for the key management design.

**Resolution order** for `ai_call` tasks:

1. **Task-level** `params.provider` — if present, look up this provider in the registry.
2. **Workflow-level** `defaults.ai.provider` — if the task does not specify a provider, use the workflow default (see 3.6).
3. **System-level** default provider — if neither task nor workflow specifies a provider, use the system default from the provider registry.
4. If no provider can be resolved, the task MUST fail with a clear error before dispatch.

**Provider prerequisite check:**

Before a workflow run is enqueued (whether by trigger activation, manual start, or API call), the runtime MUST verify that every `ai_call` task in the workflow has its required provider available in the provider registry. The check uses the same resolution order above (task-level → workflow-level → system default). If **any** required provider is missing, the workflow run MUST be blocked with a log warning, and the trigger or start request is silently dropped. This prevents workflows from starting, partially executing non-AI tasks, and then failing on the first `ai_call`.

The dashboard SHOULD indicate which workflows are blocked due to missing providers (e.g., a hazard icon and disabled Run button) and display a banner explaining how to configure providers.

**Model override:**

- If `params.model` is specified on the task, it overrides the provider's `default_model`.
- If `defaults.ai.model` is specified at the workflow level and the task does not specify `params.model`, the workflow default is used.
- Otherwise, the provider's `default_model` is used.

**Request parameters merge:**

- `params.request_params` on the task is merged into the API request body.
- If `defaults.ai.request_params` exists, the task-level object is merged on top (task wins on key conflicts).
- Common keys: `temperature`, `max_tokens`, `top_p`, `frequency_penalty`, `presence_penalty`.
- Unknown keys are passed through to the provider API verbatim.

**Provider registry entry** (conceptual — not part of the JCWF file):

| Field           | Description                                                    |
|-----------------|----------------------------------------------------------------|
| `display_name`  | Human-readable label (e.g., "OpenAI GPT-4").                  |
| `endpoint`      | Full URL to the chat completions endpoint.                     |
| `api_key`       | API key (empty for local/keyless endpoints like Ollama).       |
| `default_model` | Default model string sent in the request body.                 |
| `api_type`      | `"API1"` (OpenAI Chat Completions) · `"API2"` (OpenAI Responses) · `"API3"` (Gemini native) · `"API4"` (Anthropic Messages) · `"API5"` (AWS Bedrock: SigV4-signed; per-family body for anthropic.* / meta.llama* / amazon.titan-* / amazon.nova-*) · `"API6"` (Azure OpenAI: API1 body + `api-key:` header + per-deployment URL) · `"Test"` (fixture-driven, integration tests). |

**Example: specialized AI tasks in a single workflow**

```jsonc
{
  "tasks": {
    "draft_report": {
      "id": "draft_report",
      "type": "ai_call",
      "label": "Draft report with Claude",
      "params": {
        "provider": "anthropic_claude",
        "model": "claude-sonnet-4-20250514",
        "request_params": { "temperature": 0.4, "max_tokens": 8192 },
        "mode": "one_shot",
        "prompt_template": "Write a detailed report on:\n{{topic}}"
      }
    },
    "summarize_report": {
      "id": "summarize_report",
      "type": "ai_call",
      "label": "Summarize with GPT-4",
      "depends_on": ["draft_report"],
      "params": {
        "provider": "openai_gpt4",
        "request_params": { "temperature": 0.1 },
        "mode": "one_shot",
        "prompt_template": "Summarize in 5 bullets:\n{{report}}"
      }
    },
    "translate": {
      "id": "translate",
      "type": "ai_call",
      "label": "Translate with local Ollama",
      "depends_on": ["summarize_report"],
      "params": {
        "provider": "local_ollama",
        "model": "llama3",
        "mode": "one_shot",
        "prompt_template": "Translate to German:\n{{summary}}"
      }
    }
  }
}
```

This example uses three different AI providers in a single workflow — each task targeting the best model for its role, analogous to specialized engineers in an engineering department.

---

#### 3.3.6 Environment and Queue Integration (STNG_, TASK_, CNTX_, PROB_, PROV_)

An `ai_call` task maps to a **queue folder** — the task's `working_directory`.
For each PROB file the runtime builds an `AiInvocation` envelope (interface,
model, settings, concatenated STNG/TASK/CNTX/PROB content) and submits it to
`AiRequestPool::Submit`.  The runtime also writes the STNG/TASK/CNTX/PROB/PROV
files to the queue folder on disk — disk-first philosophy, every call is
reconstructible and replayable — but the envelope is authoritative for dispatch.

##### 3.3.6.1 Queue Folder

Each `ai_call` task declares a `working_directory` that points to a queue
folder.  The runtime:

1. Writes STNG/TASK/CNTX/PROB/PROV files to the folder (disk-first, see 3.3.6.2).
2. Materializes path-reference CNTX files (e.g. upstream `PROB_*.output.txt`)
   as `CNTX_*` files in the folder.  Office-format CNTX sources
   (`.pdf`/`.docx`/`.xlsx`/`.pptx`/`.odt`) are auto-converted to Markdown via
   `markitdown` at this step; failures abort the task with a clear error rather
   than dispatching a binary blob.
3. Builds one envelope per PROB file, with the user message assembled as:
   `[STNG] + [TASK] + [CNTX] + [PROB]`.
4. If the combined message exceeds the target interface's `max_context_tokens`
   budget and no `output_schema` is declared, **chunks the CNTX section** at
   markdown heading boundaries (§3.3.6.9), fans out N envelopes in parallel,
   then submits a **reduce envelope** that consolidates the partial answers into
   one unified reply.  Schema-declared tasks skip chunking (schema enforcement
   needs a whole-object reply).
5. Submits each envelope via `AiRequestPool::Submit`, writes the reply to
   `<stem>.output.<ext>` (e.g. `PROB_NVDA.txt` → `PROB_NVDA.output.txt`;
   structured replies land as `<stem>.output.json`), and writes a
   `<stem>.transcript.json` alongside for replay.
6. Tracks freshness — a requirement is re-sent only when its own content or
   the environment changes (timestamp comparison with the existing output file).

##### 3.3.6.2 File Categories

| Prefix / Pattern    | Category       | Role                                                                                              |
|----------------------|----------------|---------------------------------------------------------------------------------------------------|
| `STNG_*`             | Settings       | **Environment** — sets tone, style, and constraints for the AI (e.g., "be succinct").             |
| `TASK_*`             | Task           | **Environment** — work instructions describing *what* the AI should produce.                      |
| `CNTX_*`             | Context        | **Environment** — background information the AI needs to reference.                               |
| `PROB_*` / any text  | Requirement    | **Query trigger** — each requirement file produces one AI call. Content is appended to the environment and sent as the full prompt. |
| `PROV_*`             | Provider       | **Configuration** — endpoint URL, model, API type. Never sent to AI. Never contains credentials.  |
| `*.output.*`         | Ignored        | **Output** — AI response files. Ignored by the categorizer to avoid feedback loops.               |
| Binary / oversized   | Ignored        | Silently skipped.                                                                                 |

**Key principle:** STNG + CNTX + TASK files form the *shared environment*.
Every other eligible text file in the folder is a *requirement* and triggers its
own independent AI query.  The full prompt for each query is:

```
[all STNG content] + [all CNTX content] + [all TASK content] + [requirement file content]
```

##### 3.3.6.3 Output Naming Convention

The runtime derives the output filename from the requirement filename by
inserting `.output` before the extension:

| Requirement File          | Output File                  |
|---------------------------|------------------------------|
| `PROB_NVDA.txt`           | `PROB_NVDA.output.txt`       |
| `PROB_summarize.txt`      | `PROB_summarize.output.txt`  |
| `my_question.txt`         | `my_question.output.txt`     |

For `ai_call` tasks, the **PROB files are the requirement files**.  The
runtime builds one envelope per PROB and writes the reply to the corresponding
`.output.<ext>` file in the same folder.

**Exposing the AI response — two valid patterns.** The AI response is written to `<PROB_stem>.output.<ext>` in the task's queue folder. Downstream consumers can reach it in two ways:

*Pattern A — zero-copy reference (preferred for downstream JCWF tasks).* Declare an `outputs` slot and skip `file_outputs`:

```jsonc
"summarize": {
  "type": "ai_call",
  "working_directory": "../../queue/myWorkflow/01_summarize",
  "outputs": { "summary": { "type": "string" } },
  "queue_binding": { /* STNG + CNTX + TASK + PROB */ }
}
```

The runtime maps the declared slot to the `PROB_*.output.*` file. Downstream tasks reference it via `{{summarize.output_file}}` or `{{summarize.summary}}`, both resolving to the absolute path of the AI response. No duplication, no extra AI call.

*Pattern B — delivery to a specific path (preferred for terminal artefacts).* Declare `file_outputs` with paths that resolve OUTSIDE the task's own queue folder. The runtime copies `PROB_*.output.*` to each declared path once the AI response lands. Typical uses:

- **Portable delivery within the project tree.** A JCWF writes a report to `workflows/<project>/reports/summary.md`, or an adhoc run emits generated source to `workflows/build/main.cpp`. Works identically across Studio, Engine, Docker, and per-user installs.
- **External-project agent workflows** (Studio and Engine-without-Docker only). An agent operating on a codebase outside the j9t tree — say a local Polarion mockup at `~/dev/polarionMockup/src/` — writes AI-generated patches directly into that project. Skipping a copy-in-copy-out dance is meaningfully faster than round-tripping through `workflows/`.

```jsonc
"summarize": {
  "type": "ai_call",
  "working_directory": "../../queue/myWorkflow/01_summarize",
  "file_outputs": ["workflows/build/main.cpp"],
  "queue_binding": { /* STNG + CNTX + TASK + PROB */ }
}
```

**Portability note.** Paths above the launch CWD (e.g. `/tmp/...`, `~/...`, `../../outside`) work fine locally but fail in Engine-in-Docker where the filesystem above the mount is read-only. The runtime does not hard-reject them — that's an intentional trade — but the validator raises an informational finding so authors who care about cross-edition portability see the flag.

**What `file_outputs` on `ai_call` MUST NOT do.** A `file_outputs` path that resolves INSIDE the task's own `working_directory` (the queue folder) AND whose filename reuses a queue prefix — i.e. the filename matches `PROB_*` or does not match any recognized prefix (`STNG_*` / `CNTX_*` / `TASK_*` / `PROV_*` / `*.output.*`) — collides with the task's own queue binding on disk and confuses replay tooling. The runtime MUST warn about these cases; implementations MAY reject them.

If you need to chain two AI calls, add a second task with its own queue folder.

Patterns A and B can be combined: declare both an `outputs` slot (for downstream JCWF references) and `file_outputs` (for terminal artefact delivery). The slot always maps to the original `PROB_*.output.*`; `file_outputs` copies go to the declared external paths.

##### 3.3.6.4 `queue_binding` — Declaring Queue Files in JCWF

The `queue_binding` object on a task definition tells the workflow executor
which files to create in the queue folder before the runtime assembles the
envelope.

```jsonc
"queue_binding": {
  "stng_files": [ ... ],
  "task_files": [ ... ],
  "cntx_files": [ ... ],
  "prob_files": [ ... ],
  "prov_files": [ ... ]
}
```

- `stng_files` (OPTIONAL, array) — Settings files. Written once; become part of the environment.
- `task_files` (OPTIONAL, array) — Task instruction files. Written once; become part of the environment.
- `cntx_files` (OPTIONAL, array) — Context files. Written once (or materialized from upstream task outputs); become part of the environment. Entries MAY contain **glob patterns** (`*`, `?`) to dynamically match files at execution time (see 3.3.6.7).
- `prob_files` (REQUIRED for `ai_call`) — Problem/requirement files. **Each PROB file triggers one AI query.** For `per_item` tasks, each item instance writes its own PROB file with a unique name.
- `prov_files` (OPTIONAL, array) — Provider sidecar. Written by the executor when the task specifies `provider` / `model`. Contains endpoint URL, model, and API type — but **NEVER** credentials or API keys. The envelope carries the same information; PROV is kept on disk for replay tooling only.

Each entry MAY be either:

- A **string** (path to an existing file), or
- An **inline object** with file content:

```jsonc
{ "path": "TASK_summarize.txt", "content": "Summarize the report in 5 bullets." }
```

If `content` is present, the runtime MUST write (or overwrite) the file at
`path` before dispatching.  This inline form is RECOMMENDED when a workflow
should be self-contained and runnable without additional files.

##### 3.3.6.5 Path Resolution for `queue_binding`

- Relative paths in `queue_binding` entries MUST be resolved relative to the task `working_directory`.
- When writing inline `content`, the runtime MUST create parent directories if they do not exist.
- For `cntx_files` path-only entries referencing upstream task outputs (e.g., `"../01_taskA/PROB_foo.output.txt"`), the runtime MUST **materialize** a copy as `CNTX_<filename>` in the current task's working directory and include its content in the envelope's user message.
- If a `cntx_files` path contains **glob characters** (`*` or `?`), the runtime MUST expand the pattern against the filesystem before materialization. See 3.3.6.7 for details.

##### 3.3.6.6 Per-Item Template Substitution *(v1.1)*

For `mode: "per_item"` tasks driven by a filter, the runtime MUST perform
`{{binding.field}}` template substitution on **both** the inline `content`
**and** the `path` of all `queue_binding` file types (`stng_files`,
`task_files`, `cntx_files`, `prob_files`).

This uses the same `{{key}}` syntax as `prompt_template` (see 3.3.1).

Available template variables correspond to the filter item's bound values,
prefixed by the filter's `binding` name:

- `{{binding.fieldName}}` — value of `fieldName` from the filter item (e.g., `{{pos.Symbol}}`).
- `{{binding._index}}` — 0-based item index within the result set.
- `{{binding._key}}` — stable identity key (e.g., first column value for CSV sources).
- `{{binding._source_path}}` — filesystem path of the filter source file.

**Example — per-symbol AI call with a CSV filter (`"binding": "pos"`):**

```jsonc
"prob_files": [
  {
    "path": "PROB_{{pos.Symbol}}.txt",
    "content": "Symbol: {{pos.Symbol}}\nName: {{pos.Name}}\nAllocation: {{pos.Percentage}}\n"
  }
]
```

For a portfolio with 60 positions, this produces 60 uniquely named PROB files
(e.g., `PROB_NVDA.txt`, `PROB_AAPL.txt`, …).  The runtime builds one envelope
per PROB, dispatches them in parallel via `AiRequestPool::Submit`, and writes
the corresponding output files (`PROB_NVDA.output.txt`, `PROB_AAPL.output.txt`, …).

**Note:** `{{key}}` substitution applies to `queue_binding` inline `content`
and `path` strings.  For `file_inputs` and `file_outputs` path templates, use
`${inputs.key}` syntax (see 3.4).

##### 3.3.6.7 Consuming Upstream Outputs as CNTX Files

A downstream task can reference per-item output files from an upstream task's
queue folder as `cntx_files`.  The runtime materializes them as `CNTX_*` files
in the downstream task's working directory so they become part of its
environment.

###### Glob Patterns *(v1.1)*

When the number of upstream outputs is not known at authoring time (e.g.,
CSV-driven or Polarion-query-driven per-item tasks), `cntx_files` entries MAY
use **glob patterns** (`*` matches any sequence of characters, `?` matches a
single character):

```jsonc
"cntx_files": [
  "../01_lookupDividend/PROB_*.output.txt"
]
```

The runtime MUST:

1. Detect glob characters (`*` or `?`) in the path.
2. Resolve the parent directory relative to the task `working_directory`.
3. Match the filename pattern against all regular files in that directory.
4. Sort matches lexicographically for deterministic ordering.
5. Expand each match into an individual `cntx_files` entry.
6. Log the pattern and match count (for diagnostics).
7. Warn (but not fail) if the pattern matches zero files.

Each matched file is then materialized as `CNTX_<basename>` in the
downstream task's working directory.  The `.output` suffix is stripped from the
stem to prevent the `FileCategorizer` from treating the materialized file as an
output file (e.g., `PROB_NVDA.output.txt` → `CNTX_PROB_NVDA.txt`).

###### Explicit File References

Individual files MAY still be listed explicitly:

```jsonc
"cntx_files": [
  "../01_lookupDividend/PROB_NVDA.output.txt",
  "../01_lookupDividend/PROB_AAPL.output.txt"
]
```

Glob and explicit entries MAY be mixed in the same array.

##### 3.3.6.8 `environment` (Legacy / Optional)

Tasks MAY also declare an `environment` object for shell or Python tasks:

```jsonc
"environment": {
  "name": "assistant_env_daily_reports",
  "variables": { "PROJECT": "DailyReports", "LOCALE": "en-US" },
  "assistant_id": "daily-report-assistant"
}
```

- `name` (OPTIONAL, string): Logical name for this environment.
- `variables` (OPTIONAL, object): Key-value environment variables for shell or Python tasks.
- `assistant_id` (OPTIONAL, string): For `ai_call` tasks with `mode: "assistant"`, references a preconfigured assistant that keeps its own long-lived context.

##### 3.3.6.9 Structured Output (`output_schema` / `output_retries`)

`ai_call` tasks MAY declare an `output_schema` — a JSON Schema (Draft 2020-12
subset) that the AI reply is validated against.  When validation fails, the
runtime re-dispatches the same envelope up to `output_retries` times, appending
the validator's error summary to the conversation with an instruction to
produce a schema-conforming response.

**Fields on an `ai_call` task:**

- `output_schema` (OPTIONAL, object) — Raw JSON Schema.  Supported keywords:
  `type`, `properties`, `required`, `additionalProperties`, `items`, `enum`,
  `minimum`, `maximum`, `minLength`, `maxLength`, `pattern`, `oneOf`, `anyOf`,
  `$ref`, `$defs`.  Unsupported keywords are rejected at JCWF load time (not at
  reply time) so authors see errors before a run starts.
- `output_retries` (OPTIONAL, integer, default 3) — Maximum schema-validation
  attempts (including the first).  The envelope's HTTP retry budget is
  independent.

**Effect on disk layout:**

- When `output_schema` is set, a schema-valid reply is written as
  `<stem>.output.json` (the extracted, validated JSON payload).
- Without `output_schema`, the raw text lands at `<stem>.output.txt`.
- Never both.

**Dispatch-mode selection per provider:**

- OpenAI API1 / API2 / Azure OpenAI API6: `response_format: { type: "json_schema", ... }` (native).
- Gemini API3: `responseSchema` (native).
- Anthropic API4: forced-tool shim — the schema is wrapped as a tool
  parameter, `tool_choice: {"type":"tool","name":"output"}`.  The reply's
  tool-use content block carries the structured payload.
- AWS Bedrock API5: per-family — `anthropic.*` follows API4's forced-tool shim,
  `meta.llama*` and `amazon.titan-*`/`amazon.nova-*` use prompt-level instructions
  (no native schema enforcement; relies on retry-on-validation-failure).

**Chunking interaction:** chunking and `output_schema` are **mutually
exclusive**.  A chunked response can't satisfy a whole-object schema, so if an
over-budget prompt is declared with a schema, chunking is skipped and the
provider may reject the request (explicit warning in the log).  For oversized
structured output, restructure the workflow — e.g. summarise first, then
produce structured output on the summary.

**Workflow-validator rule:** a dataflow reference of the form
`{{tasks.X.output.field}}` is only valid when task X declares an
`output_schema` — the validator rejects the reference otherwise, so authors
can't accidentally consume structured fields from free-text tasks.

**Example — classify step emits one enum value, downstream task consumes it:**

```jsonc
{
  "tasks": {
    "classify": {
      "id": "classify",
      "type": "ai_call",
      "output_schema": {
        "type": "object",
        "required": ["category"],
        "properties": {
          "category": { "type": "string", "enum": ["invoice", "receipt", "other"] }
        },
        "additionalProperties": false
      },
      "output_retries": 3,
      "outputs": { "category": { "type": "string" } },
      "working_directory": "../../queue/classifier/01_classify",
      "queue_binding": {
        "stng_files": [{ "path": "STNG.txt", "content": "Respond with JSON only." }],
        "task_files": [{ "path": "TASK.txt", "content": "Classify the document." }],
        "cntx_files": ["../../../input/doc.pdf"],
        "prob_files":  [{ "path": "PROB.txt", "content": "Emit the classification." }]
      }
    },
    "route": {
      "id": "route",
      "type": "python",
      "depends_on": ["classify"],
      "params": { "module": "workflows.router", "function": "route_by_category" },
      "inputs": {
        "category": { "type": "string", "required": true }
      }
    }
  },
  "dataflow": [
    { "from_task": "classify", "from_output": "category",
      "to_task": "route",    "to_input":  "category" }
  ]
}
```

`classify` writes `PROB.output.json` containing `{"category":"invoice"}` (or
fails after 3 retries with `AiError{kind=SchemaValidation}`).  The `route`
task receives the validated value through the dataflow slot.

##### 3.3.6.10 Chunking and Reduce Pass

When the combined user message (STNG + CNTX + TASK + PROB) exceeds the target
interface's `max_context_tokens` budget, the runtime splits the **CNTX portion
only** into N chunks at markdown section boundaries (preferring whole `#` /
`##` / `###` sections, subdividing a section only when it alone overruns
budget).

- Each chunk envelope carries `[STNG + TASK] + <slice i of CNTX> + [PROB]` —
  i.e. the full instructions and original question, with just the context
  portion sliced down.  Chunks dispatch in parallel via the shared HTTP/2
  connection.
- Per-chunk intermediates land as `<stem>.output.chunk<i>-of-<N>.txt` for
  replay.
- Once all N replies arrive, a **reduce envelope** is submitted that carries
  the N partial answers plus the original PROB, asking the model to produce a
  single unified reply.  The reduced reply becomes `<stem>.output.txt`.
- If the reduce envelope can't be submitted (interface error, missing key),
  the fallback is plain concatenation of the N partial answers with HTML
  comment separators.

**Per-interface budget** comes from `max_context_tokens` in `config.json` for
each `ApiInterface`.  When unset, a curated model-name fallback table resolves
a default (GPT-4-family 128K, Claude 200K, Gemini 1.5/2 1M, Llama/Qwen/DeepSeek/Phi
128K, Mistral/Mixtral 32K, unknown model 50K).

**What chunking does NOT do:** it does not summarize, filter, or reshape the
content — it's a context-window workaround, not a content transformation.  If
a JCWF requires precise aggregation (e.g. "rank all records by X"), a single
envelope on a large-window model (Opus, Gemini 1.5) will produce a better
result than chunking on a small-window model.  The `max_context_tokens`
fallback table picks the right behaviour automatically.

##### 3.3.6.11 Complete AI Call Example

```jsonc
{
  "tasks": {
    "lookupDividend": {
      "type": "ai_call",
      "mode": "per_item",
      "filter": "positions",
      "working_directory": "../queue/portfolio/01_lookupDividend",
      "queue_binding": {
        "stng_files": [{ "path": "STNG_style.txt", "content": "Be concise.\n" }],
        "task_files": [{ "path": "TASK_lookup.txt", "content": "Look up the dividend yield.\n" }],
        "prob_files": [{
          "path": "PROB_{{pos.Symbol}}.txt",
          "content": "Symbol: {{pos.Symbol}}\nName: {{pos.Name}}\nAllocation: {{pos.Percentage}}\n"
        }]
      }
    },
    "portfolioSummary": {
      "type": "ai_call",
      "mode": "single",
      "depends_on": ["lookupDividend"],
      "working_directory": "../queue/portfolio/02_portfolioSummary",
      "queue_binding": {
        "stng_files": [{ "path": "STNG_style.txt", "content": "Be professional.\n" }],
        "task_files": [{ "path": "TASK_summary.txt", "content": "Summarize dividend data.\n" }],
        "prob_files": [{ "path": "PROB_summarize.txt", "content": "Produce the portfolio summary.\n" }],
        "cntx_files": [
          "../01_lookupDividend/PROB_*.output.txt"
        ]
      }
    }
  }
}
```

**Folder state after task 1 completes (01_lookupDividend):**

```
STNG_style.txt            ← environment (settings)
TASK_lookup.txt           ← environment (task)
PROV_provider.json        ← provider config (not sent to AI)
PROB_NVDA.txt             ← requirement → triggers AI query
PROB_NVDA.output.txt      ← AI response (output)
PROB_AAPL.txt             ← requirement → triggers AI query
PROB_AAPL.output.txt      ← AI response (output)
… (60 PROB / output pairs)
```

**Folder state after task 2 completes (02_portfolioSummary):**

```
STNG_style.txt            ← environment (settings)
TASK_summary.txt          ← environment (task)
CNTX_PROB_NVDA.output.txt ← materialized from task 1 output (context)
CNTX_PROB_AAPL.output.txt ← materialized from task 1 output (context)
… (60 CNTX files)
PROB_summarize.txt        ← requirement → triggers AI query
PROB_summarize.output.txt ← AI response (output)
```

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

- **Inline queue content and freshness**
  - If a task embeds queue files inline via `{ "path": "...", "content": "..." }` (see 3.3.6),
    the runtime MUST treat the **workflow `.jcwf` file itself** as an implicit freshness input.
  - Inline content MUST NOT be treated as a separate freshness input file; instead, edits are tracked via the `.jcwf` file modification time.

   - If a task is skipped as “up to date”, the runtime SHOULD treat it as successful and its `file_outputs` MUST be considered available to downstream tasks (for both readiness and freshness comparisons).

3. Per-item mode  
   - For `mode: "per_item"` tasks, the same freshness rules apply per item.  
   - `file_inputs` and `file_outputs` may use templates (for example, `"output/${inputs.section_title}.summary.txt"`). The runtime evaluates templates per item before checking timestamps.  
   - *(v1.1)* For filter-driven per_item tasks, the runtime writes a **filter manifest** (`<workflowBaseDir>/<filterID>/<filterID>.manifest.json`) recording the evaluated item list and a SHA-256 hash of the filter expression (`query_hash`). Per-item freshness rules:  
     - **CSV / text_lines sources:** input timestamp = source file mtime. Output timestamp = mtime of `<filterID>-<k>.txt`.  
     - **Query sources:** input timestamp = `doc_path` mtime if available; otherwise the item is always considered stale.  
     - If `query_hash` changes (filter expression was edited), **all items** are considered stale (full re-run).  
     - **New items** (not in previous manifest) are always stale.  
     - **Removed items** (in manifest but not in new results): output files are orphaned; runtime logs a warning but does not delete them.  
   - The file-driven philosophy applies throughout: CSV/text sources are consumed directly via their source file; query/database sources produce a small text-file snippet per item (`<filterID>-<k>.txt`) that downstream tasks consume.

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
    "provider": "openai_gpt4",
    "model": "gpt-4.1-mini",
    "request_params": { "temperature": 0.7 }
  }
}
```

Task-specific fields override defaults at the same key path.

**`defaults` fields:**

- `timeout_ms` (integer) — Default timeout for all tasks. Overridden by task-level `timeout_ms`.
- `retries` (object) — Default retry policy. Overridden by task-level `retries`.
  - `max_attempts` (integer)
  - `backoff_ms` (integer)
- `ai` (object) — Default AI provider settings for all `ai_call` tasks. Overridden by task-level `params.provider`, `params.model`, and `params.request_params` respectively. See 3.3.7 for the full resolution order.
  - `provider` (string) — Logical provider name from the provider registry.
  - `model` (string) — Default model override.
  - `request_params` (object) — Default request parameters (merged with task-level; task wins on conflicts).

---

### 3.7 Filters *(new in v1.1)*

Filters are declared at the workflow root level under the `"filters"` key. Each filter defines a data source and selection criteria that produce a list of items for `per_item` task expansion.

A filter node has no executor and no `depends_on` — it is purely declarative. In the workflow editor, filters appear as a distinct node type (different visual style from task nodes) with an output port that connects to a fan-out node.

```jsonc
"filters": [
  {
    "id": "reviewed-reqs",
    "source": {
      "kind": "query",
      "index_path": "data/index",
      "query": "(type:requirement OR type:defect) AND tags:JC AND created:[20230101 TO 20231231]",
      "fields": ["id", "title", "body", "status"]
    },
    "binding": "hit",
    "max_items": 10000
  }
]
```

#### 3.7.1 Filter Fields

- `id` (REQUIRED, string) — Unique filter identifier within the workflow.
- `source` (REQUIRED, object) — Defines the data source and selection criteria.
  - `kind` (REQUIRED, string) — One of `"csv"`, `"text_lines"`, or `"query"`.
- `binding` (REQUIRED, string) — Prefix for injected input variables (e.g. `"hit"` → `hit.id`, `hit.title`).
- `max_items` (OPTIONAL, integer, default: 10000) — Maximum number of items. Set to 0 for no limit.

#### 3.7.2 Source Kind: `csv`

Iterates over rows of a CSV file, with optional row range selection.

```jsonc
{
  "kind": "csv",
  "path": "data/items.csv",
  "delimiter": ",",
  "has_header": true,
  "range": "10-20"
}
```

- `path` (REQUIRED) — CSV file path, resolved relative to the Workflow Base Directory.
- `delimiter` (OPTIONAL, default `","`) — Field delimiter.
- `has_header` (OPTIONAL, default `true`) — If true, the first row is treated as column names.
- `range` (OPTIONAL) — Row range (1-based, inclusive). Examples: `"10-20"` (rows 10–20), `"5-"` (row 5 to end), `"-50"` (first 50 rows). Omit for all rows.

**Item shape** (injected under the binding prefix):

| Key               | Value                                   |
|-------------------|-----------------------------------------|
| `<binding>.index`      | 0-based index within the selected range |
| `<binding>.row_number` | 1-based row number in the source file   |
| `<binding>.<col_name>` | Cell value (header names, if present)   |
| `<binding>.col_0` …    | Cell value by positional index          |
| `<binding>.line`       | Full CSV line as raw string             |

**Freshness:** file modification time of `path`.

#### 3.7.3 Source Kind: `text_lines`

Iterates over lines of a text file.

```jsonc
{
  "kind": "text_lines",
  "path": "data/requirements.txt",
  "skip_empty": true
}
```

- `path` (REQUIRED) — Text file path, resolved relative to the Workflow Base Directory.
- `skip_empty` (OPTIONAL, default `true`) — If true, empty lines are skipped.

**Item shape:**

| Key                | Value                       |
|--------------------|-----------------------------|
| `<binding>.index`  | 0-based line number         |
| `<binding>.text`   | Full line content (trimmed) |

**Freshness:** file modification time of `path`.

#### 3.7.4 Source Kind: `query`

Iterates over hits from a Lucene-style query against an index.

```jsonc
{
  "kind": "query",
  "index_path": "data/index",
  "query": "(type:requirement OR type:defect) AND tags:JC AND created:[20230101 TO 20231231]",
  "fields": ["id", "title", "body", "status"]
}
```

- `index_path` (REQUIRED) — Path to the query index, resolved relative to the Workflow Base Directory.
- `query` (REQUIRED) — Lucene-style query expression.
- `fields` (OPTIONAL) — List of stored fields to extract per hit.

**Query language features:**

| Feature           | Syntax                         | Example                                         |
|-------------------|--------------------------------|--------------------------------------------------|
| Field match       | `field:value`                  | `tags:JC`                                        |
| Boolean AND       | `expr AND expr`                | `tags:JC AND status:reviewed`                    |
| Boolean OR        | `expr OR expr`                 | `type:requirement OR type:defect`                |
| Grouping          | `(expr)`                       | `(type:requirement OR type:defect) AND tags:JC`  |
| Range (inclusive)  | `field:[lo TO hi]`             | `created:[20230101 TO 20231231]`                 |
| Range (exclusive)  | `field:{lo TO hi}`             | `priority:{1 TO 5}`                              |
| Wildcard          | `field:val*`                   | `title:sys*`                                     |
| Negation          | `NOT expr` or `-field:value`   | `NOT status:archived`                            |

**Item shape:**

| Key                  | Value                                    |
|----------------------|------------------------------------------|
| `<binding>.index`    | 0-based hit number                       |
| `<binding>.<field>`  | Stored field value (e.g. `hit.id`)       |
| `<binding>.doc_path` | Filesystem path of the source document   |

**Freshness:** `doc_path` mtime if available; otherwise the item is always considered stale.

**Implementation note:** The query engine uses a Python bridge (Whoosh / pylucene) via `PythonEngine` for index evaluation.

#### 3.7.5 Source Kind: `polarion_query`

Iterates over work items from a Polarion ALM REST API query. The filter evaluation issues paginated HTTP requests via libcurl in C++.

```jsonc
{
  "kind": "polarion_query",
  "base_url": "https://polarion.example.com/polarion",
  "project_id": "MYPROJECT",
  "query": "tags:JC AND system:Propulsion AND majorSection:10",
  "fields": ["id", "title", "description", "status", "tags"],
  "key_name": "polarion_prod",
  "page_size": 100
}
```

- `base_url` (REQUIRED) — Polarion server base URL (scheme + host + context path).
- `project_id` (REQUIRED) — Polarion project identifier.
- `query` (REQUIRED) — Polarion work item query expression (Lucene-style syntax as supported by the Polarion REST API).
- `fields` (OPTIONAL) — List of work item fields to retrieve. If omitted, the API returns default fields.
- `key_name` (REQUIRED) — Named credential from `KeyManager` for Bearer token (PAT) authentication. The `api_key` field of the resolved provider is sent as `Authorization: Bearer <token>`. Credentials MUST NOT appear in the JCWF file.
- `page_size` (OPTIONAL, integer, default: 100) — Number of items per page. The runtime paginates automatically until all results are fetched or `max_items` is reached.

**REST API mapping:**

```
GET {base_url}/rest/v1/projects/{project_id}/workitems
  ?query={url_encoded_query}
  &fields[workitems]={comma_separated_fields}
  &page[size]={page_size}
  &page[number]={N}
```

Authentication: Bearer token (PAT) via `key_name` → `KeyManager` lookup (same pattern as AI provider keys).

**Pagination:** The `FilterEngine` fetches pages sequentially, writing each item to `<filterID>/<filterID>-<k>.json` as it goes. Peak memory usage is bounded to one page. Pagination stops when:
- The API returns fewer items than `page_size` (last page), or
- `max_items` is reached.

**Item shape:**

| Key                  | Value                                         |
|----------------------|-----------------------------------------------|
| `<binding>.index`    | 0-based item number (across all pages)        |
| `<binding>.id`       | Polarion work item ID                         |
| `<binding>.<field>`  | Field value (e.g. `item.title`, `item.status`) |

**Freshness:** The filter manifest `query_hash` tracks the query expression. Since Polarion is a remote source with no local mtime, items fetched via `polarion_query` are always considered stale unless the `query_hash` is unchanged **and** the manifest `evaluated_at` is within a configurable staleness window (default: re-evaluate every run). Implementations MAY add a `cache_ttl_seconds` field to allow caching across runs.

**Implementation note:** The paginated fetch is performed in C++ by a new `PolarionClient` class using libcurl (already vendored). This keeps the HTTP round-trips efficient and avoids Python GIL overhead for potentially thousands of items.

#### 3.7.6 Filter Manifest

Each filter evaluation writes a manifest file to `<workflowBaseDir>/<filterID>/<filterID>.manifest.json` containing:

- `filter_id` — The filter's ID.
- `evaluated_at` — ISO 8601 timestamp of the evaluation.
- `query_hash` — SHA-256 hash of the normalized filter expression. If the expression changes, all items are considered stale.
- `item_count` — Number of matched items.
- `items[]` — Per-item entries with `index`, `key` (stable identity), `source_path`, and `source_mtime`.

The manifest enables cross-run freshness comparison: new items are always stale, removed items produce a warning, and existing items are checked per the standard freshness rules.

#### 3.7.7 Filter Builder (Frontend)

The workflow editor provides a visual **filter builder dialog** for constructing complex queries:

- **Visual grouping** — nested AND/OR groups via add/remove buttons.
- **Operator selector** — `=`, `!=`, `range`, `wildcard`, `exists`.
- **Range inputs** — two-field input for `[lo TO hi]` ranges.
- **Live preview** — renders the query string in real time.
- **Test (dry run)** — evaluates the filter and shows matching item count without running tasks.
- **CSV/text mode** — file path picker, row range input, column preview, skip-empty toggle.

---

### 3.8 Control Nodes & Controlflow *(new in v1.1)*

JCWF supports **control-flow graph extensions** that allow workflows to express conditional execution paths — for example, routing to a recovery task when a build step fails, while still allowing the overall workflow run to succeed.

Control flow is declared via two root-level arrays: `control_nodes` (graph nodes that are not tasks) and `controlflow` (directed edges between tasks and control nodes).

#### 3.8.1 Control Nodes

```jsonc
"control_nodes": [
  {
    "id": "branch_1",
    "type": "branch",
    "label": "error recovery branch"
  }
]
```

Each control node has:

- `id` (REQUIRED, string) — Unique identifier. MUST NOT collide with any task id.
- `type` (REQUIRED, string) — The control node type. Currently defined types:
  - `"branch"` — A two-output conditional node that routes execution based on whether the driving task succeeded or failed. One output activates the **normal** (success) path; the other activates the **on_error** (failure) path.
- `label` (OPTIONAL, string) — Human-friendly label for UI display.

Control nodes are **not tasks**; they have no `working_directory`, no `params`, and produce no artifacts. They exist purely to direct control flow.

**Future types** (reserved, not yet implemented): `merge`, `switch`, `guard`.

#### 3.8.2 Controlflow Edges

```jsonc
"controlflow": [
  { "from": "shell",     "to": "branch_1",    "kind": "normal",       "from_port": "dep-source",    "to_port": "cf-in-normal" },
  { "from": "shell",     "to": "branch_1",    "kind": "error_signal", "from_port": "error-signal",  "to_port": "cf-in-error"  },
  { "from": "branch_1",  "to": "shell_2",     "kind": "normal",       "from_port": "cf-out-normal", "to_port": "dep-target"   },
  { "from": "branch_1",  "to": "ai_call_fix", "kind": "on_error",     "from_port": "cf-out-error",  "to_port": "dep-target"   }
]
```

Each controlflow edge has:

- `from` (REQUIRED, string) — Source node id (task or control node).
- `to` (REQUIRED, string) — Target node id (task or control node).
- `kind` (REQUIRED, string) — The edge semantics. Defined kinds:
  - `"normal"` — Carries the success/completion signal. Used for: task → branch (success input), branch → task (success output).
  - `"error_signal"` — Carries the failure signal from a task to a branch node. The source task MUST have `expose_error_signal: true`.
  - `"on_error"` — Branch output that activates when the driving task **failed**. Connects branch → downstream task(s) on the error recovery path.
- `from_port` (OPTIONAL, string) — Port identifier on the source node (used by the visual editor for handle placement).
- `to_port` (OPTIONAL, string) — Port identifier on the target node.

**Validation rules:**

- Every `from` and `to` MUST reference a valid task id or control node id.
- Controlflow edges participate in DAG cycle detection alongside `depends_on` edges.
- A branch node SHOULD have exactly one driving input (either a `normal` or `error_signal` edge from a task).
- A branch node MAY have zero or more `normal` output edges and zero or more `on_error` output edges.

**Interaction with `depends_on`:**

Controlflow edges are **independent** of `depends_on`. A task gated by controlflow (i.e., a task that is the target of a controlflow edge from a branch node) will not run until the branch activates it, regardless of `depends_on` readiness. Tasks with **no** incoming controlflow edges follow the standard `depends_on` readiness rules.

#### 3.8.3 Task Field: `expose_error_signal`

```jsonc
{
  "id": "shell",
  "type": "shell",
  "expose_error_signal": true,
  ...
}
```

- `expose_error_signal` (OPTIONAL, boolean, default: `false`)
  - When `true`, the task emits a failure signal that can be consumed by a branch node via an `error_signal` controlflow edge.
  - This flag tells the runtime that the task's failure is **potentially handled** by downstream branching logic and SHOULD NOT automatically fail the entire workflow run (see Rule A in §3.8.4).
  - The task's visual state still shows as **Failed** (red) in the UI — the flag does not mask the failure, it only affects run-level failure propagation.

#### 3.8.4 Runtime Semantics (Rule A)

**Rule A (Handled vs. Unhandled Failures):**

> A workflow run fails **only** if it contains at least one task in `Failed` state whose failure is **unhandled**.

A task's failure is considered **handled** if:
1. The task has `expose_error_signal: true`, AND
2. There exists an `error_signal` controlflow edge from that task to a branch node.

When a handled task fails:
- The branch node **fires** and activates its `on_error` output targets.
- The branch's `normal` output targets are marked `Skipped`.
- Downstream tasks that depend on the failed task via `depends_on` are still skipped (standard propagation).
- The failed task remains in `Failed` state (visible as red in the UI).
- The run does **not** set `m_HasFailed` for this task.

When a handled task **succeeds**:
- The branch node fires and activates its `normal` output targets.
- The branch's `on_error` output targets are marked `Skipped`.

**Controlflow gating:**

Tasks that are the target of controlflow edges from a branch node are **gated**: they remain in `Pending` state until the branch activates them. If the branch takes the other path, gated tasks are marked `Skipped` with a descriptive message.

If no work is in-flight and gated tasks remain non-activated, the runtime marks them `Skipped` to allow the run to reach a terminal state.

**Example: two-branch recovery pattern**

```
ai_call (generate broken hello.cpp)
ai_call_2 (generate Makefile)
    ↓ depends_on
shell (make) [expose_error_signal: true]
    ↓ normal + error_signal
branch_1
    ├─ normal → shell_2 (run hello)
    └─ on_error → ai_call_fix (fix code) → shell_retry (retry make)
                                                ↓ normal
                                            branch_2
                                                └─ normal → shell_2 (run hello)
```

See `example/workflows/exampleMakefile5.jcwf` for a complete working example.

---

## 4. Execution Model

This section describes how JarvisAgent should execute JCWF workflows across the C++ core, Python engine, and the web UI.

### 4.1 High-Level Flow

1. Load `.jcwf` files from a configured directory.  
2. Validate JSON structure, triggers, and the `depends_on` DAG (no cycles).  
3. Register workflows and triggers with the JarvisAgent core.  
4. On trigger activation (cron, file, structure, manual):  
   - **Provider prerequisite check:** verify that every `ai_call` task's required provider exists in the provider registry (see 3.3.7). If any provider is missing, block the run with a log warning.  
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

Recommended data structures (implemented in `workflowTypes.h`; shown here in simplified form):

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

The core orchestrator SHOULD be deterministic and thread-safe. It MAY use a task queue and worker threads. Document conversion is delegated to external Python processes (e.g. markitdown), while AI calls are issued by the C++ `AiRequestPool` through the shared `CurlMultiDispatcher` (HTTP/2 multiplex).

For tasks that integrate with the existing queue system, the orchestrator MAY map task execution to creation or consumption of STNG_, TASK_, CNTX_, PROB_, and PROV_ files in the queue directories according to `queue_binding`.

---

### 4.3 Python Side: Task Executor

Python is one of four task executors (`python`, `ai_call`, `shell`, `internal`). The `PythonEngine` is responsible for:

- Executing `python` tasks' logic (user-defined Python modules/functions).  
- Optional helpers for reading XLS, documents, and other sources (e.g. markitdown for document conversion).


Python interface expectations:

- For `python` tasks:

  ```python
  # Example module: workflows/doc_tasks.py

  def convert_to_markdown(source_path: str, output_dir: str, context: dict) -> dict:
      # context: workflow run state (read-only or partial write)
      # return: dict of outputs, e.g. {"markdown_path": "..."}
      ...
  ```

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

### 5.3 Failure Propagation (Rule A)

If a task fails (after all retries):

- Tasks that depend on it via `depends_on` MUST NOT run (they are marked `Skipped`).
- **Rule A:** The workflow run status is determined by whether any **unhandled** failures remain at run termination.
  - A failure is **handled** if the task has `expose_error_signal: true` AND an `error_signal` controlflow edge routes it to a branch node (see §3.8.4).
  - If all failed tasks are handled, the run completes as `Succeeded`.
  - If any failed task is unhandled, the run completes as `Failed`.
- The failed task's visual state remains `Failed` (red) in the UI regardless of whether the failure is handled — Rule A only affects the **run-level** outcome.

See §3.8.4 for the full runtime semantics of controlflow gating and branch activation.

---

## 6. Handling Triggers

### 6.1 Time-Based (Cron)

- The C++ side integrates with a scheduler to check cron expressions.  
- When a cron fires, it creates a new workflow run.

### 6.2 File-Based

- Integrated with the existing FileWatcher.  
- When a relevant event is received (path and event type), the orchestrator checks matching triggers and starts a run.

### 6.3 Filter-Driven Expansion

The runtime evaluates **filter nodes** (see 3.7) to enumerate items from CSV files, text file lines, or Lucene-style query indexes. For each matched item, the orchestrator creates a task instance (`taskId#k`), injects item values as inputs under the filter's binding prefix, and writes an output file named `<filterID>-<k>.txt`. A filter manifest tracks the item list and expression hash for freshness comparison across runs.

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

When a task starts, the runtime resolves each declared input through a **priority chain**. The first mechanism that produces a value wins:

1. **Dataflow edges** — explicit `from_task.from_output → to_task.to_input` wiring declared in the `"dataflow"` array.
2. **Run context** — lookup by input name in the workflow run's shared `ContextMap`. Context values can be seeded at run start (via the REST API) or auto-published by upstream tasks (see §8.2).
3. **Input default** — the `"default"` field on the input declaration (see §3.3.4).

If none of these mechanisms resolve a value:
- **Required** inputs (`"required": true`) cause the task to **fail immediately** with a descriptive error.
- **Optional** inputs are silently omitted.

After all inputs are resolved, template expansion (`{{inputs.slot_name}}`) is applied to substitute resolved values into `params.args` and other template-enabled fields.

**Note (shell tasks):** `{{input[i]}}` / `{{output[i]}}` positional template expansion is based on `file_inputs` / `file_outputs` and does not require named `inputs` / `outputs` declarations. If `args` is omitted, the runtime auto-injects individual `{{input[N]}}` and `{{output[N]}}` as positional arguments (see §3.3.1 Option B).

### 8.2 Outputs and Context

Task outputs are written to two destinations:

1. **Local task outputs** — stored in `TaskInstanceState.m_OutputValues` for downstream dataflow wiring.
2. **Shared run context** — when a task succeeds, the runtime **automatically publishes** all its output values into the workflow run's `ContextMap` under composite keys of the form `taskId.outputName`.

For example, if task `convertToMarkdown` produces output `markdown_path`, the context key `convertToMarkdown.markdown_path` becomes available to all downstream tasks. A downstream task can declare an input named `convertToMarkdown.markdown_path` and it will resolve via the context lookup (step 2 of §8.1) without requiring an explicit `dataflow` edge.

**Initial context from REST API:**

The `POST /api/workflows/<id>/run` endpoint accepts an optional JSON body to seed the run context before any task executes:

```json
{
  "context": {
    "user_name": "Alice",
    "environment": "production"
  }
}
```

These values are available to all tasks via the context lookup (step 2 of §8.1). If a body is omitted or empty, the run starts with an empty context.

**Freshness note:** The Makefile-style freshness checker (§3.3.3) only compares `file_inputs` / `file_outputs` timestamps. Context values and input defaults are invisible to freshness. Tasks whose inputs come solely from context or defaults will always re-run (no `file_outputs` to prove freshness). Tasks that mix file and context inputs may be incorrectly skipped if only the context value changed between runs.

---

## 9. JSON Schema (Draft)

The JCWF JSON Schema lives at **[`doc/jcwf.schema.json`](jcwf.schema.json)** (Draft 2020-12).

It is intentionally a pragmatic subset — sufficient for the editor's generate/validate flow and for schema-enforced output (`output_schema` on `ai_call`, §3.3.6). The runtime `WorkflowValidator` handles semantic checks that go beyond JSON Schema's reach (cycle detection, dataflow slot matching, path resolution).

The schema is compiled into the binary at build time via the Premake prebuild step (`tools/generateEmbeddedHeaders.py` → `application/json/jcwfSchema.generated.h`), so runtime consumers never read it from disk.

---

## 10. Security Considerations

- `shell` tasks can be dangerous; JarvisAgent SHOULD provide configuration flags to disable or restrict them and MUST enforce the `scripts/` prefix rule.  
- `ai_call` tasks send data to external services; sensitive data MUST be handled carefully.  
- **API keys** MUST NOT be stored in JCWF files or `config.json`. They are managed in JarvisAgent's encrypted key store (`keys.json.enc`, AES-256-GCM with a master password) or provided via environment variables. See `engine/keys.md` for the key management design.  
- JCWF files SHOULD be sourced from trusted locations; tampering can change automation behavior.  
- Structure-based iteration over external documents and XLS files SHOULD validate inputs to avoid unexpected expansion or injection.
- Filter nodes with `max_items: 0` (no limit) SHOULD be used with caution; unbounded expansion can exhaust system resources.

---

---

## 11. Script Metadata Format

Scripts in the `scripts/` folder MAY include a structured metadata header that JarvisAgent
parses at startup (and on file-watch events) to build an in-memory **script registry**.
The registry is used by the AI JCWF generation pipeline to provide the AI with an accurate
inventory of available scripts — their purpose, call signatures, and descriptions.

### 11.1 Marker

A script is registered if it contains the marker line:

- **Bash/shell:** `# @jarvis-script`
- **Python:** `# @jarvis-script`

The marker MUST appear within the first 20 lines of the file.

### 11.2 Metadata Fields

All fields use the `# @<field>:` prefix (comment-style, one per line).
Multi-line values continue on subsequent `#`-prefixed lines until the next `@`-field
or the end of the header block (first non-comment line).

| Field | Required | Description |
|-------|----------|-------------|
| `@short` | YES | One-line summary (≤120 chars). Used in the registry table. |
| `@params` | YES | Parameter list. Each parameter on its own `#   - ` line. |
| `@description` | NO | Extended description. May span multiple lines. |
| `@outputs` | NO | Description of what the script produces. |

### 11.3 Bash Script Example

```bash
#!/usr/bin/env bash
# @jarvis-script
# @short: Clone a git repository to a local directory
# @params:
#   - $1: repo_url (string) — HTTPS or SSH URL of the repository
#   - $2: output_dir (string) — Target directory for the clone
# @description:
#   Clones a git repository using the system git command.
#   If output_dir already exists, performs a pull instead.
#   Exits non-zero on failure.
# @outputs:
#   - The cloned repository in output_dir
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LAUNCH_DIR="$(dirname "$SCRIPT_DIR")"
# ... implementation ...
```

### 11.4 Python Script Example

```python
# @jarvis-script
# @short: Count lines of code in a directory tree
# @params:
#   - input_dir (str): Root directory to scan
#   - output_file (str): Path to write the JSON summary
# @description:
#   Walks the directory tree, counts lines per file extension,
#   and writes a JSON summary with per-language totals.
# @outputs:
#   - output_file: JSON file with line counts
from __future__ import annotations
from pathlib import Path
import json

def countLines(input_dir: str, output_file: str) -> dict:
    # ... implementation ...
    return {"output_file": str(output_path)}
```

### 11.5 Parsing Rules

1. JarvisAgent scans `scripts/` at startup for files matching `*.sh`, `*.py`, `*.ps1`.
2. For each file, read the first 50 lines looking for `@jarvis-script`.
3. If found, extract all `@`-prefixed fields into a `ScriptRegistryEntry` struct.
4. The registry is exposed to the AI generation pipeline as a Markdown table (see §11.6).
5. The `scripts/` folder is monitored by the FileWatcher. When a script is added,
   modified, or removed, the registry is updated automatically.

### 11.6 Registry Table Format

The script registry is serialized as a Markdown table for AI context injection:

```markdown
| Script | Short Description | Parameters |
|--------|-------------------|------------|
| `scripts/runMake.sh` | Wrapper for 'make' command | (none) |
| `scripts/clone_repo.sh` | Clone a git repository | `$1`: repo_url, `$2`: output_dir |
| `scripts/countLines.py` | Count lines of code in a directory tree | `input_dir` (str), `output_file` (str) |
```

This table is injected into the DECOMPOSE stage (Stage 1) of the AI JCWF generation
pipeline so the AI knows which scripts already exist and can reuse them instead of
requesting new ones.

---

---

## JCWF Container Format (zip)

Starting with the sub-workflow feature, the `.jcwf` file extension denotes a **zip container**
that bundles workflow JSON files and sub-workflow folders into a single portable package.
Individual workflow definitions use the `.json` extension.

### Container Structure

```
my-pipeline.jcwf   (zip file)
├── global.json                           ← workflow-level metadata (entire tree)
├── my-pipeline.json                      ← root canvas task DAG
├── code validation/                      ← sub-workflow folder
│   └── code validation.json              ← sub-workflow task DAG
└── report generation/
    ├── report generation.json
    └── data cleanup/                     ← nested sub-workflow
        └── data cleanup.json
```

### `global.json` (root level only)

Contains workflow-wide metadata. Never contains tasks.

| Field | Type | Description |
|-------|------|-------------|
| `version` | string | JCWF version (`"1.0"` or `"1.1"`). |
| `id` | string | Workflow identifier. |
| `label` | string | Human-friendly name. |
| `doc` | string | Documentation. |
| `manual_start` | boolean | Default `true`. |
| `triggers` | array | Trigger definitions (auto, cron, file_watch, webhook, manual). |
| `defaults` | object | Default timeout, retries, AI provider settings. |

### Canvas JSON files

Each canvas JSON contains the task DAG for one level of the hierarchy:
`tasks`, `dataflow`, `filters`, `control_nodes`, `controlflow`.

- The root canvas JSON is named after the workflow (e.g., `my-pipeline.json`), or any
  name that is not `global.json`.
- Sub-workflow canvas JSONs are named after their folder (e.g., `code validation/code validation.json`).
- Canvas JSONs do NOT contain triggers, `manual_start`, or other root-level metadata.

### Sub-workflow folders

- Each subfolder represents a sub-workflow. The folder name is the sub-workflow's display name.
- The folder name is shown in the editor's tree view on the left panel.
- A `sub_workflow` task references a sub-workflow by folder name (relative path).
- Sub-workflow folders may be nested to any depth (max 10 levels enforced by the validator).

### Extraction model

When a `.jcwf` container is loaded or run:

1. The zip is extracted to `workflows/<workflow-name>/` (sibling of the `.jcwf` file).
2. The extracted folder becomes the **workflow root directory**.
3. All `working_directory` fields on tasks are relative to this folder.
4. Build artifacts are written inside the extracted folder.
5. Deleting `workflows/<workflow-name>/` cleans all artifacts for that workflow.
6. Re-extraction occurs when the zip is newer than the extracted folder.

*End of JC Workflow File (JCWF) Specification v1.1*
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


### Absolute path naming convention (JarvisAgent runtime)

**Policy:** if a path stored in a variable/member field/signal is **guaranteed** to be absolute, that variable/member field/signal name MUST end with `Absolute` (for example `workflowFilePathAbsolute`).

### Missing source files

**Policy:** missing input sources are a **hard error** for all executors (`ai_call`, `internal`, `shell`, `python`).

If any required source file (e.g. `file_inputs` or non-inline `queue_binding` files) cannot be found/read, the task fails and subsequent tasks do not start.
