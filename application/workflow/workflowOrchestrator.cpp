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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#include "workflowOrchestrator.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "core.h"
#include "engine.h"
#include "workflowRegistry.h"
#include "dataflowResolver.h"
#include "taskExecutorRegistry.h"
#include "taskFreshnessChecker.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {
        bool ResolveFreshnessPathsForTask(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                          TaskDef const& taskDefinition, std::string const& taskId,
                                          std::vector<fs::path>& outInputPaths, std::vector<fs::path>& outOutputPaths);

        fs::path GetQueueFolderPath()
        {
            if (Core::g_Core == nullptr)
            {
                return fs::path{};
            }

            return fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath).lexically_normal();
        }

        fs::path GetScriptsFolderPath()
        {
            std::error_code errorCode;
            fs::path currentPath = fs::current_path(errorCode);
            if (errorCode)
            {
                return fs::path{};
            }

            return (currentPath / "scripts").lexically_normal();
        }

        bool IsPathWithin(fs::path const& candidatePath, fs::path const& basePath)
        {
            if (candidatePath.empty() || basePath.empty())
            {
                return false;
            }

            std::error_code errorCode;

            fs::path canonicalBasePath = fs::weakly_canonical(basePath, errorCode);
            if (errorCode)
            {
                errorCode.clear();
                canonicalBasePath = fs::absolute(basePath, errorCode).lexically_normal();
                errorCode.clear();
            }

            fs::path canonicalCandidatePath = fs::weakly_canonical(candidatePath, errorCode);
            if (errorCode)
            {
                errorCode.clear();
                canonicalCandidatePath = fs::absolute(candidatePath, errorCode).lexically_normal();
                errorCode.clear();
            }

            auto baseIterator = canonicalBasePath.begin();
            auto candidateIterator = canonicalCandidatePath.begin();

            for (; baseIterator != canonicalBasePath.end(); ++baseIterator, ++candidateIterator)
            {
                if (candidateIterator == canonicalCandidatePath.end() || *candidateIterator != *baseIterator)
                {
                    return false;
                }
            }

            return true;
        }

        bool ValidateTaskPathPolicy(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                    TaskDef const& taskDefinition, std::string const& taskId,
                                    fs::path const& workingDirectory, TaskInstanceState& taskState)
        {
            fs::path const queueFolderPath = GetQueueFolderPath();
            fs::path const scriptsFolderPath = GetScriptsFolderPath();

            auto fail = [&](std::string const& message) -> bool
            {
                taskState.m_LastErrorMessage = message;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            };

            if (IsPathWithin(workingDirectory, scriptsFolderPath))
            {
                return fail("Task working_directory resolves into ./scripts. The scripts folder is reserved for tools.");
            }

            if (taskDefinition.m_Type != TaskType::AiCall && IsPathWithin(workingDirectory, queueFolderPath))
            {
                return fail("Non-AI task working_directory resolves into the queue folder. Only ai_call tasks may use the queue folder.");
            }

            std::vector<fs::path> resolvedInputPaths;
            std::vector<fs::path> resolvedOutputPaths;
            if (!ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId, resolvedInputPaths,
                                              resolvedOutputPaths))
            {
                return fail("Failed to resolve file_outputs for reserved-folder validation.");
            }

            for (fs::path const& outputPath : resolvedOutputPaths)
            {
                if (IsPathWithin(outputPath, scriptsFolderPath))
                {
                    return fail(std::string("Task file_output resolves into ./scripts: ") + outputPath.string());
                }

                if (taskDefinition.m_Type != TaskType::AiCall && IsPathWithin(outputPath, queueFolderPath))
                {
                    return fail(std::string("Non-AI task file_output resolves into queue folder: ") + outputPath.string());
                }
            }

            return true;
        }

        bool ResolveTemplateString(std::string const& value, std::unordered_map<std::string, std::string> const& inputValues,
                                   std::unordered_map<std::string, std::string> const& outputValues,
                                   std::string& outResolved)
        {
            outResolved.clear();
            outResolved.reserve(value.size());

            size_t pos = 0;

            while (pos < value.size())
            {
                size_t const dollar = value.find("${", pos);
                if (dollar == std::string::npos)
                {
                    outResolved.append(value.substr(pos));
                    break;
                }

                outResolved.append(value.substr(pos, dollar - pos));

                size_t const close = value.find('}', dollar + 2);
                if (close == std::string::npos)
                {
                    return false;
                }

                std::string const token = value.substr(dollar + 2, close - (dollar + 2));

                if (token.rfind("inputs.", 0) == 0)
                {
                    std::string const key = token.substr(7);
                    auto iterator = inputValues.find(key);
                    if (iterator == inputValues.end())
                    {
                        return false;
                    }
                    outResolved.append(iterator->second);
                }
                else if (token.rfind("outputs.", 0) == 0)
                {
                    std::string const key = token.substr(8);
                    auto iterator = outputValues.find(key);
                    if (iterator == outputValues.end())
                    {
                        return false;
                    }
                    outResolved.append(iterator->second);
                }
                else
                {
                    return false;
                }

                pos = close + 1;
            }

            if (outResolved.find("${") != std::string::npos)
            {
                return false;
            }

            return true;
        }

        bool ResolveTemplatePathList(std::vector<std::string> const& templates,
                                     std::unordered_map<std::string, std::string> const& inputValues,
                                     std::unordered_map<std::string, std::string> const& outputValues,
                                     std::vector<fs::path>& outPaths)
        {
            outPaths.clear();
            outPaths.reserve(templates.size());

            for (std::string const& templateValue : templates)
            {
                std::string resolved;
                if (!ResolveTemplateString(templateValue, inputValues, outputValues, resolved))
                {
                    if (templateValue.find("${") == std::string::npos)
                    {
                        outPaths.emplace_back(templateValue);
                        continue;
                    }
                    return false;
                }

                if (resolved.empty())
                {
                    return false;
                }

                outPaths.emplace_back(resolved);
            }

            return true;
        }

        bool TryResolveTaskInputsForFreshness(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                              TaskDef const& taskDefinition, std::string const& taskId,
                                              std::unordered_map<std::string, std::string>& outInputValues)
        {
            DataflowResolver dataflowResolver;

            std::optional<TaskResolvedInputs> optionalResolvedInputs =
                dataflowResolver.ResolveInputsForTask(workflowDefinition, workflowRun, taskDefinition, taskId);

            if (!optionalResolvedInputs.has_value())
            {
                return false;
            }

            outInputValues = optionalResolvedInputs.value().m_StringValues;
            return true;
        }

        bool ResolveFreshnessPathsForTask(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                          TaskDef const& taskDefinition, std::string const& taskId,
                                          std::vector<fs::path>& outInputPaths, std::vector<fs::path>& outOutputPaths)
        {
            auto hasTemplatePrefix = [](std::vector<std::string> const& values, std::string const& prefix) -> bool
            {
                for (std::string const& value : values)
                {
                    if (value.find(prefix) != std::string::npos)
                    {
                        return true;
                    }
                }
                return false;
            };

            bool const needsInputResolution = hasTemplatePrefix(taskDefinition.m_FileInputs, "${inputs.") ||
                                              hasTemplatePrefix(taskDefinition.m_FileOutputs, "${inputs.");

            std::unordered_map<std::string, std::string> inputValues;
            if (needsInputResolution)
            {
                if (!TryResolveTaskInputsForFreshness(workflowDefinition, workflowRun, taskDefinition, taskId, inputValues))
                {
                    return false;
                }
            }

            std::unordered_map<std::string, std::string> outputValues;

            auto stateIterator = workflowRun.m_TaskStates.find(taskId);
            if (stateIterator != workflowRun.m_TaskStates.end())
            {
                outputValues = stateIterator->second.m_OutputValues;
            }

            if (!ResolveTemplatePathList(taskDefinition.m_FileInputs, inputValues, outputValues, outInputPaths))
            {
                return false;
            }

            if (!ResolveTemplatePathList(taskDefinition.m_FileOutputs, inputValues, outputValues, outOutputPaths))
            {
                return false;
            }

            fs::path workingDirectory = taskDefinition.m_WorkingDirectory;
            if (workingDirectory.empty())
            {
                workingDirectory = workflowDefinition.m_WorkflowBaseDirectory;
            }
            else if (workingDirectory.is_relative() && !workflowDefinition.m_WorkflowBaseDirectory.empty())
            {
                workingDirectory =
                    (fs::path(workflowDefinition.m_WorkflowBaseDirectory) / workingDirectory).lexically_normal();
            }

            if (!workingDirectory.empty())
            {
                for (fs::path& path : outInputPaths)
                {
                    if (path.is_relative())
                    {
                        path = (workingDirectory / path).lexically_normal();
                    }
                }

                for (fs::path& path : outOutputPaths)
                {
                    if (path.is_relative())
                    {
                        path = (workingDirectory / path).lexically_normal();
                    }
                }
            }

            return true;
        }

        void PopulateSkippedTaskOutputsIfPossible(WorkflowDefinition const& workflowDefinition,
                                                  WorkflowRun const& workflowRun, TaskDef const& taskDefinition,
                                                  std::string const& taskId, TaskInstanceState& taskState)
        {
            std::vector<fs::path> unusedInputPaths;
            std::vector<fs::path> resolvedOutputPaths;

            if (!ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId, unusedInputPaths,
                                              resolvedOutputPaths))
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
                for (auto const& pair : taskState.m_OutputValues)
                {
                    summary += pair.first;
                    summary += "=";
                    summary += pair.second;
                    summary += ";";
                }
                taskState.m_OutputsJson = summary;
            }
        }

        bool AreAllTasksTerminal(WorkflowRun const& workflowRun)
        {
            for (auto const& taskPair : workflowRun.m_TaskStates)
            {
                TaskInstanceStateKind const state = taskPair.second.m_State;
                if (state != TaskInstanceStateKind::Succeeded && state != TaskInstanceStateKind::Skipped &&
                    state != TaskInstanceStateKind::Failed)
                {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    WorkflowOrchestrator& WorkflowOrchestrator::Get()
    {
        static WorkflowOrchestrator instance;
        return instance;
    }

    void WorkflowOrchestrator::SetRegistry(WorkflowRegistry const* workflowRegistry)
    {
        m_WorkflowRegistry = workflowRegistry;
    }

    std::vector<std::string> WorkflowOrchestrator::GetWorkflowIds() const
    {
        std::vector<std::string> workflowIds;

        if (m_WorkflowRegistry == nullptr)
        {
            LOG_APP_WARN("WorkflowOrchestrator::GetWorkflowIds called without a registry");
            return workflowIds;
        }

        workflowIds = m_WorkflowRegistry->GetWorkflowIds();
        return workflowIds;
    }

    std::string WorkflowOrchestrator::StartWorkflowRun(std::string const& workflowId, std::string const& runId)
    {
        if (m_WorkflowRegistry == nullptr)
        {
            LOG_APP_ERROR("WorkflowOrchestrator::StartWorkflowRun: No WorkflowRegistry attached");
            return std::string();
        }

        if (Core::g_Core == nullptr)
        {
            LOG_APP_ERROR("WorkflowOrchestrator::StartWorkflowRun: Core is not initialized");
            return std::string();
        }

        std::optional<WorkflowDefinition> optionalDefinition = m_WorkflowRegistry->GetWorkflow(workflowId);
        if (!optionalDefinition.has_value())
        {
            LOG_APP_ERROR("WorkflowOrchestrator::StartWorkflowRun: Unknown workflow id '{}'", workflowId);
            return std::string();
        }

        ActiveWorkflowRun activeRun;
        activeRun.m_WorkflowDefinition = optionalDefinition.value();

        WorkflowDefinition const& workflowDefinition = activeRun.m_WorkflowDefinition;

        activeRun.m_WorkflowRun.m_WorkflowId = workflowDefinition.m_Id;
        activeRun.m_WorkflowRun.m_RunId = runId.empty() ? GenerateRunId(workflowDefinition) : runId;

        for (auto const& taskPair : workflowDefinition.m_Tasks)
        {
            TaskInstanceState taskState;
            taskState.m_State = TaskInstanceStateKind::Pending;
            activeRun.m_WorkflowRun.m_TaskStates[taskPair.first] = taskState;
        }

        std::string const activeRunId = activeRun.m_WorkflowRun.m_RunId;

        LOG_APP_INFO("WorkflowOrchestrator: Started workflow '{}' (run id '{}')", workflowDefinition.m_Id, activeRunId);

        m_ActiveRuns[activeRunId] = std::move(activeRun);
        return activeRunId;
    }

    void WorkflowOrchestrator::Tick()
    {
        std::vector<std::string> completedRunIds;
        completedRunIds.reserve(m_ActiveRuns.size());

        for (auto const& pair : m_ActiveRuns)
        {
            if (TickActiveRun(pair.first))
            {
                completedRunIds.push_back(pair.first);
            }
        }

        for (std::string const& completedRunId : completedRunIds)
        {
            auto iterator = m_ActiveRuns.find(completedRunId);
            if (iterator == m_ActiveRuns.end())
            {
                continue;
            }

            ActiveWorkflowRun& activeRun = iterator->second;
            WorkflowDefinition const& workflowDefinition = activeRun.m_WorkflowDefinition;
            WorkflowRun const& workflowRun = activeRun.m_WorkflowRun;

            m_LastRuns[workflowDefinition.m_Id] = workflowRun;

            m_ActiveRuns.erase(iterator);
        }
    }

    bool WorkflowOrchestrator::RunWorkflowOnce(std::string const& workflowId, std::string const& runId)
    {
        LOG_APP_WARN("WorkflowOrchestrator::RunWorkflowOnce is non-blocking (ai_call-safe). Use Tick() to progress runs.");

        std::string const startedRunId = StartWorkflowRun(workflowId, runId);
        return !startedRunId.empty();
    }

    bool WorkflowOrchestrator::TryGetLastRun(std::string const& workflowId, WorkflowRun& outRun) const
    {
        auto iterator = m_LastRuns.find(workflowId);
        if (iterator == m_LastRuns.end())
        {
            return false;
        }

        outRun = iterator->second;
        return true;
    }

    bool WorkflowOrchestrator::TryGetActiveRun(std::string const& runId, WorkflowRun*& outRun)
    {
        auto iterator = m_ActiveRuns.find(runId);
        if (iterator == m_ActiveRuns.end())
        {
            return false;
        }

        outRun = &iterator->second.m_WorkflowRun;
        return true;
    }

    std::string WorkflowOrchestrator::GenerateRunId(WorkflowDefinition const& workflowDefinition) const
    {
        auto now = std::chrono::system_clock::now();
        auto nowTimeT = std::chrono::system_clock::to_time_t(now);

        std::string runId = workflowDefinition.m_Id;
        runId += "_";
        runId += std::to_string(static_cast<long long>(nowTimeT));

        return runId;
    }

    bool WorkflowOrchestrator::TickActiveRun(std::string const& activeRunId)
    {
        auto iterator = m_ActiveRuns.find(activeRunId);
        if (iterator == m_ActiveRuns.end())
        {
            return true;
        }

        ActiveWorkflowRun& activeRun = iterator->second;
        WorkflowDefinition const& workflowDefinition = activeRun.m_WorkflowDefinition;
        WorkflowRun& workflowRun = activeRun.m_WorkflowRun;

        if (workflowRun.m_IsCompleted)
        {
            return true;
        }

        bool madeProgressThisTick = false;

        // ---------------------------------------------------------------------
        // 1) Collect completed futures (non-blocking)
        // ---------------------------------------------------------------------
        {
            std::vector<InFlightTask> remainingInFlight;
            remainingInFlight.reserve(activeRun.m_InFlightTasks.size());

            for (auto& inFlightTask : activeRun.m_InFlightTasks)
            {
                std::future_status const status = inFlightTask.m_Future.wait_for(std::chrono::milliseconds(0));
                if (status != std::future_status::ready)
                {
                    remainingInFlight.push_back(std::move(inFlightTask));
                    continue;
                }

                bool success = false;

                try
                {
                    success = inFlightTask.m_Future.get();
                }
                catch (std::exception const& exception)
                {
                    LOG_APP_ERROR("WorkflowOrchestrator: Task '{}' threw exception: {}", inFlightTask.m_TaskId,
                                  exception.what());
                    success = false;
                }

                if (!success)
                {
                    if (inFlightTask.m_TaskState != nullptr)
                    {
                        inFlightTask.m_TaskState->m_State = TaskInstanceStateKind::Failed;
                    }
                    workflowRun.m_HasFailed = true;
                }
                else
                {
                    if (inFlightTask.m_TaskState != nullptr)
                    {
                        if (inFlightTask.m_TaskState->m_State == TaskInstanceStateKind::Running)
                        {
                            inFlightTask.m_TaskState->m_State = TaskInstanceStateKind::Succeeded;
                        }
                    }
                }

                madeProgressThisTick = true;
            }

            activeRun.m_InFlightTasks = std::move(remainingInFlight);
        }

        // ---------------------------------------------------------------------
        // 2) Schedule new ready work (if not failed)
        // ---------------------------------------------------------------------
        if (!workflowRun.m_HasFailed)
        {
            std::vector<std::pair<std::string, TaskInstanceState*>> readyTasks;

            for (auto& taskPair : workflowRun.m_TaskStates)
            {
                std::string const& taskId = taskPair.first;
                TaskInstanceState* taskState = &taskPair.second;

                if (taskState->m_State != TaskInstanceStateKind::Pending &&
                    taskState->m_State != TaskInstanceStateKind::Ready)
                {
                    continue;
                }

                auto defIterator = workflowDefinition.m_Tasks.find(taskId);
                if (defIterator == workflowDefinition.m_Tasks.end())
                {
                    LOG_APP_ERROR("WorkflowOrchestrator: Task '{}' missing from workflow definition '{}'", taskId,
                                  workflowDefinition.m_Id);
                    taskState->m_State = TaskInstanceStateKind::Failed;
                    workflowRun.m_HasFailed = true;
                    madeProgressThisTick = true;
                    continue;
                }

                TaskDef const& taskDefinition = defIterator->second;

                if (!IsTaskReady(workflowDefinition, workflowRun, taskDefinition))
                {
                    continue;
                }

                {
                    TaskFreshnessChecker freshnessChecker;
                    TaskFreshnessChecker::ResolvedPaths resolvedPaths;

                    if (ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                     resolvedPaths.m_InputPaths, resolvedPaths.m_OutputPaths))
                    {
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

                            if (!ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, upstreamIt->second,
                                                              upstreamTaskId, unusedInputs, outputPaths))
                            {
                                return false;
                            }

                            outPaths = outputPaths;
                            return true;
                        };

                        if (freshnessChecker.IsTaskUpToDate(workflowDefinition, taskId, resolvedPaths,
                                                            resolveUpstreamOutputs))
                        {
                            LOG_APP_INFO("WorkflowOrchestrator: Task '{}' is up to date → skipped", taskId);

                            PopulateSkippedTaskOutputsIfPossible(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                                 *taskState);

                            taskState->m_State = TaskInstanceStateKind::Skipped;
                            madeProgressThisTick = true;
                            continue;
                        }
                    }
                }

                readyTasks.emplace_back(taskId, taskState);
            }

            if (!readyTasks.empty())
            {
                ThreadPool& threadPool = Core::g_Core->GetThreadPool();

                for (auto& pair : readyTasks)
                {
                    std::string const& taskId = pair.first;
                    TaskInstanceState* taskState = pair.second;

                    auto defIterator = workflowDefinition.m_Tasks.find(taskId);
                    TaskDef const& taskDefinition = defIterator->second;

                    taskState->m_State = TaskInstanceStateKind::Running;

                    TaskInstanceState* const capturedTaskState = taskState;

                    InFlightTask inFlightTask;
                    inFlightTask.m_TaskId = taskId;
                    inFlightTask.m_TaskState = capturedTaskState;
                    inFlightTask.m_Future = threadPool.SubmitTask(
                        [this, &workflowDefinition, &workflowRun, taskId, taskDefinition, capturedTaskState]() -> bool
                        {
                            return ExecuteTaskInstance(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                       *capturedTaskState);
                        });

                    activeRun.m_InFlightTasks.push_back(std::move(inFlightTask));
                }

                madeProgressThisTick = true;
            }
        }

        // ---------------------------------------------------------------------
        // 3) Completion / deadlock
        // ---------------------------------------------------------------------
        if (AreAllTasksTerminal(workflowRun))
        {
            workflowRun.m_IsCompleted = true;
        }
        else
        {
            bool hasPendingOrReadyTasks = false;
            bool hasInProgressTasks = false;

            for (auto const& taskPair : workflowRun.m_TaskStates)
            {
                TaskInstanceStateKind const state = taskPair.second.m_State;

                if (state == TaskInstanceStateKind::Pending || state == TaskInstanceStateKind::Ready)
                {
                    hasPendingOrReadyTasks = true;
                }
                else if (state == TaskInstanceStateKind::Running || state == TaskInstanceStateKind::WaitingExternal)
                {
                    hasInProgressTasks = true;
                }
            }

            if (!madeProgressThisTick && !hasInProgressTasks && hasPendingOrReadyTasks)
            {
                LOG_APP_CRITICAL("WorkflowOrchestrator: Deadlock or cycle detected in workflow '{}' (run id '{}')",
                                 workflowDefinition.m_Id, workflowRun.m_RunId);
                workflowRun.m_HasFailed = true;
                workflowRun.m_IsCompleted = true;
            }
        }

        if (workflowRun.m_IsCompleted)
        {
            if (workflowRun.m_HasFailed)
            {
                LOG_APP_ERROR("WorkflowOrchestrator: Workflow '{}' (run id '{}') finished with failure",
                              workflowDefinition.m_Id, workflowRun.m_RunId);
            }
            else
            {
                LOG_APP_INFO("WorkflowOrchestrator: Workflow '{}' (run id '{}') completed successfully",
                             workflowDefinition.m_Id, workflowRun.m_RunId);
            }

            return true;
        }

        return false;
    }

    bool WorkflowOrchestrator::IsTaskReady(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                           TaskDef const& taskDefinition) const
    {
        (void)workflowDefinition;

        for (std::string const& dependencyId : taskDefinition.m_DependsOn)
        {
            auto iterator = workflowRun.m_TaskStates.find(dependencyId);
            if (iterator == workflowRun.m_TaskStates.end())
            {
                LOG_APP_ERROR("WorkflowOrchestrator: Task '{}' depends on unknown task '{}'", taskDefinition.m_Id,
                              dependencyId);
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

    bool WorkflowOrchestrator::ExecuteTaskInstance(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                                   TaskDef const& taskDefinition, std::string const& taskId,
                                                   TaskInstanceState& taskState)
    {
        taskState.m_State = TaskInstanceStateKind::Running;
        ++taskState.m_AttemptCount;

        DataflowResolver dataflowResolver;

        std::optional<TaskResolvedInputs> optionalResolvedInputs =
            dataflowResolver.ResolveInputsForTask(workflowDefinition, workflowRun, taskDefinition, taskId);

        if (!optionalResolvedInputs.has_value())
        {
            taskState.m_LastErrorMessage = "Failed to resolve task inputs via dataflow / context";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        TaskResolvedInputs const& resolvedInputs = optionalResolvedInputs.value();

        taskState.m_InputValues = resolvedInputs.m_StringValues;

        {
            std::string summary;
            for (auto const& pair : resolvedInputs.m_StringValues)
            {
                summary += pair.first;
                summary += "=";
                summary += pair.second;
                summary += ";";
            }
            taskState.m_InputsJson = summary;
        }

        fs::path workingDirectory = taskDefinition.m_WorkingDirectory;
        if (workingDirectory.empty())
        {
            workingDirectory = workflowDefinition.m_WorkflowBaseDirectory;
        }
        else if (workingDirectory.is_relative() && !workflowDefinition.m_WorkflowBaseDirectory.empty())
        {
            workingDirectory =
                (fs::path(workflowDefinition.m_WorkflowBaseDirectory) / workingDirectory).lexically_normal();
        }

        if (!ValidateTaskPathPolicy(workflowDefinition, workflowRun, taskDefinition, taskId, workingDirectory, taskState))
        {
            return false;
        }

        if (!workingDirectory.empty())
        {
            try
            {
                fs::create_directories(workingDirectory);
            }
            catch (fs::filesystem_error const& exception)
            {
                taskState.m_LastErrorMessage = std::string("Failed to create task working directory: ") + exception.what();
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
        }

        TaskExecutorRegistry& executorRegistry = TaskExecutorRegistry::Get();
        bool const executedOk = executorRegistry.Execute(workflowDefinition, workflowRun, taskDefinition, taskState);

        if (!executedOk)
        {
            if (taskState.m_State != TaskInstanceStateKind::Failed)
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
            }
            return false;
        }

        {
            std::string summary;
            for (auto const& pair : taskState.m_OutputValues)
            {
                summary += pair.first;
                summary += "=";
                summary += pair.second;
                summary += ";";
            }
            taskState.m_OutputsJson = summary;
        }

        if (taskState.m_State == TaskInstanceStateKind::Running)
        {
            taskState.m_State = TaskInstanceStateKind::Succeeded;
        }

        return (taskState.m_State == TaskInstanceStateKind::Succeeded ||
                taskState.m_State == TaskInstanceStateKind::Skipped ||
                taskState.m_State == TaskInstanceStateKind::WaitingExternal);
    }

} // namespace AIAssistant
