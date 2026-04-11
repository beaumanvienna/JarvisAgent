/* Copyright (c) 2026 JC Technolabs
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"

namespace AIAssistant
{
    // ---------------------------------------------------------------------
    // TaskWatchdog — inactivity-based timeout for running tasks
    // ---------------------------------------------------------------------
    // Shared between the executor (implicit heartbeat on stdout / progress)
    // and the REST endpoint (explicit heartbeat from task code).
    // Thread-safe: all access through atomics.

    struct TaskWatchdog
    {
        void Kick() { m_LastActivityMs.store(NowMs(), std::memory_order_release); }

        int64_t ElapsedSinceLastKickMs() const { return NowMs() - m_LastActivityMs.load(std::memory_order_acquire); }

        std::atomic<int64_t> m_LastActivityMs{0};

    private:
        static int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    };

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
        Manual,
        Webhook,
        S3Watch,
        OneDriveWatch,
        EmailWatch
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
        Internal,
        SubWorkflow,
        PolarionWrite,
        S3,
        DbQuery,
        OneDrive,
        SnowflakeQuery,
        SlackMessage,
        EmailSend
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
        Paused,
        Stopping,
        Succeeded,
        Failed,
        Cancelled,
        Stopped
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

        // Optional default value (JCWF "default" field on the input declaration).
        // Used as fallback when neither dataflow nor run context provides a value.
        std::string m_Default;
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
    // Filter definitions (v1.1)
    // ---------------------------------------------------------------------

    struct FilterSource
    {
        // Source kind: "csv", "text_lines", "query", "polarion_query"
        std::string m_Kind;

        // csv / text_lines: source file path (resolved relative to Workflow Base Directory)
        std::string m_Path;

        // csv only
        std::string m_Delimiter{","};
        bool m_HasHeader{true};
        std::string m_Range; // e.g. "10-20", "5-", "-50"

        // text_lines only
        bool m_SkipEmpty{true};

        // query only: path to query index
        std::string m_IndexPath;

        // query / polarion_query: query expression
        std::string m_Query;

        // query / polarion_query: fields to extract per hit
        std::vector<std::string> m_Fields;

        // polarion_query only
        std::string m_Connection; // Named CloudConnection (preferred over inline base_url/project_id/key_name)
        std::string m_BaseUrl;
        std::string m_ProjectId;
        std::string m_KeyName; // KeyManager credential name (never stored in JCWF)
        uint32_t m_PageSize{100};
    };

    struct FilterDef
    {
        std::string m_Id;
        FilterSource m_Source;
        std::string m_Binding;      // prefix for injected inputs (e.g. "item", "row")
        uint32_t m_MaxItems{10000}; // 0 = unlimited
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

        // JCWF: "materialize" — copy file_inputs into working_directory under specific names.
        // Key: source path template (e.g. "{{input[0]}}"), Value: target filename (e.g. "hello.c").
        std::vector<std::pair<std::string, std::string>> m_Materialize;

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

        // JCWF v1.1: "filter" — references a filter ID for per_item expansion.
        std::string m_Filter;

        // JCWF: "expose_error_signal" (optional)
        bool m_ExposeErrorSignal{false};

        // JCWF: "workflow_file" — for SubWorkflow tasks, path to child .jcwf file
        // (relative to the workflow file directory or absolute).
        std::string m_WorkflowFile;
    };

    // Compatibility alias for code that used TaskDefinition naming today
    using TaskDefinition = TaskDef;

    // ---------------------------------------------------------------------
    // Workflow-level defaults (applied to tasks that don't override them)
    // ---------------------------------------------------------------------

    struct WorkflowDefaults
    {
        uint64_t m_TimeoutMs{0};
        RetryPolicy m_RetryPolicy;
    };

    // ---------------------------------------------------------------------
    // Control-flow graph extensions (branching)
    // ---------------------------------------------------------------------

    enum class ControlNodeType
    {
        Branch = 0,
        Unknown
    };

    struct ControlNodeDef
    {
        // JCWF: "id"
        std::string m_Id;

        // JCWF: "type" (currently only "branch")
        ControlNodeType m_Type{ControlNodeType::Unknown};

        // JCWF: "label" (optional)
        std::string m_Label;
    };

    enum class ControlflowKind
    {
        Normal = 0,
        ErrorSignal,
        OnError,
        Unknown
    };

    struct ControlflowEdgeDef
    {
        // JCWF: "from", "to"
        std::string m_From;
        std::string m_To;

        // JCWF: "kind" (normal | error_signal | on_error)
        ControlflowKind m_Kind{ControlflowKind::Unknown};

        // JCWF: "from_port", "to_port" (opaque strings used by the editor)
        std::string m_FromPort;
        std::string m_ToPort;
    };

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

        // JCWF v1.1: "filters" — filter definitions for per_item expansion.
        std::vector<FilterDef> m_Filters;

        // JCWF: "control_nodes" — orchestration-only nodes (e.g. branch)
        std::vector<ControlNodeDef> m_ControlNodes;

        // JCWF: "controlflow" — conditional edges between tasks/control nodes
        std::vector<ControlflowEdgeDef> m_ControlflowEdges;

        // JCWF: "defaults" – raw JSON kept for serialization; parsed fields in m_Defaults.
        std::string m_DefaultsJson;
        WorkflowDefaults m_Defaults;

        // Container format: path to the .jcwf zip container (if loaded from one).
        std::string m_ContainerPath;

        // Container format: true if this is a sub-workflow within a container (not the root level).
        bool m_IsSubWorkflow{false};

        // Container format: parent workflow ID (for sub-workflows loaded from a container).
        std::string m_ParentWorkflowId;

        // Container format: relative folder path within the container (e.g. "code validation").
        std::string m_ContainerFolderPath;

        // Computed at load time: true if any task in the workflow is an ai_call.
        bool m_HasAiCallTasks{false};

        // Computed at load time: the resolved provider name for each ai_call task.
        // Empty string means "system default provider".  Checked at enqueue time
        // to verify that every required provider is present in the registry.
        std::vector<std::string> m_RequiredAiProviders;
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

        // Retry backoff: if non-zero, the task should not be re-dispatched until this time.
        std::chrono::steady_clock::time_point m_RetryAfterTime{};

        // Last error message, if any.
        std::string m_LastErrorMessage;

        // Captured shell task output (first 1024 characters of stdout/stderr).
        // Written to stdout.txt / stderr.txt in the task working directory (full size).
        std::string m_CapturedStdout;
        std::string m_CapturedStderr;

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

        // Per-item child instance ID (e.g. "lookupDividend#0"). Set by the runtime
        // before calling Execute() so the executor can use it for request pool binding.
        // Empty for single-mode tasks (executor falls back to TaskDef.m_Id).
        std::string m_TaskInstanceId;

        // ai_call correlation (required for event-driven async completion)
        int64_t m_ExternalRequestId{0};
        int64_t m_ExternalRequestTimestampNs{0};

        // Timestamp for when the task entered WaitingExternal (used for timeout enforcement).
        std::chrono::steady_clock::time_point m_WaitingExternalSince{};

        // Inactivity watchdog (shared with WRM and REST heartbeat endpoint).
        // Executors kick this on progress (stdout lines, loop iterations, etc.).
        std::shared_ptr<TaskWatchdog> m_Watchdog;
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
