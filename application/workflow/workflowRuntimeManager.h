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

#include <chrono>
#include <functional>
#include <future>
#include <queue>
#include <unordered_map>
#include <unordered_set>
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
        // Returns true if any workflow run changed state (completed, failed, new run started).
        bool Update();

        void SetRegistry(WorkflowRegistry const* workflowRegistry);

        bool TryGetLastRun(std::string const& workflowId, WorkflowRun& outRun) const;

        bool TryGetActiveRun(std::string const& runId, WorkflowRun& outRun) const;

        // Returns true if cancellation was requested successfully for an active run.
        bool RequestCancelRun(std::string const& runId);

        // Returns true if the pause/resume/stop was applied to an active run.
        bool RequestPauseRun(std::string const& runId);
        bool RequestResumeRun(std::string const& runId);
        bool RequestStopRun(std::string const& runId);

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

        void GetRunCounters(uint64_t& outCompleted, uint64_t& outFailed) const;

        // Heartbeat watchdog: called by the REST endpoint to reset the inactivity timer.
        // Returns true if the task was found and kicked.
        bool Heartbeat(std::string const& taskInstanceId);

        // Sub-workflow support: register a parent-child link so the runtime manager
        // can propagate child run completion back to the parent task.
        void RegisterSubWorkflowLink(std::string const& childRunId, std::string const& parentRunId,
                                     std::string const& parentTaskInstanceId);

        // Optional observer invoked each time a run reaches a terminal state.
        // Used by AdhocWorkflowManager to clean up `on_completion` folders.
        using RunTerminalObserver = std::function<void(std::string const& runId, WorkflowRunState finalState)>;
        void SetRunTerminalObserver(RunTerminalObserver observer);

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

            // Controlflow runtime (branching)
            std::unordered_set<std::string> m_ActivatedTasks; // task IDs eligible to run (controlflow-gated)
            std::unordered_set<std::string> m_TasksWithIncomingControlflow;
            std::unordered_set<std::string> m_HandledFailureTasks; // tasks whose failure is handled by a branch

            std::unordered_set<std::string> m_FiredBranches;

            // branchId -> driving taskId
            std::unordered_map<std::string, std::string> m_BranchDrivingTask;
            // branchId -> tasks activated on normal output
            std::unordered_map<std::string, std::vector<std::string>> m_BranchNormalTargets;
            // branchId -> tasks activated on error output
            std::unordered_map<std::string, std::vector<std::string>> m_BranchOnErrorTargets;

            bool m_CancelRequested{false};
            bool m_PauseRequested{false};
            bool m_StopRequested{false};

            // Count of ai_call tasks this run has dispatched so far. Enforced against
            // EngineConfig::m_MaxAiCallsPerJcwf (0 = no cap) in the dispatch path.
            // Single-threaded access from the tick loop — no atomic needed.
            size_t m_AiCallsDispatched{0};

            // Steady-clock start time for the 2-second minimum-visibility hold.
            // Sub-second runs (common for adhoc) would otherwise flash through m_ActiveRuns
            // faster than the dashboard's snapshot broadcast can deliver them.
            std::chrono::steady_clock::time_point m_StartedAtSteady{
                std::chrono::steady_clock::now()};
        };

    private:
        void StartPendingRuns(std::vector<PendingRun>&& pendingRuns);

        void TickActiveRun(ActiveRun& activeRun);

        // Controlflow (branching)
        void InitializeControlflowRuntime(ActiveRun& activeRun);
        void FireBranchIfReady(ActiveRun& activeRun, std::string const& completedInstanceId,
                               TaskInstanceStateKind completedState);
        void SkipAllInstancesOfTask(WorkflowRun& workflowRun, std::string const& taskId, std::string const& message);

        bool IsRunTerminal(ActiveRun const& activeRun) const;

        TaskExecutionResult ExecuteTaskOnWorker(WorkflowDefinition const& workflowDefinition,
                                                WorkflowRun const& workflowRunSnapshot, TaskDef const& taskDefinition,
                                                std::string const& taskId, TaskInstanceState const& taskStateSnapshot) const;

        std::string GenerateRunId(WorkflowDefinition const& workflowDefinition) const;

        // Returns true if all AI provider prerequisites are satisfied for this workflow.
        // If the workflow has no ai_call tasks, returns true unconditionally.
        bool CheckAiProviderPrerequisites(WorkflowDefinition const& workflowDefinition) const;

        void DrainAiRequestCompletions();
        bool TryApplyAiCompletion(AiRequestCompletion const& completion);

        // Failed-dependency propagation: skip all tasks that (transitively) depend on a failed task.
        void SkipDownstreamOfFailed(ActiveRun& activeRun, std::string const& failedTaskId);

        // Safety-net timeout for WaitingExternal tasks (in case AiRequestPool timeout didn't fire).
        void TimeoutWaitingExternalTasks(ActiveRun& activeRun);

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

        uint64_t m_TotalCompletedRuns{0};
        uint64_t m_TotalFailedRuns{0};

        // Optional one-shot observer fired on terminal state transitions. Set by WebServer
        // at startup to let AdhocWorkflowManager clean up `on_completion` runs inline.
        RunTerminalObserver m_RunTerminalObserver;

        std::vector<AiRequestCompletion> m_DeferredAiCompletions;

        // Heartbeat watchdog tracking (taskInstanceId → watchdog)
        mutable std::mutex m_WatchdogMutex;
        std::unordered_map<std::string, std::shared_ptr<TaskWatchdog>> m_ActiveWatchdogs;

        void RegisterWatchdog(std::string const& taskInstanceId, std::shared_ptr<TaskWatchdog> const& watchdog);
        void UnregisterWatchdog(std::string const& taskInstanceId);

        // Sub-workflow parent-child tracking.
        struct SubWorkflowLink
        {
            std::string m_ParentRunId;
            std::string m_ParentTaskInstanceId;
        };

        std::unordered_map<std::string, SubWorkflowLink> m_SubWorkflowLinks; // childRunId → link

        void PropagateSubWorkflowCompletions();
        void CancelChildSubWorkflowRuns(std::string const& parentRunId);
    };
} // namespace AIAssistant