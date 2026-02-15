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

#include "workflow/workflowRuntimeManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "core.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "workflow/dataflowResolver.h"
#include "workflow/taskExecutorRegistry.h"
#include "workflow/taskFreshnessChecker.h"
#include "workflow/taskPathResolver.h"
#include "workflow/workflowRegistry.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {

        std::string GetIso8601NowUTC()
        {
            auto const now = std::chrono::system_clock::now();
            std::time_t const nowTimeT = std::chrono::system_clock::to_time_t(now);

            std::tm utcTime{};
#if defined(_WIN32)
            gmtime_s(&utcTime, &nowTimeT);
#else
            gmtime_r(&nowTimeT, &utcTime);
#endif

            std::ostringstream stream;
            stream << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
            return stream.str();
        }

        void PopulateSkippedTaskOutputsIfPossible(WorkflowDefinition const& workflowDefinition,
                                                  WorkflowRun const& workflowRun, TaskDef const& taskDefinition,
                                                  std::string const& taskId, TaskInstanceState& taskState)
        {
            std::vector<fs::path> unusedInputPaths;
            std::vector<fs::path> resolvedOutputPaths;

            if (!TaskPathResolver::ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                                unusedInputPaths, resolvedOutputPaths))
            {
                return;
            }

            if (taskDefinition.m_Outputs.empty() || resolvedOutputPaths.empty())
            {
                return;
            }

            std::vector<std::string> outputSlotNames;
            outputSlotNames.reserve(taskDefinition.m_Outputs.size());

            for (auto const& outputPair : taskDefinition.m_Outputs)
            {
                outputSlotNames.push_back(outputPair.first);
            }

            std::sort(outputSlotNames.begin(), outputSlotNames.end());

            if (outputSlotNames.size() == resolvedOutputPaths.size())
            {
                for (size_t index = 0; index < outputSlotNames.size(); ++index)
                {
                    taskState.m_OutputValues[outputSlotNames[index]] = resolvedOutputPaths[index].string();
                }
            }
            else if (resolvedOutputPaths.size() == 1)
            {
                std::string const onlyPath = resolvedOutputPaths[0].string();
                for (std::string const& slotName : outputSlotNames)
                {
                    taskState.m_OutputValues[slotName] = onlyPath;
                }
            }
            else if (outputSlotNames.size() == 1)
            {
                taskState.m_OutputValues[outputSlotNames[0]] = resolvedOutputPaths[0].string();
            }
            else
            {
                return;
            }

            {
                std::string summary;
                for (auto const& p : taskState.m_OutputValues)
                {
                    summary += p.first;
                    summary += "=";
                    summary += p.second;
                    summary += ";";
                }
                taskState.m_OutputsJson = summary;
            }
        }

        bool IsTerminal(TaskInstanceStateKind const state)
        {
            return (state == TaskInstanceStateKind::Succeeded || state == TaskInstanceStateKind::Skipped ||
                    state == TaskInstanceStateKind::Failed);
        }

        // Returns true if the task was rescheduled for retry (caller should NOT mark run as failed).
        // Returns false if retries are exhausted or the policy has no retries configured.
        bool TryScheduleRetry(TaskInstanceState& taskState, TaskDef const& taskDef, std::string const& taskId,
                              std::string const& runId)
        {
            RetryPolicy const& policy = taskDef.m_RetryPolicy;

            if (policy.m_MaxAttempts == 0)
            {
                return false;
            }

            if (taskState.m_AttemptCount >= policy.m_MaxAttempts)
            {
                LOG_APP_WARN("[retry] task '{}' in run '{}' exhausted all {} retries: {}", taskId, runId,
                             policy.m_MaxAttempts, taskState.m_LastErrorMessage);
                return false;
            }

            uint32_t const backoffMs = policy.m_BackoffMs * (taskState.m_AttemptCount + 1);

            taskState.m_State = TaskInstanceStateKind::Pending;
            taskState.m_RetryAfterTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoffMs);

            LOG_APP_INFO("[retry] task '{}' in run '{}' scheduled for retry (attempt {}/{}, backoff {}ms): {}", taskId,
                         runId, taskState.m_AttemptCount + 1, policy.m_MaxAttempts, backoffMs, taskState.m_LastErrorMessage);

            return true;
        }

        bool IsTaskReady(WorkflowRun const& workflowRun, TaskDef const& taskDefinition)
        {
            for (std::string const& dependencyId : taskDefinition.m_DependsOn)
            {
                auto iterator = workflowRun.m_TaskStates.find(dependencyId);
                if (iterator == workflowRun.m_TaskStates.end())
                {
                    return false;
                }

                TaskInstanceStateKind const dependencyState = iterator->second.m_State;
                if (dependencyState != TaskInstanceStateKind::Succeeded && dependencyState != TaskInstanceStateKind::Skipped)
                {
                    return false;
                }
            }

            return true;
        }

        std::unordered_map<std::string, TaskInstanceState>
        BuildInitialTaskStates(WorkflowDefinition const& workflowDefinition)
        {
            std::unordered_map<std::string, TaskInstanceState> taskStates;
            taskStates.reserve(workflowDefinition.m_Tasks.size());

            for (auto const& taskPair : workflowDefinition.m_Tasks)
            {
                TaskInstanceState state;
                state.m_State = TaskInstanceStateKind::Pending;
                state.m_AttemptCount = 0;
                state.m_LastErrorMessage.clear();
                state.m_InputValues.clear();
                state.m_OutputValues.clear();
                state.m_InputsJson.clear();
                state.m_OutputsJson.clear();

                taskStates.emplace(taskPair.first, std::move(state));
            }

            return taskStates;
        }

    } // namespace

    WorkflowRuntimeManager::~WorkflowRuntimeManager() { Stop(); }

    void WorkflowRuntimeManager::SetRegistry(WorkflowRegistry const* workflowRegistry)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_WorkflowRegistry = workflowRegistry;
    }

    void WorkflowRuntimeManager::Start()
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_IsRunning = true;
    }

    void WorkflowRuntimeManager::Stop()
    {
        SignalStop();
        WaitStop();
    }

    void WorkflowRuntimeManager::SignalStop()
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        m_IsRunning = false;

        std::queue<PendingRun> emptyQueue;
        m_PendingRuns.swap(emptyQueue);

        for (auto& activeRun : m_ActiveRuns)
        {
            activeRun.m_CancelRequested = true;
        }
    }

    void WorkflowRuntimeManager::WaitStop()
    {
        std::vector<std::shared_future<TaskExecutionResult>> taskFutures;
        std::vector<std::shared_future<FilterEvalResult>> filterFutures;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            for (auto& activeRun : m_ActiveRuns)
            {
                for (auto& [id, future] : activeRun.m_RunningTasks)
                {
                    taskFutures.push_back(future);
                }
                for (auto& [id, future] : activeRun.m_FilterEvalTasks)
                {
                    filterFutures.push_back(future);
                }
            }
        }

        for (auto& f : taskFutures)
        {
            if (f.valid())
            {
                f.wait_for(std::chrono::milliseconds(500));
            }
        }
        for (auto& f : filterFutures)
        {
            if (f.valid())
            {
                f.wait_for(std::chrono::milliseconds(500));
            }
        }

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_ActiveRuns.clear();
            m_DeferredAiCompletions.clear();
        }
    }

    bool WorkflowRuntimeManager::Heartbeat(std::string const& taskInstanceId)
    {
        std::lock_guard<std::mutex> lock(m_WatchdogMutex);
        auto it = m_ActiveWatchdogs.find(taskInstanceId);
        if (it == m_ActiveWatchdogs.end())
        {
            return false;
        }
        it->second->Kick();
        return true;
    }

    void WorkflowRuntimeManager::RegisterWatchdog(std::string const& taskInstanceId,
                                                  std::shared_ptr<TaskWatchdog> const& watchdog)
    {
        std::lock_guard<std::mutex> lock(m_WatchdogMutex);
        m_ActiveWatchdogs[taskInstanceId] = watchdog;
    }

    void WorkflowRuntimeManager::UnregisterWatchdog(std::string const& taskInstanceId)
    {
        std::lock_guard<std::mutex> lock(m_WatchdogMutex);
        m_ActiveWatchdogs.erase(taskInstanceId);
    }

    void WorkflowRuntimeManager::EnqueueWorkflowRun(std::string const& workflowId)
    {
        (void)EnqueueWorkflowRunAndGetRunId(workflowId);
    }

    std::string WorkflowRuntimeManager::EnqueueWorkflowRunAndGetRunId(std::string const& workflowId)
    {
        if (workflowId.empty())
        {
            return std::string();
        }

        WorkflowRegistry const* workflowRegistry = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
        }

        std::string runId;
        if (workflowRegistry != nullptr)
        {
            std::optional<WorkflowDefinition> const workflowDefinition = workflowRegistry->GetWorkflow(workflowId);
            if (workflowDefinition.has_value())
            {
                runId = GenerateRunId(workflowDefinition.value());
            }
        }

        if (runId.empty())
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            runId = workflowId + "_" + std::to_string(millis);
        }

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_PendingRuns.push(PendingRun{workflowId, runId, ContextMap{}});
        }

        return runId;
    }

    std::string WorkflowRuntimeManager::EnqueueWorkflowRunWithContextAndGetRunId(std::string const& workflowId,
                                                                                 std::string const& runId,
                                                                                 ContextMap const& context)
    {
        if (workflowId.empty())
        {
            return std::string();
        }

        WorkflowRegistry const* workflowRegistry = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
        }

        std::string resolvedRunId = runId;
        if (resolvedRunId.empty())
        {
            if (workflowRegistry != nullptr)
            {
                std::optional<WorkflowDefinition> const workflowDefinition = workflowRegistry->GetWorkflow(workflowId);
                if (workflowDefinition.has_value())
                {
                    resolvedRunId = GenerateRunId(workflowDefinition.value());
                }
            }
        }

        if (resolvedRunId.empty())
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            resolvedRunId = workflowId + "_" + std::to_string(millis);
        }

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_PendingRuns.push(PendingRun{workflowId, resolvedRunId, context});
        }

        return resolvedRunId;
    }

    bool WorkflowRuntimeManager::TryGetLastRun(std::string const& workflowId, WorkflowRun& outRun) const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        auto iterator = m_LastRuns.find(workflowId);
        if (iterator == m_LastRuns.end())
        {
            return false;
        }

        outRun = iterator->second;
        return true;
    }

    void WorkflowRuntimeManager::DrainAiRequestCompletions()
    {
        JarvisAgent* app = App::g_App;
        AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

        if (requestPool == nullptr)
        {
            return;
        }

        if (!m_DeferredAiCompletions.empty())
        {
            std::vector<AiRequestCompletion> stillDeferred;
            stillDeferred.reserve(m_DeferredAiCompletions.size());

            for (AiRequestCompletion const& completion : m_DeferredAiCompletions)
            {
                if (!TryApplyAiCompletion(completion))
                {
                    stillDeferred.push_back(completion);
                }
            }

            m_DeferredAiCompletions = std::move(stillDeferred);
        }

        AiRequestCompletion completion;

        while (requestPool->TryPopCompletion(completion))
        {
            if (!TryApplyAiCompletion(completion))
            {
                if (m_DeferredAiCompletions.size() < 256)
                {
                    m_DeferredAiCompletions.push_back(std::move(completion));
                }
                else
                {
                    LOG_APP_WARN("[WorkflowRuntimeManager] dropping deferred ai_call completion (queue full): wf='{}' "
                                 "run='{}' task='{}'",
                                 completion.m_WorkflowId, completion.m_RunId, completion.m_TaskId);
                }
            }
        }
    }

    bool WorkflowRuntimeManager::TryApplyAiCompletion(AiRequestCompletion const& completion)
    {
        for (ActiveRun& activeRun : m_ActiveRuns)
        {
            if (activeRun.m_Run.m_WorkflowId != completion.m_WorkflowId)
            {
                continue;
            }

            if (activeRun.m_Run.m_RunId != completion.m_RunId)
            {
                continue;
            }

            auto stateIterator = activeRun.m_Run.m_TaskStates.find(completion.m_TaskId);
            if (stateIterator == activeRun.m_Run.m_TaskStates.end())
            {
                return true;
            }

            TaskInstanceState& taskState = stateIterator->second;

            bool retryScheduled = false;

            if (completion.m_WasFailed)
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage =
                    completion.m_ErrorMessage.empty() ? "ai_call failed" : completion.m_ErrorMessage;

                std::string const parentId = ParentTaskId(completion.m_TaskId);
                auto defIt = activeRun.m_Definition.m_Tasks.find(parentId);
                if (defIt != activeRun.m_Definition.m_Tasks.end() &&
                    TryScheduleRetry(taskState, defIt->second, completion.m_TaskId, activeRun.m_Run.m_RunId))
                {
                    retryScheduled = true;
                }
                else
                {
                    activeRun.m_Run.m_HasFailed = true;
                }
            }
            else
            {
                taskState.m_State = TaskInstanceStateKind::Succeeded;
                taskState.m_LastErrorMessage.clear();
            }

            // Forget the old request handle before potentially clearing correlation IDs.
            {
                JarvisAgent* app = App::g_App;
                AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

                if (requestPool != nullptr)
                {
                    AiRequestHandle requestHandle{};
                    requestHandle.requestId = taskState.m_ExternalRequestId;
                    requestHandle.requestTimestampNs = taskState.m_ExternalRequestTimestampNs;

                    if (requestHandle.IsValid())
                    {
                        requestPool->Forget(requestHandle);
                    }
                }
            }

            if (retryScheduled)
            {
                // Clear external request correlation so the next attempt registers fresh.
                taskState.m_ExternalRequestId = 0;
                taskState.m_ExternalRequestTimestampNs = 0;
                taskState.m_OutputValues.clear();
                taskState.m_OutputsJson.clear();
            }
            else
            {
                taskState.m_OutputValues = completion.m_OutputValues;

                std::string summary;
                for (auto const& p : taskState.m_OutputValues)
                {
                    summary += p.first;
                    summary += "=";
                    summary += p.second;
                    summary += ";";
                }
                taskState.m_OutputsJson = summary;
            }

            return true;
        }

        auto lastIterator = m_LastRuns.find(completion.m_WorkflowId);
        if (lastIterator != m_LastRuns.end())
        {
            if (lastIterator->second.m_RunId == completion.m_RunId)
            {
                return true;
            }
        }

        // If the run is already completed (e.g. cancelled), drop the completion instead of deferring forever.
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            auto lastIt = m_LastRuns.find(completion.m_WorkflowId);
            if (lastIt != m_LastRuns.end())
            {
                if (lastIt->second.m_RunId == completion.m_RunId)
                {
                    return true;
                }
            }
        }

        return false;
    }

    void WorkflowRuntimeManager::Update()
    {
        std::vector<PendingRun> pendingToStart;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            if (!m_IsRunning)
            {
                return;
            }

            while (!m_PendingRuns.empty())
            {
                pendingToStart.push_back(std::move(m_PendingRuns.front()));
                m_PendingRuns.pop();
            }
        }

        if (!pendingToStart.empty())
        {
            StartPendingRuns(std::move(pendingToStart));
        }

        DrainAiRequestCompletions();

        for (size_t index = 0; index < m_ActiveRuns.size();)
        {
            TickActiveRun(m_ActiveRuns[index]);

            if (m_ActiveRuns[index].m_Run.m_IsCompleted)
            {
                {
                    std::scoped_lock<std::mutex> const lock(m_Mutex);
                    m_LastRuns[m_ActiveRuns[index].m_Run.m_WorkflowId] = m_ActiveRuns[index].m_Run;
                }

                m_ActiveRuns.erase(m_ActiveRuns.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }

            ++index;
        }
    }
    void WorkflowRuntimeManager::StartPendingRuns(std::vector<PendingRun>&& pendingRuns)
    {
        WorkflowRegistry const* workflowRegistry = nullptr;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
        }

        if (workflowRegistry == nullptr)
        {
            return;
        }

        for (PendingRun const& pendingRun : pendingRuns)
        {
            if (pendingRun.m_WorkflowId.empty())
            {
                continue;
            }

            std::optional<WorkflowDefinition> workflowDefinition = workflowRegistry->GetWorkflow(pendingRun.m_WorkflowId);
            if (!workflowDefinition.has_value())
            {
                LOG_APP_WARN(
                    "WorkflowRuntimeManager::StartPendingRuns: workflow '{}' not found in registry, skipping run '{}'",
                    pendingRun.m_WorkflowId, pendingRun.m_RunId);
                continue;
            }

            std::string const runId =
                pendingRun.m_RunId.empty() ? GenerateRunId(workflowDefinition.value()) : pendingRun.m_RunId;

            ActiveRun activeRun;
            activeRun.m_Definition = workflowDefinition.value();
            activeRun.m_Run.m_RunId = runId;
            activeRun.m_Run.m_WorkflowId = pendingRun.m_WorkflowId;
            activeRun.m_Run.m_Context = pendingRun.m_Context;
            activeRun.m_Run.m_TaskStates = BuildInitialTaskStates(activeRun.m_Definition);

            {
                std::scoped_lock<std::mutex> const lock(m_Mutex);
                m_ActiveRuns.push_back(std::move(activeRun));
            }
        }
    }

    void WorkflowRuntimeManager::TickActiveRun(ActiveRun& activeRun)
    {
        WorkflowDefinition const& workflowDefinition = activeRun.m_Definition;
        WorkflowRun& workflowRun = activeRun.m_Run;

        // ---------------------------------------------------------
        // 0) Harvest filter evaluation completions + aggregate per-item results
        // ---------------------------------------------------------
        HarvestFilterEvalCompletions(activeRun);
        AggregatePerItemResults(activeRun);

        // ---------------------------------------------------------
        // 1) Harvest completed worker tasks (non-blocking)
        // ---------------------------------------------------------
        for (auto iterator = activeRun.m_RunningTasks.begin(); iterator != activeRun.m_RunningTasks.end();)
        {
            std::shared_future<TaskExecutionResult>& future = iterator->second;

            if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                ++iterator;
                continue;
            }

            TaskExecutionResult result;
            bool gotResult = false;

            try
            {
                result = future.get();
                gotResult = true;
            }
            catch (...)
            {
                gotResult = false;
            }

            if (!gotResult)
            {
                auto stateIterator = workflowRun.m_TaskStates.find(iterator->first);
                if (stateIterator != workflowRun.m_TaskStates.end())
                {
                    stateIterator->second.m_State = TaskInstanceStateKind::Failed;
                    stateIterator->second.m_LastErrorMessage = "task future threw";

                    std::string const parentId = ParentTaskId(iterator->first);
                    auto defIt = workflowDefinition.m_Tasks.find(parentId);
                    if (defIt == workflowDefinition.m_Tasks.end() ||
                        !TryScheduleRetry(stateIterator->second, defIt->second, iterator->first, workflowRun.m_RunId))
                    {
                        workflowRun.m_HasFailed = true;
                    }
                }
                else
                {
                    workflowRun.m_HasFailed = true;
                }
            }
            else
            {
                auto stateIterator = workflowRun.m_TaskStates.find(result.m_TaskId);
                if (stateIterator != workflowRun.m_TaskStates.end())
                {
                    stateIterator->second = result.m_TaskState;
                }

                if (!result.m_ExecuteOk)
                {
                    LOG_APP_WARN("[workflow] task '{}' failed in run '{}': {}", result.m_TaskId, workflowRun.m_RunId,
                                 result.m_TaskState.m_LastErrorMessage);

                    std::string const parentId = ParentTaskId(result.m_TaskId);
                    auto defIt = workflowDefinition.m_Tasks.find(parentId);
                    if (stateIterator != workflowRun.m_TaskStates.end() && defIt != workflowDefinition.m_Tasks.end() &&
                        TryScheduleRetry(stateIterator->second, defIt->second, result.m_TaskId, workflowRun.m_RunId))
                    {
                        // Retry scheduled — do not fail the run.
                    }
                    else
                    {
                        workflowRun.m_HasFailed = true;
                    }
                }
            }

            iterator = activeRun.m_RunningTasks.erase(iterator);
        }

        if (workflowRun.m_HasFailed && activeRun.m_RunningTasks.empty() && activeRun.m_FilterEvalTasks.empty())
        {
            // Ensure WaitingExternal tasks don't linger forever once the run is failed.
            JarvisAgent* app = App::g_App;
            AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

            for (auto& taskPair : workflowRun.m_TaskStates)
            {
                TaskInstanceState& taskState = taskPair.second;

                if (taskState.m_State != TaskInstanceStateKind::WaitingExternal)
                {
                    continue;
                }

                AiRequestHandle requestHandle{};
                requestHandle.requestId = taskState.m_ExternalRequestId;
                requestHandle.requestTimestampNs = taskState.m_ExternalRequestTimestampNs;

                if (requestPool != nullptr && requestHandle.IsValid())
                {
                    requestPool->Forget(requestHandle);
                }

                taskState.m_State = TaskInstanceStateKind::Failed;
                if (taskState.m_LastErrorMessage.empty())
                {
                    taskState.m_LastErrorMessage = "workflow failed while waiting for external completion";
                }
            }

            LOG_APP_WARN("[workflow] run '{}' failed (workflow '{}')", workflowRun.m_RunId, workflowRun.m_WorkflowId);
            workflowRun.m_IsCompleted = true;
            return;
        }

        // ---------------------------------------------------------
        // Cancellation gate (best-effort, cooperative)
        // ---------------------------------------------------------
        if (activeRun.m_CancelRequested)
        {
            // Do not dispatch new work. Once all running tasks finish, mark the run cancelled.
            if (activeRun.m_RunningTasks.empty())
            {
                for (auto& taskPair : workflowRun.m_TaskStates)
                {
                    TaskInstanceState& taskState = taskPair.second;
                    if (taskState.m_State == TaskInstanceStateKind::Pending ||
                        taskState.m_State == TaskInstanceStateKind::Ready ||
                        taskState.m_State == TaskInstanceStateKind::WaitingExternal)
                    {
                        taskState.m_State = TaskInstanceStateKind::Skipped;
                        if (taskState.m_LastErrorMessage.empty())
                        {
                            taskState.m_LastErrorMessage = "cancelled";
                        }
                    }
                }

                workflowRun.m_State = WorkflowRunState::Cancelled;
                workflowRun.m_CompletedAtIso8601 = GetIso8601NowUTC();
                workflowRun.m_IsCompleted = true;
                return;
            }
        }

        // ---------------------------------------------------------
        // 2) Dispatch newly-ready tasks (no waiting)
        // ---------------------------------------------------------
        if (Core::g_Core == nullptr)
        {
            workflowRun.m_HasFailed = true;
            workflowRun.m_IsCompleted = true;
            return;
        }

        ThreadPool& pool = Core::g_Core->GetThreadPool();

        bool dispatchedAny = false;

        for (auto& taskPair : workflowRun.m_TaskStates)
        {
            std::string const& taskId = taskPair.first;
            TaskInstanceState& taskState = taskPair.second;

            if (taskState.m_State != TaskInstanceStateKind::Pending && taskState.m_State != TaskInstanceStateKind::Ready)
            {
                continue;
            }

            // Respect retry backoff: skip if the retry-after time hasn't arrived yet.
            if (taskState.m_RetryAfterTime != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() < taskState.m_RetryAfterTime)
            {
                continue;
            }

            // For child instances (taskId#k), look up the parent's TaskDef
            std::string const parentId = ParentTaskId(taskId);
            bool const isChild = IsChildInstance(taskId);

            auto defIterator = workflowDefinition.m_Tasks.find(parentId);
            if (defIterator == workflowDefinition.m_Tasks.end())
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "task missing from workflow definition";
                workflowRun.m_HasFailed = true;
                continue;
            }

            TaskDef const& taskDefinition = defIterator->second;

            // Child instances skip DAG readiness (parent manages them)
            if (!isChild && !IsTaskReady(workflowRun, taskDefinition))
            {
                continue;
            }

            // Per-item parent tasks: dispatch filter evaluation, not normal execution
            if (!isChild && taskDefinition.m_Mode == TaskMode::PerItem && !taskDefinition.m_Filter.empty())
            {
                if (activeRun.m_FilterEvalTasks.find(taskId) == activeRun.m_FilterEvalTasks.end() &&
                    activeRun.m_PerItemChildren.find(taskId) == activeRun.m_PerItemChildren.end())
                {
                    DispatchFilterEvaluation(activeRun, taskId, taskDefinition);
                    dispatchedAny = true;
                }
                continue;
            }

            // Freshness check + skip (not for child instances — already handled during fan-out)
            if (!isChild)
            {
                TaskFreshnessChecker freshnessChecker;
                TaskFreshnessChecker::ResolvedPaths resolvedPaths;

                if (TaskPathResolver::ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                                   resolvedPaths.m_InputPaths, resolvedPaths.m_OutputPaths))
                {
                    LOG_APP_INFO("[paths debug] debug reason=resolveFreshnessPaths workflowId='{}' runId='{}' taskId='{}' "
                                 "inputCount={} outputCount={}",
                                 workflowRun.m_WorkflowId, workflowRun.m_RunId, taskId,
                                 static_cast<int>(resolvedPaths.m_InputPaths.size()),
                                 static_cast<int>(resolvedPaths.m_OutputPaths.size()));

                    for (fs::path const& inputPath : resolvedPaths.m_InputPaths)
                    {
                        LOG_APP_INFO("[paths debug] debug reason=resolveFreshnessPathsInput workflowId='{}' runId='{}' "
                                     "taskId='{}' inputPathAbsolute='{}'",
                                     workflowRun.m_WorkflowId, workflowRun.m_RunId, taskId, inputPath.string());
                    }

                    for (fs::path const& outputPath : resolvedPaths.m_OutputPaths)
                    {
                        LOG_APP_INFO("[paths debug] debug reason=resolveFreshnessPathsOutput workflowId='{}' runId='{}' "
                                     "taskId='{}' outputPathAbsolute='{}'",
                                     workflowRun.m_WorkflowId, workflowRun.m_RunId, taskId, outputPath.string());
                    }
                    auto resolveUpstreamOutputs = [&](std::string const& upstreamTaskId,
                                                      std::vector<fs::path>& outPaths) -> bool
                    {
                        auto upstreamIt = workflowDefinition.m_Tasks.find(upstreamTaskId);
                        if (upstreamIt == workflowDefinition.m_Tasks.end())
                        {
                            return false;
                        }

                        std::vector<fs::path> unusedInputs;
                        std::vector<fs::path> outputPaths;

                        if (!TaskPathResolver::ResolveFreshnessPathsForTask(workflowDefinition, workflowRun,
                                                                            upstreamIt->second, upstreamTaskId, unusedInputs,
                                                                            outputPaths))
                        {
                            return false;
                        }

                        outPaths = outputPaths;
                        return true;
                    };

                    if (freshnessChecker.IsTaskUpToDate(workflowDefinition, taskId, resolvedPaths, resolveUpstreamOutputs))
                    {
                        PopulateSkippedTaskOutputsIfPossible(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                             taskState);
                        taskState.m_State = TaskInstanceStateKind::Skipped;
                        LOG_APP_INFO("[paths debug] debug reason=freshnessSkip workflowId='{}' runId='{}' taskId='{}'",
                                     workflowRun.m_WorkflowId, workflowRun.m_RunId, taskId);
                        dispatchedAny = true;
                        continue;
                    }
                }
            }

            if (activeRun.m_RunningTasks.find(taskId) != activeRun.m_RunningTasks.end())
            {
                continue;
            }

            taskState.m_State = TaskInstanceStateKind::Running;

            WorkflowRun const workflowRunSnapshot = workflowRun;
            TaskInstanceState const taskStateSnapshot = taskState;

            activeRun.m_RunningTasks[taskId] =
                pool.SubmitTask(
                        [this, &workflowDefinition, workflowRunSnapshot, taskDefinition, taskId,
                         taskStateSnapshot]() -> TaskExecutionResult {
                            return ExecuteTaskOnWorker(workflowDefinition, workflowRunSnapshot, taskDefinition, taskId,
                                                       taskStateSnapshot);
                        })
                    .share();

            dispatchedAny = true;
        }

        // ---------------------------------------------------------
        // 3) Completion / deadlock detection
        // ---------------------------------------------------------
        if (IsRunTerminal(activeRun))
        {
            if (workflowRun.m_HasFailed)
            {
                LOG_APP_WARN("[workflow] run '{}' failed (workflow '{}')", workflowRun.m_RunId, workflowRun.m_WorkflowId);
            }
            else
            {
                LOG_APP_INFO("[workflow] run '{}' completed (workflow '{}')", workflowRun.m_RunId, workflowRun.m_WorkflowId);
            }
            workflowRun.m_IsCompleted = true;
            return;
        }

        if (!dispatchedAny && activeRun.m_RunningTasks.empty() && activeRun.m_FilterEvalTasks.empty())
        {
            bool hasWaitingExternal = false;
            bool hasPendingOrReady = false;
            bool hasRetryPending = false;

            for (auto const& taskPair : workflowRun.m_TaskStates)
            {
                TaskInstanceStateKind const state = taskPair.second.m_State;

                if (state == TaskInstanceStateKind::WaitingExternal)
                {
                    hasWaitingExternal = true;
                }
                else if (state == TaskInstanceStateKind::Pending || state == TaskInstanceStateKind::Ready)
                {
                    hasPendingOrReady = true;

                    // A task waiting for retry backoff is not a deadlock.
                    if (taskPair.second.m_RetryAfterTime != std::chrono::steady_clock::time_point{})
                    {
                        hasRetryPending = true;
                    }
                }
            }

            // WaitingExternal means we are legitimately waiting for filesystem-driven completion.
            // Retry-pending tasks are also legitimately waiting (for their backoff timer).
            if (!hasWaitingExternal && !hasRetryPending && hasPendingOrReady)
            {
                LOG_APP_CRITICAL("[WorkflowRuntimeManager] deadlock/cycle detected in workflow '{}' (run id '{}')",
                                 workflowRun.m_WorkflowId, workflowRun.m_RunId);
                workflowRun.m_HasFailed = true;
                workflowRun.m_IsCompleted = true;
            }
        }
    }

    bool WorkflowRuntimeManager::IsRunTerminal(ActiveRun const& activeRun) const
    {
        for (auto const& taskPair : activeRun.m_Run.m_TaskStates)
        {
            if (!IsTerminal(taskPair.second.m_State))
            {
                return false;
            }
        }

        return true;
    }

    WorkflowRuntimeManager::TaskExecutionResult
    WorkflowRuntimeManager::ExecuteTaskOnWorker(WorkflowDefinition const& workflowDefinition,
                                                WorkflowRun const& workflowRunSnapshot, TaskDef const& taskDefinition,
                                                std::string const& taskId, TaskInstanceState const& taskStateSnapshot) const
    {
        TaskExecutionResult result;
        result.m_TaskId = taskId;
        result.m_TaskState = taskStateSnapshot;

        LOG_APP_INFO("[paths debug] debug reason=dispatchTask workflowId='{}' runId='{}' taskId='{}'",
                     workflowRunSnapshot.m_WorkflowId, workflowRunSnapshot.m_RunId, taskId);

        result.m_TaskState.m_State = TaskInstanceStateKind::Running;
        result.m_TaskState.m_AttemptCount = taskStateSnapshot.m_AttemptCount + 1;

        WorkflowRun workerRun = workflowRunSnapshot;

        DataflowResolver dataflowResolver;

        std::optional<TaskResolvedInputs> optionalResolvedInputs =
            dataflowResolver.ResolveInputsForTask(workflowDefinition, workerRun, taskDefinition, taskId);

        if (!optionalResolvedInputs.has_value())
        {
            result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
            result.m_TaskState.m_LastErrorMessage = "Failed to resolve task inputs via dataflow / context";
            result.m_ExecuteOk = false;
            return result;
        }

        TaskResolvedInputs const& resolvedInputs = optionalResolvedInputs.value();

        if (!resolvedInputs.m_ErrorMessage.empty())
        {
            result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
            result.m_TaskState.m_LastErrorMessage = resolvedInputs.m_ErrorMessage;
            result.m_ExecuteOk = false;
            return result;
        }

        // Merge dataflow-resolved inputs into existing values (preserves per_item filter bindings)
        for (auto const& [key, value] : resolvedInputs.m_StringValues)
        {
            result.m_TaskState.m_InputValues[key] = value;
        }

        {
            std::string summary;
            for (auto const& p : resolvedInputs.m_StringValues)
            {
                summary += p.first;
                summary += "=";
                summary += p.second;
                summary += ";";
            }
            result.m_TaskState.m_InputsJson = summary;
        }

        TaskExecutorRegistry& executorRegistry = TaskExecutorRegistry::Get();

        // Propagate the actual task instance ID so executors can use it for request pool binding.
        // For per_item children this is e.g. "lookupDividend#0"; for single tasks it equals taskDefinition.m_Id.
        result.m_TaskState.m_TaskInstanceId = taskId;

        LOG_APP_INFO("[paths debug] debug reason=executeTask workflowId='{}' runId='{}' taskId='{}'", workerRun.m_WorkflowId,
                     workerRun.m_RunId, taskId);

        // Create inactivity watchdog for tasks with timeout_ms (excluding ai_call which has its own).
        std::shared_ptr<TaskWatchdog> watchdog;
        if (taskDefinition.m_TimeoutMs > 0 && taskDefinition.m_Type != TaskType::AiCall)
        {
            watchdog = std::make_shared<TaskWatchdog>();
            watchdog->Kick(); // initial heartbeat = now
            result.m_TaskState.m_Watchdog = watchdog;
            const_cast<WorkflowRuntimeManager*>(this)->RegisterWatchdog(taskId, watchdog);
        }

        bool const executedOk = executorRegistry.Execute(workflowDefinition, workerRun, taskDefinition, result.m_TaskState);

        // Unregister watchdog before returning.
        if (watchdog)
        {
            const_cast<WorkflowRuntimeManager*>(this)->UnregisterWatchdog(taskId);
        }

        if (!executedOk)
        {
            if (result.m_TaskState.m_State != TaskInstanceStateKind::Failed)
            {
                result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
            }
            result.m_ExecuteOk = false;
            return result;
        }

        // Post-execution inactivity check for synchronous tasks (python, internal).
        // Shell tasks enforce timeout inline via fork/exec/poll watchdog.
        if (watchdog && taskDefinition.m_Type != TaskType::Shell)
        {
            int64_t const inactiveMs = watchdog->ElapsedSinceLastKickMs();
            if (static_cast<uint64_t>(inactiveMs) > taskDefinition.m_TimeoutMs)
            {
                LOG_APP_WARN("Task '{}' exceeded inactivity timeout ({}ms inactive, {}ms limit)", taskId, inactiveMs,
                             taskDefinition.m_TimeoutMs);
                result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
                result.m_TaskState.m_LastErrorMessage = "Task timed out (inactivity: " + std::to_string(inactiveMs) +
                                                        "ms, limit: " + std::to_string(taskDefinition.m_TimeoutMs) + "ms)";
                result.m_ExecuteOk = false;
                return result;
            }
        }

        {
            std::string summary;
            for (auto const& p : result.m_TaskState.m_OutputValues)
            {
                summary += p.first;
                summary += "=";
                summary += p.second;
                summary += ";";
            }
            result.m_TaskState.m_OutputsJson = summary;
        }

        // Preserve non-terminal executor-selected states (WaitingExternal).
        if (result.m_TaskState.m_State == TaskInstanceStateKind::Running)
        {
            result.m_TaskState.m_State = TaskInstanceStateKind::Succeeded;
        }

        result.m_ExecuteOk = (result.m_TaskState.m_State == TaskInstanceStateKind::Succeeded ||
                              result.m_TaskState.m_State == TaskInstanceStateKind::Skipped ||
                              result.m_TaskState.m_State == TaskInstanceStateKind::WaitingExternal);

        return result;
    }

    std::string WorkflowRuntimeManager::GenerateRunId(WorkflowDefinition const& workflowDefinition) const
    {
        auto now = std::chrono::system_clock::now();
        auto nowTimeT = std::chrono::system_clock::to_time_t(now);

        std::string runId = workflowDefinition.m_Id;
        runId += "_";
        runId += std::to_string(static_cast<long long>(nowTimeT));

        return runId;
    }

    bool WorkflowRuntimeManager::TryGetActiveRun(std::string const& runId, WorkflowRun& outRun) const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (ActiveRun const& activeRun : m_ActiveRuns)
        {
            if (activeRun.m_Run.m_RunId == runId)
            {
                outRun = activeRun.m_Run;
                return true;
            }
        }

        return false;
    }

    bool WorkflowRuntimeManager::RequestCancelRun(std::string const& runId)
    {
        if (runId.empty())
        {
            return false;
        }

        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (ActiveRun& activeRun : m_ActiveRuns)
        {
            if (activeRun.m_Run.m_RunId == runId)
            {
                activeRun.m_CancelRequested = true;
                return true;
            }
        }

        return false;
    }

    bool WorkflowRuntimeManager::TryGetRunById(std::string const& runId, WorkflowRun& outRun) const
    {
        if (TryGetActiveRun(runId, outRun))
        {
            return true;
        }

        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (auto const& runPair : m_LastRuns)
        {
            WorkflowRun const& run = runPair.second;
            if (run.m_RunId == runId)
            {
                outRun = run;
                return true;
            }
        }

        return false;
    }

    std::vector<WorkflowRun> WorkflowRuntimeManager::GetActiveRunsSnapshot() const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        std::vector<WorkflowRun> runs;
        runs.reserve(m_ActiveRuns.size());
        for (ActiveRun const& activeRun : m_ActiveRuns)
        {
            runs.emplace_back(activeRun.m_Run);
        }

        return runs;
    }

    std::unordered_map<std::string, WorkflowRun> WorkflowRuntimeManager::GetLastRunsSnapshot() const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        return m_LastRuns; // copy
    }

    // =================================================================
    // Clean command
    // =================================================================

    bool WorkflowRuntimeManager::CleanWorkflow(std::string const& workflowId, std::string& outErrorMessage)
    {
        outErrorMessage.clear();

        if (workflowId.empty())
        {
            outErrorMessage = "workflow id is empty";
            return false;
        }

        // Reject if there is an active run for this workflow.
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            for (ActiveRun const& activeRun : m_ActiveRuns)
            {
                if (activeRun.m_Run.m_WorkflowId == workflowId)
                {
                    outErrorMessage =
                        "cannot clean workflow '" + workflowId + "' while run '" + activeRun.m_Run.m_RunId + "' is active";
                    return false;
                }
            }
        }

        // Fetch workflow definition from registry.
        WorkflowRegistry const* workflowRegistry = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
        }

        if (workflowRegistry == nullptr)
        {
            outErrorMessage = "workflow registry is not available";
            return false;
        }

        std::optional<WorkflowDefinition> const workflowDefOpt = workflowRegistry->GetWorkflow(workflowId);
        if (!workflowDefOpt.has_value())
        {
            outErrorMessage = "workflow '" + workflowId + "' not found in registry";
            return false;
        }

        WorkflowDefinition const& workflowDef = workflowDefOpt.value();

        // Resolve the workflow base directory (same logic as ExecuteTaskOnWorker / DispatchFilterEvaluation).
        std::string workflowBaseDir = workflowDef.m_WorkflowBaseDirectoryAbsolute;
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowBaseDirectory;
        }
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowFileDirectoryAbsolute;
        }
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowFileDirectory;
        }

        fs::path const workflowBasePath = fs::absolute(fs::path(workflowBaseDir)).lexically_normal();

        size_t filesDeleted = 0;
        size_t dirsDeleted = 0;
        std::vector<std::string> errors;

        // ---------------------------------------------------------------
        // 1) Delete queue/<workflowId>/ recursively
        // ---------------------------------------------------------------
        if (Core::g_Core != nullptr)
        {
            fs::path const queueRoot =
                fs::absolute(fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath)).lexically_normal();
            fs::path const queueWorkflowDir = queueRoot / workflowId;

            if (fs::exists(queueWorkflowDir))
            {
                std::error_code ec;
                auto const removed = fs::remove_all(queueWorkflowDir, ec);
                if (ec)
                {
                    errors.push_back("failed to remove queue directory '" + queueWorkflowDir.string() +
                                     "': " + ec.message());
                }
                else
                {
                    dirsDeleted += 1;
                    filesDeleted += (removed > 1) ? (removed - 1) : 0;
                    LOG_APP_INFO("[clean] removed queue directory '{}' ({} entries)", queueWorkflowDir.string(), removed);
                }
            }
        }

        // ---------------------------------------------------------------
        // 2) Delete declared file_outputs and working directories per task
        // ---------------------------------------------------------------
        // Collect working directories to try cleaning up (empty dirs only).
        std::vector<fs::path> workingDirsToClean;

        for (auto const& [taskId, taskDef] : workflowDef.m_Tasks)
        {
            // Resolve task working directory.
            fs::path taskWorkDir;
            if (!taskDef.m_WorkingDirectory.empty())
            {
                taskWorkDir =
                    TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBasePath, taskDef.m_WorkingDirectory);
            }

            // Delete declared file_outputs.
            for (std::string const& fileOutputTemplate : taskDef.m_FileOutputs)
            {
                if (fileOutputTemplate.empty())
                {
                    continue;
                }

                // file_outputs may contain glob-like patterns (e.g. "*.o").
                // Resolve relative to the task working directory.
                fs::path const outputPath(fileOutputTemplate);
                fs::path resolvedPath;

                if (outputPath.is_absolute())
                {
                    resolvedPath = outputPath.lexically_normal();
                }
                else if (!taskWorkDir.empty())
                {
                    resolvedPath = (taskWorkDir / outputPath).lexically_normal();
                }
                else
                {
                    resolvedPath = (workflowBasePath / outputPath).lexically_normal();
                }

                // If the path contains glob characters, expand and delete matching files.
                std::string const resolvedStr = resolvedPath.string();
                if (resolvedStr.find('*') != std::string::npos || resolvedStr.find('?') != std::string::npos)
                {
                    fs::path const parentDir = resolvedPath.parent_path();
                    std::string const pattern = resolvedPath.filename().string();

                    if (fs::exists(parentDir) && fs::is_directory(parentDir))
                    {
                        std::error_code ec;
                        for (auto const& entry : fs::directory_iterator(parentDir, ec))
                        {
                            if (!entry.is_regular_file())
                            {
                                continue;
                            }

                            std::string const filename = entry.path().filename().string();

                            // Simple glob match: support only '*' as "match anything".
                            bool matches = false;
                            if (pattern == "*")
                            {
                                matches = true;
                            }
                            else if (pattern.front() == '*')
                            {
                                // e.g. "*.o" — check suffix
                                std::string const suffix = pattern.substr(1);
                                if (filename.size() >= suffix.size() &&
                                    filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0)
                                {
                                    matches = true;
                                }
                            }
                            else if (pattern.back() == '*')
                            {
                                // e.g. "PROB_*" — check prefix
                                std::string const prefix = pattern.substr(0, pattern.size() - 1);
                                if (filename.size() >= prefix.size() && filename.compare(0, prefix.size(), prefix) == 0)
                                {
                                    matches = true;
                                }
                            }

                            if (matches)
                            {
                                std::error_code removeEc;
                                if (fs::remove(entry.path(), removeEc))
                                {
                                    ++filesDeleted;
                                }
                            }
                        }
                    }
                }
                else
                {
                    // Literal file path.
                    if (fs::exists(resolvedPath) && fs::is_regular_file(resolvedPath))
                    {
                        std::error_code ec;
                        if (fs::remove(resolvedPath, ec))
                        {
                            ++filesDeleted;
                            LOG_APP_INFO("[clean] removed file '{}'", resolvedPath.string());
                        }
                        else if (ec)
                        {
                            errors.push_back("failed to remove '" + resolvedPath.string() + "': " + ec.message());
                        }
                    }
                }
            }

            // Remember working directories for empty-directory cleanup.
            if (!taskWorkDir.empty() && fs::exists(taskWorkDir) && fs::is_directory(taskWorkDir))
            {
                workingDirsToClean.push_back(taskWorkDir);
            }
        }

        // ---------------------------------------------------------------
        // 3) Clean up empty working directories (deepest first)
        // ---------------------------------------------------------------
        // Sort by path length descending so child dirs are removed before parents.
        std::sort(workingDirsToClean.begin(), workingDirsToClean.end(),
                  [](fs::path const& a, fs::path const& b) { return a.string().size() > b.string().size(); });

        for (fs::path const& dirPath : workingDirsToClean)
        {
            if (!fs::exists(dirPath))
            {
                continue;
            }

            if (fs::is_empty(dirPath))
            {
                std::error_code ec;
                if (fs::remove(dirPath, ec))
                {
                    ++dirsDeleted;
                    LOG_APP_INFO("[clean] removed empty directory '{}'", dirPath.string());
                }
            }
        }

        // ---------------------------------------------------------------
        // Summary
        // ---------------------------------------------------------------
        if (!errors.empty())
        {
            outErrorMessage = "clean completed with errors:";
            for (std::string const& err : errors)
            {
                outErrorMessage += " [" + err + "]";
            }
            LOG_APP_WARN("[clean] workflow '{}': {} files, {} dirs deleted, {} errors", workflowId, filesDeleted,
                         dirsDeleted, errors.size());
            return false;
        }

        LOG_APP_INFO("[clean] workflow '{}': {} files, {} dirs deleted", workflowId, filesDeleted, dirsDeleted);
        return true;
    }

    // =================================================================
    // Per-item fan-out helpers
    // =================================================================

    FilterDef const* WorkflowRuntimeManager::FindFilterDef(WorkflowDefinition const& workflowDef,
                                                           std::string const& filterId) const
    {
        for (auto const& filter : workflowDef.m_Filters)
        {
            if (filter.m_Id == filterId)
            {
                return &filter;
            }
        }
        return nullptr;
    }

    std::string WorkflowRuntimeManager::ParentTaskId(std::string const& instanceId)
    {
        auto const pos = instanceId.find('#');
        if (pos == std::string::npos)
        {
            return instanceId;
        }
        return instanceId.substr(0, pos);
    }

    bool WorkflowRuntimeManager::IsChildInstance(std::string const& instanceId)
    {
        return instanceId.find('#') != std::string::npos;
    }

    void WorkflowRuntimeManager::DispatchFilterEvaluation(ActiveRun& activeRun, std::string const& taskId,
                                                          TaskDef const& taskDef)
    {
        WorkflowDefinition const& workflowDef = activeRun.m_Definition;
        WorkflowRun& workflowRun = activeRun.m_Run;

        FilterDef const* filterDef = FindFilterDef(workflowDef, taskDef.m_Filter);
        if (!filterDef)
        {
            auto& taskState = workflowRun.m_TaskStates[taskId];
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage =
                "per_item task '" + taskId + "' references unknown filter '" + taskDef.m_Filter + "'";
            workflowRun.m_HasFailed = true;
            return;
        }

        // Resolve workflow base directory (same logic as ExecuteTaskOnWorker)
        std::string workflowBaseDir = workflowDef.m_WorkflowBaseDirectoryAbsolute;
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowBaseDirectory;
        }
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowFileDirectoryAbsolute;
        }
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowFileDirectory;
        }

        FilterDef const filterDefCopy = *filterDef;
        std::string const parentTaskId = taskId;

        if (Core::g_Core == nullptr)
        {
            auto& taskState = workflowRun.m_TaskStates[taskId];
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "Core is null";
            workflowRun.m_HasFailed = true;
            return;
        }

        ThreadPool& pool = Core::g_Core->GetThreadPool();

        workflowRun.m_TaskStates[taskId].m_State = TaskInstanceStateKind::Running;

        activeRun.m_FilterEvalTasks[taskId] =
            pool.SubmitTask(
                    [filterDefCopy, workflowBaseDir, parentTaskId]() -> FilterEvalResult
                    {
                        FilterEvalResult result;
                        result.m_ParentTaskId = parentTaskId;

                        std::string errorMessage;
                        FilterEngine engine;
                        result.m_Items = engine.Evaluate(filterDefCopy, workflowBaseDir, errorMessage);

                        if (!errorMessage.empty())
                        {
                            result.m_Success = false;
                            result.m_ErrorMessage = errorMessage;
                            return result;
                        }

                        // Build and write manifest
                        FilterManifestManager manifestManager;
                        result.m_Manifest = manifestManager.BuildManifest(filterDefCopy.m_Id, result.m_Items, filterDefCopy);

                        std::string manifestError;
                        if (!manifestManager.WriteManifest(result.m_Manifest, workflowBaseDir, manifestError))
                        {
                            LOG_APP_WARN("[per_item] failed to write manifest for filter '{}': {}", filterDefCopy.m_Id,
                                         manifestError);
                        }

                        result.m_Success = true;
                        return result;
                    })
                .share();

        LOG_APP_INFO("[per_item] dispatched filter evaluation for task '{}' (filter '{}')", taskId, taskDef.m_Filter);
    }

    void WorkflowRuntimeManager::HarvestFilterEvalCompletions(ActiveRun& activeRun)
    {
        WorkflowRun& workflowRun = activeRun.m_Run;

        for (auto it = activeRun.m_FilterEvalTasks.begin(); it != activeRun.m_FilterEvalTasks.end();)
        {
            std::shared_future<FilterEvalResult>& future = it->second;

            if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                ++it;
                continue;
            }

            std::string const parentTaskId = it->first;
            FilterEvalResult evalResult;
            bool gotResult = false;

            try
            {
                evalResult = future.get();
                gotResult = true;
            }
            catch (std::exception const& e)
            {
                auto stateIt = workflowRun.m_TaskStates.find(parentTaskId);
                if (stateIt != workflowRun.m_TaskStates.end())
                {
                    stateIt->second.m_State = TaskInstanceStateKind::Failed;
                    stateIt->second.m_LastErrorMessage = std::string("filter evaluation threw: ") + e.what();
                }
                workflowRun.m_HasFailed = true;
            }
            catch (...)
            {
                auto stateIt = workflowRun.m_TaskStates.find(parentTaskId);
                if (stateIt != workflowRun.m_TaskStates.end())
                {
                    stateIt->second.m_State = TaskInstanceStateKind::Failed;
                    stateIt->second.m_LastErrorMessage = "filter evaluation threw unknown exception";
                }
                workflowRun.m_HasFailed = true;
            }

            if (gotResult)
            {
                if (!evalResult.m_Success)
                {
                    auto stateIt = workflowRun.m_TaskStates.find(parentTaskId);
                    if (stateIt != workflowRun.m_TaskStates.end())
                    {
                        stateIt->second.m_State = TaskInstanceStateKind::Failed;
                        stateIt->second.m_LastErrorMessage = evalResult.m_ErrorMessage;
                    }
                    workflowRun.m_HasFailed = true;
                }
                else
                {
                    // Look up the task definition to get the filter binding
                    auto defIt = activeRun.m_Definition.m_Tasks.find(parentTaskId);
                    if (defIt != activeRun.m_Definition.m_Tasks.end())
                    {
                        FanOutPerItemChildren(activeRun, parentTaskId, evalResult, defIt->second);
                    }
                    else
                    {
                        auto stateIt = workflowRun.m_TaskStates.find(parentTaskId);
                        if (stateIt != workflowRun.m_TaskStates.end())
                        {
                            stateIt->second.m_State = TaskInstanceStateKind::Failed;
                            stateIt->second.m_LastErrorMessage = "task definition not found after filter eval";
                        }
                        workflowRun.m_HasFailed = true;
                    }
                }
            }

            it = activeRun.m_FilterEvalTasks.erase(it);
        }
    }

    void WorkflowRuntimeManager::FanOutPerItemChildren(ActiveRun& activeRun, std::string const& parentTaskId,
                                                       FilterEvalResult const& evalResult, TaskDef const& taskDef)
    {
        WorkflowRun& workflowRun = activeRun.m_Run;
        WorkflowDefinition const& workflowDef = activeRun.m_Definition;

        FilterDef const* filterDef = FindFilterDef(workflowDef, taskDef.m_Filter);
        std::string const binding = filterDef ? filterDef->m_Binding : "item";

        std::vector<std::string> childIds;
        childIds.reserve(evalResult.m_Items.size());

        // Read previous manifest for freshness comparison
        FilterManifestManager manifestManager;
        FilterManifest previousManifest;
        std::string prevManifestError;

        std::string workflowBaseDir = workflowDef.m_WorkflowBaseDirectoryAbsolute;
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowBaseDirectory;
        }

        bool const hasPreviousManifest =
            manifestManager.ReadManifest(taskDef.m_Filter, workflowBaseDir, previousManifest, prevManifestError);

        FilterManifestDiff diff;
        if (hasPreviousManifest)
        {
            diff = manifestManager.CompareManifests(previousManifest, evalResult.m_Manifest);
        }

        size_t skippedCount = 0;

        for (auto const& item : evalResult.m_Items)
        {
            std::string const childId = parentTaskId + "#" + std::to_string(item.m_Index);
            childIds.push_back(childId);

            TaskInstanceState childState;
            childState.m_State = TaskInstanceStateKind::Pending;
            childState.m_AttemptCount = 0;

            // Inject filter item values with binding prefix
            for (auto const& [fieldName, fieldValue] : item.m_Values)
            {
                childState.m_InputValues[binding + "." + fieldName] = fieldValue;
            }

            // Also inject the raw index and key
            childState.m_InputValues[binding + "._index"] = std::to_string(item.m_Index);
            childState.m_InputValues[binding + "._key"] = item.m_Key;
            childState.m_InputValues[binding + "._source_path"] = item.m_SourcePath;

            // Per-item freshness: skip if unchanged and outputs exist
            if (hasPreviousManifest && !diff.m_ExpressionChanged)
            {
                bool isUnchanged = false;
                for (size_t unchangedIdx : diff.m_UnchangedIndices)
                {
                    if (unchangedIdx == item.m_Index)
                    {
                        isUnchanged = true;
                        break;
                    }
                }

                if (isUnchanged)
                {
                    // Check if output files exist (simple existence check)
                    bool outputsExist = true;
                    for (auto const& fileOutput : taskDef.m_FileOutputs)
                    {
                        // Substitute binding variables in the output path
                        std::string resolvedOutput = fileOutput;
                        for (auto const& [k, v] : childState.m_InputValues)
                        {
                            std::string const placeholder = "{{" + k + "}}";
                            size_t pos = resolvedOutput.find(placeholder);
                            while (pos != std::string::npos)
                            {
                                resolvedOutput.replace(pos, placeholder.size(), v);
                                pos = resolvedOutput.find(placeholder, pos + v.size());
                            }
                        }

                        if (!resolvedOutput.empty() && !fs::exists(resolvedOutput))
                        {
                            outputsExist = false;
                            break;
                        }
                    }

                    if (outputsExist && !taskDef.m_FileOutputs.empty())
                    {
                        childState.m_State = TaskInstanceStateKind::Skipped;
                        ++skippedCount;
                    }
                }
            }

            workflowRun.m_TaskStates[childId] = std::move(childState);
        }

        activeRun.m_PerItemChildren[parentTaskId] = std::move(childIds);

        LOG_APP_INFO("[per_item] fan-out for task '{}': {} children ({} skipped as fresh)", parentTaskId,
                     evalResult.m_Items.size(), skippedCount);
    }

    void WorkflowRuntimeManager::AggregatePerItemResults(ActiveRun& activeRun)
    {
        WorkflowRun& workflowRun = activeRun.m_Run;

        for (auto const& [parentTaskId, childIds] : activeRun.m_PerItemChildren)
        {
            auto parentIt = workflowRun.m_TaskStates.find(parentTaskId);
            if (parentIt == workflowRun.m_TaskStates.end())
            {
                continue;
            }

            // Parent must be Running (set during DispatchFilterEvaluation)
            if (parentIt->second.m_State != TaskInstanceStateKind::Running)
            {
                continue;
            }

            bool allTerminal = true;
            bool anyFailed = false;
            size_t succeededCount = 0;
            size_t skippedCount = 0;

            for (std::string const& childId : childIds)
            {
                auto childIt = workflowRun.m_TaskStates.find(childId);
                if (childIt == workflowRun.m_TaskStates.end())
                {
                    allTerminal = false;
                    continue;
                }

                TaskInstanceStateKind const childState = childIt->second.m_State;

                if (!IsTerminal(childState))
                {
                    allTerminal = false;
                }

                if (childState == TaskInstanceStateKind::Failed)
                {
                    anyFailed = true;
                }
                else if (childState == TaskInstanceStateKind::Succeeded)
                {
                    ++succeededCount;
                }
                else if (childState == TaskInstanceStateKind::Skipped)
                {
                    ++skippedCount;
                }
            }

            if (!allTerminal)
            {
                continue;
            }

            if (anyFailed)
            {
                parentIt->second.m_State = TaskInstanceStateKind::Failed;
                parentIt->second.m_LastErrorMessage =
                    "per_item: one or more child instances failed (" + std::to_string(childIds.size()) + " total)";
                workflowRun.m_HasFailed = true;
            }
            else
            {
                parentIt->second.m_State = TaskInstanceStateKind::Succeeded;
            }

            parentIt->second.m_OutputsJson = "children=" + std::to_string(childIds.size()) +
                                             ";succeeded=" + std::to_string(succeededCount) +
                                             ";skipped=" + std::to_string(skippedCount);

            LOG_APP_INFO("[per_item] parent '{}' completed: {} children, {} succeeded, {} skipped", parentTaskId,
                         childIds.size(), succeededCount, skippedCount);
        }
    }

} // namespace AIAssistant
