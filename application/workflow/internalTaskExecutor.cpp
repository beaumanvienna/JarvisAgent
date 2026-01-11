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

#include "workflow/internalTaskExecutor.h"
#include "workflow/taskPathResolver.h"

#include "jarvisAgent.h"
#include "task/internalTaskRegistry.h"

#include <filesystem>
#include <string>
#include <vector>

#include <simdjson/simdjson.h>

namespace AIAssistant
{
    namespace
    {

        bool TryGetActionFromParamsJson(std::string const& paramsJson, std::string& outAction, std::string& outError)
        {
            outAction.clear();

            if (paramsJson.empty())
            {
                outError = "Internal task params are empty (expected JSON with field \"action\").";
                return false;
            }

            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(paramsJson);

            simdjson::ondemand::document doc;
            simdjson::error_code parseError = parser.iterate(padded).get(doc);
            if (parseError != simdjson::SUCCESS)
            {
                outError = "Failed to parse internal task params JSON.";
                return false;
            }

            simdjson::ondemand::object obj;
            simdjson::error_code objError = doc.get_object().get(obj);
            if (objError != simdjson::SUCCESS)
            {
                outError = "Internal task params JSON is not an object.";
                return false;
            }

            simdjson::ondemand::value actionValue;
            simdjson::error_code actionError = obj["action"].get(actionValue);
            if (actionError != simdjson::SUCCESS)
            {
                outError = "Internal task params JSON missing required field \"action\".";
                return false;
            }

            std::string_view actionView;
            simdjson::error_code actionStringError = actionValue.get_string().get(actionView);
            if (actionStringError != simdjson::SUCCESS)
            {
                outError = "Internal task params field \"action\" is not a string.";
                return false;
            }

            outAction = std::string(actionView);
            if (outAction.empty())
            {
                outError = "Internal task params field \"action\" is empty.";
                return false;
            }

            return true;
        }

        bool ValidateExistingInputs(std::vector<std::filesystem::path> const& inputPaths, std::string& outError)
        {
            for (std::filesystem::path const& inputPath : inputPaths)
            {
                if (!std::filesystem::exists(inputPath))
                {
                    outError = "Missing required input file: " + inputPath.string();
                    return false;
                }
            }

            return true;
        }

        void EnsureOutputParentDirectories(std::vector<std::filesystem::path> const& outputPaths)
        {
            for (std::filesystem::path const& outputPath : outputPaths)
            {
                std::filesystem::path parentPath = outputPath.parent_path();
                if (!parentPath.empty())
                {
                    std::error_code existsError;
                    bool const existedBefore = std::filesystem::exists(parentPath, existsError);

                    LOG_APP_INFO(
                        "[folder creation debug] debug create_directories attempt path='{}' reason='internal output parent'",
                        parentPath.string());
                    LOG_APP_INFO("[paths debug] debug reason=writeOutputEnsureParentDirectory outputPathAbsolute='{}' "
                                 "outputParentDirectoryAbsolute='{}'",
                                 outputPath.string(), parentPath.string());
                    std::error_code error;
                    std::filesystem::create_directories(parentPath, error);

                    if (error)
                    {
                        LOG_APP_ERROR("[folder creation debug] debug create_directories failed path='{}' ec={} message='{}'",
                                      parentPath.string(), error.value(), error.message());
                    }
                    else
                    {
                        std::error_code existsAfterError;
                        bool const existsAfter = std::filesystem::exists(parentPath, existsAfterError);
                        bool const created = (!existedBefore && existsAfter);
                        LOG_APP_INFO("[folder creation debug] debug create_directories ok path='{}' created={}",
                                     parentPath.string(), created);
                    }
                }
            }
        }
    } // namespace

    InternalTaskExecutor::InternalTaskExecutor(std::shared_ptr<IInternalTaskRegistry> const& internalTaskRegistryPtr)
        : m_InternalTaskRegistryPtr(internalTaskRegistryPtr)
    {
    }

    bool InternalTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                       TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        std::filesystem::path const workflowBaseDirectoryPath(!workflowDefinition.m_WorkflowBaseDirectoryAbsolute.empty()
                                                                  ? workflowDefinition.m_WorkflowBaseDirectoryAbsolute
                                                                  : workflowDefinition.m_WorkflowBaseDirectory);
        std::filesystem::path const taskWorkingDirectoryPath =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPath, taskDefinition.m_WorkingDirectory);
        LOG_APP_INFO(
            "[paths debug] debug reason=executeInternalTask workflowId='{}' runId='{}' taskId='{}' taskType='internal' "
            "workflowFilePathRelative='{}' workflowFilePathAbsolute='{}' "
            "workflowFileDirectoryRelative='{}' workflowFileDirectoryAbsolute='{}' "
            "workflowBaseDirectoryRelative='{}' workflowBaseDirectoryAbsolute='{}' "
            "taskWorkingDirectoryRelative='{}' taskWorkingDirectoryAbsolute='{}'",
            workflowRun.m_WorkflowId, workflowRun.m_RunId, taskDefinition.m_Id, workflowDefinition.m_WorkflowFilePath,
            workflowDefinition.m_WorkflowFilePathAbsolute, workflowDefinition.m_WorkflowFileDirectory,
            workflowDefinition.m_WorkflowFileDirectoryAbsolute, workflowDefinition.m_WorkflowBaseDirectory,
            workflowDefinition.m_WorkflowBaseDirectoryAbsolute, taskDefinition.m_WorkingDirectory,
            taskWorkingDirectoryPath.string());
        {
            std::error_code error;
            std::error_code existsError;
            bool const existedBefore = std::filesystem::exists(taskWorkingDirectoryPath, existsError);

            LOG_APP_INFO("[folder creation debug] debug create_directories attempt path='{}' reason='internal task "
                         "working_directory'",
                         taskWorkingDirectoryPath.string());
            LOG_APP_INFO("[paths debug] debug reason=ensureWorkingDirectory taskId='{}' taskWorkingDirectoryAbsolute='{}'",
                         taskDefinition.m_Id, taskWorkingDirectoryPath.string());
            std::filesystem::create_directories(taskWorkingDirectoryPath, error);
            if (error)
            {
                LOG_APP_ERROR("[folder creation debug] debug create_directories failed path='{}' ec={} message='{}'",
                              taskWorkingDirectoryPath.string(), error.value(), error.message());
                taskState.m_LastErrorMessage =
                    "Failed to create internal task working directory: " + taskWorkingDirectoryPath.string();
                LOG_APP_ERROR("debug: {}", taskState.m_LastErrorMessage);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::error_code existsAfterError;
            bool const existsAfter = std::filesystem::exists(taskWorkingDirectoryPath, existsAfterError);
            bool const created = (!existedBefore && existsAfter);
            LOG_APP_INFO("[folder creation debug] debug create_directories ok path='{}' created={}",
                         taskWorkingDirectoryPath.string(), created);
        }

        std::vector<std::filesystem::path> resolvedInputPaths;
        resolvedInputPaths.reserve(taskDefinition.m_FileInputs.size());
        for (std::string const& inputPathString : taskDefinition.m_FileInputs)
        {
            resolvedInputPaths.push_back(TaskPathResolver::ResolveTaskScopedPath(taskWorkingDirectoryPath, inputPathString));
            LOG_APP_INFO(
                "[paths debug] debug reason=resolveInputPath taskId='{}' inputPathRelative='{}' inputPathAbsolute='{}'",
                taskDefinition.m_Id, inputPathString, resolvedInputPaths.back().string());
        }

        std::vector<std::filesystem::path> resolvedOutputPaths;
        resolvedOutputPaths.reserve(taskDefinition.m_FileOutputs.size());
        for (std::string const& outputPathString : taskDefinition.m_FileOutputs)
        {
            resolvedOutputPaths.push_back(
                TaskPathResolver::ResolveTaskScopedPath(taskWorkingDirectoryPath, outputPathString));
            LOG_APP_INFO(
                "[paths debug] debug reason=resolveOutputPath taskId='{}' outputPathRelative='{}' outputPathAbsolute='{}'",
                taskDefinition.m_Id, outputPathString, resolvedOutputPaths.back().string());
        }

        {
            std::string validationError;
            if (!ValidateExistingInputs(resolvedInputPaths, validationError))
            {
                taskState.m_LastErrorMessage = validationError;
                LOG_APP_ERROR("debug: {}", taskState.m_LastErrorMessage);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            EnsureOutputParentDirectories(resolvedOutputPaths);
        }

        std::string action;
        {
            std::string paramsError;
            if (!TryGetActionFromParamsJson(taskDefinition.m_ParamsJson, action, paramsError))
            {
                taskState.m_LastErrorMessage = paramsError;
                LOG_APP_ERROR("debug: {}", taskState.m_LastErrorMessage);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
        }

        IInternalTaskRegistry* registry = nullptr;
        LOG_APP_INFO(
            "[paths debug] debug reason=internalTaskAction taskId='{}' action='{}' inputsCount='{}' outputsCount='{}'",
            taskDefinition.m_Id, action, resolvedInputPaths.size(), resolvedOutputPaths.size());

        if (m_InternalTaskRegistryPtr)
        {
            registry = m_InternalTaskRegistryPtr.get();
        }
        else
        {
            if (App::g_App == nullptr)
            {
                taskState.m_LastErrorMessage = "App::g_App is null (cannot access internal task registry).";
                LOG_APP_ERROR("debug: {}", taskState.m_LastErrorMessage);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            registry = App::g_App->GetInternalTaskRegistry();
        }

        if (registry == nullptr)
        {
            taskState.m_LastErrorMessage =
                "Internal task registry is null (did you register internal tasks in JarvisAgent::OnStart?).";
            LOG_APP_ERROR("debug: {}", taskState.m_LastErrorMessage);
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::unique_ptr<ITask> task = registry->CreateTask(action);
        if (!task)
        {
            taskState.m_LastErrorMessage = "No internal task registered for action: " + action;
            LOG_APP_ERROR("debug: {}", taskState.m_LastErrorMessage);
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        for (std::filesystem::path const& inputPath : resolvedInputPaths)
        {
            LOG_APP_INFO("[paths debug] debug reason=internalTaskResolvedInput taskId='{}' inputPathAbsolute='{}'",
                         taskDefinition.m_Id, inputPath.string());
        }
        for (std::filesystem::path const& outputPath : resolvedOutputPaths)
        {
            LOG_APP_INFO("[paths debug] debug reason=internalTaskResolvedOutput taskId='{}' outputPathAbsolute='{}'",
                         taskDefinition.m_Id, outputPath.string());
        }

        std::string taskError;
        bool const ok = task->Execute(resolvedInputPaths, resolvedOutputPaths, taskError);
        if (!ok)
        {
            taskState.m_LastErrorMessage = taskError.empty() ? "Internal task failed." : taskError;
            LOG_APP_ERROR("debug: {}", taskState.m_LastErrorMessage);
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        taskState.m_State = TaskInstanceStateKind::Succeeded;
        taskState.m_LastErrorMessage.clear();
        return true;
    }

} // namespace AIAssistant
