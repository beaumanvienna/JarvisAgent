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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#include "taskExecutorRegistry.h"

#include <fstream>

#include "engine.h"
#include "taskPathResolver.h"
#include "templateEngine.h"

namespace AIAssistant
{
    TaskExecutorRegistry& TaskExecutorRegistry::Get()
    {
        static TaskExecutorRegistry instance;
        return instance;
    }

    void TaskExecutorRegistry::RegisterExecutor(TaskType type, std::shared_ptr<ITaskExecutor> const& executorPtr)
    {
        m_Executors[type] = executorPtr;
    }

    // ----------------------------------------------------------------
    // Pre-execution: materialize input files into the task working
    // directory under user-specified names.  This generalises the
    // cntx_files / prob_files materialization that already exists for
    // ai_call tasks (see AiCallTaskExecutor).
    // ----------------------------------------------------------------
    static bool MaterializeFiles(WorkflowDefinition const& workflowDefinition, TaskDef const& taskDefinition,
                                 TaskInstanceState const& taskState)
    {
        if (taskDefinition.m_Materialize.empty())
        {
            return true;
        }

        std::filesystem::path const workflowBaseDirectoryPath =
            TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        if (workflowBaseDirectoryPath.empty())
        {
            LOG_APP_ERROR("MaterializeFiles: Cannot resolve workflow base directory for task '{}'", taskDefinition.m_Id);
            return false;
        }

        std::filesystem::path const taskWorkingDirectoryPath =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPath, taskDefinition.m_WorkingDirectory);

        if (taskWorkingDirectoryPath.empty())
        {
            LOG_APP_ERROR("MaterializeFiles: Cannot resolve working directory for task '{}'", taskDefinition.m_Id);
            return false;
        }

        // Ensure working directory exists.
        std::error_code ec;
        std::filesystem::create_directories(taskWorkingDirectoryPath, ec);
        if (ec)
        {
            LOG_APP_ERROR("MaterializeFiles: Failed to create working directory '{}' for task '{}': {}",
                          taskWorkingDirectoryPath.string(), taskDefinition.m_Id, ec.message());
            return false;
        }

        // Build template context for expanding {{input[i]}} / {{output[i]}}.
        TemplateContext templateContext;
        templateContext.m_FileInputs = &taskDefinition.m_FileInputs;
        templateContext.m_FileOutputs = &taskDefinition.m_FileOutputs;
        templateContext.m_InputValues = &taskState.m_InputValues;
        templateContext.m_EnvVariables = &taskDefinition.m_Environment.m_Variables;

        for (auto const& [sourceTemplate, targetFilename] : taskDefinition.m_Materialize)
        {
            // Expand {{input[0]}}, {{output[0]}}, etc. in the source key.
            std::string expandedSource;
            std::string expandError;
            if (!ExpandTemplate(sourceTemplate, templateContext, TemplateMode::Strict, expandedSource, expandError))
            {
                LOG_APP_ERROR("MaterializeFiles: Template expansion failed for source '{}' in task '{}': {}", sourceTemplate,
                              taskDefinition.m_Id, expandError);
                return false;
            }

            // Resolve source path relative to task working directory.
            std::filesystem::path sourcePath(expandedSource);
            if (sourcePath.is_relative())
            {
                sourcePath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, sourcePath);
            }
            else
            {
                sourcePath = sourcePath.lexically_normal();
            }

            // Resolve target path in working directory.
            std::filesystem::path const targetPath = (taskWorkingDirectoryPath / targetFilename).lexically_normal();

            // Ensure target parent directory exists.
            std::filesystem::path const targetParent = targetPath.parent_path();
            if (!targetParent.empty())
            {
                std::filesystem::create_directories(targetParent, ec);
                if (ec)
                {
                    LOG_APP_ERROR("MaterializeFiles: Failed to create target directory '{}' for task '{}': {}",
                                  targetParent.string(), taskDefinition.m_Id, ec.message());
                    return false;
                }
            }

            // Check source exists.
            if (!std::filesystem::exists(sourcePath, ec))
            {
                LOG_APP_ERROR("MaterializeFiles: Source file '{}' does not exist for task '{}' (template: '{}')",
                              sourcePath.string(), taskDefinition.m_Id, sourceTemplate);
                return false;
            }

            // Copy file.
            std::error_code copyEc;
            std::filesystem::copy_file(sourcePath, targetPath, std::filesystem::copy_options::overwrite_existing, copyEc);
            if (copyEc)
            {
                LOG_APP_ERROR("MaterializeFiles: Failed to copy '{}' -> '{}' for task '{}': {}", sourcePath.string(),
                              targetPath.string(), taskDefinition.m_Id, copyEc.message());
                return false;
            }

            LOG_APP_INFO("[materialize] Copied '{}' -> '{}' for task '{}'", sourcePath.string(), targetPath.string(),
                         taskDefinition.m_Id);
        }

        return true;
    }

    bool TaskExecutorRegistry::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                       TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        auto iterator = m_Executors.find(taskDefinition.m_Type);

        if (iterator == m_Executors.end())
        {
            LOG_APP_ERROR("TaskExecutorRegistry: No executor registered for task type {}",
                          static_cast<int>(taskDefinition.m_Type));

            taskState.m_LastErrorMessage = "No executor registered";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Pre-execution: materialize input files into working directory.
        if (!MaterializeFiles(workflowDefinition, taskDefinition, taskState))
        {
            taskState.m_LastErrorMessage = "MaterializeFiles failed (see log for details)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        return iterator->second->Execute(workflowDefinition, workflowRun, taskDefinition, taskState);
    }

} // namespace AIAssistant