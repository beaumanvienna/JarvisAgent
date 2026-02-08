/* Copyright (c) 2025 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"

namespace AIAssistant
{
    // ---------------------------------------------------------------------
    // Workflows → triggers
    // ---------------------------------------------------------------------

    enum class WorkflowTriggerType
    {
        Unknown = 0,
        Auto,
        Cron,
        FileWatch,
        Structure,
        Manual
    };

    // ---------------------------------------------------------------------
    // Task types and modes
    // ---------------------------------------------------------------------

    enum class TaskType
    {
        Unknown = 0,
        Python,
        Shell,
        AiCall,
        Internal
    };

    enum class TaskMode
    {
        Single = 0,
        PerItem
    };

    enum class TaskInstanceStateKind
    {
        Pending = 0,
        Ready,
        Running,
        Skipped,
        Succeeded,
        Failed,

        // New: non-terminal "waiting for filesystem-driven completion" (ai_call)
        WaitingExternal
    };

    // Overall workflow-run state (separate from per-task states)
    enum class WorkflowRunState
    {
        Pending = 0,
        Running,
        Succeeded,
        Failed,
        Cancelled
    };

    // ---------------------------------------------------------------------
    // Context map for workflow runs
    // ---------------------------------------------------------------------

    struct ContextValue
    {
        // For now this is a simple string; it can hold raw JSON if needed.
        std::string m_Value;
    };

    using ContextMap = std::unordered_map<std::string, ContextValue>;

    // ---------------------------------------------------------------------
    // IO Slot Definitions
    // ---------------------------------------------------------------------

    struct TaskIOField
    {
        // Advisory type (string, object, json, etc.) – matches JCWF "type".
        std::string m_Type;
        bool m_IsRequired{false};
    };

    using TaskIOMap = std::unordered_map<std::string, TaskIOField>;

    // ---------------------------------------------------------------------
    // Environment and queue bindings
    // ---------------------------------------------------------------------

    struct TaskEnvironment
    {
        // Logical name for this environment (optional).
        std::string m_Name;

        // For ai_call tasks in assistant mode: JCWF "assistant_id".
        std::string m_AssistantId;

        // Environment variables (shell / python / ai_call).
        std::unordered_map<std::string, std::string> m_Variables;
    };

    // Queue file reference (supports either "path only" or "inline content")
    struct QueueFileRef
    {
        std::string m_Path;
        std::string m_Content;
        bool m_HasInlineContent{false};

        QueueFileRef() = default;
        QueueFileRef(std::string const& path) : m_Path(path) {}

        QueueFileRef(char const* path) : m_Path(path) {}

        std::string const& Path() const { return m_Path; }

        // Compatibility: lets code treat QueueFileRef like a "path string" in many contexts
        operator std::string const&() const { return m_Path; }
    };

    struct QueueBinding
    {
        // STNG_* files (settings / tone).
        std::vector<QueueFileRef> m_StngFiles;

        // TASK_* files (instructions).
        std::vector<QueueFileRef> m_TaskFiles;

        // CNTX_* files (context).
        std::vector<QueueFileRef> m_CntxFiles;

        // PROV_* files (provider configuration — never sent to AI).
        // Written by AiCallTaskExecutor when a task specifies "provider" / "model".
        // Read by SessionManager to resolve endpoint/key/model from KeyManager.

        // PROB_* files (problem / request payload).
        std::vector<QueueFileRef> m_ProbFiles;
    };

    // ---------------------------------------------------------------------
    // Triggers and Dataflow
    // ---------------------------------------------------------------------

    struct WorkflowTrigger
    {
        WorkflowTriggerType m_Type{WorkflowTriggerType::Unknown};
        std::string m_Id;
        bool m_IsEnabled{true};

        // Raw JSON blob of "params" (cron expression, file patterns, etc.).
        std::string m_ParamsJson;
    };

    struct DataflowDef
    {
        // Source task / output.
        std::string m_FromTask;
        std::string m_FromOutput;

        // Target task / input.
        std::string m_ToTask;
        std::string m_ToInput;

        // Optional mapping object from JCWF ("mapping").
        std::unordered_map<std::string, std::string> m_Mapping;
    };

    // ---------------------------------------------------------------------
    // Task definition (static configuration)
    // ---------------------------------------------------------------------

    struct RetryPolicy
    {
        uint32_t m_MaxAttempts{0};
        uint32_t m_BackoffMs{0};
    };

    struct TaskDef
    {
        // JCWF: "id"
        std::string m_Id;

        // JCWF: "type"
        TaskType m_Type{TaskType::Unknown};

        // JCWF: "mode" (single / per_item)
        TaskMode m_Mode{TaskMode::Single};

        // JCWF: "label", "doc"
        std::string m_Label;
        std::string m_Doc;

        // JCWF: "depends_on"
        std::vector<std::string> m_DependsOn; // task IDs

        // JCWF: "working_directory"
        std::string m_WorkingDirectory;

        // JCWF: "file_inputs", "file_outputs"
        std::vector<std::string> m_FileInputs;  // file paths or templates
        std::vector<std::string> m_FileOutputs; // file paths or templates

        // JCWF: "environment"
        TaskEnvironment m_Environment;

        // JCWF: "queue_binding"
        QueueBinding m_QueueBinding;

        // JCWF: "inputs", "outputs" (data slots)
        TaskIOMap m_Inputs;  // runtime input model
        TaskIOMap m_Outputs; // runtime output model

        // JCWF: "timeout_ms"
        uint64_t m_TimeoutMs{0};

        // JCWF: "retries"
        RetryPolicy m_RetryPolicy;

        // Raw JSON for task-specific "params" object.
        std::string m_ParamsJson;
    };

    // Compatibility alias for code that used TaskDefinition naming today
    using TaskDefinition = TaskDef;

    // ---------------------------------------------------------------------
    // Workflow definition (static configuration)
    // ---------------------------------------------------------------------

    struct WorkflowDefinition
    {
        // JCWF: "version"
        std::string m_Version;

        // JCWF: "id", "label", "doc"
        std::string m_Id;
        std::string m_Label;
        std::string m_Doc;

        // Workflow File Path: filesystem path of the loaded .jcwf file (set by the loader).
        std::string m_WorkflowFilePath;

        // Workflow File Directory: directory that contains the loaded .jcwf file (set by the loader).
        std::string m_WorkflowFileDirectory;

        // Workflow Base Directory: base directory used for resolving workflow-level relative paths (set by the loader).
        std::string m_WorkflowBaseDirectory;

        // Workflow File Path Absolute: canonical absolute (normalized) filesystem path of the loaded .jcwf file (set by the
        // loader).
        std::string m_WorkflowFilePathAbsolute;

        // Workflow File Directory Absolute: canonical absolute (normalized) directory containing the loaded .jcwf file (set
        // by the loader).
        std::string m_WorkflowFileDirectoryAbsolute;

        // Workflow Base Directory Absolute: canonical absolute (normalized) base directory used for resolving workflow-level
        // relative paths (set by the loader).
        std::string m_WorkflowBaseDirectoryAbsolute;

        // JCWF: "manual_start" (default: true)
        bool m_ManualStart{true};

        // JCWF: "triggers"
        std::vector<WorkflowTrigger> m_Triggers;

        // JCWF: "tasks" (map from taskId → TaskDef)
        std::unordered_map<std::string, TaskDef> m_Tasks;

        // JCWF: "dataflow"
        std::vector<DataflowDef> m_Dataflows;

        // JCWF: "defaults" – kept as raw JSON; the orchestrator can interpret.
        std::string m_DefaultsJson;
    };

    // ---------------------------------------------------------------------
    // Runtime task state
    // ---------------------------------------------------------------------

    struct TaskInstanceState
    {
        // High-level state (pending, running, succeeded, etc.)
        TaskInstanceStateKind m_State{TaskInstanceStateKind::Pending};

        // How many attempts already made for this instance.
        uint32_t m_AttemptCount{0};

        // Last error message, if any.
        std::string m_LastErrorMessage;

        // ISO-8601 timestamps for UI / logging (may be empty if not set).
        std::string m_StartedAtIso8601;
        std::string m_CompletedAtIso8601;

        // Optional snapshots for debugging / UI:
        // - Inputs as they were resolved at run time.
        // - Outputs as produced by the executor (Python, shell, internal).
        std::string m_InputsJson;
        std::string m_OutputsJson;

        // Resolved input values by logical slot name (e.g. "section_text").
        std::unordered_map<std::string, std::string> m_InputValues;

        // Produced output values by logical slot name (e.g. "markdown_path").
        std::unordered_map<std::string, std::string> m_OutputValues;

        // ai_call correlation (required for event-driven async completion)
        int64_t m_ExternalRequestId{0};
        int64_t m_ExternalRequestTimestampNs{0};
    };

    // ---------------------------------------------------------------------
    // Workflow run (ephemeral, per activation)
    // ---------------------------------------------------------------------

    struct WorkflowRun
    {
        // Unique run identifier and owning workflow id.
        std::string m_RunId;
        std::string m_WorkflowId;

        // Overall run state (pending, running, succeeded, failed, cancelled).
        WorkflowRunState m_State{WorkflowRunState::Pending};

        // Shared run-level context (JCWF "context / state").
        ContextMap m_Context;

        // Per-task instance state (keyed by task instance id, e.g. "task" or "task#item").
        std::unordered_map<std::string, TaskInstanceState> m_TaskStates;

        // Timestamps for the run.
        std::string m_StartedAtIso8601;
        std::string m_CompletedAtIso8601;

        // Internal orchestration flags
        bool m_IsCompleted{false};
        bool m_HasFailed{false};
    };

} // namespace AIAssistant
