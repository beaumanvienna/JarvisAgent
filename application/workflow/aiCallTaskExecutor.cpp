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
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "jarvisAgent.h"
#include "workflow/aiRequestPool.h"
#include "workflow/taskPathResolver.h"

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

        static bool ReadTextFile(std::filesystem::path const& filePath, std::string& outText, std::string& outErrorMessage)
        {
            std::ifstream inputStream(filePath, std::ios::binary);
            if (!inputStream.is_open())
            {
                std::ostringstream errorStream;
                errorStream << "Failed to open file for reading: " << filePath.string();
                outErrorMessage = errorStream.str();
                return false;
            }

            std::ostringstream textStream;
            textStream << inputStream.rdbuf();
            outText = textStream.str();
            return true;
        }

        static bool BuildProbTextFromQueueBinding(std::filesystem::path const& taskWorkingDirectoryPath,
                                                  std::vector<AIAssistant::QueueFileRef> const& probFiles,
                                                  std::string& outProbText, std::string& outErrorMessage)
        {
            if (probFiles.empty())
            {
                outErrorMessage = "queue_binding.prob_files is empty - cannot materialize PROB";
                return false;
            }

            std::ostringstream probStream;
            bool isFirst = true;

            for (AIAssistant::QueueFileRef const& fileRef : probFiles)
            {
                if (!isFirst)
                {
                    probStream << "\n\n";
                }
                isFirst = false;

                if (fileRef.m_HasInlineContent)
                {
                    LOG_APP_INFO("debug ai_call: PROB inline content path='{}' bytes={}", fileRef.m_Path,
                                 fileRef.m_Content.size());
                    probStream << fileRef.m_Content;
                    continue;
                }

                std::filesystem::path const sourcePath =
                    TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, fileRef.m_Path);
                LOG_APP_INFO("debug ai_call: PROB source path='{}' resolved='{}'", fileRef.m_Path, sourcePath.string());

                std::string sourceText;
                if (!ReadTextFile(sourcePath, sourceText, outErrorMessage))
                {
                    std::ostringstream errorStream;
                    errorStream << "Missing PROB source '" << sourcePath.string() << "': " << outErrorMessage;
                    outErrorMessage = errorStream.str();
                    return false;
                }

                LOG_APP_INFO("debug ai_call: PROB read bytes={} from '{}'", sourceText.size(), sourcePath.string());
                probStream << sourceText;
            }

            outProbText = probStream.str();
            return true;
        }
        static bool StartsWith(std::string const& value, std::string const& prefix) { return value.rfind(prefix, 0) == 0; }

        static bool MaterializeCntxFilesFromQueueBinding(std::filesystem::path const& taskWorkingDirectoryPath,
                                                         std::vector<AIAssistant::QueueFileRef> const& cntxFiles,
                                                         std::string& outErrorMessage)
        {
            std::unordered_set<std::string> usedFilenames;

            for (size_t index = 0; index < cntxFiles.size(); ++index)
            {
                AIAssistant::QueueFileRef const& fileRef = cntxFiles[index];

                // Inline CNTX files are handled by WriteInlineQueueBindingFiles().
                if (fileRef.m_HasInlineContent)
                {
                    continue;
                }

                if (fileRef.m_Path.empty())
                {
                    outErrorMessage = "queue_binding contains CNTX file with empty 'path'";
                    return false;
                }

                std::filesystem::path sourcePath(fileRef.m_Path);
                if (!sourcePath.is_absolute())
                {
                    sourcePath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, sourcePath);
                }
                else
                {
                    sourcePath = sourcePath.lexically_normal();
                }

                std::string sourceText;
                if (!ReadTextFile(sourcePath, sourceText, outErrorMessage))
                {
                    std::ostringstream errorStream;
                    errorStream << "Missing CNTX source '" << sourcePath.string() << "': " << outErrorMessage;
                    outErrorMessage = errorStream.str();
                    return false;
                }

                std::string baseName = sourcePath.filename().string();
                if (!StartsWith(baseName, "CNTX_"))
                {
                    baseName = "CNTX_" + baseName;
                }

                if (usedFilenames.find(baseName) != usedFilenames.end())
                {
                    std::ostringstream renamed;
                    renamed << "CNTX_" << index << "_" << sourcePath.filename().string();
                    baseName = renamed.str();
                }
                usedFilenames.insert(baseName);

                std::filesystem::path const destPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, baseName);

                LOG_APP_INFO("debug ai_call: CNTX source path='{}' resolved='{}' -> writing '{}'", fileRef.m_Path,
                             sourcePath.string(), destPath.string());

                if (!AiCallTaskExecutor::WriteTextFile(destPath.string(), sourceText, outErrorMessage))
                {
                    return false;
                }
            }

            return true;
        }

    } // namespace

    std::string AiCallTaskExecutor::BuildProbFilename(int64_t const requestId, int64_t const timestampNs)
    {
        // Format: PROB_<id>_<timestampNs>.txt or PROB_<id>_<timestampNs>.txt
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

        std::filesystem::path const taskWorkingDirectoryPath =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPath, taskDefinition.m_WorkingDirectory);

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

        auto const localizeInlineFileRefs = [&](std::vector<QueueFileRef>& fileRefs)
        {
            for (QueueFileRef& fileRef : fileRefs)
            {
                if (!fileRef.m_HasInlineContent)
                {
                    continue;
                }

                if (fileRef.m_Path.empty())
                {
                    continue;
                }

                std::filesystem::path const filePath(fileRef.m_Path);
                if (!filePath.is_absolute())
                {
                    std::filesystem::path const rewritten =
                        TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, filePath);
                    fileRef.m_Path = rewritten.string();
                }
            }
        };

        localizeInlineFileRefs(localizedQueueBinding.m_StngFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_TaskFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_CntxFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_ProbFiles);

        if (!WriteInlineQueueBindingFiles(localizedQueueBinding, errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        if (!MaterializeCntxFilesFromQueueBinding(taskWorkingDirectoryPath, localizedQueueBinding.m_CntxFiles, errorMessage))
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
                outputPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, outputPath);
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

        std::string probText;
        if (!BuildProbTextFromQueueBinding(taskWorkingDirectoryPath, localizedQueueBinding.m_ProbFiles, probText,
                                           errorMessage))
        {
            LOG_APP_INFO("debug ai_call: failed to materialize PROB text for task '{}': {}", taskDefinition.m_Id,
                         errorMessage);
            requestPool->Forget(requestHandle);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        if (!WriteTextFile(requestPath.string(), probText, errorMessage))
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
