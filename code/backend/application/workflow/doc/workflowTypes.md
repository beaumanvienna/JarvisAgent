# Documentation for workflowTypes.h

## Overview
This document provides an overview of the structures, enumerations, and types defined in `workflowTypes.h`, which are used in the JarvisAgent's workflow management.

## Enumerations
### WorkflowTriggerType
Defines various types of workflow triggers:
- **Unknown**: Represents an unknown trigger type.
- **Auto**: Trigger automatically when workflow loaded.
- **Cron**: Trigger based on a cron schedule.
- **FileWatch**: Trigger based on file changes.
- **Structure**: Trigger based on document structure.
- **Manual**: Trigger initiated manually.

### TaskType
Specifies types of tasks:
- **Unknown**: Represents an unknown task type.
- **Python**: Task executed using Python scripts.
- **Shell**: Task executed as a shell command.
- **AiCall**: Task that calls an AI service.
- **Internal**: Internal task executed by the system.

### TaskMode
Indicates modes for task execution:
- **Single**: Task executes once per workflow run.
- **PerItem**: Task executes for each item in an iterator.

### TaskInstanceStateKind
Represents the state of a task instance:
- **Pending**: Task is waiting to be executed.
- **Ready**: Task is ready for execution.
- **Running**: Task is currently executing.
- **Skipped**: Task was skipped due to conditions.
- **Succeeded**: Task completed successfully.
- **Failed**: Task encountered an error during execution.
- **WaitingExternal**: Task is waiting for filesystem-driven external completion (e.g. ai_call).

### WorkflowRunState
Represents the state of a workflow run:
- **Pending**: Workflow is waiting to start.
- **Running**: Workflow is currently executing.
- **Succeeded**: Workflow completed successfully.
- **Failed**: Workflow encountered an error.
- **Cancelled**: Workflow was cancelled by the user.

## Structures
### ContextValue
Represents a value in the context map for workflow runs.
- **m_Value**: Holds the string value of the context.

### TaskIOField
Defines input/output fields for tasks:
- **m_Type**: Advisory type (string, object, etc.).
- **m_IsRequired**: Indicates if the field is required.
- **m_Default**: Optional default value (string). Used as fallback when neither dataflow edges nor run context provide a value for an input.

### TaskEnvironment
Describes the environment for tasks:
- **m_Name**: Logical name of the environment.
- **m_AssistantId**: ID for the AI assistant environment.
- **m_Variables**: Environment variables for the task.

### QueueFileRef
Represents a queue file reference used by `queue_binding` entries.
- **m_Path**: Path to the referenced file.
- **m_Content**: Inline file content (when provided).
- **m_HasInlineContent**: Indicates whether `m_Content` is populated.

### QueueBinding
Specifies files related to task execution:
- **m_StngFiles**: STNG settings files.
- **m_TaskFiles**: TASK instruction files.
- **m_CntxFiles**: CNTX context files.
- **m_ProbFiles**: PROB problem/request files.

### WorkflowTrigger
Represents a trigger with its type and parameters:
- **m_Type**: Type of the trigger.
- **m_Id**: Unique identifier for the trigger.
- **m_IsEnabled**: Indicates if the trigger is enabled.
- **m_ParamsJson**: Raw JSON blob of parameters.

### DataflowDef
Defines the data flow between tasks:
- **m_FromTask**: Source task.
- **m_FromOutput**: Output from the source task.
- **m_ToTask**: Target task.
- **m_ToInput**: Input for the target task.
- **m_Mapping**: Optional mapping from the JCWF.

### TaskDef
Represents the static configuration of a task:
- **m_Id**: Unique identifier for the task.
- **m_Type**: Type of the task.
- **m_Mode**: Mode of execution.
- **m_Label**: Human-readable label for the task.
- **m_Doc**: Documentation string for the task.
- **m_DependsOn**: List of task IDs that this task depends on.
- **m_WorkingDirectory**: Task working directory (JCWF: `working_directory`).
- **m_FileInputs**: List of input files.
- **m_FileOutputs**: List of output files.
- **m_Environment**: Environment configuration for the task.
- **m_QueueBinding**: Queue binding information.
- **m_Inputs**: Input fields for the task.
- **m_Outputs**: Output fields for the task.
- **m_TimeoutMs**: Timeout for the task execution.
- **m_RetryPolicy**: Retry policy for the task.
- **m_ParamsJson**: Raw JSON for task-specific parameters.

### WorkflowDefinition
Describes the overall workflow:
- **m_Version**: Version of the workflow.
- **m_Id**: Unique identifier for the workflow.
- **m_Label**: Human-readable label for the workflow.
- **m_Doc**: Documentation string for the workflow.
- **m_WorkflowFilePath**: Workflow File Path: filesystem path of the loaded .jcwf file (set by the loader).
- **m_WorkflowFileDirectory**: Workflow File Directory: directory that contains the loaded .jcwf file (set by the loader).
- **m_WorkflowBaseDirectory**: Workflow Base Directory: base directory used for resolving workflow-level relative paths (set by the loader).
- **m_ManualStart**: Controls whether the workflow can be started manually via UI or CLI (default: `true`). When `false`, the workflow can only be started by its defined triggers.
- **m_Triggers**: List of triggers for the workflow.
- **m_Tasks**: Map of task definitions.
- **m_Dataflows**: List of data flows.
- **m_DefaultsJson**: Default settings in JSON format.

### TaskInstanceState
Represents the runtime state of a task instance:
- **m_State**: Current state of the task instance.
- **m_AttemptCount**: Number of attempts made.
- **m_LastErrorMessage**: Last error message, if any.
- **m_StartedAtIso8601**: Start timestamp in ISO 8601 format.
- **m_CompletedAtIso8601**: Completion timestamp in ISO 8601 format.
- **m_InputsJson**: Inputs resolved at runtime.
- **m_OutputsJson**: Outputs produced by the executor.
- **m_InputValues**: Resolved input values by logical slot name.
- **m_OutputValues**: Produced output values by logical slot name.
- **m_ExternalRequestId**: AI-call correlation request id (for event-driven completion).
- **m_ExternalRequestTimestampNs**: AI-call correlation timestamp (nanoseconds).

### WorkflowRun
Represents a workflow run:
- **m_RunId**: Unique identifier for the run.
- **m_WorkflowId**: ID of the associated workflow.
- **m_State**: Current state of the workflow run.
- **m_Context**: Shared context for the run.
- **m_TaskStates**: States of task instances.
- **m_StartedAtIso8601**: Start timestamp.
- **m_CompletedAtIso8601**: Completion timestamp.
- **m_IsCompleted**: Indicates if the run is completed.
- **m_HasFailed**: Indicates if the run has failed.

## Conclusion
This document summarizes the key components of the `workflowTypes.h` file, providing a structured overview of the types and enumerations used in the JarvisAgent workflow management system.
