# JC Workflow Spec ↔ JarvisAgent Implementation (with Dataflow / “Data Slot” lifecycle)

This document explains how the **JC Workflow File Format™ (JCWF)** maps onto the **JarvisAgent** C++ implementation, with a focus on **data slots**, **dataflow**, and how values move through the runtime pipeline.

---

## 1) Core concepts from the JCWF spec

### 1.1 Workflow / Task / Dependency / Trigger (recap)
JCWF describes a **workflow** as a graph of **tasks**, optionally started by **triggers**, and executed under dependency + freshness rules.

The spec also introduces two “data” concepts that matter for pipelines:

- **Data Slot**: a *named input or output field* associated with a task (e.g., `zipAnswer.archive_path`).  
- **Context / State**: a *key-value store* that persists data across tasks within a workflow run.

---

## 2) Mapping: spec concepts → JarvisAgent types/classes

| Spec concept | JarvisAgent representation |
|---|---|
| Workflow definition (static) | `WorkflowDefinition` (loaded & stored in `WorkflowRegistry`) |
| Task definition (static) | `TaskDefinition` (inside `WorkflowDefinition::m_Tasks`) |
| Data slot declaration (static) | `TaskIOField` (input/output slot type + required flag) |
| Dataflow wiring (static) | `DataflowDef` (connects `from_task.output` → `to_task.input`) |
| Workflow run (runtime) | `WorkflowRun` (run-level context + per-task runtime states) |
| Task state (runtime) | `TaskInstanceState` (status + resolved input/output values + timestamps) |
| Resolve per-task inputs | `DataflowResolver` |
| Freshness / up-to-date decision | `TaskFreshnessChecker` |
| Dispatch/advance a run | `WorkflowRuntimeManager` (plus orchestration helpers) |
| Execute a task by type | `TaskExecutorRegistry` + executors (`AiCallTaskExecutor`, `ShellTaskExecutor`, `PythonTaskExecutor`, `InternalTaskExecutor`) |
| Bind triggers to runtime | `WorkflowTriggerBinder` + `TriggerEngine` |
| App lifecycle hook | `JarvisAgent::InitializeWorkflows()` (loads, binds, starts runtime) |

---

## 3) “Data Slot” lifecycle, end-to-end

### 3.1 What “data” looks like at runtime
In practice, JarvisAgent largely treats values as **strings** as they travel between tasks:

- **Static slot schema**: a slot is declared as a `TaskIOField` (name + type + required).
- **Runtime values**: once resolved, values are stored in `TaskInstanceState` as resolved inputs/outputs (string-like values) and then become available to downstream tasks through `DataflowResolver`.

### 3.2 Mermaid: data structure overview (static + runtime)

```mermaid
classDiagram
direction LR

class WorkflowRegistry {
  +unordered_map~string, WorkflowDefinition~ m_Workflows
}

class WorkflowDefinition {
  +string m_Id
  +map~string, TaskDefinition~ m_Tasks
  +vector~DataflowDef~ m_Dataflows
  +vector~WorkflowTrigger~ m_Triggers
}

class TaskDefinition {
  +string m_Id
  +TaskType m_Type
  +vector~string~ m_DependsOn
  +vector~TaskIOField~ m_Inputs
  +vector~TaskIOField~ m_Outputs
  +QueueBinding m_QueueBinding
  +... params/environment/retry ...
}

class TaskIOField {
  +string m_Name
  +string m_Type
  +bool m_Required
}

class DataflowDef {
  +string m_FromTask
  +string m_FromOutput
  +string m_ToTask
  +string m_ToInput
  +map~string,string~ m_Mapping
}

class WorkflowRun {
  +string m_WorkflowId
  +WorkflowRunState m_State
  +map~string, ContextValue~ m_Context
  +map~string, TaskInstanceState~ m_TaskStates
}

class TaskInstanceState {
  +TaskInstanceStateKind m_State
  +map~string,string~ m_ResolvedInputs
  +map~string,string~ m_ResolvedOutputs
  +... timestamps/errors/attempts ...
}

class ContextValue {
  +string m_Value
}

WorkflowRegistry "1" --> "many" WorkflowDefinition
WorkflowDefinition "1" --> "many" TaskDefinition
WorkflowDefinition "1" --> "many" DataflowDef
WorkflowDefinition "1" --> "many" WorkflowTrigger
WorkflowRun "1" --> "many" TaskInstanceState
WorkflowRun "1" --> "many" ContextValue
TaskDefinition "1" --> "many" TaskIOField : inputs
TaskDefinition "1" --> "many" TaskIOField : outputs
```

---

## 4) Execution pipeline: from file → run → task execution

### 4.1 High-level runtime flow
At startup, `JarvisAgent` initializes the workflow system (loads workflows, registers executors, binds triggers, starts runtime ticking).  
The runtime then advances workflow runs in a non-blocking, tick-based manner.

```mermaid
flowchart TD
  A[JarvisAgent::OnStart] --> B[InitializeWorkflows]
  B --> C[WorkflowRegistry: load .jcwf]
  B --> D[TaskExecutorRegistry: register AI/Shell/Python/Internal]
  B --> E[WorkflowRuntimeManager: Start]
  B --> F[WorkflowTriggerBinder: bind triggers]
  F --> G[TriggerEngine: watch/cron/manual/auto]

  H[JarvisAgent::OnUpdate tick] --> I[TriggerEngine tick]
  H --> J[WorkflowRuntimeManager::Update]
  I -->|fires trigger| J
  J --> K[StartPendingRuns]
  J --> L[TickActiveRun]
```

### 4.2 TickActiveRun (what happens inside a run)

```mermaid
flowchart TD
  R0[TickActiveRun] --> R1[Harvest completed worker futures]
  R1 --> R2[Drain AI completions from AiRequestPool]
  R2 --> R3[For each task: compute readiness]
  R3 --> R4{All dependencies succeeded/skipped?}
  R4 -- no --> R3
  R4 -- yes --> R5[Resolve inputs via DataflowResolver]
  R5 --> R6[Freshness check via TaskFreshnessChecker]
  R6 --> R7{Up-to-date?}
  R7 -- yes --> R8[Mark task SKIPPED]
  R7 -- no --> R9[Dispatch to TaskExecutorRegistry]
  R9 --> R10[Worker executes -> result state/output values]
  R10 --> R11[Persist outputs & update TaskInstanceState]
  R11 --> R12{Run terminal?}
  R12 -- no --> R0
  R12 -- yes --> R13[Mark WorkflowRun succeeded/failed]
```

---

## 5) The “dataflow” resolution step (where `data` really moves)

### 5.1 What DataflowResolver does
Conceptually, `DataflowResolver` computes a task’s resolved inputs by combining:

1. **Explicit declared inputs** on the task definition
2. **Dataflow edges** (upstream `task.output` → this `task.input`)
3. (Optionally) **template expansion** (if your workflow uses templates)

It also validates that required inputs exist and logs errors when resolution fails.

### 5.2 Mermaid: “resolve inputs” decision graph

```mermaid
flowchart TD
  D0["ResolveInputsForTask"] --> D1["Start with empty resolvedInputs"]
  D1 --> D2["For each input slot declared in TaskDefinition"]
  D2 --> D3{"Has dataflow edge feeding this input?"}
  D3 -- yes --> D4["Lookup upstream task state"]
  D4 --> D5{"Upstream output exists?"}
  D5 -- yes --> D6["Copy value into resolvedInputs[inputName]"]
  D5 -- no --> D7["Log error: missing upstream output"]
  D3 -- no --> D8{"Has explicit raw param/template?"}
  D8 -- yes --> D9["Expand templates -> resolvedInputs"]
  D8 -- no --> D10{"Is input required?"}
  D10 -- yes --> D11["Log error: missing required input"]
  D10 -- no --> D12["Leave unset / optional"]
  D6 --> D2
  D7 --> D2
  D9 --> D2
  D11 --> D2
  D12 --> D2
  D2 --> D13["Return resolvedInputs (or failure)"]
```

---

## 6) Example: AI Car Maintenance Pipeline (straight pipeline + one dataflow edge)

The AI car maintenance example is a straight pipeline (AI → internal → AI → shell → python), with a **dataflow** wiring from the shell task output into the python task input.

```mermaid
flowchart TD
  %% Stage 1: classify
  Q["message.txt (user question)"] --> T1["classifyQuestion (ai_call)"]
  T1 --> C["classification.output.txt"]

  %% Stage 2: build manual (internal branch on classification)
  C --> D{"classification == 'engine'?"}
  D -- yes --> M1["Select engine manual"]
  D -- no --> E{"classification == 'tires'?"}
  E -- yes --> M2["Select tires manual"]
  E -- no --> M3["Select rephrase guidance"]

  M1 --> T2["buildManual (internal)"]
  M2 --> T2
  M3 --> T2
  T2 --> MAN["manual.txt"]

  %% Stage 3: answer (uses selected manual)
  MAN --> T3["answerWithManual (ai_call)"]
  Q --> T3
  T3 --> AOUT["answer.output.txt"]

  %% Stage 4: zip + python info
  AOUT --> T4["zipAnswer (shell)"]
  T4 --> ZIP["answer.zip"]
  ZIP --> T5["printZipInfo (python)"]

  %% dataflow edge (explicit)
  T4 -->|archive_path| T5
```

In this workflow:
- `classifyQuestion` reads the user question (PROB source) and produces `classification.output.txt`.
- `buildManual` reads that classification output and writes `manual.txt`.
- `answerWithManual` uses `manual.txt` (context) + the original question to generate `answer.output.txt`.
- `zipAnswer` packages the answer into `answer.zip`.
- `printZipInfo` receives the `answer.zip` path via **dataflow** (`zipAnswer.archive_path → printZipInfo.filename`) and calls a Python function to print file info.

---

## 7) AI tasks and “queue artifacts” (STNG / TASK / CNTX / PROB)

AI-call tasks generate a set of files in the **queue** directory per task run. This is the same “file transparency” pattern used in `aiZipDemo`:

- The engine writes **STNG**, **TASK**, **CNTX** and a uniquely named **PROB** file per request.
- The AI response is written as a corresponding `PROB_...output...` file.
- The workflow-level output file (e.g. `classification.output.txt`, `answer.output.txt`) is the stable “product” that downstream tasks depend on via freshness rules.

```mermaid
flowchart LR
  A["TaskDefinition (ai_call)"] --> B["AiCallTaskExecutor"]
  B --> C["Write STNG_*.txt"]
  B --> D["Write TASK_*.txt"]
  B --> E["Write CNTX_*.txt"]
  B --> F["Write PROB_{idx}_{timestamp}.txt"]
  F --> G["Submit async request -> AiRequestPool"]
  G --> H["Completion arrives"]
  H --> I["Write PROB_{idx}_{timestamp}.output.txt"]
  I --> J["Write stable workflow output file"]
```

---

## 8) About the “data.data type” wording

In JCWF terms, the “data” that flows between tasks is best thought of as:

- **Data slots**: named input/output fields (declared by `TaskIOField`)
- **Dataflow edges**: wiring that copies one task’s output slot into another task’s input slot (`DataflowDef`)
- **Runtime values**: the resolved string values stored on `TaskInstanceState` and optionally in the run-level `WorkflowRun` context map

If you meant a *specific C++ type literally named* `data` or `Data`, that type would need to be referenced directly (it does not appear as a named type in the high-level workflow types summary).

---

## 9) Practical tips (authoring workflows so dataflow stays clear)

- Prefer **one output file per stage** as the stable artifact for freshness (`*.output.txt`, `*.output.md`, `*.zip`, etc.).
- Use **dataflow** for “parameter passing” (e.g. file path of produced artifact) instead of duplicating paths in multiple tasks.
- When debugging “why did this run again?”, check:
  - missing outputs
  - any input timestamp newer than outputs
  - or an upstream task re-ran and updated its outputs, forcing downstream tasks to be stale.

---

## Appendix A: Pointers to existing docs in this repo

- **JarvisAgent Architecture** (`architecture.md`)
- **AI Zip Demo workflow doc** (`aiZipDemoWorkflow.md`)
- **AI Car Maintenance workflow doc** (`aiCarMaintenancePipeline.md`)
- **Combined C++ documentation** (`combinedDocumentation.md`)
