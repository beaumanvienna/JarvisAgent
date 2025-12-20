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

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    fs::path ResolveWorkingDirectory(AIAssistant::WorkflowDefinition const& workflowDefinition,
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

    bool HasPathPrefix(fs::path const& fullPath, fs::path const& prefixPath)
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
                                              ResolvedPaths const& resolvedPaths,
                                              ResolveOutputPathsFn const& resolveOutputPaths) const
    {
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

        fs::path const taskWorkingDirectory = ResolveWorkingDirectory(workflowDefinition, taskDefinition);

        auto resolveTaskScopedPath = [&](fs::path const& candidatePath) -> fs::path
        {
            fs::path const normalizedCandidatePath = candidatePath.lexically_normal();
            if (!normalizedCandidatePath.is_relative() || taskWorkingDirectory.empty())
            {
                return normalizedCandidatePath;
            }

            if (HasPathPrefix(normalizedCandidatePath, taskWorkingDirectory))
            {
                return normalizedCandidatePath;
            }

            return (taskWorkingDirectory / normalizedCandidatePath).lexically_normal();
        };

        std::error_code errorCode;

        // ---------------------------------------------------------
        // 1) Collect timestamps for this task's declared inputs
        // ---------------------------------------------------------
        std::vector<fs::file_time_type> inputTimes;

        for (fs::path const& inputPath : resolvedPaths.m_InputPaths)
        {
            fs::path const resolvedInputPath = resolveTaskScopedPath(inputPath);
            if (!fs::exists(resolvedInputPath, errorCode))
            {
                // Missing input ⇒ not up to date.
                return false;
            }

            auto writeTime = fs::last_write_time(resolvedInputPath, errorCode);
            if (errorCode)
            {
                return false;
            }

            inputTimes.push_back(writeTime);
        }

        // ---------------------------------------------------------
        // 2) Collect timestamps for all upstream outputs (transitively)
        // ---------------------------------------------------------
        std::unordered_set<std::string> visitedTasks;
        std::vector<fs::file_time_type> upstreamTimes;

        if (!CollectUpstreamOutputTimes(workflowDefinition, taskId, visitedTasks, upstreamTimes, resolveOutputPaths))
        {
            // If upstream outputs are missing or unreadable, we err on the
            // side of *not* considering this task up to date.
            return false;
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

        fs::file_time_type latestInputTime = *std::max_element(inputTimes.begin(), inputTimes.end());

        // ---------------------------------------------------------
        // 3) Collect timestamps for this task's outputs
        // ---------------------------------------------------------
        std::vector<fs::file_time_type> outputTimes;
        outputTimes.reserve(resolvedPaths.m_OutputPaths.size());

        for (fs::path const& outputPath : resolvedPaths.m_OutputPaths)
        {
            fs::path const resolvedOutputPath = resolveTaskScopedPath(outputPath);
            if (!fs::exists(resolvedOutputPath, errorCode))
            {
                // An output is missing ⇒ not up to date.
                return false;
            }

            auto writeTime = fs::last_write_time(resolvedOutputPath, errorCode);
            if (errorCode)
            {
                return false;
            }

            outputTimes.push_back(writeTime);
        }

        if (outputTimes.empty())
        {
            return false;
        }

        fs::file_time_type earliestOutputTime = *std::min_element(outputTimes.begin(), outputTimes.end());

        // Makefile-style rule extended with upstream outputs:
        // Task is up to date if all outputs exist and the oldest output
        // is >= the newest input or upstream output.
        bool const isUpToDate = (earliestOutputTime >= latestInputTime);
        return isUpToDate;
    }

    bool TaskFreshnessChecker::CollectUpstreamOutputTimes(WorkflowDefinition const& workflowDefinition,
                                                          std::string const& taskId,
                                                          std::unordered_set<std::string>& visitedTasks,
                                                          std::vector<fs::file_time_type>& outTimes,
                                                          ResolveOutputPathsFn const& resolveOutputPaths) const
    {
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

        fs::path const taskWorkingDirectory = ResolveWorkingDirectory(workflowDefinition, taskDefinition);

        auto resolveTaskScopedPath = [&](fs::path const& candidatePath) -> fs::path
        {
            fs::path const normalizedCandidatePath = candidatePath.lexically_normal();
            if (!normalizedCandidatePath.is_relative() || taskWorkingDirectory.empty())
            {
                return normalizedCandidatePath;
            }

            if (HasPathPrefix(normalizedCandidatePath, taskWorkingDirectory))
            {
                return normalizedCandidatePath;
            }

            return (taskWorkingDirectory / normalizedCandidatePath).lexically_normal();
        };

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
            return false;
        }

        std::error_code errorCode;

        for (fs::path const& outputPath : outputPaths)
        {
            fs::path const resolvedOutputPath = resolveTaskScopedPath(outputPath);
            if (!fs::exists(resolvedOutputPath, errorCode))
            {
                return false;
            }

            auto writeTime = fs::last_write_time(resolvedOutputPath, errorCode);
            if (errorCode)
            {
                return false;
            }

            outTimes.push_back(writeTime);
        }

        return true;
    }

} // namespace AIAssistant
