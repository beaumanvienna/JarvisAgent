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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#include "taskFreshnessChecker.h"

#include "engine.h"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

    [[maybe_unused]] fs::path ResolveWorkingDirectory(AIAssistant::WorkflowDefinition const& workflowDefinition,
                                                      AIAssistant::TaskDef const& taskDefinition)
    {
        fs::path workflowBaseDirectory = workflowDefinition.m_WorkflowBaseDirectory;

        if (workflowBaseDirectory.empty())
        {
            workflowBaseDirectory = workflowDefinition.m_WorkflowFileDirectory;
        }

        if (workflowBaseDirectory.empty())
        {
            fs::path const workflowFilePath = workflowDefinition.m_WorkflowFilePath;
            if (!workflowFilePath.empty())
            {
                workflowBaseDirectory = workflowFilePath.parent_path();
            }
        }
        fs::path taskWorkingDirectory = taskDefinition.m_WorkingDirectory;

        bool const hasExplicitWorkingDirectory = !taskWorkingDirectory.empty();
        if (!hasExplicitWorkingDirectory)
        {
            taskWorkingDirectory = workflowBaseDirectory;
        }
        else if (!workflowBaseDirectory.empty() && taskWorkingDirectory.is_relative())
        {
            taskWorkingDirectory = fs::path(workflowBaseDirectory) / taskWorkingDirectory;
        }

        if (!taskWorkingDirectory.empty())
        {
            taskWorkingDirectory = taskWorkingDirectory.lexically_normal();
        }

        return taskWorkingDirectory;
    }

    [[maybe_unused]] bool HasPathPrefix(fs::path const& fullPath, fs::path const& prefixPath)
    {
        if (prefixPath.empty())
        {
            return false;
        }

        auto fullIterator = fullPath.begin();
        for (auto prefixIterator = prefixPath.begin(); prefixIterator != prefixPath.end(); ++prefixIterator)
        {
            if (fullIterator == fullPath.end() || *fullIterator != *prefixIterator)
            {
                return false;
            }

            ++fullIterator;
        }

        return true;
    }
} // namespace

namespace AIAssistant
{
    bool TaskFreshnessChecker::IsTaskUpToDate(WorkflowDefinition const& workflowDefinition, std::string const& taskId,
                                         ResolvedPaths const& resolvedPaths, ResolveOutputPathsFn const& resolveOutputPaths,
                                         std::filesystem::path* comparedInputPath, std::filesystem::path* comparedOutputPath) const
    {
        LOG_APP_INFO("[paths debug] debug reason=freshnessCheck taskId={} inputCount={} outputCount={}", taskId,
                     static_cast<int>(resolvedPaths.m_InputPaths.size()),
                     static_cast<int>(resolvedPaths.m_OutputPaths.size()));

        // If the task has no declared outputs, treat it as not provably up to date.
        if (resolvedPaths.m_OutputPaths.empty())
        {
            return false;
        }

        auto definitionIterator = workflowDefinition.m_Tasks.find(taskId);
        if (definitionIterator == workflowDefinition.m_Tasks.end())
        {
            return false;
        }

        TaskDef const& taskDefinition = definitionIterator->second;

        std::error_code errorCode;

        // ---------------------------------------------------------
        // 1) Collect timestamps for this task's declared inputs
        // ---------------------------------------------------------
        std::vector<TimedPath> inputTimes;

        for (fs::path const& inputPath : resolvedPaths.m_InputPaths)
        {
            fs::path const resolvedInputPath = inputPath.lexically_normal();
            if (!fs::exists(resolvedInputPath, errorCode))
            {
                LOG_APP_INFO(
                    "[paths debug] debug reason=freshnessMissingInput taskId={} inputPathRelative={} inputPathAbsolute={} isRelative={}",
                    taskId, inputPath.string(), resolvedInputPath.string(), static_cast<int>(resolvedInputPath.is_relative()));

                // Missing input ⇒ not up to date.
                if (comparedInputPath != nullptr)
                {
                    *comparedInputPath = resolvedInputPath;
                }
                return false;
            }

            auto writeTime = fs::last_write_time(resolvedInputPath, errorCode);
            if (errorCode)
            {
                LOG_APP_INFO(
                    "[paths debug] debug reason=freshnessInputStatFailed taskId={} inputPathAbsolute={} errorCode={}",
                    taskId, resolvedInputPath.string(), errorCode.value());
                return false;
            }

            inputTimes.push_back(TimedPath{resolvedInputPath, writeTime});
        }

        // ---------------------------------------------------------
        // 2) Collect timestamps for all upstream outputs (transitively)
        // ---------------------------------------------------------
        std::unordered_set<std::string> visitedTasks;
        std::vector<TimedPath> upstreamTimes;

        // Collect timestamps for all upstream outputs (transitively).
        // Upstream means: dependencies of this task (not the task itself).
        for (std::string const& dependencyId : taskDefinition.m_DependsOn)
        {
            if (!CollectUpstreamOutputTimes(workflowDefinition, dependencyId, visitedTasks, upstreamTimes,
                                            resolveOutputPaths))
            {
                // If upstream outputs are missing or unreadable, we err on the
                // side of *not* considering this task up to date.
                return false;
            }
        }
        if (!upstreamTimes.empty())
        {
            inputTimes.insert(inputTimes.end(), upstreamTimes.begin(), upstreamTimes.end());
        }

        if (inputTimes.empty())
        {
            // No inputs and no upstream outputs => cannot prove freshness.
            return false;
        }

        auto const latestInputIterator = std::max_element(
            inputTimes.begin(), inputTimes.end(),
            [](TimedPath const& left, TimedPath const& right)
            {
                return left.m_Time < right.m_Time;
            });

        fs::file_time_type latestInputTime = latestInputIterator->m_Time;
        fs::path latestInputPath = latestInputIterator->m_Path;

        if (!upstreamTimes.empty())
        {
            auto const upstreamLatestIterator = std::max_element(
                upstreamTimes.begin(), upstreamTimes.end(),
                [](TimedPath const& left, TimedPath const& right)
                {
                    return left.m_Time < right.m_Time;
                });

            if (upstreamLatestIterator->m_Time > latestInputTime)
            {
                latestInputTime = upstreamLatestIterator->m_Time;
                latestInputPath = upstreamLatestIterator->m_Path;
            }
        }

        // ---------------------------------------------------------
        // 3) Collect timestamps for this task's outputs
        // ---------------------------------------------------------

        std::vector<TimedPath> outputTimes;
        outputTimes.reserve(resolvedPaths.m_OutputPaths.size());

        for (fs::path const& outputPath : resolvedPaths.m_OutputPaths)
        {
            fs::path const resolvedOutputPath = outputPath.lexically_normal();
            if (!fs::exists(resolvedOutputPath, errorCode))
            {
                LOG_APP_INFO(
                    "[paths debug] debug reason=freshnessMissingOutput taskId={} outputPathRelative={} outputPathAbsolute={} isRelative={}",
                    taskId, outputPath.string(), resolvedOutputPath.string(), static_cast<int>(resolvedOutputPath.is_relative()));

                if (comparedOutputPath != nullptr)
                {
                    *comparedOutputPath = resolvedOutputPath;
                }
                return false;
            }

            fs::file_time_type const writeTime = fs::last_write_time(resolvedOutputPath, errorCode);
            if (errorCode)
            {
                LOG_APP_INFO(
                    "[paths debug] debug reason=freshnessOutputStatFailed taskId={} outputPathAbsolute={} errorCode={}",
                    taskId, resolvedOutputPath.string(), errorCode.value());
                return false;
            }

            outputTimes.push_back(TimedPath{resolvedOutputPath, writeTime});
        }

        if (outputTimes.empty())
        {
            // No outputs => cannot prove freshness.
            return false;
        }

        auto const earliestOutputIterator = std::min_element(
            outputTimes.begin(), outputTimes.end(),
            [](TimedPath const& left, TimedPath const& right)
            {
                return left.m_Time < right.m_Time;
            });

        fs::file_time_type const earliestOutputTime = earliestOutputIterator->m_Time;
        fs::path const earliestOutputPath = earliestOutputIterator->m_Path;

        if (comparedInputPath != nullptr)
        {
            *comparedInputPath = latestInputPath;
        }
        if (comparedOutputPath != nullptr)
        {
            *comparedOutputPath = earliestOutputPath;
        }

        // Makefile-style rule extended with upstream outputs:
        // Task is up to date if all outputs exist and the oldest output
        // is >= the newest input or upstream output.
        bool const isUpToDate = (earliestOutputTime >= latestInputTime);

        LOG_APP_INFO(
            "[paths debug] debug reason=freshnessCompare taskId={} inputPathAbsolute={} outputPathAbsolute={} isUpToDate={} inputIsRelative={} outputIsRelative={}",
            taskId, latestInputPath.string(), earliestOutputPath.string(), static_cast<int>(isUpToDate),
            static_cast<int>(latestInputPath.is_relative()), static_cast<int>(earliestOutputPath.is_relative()));
        return isUpToDate;
    }

    bool TaskFreshnessChecker::CollectUpstreamOutputTimes(WorkflowDefinition const& workflowDefinition,
                                                          std::string const& taskId,
                                                          std::unordered_set<std::string>& visitedTasks,
                                                          std::vector<TaskFreshnessChecker::TimedPath>& outTimes,
                                                          ResolveOutputPathsFn const& resolveOutputPaths) const
    {
        LOG_APP_INFO("[paths debug] debug reason=collectUpstreamOutputTimes taskId={}", taskId);

        // Avoid infinite recursion in case validation was skipped.
        if (visitedTasks.contains(taskId))
        {
            return true;
        }
        visitedTasks.insert(taskId);

        auto definitionIterator = workflowDefinition.m_Tasks.find(taskId);
        if (definitionIterator == workflowDefinition.m_Tasks.end())
        {
            return false;
        }

        TaskDef const& taskDefinition = definitionIterator->second;

        // Recurse into dependencies first (transitive closure).
        for (std::string const& dependencyId : taskDefinition.m_DependsOn)
        {
            if (!CollectUpstreamOutputTimes(workflowDefinition, dependencyId, visitedTasks, outTimes, resolveOutputPaths))
            {
                return false;
            }
        }

        // Then collect this task's own outputs.
        std::vector<fs::path> outputPaths;
        if (!resolveOutputPaths(taskId, outputPaths))
        {
            LOG_APP_INFO("[paths debug] debug reason=collectUpstreamOutputResolveFailed taskId={}", taskId);
            return false;
        }

        LOG_APP_INFO("[paths debug] debug reason=collectUpstreamOutputResolved taskId={} outputCount={}", taskId,
                     static_cast<int>(outputPaths.size()));

        std::error_code errorCode;

        for (fs::path const& outputPath : outputPaths)
        {
            fs::path const resolvedOutputPath = outputPath.lexically_normal();
            if (!fs::exists(resolvedOutputPath, errorCode))
            {
                LOG_APP_INFO(
                    "[paths debug] debug reason=collectUpstreamOutputMissing taskId={} outputPathRelative={} outputPathAbsolute={} isRelative={}",
                    taskId, outputPath.string(), resolvedOutputPath.string(), static_cast<int>(resolvedOutputPath.is_relative()));
                return false;
            }

            auto writeTime = fs::last_write_time(resolvedOutputPath, errorCode);
            if (errorCode)
            {
                LOG_APP_INFO(
                    "[paths debug] debug reason=collectUpstreamOutputStatFailed taskId={} outputPathAbsolute={} errorCode={}",
                    taskId, resolvedOutputPath.string(), errorCode.value());
                return false;
            }

            outTimes.push_back(TimedPath{resolvedOutputPath, writeTime});
        }

        return true;
    }

} // namespace AIAssistant
