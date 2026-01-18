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



        std::unordered_map<std::string, TaskInstanceState> BuildInitialTaskStates(WorkflowDefinition const& workflowDefinition)
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
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        m_IsRunning = false;

        std::queue<PendingRun> emptyQueue;
        m_PendingRuns.swap(emptyQueue);

        m_ActiveRuns.clear();
        m_DeferredAiCompletions.clear();
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
            m_PendingRuns.push(PendingRun{ workflowId, runId });
        }

        return runId;
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

            if (completion.m_WasFailed)
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage =
                    completion.m_ErrorMessage.empty() ? "ai_call failed" : completion.m_ErrorMessage;
                activeRun.m_Run.m_HasFailed = true;
            }
            else
            {
                taskState.m_State = TaskInstanceStateKind::Succeeded;
                taskState.m_LastErrorMessage.clear();
            }

            taskState.m_OutputValues = completion.m_OutputValues;

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
                continue;
            }

            std::string const runId = pendingRun.m_RunId.empty() ? GenerateRunId(workflowDefinition.value()) : pendingRun.m_RunId;

            ActiveRun activeRun;
            activeRun.m_Definition = workflowDefinition.value();
            activeRun.m_Run.m_RunId = runId;
            activeRun.m_Run.m_WorkflowId = pendingRun.m_WorkflowId;
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
                }

                workflowRun.m_HasFailed = true;
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
                    workflowRun.m_HasFailed = true;
                }
            }

            iterator = activeRun.m_RunningTasks.erase(iterator);
        }

        if (workflowRun.m_HasFailed && activeRun.m_RunningTasks.empty())
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
            if (taskState.m_State == TaskInstanceStateKind::Pending || taskState.m_State == TaskInstanceStateKind::Ready ||
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

            auto defIterator = workflowDefinition.m_Tasks.find(taskId);
            if (defIterator == workflowDefinition.m_Tasks.end())
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "task missing from workflow definition";
                workflowRun.m_HasFailed = true;
                continue;
            }

            TaskDef const& taskDefinition = defIterator->second;

            if (!IsTaskReady(workflowRun, taskDefinition))
            {
                continue;
            }

            // Freshness check + skip
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
                         taskStateSnapshot]() -> TaskExecutionResult
                        {
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
            workflowRun.m_IsCompleted = true;
            return;
        }

        if (!dispatchedAny && activeRun.m_RunningTasks.empty())
        {
            bool hasWaitingExternal = false;
            bool hasPendingOrReady = false;

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
                }
            }

            // WaitingExternal means we are legitimately waiting for filesystem-driven completion.
            if (!hasWaitingExternal && hasPendingOrReady)
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

        result.m_TaskState.m_InputValues = resolvedInputs.m_StringValues;

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

        LOG_APP_INFO("[paths debug] debug reason=executeTask workflowId='{}' runId='{}' taskId='{}'", workerRun.m_WorkflowId,
                     workerRun.m_RunId, taskId);

        bool const executedOk = executorRegistry.Execute(workflowDefinition, workerRun, taskDefinition, result.m_TaskState);

        if (!executedOk)
        {
            if (result.m_TaskState.m_State != TaskInstanceStateKind::Failed)
            {
                result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
            }
            result.m_ExecuteOk = false;
            return result;
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
} // namespace AIAssistant
