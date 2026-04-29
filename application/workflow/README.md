# Workflow Runtime

DAG-based workflow execution engine. Parses JCWF files, resolves dependencies, dispatches tasks in parallel, and tracks execution state.

## Key Files

| File | Purpose |
|------|---------|
| `workflowTypes.h` | Task types, trigger types, run state, TaskDef, WorkflowRun |
| `workflowJsonParser.h/cpp` | JCWF JSON parsing into WorkflowDefinition |
| `workflowRegistry.h/cpp` | Loads and indexes .jcwf containers from the workflows folder |
| `workflowRuntimeManager.h/cpp` | Run lifecycle, task dispatch, dependency resolution, cancellation |
| `workflowValidator.h/cpp` | JCWF validation (structure, references, working_directory) |
| `workflowFileIndex.h/cpp` | File freshness tracking (SHA-256 manifests) |
| `jcwfContainer.h/cpp` | JCWF zip container read/write |
| `triggerEngine.h/cpp` | Cron, file-watch, webhook, S3/OneDrive/email polling triggers |
| `workflowTriggerBinder.h/cpp` | Binds parsed trigger definitions to TriggerEngine |
| `taskPathResolver.h/cpp` | Resolves working directories and file paths per task |
| `taskExecutorRegistry.h/cpp` | Maps TaskType enum to ITaskExecutor implementations |
| `aiRequestPool.h/cpp` | Parallel AI API dispatch pool. Computes the size-aware curl `CURLOPT_TIMEOUT_MS` budget per request, plumbs `QuotaKey` to the dispatcher's adaptive controller, fires cascade-cancellation when a run terminates (`CancelRequestsForRun`). |
| `aiCallTaskExecutor.h/cpp` | AI call task execution (queue file assembly, API dispatch) |

The adaptive rate-limit controller (`RateLimitController`) and per-provider strategy (`IRateLimitStrategy`) live in `engine/curlWrapper/` — see `engine/curlWrapper/curlWrapper.md` §15 and `doc/architecture.md` "Rate-limit + concurrency control" for the design.

## Task Executors

| File | Task Type | Description |
|------|-----------|-------------|
| `shellTaskExecutor` | `shell` | Shell command execution via subprocess |
| `pythonTaskExecutor` | `python` | Python script execution via PythonEnginePool |
| `aiCallTaskExecutor` | `ai_call` | AI API calls with queue file assembly |
| `internalTaskExecutor` | `internal` | Built-in operations (zip, copy, etc.) |
| `subWorkflowTaskExecutor` | `sub_workflow` | Nested workflow execution |

Cloud task executors are in `application/cloud/` — see `application/cloud/README.md`.

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `filter/` | PolarionClient, FilterEngine for per-item expansion |
| `cloudTaskExecutors/` | *(legacy, migrated to `application/cloud/`)* |
| `doc/` | Workflow-specific TODO list |

## JCWF Specification

See `doc/JC_Workflow_Specification.md` for the full format definition.
