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

        LOG_APP_INFO("[paths debug] debug reason=resolveWorkflowBaseDirectory workflowId='{}' runId='{}' "
                     "selectedWorkflowBaseDirectory='{}' selectedWasRelative='{}'",
                     workflowDefinition.m_Id, workflowRun.m_RunId,
                     workflowBaseDirectoryPath.lexically_normal().generic_string(), workflowBaseDirectoryPath.is_relative());
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

        if (!WriteInlineQueueBindingFiles(localizedQueueBinding, errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        // Expand glob patterns (e.g. "../01_lookupDividend/PROB_*.output.txt") into
        // individual file references before materialization.
        std::vector<QueueFileRef> expandedCntxFiles;
        if (!ExpandCntxFileGlobs(taskWorkingDirectoryPath, localizedQueueBinding.m_CntxFiles, expandedCntxFiles,
                                 errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        if (!MaterializeCntxFilesFromQueueBinding(taskWorkingDirectoryPath, expandedCntxFiles, errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        if (!MaterializeProbFilesFromQueueBinding(taskWorkingDirectoryPath, localizedQueueBinding.m_ProbFiles, errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        // ------------------------------------------------------------
        // Determine expected output path from the first PROB file.
        // The SessionManager writes output as <stem>.output.<ext>.
        // ------------------------------------------------------------
        std::string expectedOutputPath;
        for (auto const& probFile : localizedQueueBinding.m_ProbFiles)
        {
            if (probFile.m_HasInlineContent && !probFile.m_Path.empty())
            {
                std::filesystem::path const probPath(probFile.m_Path);
                std::filesystem::path outputPath = probPath;
                outputPath.replace_filename(probPath.stem().string() + ".output" + probPath.extension().string());
                expectedOutputPath = outputPath.lexically_normal().generic_string();
                break;
            }
        }

        if (expectedOutputPath.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ai_call has no inline prob_files — cannot determine expected output path";
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
        // Register with AiRequestPool for path-based completion routing.
        // PROB files are already written by WriteInlineQueueBindingFiles;
        // the SessionManager dispatches the AI query and writes the .output.txt.
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
        // Per-subfolder provider settings (optional)
        // If the task params contain "provider", resolve the full
        // provider config and write a PROV_provider.json to the
        // subfolder.  The file carries url (mandatory), model,
        // api_type, temperature — but NEVER credentials / API key.
        // The "provider" field is the KeyManager lookup ID so that
        // SessionManager can retrieve the key at runtime.
        // Written only once per subfolder (idempotent).
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
                    // Resolve provider config from KeyManager for non-credential fields
                    auto const* provCfg = Core::g_Core->GetKeyManager().GetProvider(providerOpt.value());

                    std::string sidecarJson = "{";
                    // provider (KeyManager lookup ID)
                    sidecarJson += "\"provider\":\"" + providerOpt.value() + "\"";

                    // url (mandatory — from KeyManager endpoint)
                    if (provCfg && !provCfg->m_Endpoint.empty())
                    {
                        sidecarJson += ",\"url\":\"" + provCfg->m_Endpoint + "\"";
                    }

                    // api_type (from KeyManager)
                    if (provCfg && !provCfg->m_ApiType.empty())
                    {
                        sidecarJson += ",\"api_type\":\"" + provCfg->m_ApiType + "\"";
                    }

                    // model (task param overrides provider default)
                    if (modelOpt.has_value())
                    {
                        sidecarJson += ",\"model\":\"" + modelOpt.value() + "\"";
                    }
                    else if (provCfg && !provCfg->m_DefaultModel.empty())
                    {
                        sidecarJson += ",\"model\":\"" + provCfg->m_DefaultModel + "\"";
                    }

                    // temperature (optional, from task params)
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
        // Asynchronous completion (event-driven)
        // ------------------------------------------------------------
        taskState.m_ExternalRequestId = requestId;
        taskState.m_ExternalRequestTimestampNs = timestampNs;

        taskState.m_State = TaskInstanceStateKind::WaitingExternal;
        taskState.m_LastErrorMessage.clear();

        return true;
    }
} // namespace AIAssistant
