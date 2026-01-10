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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#include "taskPathResolver.h"

#include <optional>

#include "dataflowResolver.h"
#include "log/log.h"

namespace AIAssistant
{
    std::filesystem::path
    TaskPathResolver::ResolveTaskWorkingDirectoryPath(std::filesystem::path const& workflowBaseDirectoryPath,
                                                      std::string const& taskWorkingDirectoryText)
    {
        return ResolveTaskScopedPath(workflowBaseDirectoryPath, taskWorkingDirectoryText);
    }

    std::filesystem::path TaskPathResolver::ResolveTaskScopedPath(std::filesystem::path const& taskWorkingDirectoryPath,
                                                                  std::string const& pathText)
    {
        std::filesystem::path const path{pathText};
        return ResolvePath(taskWorkingDirectoryPath, path);
    }

    std::filesystem::path TaskPathResolver::ResolvePath(std::filesystem::path const& baseDirectoryPath,
                                                        std::filesystem::path const& path)
    {
        if (path.empty())
        {
            return std::filesystem::path{};
        }

        if (path.is_absolute())
        {
            return std::filesystem::absolute(path).lexically_normal();
        }


    std::filesystem::path const baseLeaf = baseDirectoryPath.filename();
    if (!baseLeaf.empty())
    {
        auto pathIterator = path.begin();
        if (pathIterator != path.end() && *pathIterator == baseLeaf)
        {
            // Avoid duplicating the base directory when the provided path already starts with it.
            // Example: baseDirectoryPath=".../workflows" and path="workflows/aiZipDemo.jcwf".
            std::filesystem::path const baseParent = baseDirectoryPath.parent_path();
            if (!baseParent.empty())
            {
                return std::filesystem::absolute(baseParent / path).lexically_normal();
            }

            return std::filesystem::absolute(path).lexically_normal();
        }
    }

        return std::filesystem::absolute(baseDirectoryPath / path).lexically_normal();
    }

    void TaskPathResolver::BuildOutputSlotMap(TaskDef const& taskDefinition, TaskInstanceState const& taskState,
                                              std::unordered_map<std::string, std::string>& outputSlotMapOut)
    {
        outputSlotMapOut.clear();

        for (auto const& outputField : taskDefinition.m_Outputs)
        {
            std::string const& outputName = outputField.first;

            auto outputIterator = taskState.m_OutputValues.find(outputName);
            if (outputIterator != taskState.m_OutputValues.end())
            {
                outputSlotMapOut[outputName] = outputIterator->second;
                continue;
            }

            // Backward-compat / fallback: some older tasks may have written produced outputs into m_InputValues.
            if (outputSlotMapOut.contains(outputName))
            {
                continue;
            }

            auto inputIterator = taskState.m_InputValues.find(outputName);
            if (inputIterator != taskState.m_InputValues.end())
            {
                outputSlotMapOut[outputName] = inputIterator->second;
            }
        }
    }

    bool TaskPathResolver::ResolveFreshnessPathsForTask(WorkflowDefinition const& workflowDefinition,
                                                        WorkflowRun const& workflowRun, TaskDef const& taskDefinition,
                                                        std::string const& taskId,
                                                        std::vector<std::filesystem::path>& inputPathsOut,
                                                        std::vector<std::filesystem::path>& outputPathsOut)
    {
        inputPathsOut.clear();
        outputPathsOut.clear();

        std::filesystem::path const workflowBaseDirectoryPath{workflowDefinition.m_WorkflowBaseDirectory};
        std::filesystem::path const taskWorkingDirectoryPath =
            ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPath, taskDefinition.m_WorkingDirectory);

        // Resolve inputs (dataflow) for this task.
        DataflowResolver const resolver;
        std::optional<TaskResolvedInputs> resolvedInputsOptional =
            resolver.ResolveInputsForTask(workflowDefinition, workflowRun, taskDefinition, taskId);

        if (!resolvedInputsOptional.has_value())
        {
            LOG_APP_WARN("Failed to resolve inputs for task '{}' during path resolution.", taskId);
            return false;
        }

        std::unordered_map<std::string, std::string> mergedValues = resolvedInputsOptional->m_StringValues;

        // Merge output slot values (from current task state) so templates that reference produced outputs can expand.
        TaskInstanceState const* taskStatePtr = nullptr;
        auto taskStateIt = workflowRun.m_TaskStates.find(taskId);
        if (taskStateIt != workflowRun.m_TaskStates.end())
        {
            taskStatePtr = &taskStateIt->second;
        }

        if (taskStatePtr != nullptr)
        {
            std::unordered_map<std::string, std::string> outputSlotMap;
            BuildOutputSlotMap(taskDefinition, *taskStatePtr, outputSlotMap);
            for (auto const& kv : outputSlotMap)
            {
                mergedValues[kv.first] = kv.second;
            }
        }


        auto const hasInlineQueueContent = [&]() -> bool
        {
            auto const anyInline = [](std::vector<QueueFileRef> const& refs) -> bool
            {
                for (QueueFileRef const& ref : refs)
                {
                    if (ref.m_HasInlineContent)
                    {
                        return true;
                    }
                }
                return false;
            };

            return anyInline(taskDefinition.m_QueueBinding.m_StngFiles) || anyInline(taskDefinition.m_QueueBinding.m_TaskFiles) ||
                   anyInline(taskDefinition.m_QueueBinding.m_CntxFiles) || anyInline(taskDefinition.m_QueueBinding.m_ProbFiles);
        };

        // If a task embeds inline queue content, freshness MUST depend on the JCWF file itself.
        // Reason: the inline content is stored inside the workflow file; changes to that content update the JCWF mtime.
        if (hasInlineQueueContent())
        {
            std::filesystem::path const workflowFilePath{workflowDefinition.m_WorkflowFilePath};
            std::filesystem::path const workflowFileDirectoryPath{workflowDefinition.m_WorkflowFileDirectory};

            inputPathsOut.push_back(ResolvePath(workflowFileDirectoryPath, workflowFilePath));
        }

        // Expand and resolve file inputs.
        for (std::string const& rawInput : taskDefinition.m_FileInputs)
        {
            std::string expanded;
            if (!resolver.ExpandTemplates(rawInput, mergedValues, expanded))
            {
                LOG_APP_WARN("Failed to expand template for file input '{}' in task '{}'.", rawInput, taskId);
                return false;
            }

            inputPathsOut.push_back(ResolveTaskScopedPath(taskWorkingDirectoryPath, expanded));
        }

        // Expand and resolve file outputs.
        for (std::string const& rawOutput : taskDefinition.m_FileOutputs)
        {
            std::string expanded;
            if (!resolver.ExpandTemplates(rawOutput, mergedValues, expanded))
            {
                LOG_APP_WARN("Failed to expand template for file output '{}' in task '{}'.", rawOutput, taskId);
                return false;
            }

            outputPathsOut.push_back(ResolveTaskScopedPath(taskWorkingDirectoryPath, expanded));
        }

        return true;
    }
} // namespace AIAssistant
