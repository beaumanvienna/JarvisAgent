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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
   OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "engine.h"
#include "jarvisAgent.h"
#include "pythonTaskExecutor.h"
#include "python/pythonEngine.h"
#include "taskPathResolver.h"

#include <filesystem>
#include <unordered_map>

namespace AIAssistant
{
    namespace
    {
        namespace fs = std::filesystem;

        fs::path ResolveWorkflowBaseDirectory(WorkflowDefinition const& workflowDefinition)
        {
            fs::path workflowBaseDirectoryPath(workflowDefinition.m_WorkflowBaseDirectory);

            if (workflowBaseDirectoryPath.empty())
            {
                fs::path const workflowFileDirectoryPath(workflowDefinition.m_WorkflowFileDirectory);
                if (!workflowFileDirectoryPath.empty())
                {
                    workflowBaseDirectoryPath = workflowFileDirectoryPath;
                }
            }

            if (workflowBaseDirectoryPath.empty())
            {
                fs::path const workflowFilePath(workflowDefinition.m_WorkflowFilePath);
                if (!workflowFilePath.empty())
                {
                    workflowBaseDirectoryPath = workflowFilePath.parent_path();
                }
            }

            return workflowBaseDirectoryPath.lexically_normal();
        }

        bool ValidateFileInputsExist(TaskDef const& taskDefinition, fs::path const& taskWorkingDirectoryPath,
                                     std::string& errorMessageOut)
        {
            std::error_code errorCode;

            for (std::string const& fileInput : taskDefinition.m_FileInputs)
            {
                fs::path inputPath(fileInput);

                inputPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, inputPath);

                if (!fs::exists(inputPath, errorCode))
                {
                    errorMessageOut = "PythonTaskExecutor: Missing input file: " + inputPath.string();
                    return false;
                }
            }

            return true;
        }
    } // anonymous namespace

    bool PythonTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                     TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        LOG_APP_INFO("[python] Executing Python task '{}'", taskDefinition.m_Id);

        PythonEngine* pythonEngine = App::g_App->GetPythonEngine();

        if (pythonEngine == nullptr)
        {
            taskState.m_LastErrorMessage = "PythonTaskExecutor: PythonEngine not initialized";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        fs::path const workflowBaseDirectoryPath = ResolveWorkflowBaseDirectory(workflowDefinition);
        fs::path const taskWorkingDirectoryPath =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPath, taskDefinition.m_WorkingDirectory);

        if (taskWorkingDirectoryPath.empty())
        {
            taskState.m_LastErrorMessage = "PythonTaskExecutor: Missing working directory";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::error_code createErrorCode;
        fs::create_directories(taskWorkingDirectoryPath, createErrorCode);

        if (createErrorCode)
        {
            taskState.m_LastErrorMessage = "PythonTaskExecutor: Failed to create working directory";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string errorMessage;
        if (!ValidateFileInputsExist(taskDefinition, taskWorkingDirectoryPath, errorMessage))
        {
            taskState.m_LastErrorMessage = errorMessage;
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::unordered_map<std::string, std::string> contextValues;
        contextValues.reserve(workflowRun.m_Context.size() + 4);

        for (auto const& contextPair : workflowRun.m_Context)
        {
            contextValues[contextPair.first] = contextPair.second.m_Value;
        }

        contextValues["_workflow_id"] = workflowRun.m_WorkflowId;
        contextValues["_run_id"] = workflowRun.m_RunId;
        contextValues["_workflow_base_directory"] = workflowBaseDirectoryPath.string();
        contextValues["_task_working_directory"] = taskWorkingDirectoryPath.string();

        std::unordered_map<std::string, std::string> pythonOutputs;

        if (taskDefinition.m_ParamsJson.empty())
        {
            LOG_APP_ERROR("[python] Task '{}' is missing params JSON (module/function)", taskDefinition.m_Id);
        }


        std::unordered_map<std::string, std::string> callArguments = taskState.m_InputValues;

        // Provide output file path arguments to Python functions when the workflow declares an output slot.
        // This avoids requiring Python scripts to re-derive the output file location.
        if ((taskDefinition.m_FileOutputs.size() == 1) && (!taskDefinition.m_Outputs.empty()))
        {
            fs::path const outputPath = TaskPathResolver::ResolveTaskScopedPath(taskWorkingDirectoryPath, taskDefinition.m_FileOutputs[0]);

            for (auto const& outputField : taskDefinition.m_Outputs)
            {
                std::string const& outputName = outputField.first;

                if (callArguments.contains(outputName))
                {
                    continue;
                }

                if (outputName.find("Path") == std::string::npos)
                {
                    continue;
                }

                callArguments[outputName] = outputPath.string();
            }
        }
        bool const ok =
            pythonEngine->ExecuteWorkflowTask(taskDefinition, taskWorkingDirectoryPath.string(), callArguments,
                                              contextValues, pythonOutputs, errorMessage);

        if (!ok)
        {
            LOG_APP_ERROR("[python] Task '{}' failed: {}", taskDefinition.m_Id, errorMessage);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        for (auto const& outputPair : pythonOutputs)
        {
            taskState.m_OutputValues[outputPair.first] = outputPair.second;
        }

        // Fallback: derive outputs from the task definition and resolved inputs (only fills missing keys).
        std::unordered_map<std::string, std::string> derivedOutputs;
        TaskPathResolver::BuildOutputSlotMap(taskDefinition, taskState, derivedOutputs);

        for (auto const& outputPair : derivedOutputs)
        {
            if (!taskState.m_OutputValues.contains(outputPair.first))
            {
                taskState.m_OutputValues[outputPair.first] = outputPair.second;
            }
        }

        taskState.m_State = TaskInstanceStateKind::Succeeded;
        return true;
    }

} // namespace AIAssistant
