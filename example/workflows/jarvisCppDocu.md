# jarvisCppDocu Workflow – C++ Class Documentation Generation

**Label:** JarvisAgent C++ Docu Generator

**Workflow doc:** ['Generates Markdown documentation for each C++ header (and matching .cpp when present) in application/ and engine/.', 'Each task is an ai_call and writes its artifacts into a per-task folder under ../queue/<workflowId>/.']

This workflow generates one documentation artifact per C++ header in the `application/` and `engine/` trees (optionally including the matching `.cpp` file when present). Each class doc is produced by an `ai_call` task that writes STNG/TASK/PROB queue artifacts inline and references source files via CNTX.

## Triggers

- `auto` (`auto-run`) – enabled=true
- `manual` (`manual-run`) – enabled=true

## Directory layout

- Workflow file lives under `../workflows/`.
- Each AI task uses a working directory under `../queue/jarvisCppDocu/<NN>_<taskId>/`.
- Relative file paths in tasks are resolved relative to the **task working directory** (per JCWF spec).

## Queue artifacts produced per AI task

Each `ai_call` task declares a `queue_binding` with four parts:

- **STNG**: written inline to `STNG_docu.txt`
- **TASK**: written inline to `TASK_docu.txt`
- **CNTX**: points at the header and (optionally) cpp file paths
- **PROB**: written inline to `PROB_docu.txt`

### STNG_docu.txt (inline content)

```text
write consise, succinct, no guessing, no embelishments
```

### TASK_docu.txt (inline content)

```text
Write a docu about this C++ class that helps humans and AIs to quickly and efficiently learn about what the function does.
```

### PROB_docu.txt (inline content)

```text
Generate documentation for the provided C++ class. Output Markdown.
```

## Task pattern

All tasks follow the same pattern:

- `type`: `ai_call`
- `mode`: `single`
- `file_inputs`: the source header, plus the matching `.cpp` if it exists
- `file_outputs`: `docu.md`
- `queue_binding.cntx_files`: the same source file paths (header + optional `.cpp`)

## Tasks generated from the source tree

Total AI tasks: **60**

### application/

| Source | Includes .cpp? | Task ID | Working directory |
|---|---:|---|---|

| `application/application.h` | no | `doc_application_application_h` | `../queue/jarvisCppDocu/01_doc_application_application_h` |
| `application/file/fileCategorizer.h` | yes | `doc_application_file_fileCategorizer_h` | `../queue/jarvisCppDocu/02_doc_application_file_fileCategorizer_h` |
| `application/file/fileCategory.h` | no | `doc_application_file_fileCategory_h` | `../queue/jarvisCppDocu/03_doc_application_file_fileCategory_h` |
| `application/file/fileWatcher.h` | yes | `doc_application_file_fileWatcher_h` | `../queue/jarvisCppDocu/04_doc_application_file_fileWatcher_h` |
| `application/file/probUtils.h` | yes | `doc_application_file_probUtils_h` | `../queue/jarvisCppDocu/05_doc_application_file_probUtils_h` |
| `application/file/trackedFile.h` | yes | `doc_application_file_trackedFile_h` | `../queue/jarvisCppDocu/06_doc_application_file_trackedFile_h` |
| `application/jarvisAgent.h` | yes | `doc_application_jarvisAgent_h` | `../queue/jarvisCppDocu/07_doc_application_jarvisAgent_h` |
| `application/json/jsonObjectParser.h` | yes | `doc_application_json_jsonObjectParser_h` | `../queue/jarvisCppDocu/08_doc_application_json_jsonObjectParser_h` |
| `application/json/replyParser.h` | yes | `doc_application_json_replyParser_h` | `../queue/jarvisCppDocu/11_doc_application_json_replyParser_h` |
| `application/json/replyParserAPI1.h` | yes | `doc_application_json_replyParserAPI1_h` | `../queue/jarvisCppDocu/09_doc_application_json_replyParserAPI1_h` |
| `application/json/replyParserAPI2.h` | yes | `doc_application_json_replyParserAPI2_h` | `../queue/jarvisCppDocu/10_doc_application_json_replyParserAPI2_h` |
| `application/log/statusRenderer.h` | yes | `doc_application_log_statusRenderer_h` | `../queue/jarvisCppDocu/12_doc_application_log_statusRenderer_h` |
| `application/python/pythonEngine.h` | yes | `doc_application_python_pythonEngine_h` | `../queue/jarvisCppDocu/13_doc_application_python_pythonEngine_h` |
| `application/session/fileWriter.h` | yes | `doc_application_session_fileWriter_h` | `../queue/jarvisCppDocu/14_doc_application_session_fileWriter_h` |
| `application/session/sessionManager.h` | yes | `doc_application_session_sessionManager_h` | `../queue/jarvisCppDocu/15_doc_application_session_sessionManager_h` |
| `application/task/carMaintenanceTask.h` | yes | `doc_application_task_carMaintenanceTask_h` | `../queue/jarvisCppDocu/16_doc_application_task_carMaintenanceTask_h` |
| `application/task/internalTaskRegistry.h` | no | `doc_application_task_internalTaskRegistry_h` | `../queue/jarvisCppDocu/17_doc_application_task_internalTaskRegistry_h` |
| `application/task/taskBase.h` | no | `doc_application_task_taskBase_h` | `../queue/jarvisCppDocu/18_doc_application_task_taskBase_h` |
| `application/web/chatMessages.h` | yes | `doc_application_web_chatMessages_h` | `../queue/jarvisCppDocu/19_doc_application_web_chatMessages_h` |
| `application/web/webServer.h` | yes | `doc_application_web_webServer_h` | `../queue/jarvisCppDocu/20_doc_application_web_webServer_h` |
| `application/workflow/aiCallTaskExecutor.h` | yes | `doc_application_workflow_aiCallTaskExecutor_h` | `../queue/jarvisCppDocu/21_doc_application_workflow_aiCallTaskExecutor_h` |
| `application/workflow/aiRequestPool.h` | yes | `doc_application_workflow_aiRequestPool_h` | `../queue/jarvisCppDocu/22_doc_application_workflow_aiRequestPool_h` |
| `application/workflow/dataflowResolver.h` | yes | `doc_application_workflow_dataflowResolver_h` | `../queue/jarvisCppDocu/23_doc_application_workflow_dataflowResolver_h` |
| `application/workflow/internalTaskExecutor.h` | yes | `doc_application_workflow_internalTaskExecutor_h` | `../queue/jarvisCppDocu/24_doc_application_workflow_internalTaskExecutor_h` |
| `application/workflow/pythonTaskExecutor.h` | yes | `doc_application_workflow_pythonTaskExecutor_h` | `../queue/jarvisCppDocu/25_doc_application_workflow_pythonTaskExecutor_h` |
| `application/workflow/shellTaskExecutor.h` | yes | `doc_application_workflow_shellTaskExecutor_h` | `../queue/jarvisCppDocu/26_doc_application_workflow_shellTaskExecutor_h` |
| `application/workflow/taskExecutor.h` | yes | `doc_application_workflow_taskExecutor_h` | `../queue/jarvisCppDocu/27_doc_application_workflow_taskExecutor_h` |
| `application/workflow/taskExecutorRegistry.h` | yes | `doc_application_workflow_taskExecutorRegistry_h` | `../queue/jarvisCppDocu/28_doc_application_workflow_taskExecutorRegistry_h` |
| `application/workflow/taskFreshnessChecker.h` | yes | `doc_application_workflow_taskFreshnessChecker_h` | `../queue/jarvisCppDocu/29_doc_application_workflow_taskFreshnessChecker_h` |
| `application/workflow/triggerEngine.h` | yes | `doc_application_workflow_triggerEngine_h` | `../queue/jarvisCppDocu/30_doc_application_workflow_triggerEngine_h` |
| `application/workflow/workflowDataflow.h` | no | `doc_application_workflow_workflowDataflow_h` | `../queue/jarvisCppDocu/31_doc_application_workflow_workflowDataflow_h` |
| `application/workflow/workflowJsonParser.h` | yes | `doc_application_workflow_workflowJsonParser_h` | `../queue/jarvisCppDocu/33_doc_application_workflow_workflowJsonParser_h` |
| `application/workflow/workflowJsonParserDetails.h` | yes | `doc_application_workflow_workflowJsonParserDetails_h` | `../queue/jarvisCppDocu/32_doc_application_workflow_workflowJsonParserDetails_h` |
| `application/workflow/workflowOrchestrator.h` | yes | `doc_application_workflow_workflowOrchestrator_h` | `../queue/jarvisCppDocu/34_doc_application_workflow_workflowOrchestrator_h` |
| `application/workflow/workflowRegistry.h` | yes | `doc_application_workflow_workflowRegistry_h` | `../queue/jarvisCppDocu/35_doc_application_workflow_workflowRegistry_h` |
| `application/workflow/workflowRuntimeManager.h` | yes | `doc_application_workflow_workflowRuntimeManager_h` | `../queue/jarvisCppDocu/36_doc_application_workflow_workflowRuntimeManager_h` |
| `application/workflow/workflowTriggerBinder.h` | yes | `doc_application_workflow_workflowTriggerBinder_h` | `../queue/jarvisCppDocu/37_doc_application_workflow_workflowTriggerBinder_h` |
| `application/workflow/workflowTypes.h` | no | `doc_application_workflow_workflowTypes_h` | `../queue/jarvisCppDocu/38_doc_application_workflow_workflowTypes_h` |

### engine/

| Source | Includes .cpp? | Task ID | Working directory |
|---|---:|---|---|

| `engine/auxiliary/file.h` | yes | `doc_engine_auxiliary_file_h` | `../queue/jarvisCppDocu/39_doc_engine_auxiliary_file_h` |
| `engine/auxiliary/threadPool.h` | yes | `doc_engine_auxiliary_threadPool_h` | `../queue/jarvisCppDocu/40_doc_engine_auxiliary_threadPool_h` |
| `engine/core.h` | yes | `doc_engine_core_h` | `../queue/jarvisCppDocu/41_doc_engine_core_h` |
| `engine/curlWrapper/curlManager.h` | no | `doc_engine_curlWrapper_curlManager_h` | `../queue/jarvisCppDocu/42_doc_engine_curlWrapper_curlManager_h` |
| `engine/curlWrapper/curlWrapper.h` | yes | `doc_engine_curlWrapper_curlWrapper_h` | `../queue/jarvisCppDocu/43_doc_engine_curlWrapper_curlWrapper_h` |
| `engine/engine.h` | yes | `doc_engine_engine_h` | `../queue/jarvisCppDocu/44_doc_engine_engine_h` |
| `engine/event/applicationEvent.h` | no | `doc_engine_event_applicationEvent_h` | `../queue/jarvisCppDocu/45_doc_engine_event_applicationEvent_h` |
| `engine/event/engineEvent.h` | no | `doc_engine_event_engineEvent_h` | `../queue/jarvisCppDocu/46_doc_engine_event_engineEvent_h` |
| `engine/event/event.h` | no | `doc_engine_event_event_h` | `../queue/jarvisCppDocu/47_doc_engine_event_event_h` |
| `engine/event/eventQueue.h` | yes | `doc_engine_event_eventQueue_h` | `../queue/jarvisCppDocu/48_doc_engine_event_eventQueue_h` |
| `engine/event/events.h` | no | `doc_engine_event_events_h` | `../queue/jarvisCppDocu/49_doc_engine_event_events_h` |
| `engine/event/filesystemEvent.h` | no | `doc_engine_event_filesystemEvent_h` | `../queue/jarvisCppDocu/50_doc_engine_event_filesystemEvent_h` |
| `engine/event/keyboardEvent.h` | no | `doc_engine_event_keyboardEvent_h` | `../queue/jarvisCppDocu/51_doc_engine_event_keyboardEvent_h` |
| `engine/event/pythonErrorEvent.h` | no | `doc_engine_event_pythonErrorEvent_h` | `../queue/jarvisCppDocu/52_doc_engine_event_pythonErrorEvent_h` |
| `engine/event/timerEvent.h` | no | `doc_engine_event_timerEvent_h` | `../queue/jarvisCppDocu/53_doc_engine_event_timerEvent_h` |
| `engine/input/keyboardInput.h` | yes | `doc_engine_input_keyboardInput_h` | `../queue/jarvisCppDocu/54_doc_engine_input_keyboardInput_h` |
| `engine/json/configChecker.h` | yes | `doc_engine_json_configChecker_h` | `../queue/jarvisCppDocu/55_doc_engine_json_configChecker_h` |
| `engine/json/configParser.h` | yes | `doc_engine_json_configParser_h` | `../queue/jarvisCppDocu/56_doc_engine_json_configParser_h` |
| `engine/json/jsonHelper.h` | yes | `doc_engine_json_jsonHelper_h` | `../queue/jarvisCppDocu/57_doc_engine_json_jsonHelper_h` |
| `engine/log/log.h` | yes | `doc_engine_log_log_h` | `../queue/jarvisCppDocu/58_doc_engine_log_log_h` |
| `engine/log/terminalLogStreamBuf.h` | no | `doc_engine_log_terminalLogStreamBuf_h` | `../queue/jarvisCppDocu/59_doc_engine_log_terminalLogStreamBuf_h` |
| `engine/log/terminalManager.h` | yes | `doc_engine_log_terminalManager_h` | `../queue/jarvisCppDocu/60_doc_engine_log_terminalManager_h` |

## Up-to-date behavior

The JCWF freshness model is Makefile-like: a task can be skipped as up-to-date only when its declared `file_outputs` exist and are newer than all declared `file_inputs`, and the task’s dependencies are also satisfied (see JC Workflow Spec §3.4).

In this workflow, each AI task declares `file_outputs: ["docu.md"]`, so the runtime will use that output filename (within the task working directory) for up-to-date checks.

## Notes

- This workflow is intentionally **AI-only**: it produces one markdown doc per header (plus optional `.cpp`) under the queue directories.
- If you later want to *combine* the generated per-class docs into a single `combinedDocumentation.md`, add a final `python` task that depends on all AI tasks and consumes their outputs.
