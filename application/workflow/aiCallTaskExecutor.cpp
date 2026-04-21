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
#include "workflow/templateEngine.h"

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
#include "content/chunkPlanner.h"
#include "workflow/aiInvocation.h"
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

        // Build a flat key-value map from the workflow "defaults" JSON, prefixed with "defaults.".
        // E.g. {"ai":{"provider":"openai","model":"gpt-4.1-mini"}} becomes:
        //   "defaults.ai.provider" -> "openai"
        //   "defaults.ai.model"    -> "gpt-4.1-mini"
        static std::unordered_map<std::string, std::string> BuildDefaultsMap(std::string const& defaultsJson)
        {
            std::unordered_map<std::string, std::string> result;
            if (defaultsJson.empty())
            {
                return result;
            }

            try
            {
                simdjson::ondemand::parser parser;
                simdjson::padded_string padded(defaultsJson);
                simdjson::ondemand::document doc = parser.iterate(padded);
                simdjson::ondemand::object root = doc.get_object().value();

                for (auto field : root)
                {
                    std::string_view const key = field.unescaped_key().value();
                    simdjson::ondemand::value val = field.value();
                    simdjson::ondemand::json_type const type = val.type().value();

                    if (type == simdjson::ondemand::json_type::object)
                    {
                        simdjson::ondemand::object nested = val.get_object().value();
                        for (auto nestedField : nested)
                        {
                            std::string_view const nk = nestedField.unescaped_key().value();
                            simdjson::ondemand::value nv = nestedField.value();
                            simdjson::ondemand::json_type const nt = nv.type().value();

                            std::string fullKey = "defaults." + std::string(key) + "." + std::string(nk);

                            if (nt == simdjson::ondemand::json_type::string)
                            {
                                result[fullKey] = std::string(nv.get_string().value());
                            }
                            else if (nt == simdjson::ondemand::json_type::number)
                            {
                                result[fullKey] = std::to_string(nv.get_int64().value());
                            }
                        }
                    }
                    else if (type == simdjson::ondemand::json_type::string)
                    {
                        result["defaults." + std::string(key)] = std::string(val.get_string().value());
                    }
                    else if (type == simdjson::ondemand::json_type::number)
                    {
                        result["defaults." + std::string(key)] = std::to_string(val.get_int64().value());
                    }
                }
            }
            catch (...)
            {
                // Silently fall through — result will be empty or partial.
            }

            return result;
        }

        static std::string ExpandWithDefaults(std::string const& raw,
                                              std::unordered_map<std::string, std::string> const& defaultsMap)
        {
            if (raw.find("{{") == std::string::npos || defaultsMap.empty())
            {
                return raw;
            }

            TemplateContext ctx{};
            ctx.m_InputValues = &defaultsMap;

            std::string expanded;
            std::string error;
            if (ExpandTemplate(raw, ctx, TemplateMode::Lenient, expanded, error))
            {
                return expanded;
            }
            return raw;
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

                LOG_APP_INFO("[paths debug] debug reason=writeInlineQueueBindingFile filePathRelative='{}' wasRelative='{}' "
                             "bytes={} ",
                             fileRef.m_Path, std::filesystem::path(fileRef.m_Path).is_relative(), fileRef.m_Content.size());

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

        static bool StartsWith(std::string const& value, std::string const& prefix) { return value.rfind(prefix, 0) == 0; }

        static bool ContainsGlobChars(std::string const& path)
        {
            return path.find('*') != std::string::npos || path.find('?') != std::string::npos;
        }

        // Simple glob matcher supporting '*' (any sequence) and '?' (single char).
        static bool MatchGlob(std::string const& pattern, std::string const& text)
        {
            size_t pIdx = 0;
            size_t tIdx = 0;
            size_t starP = std::string::npos;
            size_t starT = 0;

            while (tIdx < text.size())
            {
                if (pIdx < pattern.size() && (pattern[pIdx] == text[tIdx] || pattern[pIdx] == '?'))
                {
                    ++pIdx;
                    ++tIdx;
                }
                else if (pIdx < pattern.size() && pattern[pIdx] == '*')
                {
                    starP = pIdx;
                    starT = tIdx;
                    ++pIdx;
                }
                else if (starP != std::string::npos)
                {
                    pIdx = starP + 1;
                    ++starT;
                    tIdx = starT;
                }
                else
                {
                    return false;
                }
            }

            while (pIdx < pattern.size() && pattern[pIdx] == '*')
            {
                ++pIdx;
            }

            return pIdx == pattern.size();
        }

        // Expand glob patterns in cntx_files into individual QueueFileRef entries.
        // Non-glob entries are passed through unchanged.
        static bool ExpandCntxFileGlobs(std::filesystem::path const& taskWorkingDirectoryPath,
                                        std::vector<AIAssistant::QueueFileRef> const& cntxFiles,
                                        std::vector<AIAssistant::QueueFileRef>& outExpanded, std::string& outErrorMessage)
        {
            outExpanded.clear();

            for (AIAssistant::QueueFileRef const& fileRef : cntxFiles)
            {
                if (fileRef.m_HasInlineContent || !ContainsGlobChars(fileRef.m_Path))
                {
                    outExpanded.push_back(fileRef);
                    continue;
                }

                // Resolve the glob path relative to the task working directory.
                std::filesystem::path globPath(fileRef.m_Path);
                if (!globPath.is_absolute())
                {
                    globPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, globPath);
                }
                else
                {
                    globPath = globPath.lexically_normal();
                }

                std::filesystem::path const parentDir = globPath.parent_path();
                std::string const filenamePattern = globPath.filename().string();

                if (!std::filesystem::is_directory(parentDir))
                {
                    outErrorMessage = "cntx_files glob directory does not exist: " + parentDir.string();
                    return false;
                }

                // Collect matching entries, then sort for deterministic ordering.
                std::vector<std::filesystem::path> matches;
                for (auto const& entry : std::filesystem::directory_iterator(parentDir))
                {
                    if (!entry.is_regular_file())
                    {
                        continue;
                    }

                    if (MatchGlob(filenamePattern, entry.path().filename().string()))
                    {
                        matches.push_back(entry.path());
                    }
                }

                std::sort(matches.begin(), matches.end());

                LOG_APP_INFO("[paths debug] debug reason=expandCntxGlob pattern='{}' parentDir='{}' matchCount={}",
                             fileRef.m_Path, parentDir.string(), matches.size());

                if (matches.empty())
                {
                    LOG_APP_WARN("cntx_files glob matched zero files: {}", fileRef.m_Path);
                }

                for (std::filesystem::path const& matchPath : matches)
                {
                    AIAssistant::QueueFileRef expanded{};
                    expanded.m_Path = matchPath.lexically_normal().string();
                    expanded.m_HasInlineContent = false;
                    outExpanded.push_back(std::move(expanded));
                }
            }

            return true;
        }

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

                // Strip ".output" from the stem so the materialized CNTX file is not
                // ignored by the FileCategorizer (which treats *.output.* as output files).
                // Example: PROB_NVDA.output.txt  →  CNTX_PROB_NVDA.txt
                std::string baseName;
                {
                    std::filesystem::path const srcFilename = sourcePath.filename();
                    std::string stem = srcFilename.stem().string();
                    std::string ext = srcFilename.extension().string();

                    if (stem.size() > 7 && stem.ends_with(".output"))
                    {
                        stem.erase(stem.size() - 7); // remove ".output"
                    }

                    baseName = stem + ext;
                }

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

                LOG_APP_INFO("[paths debug] debug reason=materializeCntxFile sourcePathRelative='{}' "
                             "sourcePathAbsolute='{}' destPathAbsolute='{}'",
                             fileRef.m_Path, sourcePath.lexically_normal().generic_string(),
                             destPath.lexically_normal().generic_string());
                if (!AiCallTaskExecutor::WriteTextFile(destPath.string(), sourceText, outErrorMessage))
                {
                    return false;
                }
            }

            return true;
        }

        // Materialize non-inline PROB file references into the working directory.
        // After materialization the QueueFileRef entries are updated in-place so that
        // the downstream expectedOutputPath logic (which checks m_HasInlineContent)
        // can find them.
        static bool MaterializeProbFilesFromQueueBinding(std::filesystem::path const& taskWorkingDirectoryPath,
                                                         std::vector<AIAssistant::QueueFileRef>& probFiles,
                                                         std::string& outErrorMessage)
        {
            std::unordered_set<std::string> usedFilenames;

            for (size_t index = 0; index < probFiles.size(); ++index)
            {
                AIAssistant::QueueFileRef& fileRef = probFiles[index];

                // Inline PROB files are handled by WriteInlineQueueBindingFiles().
                if (fileRef.m_HasInlineContent)
                {
                    continue;
                }

                if (fileRef.m_Path.empty())
                {
                    outErrorMessage = "queue_binding contains PROB file with empty 'path'";
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
                    errorStream << "Missing PROB source '" << sourcePath.string() << "': " << outErrorMessage;
                    outErrorMessage = errorStream.str();
                    return false;
                }

                // Build destination filename with PROB_ prefix.
                std::string baseName;
                {
                    std::filesystem::path const srcFilename = sourcePath.filename();
                    baseName = srcFilename.string();
                }

                if (!StartsWith(baseName, "PROB_"))
                {
                    baseName = "PROB_" + baseName;
                }

                if (usedFilenames.find(baseName) != usedFilenames.end())
                {
                    std::ostringstream renamed;
                    renamed << "PROB_" << index << "_" << sourcePath.filename().string();
                    baseName = renamed.str();
                }
                usedFilenames.insert(baseName);

                std::filesystem::path const destPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, baseName);

                LOG_APP_INFO("[paths debug] debug reason=materializeProbFile sourcePathRelative='{}' "
                             "sourcePathAbsolute='{}' destPathAbsolute='{}'",
                             fileRef.m_Path, sourcePath.lexically_normal().generic_string(),
                             destPath.lexically_normal().generic_string());
                if (!AiCallTaskExecutor::WriteTextFile(destPath.string(), sourceText, outErrorMessage))
                {
                    return false;
                }

                // Update the ref so downstream logic treats it as inline.
                fileRef.m_Path = destPath.string();
                fileRef.m_Content = sourceText;
                fileRef.m_HasInlineContent = true;
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

        std::filesystem::path const filesystemPathAbsolute = std::filesystem::absolute(filesystemPath).lexically_normal();
        LOG_APP_INFO("[paths debug] debug reason=writeTextFile filePathRelative='{}' filePathAbsolute='{}' wasRelative='{}'",
                     filePath, filesystemPathAbsolute.generic_string(), filesystemPath.is_relative());
        std::error_code errorCode;

        std::filesystem::path const parentPath = filesystemPath.parent_path();
        if (!parentPath.empty())
        {
            std::error_code existsBeforeErrorCode;
            bool const existedBefore = std::filesystem::exists(parentPath, existsBeforeErrorCode);
            std::filesystem::path const parentPathAbsolute = std::filesystem::absolute(parentPath).lexically_normal();
            LOG_APP_INFO("[folder creation debug] debug create_directories attempt path='{}' reason='aiCallTaskExecutor "
                         "writeTextFile parent' existedBefore='{}' existsBeforeEc='{}' existsBeforeMsg='{}'",
                         parentPathAbsolute.generic_string(), existedBefore, existsBeforeErrorCode.value(),
                         existsBeforeErrorCode.message());
            std::filesystem::create_directories(parentPath, errorCode);
            if (errorCode)
            {
                LOG_APP_INFO("[folder creation debug] debug create_directories failed path='{}' ec='{}' message='{}' "
                             "reason='aiCallTaskExecutor writeTextFile parent'",
                             parentPathAbsolute.generic_string(), errorCode.value(), errorCode.message());
                outErrorMessage = "failed to create directories for: " + filePath + " (" + errorCode.message() + ")";
                return false;
            }
            if (!errorCode)
            {
                std::error_code existsAfterErrorCode;
                bool const existsAfter = std::filesystem::exists(parentPath, existsAfterErrorCode);
                bool const created = (!existedBefore && existsAfter && !existsAfterErrorCode);
                LOG_APP_INFO("[folder creation debug] debug create_directories ok path='{}' created='{}' existsAfter='{}' "
                             "existsAfterEc='{}' existsAfterMsg='{}'",
                             parentPathAbsolute.generic_string(), created, existsAfter, existsAfterErrorCode.value(),
                             existsAfterErrorCode.message());
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
        TemplateContext context;
        context.m_InputValues = &taskState.m_InputValues;

        std::string expandedOut;
        std::string errorMessage;
        ExpandTemplate(templateText, context, TemplateMode::Lenient, expandedOut, errorMessage);
        return expandedOut;
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
        // Write all environment artifacts (STNG/TASK/CNTX) and PROB requirement files.
        // The SessionManager will categorize them and dispatch AI queries for PROB files.
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

        if (!WriteInlineQueueFileRefs(queueBinding.m_ProbFiles, outErrorMessage))
        {
            return false;
        }

        return true;
    }

    bool AiCallTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                     TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        std::string errorMessage;

        std::string launchCWDAbsolute;
        if (Core::g_Core != nullptr)
        {
            launchCWDAbsolute = Core::g_Core->GetLaunchCWDAbsolute().string();
        }

        LOG_APP_INFO(
            "[paths debug] debug reason=spawnAiCallTask workflowId='{}' runId='{}' taskId='{}' "
            "workflowFilePathRelative='{}' workflowFilePathAbsolute='{}' workflowFileDirectoryRelative='{}' "
            "workflowFileDirectoryAbsolute='{}' workflowBaseDirectoryRelative='{}' workflowBaseDirectoryAbsolute='{}' "
            "launchCWDAbsolute='{}'",
            workflowDefinition.m_Id, workflowRun.m_RunId, taskDefinition.m_Id, workflowDefinition.m_WorkflowFilePath,
            workflowDefinition.m_WorkflowFilePathAbsolute, workflowDefinition.m_WorkflowFileDirectory,
            workflowDefinition.m_WorkflowFileDirectoryAbsolute, workflowDefinition.m_WorkflowBaseDirectory,
            workflowDefinition.m_WorkflowBaseDirectoryAbsolute, launchCWDAbsolute);

        // ------------------------------------------------------------
        // Resolve workflow base directory (directory containing the loaded .jcwf file)
        // ------------------------------------------------------------
        std::filesystem::path const workflowBaseDirectoryPath =
            TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);

        LOG_APP_INFO("[paths debug] debug reason=resolveWorkflowBaseDirectory workflowId='{}' runId='{}' "
                     "selectedWorkflowBaseDirectory='{}'",
                     workflowDefinition.m_Id, workflowRun.m_RunId,
                     workflowBaseDirectoryPath.lexically_normal().generic_string());
        if (workflowBaseDirectoryPath.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "workflow base directory is empty (WorkflowDefinition not populated by loader)";
            return false;
        }

        std::filesystem::path const taskWorkingDirectoryPath =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPath, taskDefinition.m_WorkingDirectory);

        LOG_APP_INFO("[paths debug] debug reason=resolveTaskWorkingDirectory workflowId='{}' runId='{}' taskId='{}' "
                     "taskWorkingDirectoryRelative='{}' taskWorkingDirectoryAbsolute='{}'",
                     workflowDefinition.m_Id, workflowRun.m_RunId, taskDefinition.m_Id, taskDefinition.m_WorkingDirectory,
                     taskWorkingDirectoryPath.lexically_normal().generic_string());

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

        std::string const taskIdForBinding =
            taskState.m_TaskInstanceId.empty() ? taskDefinition.m_Id : taskState.m_TaskInstanceId;
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

        // Per-item tasks: substitute {{binding.field}} placeholders in inline paths BEFORE localization
        if (!taskState.m_InputValues.empty())
        {
            auto const substituteInlinePaths = [&](std::vector<QueueFileRef>& fileRefs)
            {
                for (QueueFileRef& fileRef : fileRefs)
                {
                    if (fileRef.m_HasInlineContent && !fileRef.m_Path.empty())
                    {
                        fileRef.m_Path = ApplySimpleTemplate(fileRef.m_Path, taskState);
                    }
                }
            };

            substituteInlinePaths(localizedQueueBinding.m_StngFiles);
            substituteInlinePaths(localizedQueueBinding.m_TaskFiles);
            substituteInlinePaths(localizedQueueBinding.m_CntxFiles);
            substituteInlinePaths(localizedQueueBinding.m_ProbFiles);
        }

        localizeInlineFileRefs(localizedQueueBinding.m_StngFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_TaskFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_CntxFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_ProbFiles);

        // Per-item tasks: substitute {{binding.field}} placeholders in inline content
        if (!taskState.m_InputValues.empty())
        {
            auto const substituteInlineContent = [&](std::vector<QueueFileRef>& fileRefs)
            {
                for (QueueFileRef& fileRef : fileRefs)
                {
                    if (fileRef.m_HasInlineContent && !fileRef.m_Content.empty())
                    {
                        fileRef.m_Content = ApplySimpleTemplate(fileRef.m_Content, taskState);
                    }
                }
            };

            substituteInlineContent(localizedQueueBinding.m_StngFiles);
            substituteInlineContent(localizedQueueBinding.m_TaskFiles);
            substituteInlineContent(localizedQueueBinding.m_CntxFiles);
            substituteInlineContent(localizedQueueBinding.m_ProbFiles);
        }

        // ------------------------------------------------------------
        // Determine expected output path from the first PROB file.
        // The SessionManager writes output as <stem>.output.<ext>.
        // Computed before file writes so we can register with the pool early.
        // ------------------------------------------------------------
        std::string expectedOutputPath;
        for (auto const& probFile : localizedQueueBinding.m_ProbFiles)
        {
            if (!probFile.m_Path.empty())
            {
                std::filesystem::path probPath(probFile.m_Path);

                // For non-inline (path-reference) PROB files, resolve relative to
                // the task working directory — they will be materialized into
                // the working directory later by MaterializeProbFilesFromQueueBinding.
                if (!probFile.m_HasInlineContent)
                {
                    if (!probPath.is_absolute())
                    {
                        probPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, probPath);
                    }
                    // Build the destination filename the same way MaterializeProbFilesFromQueueBinding does.
                    std::string baseName = probPath.filename().string();
                    if (baseName.rfind("PROB_", 0) != 0)
                    {
                        baseName = "PROB_" + baseName;
                    }
                    probPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, baseName);
                }

                std::filesystem::path outputPath = probPath;
                outputPath.replace_filename(probPath.stem().string() + ".output" + probPath.extension().string());
                expectedOutputPath = outputPath.lexically_normal().generic_string();
                break;
            }
        }

        if (expectedOutputPath.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ai_call has no prob_files — cannot determine expected output path";
            return false;
        }

        LOG_APP_INFO("[paths debug] debug reason=resolveExpectedOutput workflowId='{}' runId='{}' taskId='{}' "
                     "expectedOutputPath='{}'",
                     workflowDefinition.m_Id, workflowRun.m_RunId, taskIdForBinding, expectedOutputPath);

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
            outputPathText = outputPath.lexically_normal().generic_string();
        }

        // ------------------------------------------------------------
        // Register with AiRequestPool BEFORE writing queue files.
        // This starts the file-activity watchdog so that stalled
        // file placement or missing environment files are caught
        // within seconds instead of waiting for the full timeout.
        // ------------------------------------------------------------
        int64_t const requestId = requestPool->AllocateRequestId();
        int64_t const timestampNs = NowTimestampNs();

        AiRequestHandle requestHandle{};
        requestHandle.requestId = requestId;
        requestHandle.requestTimestampNs = timestampNs;

        AiRequestHandle const registered = requestPool->RegisterPendingWorkflowTask(
            requestHandle, workflowRun.m_WorkflowId, workflowRun.m_RunId, taskIdForBinding, resolvedFileOutputs,
            outputSlotNames, taskDefinition.m_TimeoutMs, expectedOutputPath);

        if (!registered.IsValid())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "AiRequestPool::RegisterPendingWorkflowTask failed";
            return false;
        }

        // ------------------------------------------------------------
        // Per-subfolder provider settings (optional).
        // MUST be written BEFORE any other queue file (STNG, CNTX,
        // TASK, PROB) so that the SessionManager sees the override
        // before the environment becomes complete.
        // ------------------------------------------------------------
        {
            std::string providerOverrideError;
            std::optional<std::string> rawProviderOpt =
                TryExtractStringParam(taskDefinition.m_ParamsJson, "provider", providerOverrideError);
            std::optional<std::string> rawModelOpt =
                TryExtractStringParam(taskDefinition.m_ParamsJson, "model", providerOverrideError);
            std::optional<std::string> const temperatureOpt =
                TryExtractStringParam(taskDefinition.m_ParamsJson, "temperature", providerOverrideError);

            // Expand {{defaults.*}} templates in provider/model params.
            auto const defaultsMap = BuildDefaultsMap(workflowDefinition.m_DefaultsJson);
            std::optional<std::string> const providerOpt =
                rawProviderOpt ? std::optional(ExpandWithDefaults(*rawProviderOpt, defaultsMap)) : std::nullopt;
            std::optional<std::string> const modelOpt =
                rawModelOpt ? std::optional(ExpandWithDefaults(*rawModelOpt, defaultsMap)) : std::nullopt;

            if (providerOpt.has_value())
            {
                std::filesystem::path const providerSettingsPath = taskWorkingDirectoryPath / "PROV_provider.json";

                if (!std::filesystem::exists(providerSettingsPath))
                {
                    // Warn if the folder already has files — PROV must come first.
                    if (std::filesystem::exists(taskWorkingDirectoryPath) &&
                        !std::filesystem::is_empty(taskWorkingDirectoryPath))
                    {
                        LOG_APP_WARN("[ai_call] PROV file for task '{}' is being placed after other files "
                                     "already exist in '{}'. PROV should always be the first file in the queue folder.",
                                     taskIdForBinding, taskWorkingDirectoryPath.string());
                    }

                    // The JCWF "provider" field is an interface name from config.json
                    // (e.g. "api.openai.com/gpt-4.1-mini/API1"). Look it up by name to get
                    // URL, model, API type, and key_name. The PROV file stores the key_name
                    // as "provider" so the SessionManager can resolve the API key.
                    std::string resolvedUrl;
                    std::string resolvedApiType;
                    std::string effectiveModel;
                    std::string keyNameForProv; // key_name from interface → goes into PROV "provider"
                    {
                        auto const& interfaces = Core::g_Core->GetConfig().m_ApiInterfaces;

                        // Primary lookup: match by interface name.
                        for (auto const& iface : interfaces)
                        {
                            if (iface.m_Name == providerOpt.value())
                            {
                                resolvedUrl = iface.m_Url;
                                effectiveModel = modelOpt.value_or(iface.m_Model);
                                keyNameForProv = iface.m_KeyName;
                                switch (iface.m_InterfaceType)
                                {
                                    case ConfigParser::EngineConfig::InterfaceType::API1:
                                        resolvedApiType = "API1";
                                        break;
                                    case ConfigParser::EngineConfig::InterfaceType::API2:
                                        resolvedApiType = "API2";
                                        break;
                                    case ConfigParser::EngineConfig::InterfaceType::API3:
                                        resolvedApiType = "API3";
                                        break;
                                    default:
                                        break;
                                }
                                break;
                            }
                        }

                        if (resolvedUrl.empty())
                        {
                            LOG_APP_WARN("[ai_call] PROV: provider '{}' does not match any "
                                         "interface name in config.json for task '{}'",
                                         providerOpt.value(), taskIdForBinding);
                        }
                    }

                    std::string sidecarJson = "{";
                    // Store key_name (not interface name) so SessionManager can resolve the API key.
                    std::string const& provForSidecar = keyNameForProv.empty() ? providerOpt.value() : keyNameForProv;
                    sidecarJson += "\"provider\":\"" + provForSidecar + "\"";

                    if (!resolvedUrl.empty())
                    {
                        sidecarJson += ",\"url\":\"" + resolvedUrl + "\"";
                    }

                    if (!resolvedApiType.empty())
                    {
                        sidecarJson += ",\"api_type\":\"" + resolvedApiType + "\"";
                    }

                    if (!effectiveModel.empty())
                    {
                        sidecarJson += ",\"model\":\"" + effectiveModel + "\"";
                    }

                    if (temperatureOpt.has_value())
                    {
                        sidecarJson += ",\"temperature\":" + temperatureOpt.value();
                    }

                    sidecarJson += "}";

                    std::string sidecarError;
                    if (!WriteTextFile(providerSettingsPath.string(), sidecarJson, sidecarError))
                    {
                        LOG_APP_WARN("Failed to write provider settings '{}': {}", providerSettingsPath.string(),
                                     sidecarError);
                    }
                }
            }
        }

        // ------------------------------------------------------------
        // Write queue files.  Kick the file-activity watchdog after
        // each step so the 5 s inactivity window restarts.
        // ------------------------------------------------------------
        if (!WriteInlineQueueBindingFiles(localizedQueueBinding, errorMessage))
        {
            requestPool->Forget(requestHandle);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }
        requestPool->KickFileActivityWatchdog(requestHandle);

        // Expand glob patterns (e.g. "../01_lookupDividend/PROB_*.output.txt") into
        // individual file references before materialization.
        std::vector<QueueFileRef> expandedCntxFiles;
        if (!ExpandCntxFileGlobs(taskWorkingDirectoryPath, localizedQueueBinding.m_CntxFiles, expandedCntxFiles,
                                 errorMessage))
        {
            requestPool->Forget(requestHandle);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        if (!MaterializeCntxFilesFromQueueBinding(taskWorkingDirectoryPath, expandedCntxFiles, errorMessage))
        {
            requestPool->Forget(requestHandle);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }
        requestPool->KickFileActivityWatchdog(requestHandle);

        if (!MaterializeProbFilesFromQueueBinding(taskWorkingDirectoryPath, localizedQueueBinding.m_ProbFiles, errorMessage))
        {
            requestPool->Forget(requestHandle);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }
        requestPool->KickFileActivityWatchdog(requestHandle);

        // ------------------------------------------------------------
        // Direct envelope dispatch (§4 of AI dispatch refactor).
        // Concatenates STNG + CNTX + TASK + first PROB content into a
        // single user message and submits directly via AiRequestPool.
        // SessionManager's IsQueryRequired gates on IsDirectDispatchActive
        // and skips its file-event-driven DispatchQuery for these PROBs.
        // Completion still flows through the path-based OnOutputFileCreated
        // mechanism — Submit's callback writes <prob>.output.txt.
        // ------------------------------------------------------------
        {
            std::string combinedMessage;
            auto const appendContent = [&combinedMessage](std::vector<QueueFileRef> const& fileRefs)
            {
                for (auto const& fileRef : fileRefs)
                {
                    if (!fileRef.m_Content.empty())
                    {
                        if (!combinedMessage.empty())
                        {
                            combinedMessage += "\n";
                        }
                        combinedMessage += fileRef.m_Content;
                    }
                }
            };
            appendContent(localizedQueueBinding.m_StngFiles);
            appendContent(localizedQueueBinding.m_TaskFiles);
            appendContent(localizedQueueBinding.m_CntxFiles);

            bool hasStng = false;
            for (auto const& fileRef : localizedQueueBinding.m_StngFiles)
            {
                if (!fileRef.m_Content.empty()) { hasStng = true; break; }
            }
            bool hasTask = false;
            for (auto const& fileRef : localizedQueueBinding.m_TaskFiles)
            {
                if (!fileRef.m_Content.empty()) { hasTask = true; break; }
            }
            bool hasCntx = false;
            for (auto const& fileRef : localizedQueueBinding.m_CntxFiles)
            {
                if (!fileRef.m_Content.empty()) { hasCntx = true; break; }
            }
            if (!hasStng)
            {
                LOG_APP_INFO("[ai_call] no STNG content for task '{}' — default settings apply", taskIdForBinding);
            }
            if (!hasTask)
            {
                LOG_APP_INFO("[ai_call] no TASK content for task '{}' — relying on PROB self-description",
                             taskIdForBinding);
            }
            if (!hasCntx)
            {
                LOG_APP_INFO("[ai_call] no CNTX content for task '{}' — results may be less precise",
                             taskIdForBinding);
            }

            std::string probRelativeName;
            for (auto const& probFile : localizedQueueBinding.m_ProbFiles)
            {
                std::filesystem::path const probPath(probFile.m_Path);
                std::string baseName = probPath.filename().string();
                if (!probFile.m_HasInlineContent && baseName.rfind("PROB_", 0) != 0)
                {
                    baseName = "PROB_" + baseName;
                }
                probRelativeName = baseName;
                if (!probFile.m_Content.empty())
                {
                    if (!combinedMessage.empty())
                    {
                        combinedMessage += "\n";
                    }
                    combinedMessage += probFile.m_Content;
                }
                break;
            }

            bool hasNonWhitespace = false;
            for (char const character : combinedMessage)
            {
                if (!std::isspace(static_cast<unsigned char>(character)))
                {
                    hasNonWhitespace = true;
                    break;
                }
            }

            if (hasNonWhitespace && !probRelativeName.empty())
            {
                // Resolve per-task api_interface override (JCWF "provider" param), if any.
                std::string interfaceOverrideError;
                std::optional<std::string> const rawProviderOpt =
                    TryExtractStringParam(taskDefinition.m_ParamsJson, "provider", interfaceOverrideError);
                auto const defaultsMapEnvelope = BuildDefaultsMap(workflowDefinition.m_DefaultsJson);
                std::string envelopeInterfaceName;
                if (rawProviderOpt.has_value())
                {
                    envelopeInterfaceName = ExpandWithDefaults(*rawProviderOpt, defaultsMapEnvelope);
                }

                AiInvocation envelope;
                envelope.m_InterfaceName = envelopeInterfaceName;
                envelope.m_QueueFolder = taskWorkingDirectoryPath;
                envelope.m_ProbName = probRelativeName;
                if (!taskDefinition.m_OutputSchemaJson.empty())
                {
                    envelope.m_OutputSchemaJson = taskDefinition.m_OutputSchemaJson;
                }
                if (taskDefinition.m_OutputSchemaMaxAttempts > 0)
                {
                    envelope.m_Retry.m_OutputSchemaMaxAttempts =
                        static_cast<int>(taskDefinition.m_OutputSchemaMaxAttempts);
                }

                Message userMessage;
                userMessage.m_Role = MessageRole::User;
                userMessage.m_Content = std::move(combinedMessage);
                envelope.m_Messages.push_back(std::move(userMessage));

                // Chunking advisory (§8 Phase 6 — planner wired, fan-out is a follow-up).
                // Warn when the body likely overruns the interface's max_context_tokens.
                {
                    auto const& configForChunk = Core::g_Core->GetConfig();
                    uint64_t maxContextTokens = 0;
                    for (auto const& apiCandidate : configForChunk.m_ApiInterfaces)
                    {
                        bool const matchesRequested =
                            envelopeInterfaceName.empty()
                                ? (&apiCandidate == &configForChunk.m_ApiInterfaces[configForChunk.m_ApiIndex])
                                : (apiCandidate.m_Name == envelopeInterfaceName);
                        if (matchesRequested && apiCandidate.m_MaxContextTokens > 0)
                        {
                            maxContextTokens = apiCandidate.m_MaxContextTokens;
                            break;
                        }
                    }
                    if (maxContextTokens > 0)
                    {
                        uint64_t const estimatedTokens = ChunkPlanner::EstimateTokens(envelope.m_Messages.back().m_Content);
                        if (estimatedTokens > maxContextTokens)
                        {
                            LOG_APP_WARN("[ai_call] task '{}' prompt est ~{} tokens exceeds interface "
                                          "max_context_tokens={} — chunking fan-out not yet wired; request will be "
                                          "sent as-is and may be rejected by the provider",
                                          taskIdForBinding, estimatedTokens, maxContextTokens);
                        }
                    }
                }

                // Fire-and-forget — completion is handled via the path-based
                // OnOutputFileCreated matching when Submit's callback writes .output.txt.
                if (!requestPool->Submit(envelope, nullptr))
                {
                    LOG_APP_WARN("[ai_call] direct dispatch Submit rejected envelope for task '{}' — "
                                 "falling back to SessionManager file-event path",
                                 taskIdForBinding);
                }
            }
            else
            {
                LOG_APP_WARN("[ai_call] task '{}' has no PROB content or prob file — skipping direct dispatch",
                             taskIdForBinding);
            }
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
