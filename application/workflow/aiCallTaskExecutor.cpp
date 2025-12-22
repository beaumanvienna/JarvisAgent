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

#include "workflow/aiCallTaskExecutor.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include "engine.h"
#include "jarvisAgent.h"
#include "workflow/aiRequestPool.h"

#include "simdjson/simdjson.h"

namespace AIAssistant
{
    namespace
    {
        static int64_t NowTimestampNs()
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        }

        static bool ResolveTemplateString(std::string const& value,
                                          std::unordered_map<std::string, std::string> const& inputValues,
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

        static bool ResolveTemplatePathList(std::vector<std::string> const& templates,
                                            std::unordered_map<std::string, std::string> const& inputValues,
                                            std::vector<std::string>& outResolvedPaths)
        {
            outResolvedPaths.clear();
            outResolvedPaths.reserve(templates.size());

            for (std::string const& t : templates)
            {
                if (t.find("${") == std::string::npos)
                {
                    outResolvedPaths.push_back(t);
                    continue;
                }

                std::string resolved;
                if (!ResolveTemplateString(t, inputValues, resolved))
                {
                    return false;
                }

                if (resolved.empty())
                {
                    return false;
                }

                outResolvedPaths.push_back(std::move(resolved));
            }

            return true;
        }

        static bool ReadTextFile(std::string const& filePath, std::string& outFileContent, std::string& outErrorMessage)
        {
            outErrorMessage.clear();
            outFileContent.clear();

            std::error_code errorCode;
            bool const exists = std::filesystem::exists(filePath, errorCode);
            if (errorCode)
            {
                outErrorMessage = fmt::format("exists() failed for '{}' (error: {})", filePath, errorCode.message());
                return false;
            }

            if (!exists)
            {
                outErrorMessage = fmt::format("file does not exist: '{}'", filePath);
                return false;
            }

            std::ifstream inputStream(filePath, std::ios::binary);
            if (!inputStream.is_open())
            {
                outErrorMessage = fmt::format("failed to open file '{}' for reading", filePath);
                return false;
            }

            std::string fileContent((std::istreambuf_iterator<char>(inputStream)), std::istreambuf_iterator<char>());
            outFileContent = std::move(fileContent);
            return true;
        }

        static bool WriteInlineQueueFileRefs(std::vector<QueueFileRef> const& fileRefs, std::string& outErrorMessage)
        {
            for (QueueFileRef const& fileRef : fileRefs)
            {
                if (!fileRef.m_HasInlineContent)
                {
                    continue;
                }

                if (fileRef.m_Path.empty())
                {
                    outErrorMessage = "queue_binding contains inline file with empty 'path'";
                    return false;
                }

                if (!AiCallTaskExecutor::WriteTextFile(fileRef.m_Path, fileRef.m_Content, outErrorMessage))
                {
                    return false;
                }
            }

            return true;
        }
    } // namespace

    std::string AiCallTaskExecutor::BuildProbFilename(int64_t const requestId, int64_t const timestampNs)
    {
        // Format: PROB_<id>_<timestampNs>.txt or PROB_<id>_<timestampNs>.output.txt
        std::ostringstream stringStream;
        stringStream << "PROB_" << requestId << "_" << timestampNs << ".txt";
        return stringStream.str();
    }

    bool AiCallTaskExecutor::WriteTextFile(std::string const& filePath, std::string const& fileContent,
                                           std::string& outErrorMessage)
    {
        outErrorMessage.clear();

        std::filesystem::path const filesystemPath(filePath);
        std::error_code errorCode;

        std::filesystem::path const parentPath = filesystemPath.parent_path();
        if (!parentPath.empty())
        {
            std::filesystem::create_directories(parentPath, errorCode);
            if (errorCode)
            {
                outErrorMessage = "failed to create directories for: " + filePath + " (" + errorCode.message() + ")";
                return false;
            }
        }

        std::ofstream outputStream(filePath, std::ios::binary | std::ios::trunc);
        if (!outputStream.is_open())
        {
            outErrorMessage = "failed to open for writing: " + filePath;
            return false;
        }

        outputStream.write(fileContent.data(), static_cast<std::streamsize>(fileContent.size()));
        if (!outputStream.good())
        {
            outErrorMessage = "failed while writing: " + filePath;
            return false;
        }

        return true;
    }

    std::optional<std::string> AiCallTaskExecutor::TryExtractStringParam(std::string const& rawParamsJson,
                                                                         std::string const& fieldName,
                                                                         std::string& outErrorMessage)
    {
        outErrorMessage.clear();

        if (rawParamsJson.empty())
        {
            return std::nullopt;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string const paddedJson(rawParamsJson);

        auto document = parser.iterate(paddedJson);
        if (document.error() != simdjson::SUCCESS)
        {
            outErrorMessage = "invalid params JSON: " + std::string(simdjson::error_message(document.error()));
            return std::nullopt;
        }

        std::string_view const fieldNameView(fieldName);

        auto field = document[fieldNameView];

        if (field.error() == simdjson::NO_SUCH_FIELD)
        {
            return std::nullopt;
        }

        if (field.error() != simdjson::SUCCESS)
        {
            outErrorMessage = "error accessing params JSON field '" + fieldName +
                              "': " + std::string(simdjson::error_message(field.error()));
            return std::nullopt;
        }

        std::string_view fieldText;
        auto const stringError = field.get(fieldText);
        if (stringError != simdjson::SUCCESS)
        {
            return std::nullopt;
        }

        return std::string(fieldText);
    }

    std::string AiCallTaskExecutor::ApplySimpleTemplate(std::string const& templateText, TaskInstanceState const& taskState)
    {
        // Replaces occurrences of {{key}} with the corresponding taskState.m_InputValues[key].
        // This is intentionally simple and deterministic (no conditionals, loops, etc.).
        std::string result = templateText;

        std::size_t searchStart = 0;
        for (;;)
        {
            std::size_t const openPos = result.find("{{", searchStart);
            if (openPos == std::string::npos)
            {
                break;
            }

            std::size_t const closePos = result.find("}}", openPos + 2);
            if (closePos == std::string::npos)
            {
                break;
            }

            std::string const key = result.substr(openPos + 2, closePos - (openPos + 2));

            std::string replacement;
            auto const iterator = taskState.m_InputValues.find(key);
            if (iterator != taskState.m_InputValues.end())
            {
                replacement = iterator->second;
            }

            result.replace(openPos, (closePos + 2) - openPos, replacement);
            searchStart = openPos + replacement.size();
        }

        return result;
    }

    std::string AiCallTaskExecutor::TryBuildPromptFromParams(TaskDef const& taskDefinition,
                                                             TaskInstanceState const& taskState)
    {
        std::string errorMessage;

        std::optional<std::string> const promptTemplate =
            TryExtractStringParam(taskDefinition.m_ParamsJson, "prompt_template", errorMessage);

        if (promptTemplate.has_value())
        {
            return ApplySimpleTemplate(promptTemplate.value(), taskState);
        }

        // Fallback: deterministic prompt that includes the raw params JSON and current inputs.
        std::ostringstream stringStream;

        stringStream << "[ai_call]\n";

        if (!taskDefinition.m_Label.empty())
        {
            stringStream << "task_label: " << taskDefinition.m_Label << "\n";
        }

        if (!taskDefinition.m_ParamsJson.empty())
        {
            stringStream << "params_json: " << taskDefinition.m_ParamsJson << "\n";
        }

        stringStream << "inputs:\n";
        for (auto const& pair : taskState.m_InputValues)
        {
            stringStream << "  " << pair.first << ": " << pair.second << "\n";
        }

        return stringStream.str();
    }

    bool AiCallTaskExecutor::WriteInlineQueueBindingFiles(QueueBinding const& queueBinding, std::string& outErrorMessage)
    {
        // ai_call: write environment artifacts (STNG/TASK/CNTX). PROB submission is created by this executor.
        if (!WriteInlineQueueFileRefs(queueBinding.m_StngFiles, outErrorMessage))
        {
            return false;
        }

        if (!WriteInlineQueueFileRefs(queueBinding.m_TaskFiles, outErrorMessage))
        {
            return false;
        }

        if (!WriteInlineQueueFileRefs(queueBinding.m_CntxFiles, outErrorMessage))
        {
            return false;
        }

        return true;
    }

    bool AiCallTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                     TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        std::string errorMessage;

        // ------------------------------------------------------------
        // Resolve workflow base directory (directory containing the loaded .jcwf file)
        // ------------------------------------------------------------
        std::filesystem::path workflowBaseDirectoryPath(workflowDefinition.m_WorkflowBaseDirectory);

        if (workflowBaseDirectoryPath.empty())
        {
            std::filesystem::path const workflowFileDirectoryPath(workflowDefinition.m_WorkflowFileDirectory);
            if (!workflowFileDirectoryPath.empty())
            {
                workflowBaseDirectoryPath = workflowFileDirectoryPath;
            }
        }

        if (workflowBaseDirectoryPath.empty())
        {
            std::filesystem::path const workflowFilePath(workflowDefinition.m_WorkflowFilePath);
            if (!workflowFilePath.empty())
            {
                workflowBaseDirectoryPath = workflowFilePath.parent_path();
            }
        }

        if (workflowBaseDirectoryPath.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "workflow base directory is empty (WorkflowDefinition not populated by loader)";
            return false;
        }

        std::filesystem::path taskWorkingDirectoryPath;
        if (!taskDefinition.m_WorkingDirectory.empty())
        {
            taskWorkingDirectoryPath = std::filesystem::path(taskDefinition.m_WorkingDirectory);
            if (!taskWorkingDirectoryPath.is_absolute())
            {
                taskWorkingDirectoryPath = (workflowBaseDirectoryPath / taskWorkingDirectoryPath).lexically_normal();
            }
            else
            {
                taskWorkingDirectoryPath = taskWorkingDirectoryPath.lexically_normal();
            }
        }
        else
        {
            taskWorkingDirectoryPath = workflowBaseDirectoryPath;
        }

        {
            LOG_APP_INFO("debug: ai_call: workflowBaseDir='{}' taskWorkingDir='{}' taskId='{}'",
                         workflowBaseDirectoryPath.string(), taskWorkingDirectoryPath.string(), taskDefinition.m_Id);

            std::error_code createError;
            std::filesystem::create_directories(taskWorkingDirectoryPath, createError);
            if (createError)
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage =
                    std::format("failed to create task working directory '{}' ({}): {}", taskWorkingDirectoryPath.string(),
                                createError.value(), createError.message());
                LOG_APP_ERROR("debug: ai_call: {}", taskState.m_LastErrorMessage);
                return false;
            }
        }

        JarvisAgent* app = App::g_App;
        if (app == nullptr)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "App::g_App is null";
            return false;
        }

        AiRequestPool* requestPool = app->GetAiRequestPool();
        if (requestPool == nullptr)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "AiRequestPool is null";
            return false;
        }

        std::string const taskIdForBinding = taskDefinition.m_Id;
        if (taskIdForBinding.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ai_call cannot bind request to workflow task: TaskDef.m_Id is empty";
            return false;
        }

        // ------------------------------------------------------------
        // Write inline queue binding files (STNG/TASK/CNTX static artifacts)
        // ------------------------------------------------------------
        QueueBinding localizedQueueBinding = taskDefinition.m_QueueBinding;

        auto const localizeFileRefs = [&](std::vector<QueueFileRef>& fileRefs, std::string const& listName)
        {
            for (QueueFileRef& fileRef : fileRefs)
            {
                if (fileRef.m_Path.empty())
                {
                    continue;
                }

                std::string const originalPath = fileRef.m_Path;

                std::filesystem::path filePath(originalPath);
                if (!filePath.is_absolute())
                {
                    filePath = (taskWorkingDirectoryPath / filePath).lexically_normal();
                    fileRef.m_Path = filePath.string();
                }

                LOG_APP_INFO("debug: ai_call: localized {} path='{}' -> '{}' (inline={})", listName, originalPath,
                             fileRef.m_Path, fileRef.m_HasInlineContent);
            }
        };

        localizeFileRefs(localizedQueueBinding.m_StngFiles, "stng_files");
        localizeFileRefs(localizedQueueBinding.m_TaskFiles, "task_files");
        localizeFileRefs(localizedQueueBinding.m_CntxFiles, "cntx_files");
        localizeFileRefs(localizedQueueBinding.m_ProbFiles, "prob_files");

        if (!WriteInlineQueueBindingFiles(localizedQueueBinding, errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        // ------------------------------------------------------------
        // Determine output mapping for completion (deterministic)
        // ------------------------------------------------------------
        std::vector<std::string> outputSlotNames;
        outputSlotNames.reserve(taskDefinition.m_Outputs.size());

        for (auto const& pair : taskDefinition.m_Outputs)
        {
            outputSlotNames.push_back(pair.first);
        }

        std::sort(outputSlotNames.begin(), outputSlotNames.end());

        std::vector<std::string> resolvedFileOutputs;
        if (!ResolveTemplatePathList(taskDefinition.m_FileOutputs, taskState.m_InputValues, resolvedFileOutputs))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "failed to resolve file_outputs template paths for ai_call";
            return false;
        }

        for (std::string& outputPathText : resolvedFileOutputs)
        {
            std::filesystem::path outputPath(outputPathText);
            if (!outputPath.is_absolute())
            {
                outputPath = (taskWorkingDirectoryPath / outputPath).lexically_normal();
            }
            else
            {
                outputPath = outputPath.lexically_normal();
            }
            outputPathText = outputPath.string();
        }

        // ------------------------------------------------------------
        // Build prompt + submit request (PROB_<id>_<ts>.txt)
        // ------------------------------------------------------------
        int64_t const requestId = requestPool->AllocateRequestId();
        int64_t const timestampNs = NowTimestampNs();

        AiRequestHandle requestHandle{};
        requestHandle.requestId = requestId;
        requestHandle.requestTimestampNs = timestampNs;

        // Register *workflow-bound* request BEFORE writing PROB (so completion can be routed deterministically).
        AiRequestHandle const registered = requestPool->RegisterPendingWorkflowTask(
            requestHandle, workflowRun.m_WorkflowId, workflowRun.m_RunId, taskIdForBinding, resolvedFileOutputs,
            outputSlotNames, taskDefinition.m_TimeoutMs);

        if (!registered.IsValid())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "AiRequestPool::RegisterPendingWorkflowTask failed";
            return false;
        }

        std::filesystem::path const requestPath = taskWorkingDirectoryPath / BuildProbFilename(requestId, timestampNs);

        LOG_APP_INFO("debug: ai_call: writing PROB file to '{}'", requestPath.string());

        std::string promptText;
        if (!localizedQueueBinding.m_ProbFiles.empty())
        {
            std::ostringstream probStream;
            for (size_t probIndex = 0; probIndex < localizedQueueBinding.m_ProbFiles.size(); ++probIndex)
            {
                QueueFileRef const& fileRef = localizedQueueBinding.m_ProbFiles[probIndex];
                if (fileRef.m_HasInlineContent)
                {
                    LOG_APP_INFO("debug: ai_call: PROB inline content (path='{}', bytes={})", fileRef.m_Path,
                                 fileRef.m_Content.size());
                    probStream << fileRef.m_Content;
                }
                else
                {
                    std::string fileContent;
                    std::string readError;
                    LOG_APP_INFO("debug: ai_call: PROB source path='{}'", fileRef.m_Path);
                    if (!ReadTextFile(fileRef.m_Path, fileContent, readError))
                    {
                        requestPool->Forget(requestHandle);
                        taskState.m_State = TaskInstanceStateKind::Failed;
                        taskState.m_LastErrorMessage = readError;
                        LOG_APP_ERROR("debug: ai_call: {}", readError);
                        return false;
                    }

                    probStream << fileContent;
                }

                if (probIndex + 1 < localizedQueueBinding.m_ProbFiles.size())
                {
                    probStream << "\n";
                }
            }

            promptText = probStream.str();
            LOG_APP_INFO("debug: ai_call: built PROB from queue_binding prob_files (count={}, bytes={})",
                         localizedQueueBinding.m_ProbFiles.size(), promptText.size());
        }
        else
        {
            promptText = TryBuildPromptFromParams(taskDefinition, taskState);
            LOG_APP_INFO("debug: ai_call: built PROB from task params (bytes={})", promptText.size());
        }

        if (!WriteTextFile(requestPath.string(), promptText, errorMessage))
        {
            requestPool->Forget(requestHandle);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        // ------------------------------------------------------------
        // Asynchronous completion (event-driven)
        // ------------------------------------------------------------
        taskState.m_ExternalRequestId = requestId;
        taskState.m_ExternalRequestTimestampNs = timestampNs;

        taskState.m_State = TaskInstanceStateKind::WaitingExternal;
        taskState.m_LastErrorMessage.clear();

        return true;
    }
} // namespace AIAssistant