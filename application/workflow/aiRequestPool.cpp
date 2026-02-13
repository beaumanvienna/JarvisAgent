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

#include "engine.h"
#include "workflow/aiRequestPool.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "file/probUtils.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {
        uint64_t const kDefaultTimeoutMs = 300000; // 5 minutes

        bool WriteTextFile(std::string const& filePath, std::string const& fileContent, std::string& outErrorMessage)
        {
            outErrorMessage.clear();

            fs::path const filesystemPath(filePath);

            if (filesystemPath.is_relative())
            {
                std::string const launchCWDAbsoluteText =
                    (Core::g_Core != nullptr) ? Core::g_Core->GetLaunchCWDAbsolute().string() : "<null>";
                LOG_APP_INFO("[paths debug] debug reason=writeAiRequestPoolTextFileCwdFallback filePathRelative='{}' "
                             "launchCWDAbsolute='{}'",
                             filePath, launchCWDAbsoluteText);
            }

            fs::path const filesystemPathAbsolute = fs::absolute(filesystemPath).lexically_normal();
            LOG_APP_INFO("[paths debug] debug reason=writeAiRequestPoolTextFile filePathRelative='{}' filePathAbsolute='{}' "
                         "wasRelative='{}'",
                         filePath, filesystemPathAbsolute.generic_string(), filesystemPath.is_relative());
            std::error_code errorCode;

            fs::path const parent = filesystemPath.parent_path();
            fs::path const parentAbsolute = filesystemPathAbsolute.parent_path();
            if (!parent.empty())
            {
                std::error_code existsBeforeErrorCode;
                bool const existedBefore = fs::exists(parentAbsolute, existsBeforeErrorCode);

                LOG_APP_INFO("[folder creation debug] debug create_directories attempt path='{}' reason='aiRequestPool "
                             "parent directory'",
                             parentAbsolute.generic_string());

                fs::create_directories(parent, errorCode);
                if (errorCode)
                {
                    LOG_APP_INFO("[folder creation debug] debug create_directories failed path='{}' ec={} message='{}' "
                                 "reason='aiRequestPool parent directory'",
                                 parentAbsolute.generic_string(), errorCode.value(), errorCode.message());
                    outErrorMessage = "failed to create directories for: " + filePath + " (" + errorCode.message() + ")";
                    return false;
                }

                std::error_code existsAfterErrorCode;
                bool const existsAfter = fs::exists(parentAbsolute, existsAfterErrorCode);
                bool const created = (!existedBefore && existsAfter);
                LOG_APP_INFO("[folder creation debug] debug create_directories ok path='{}' created={}",
                             parentAbsolute.generic_string(), created);
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

        void BuildCompletionOutputs(std::vector<std::string> const& outputFilePaths,
                                    std::vector<std::string> const& outputSlotNames, std::string const& responseText,
                                    std::string const& sourceOutputFilePath, bool& outWasFailed,
                                    std::string& outErrorMessage,
                                    std::unordered_map<std::string, std::string>& outOutputValues)
        {
            outWasFailed = false;
            outErrorMessage.clear();
            outOutputValues.clear();

            // Explicit file outputs: write response to those paths and map slots -> paths.
            if (!outputFilePaths.empty())
            {
                for (std::string const& outputPath : outputFilePaths)
                {
                    fs::path const outputFilesystemPath(outputPath);
                    fs::path const outputFilesystemPathAbsolute = fs::absolute(outputFilesystemPath).lexically_normal();
                    LOG_APP_INFO("[paths debug] debug reason=writeAiCompletionOutput outputPathRelative='{}' "
                                 "outputPathAbsolute='{}' wasRelative='{}'",
                                 outputPath, outputFilesystemPathAbsolute.generic_string(),
                                 outputFilesystemPath.is_relative());

                    std::string writeError;
                    if (!WriteTextFile(outputPath, responseText, writeError))
                    {
                        outWasFailed = true;
                        outErrorMessage = writeError;
                        return;
                    }
                }

                // Deterministic slot mapping.
                if (!outputSlotNames.empty() && outputSlotNames.size() == outputFilePaths.size())
                {
                    for (size_t index = 0; index < outputSlotNames.size(); ++index)
                    {
                        outOutputValues[outputSlotNames[index]] = outputFilePaths[index];
                    }
                    return;
                }

                if (outputFilePaths.size() == 1 && !outputSlotNames.empty())
                {
                    std::string const onlyPath = outputFilePaths[0];
                    for (std::string const& slotName : outputSlotNames)
                    {
                        outOutputValues[slotName] = onlyPath;
                    }
                    return;
                }

                if (outputSlotNames.size() == 1)
                {
                    outOutputValues[outputSlotNames[0]] = outputFilePaths[0];
                    return;
                }

                outOutputValues["file"] = outputFilePaths[0];
                return;
            }

            // No explicit file outputs: use the source .output.txt path created by the core engine.
            // Output values are always file paths (never raw text in memory).
            if (!sourceOutputFilePath.empty())
            {
                if (outputSlotNames.size() == 1)
                {
                    outOutputValues[outputSlotNames[0]] = sourceOutputFilePath;
                    return;
                }

                for (std::string const& slotName : outputSlotNames)
                {
                    outOutputValues[slotName] = sourceOutputFilePath;
                }

                if (outOutputValues.empty())
                {
                    outOutputValues["file"] = sourceOutputFilePath;
                }
                return;
            }

            // No file outputs and no source path (should not happen in normal operation).
            LOG_APP_WARN("BuildCompletionOutputs: no file outputs and no source output file path; "
                         "output values will be empty");
        }
    } // namespace

    AiRequestPool::RequestKey AiRequestPool::MakeKey(AiRequestHandle const& requestHandle)
    {
        RequestKey requestKey{};
        requestKey.requestId = requestHandle.requestId;
        requestKey.requestTimestampNs = requestHandle.requestTimestampNs;
        return requestKey;
    }

    int64_t AiRequestPool::AllocateRequestId()
    {
        std::scoped_lock<std::mutex> const lock(m_IdMutex);

        int64_t const requestId = m_NextRequestId;
        m_NextRequestId++;

        if (m_NextRequestId <= 0)
        {
            // Wrap-around protection (extremely unlikely).
            m_NextRequestId = 1;
        }

        return requestId;
    }

    AiRequestHandle AiRequestPool::RegisterPending(AiRequestHandle const& requestHandle)
    {
        return RegisterPending(requestHandle, kDefaultTimeoutMs);
    }

    AiRequestHandle AiRequestPool::RegisterPending(AiRequestHandle const& requestHandle, uint64_t const timeoutMs)
    {
        if (!requestHandle.IsValid())
        {
            return {};
        }

        std::shared_ptr<PendingEntry> pendingEntry = std::make_shared<PendingEntry>();

        uint64_t const effectiveTimeoutMs = (timeoutMs > 0) ? timeoutMs : kDefaultTimeoutMs;

        pendingEntry->m_Handle = requestHandle;
        pendingEntry->m_HasDeadline = true;
        pendingEntry->m_Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(effectiveTimeoutMs);

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);
            m_PendingRequests[MakeKey(requestHandle)] = pendingEntry;
        }

        return requestHandle;
    }

    AiRequestHandle AiRequestPool::RegisterPendingWorkflowTask(
        AiRequestHandle const& requestHandle, std::string const& workflowId, std::string const& runId,
        std::string const& taskId, std::vector<std::string> const& outputFilePaths,
        std::vector<std::string> const& outputSlotNames, uint64_t const timeoutMs, std::string const& expectedOutputPath)
    {
        if (!requestHandle.IsValid())
        {
            return {};
        }

        AiRequestHandle const registered = RegisterPending(requestHandle, timeoutMs);
        if (!registered.IsValid())
        {
            return {};
        }

        std::shared_ptr<PendingEntry> pendingEntry;

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);

            auto const iterator = m_PendingRequests.find(MakeKey(requestHandle));
            if (iterator == m_PendingRequests.end())
            {
                return {};
            }

            pendingEntry = iterator->second;
        }

        std::vector<std::string> sortedSlotNames = outputSlotNames;
        std::sort(sortedSlotNames.begin(), sortedSlotNames.end());

        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);

            pendingEntry->m_Context.m_HasWorkflowBinding = (!workflowId.empty() && !runId.empty() && !taskId.empty());
            pendingEntry->m_Context.m_WorkflowId = workflowId;
            pendingEntry->m_Context.m_RunId = runId;
            pendingEntry->m_Context.m_TaskId = taskId;

            pendingEntry->m_Context.m_OutputFilePaths = outputFilePaths;
            pendingEntry->m_Context.m_OutputSlotNames = std::move(sortedSlotNames);
            pendingEntry->m_Context.m_ExpectedOutputPath = expectedOutputPath;
        }

        // Register in path-based lookup map (for non-PROB_<id>_<ts> naming).
        if (!expectedOutputPath.empty())
        {
            std::string const canonicalPath = fs::absolute(fs::path(expectedOutputPath)).lexically_normal().generic_string();

            std::scoped_lock<std::mutex> const lock(m_OutputPathMutex);
            m_PendingByOutputPath[canonicalPath] = pendingEntry;

            LOG_APP_INFO("[AiRequestPool] registered expected output path: '{}'", canonicalPath);
        }

        // If the request had already completed (rare but possible), ensure we queue now that binding exists.
        QueueCompletionIfNeeded(pendingEntry);

        return registered;
    }

    void AiRequestPool::OnCurlDispatched(std::string const& probFilePath)
    {
        // Derive expected output path from PROB path: PROB_x.txt → PROB_x.output.txt
        fs::path const probPath(probFilePath);
        fs::path expectedOutputPath = probPath;
        expectedOutputPath.replace_filename(probPath.stem().string() + ".output" + probPath.extension().string());

        std::string const canonicalPath = fs::absolute(expectedOutputPath).lexically_normal().generic_string();

        std::shared_ptr<PendingEntry> pendingEntry;

        {
            std::scoped_lock<std::mutex> const lock(m_OutputPathMutex);

            auto const iterator = m_PendingByOutputPath.find(canonicalPath);
            if (iterator == m_PendingByOutputPath.end())
            {
                LOG_APP_INFO("[AiRequestPool] OnCurlDispatched: no pending request for '{}'", canonicalPath);
                return;
            }

            pendingEntry = iterator->second;
        }

        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
            pendingEntry->m_CurlDispatched = true;
        }

        LOG_APP_INFO("[AiRequestPool] OnCurlDispatched: curl issued for PROB '{}' (expected output '{}')", probFilePath,
                     canonicalPath);
    }

    bool AiRequestPool::OnOutputFileCreated(std::string const& fullFilePath)
    {
        std::string const canonicalPath = fs::absolute(fs::path(fullFilePath)).lexically_normal().generic_string();

        std::shared_ptr<PendingEntry> pendingEntry;

        {
            std::scoped_lock<std::mutex> const lock(m_OutputPathMutex);

            auto const iterator = m_PendingByOutputPath.find(canonicalPath);
            if (iterator == m_PendingByOutputPath.end())
            {
                return false;
            }

            pendingEntry = iterator->second;
            m_PendingByOutputPath.erase(iterator);
        }

        // Read file content.
        std::string fileContent;
        {
            std::ifstream inputStream(fullFilePath, std::ios::binary);
            if (!inputStream.is_open())
            {
                std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
                pendingEntry->m_IsCompleted = true;
                pendingEntry->m_IsFailed = true;
                pendingEntry->m_ErrorMessage = "ai_call output file could not be opened: " + fullFilePath;
                pendingEntry->conditionVariable.notify_all();
                QueueCompletionIfNeeded(pendingEntry);
                return true;
            }

            inputStream.seekg(0, std::ios::end);
            std::streamoff const size = inputStream.tellg();
            inputStream.seekg(0, std::ios::beg);

            if (size > 0)
            {
                fileContent.resize(static_cast<std::size_t>(size));
                inputStream.read(fileContent.data(), static_cast<std::streamsize>(size));
            }
        }

        LOG_APP_INFO("[AiRequestPool] OnOutputFileCreated matched path='{}' bytesRead={}", fullFilePath, fileContent.size());

        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
            pendingEntry->m_IsCompleted = true;
            pendingEntry->m_IsFailed = false;
            pendingEntry->m_ResponseText = std::move(fileContent);
            pendingEntry->m_SourceOutputFilePath = fullFilePath;
            pendingEntry->conditionVariable.notify_all();
        }

        QueueCompletionIfNeeded(pendingEntry);
        return true;
    }

    void AiRequestPool::QueueCompletionIfNeeded(std::shared_ptr<PendingEntry> const& pendingEntry)
    {
        if (pendingEntry == nullptr)
        {
            return;
        }

        AiRequestCompletion completion;
        bool shouldQueue = false;

        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);

            if (!pendingEntry->m_IsCompleted)
            {
                return;
            }

            if (!pendingEntry->m_Context.m_HasWorkflowBinding)
            {
                // Not workflow-bound: do not queue a completion.
                // (TryConsumeResult / WaitForCompletion can still be used for legacy call sites.)
                return;
            }

            if (pendingEntry->m_HasQueuedCompletion)
            {
                return;
            }

            completion.m_WorkflowId = pendingEntry->m_Context.m_WorkflowId;
            completion.m_RunId = pendingEntry->m_Context.m_RunId;
            completion.m_TaskId = pendingEntry->m_Context.m_TaskId;

            completion.m_WasFailed = pendingEntry->m_IsFailed;
            completion.m_ResponseText = pendingEntry->m_ResponseText;
            completion.m_ErrorMessage = pendingEntry->m_ErrorMessage;

            // Build deterministic output values (and write file outputs if configured).
            if (!completion.m_WasFailed)
            {
                bool writeFailed = false;
                std::string writeError;

                BuildCompletionOutputs(pendingEntry->m_Context.m_OutputFilePaths, pendingEntry->m_Context.m_OutputSlotNames,
                                       completion.m_ResponseText, pendingEntry->m_SourceOutputFilePath, writeFailed,
                                       writeError, completion.m_OutputValues);

                if (writeFailed)
                {
                    completion.m_WasFailed = true;
                    completion.m_ErrorMessage =
                        writeError.empty() ? "ai_call failed while writing file outputs" : writeError;
                    completion.m_OutputValues.clear();
                }
            }

            pendingEntry->m_HasQueuedCompletion = true;
            shouldQueue = true;
        }

        if (!shouldQueue)
        {
            return;
        }

        {
            std::scoped_lock<std::mutex> const lock(m_CompletedMutex);
            m_Completed.push(std::move(completion));
        }
    }

    bool AiRequestPool::OnProbFileEvent(ProbUtils::ProbFileInfo const& probFileInfo, std::string const& fullFilePath)
    {
        // We only care about output artifacts.
        if (!probFileInfo.isOutput)
        {
            return false;
        }

        // NOTE: ProbUtils::ProbFileInfo::timestamp is treated as "timestampNs" by convention in your codebase.
        RequestKey const requestKey{static_cast<int64_t>(probFileInfo.id), probFileInfo.timestamp};

        LOG_APP_INFO("[paths debug] debug reason=probFileEvent requestId='{}' timestampNs='{}' fullFilePathAbsolute='{}'",
                     requestKey.requestId, requestKey.requestTimestampNs, fullFilePath);

        std::shared_ptr<PendingEntry> pendingEntry;

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);

            auto const iterator = m_PendingRequests.find(requestKey);
            if (iterator == m_PendingRequests.end())
            {
                return false;
            }

            pendingEntry = iterator->second;
        }

        // Read file content. If file can't be read, fail this request but still consume the event.
        std::string fileContent;
        {
            std::ifstream inputStream(fullFilePath, std::ios::binary);
            if (!inputStream.is_open())
            {
                std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
                pendingEntry->m_IsCompleted = true;
                pendingEntry->m_IsFailed = true;
                pendingEntry->m_ErrorMessage = "ai_call output file could not be opened: " + fullFilePath;
                pendingEntry->conditionVariable.notify_all();
                QueueCompletionIfNeeded(pendingEntry);
                return true;
            }

            inputStream.seekg(0, std::ios::end);
            std::streamoff const size = inputStream.tellg();
            inputStream.seekg(0, std::ios::beg);

            if (size > 0)
            {
                fileContent.resize(static_cast<std::size_t>(size));
                inputStream.read(fileContent.data(), static_cast<std::streamsize>(size));
            }
        }

        LOG_APP_INFO("[paths debug] debug reason=probFileReadCompleted fullFilePathAbsolute='{}' bytesRead={}", fullFilePath,
                     fileContent.size());

        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
            pendingEntry->m_IsCompleted = true;
            pendingEntry->m_IsFailed = false;
            pendingEntry->m_ResponseText = std::move(fileContent);
            pendingEntry->m_SourceOutputFilePath = fullFilePath;
            pendingEntry->conditionVariable.notify_all();
        }

        QueueCompletionIfNeeded(pendingEntry);
        return true;
    }

    bool AiRequestPool::TryConsumeResult(AiRequestHandle const& requestHandle, bool& outWasFailed,
                                         std::string& outResponseText, std::string& outErrorMessage)
    {
        outWasFailed = false;
        outResponseText.clear();
        outErrorMessage.clear();

        if (!requestHandle.IsValid())
        {
            outWasFailed = true;
            outErrorMessage = "invalid ai_call request handle";
            return true;
        }

        std::shared_ptr<PendingEntry> pendingEntry;

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);

            auto const iterator = m_PendingRequests.find(MakeKey(requestHandle));
            if (iterator == m_PendingRequests.end())
            {
                return false;
            }

            pendingEntry = iterator->second;
        }

        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);

            if (!pendingEntry->m_IsCompleted)
            {
                return false;
            }

            if (pendingEntry->m_IsFailed)
            {
                outWasFailed = true;
                outErrorMessage = pendingEntry->m_ErrorMessage.empty() ? "ai_call failed" : pendingEntry->m_ErrorMessage;
            }
            else
            {
                outWasFailed = false;
                outResponseText = pendingEntry->m_ResponseText;
            }
        }

        Forget(requestHandle);
        return true;
    }

    bool AiRequestPool::TryPopCompletion(AiRequestCompletion& outCompletion)
    {
        std::scoped_lock<std::mutex> const lock(m_CompletedMutex);

        if (m_Completed.empty())
        {
            return false;
        }

        outCompletion = std::move(m_Completed.front());
        m_Completed.pop();
        return true;
    }

    void AiRequestPool::Update()
    {
        std::vector<std::shared_ptr<PendingEntry>> entries;

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);
            entries.reserve(m_PendingRequests.size());

            for (auto const& pair : m_PendingRequests)
            {
                entries.push_back(pair.second);
            }
        }

        std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();

        for (std::shared_ptr<PendingEntry> const& entry : entries)
        {
            bool becameCompleted = false;

            {
                std::scoped_lock<std::mutex> const lock(entry->mutex);

                if (entry->m_IsCompleted)
                {
                    continue;
                }

                if (!entry->m_HasDeadline)
                {
                    continue;
                }

                if (now < entry->m_Deadline)
                {
                    continue;
                }

                entry->m_IsCompleted = true;
                entry->m_IsFailed = true;

                if (!entry->m_CurlDispatched)
                {
                    entry->m_ErrorMessage = "ai_call timed out: curl was never dispatched by SessionManager "
                                            "(files placed in queue but SessionManager did not pick them up)";
                }
                else
                {
                    entry->m_ErrorMessage = "ai_call timed out waiting for output artifact "
                                            "(curl was dispatched but no response received)";
                }

                std::string const& taskId = entry->m_Context.m_TaskId;
                if (!taskId.empty())
                {
                    LOG_APP_WARN("[AiRequestPool] timeout for task '{}' (workflow '{}', run '{}'): {}", taskId,
                                 entry->m_Context.m_WorkflowId, entry->m_Context.m_RunId, entry->m_ErrorMessage);
                }

                entry->conditionVariable.notify_all();

                becameCompleted = true;
            }

            if (becameCompleted)
            {
                QueueCompletionIfNeeded(entry);
            }
        }
    }

    bool AiRequestPool::WaitForCompletion(AiRequestHandle const& requestHandle, uint64_t const timeoutMs,
                                          std::string& outResponseText, std::string& outErrorMessage)
    {
        outResponseText.clear();
        outErrorMessage.clear();

        if (!requestHandle.IsValid())
        {
            outErrorMessage = "invalid ai_call request handle";
            return false;
        }

        std::shared_ptr<PendingEntry> pendingEntry;

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);

            auto const iterator = m_PendingRequests.find(MakeKey(requestHandle));
            if (iterator == m_PendingRequests.end())
            {
                outErrorMessage = "ai_call request handle is not registered";
                return false;
            }

            pendingEntry = iterator->second;
        }

        std::unique_lock<std::mutex> lock(pendingEntry->mutex);

        bool const wasNotified = pendingEntry->conditionVariable.wait_for(
            lock, std::chrono::milliseconds(timeoutMs), [&pendingEntry]() { return pendingEntry->m_IsCompleted; });

        if (!wasNotified)
        {
            outErrorMessage = "ai_call timed out waiting for output artifact";
            return false;
        }

        if (pendingEntry->m_IsFailed)
        {
            outErrorMessage = pendingEntry->m_ErrorMessage.empty() ? "ai_call failed" : pendingEntry->m_ErrorMessage;
            return false;
        }

        outResponseText = pendingEntry->m_ResponseText;
        return true;
    }

    void AiRequestPool::Forget(AiRequestHandle const& requestHandle)
    {
        if (!requestHandle.IsValid())
        {
            return;
        }

        std::scoped_lock<std::mutex> const lock(m_MapMutex);
        m_PendingRequests.erase(MakeKey(requestHandle));
    }
} // namespace AIAssistant
