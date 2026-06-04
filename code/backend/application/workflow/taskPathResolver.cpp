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
#include "core.h"
#include "log/log.h"

namespace AIAssistant
{
    std::filesystem::path TaskPathResolver::ResolveWorkflowBaseDirectory(WorkflowDefinition const& workflowDefinition)
    {
        namespace fs = std::filesystem;

        fs::path result(workflowDefinition.m_WorkflowBaseDirectoryAbsolute);
        if (result.empty())
        {
            result = fs::path(workflowDefinition.m_WorkflowBaseDirectory);
        }

        if (result.empty())
        {
            fs::path fileDir(workflowDefinition.m_WorkflowFileDirectoryAbsolute);
            if (fileDir.empty())
            {
                fileDir = fs::path(workflowDefinition.m_WorkflowFileDirectory);
            }

            if (!fileDir.empty())
            {
                result = fileDir;
            }
        }

        if (result.empty())
        {
            fs::path filePath(workflowDefinition.m_WorkflowFilePathAbsolute);
            if (filePath.empty())
            {
                filePath = fs::path(workflowDefinition.m_WorkflowFilePath);
            }

            if (!filePath.empty())
            {
                result = filePath.parent_path();
            }
        }

        if (result.empty())
        {
            return result;
        }

        if (result.is_relative() && (Core::g_Core != nullptr))
        {
            result = (Core::g_Core->GetLaunchCWDAbsolute() / result).lexically_normal();
        }

        std::error_code ec;
        fs::path const absolutePath = fs::absolute(result, ec).lexically_normal();
        if (ec)
        {
            return result.lexically_normal();
        }

        return absolutePath;
    }

    std::filesystem::path
    TaskPathResolver::ResolveTaskWorkingDirectoryPath(std::filesystem::path const& workflowBaseDirectoryPath,
                                                      std::string const& taskWorkingDirectoryText)
    {
        LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveTaskWorkingDirectoryPath debug: "
                     "reason=resolveTaskWorkingDirectory baseDirectoryAbsolute='{}' taskWorkingDirectoryRelative='{}'",
                     workflowBaseDirectoryPath.string(), taskWorkingDirectoryText);

        std::filesystem::path taskWorkingDirectoryPathAbsolute =
            ResolveTaskScopedPath(workflowBaseDirectoryPath, taskWorkingDirectoryText);

        // Spec §3.3: if working_directory is omitted or empty, default to the Workflow Base Directory
        if (taskWorkingDirectoryPathAbsolute.empty() && !workflowBaseDirectoryPath.empty())
        {
            taskWorkingDirectoryPathAbsolute = workflowBaseDirectoryPath;
        }

        LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveTaskWorkingDirectoryPath debug: "
                     "reason=resolveTaskWorkingDirectory taskWorkingDirectoryAbsolute='{}'",
                     taskWorkingDirectoryPathAbsolute.string());

        return taskWorkingDirectoryPathAbsolute;
    }

    std::filesystem::path TaskPathResolver::ResolveTaskScopedPath(std::filesystem::path const& taskWorkingDirectoryPath,
                                                                  std::string const& pathText)
    {
        LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveTaskScopedPath debug: reason=resolveTaskScopedPath "
                     "baseDirectoryAbsolute='{}' inputPathRelative='{}'",
                     taskWorkingDirectoryPath.string(), pathText);

        std::filesystem::path const path{pathText};
        std::filesystem::path const resolvedPathAbsolute = ResolvePath(taskWorkingDirectoryPath, path);

        LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveTaskScopedPath debug: reason=resolveTaskScopedPath "
                     "inputPathAbsolute='{}'",
                     resolvedPathAbsolute.string());

        return resolvedPathAbsolute;
    }

    std::filesystem::path TaskPathResolver::ResolvePath(std::filesystem::path const& baseDirectoryPath,
                                                        std::filesystem::path const& path)
    {
        if (path.empty())
        {
            LOG_APP_INFO("[paths debug] TaskPathResolver::ResolvePath debug: reason=resolvePath inputPathRelative='<empty>' "
                         "baseDirectoryAbsolute='{}'",
                         baseDirectoryPath.string());
            return std::filesystem::path{};
        }

        if (path.is_absolute())
        {
            std::filesystem::path const resolvedPathAbsolute = std::filesystem::absolute(path).lexically_normal();
            LOG_APP_INFO("[paths debug] TaskPathResolver::ResolvePath debug: reason=resolvePath inputPathRelative='{}' "
                         "baseDirectoryAbsolute='{}' inputPathAbsolute='{}'",
                         path.string(), baseDirectoryPath.string(), resolvedPathAbsolute.string());
            return resolvedPathAbsolute;
        }

        if (!baseDirectoryPath.empty() && !baseDirectoryPath.is_absolute())
        {
            std::string const launchCwdAbsolute =
                (Core::g_Core != nullptr) ? Core::g_Core->GetLaunchCWDAbsolute().string() : "<null>";
            LOG_APP_INFO("[paths debug] TaskPathResolver::ResolvePath debug: reason=resolvePathCwdFallback "
                         "baseDirectoryRelative='{}' launchCWDAbsolute='{}'",
                         baseDirectoryPath.string(), launchCwdAbsolute);
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
                    std::filesystem::path const resolvedPathAbsolute =
                        std::filesystem::absolute(baseParent / path).lexically_normal();
                    LOG_APP_INFO(
                        "[paths debug] TaskPathResolver::ResolvePath debug: reason=resolvePath inputPathRelative='{}' "
                        "baseDirectoryAbsolute='{}' baseParentDirectoryAbsolute='{}' inputPathAbsolute='{}'",
                        path.string(), baseDirectoryPath.string(), baseParent.string(), resolvedPathAbsolute.string());
                    return resolvedPathAbsolute;
                }

                std::filesystem::path const resolvedPathAbsolute = std::filesystem::absolute(path).lexically_normal();
                LOG_APP_INFO("[paths debug] TaskPathResolver::ResolvePath debug: reason=resolvePath inputPathRelative='{}' "
                             "baseDirectoryAbsolute='{}' inputPathAbsolute='{}'",
                             path.string(), baseDirectoryPath.string(), resolvedPathAbsolute.string());
                return resolvedPathAbsolute;
            }
        }

        std::filesystem::path const resolvedPathAbsolute =
            std::filesystem::absolute(baseDirectoryPath / path).lexically_normal();
        LOG_APP_INFO("[paths debug] TaskPathResolver::ResolvePath debug: reason=resolvePath inputPathRelative='{}' "
                     "baseDirectoryAbsolute='{}' inputPathAbsolute='{}'",
                     path.string(), baseDirectoryPath.string(), resolvedPathAbsolute.string());
        return resolvedPathAbsolute;
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
        LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveFreshnessPathsForTask debug: reason=resolveFreshnessPaths "
                     "taskId='{}' workflowId='{}'",
                     taskId, workflowDefinition.m_Id);
        LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveFreshnessPathsForTask debug: reason=resolveFreshnessPaths "
                     "workflowBaseDirectoryAbsolute='{}' launchCWDAbsolute='{}'",
                     workflowDefinition.m_WorkflowBaseDirectoryAbsolute,
                     (Core::g_Core != nullptr) ? Core::g_Core->GetLaunchCWDAbsolute().string() : "<null>");

        std::filesystem::path const workflowBaseDirectoryPathAbsolute = ResolveWorkflowBaseDirectory(workflowDefinition);

        std::filesystem::path const taskWorkingDirectoryPath =
            ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPathAbsolute, taskDefinition.m_WorkingDirectory);

        // Resolve inputs (dataflow) for this task.
        DataflowResolver const resolver;
        std::optional<TaskResolvedInputs> resolvedInputsOptional =
            resolver.ResolveInputsForTask(workflowDefinition, workflowRun, taskDefinition, taskId);

        if (!resolvedInputsOptional.has_value())
        {
            LOG_APP_INFO(
                "[paths debug] TaskPathResolver::ResolveFreshnessPathsForTask debug: reason=resolveInputsFailed taskId='{}'",
                taskId);
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

            return anyInline(taskDefinition.m_QueueBinding.m_StngFiles) ||
                   anyInline(taskDefinition.m_QueueBinding.m_TaskFiles) ||
                   anyInline(taskDefinition.m_QueueBinding.m_CntxFiles) ||
                   anyInline(taskDefinition.m_QueueBinding.m_ProbFiles);
        };

        // If a task embeds inline queue content, freshness MUST depend on the JCWF file itself.
        // Reason: the inline content is stored inside the workflow file; changes to that content update the JCWF mtime.
        if (hasInlineQueueContent())
        {
            std::filesystem::path const workflowFilePath{workflowDefinition.m_WorkflowFilePath};
            std::filesystem::path const workflowFileDirectoryPath{workflowDefinition.m_WorkflowFileDirectory};

            std::filesystem::path const resolvedWorkflowFilePathAbsolute =
                ResolvePath(workflowFileDirectoryPath, workflowFilePath);
            LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveFreshnessPathsForTask debug: reason=resolveFreshnessPaths "
                         "inputPathRelative='{}' inputPathAbsolute='{}'",
                         workflowFilePath.string(), resolvedWorkflowFilePathAbsolute.string());
            inputPathsOut.push_back(resolvedWorkflowFilePathAbsolute);
        }

        // Expand and resolve file inputs.
        for (std::string const& rawInput : taskDefinition.m_FileInputs)
        {
            std::string expanded;
            if (!resolver.ExpandTemplates(rawInput, mergedValues, expanded))
            {
                LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveFreshnessPathsForTask debug: "
                             "reason=expandInputTemplateFailed taskId='{}' inputPathRelative='{}'",
                             taskId, rawInput);
                return false;
            }

            std::filesystem::path const resolvedInputPathAbsolute =
                ResolveTaskScopedPath(taskWorkingDirectoryPath, expanded);
            LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveFreshnessPathsForTask debug: reason=resolveFreshnessPaths "
                         "inputPathRelative='{}' expandedInputPathRelative='{}' inputPathAbsolute='{}'",
                         rawInput, expanded, resolvedInputPathAbsolute.string());
            inputPathsOut.push_back(resolvedInputPathAbsolute);
        }

        // Expand and resolve file outputs.
        for (std::string const& rawOutput : taskDefinition.m_FileOutputs)
        {
            std::string expanded;
            if (!resolver.ExpandTemplates(rawOutput, mergedValues, expanded))
            {
                LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveFreshnessPathsForTask debug: "
                             "reason=expandOutputTemplateFailed taskId='{}' outputPathRelative='{}'",
                             taskId, rawOutput);
                return false;
            }

            std::filesystem::path const resolvedOutputPathAbsolute =
                ResolveTaskScopedPath(taskWorkingDirectoryPath, expanded);
            LOG_APP_INFO("[paths debug] TaskPathResolver::ResolveFreshnessPathsForTask debug: reason=resolveFreshnessPaths "
                         "outputPathRelative='{}' expandedOutputPathRelative='{}' outputPathAbsolute='{}'",
                         rawOutput, expanded, resolvedOutputPathAbsolute.string());
            outputPathsOut.push_back(resolvedOutputPathAbsolute);
        }

        return true;
    }
} // namespace AIAssistant
