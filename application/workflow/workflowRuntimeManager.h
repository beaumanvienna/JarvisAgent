/* Copyright (c) 2025 JC Technolabs

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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <future>
#include <queue>
#include <unordered_map>
#include <string>
#include <vector>

#include "aiRequestPool.h"
#include "workflow/filter/filterEngine.h"
#include "workflow/filter/filterManifest.h"
#include "workflowTypes.h"

namespace AIAssistant
{
    class WorkflowRegistry;

    // Tick-based workflow runtime.
    //
    // This manager must not block the main thread. It is designed to be called
    // from JarvisAgent::OnUpdate() so filesystem events can still be delivered
    // to JarvisAgent::OnEvent() (required for ai_call completion).
    class WorkflowRuntimeManager final
    {
    public:
        WorkflowRuntimeManager() = default;
        ~WorkflowRuntimeManager();

        WorkflowRuntimeManager(WorkflowRuntimeManager const&) = delete;
        WorkflowRuntimeManager& operator=(WorkflowRuntimeManager const&) = delete;

        void Start();
        void Stop();
        void SignalStop();
        void WaitStop();

        void EnqueueWorkflowRun(std::string const& workflowId);

        // Enqueue a workflow run and return the assigned run id.
        // The run id is stable for the queued run and will match the run id
        // observed by the UI once the run becomes active.
        std::string EnqueueWorkflowRunAndGetRunId(std::string const& workflowId);

        // Integration-friendly enqueue: allow specifying a run id and initial run context.
        // If runId is empty, a run id is generated.
        std::string EnqueueWorkflowRunWithContextAndGetRunId(std::string const& workflowId, std::string const& runId,
                                                             ContextMap const& context);

        // Must be called periodically (from main thread).
        void Update();

        void SetRegistry(WorkflowRegistry const* workflowRegistry);

        bool TryGetLastRun(std::string const& workflowId, WorkflowRun& outRun) const;

        bool TryGetActiveRun(std::string const& runId, WorkflowRun& outRun) const;

        // Returns true if cancellation was requested successfully for an active run.
        bool RequestCancelRun(std::string const& runId);

        // Finds a run by run id across active and last-runs snapshots.
        bool TryGetRunById(std::string const& runId, WorkflowRun& outRun) const;

        // Returns a copy of all active runs (thread-safe snapshot).
        std::vector<WorkflowRun> GetActiveRunsSnapshot() const;

        // Returns a copy of the last completed run per workflow id (thread-safe snapshot).
        std::unordered_map<std::string, WorkflowRun> GetLastRunsSnapshot() const;

        // Deletes all output artifacts produced by running the given workflow.
        // Returns true on success.  On failure, outErrorMessage describes the issue.
        // Source/input files are never touched.
        bool CleanWorkflow(std::string const& workflowId, std::string& outErrorMessage);

        // Heartbeat watchdog: called by the REST endpoint to reset the inactivity timer.
        // Returns true if the task was found and kicked.
        bool Heartbeat(std::string const& taskInstanceId);

    private:
        struct TaskExecutionResult
        {
            std::string m_TaskId;
            bool m_ExecuteOk = false;
            TaskInstanceState m_TaskState;
        };

        struct FilterEvalResult
        {
            std::string m_ParentTaskId;
            bool m_Success{false};
            std::string m_ErrorMessage;
            std::vector<FilterItem> m_Items;
            FilterManifest m_Manifest;
        };

        struct PendingRun
        {
            std::string m_WorkflowId;
            std::string m_RunId;
            ContextMap m_Context;
        };

        struct ActiveRun
        {
            WorkflowDefinition m_Definition;
            WorkflowRun m_Run;

            std::unordered_map<std::string, std::shared_future<TaskExecutionResult>> m_RunningTasks;

            // Per-item fan-out: filter evaluation futures (parent taskId → future)
            std::unordered_map<std::string, std::shared_future<FilterEvalResult>> m_FilterEvalTasks;

            // Per-item fan-out: parent taskId → child instance IDs (e.g. "taskA" → ["taskA#0", "taskA#1"])
            std::unordered_map<std::string, std::vector<std::string>> m_PerItemChildren;

            bool m_CancelRequested{false};
        };

    private:
        void StartPendingRuns(std::vector<PendingRun>&& pendingRuns);

        void TickActiveRun(ActiveRun& activeRun);

        bool IsRunTerminal(ActiveRun const& activeRun) const;

        TaskExecutionResult ExecuteTaskOnWorker(WorkflowDefinition const& workflowDefinition,
                                                WorkflowRun const& workflowRunSnapshot, TaskDef const& taskDefinition,
                                                std::string const& taskId, TaskInstanceState const& taskStateSnapshot) const;

        std::string GenerateRunId(WorkflowDefinition const& workflowDefinition) const;

        void DrainAiRequestCompletions();
        bool TryApplyAiCompletion(AiRequestCompletion const& completion);

        // Per-item fan-out helpers
        FilterDef const* FindFilterDef(WorkflowDefinition const& workflowDef, std::string const& filterId) const;
        void DispatchFilterEvaluation(ActiveRun& activeRun, std::string const& taskId, TaskDef const& taskDef);
        void HarvestFilterEvalCompletions(ActiveRun& activeRun);
        void FanOutPerItemChildren(ActiveRun& activeRun, std::string const& parentTaskId, FilterEvalResult const& evalResult,
                                   TaskDef const& taskDef);
        void AggregatePerItemResults(ActiveRun& activeRun);
        static std::string ParentTaskId(std::string const& instanceId);
        static bool IsChildInstance(std::string const& instanceId);

    private:
        mutable std::mutex m_Mutex;

        WorkflowRegistry const* m_WorkflowRegistry = nullptr;

        bool m_IsRunning = false;
        std::queue<PendingRun> m_PendingRuns;

        std::vector<ActiveRun> m_ActiveRuns;

        std::unordered_map<std::string, WorkflowRun> m_LastRuns;

        std::vector<AiRequestCompletion> m_DeferredAiCompletions;

        // Heartbeat watchdog tracking (taskInstanceId → watchdog)
        mutable std::mutex m_WatchdogMutex;
        std::unordered_map<std::string, std::shared_ptr<TaskWatchdog>> m_ActiveWatchdogs;

        void RegisterWatchdog(std::string const& taskInstanceId, std::shared_ptr<TaskWatchdog> const& watchdog);
        void UnregisterWatchdog(std::string const& taskInstanceId);
    };
} // namespace AIAssistant