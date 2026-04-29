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
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "core.h"
#include "curlWrapper/curlMultiDispatcher.h"
#include "curlWrapper/curlWrapper.h"
#include "curlWrapper/rateLimitStrategy.h"
#include "jarvisAgent.h"
#include "json/configParser.h"
#include "json/jsonHelper.h"
#include "json/replyParser.h"
#include "json/requestBuilder.h"
#include "json/schemaValidator.h"
#include "keys/keyManager.h"
#include "session/fileWriter.h"
#include "workflow/aiCallEvents.h"
#include "workflow/aiTranscript.h"
#include "workflow/workflowTypes.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {
        uint64_t const kFileActivityWatchdogMs = 5000; // 5 seconds — max gap between file writes or curl dispatch

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

    void AiRequestPool::Shutdown()
    {
        m_ShuttingDown = true;

        std::vector<std::shared_ptr<PendingEntry>> entries;
        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);
            entries.reserve(m_PendingRequests.size());
            for (auto& [key, entry] : m_PendingRequests)
            {
                entries.push_back(entry);
            }
        }

        for (auto& entry : entries)
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (!entry->m_IsCompleted)
            {
                entry->m_IsCompleted = true;
                entry->m_IsFailed = true;
                entry->m_ErrorMessage = "shutdown";
            }
            entry->conditionVariable.notify_all();
        }

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);
            m_PendingRequests.clear();
        }
        {
            std::scoped_lock<std::mutex> const lock(m_OutputPathMutex);
            m_PendingByOutputPath.clear();
        }
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
        if (!requestHandle.IsValid())
        {
            return {};
        }

        std::shared_ptr<PendingEntry> pendingEntry = std::make_shared<PendingEntry>();

        auto const now = std::chrono::steady_clock::now();

        pendingEntry->m_Handle = requestHandle;
        pendingEntry->m_FileActivityWatchdogActive = true;
        pendingEntry->m_FileActivityDeadline = now + std::chrono::milliseconds(kFileActivityWatchdogMs);

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);
            m_PendingRequests[MakeKey(requestHandle)] = pendingEntry;
        }

        return requestHandle;
    }

    AiRequestHandle AiRequestPool::RegisterPendingWorkflowTask(
        AiRequestHandle const& requestHandle, std::string const& workflowId, std::string const& runId,
        std::string const& taskId, std::vector<std::string> const& outputFilePaths,
        std::vector<std::string> const& outputSlotNames, std::string const& expectedOutputPath)
    {
        if (!requestHandle.IsValid())
        {
            return {};
        }

        AiRequestHandle const registered = RegisterPending(requestHandle);
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

    void AiRequestPool::OnSubmitHandoff(std::string const& outputAbsolutePath)
    {
        if (outputAbsolutePath.empty())
        {
            return;
        }
        std::shared_ptr<PendingEntry> pendingEntry;
        {
            std::scoped_lock<std::mutex> const lock(m_OutputPathMutex);
            auto const iterator = m_PendingByOutputPath.find(outputAbsolutePath);
            if (iterator == m_PendingByOutputPath.end())
            {
                return;
            }
            pendingEntry = iterator->second;
        }
        std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
        pendingEntry->m_CurlDispatched = true;
        pendingEntry->m_FileActivityWatchdogActive = false;
    }

    void AiRequestPool::KickFileActivityWatchdog(AiRequestHandle const& requestHandle)
    {
        if (!requestHandle.IsValid())
        {
            return;
        }

        std::shared_ptr<PendingEntry> pendingEntry;

        {
            std::scoped_lock<std::mutex> const lock(m_MapMutex);

            auto const iterator = m_PendingRequests.find(MakeKey(requestHandle));
            if (iterator == m_PendingRequests.end())
            {
                return;
            }

            pendingEntry = iterator->second;
        }

        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);

            if (!pendingEntry->m_FileActivityWatchdogActive || pendingEntry->m_CurlDispatched)
            {
                return;
            }

            pendingEntry->m_FileActivityDeadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kFileActivityWatchdogMs);
        }
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

        // Skip if already completed (e.g. duplicate event or deletion after creation).
        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
            if (pendingEntry->m_IsCompleted)
            {
                return true;
            }
        }

        // Read file content.
        std::string fileContent;
        {
            std::ifstream inputStream(fullFilePath, std::ios::binary);
            if (!inputStream.is_open())
            {
                {
                    std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
                    pendingEntry->m_IsCompleted = true;
                    pendingEntry->m_IsFailed = true;
                    pendingEntry->m_ErrorMessage = "ai_call output file could not be opened: " + fullFilePath;
                    pendingEntry->conditionVariable.notify_all();
                }
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

    bool AiRequestPool::OnRequestFailed(std::string const& expectedOutputPath, std::string const& errorMessage)
    {
        if (expectedOutputPath.empty())
        {
            return false;
        }
        std::string const canonicalPath = fs::absolute(fs::path(expectedOutputPath)).lexically_normal().generic_string();

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

        {
            std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
            if (pendingEntry->m_IsCompleted)
            {
                return true;
            }
            pendingEntry->m_IsCompleted = true;
            pendingEntry->m_IsFailed = true;
            pendingEntry->m_ErrorMessage = errorMessage;
            pendingEntry->conditionVariable.notify_all();
        }

        LOG_APP_ERROR("[AiRequestPool] OnRequestFailed run='{}' workflow='{}' task='{}' message='{}' path='{}'",
                      pendingEntry->m_Context.m_RunId, pendingEntry->m_Context.m_WorkflowId,
                      pendingEntry->m_Context.m_TaskId, errorMessage, canonicalPath);

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

                // File-activity watchdog: fires quickly (5 s) if the executor
                // placed queue files but Submit was never called (e.g. envelope
                // rejected before dispatch).  The per-attempt timeout is owned
                // by curl (CURLOPT_TIMEOUT_MS), so this is the only watchdog
                // AiRequestPool still runs in Update().
                if (entry->m_FileActivityWatchdogActive && !entry->m_CurlDispatched &&
                    now >= entry->m_FileActivityDeadline)
                {
                    entry->m_IsCompleted = true;
                    entry->m_IsFailed = true;
                    entry->m_ErrorMessage = "ai_call file-activity watchdog expired: no curl dispatch within " +
                                            std::to_string(kFileActivityWatchdogMs / 1000) +
                                            "s of last queue-file write";

                    std::string const& taskId = entry->m_Context.m_TaskId;
                    if (!taskId.empty())
                    {
                        LOG_APP_ERROR("[AiRequestPool] file-activity watchdog run='{}' workflow='{}' task='{}': {}",
                                      entry->m_Context.m_RunId, entry->m_Context.m_WorkflowId, taskId,
                                      entry->m_ErrorMessage);
                    }

                    entry->conditionVariable.notify_all();
                    becameCompleted = true;
                }
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

    void AiRequestPool::CancelRequestsForRun(std::string const& runId)
    {
        if (runId.empty())
        {
            return;
        }

        // Collect cancel keys (= expectedOutputPaths) for the matching entries.
        // Snapshot under the lock so the dispatcher call doesn't happen while
        // m_OutputPathMutex is held (the dispatcher's I/O thread takes its own
        // mutexes and we don't want to introduce a lock-ordering trap).
        std::vector<std::string> cancelKeys;
        {
            std::scoped_lock<std::mutex> const lock(m_OutputPathMutex);
            cancelKeys.reserve(m_PendingByOutputPath.size());
            for (auto const& [path, entry] : m_PendingByOutputPath)
            {
                if (entry == nullptr)
                {
                    continue;
                }
                std::scoped_lock<std::mutex> entryLock(entry->mutex);
                if (entry->m_Context.m_RunId == runId)
                {
                    cancelKeys.push_back(path);
                }
            }
        }

        if (cancelKeys.empty())
        {
            return;
        }

        JarvisAgent* jarvisAgent = dynamic_cast<JarvisAgent*>(App::g_App);
        CurlMultiDispatcher* dispatcher = (jarvisAgent != nullptr) ? jarvisAgent->GetCurlMultiDispatcher() : nullptr;
        if (dispatcher == nullptr)
        {
            LOG_APP_ERROR("AiRequestPool::CancelRequestsForRun: dispatcher unavailable run='{}' pending={}", runId,
                          cancelKeys.size());
            return;
        }

        LOG_APP_INFO("AiRequestPool::CancelRequestsForRun: aborting {} in-flight ai_calls for run='{}'", cancelKeys.size(),
                     runId);
        for (std::string const& cancelKey : cancelKeys)
        {
            dispatcher->CancelByCancelKey(cancelKey);
        }
    }

    namespace
    {
        ConfigParser::EngineConfig::ApiInterface const* ResolveInterface(std::string const& interfaceName)
        {
            if (Core::g_Core == nullptr)
            {
                return nullptr;
            }
            auto const& config = Core::g_Core->GetConfig();
            if (!interfaceName.empty())
            {
                for (auto const& api : config.m_ApiInterfaces)
                {
                    if (api.m_Name == interfaceName)
                    {
                        return &api;
                    }
                }
            }
            if (config.m_ApiIndex < config.m_ApiInterfaces.size())
            {
                return &config.m_ApiInterfaces[config.m_ApiIndex];
            }
            return nullptr;
        }

        std::string ResolveApiKey(ConfigParser::EngineConfig::ApiInterface const& api)
        {
            if (Core::g_Core == nullptr)
            {
                return {};
            }
            auto const* provider = api.m_KeyName.empty() ? Core::g_Core->GetKeyManager().GetDefaultProvider()
                                                          : Core::g_Core->GetKeyManager().GetProvider(api.m_KeyName);
            return (provider != nullptr) ? provider->m_ApiKey : std::string{};
        }

        // SigV4 (AWS) needs region + secret_access_key beyond the api_key. They live in
        // ProviderConfig::m_Params and need to be forwarded to QueryData::m_Params so the
        // signer can reach them. Returns empty map when the provider has none.
        std::unordered_map<std::string, std::string> ResolveProviderParams(
            ConfigParser::EngineConfig::ApiInterface const& api)
        {
            if (Core::g_Core == nullptr) { return {}; }
            auto const* provider = api.m_KeyName.empty() ? Core::g_Core->GetKeyManager().GetDefaultProvider()
                                                          : Core::g_Core->GetKeyManager().GetProvider(api.m_KeyName);
            return (provider != nullptr) ? provider->m_Params : std::unordered_map<std::string, std::string>{};
        }

        std::string ConcatMessagesForCheck(std::vector<Message> const& messages)
        {
            std::string combined;
            for (auto const& message : messages)
            {
                combined += message.m_Content;
            }
            return combined;
        }

        // Extracts a JSON value from raw reply text, tolerating common model artifacts:
        // leading/trailing whitespace, markdown ```json fences, and text before/after the JSON.
        std::string ExtractJsonFromText(std::string const& text)
        {
            std::string trimmed = text;
            auto const firstNonSpace = trimmed.find_first_not_of(" \t\r\n");
            if (firstNonSpace != std::string::npos)
            {
                trimmed.erase(0, firstNonSpace);
            }
            auto const lastNonSpace = trimmed.find_last_not_of(" \t\r\n");
            if (lastNonSpace != std::string::npos)
            {
                trimmed.erase(lastNonSpace + 1);
            }
            if (trimmed.rfind("```", 0) == 0)
            {
                size_t const firstNewline = trimmed.find('\n');
                if (firstNewline != std::string::npos)
                {
                    trimmed.erase(0, firstNewline + 1);
                }
                size_t const closingFence = trimmed.rfind("```");
                if (closingFence != std::string::npos)
                {
                    trimmed.erase(closingFence);
                }
                while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '))
                {
                    trimmed.pop_back();
                }
            }
            return trimmed;
        }

        // Best-effort strip of a single triple-backtick fence that wraps the entire reply.
        // Models (notably Claude Haiku) occasionally ignore STNG rules and emit e.g.:
        //     ```cpp
        //     <real content>
        //     ```
        // Downstream consumers (compilers, make, python-exec) choke on those three
        // leading backticks.  We only strip when the whole content is a single fenced
        // block with no intermediate ``` sequences — anything more complex we leave
        // alone so embedded code blocks in legitimately markdown replies survive.
        //
        // Exception: diagram/markdown formats (mermaid, dot, plantuml, graphviz, latex,
        // markdown/md) are MEANT to live inside a fenced block when embedded in a .md
        // output — downstream renderers (mmdc, pandoc) match on ```<tag>. Stripping
        // those fences silently destroys the diagram. Keep the fence whenever the
        // language tag matches such a format.
        std::string StripWholeReplyFence(std::string const& text)
        {
            auto firstNonSpace = text.find_first_not_of(" \t\r\n");
            if (firstNonSpace == std::string::npos) return text;
            auto lastNonSpace = text.find_last_not_of(" \t\r\n");
            if (lastNonSpace == std::string::npos || lastNonSpace < firstNonSpace) return text;

            if (text.compare(firstNonSpace, 3, "```") != 0) return text;
            if (lastNonSpace < 2 || text.compare(lastNonSpace - 2, 3, "```") != 0) return text;

            // Reject if there's an intermediate ``` inside the body — that means
            // the reply contains nested code blocks, not just a single wrapper.
            size_t const innerStart = firstNonSpace + 3;
            size_t const innerEnd = lastNonSpace - 2;
            if (innerEnd <= innerStart) return text;
            size_t const scanFrom = innerStart;
            size_t const scanTo = innerEnd;
            size_t searchPos = scanFrom;
            while (searchPos < scanTo)
            {
                size_t const found = text.find("```", searchPos);
                if (found == std::string::npos || found >= scanTo) break;
                return text; // nested fence — leave content alone
            }

            // Skip the opening fence line (optional language tag + newline).
            size_t bodyStart = firstNonSpace + 3;
            size_t const firstNewline = text.find('\n', bodyStart);
            if (firstNewline == std::string::npos || firstNewline >= innerEnd) return text;

            // Language tag lives between the opening ``` and the first newline.
            // Carriage-returns and surrounding whitespace may appear on that line.
            std::string languageTag;
            {
                size_t const tagBegin = firstNonSpace + 3;
                size_t tagEnd = firstNewline;
                while (tagEnd > tagBegin && (text[tagEnd - 1] == '\r' || text[tagEnd - 1] == ' ' ||
                                             text[tagEnd - 1] == '\t'))
                {
                    --tagEnd;
                }
                size_t tagStart = tagBegin;
                while (tagStart < tagEnd && (text[tagStart] == ' ' || text[tagStart] == '\t'))
                {
                    ++tagStart;
                }
                if (tagStart < tagEnd)
                {
                    languageTag.reserve(tagEnd - tagStart);
                    for (size_t i = tagStart; i < tagEnd; ++i)
                    {
                        unsigned char const c = static_cast<unsigned char>(text[i]);
                        languageTag.push_back(static_cast<char>(std::tolower(c)));
                    }
                }
            }

            // Formats that are authored to live inside a fenced block in the final
            // markdown artifact. Stripping the fence here would silently break the
            // downstream renderer (mmdc / pandoc / Mermaid-aware viewers).
            static constexpr std::string_view kKeepFenceTags[] = {
                "mermaid", "dot", "plantuml", "graphviz", "latex", "tex", "markdown", "md"
            };
            for (auto const& tag : kKeepFenceTags)
            {
                if (languageTag == tag) return text;
            }

            bodyStart = firstNewline + 1;

            // Trim trailing whitespace before the closing fence so the result doesn't
            // end with a bare newline from the line that held the closing backticks.
            size_t bodyEnd = innerEnd;
            while (bodyEnd > bodyStart && (text[bodyEnd - 1] == '\n' || text[bodyEnd - 1] == '\r' ||
                                            text[bodyEnd - 1] == ' ' || text[bodyEnd - 1] == '\t'))
            {
                --bodyEnd;
            }

            return text.substr(bodyStart, bodyEnd - bodyStart);
        }
    } // anonymous namespace

    bool AiRequestPool::Submit(AiInvocation const& envelopeInput, ReplyCallback onReply)
    {
        AiInvocation envelope = envelopeInput;

        // Apply determinism defaults from config when the envelope didn't already set them.
        if (Core::g_Core != nullptr)
        {
            auto const& config = Core::g_Core->GetConfig();
            if (envelope.m_Settings.m_Temperature == 0.0)
            {
                envelope.m_Settings.m_Temperature = config.m_DeterminismTemperature;
            }
            if (!envelope.m_Settings.m_Seed.has_value() && config.m_DeterminismSeedSet)
            {
                envelope.m_Settings.m_Seed = config.m_DeterminismSeed;
            }
        }

        // Resolve the workflow binding (registered by RegisterPendingWorkflowTask before Submit)
        // upfront so every fail-path log line in this function and the dispatch callback can
        // carry runId/workflowId/taskId. The dashboard run analyzer's per-run filter requires
        // these markers — see feedback_log_failures memory. Empty strings for non-workflow
        // callers (assistant, jcwfService) — log lines render run='' there.
        // Suffix matches what RegisterPendingWorkflowTask was given: structured
        // tasks register under .output.json (writes via FileWriter when reply
        // is Kind::Structured); plain text tasks register under .output.txt.
        // Without this match the binding lookup misses and:
        //   (a) runId/workflowId/taskId stay empty in fail-path logs (the
        //       dashboard run analyzer can't surface them);
        //   (b) the onDispatched callback can't find the entry to disarm the
        //       file-activity watchdog → false-positive 5 s timeout if the
        //       provider takes more than 5 s to respond (e.g., Gemini Native).
        std::string const outputSuffix = envelope.m_OutputSchemaJson.has_value() ? ".output.json" : ".output.txt";

        std::string runIdForLog;
        std::string workflowIdForLog;
        std::string taskIdForLog;
        if (!envelope.m_QueueFolder.empty() && !envelope.m_ProbName.empty())
        {
            fs::path const outputPath =
                envelope.m_QueueFolder / (fs::path(envelope.m_ProbName).stem().string() + outputSuffix);
            std::string const lookupKey = fs::absolute(outputPath).lexically_normal().generic_string();
            std::scoped_lock<std::mutex> const lock(m_OutputPathMutex);
            auto const it = m_PendingByOutputPath.find(lookupKey);
            if (it != m_PendingByOutputPath.end() && it->second != nullptr)
            {
                runIdForLog = it->second->m_Context.m_RunId;
                workflowIdForLog = it->second->m_Context.m_WorkflowId;
                taskIdForLog = it->second->m_Context.m_TaskId;
            }
        }

        // Count the first chunk of a chunked dispatch as one "chunked dispatch"
        // event — not every chunk envelope, so the signal reflects distinct PROBs
        // that required fan-out rather than the raw chunk count.
        if (envelope.m_ChunkIndex.has_value() && envelope.m_ChunkCount.has_value() &&
            *envelope.m_ChunkIndex == 0)
        {
            ++m_ChunkedDispatches;
        }

        auto const* api = ResolveInterface(envelope.m_InterfaceName);
        if (api == nullptr)
        {
            LOG_APP_ERROR("AiRequestPool::Submit: no resolvable AI interface run='{}' workflow='{}' task='{}' "
                          "requested='{}'",
                          runIdForLog, workflowIdForLog, taskIdForLog, envelope.m_InterfaceName);
            return false;
        }

        std::string const combinedPrompt = ConcatMessagesForCheck(envelope.m_Messages);
        bool hasNonWhitespace = false;
        for (char const character : combinedPrompt)
        {
            if (!std::isspace(static_cast<unsigned char>(character)))
            {
                hasNonWhitespace = true;
                break;
            }
        }
        if (!hasNonWhitespace)
        {
            LOG_APP_ERROR("AiRequestPool::Submit: empty prompt body run='{}' workflow='{}' task='{}' prob='{}' — rejecting",
                          runIdForLog, workflowIdForLog, taskIdForLog, envelope.m_ProbName);
            return false;
        }

        std::string const model =
            envelope.m_ModelOverride.has_value() ? envelope.m_ModelOverride.value() : api->m_Model;

        // TestInterface: short-circuit curl and synthesize a canned reply (§8 Phase 7).
        // The interface's m_Url field is repurposed as an optional fixture path.
        // An empty or missing fixture produces a generic "test reply" placeholder.
        if (api->m_InterfaceType == ConfigParser::EngineConfig::InterfaceType::Test)
        {
            std::string replyText = "test reply (no fixture configured)";
            fs::path fixturePath(api->m_Url);
            if (!fixturePath.empty() && fs::exists(fixturePath))
            {
                std::ifstream input(fixturePath, std::ios::binary);
                if (input)
                {
                    std::ostringstream buffer;
                    buffer << input.rdbuf();
                    replyText = buffer.str();
                }
            }
            // §14 TUI invalid-UTF-8 stress fixture: malformed bytes from a
            // Test-interface fixture must be sanitized before they enter the
            // log/dashboard pipeline.  Output file (binary write below)
            // legitimately preserves the original bytes for inspection.
            std::string const replyTextSanitized = SanitizeUtf8(replyText);

            fs::path const queueFolderCopy = envelope.m_QueueFolder;
            std::string const probNameCopy = envelope.m_ProbName;
            std::string const modelLocal = model;
            fs::path transcriptPathLocal;
            if (!queueFolderCopy.empty() && !probNameCopy.empty())
            {
                transcriptPathLocal =
                    queueFolderCopy / (fs::path(probNameCopy).stem().string() + ".transcript.json");
                AiTranscript::AppendRequest(transcriptPathLocal, envelope, modelLocal);
            }

            if (Core::g_Core != nullptr)
            {
                auto startedEvent = std::make_shared<AiCallStartedEvent>(probNameCopy, api->m_Name);
                Core::g_Core->PushEvent(startedEvent);
            }

            fs::path writtenOutputPath;
            if (!queueFolderCopy.empty() && !probNameCopy.empty())
            {
                writtenOutputPath = queueFolderCopy / (fs::path(probNameCopy).stem().string() + ".output.txt");
                // Output file gets the SANITIZED bytes — downstream tasks
                // (combineDocumentation et al.) read this file as UTF-8 text;
                // the inspection-on-disk benefit of preserving raw bytes is
                // outweighed by every downstream consumer assuming valid UTF-8.
                FileWriter::Get().WriteWithHeader(writtenOutputPath, replyTextSanitized, modelLocal);
            }

            AiReply testReply;
            testReply.m_Kind = AiReply::Kind::Text;
            testReply.m_Text = replyTextSanitized;
            testReply.m_FinishReason = "test";

            if (!transcriptPathLocal.empty())
            {
                AiTranscript::AppendResponse(transcriptPathLocal, testReply);
            }

            // Signal completion so workflow-bound tasks release from waiting_external.
            // The real-provider path does this from its curl callback; the Test short-circuit
            // must do it explicitly since there is no dispatcher callback.
            if (!writtenOutputPath.empty())
            {
                std::string const normalizedPath =
                    fs::absolute(writtenOutputPath).lexically_normal().generic_string();
                OnOutputFileCreated(normalizedPath);
            }

            if (Core::g_Core != nullptr)
            {
                auto completedEvent =
                    std::make_shared<AiCallCompletedEvent>(probNameCopy, testReply.m_Usage, testReply.m_FinishReason);
                Core::g_Core->PushEvent(completedEvent);
            }

            if (onReply)
            {
                onReply(testReply);
            }
            return true;
        }

        auto requestBuilder = IRequestBuilder::Create(api->m_InterfaceType);
        if (!requestBuilder)
        {
            LOG_APP_ERROR("AiRequestPool::Submit: no request builder run='{}' workflow='{}' task='{}' interface='{}'",
                          runIdForLog, workflowIdForLog, taskIdForLog, api->m_Name);
            return false;
        }

        std::string const requestBody = requestBuilder->BuildBody(envelope, model);
        std::string const queryUrl = requestBuilder->ResolveUrl(api->m_Url, model);
        CurlWrapper::AuthStyle const authStyle = requestBuilder->GetAuthStyle();

        std::string const apiKey = ResolveApiKey(*api);
        if (apiKey.empty())
        {
            LOG_APP_ERROR("AiRequestPool::Submit: no API key resolvable run='{}' workflow='{}' task='{}' "
                          "interface='{}' key_name='{}'",
                          runIdForLog, workflowIdForLog, taskIdForLog, api->m_Name, api->m_KeyName);
            return false;
        }

        // QuotaKey = "<host>|<modelFamily>" — controllers in the dispatcher are
        // keyed by this so per-(host, modelFamily) AIMD signals stay independent
        // (Anthropic Sonnet vs Opus on same host).  Strategy derives the family;
        // we extract host from the resolved URL.
        std::string quotaKey;
        {
            std::string host;
            size_t const schemeEnd = queryUrl.find("://");
            if (schemeEnd != std::string::npos)
            {
                size_t const hostStart = schemeEnd + 3;
                size_t hostEnd = queryUrl.find('/', hostStart);
                if (hostEnd == std::string::npos)
                    hostEnd = queryUrl.size();
                size_t const colon = queryUrl.find(':', hostStart);
                if (colon != std::string::npos && colon < hostEnd)
                    hostEnd = colon;
                host = queryUrl.substr(hostStart, hostEnd - hostStart);
            }
            std::string const family = IRateLimitStrategy::Get(api->m_InterfaceType).DeriveQuotaKey(model);
            quotaKey = host + "|" + family;
        }

        int64_t const estimatedInputTokens =
            IRateLimitStrategy::Get(api->m_InterfaceType).EstimateInputTokens(combinedPrompt);

        // Size-aware in-flight budget (§6).  Computed here, enforced by curl's
        // CURLOPT_TIMEOUT_MS — only counts time on the wire, resets per attempt
        // because each retry creates a fresh easy handle.  Replaces the old
        // AiRequestPool::m_Deadline machinery (deferred-arm + retry-extend).
        // JCWF-level explicit timeout (envelope.m_Timeout) wins when set.
        long timeoutMs = 0;
        if (envelope.m_Timeout.has_value())
        {
            timeoutMs = static_cast<long>(envelope.m_Timeout->count());
            LOG_APP_INFO("AiRequestPool::Submit: budget run='{}' workflow='{}' task='{}' explicit timeoutMs={}",
                         runIdForLog, workflowIdForLog, taskIdForLog, timeoutMs);
        }
        else
        {
            auto const& budget = api->m_RateLimit.m_RequestBudget;
            int32_t const maxOutTokens =
                envelope.m_Settings.m_MaxTokens.value_or(api->m_DefaultOutputTokens);
            double seconds = (static_cast<double>(estimatedInputTokens) / 1000.0) * budget.m_Per1kInputTokenSeconds
                           + (static_cast<double>(maxOutTokens)        / 1000.0) * budget.m_Per1kOutputTokenSeconds
                           + budget.m_FixedOverheadSeconds;
            seconds *= budget.m_SafetyMarginFactor;
            seconds = std::clamp(seconds, budget.m_MinSeconds, budget.m_MaxSeconds);
            timeoutMs = static_cast<long>(seconds * 1000.0);
            LOG_APP_INFO("AiRequestPool::Submit: budget run='{}' workflow='{}' task='{}' inTok={} outTok={} "
                         "timeoutMs={}",
                         runIdForLog, workflowIdForLog, taskIdForLog, estimatedInputTokens, maxOutTokens, timeoutMs);
        }

        CurlWrapper::QueryData queryData{.m_Url = queryUrl, .m_Data = requestBody, .m_ApiKey = apiKey,
                                          .m_AuthStyle = authStyle, .m_TimeoutMs = timeoutMs,
                                          .m_Params = ResolveProviderParams(*api),
                                          .m_InterfaceType = static_cast<int>(api->m_InterfaceType),
                                          .m_QuotaKey = quotaKey,
                                          .m_EstimatedInputTokens = estimatedInputTokens,
                                          .m_CancelKey = {}, // populated below once expectedOutputPath is computed
                                          .m_MaxConcurrency = api->m_RateLimit.m_MaxConcurrency,
                                          .m_MaxRetries429 = api->m_RateLimit.m_MaxRetries429,
                                          .m_MaxRetriesTransient = api->m_RateLimit.m_MaxRetriesTransient,
                                          .m_BaseRetryMs = api->m_RateLimit.m_BaseRetryMs};

        JarvisAgent* jarvisAgent = dynamic_cast<JarvisAgent*>(App::g_App);
        CurlMultiDispatcher* dispatcher = (jarvisAgent != nullptr) ? jarvisAgent->GetCurlMultiDispatcher() : nullptr;
        if (dispatcher == nullptr)
        {
            LOG_APP_ERROR("AiRequestPool::Submit: CurlMultiDispatcher unavailable run='{}' workflow='{}' task='{}' prob='{}'",
                          runIdForLog, workflowIdForLog, taskIdForLog, envelope.m_ProbName);
            return false;
        }

        ConfigParser::EngineConfig::InterfaceType const interfaceType = api->m_InterfaceType;
        fs::path const queueFolder = envelope.m_QueueFolder;
        std::string const probName = envelope.m_ProbName;
        std::string const modelCapture = model;
        ReplyCallback callbackCopy = std::move(onReply);

        std::string expectedOutputPath;
        if (!queueFolder.empty() && !probName.empty())
        {
            // Same suffix logic as the binding-lookup above so onDispatched and
            // OnRequestFailed find the registered entry.
            fs::path outputPath = queueFolder / (fs::path(probName).stem().string() + outputSuffix);
            expectedOutputPath = fs::absolute(outputPath).lexically_normal().generic_string();
        }
        // Cancel key = expectedOutputPath (unique per workflow task per run).
        // Used by AiRequestPool::CancelRequestsForRun → dispatcher CancelByCancelKey
        // to abort in-flight HTTP requests when the calling workflow terminates.
        queryData.m_CancelKey = expectedOutputPath;

        ++m_DirectDispatchInflight;

        fs::path transcriptPath;
        if (!queueFolder.empty() && !probName.empty())
        {
            transcriptPath = queueFolder / (fs::path(probName).stem().string() + ".transcript.json");
            AiTranscript::AppendRequest(transcriptPath, envelope, modelCapture);
        }

        if (Core::g_Core != nullptr)
        {
            auto startedEvent = std::make_shared<AiCallStartedEvent>(probName, api->m_Name);
            Core::g_Core->PushEvent(startedEvent);
        }

        AiInvocation envelopeForRetry = envelope;

        auto curlCallback = [this, interfaceType, queueFolder, probName, modelCapture, expectedOutputPath,
                             transcriptPath, callbackCopy, envelopeForRetry,
                             runIdForLog, workflowIdForLog,
                             taskIdForLog](QueryResult curlResult, std::string responseBody) mutable
        {
            AiReply aiReply;

            try
            {
                if (!curlResult.m_Ok)
                {
                    aiReply.m_Kind = AiReply::Kind::Error;
                    aiReply.m_Error.m_Kind = AiError::Kind::Http;
                    aiReply.m_Error.m_Message = curlResult.m_ErrorMessage;
                    LOG_APP_ERROR("AiRequestPool::Submit callback: curl error ({}) run='{}' workflow='{}' task='{}' prob='{}': {}",
                                  curlResult.m_ErrorCode, runIdForLog, workflowIdForLog, taskIdForLog, probName,
                                  curlResult.m_ErrorMessage);
                }
                else
                {
                    auto replyParser = ReplyParser::Create(interfaceType, responseBody);
                    if (!replyParser)
                    {
                        aiReply.m_Kind = AiReply::Kind::Error;
                        aiReply.m_Error.m_Kind = AiError::Kind::Parse;
                        aiReply.m_Error.m_Message = "reply parser unavailable for interface type";
                    }
                    else if (replyParser->HasError())
                    {
                        aiReply = AiReply{};
                        aiReply.m_Kind = AiReply::Kind::Error;
                        aiReply.m_Error = replyParser->GetError();
                        aiReply.m_Usage = replyParser->GetUsage();
                    }
                    else if (replyParser->HasContent() == 0)
                    {
                        aiReply.m_Kind = AiReply::Kind::Error;
                        aiReply.m_Error.m_Kind = AiError::Kind::Provider;
                        aiReply.m_Error.m_Message = "empty response";
                    }
                    else
                    {
                        aiReply.m_Kind = AiReply::Kind::Text;
                        {
                            std::string const raw = replyParser->GetContent(0);
                            aiReply.m_Text = StripWholeReplyFence(raw);
                            if (aiReply.m_Text.size() != raw.size())
                            {
                                ++m_FenceStrips;
                            }
                        }
                        aiReply.m_Usage = replyParser->GetUsage();
                        aiReply.m_FinishReason = replyParser->GetFinishReason();
                        aiReply.m_SystemFingerprint = replyParser->GetSystemFingerprint();
                    }
                }

                // Schema validation + bounded retry.
                // Applies only when the task declared output_schema AND we got text content back.
                // Skip schema validation for chunked replies — each chunk is only a
                // slice of the final response, not a complete schema-conforming object.
                bool const skipSchemaForChunk =
                    envelopeForRetry.m_ChunkIndex.has_value() && envelopeForRetry.m_ChunkCount.has_value();
                if (aiReply.m_Kind == AiReply::Kind::Text && envelopeForRetry.m_OutputSchemaJson.has_value() &&
                    !skipSchemaForChunk)
                {
                    // Count the first attempt of each structured submission (not retries).
                    if (envelopeForRetry.m_SchemaAttemptNumber == 0)
                    {
                        ++m_StructuredSubmissions;
                    }
                    std::string const extractedJson = ExtractJsonFromText(aiReply.m_Text);
                    SchemaValidator validator(*envelopeForRetry.m_OutputSchemaJson);
                    if (!validator.IsLoaded())
                    {
                        aiReply.m_Kind = AiReply::Kind::Error;
                        aiReply.m_Error.m_Kind = AiError::Kind::SchemaValidation;
                        aiReply.m_Error.m_Message = "output_schema failed to load: " + validator.LoadError();
                    }
                    else
                    {
                        ValidationResult const validation = validator.Validate(extractedJson);
                        if (validation.m_Ok)
                        {
                            aiReply.m_Kind = AiReply::Kind::Structured;
                            aiReply.m_StructuredJson = extractedJson;
                        }
                        else
                        {
                            int const maxAttempts = envelopeForRetry.m_Retry.m_OutputSchemaMaxAttempts;
                            int const nextAttempt = envelopeForRetry.m_SchemaAttemptNumber + 1;
                            if (nextAttempt < maxAttempts)
                            {
                                ++m_SchemaValidationRetries;
                                std::string const errorSummary =
                                    SchemaValidator::FormatErrorsForModel(validation.m_Errors);
                                LOG_APP_INFO("AiRequestPool: schema validation failed (attempt {}/{}) for prob='{}' — "
                                              "retrying with validator feedback",
                                              nextAttempt, maxAttempts, probName);
                                AiInvocation retryEnvelope = envelopeForRetry;
                                retryEnvelope.m_SchemaAttemptNumber = nextAttempt;
                                Message correctionMessage;
                                correctionMessage.m_Role = MessageRole::User;
                                correctionMessage.m_Content =
                                    "Your previous response failed schema validation:\n" + errorSummary +
                                    "\nPlease correct the response to match the requested schema exactly. Emit only "
                                    "JSON matching the schema, no prose or markdown fences.";
                                retryEnvelope.m_Messages.push_back(std::move(correctionMessage));

                                if (!transcriptPath.empty())
                                {
                                    AiTranscript::AppendResponse(transcriptPath, aiReply);
                                }

                                if (m_DirectDispatchInflight.load() > 0)
                                {
                                    --m_DirectDispatchInflight;
                                }

                                Submit(retryEnvelope, callbackCopy);
                                return;
                            }
                            ++m_SchemaValidationFailures;
                            aiReply.m_Kind = AiReply::Kind::Error;
                            aiReply.m_Error.m_Kind = AiError::Kind::SchemaValidation;
                            aiReply.m_Error.m_Message =
                                "schema validation failed after " + std::to_string(maxAttempts) +
                                " attempts: " + SchemaValidator::FormatErrorsForModel(validation.m_Errors);
                        }
                    }
                }

                // Chunked envelopes: write a per-chunk file for replay/debug but DON'T
                // signal completion here — the executor's aggregator is responsible for
                // writing the final <prob>.output.txt once all chunks arrive.
                bool const isChunked =
                    envelopeForRetry.m_ChunkIndex.has_value() && envelopeForRetry.m_ChunkCount.has_value();

                fs::path writtenOutputPath;
                if (aiReply.m_Kind == AiReply::Kind::Text && !queueFolder.empty() && !probName.empty())
                {
                    if (isChunked)
                    {
                        std::string const suffix = ".output.chunk" +
                                                   std::to_string(*envelopeForRetry.m_ChunkIndex) + "-of-" +
                                                   std::to_string(*envelopeForRetry.m_ChunkCount) + ".txt";
                        writtenOutputPath = queueFolder / (fs::path(probName).stem().string() + suffix);
                    }
                    else
                    {
                        writtenOutputPath = queueFolder / (fs::path(probName).stem().string() + ".output.txt");
                    }
                    FileWriter::Get().WriteWithHeader(writtenOutputPath, aiReply.m_Text, modelCapture);
                }

                if (aiReply.m_Kind == AiReply::Kind::Structured && !queueFolder.empty() && !probName.empty())
                {
                    writtenOutputPath =
                        queueFolder / (fs::path(probName).stem().string() + ".output.json");
                    FileWriter::Get().WriteWithHeader(writtenOutputPath, aiReply.m_StructuredJson, modelCapture);
                }

                // Directly signal completion — don't wait for the queue FileWatcher to notice
                // the .output.* file we just wrote.  This makes completion deterministic and
                // lets the queue-folder FileWatcher be retired (its events were the only other
                // path into OnOutputFileCreated for runtime-dispatched ai_call).
                //
                // For chunked dispatches, the executor aggregates and writes the final
                // <prob>.output.txt itself, so skip that signal here.
                if (!writtenOutputPath.empty() && !isChunked)
                {
                    std::string const normalizedPath =
                        fs::absolute(writtenOutputPath).lexically_normal().generic_string();
                    OnOutputFileCreated(normalizedPath);
                }
                else if (aiReply.m_Kind == AiReply::Kind::Error && !isChunked && !expectedOutputPath.empty())
                {
                    // No .output.* file is written for error replies; without this signal the
                    // workflow runtime would stay parked in waiting_external until the ai_call
                    // deadline fires. Symmetric to the success path above.
                    OnRequestFailed(expectedOutputPath, aiReply.m_Error.m_Message);
                }

                if (!transcriptPath.empty())
                {
                    AiTranscript::AppendResponse(transcriptPath, aiReply);
                }

                if (Core::g_Core != nullptr)
                {
                    if (aiReply.m_Kind == AiReply::Kind::Error)
                    {
                        auto failedEvent = std::make_shared<AiCallFailedEvent>(probName, aiReply.m_Error);
                        Core::g_Core->PushEvent(failedEvent);
                    }
                    else
                    {
                        auto completedEvent =
                            std::make_shared<AiCallCompletedEvent>(probName, aiReply.m_Usage, aiReply.m_FinishReason);
                        Core::g_Core->PushEvent(completedEvent);
                    }
                }

                if (callbackCopy)
                {
                    callbackCopy(aiReply);
                }
            }
            catch (std::exception const& e)
            {
                LOG_APP_ERROR("AiRequestPool::Submit callback exception run='{}' workflow='{}' task='{}' prob='{}': {}",
                              runIdForLog, workflowIdForLog, taskIdForLog, probName, e.what());
                AiReply errorReply;
                errorReply.m_Kind = AiReply::Kind::Error;
                errorReply.m_Error.m_Kind = AiError::Kind::Transport;
                errorReply.m_Error.m_Message = e.what();
                if (!expectedOutputPath.empty())
                {
                    OnRequestFailed(expectedOutputPath, errorReply.m_Error.m_Message);
                }
                if (callbackCopy)
                {
                    callbackCopy(errorReply);
                }
            }

            if (m_DirectDispatchInflight.load() > 0)
            {
                --m_DirectDispatchInflight;
            }
        };

        // Disarm the file-activity watchdog at handoff time, NOT at
        // curl_multi_add_handle time.  The watchdog catches "executor wrote
        // queue files but Submit was never called" — once Submit is reached,
        // its purpose is fulfilled regardless of whether the dispatcher's
        // controller throttles the request in its inbox.  Pre-Phase-4b code
        // armed deadlines from the dispatcher's onDispatched callback; with
        // curl now owning the per-attempt timeout via CURLOPT_TIMEOUT_MS, the
        // dispatcher needs no callback at all and we disarm here.
        OnSubmitHandoff(expectedOutputPath);

        dispatcher->Submit(queryData, std::move(curlCallback));

        return true;
    }
} // namespace AIAssistant
