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
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "auxiliary/file.h"
#include "core.h"
#include "curlWrapper/curlManager.h"
#include "curlWrapper/curlMultiDispatcher.h"
#include "curlWrapper/curlWrapper.h"
#include "curlWrapper/rateLimitStrategy.h"
#include "file/pathConfinement.h"
#include "jarvisAgent.h"
#include "json/configParser.h"
#include "json/jsonHelper.h"
#include "json/replyParser.h"
#include "json/requestBuilder.h"
#include "json/schemaValidator.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "network/urlPolicy.h"
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

        // Cap on a single ai_call output file read into memory.  10 MB is an
        // order of magnitude above realistic AI text replies and well below
        // any reasonable RAM-budget concern.  Prevents a malicious workflow
        // from triggering a huge allocation by pointing expectedOutputPath at
        // a giant file.
        std::size_t const kMaxOutputFileBytes = 10ull * 1024ull * 1024ull;

        bool WriteTextFile(std::string const& filePath, std::string const& fileContent, std::string& outErrorMessage)
        {
            outErrorMessage.clear();

            // Containment gate.  All ai_call output paths originate from
            // workflow/task input (queue folders + JCWF-declared file_outputs)
            // and resolve under the project root.  Routing through the shared
            // helper rejects any `..`/symlink escape with an empty return —
            // fail-closed.  This is the canonical project-root gate; an
            // Engine-edition queue-root tightening would be additive on top.
            fs::path const confinedPath = ConfineUnderProjectRoot(filePath);
            if (confinedPath.empty())
            {
                outErrorMessage = "path does not resolve under project root: " + filePath;
                LOG_APP_ERROR("AiRequestPool::WriteTextFile: rejected path '{}' — does not resolve under project root",
                              filePath);
                return false;
            }

            fs::path const filesystemPathAbsolute = confinedPath;
            LOG_APP_INFO("[paths debug] debug reason=writeAiRequestPoolTextFile filePathRelative='{}' filePathAbsolute='{}' "
                         "wasRelative='{}'",
                         filePath, filesystemPathAbsolute.generic_string(), fs::path(filePath).is_relative());
            std::error_code errorCode;

            fs::path const parentAbsolute = filesystemPathAbsolute.parent_path();
            if (!parentAbsolute.empty())
            {
                std::error_code existsBeforeErrorCode;
                bool const existedBefore = fs::exists(parentAbsolute, existsBeforeErrorCode);

                LOG_APP_INFO("[folder creation debug] debug create_directories attempt path='{}' reason='aiRequestPool "
                             "parent directory'",
                             parentAbsolute.generic_string());

                fs::create_directories(parentAbsolute, errorCode);
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

            // Atomic write via the confined path: helper opens
            // <final>.tmp.<counter>, enables ofstream exceptions, writes,
            // closes, then renames.  Lands at the canonical location (not any
            // symlinked alias the input string pointed at).  A SIGKILL or
            // disk-full mid-write leaves the previous version intact instead
            // of a truncated partial that downstream consumers parse as
            // malformed (notably AiRequestPool::OnOutputFileCreated reading
            // the .output.{txt,json} file as the completion signal).
            if (!EngineCore::AtomicWriteFile(filesystemPathAbsolute, fileContent, outErrorMessage))
            {
                LOG_APP_ERROR("AiRequestPool::WriteTextFile: {} (path='{}')", outErrorMessage, filePath);
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
        // Containment gate: the expectedOutputPath comes from the workflow
        // executor's task plumbing — defense in depth ensures it canonicalises
        // under the project root before becoming a map key (so a hostile JCWF
        // can't register a binding under `/etc/passwd`).
        if (!expectedOutputPath.empty())
        {
            fs::path const confinedExpected = ConfineUnderProjectRoot(expectedOutputPath);
            if (confinedExpected.empty())
            {
                LOG_APP_ERROR("AiRequestPool::RegisterPendingWorkflowTask: rejected expectedOutputPath '{}' "
                              "run='{}' workflow='{}' task='{}' — does not resolve under project root",
                              expectedOutputPath, runId, workflowId, taskId);
                return {};
            }
            std::string const canonicalPath = confinedExpected.generic_string();

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
        // Containment gate.  fullFilePath is the canonical absolute path
        // produced by Submit's reply callback for an ai_call task; it must
        // resolve under the project root.  Pass through ConfineUnderProjectRoot
        // (no-op for already-canonical absolute paths inside the tree, fail-
        // closed for anything else).  The path is then used both as the map
        // key and as the read source — same gate covers both.
        fs::path const confinedPath = ConfineUnderProjectRoot(fullFilePath);
        if (confinedPath.empty())
        {
            LOG_APP_ERROR("AiRequestPool::OnOutputFileCreated: rejected path '{}' — does not resolve under "
                          "project root",
                          fullFilePath);
            return false;
        }
        std::string const canonicalPath = confinedPath.generic_string();

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

        // Read file content.  Open via the confined path so symlink targets
        // resolved into the canonical form are honoured, not the raw input.
        std::string fileContent;
        {
            std::ifstream inputStream(confinedPath, std::ios::binary);
            if (!inputStream.is_open())
            {
                std::string contextRunId, contextWorkflowId, contextTaskId;
                {
                    std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
                    contextRunId = pendingEntry->m_Context.m_RunId;
                    contextWorkflowId = pendingEntry->m_Context.m_WorkflowId;
                    contextTaskId = pendingEntry->m_Context.m_TaskId;
                    pendingEntry->m_IsCompleted = true;
                    pendingEntry->m_IsFailed = true;
                    pendingEntry->m_ErrorMessage = "ai_call output file could not be opened: " + fullFilePath;
                    pendingEntry->conditionVariable.notify_all();
                }
                LOG_APP_ERROR("[AiRequestPool] OnOutputFileCreated: failed to open output file run='{}' workflow='{}' "
                              "task='{}' path='{}'",
                              contextRunId, contextWorkflowId, contextTaskId, fullFilePath);
                QueueCompletionIfNeeded(pendingEntry);
                return true;
            }

            inputStream.seekg(0, std::ios::end);
            std::streamoff const size = inputStream.tellg();
            inputStream.seekg(0, std::ios::beg);

            // Cap to kMaxOutputFileBytes — defend against an attacker-pointed
            // expectedOutputPath landing on a multi-GB file and exhausting RAM.
            if (size > 0 && static_cast<std::size_t>(size) > kMaxOutputFileBytes)
            {
                std::string contextRunId, contextWorkflowId, contextTaskId;
                {
                    std::scoped_lock<std::mutex> const lock(pendingEntry->mutex);
                    contextRunId = pendingEntry->m_Context.m_RunId;
                    contextWorkflowId = pendingEntry->m_Context.m_WorkflowId;
                    contextTaskId = pendingEntry->m_Context.m_TaskId;
                    pendingEntry->m_IsCompleted = true;
                    pendingEntry->m_IsFailed = true;
                    pendingEntry->m_ErrorMessage = "ai_call output file exceeds " +
                                                   std::to_string(kMaxOutputFileBytes) + " byte cap (size " +
                                                   std::to_string(size) + ")";
                    pendingEntry->conditionVariable.notify_all();
                }
                LOG_APP_ERROR("[AiRequestPool] OnOutputFileCreated: output file exceeds size cap run='{}' "
                              "workflow='{}' task='{}' path='{}' bytes={} cap={}",
                              contextRunId, contextWorkflowId, contextTaskId, fullFilePath, size,
                              kMaxOutputFileBytes);
                QueueCompletionIfNeeded(pendingEntry);
                return true;
            }

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

    bool AiRequestPool::OnRequestFailed(std::string const& expectedOutputPath, AiError const& error)
    {
        if (expectedOutputPath.empty())
        {
            return false;
        }
        // Containment gate: expectedOutputPath here is the same shape as the
        // map key inserted in RegisterPendingWorkflowTask, so the canonical
        // form must match.  Pass through ConfineUnderProjectRoot for symmetry;
        // a path that doesn't resolve under project root won't have a
        // registered entry anyway (containment was enforced at insert time).
        fs::path const confinedExpected = ConfineUnderProjectRoot(expectedOutputPath);
        if (confinedExpected.empty())
        {
            return false;
        }
        std::string const canonicalPath = confinedExpected.generic_string();

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
            pendingEntry->m_ErrorMessage = error.m_Message;
            pendingEntry->conditionVariable.notify_all();
        }

        // Single consolidated ERROR with the body discriminator + semantic category
        // + runId/workflowId/taskId.  This is the line the dashboard run analyzer
        // filters on (per CLAUDE.md "Failure-path logs are ERROR-level AND mention
        // the runId or workflowId as a literal substring").  Sitting-5 enrichment
        // turns "HTTP 429" into "HTTP 429 (code='insufficient_quota',
        // type='insufficient_quota', category=BillingExhausted)" so operators can
        // tell billing exhaustion from genuine throttling without inspecting the
        // transcript.  Dispatcher's intermediate 429-retries-exhausted line drops
        // to WARN to avoid a duplicate ERROR for the same failure.
        LOG_APP_ERROR("[AiRequestPool] OnRequestFailed HTTP {} (code='{}', type='{}', category={}) "
                      "run='{}' workflow='{}' task='{}' message='{}' path='{}'",
                      error.m_HttpStatus, error.m_ProviderErrorCode, error.m_ProviderErrorType,
                      CategoryToString(error.m_Category),
                      pendingEntry->m_Context.m_RunId, pendingEntry->m_Context.m_WorkflowId,
                      pendingEntry->m_Context.m_TaskId, error.m_Message, canonicalPath);

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
        //
        // Lifetime safety: only `std::string` path keys leave the lock — no
        // PendingEntry pointer crosses the boundary.  The dispatcher matches
        // by opaque cancel-key string set into QueryData::m_CancelKey at
        // submit time, so cancellation never needs an entry deref — closes
        // the UAF window where a snapshot of `shared_ptr<PendingEntry>`
        // would race with concurrent erase.
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

        JarvisAgent* jarvisAgent = dynamic_cast<JarvisAgent*>(App::g_App.load(std::memory_order_acquire));
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

        // Resolves the secret credential material for static-header AuthStyles into
        // a SecureString — the secret never touches a plain std::string allocation
        // between KeyManager and QueryData::m_ApiKey.  AwsCredential paths land the
        // (public) AccessKeyId here for legacy compatibility; the actual SigV4
        // secrets flow via QueryData::m_AwsCredential.
        SecureString ResolveApiKey(ConfigParser::EngineConfig::ApiInterface const& api)
        {
            SecureString result;
            if (Core::g_Core == nullptr)
            {
                return result;
            }
            auto extract = [&](ICredential const& cred)
            {
                // ApiKeyCredential — bearer secret (OpenAI, Anthropic, Gemini, Azure, Test).
                if (auto const* apiKey = dynamic_cast<ApiKeyCredential const*>(&cred))
                {
                    result.Set(apiKey->m_ApiKey.Get());
                    return;
                }
                // OAuthCredential — cached access token (rotated by OAuthTokenManager).
                if (auto const* oauth = dynamic_cast<OAuthCredential const*>(&cred))
                {
                    result.Set(oauth->m_AccessToken.Get());
                    return;
                }
                // AwsCredential — access_key_id is public per AWS conventions and is what the
                // SigV4 signer logs as the credential identifier.  The actual secret material
                // (secret_access_key + session_token) flows via ResolveProviderParams below.
                if (auto const* aws = dynamic_cast<AwsCredential const*>(&cred))
                {
                    result.Set(aws->m_AccessKeyId);
                    return;
                }
            };
            auto& keyManager = Core::g_Core->GetKeyManager();
            if (api.m_KeyName.empty())
            {
                keyManager.WithDefaultCredential(extract);
            }
            else
            {
                keyManager.WithCredential(api.m_KeyName, extract);
            }
            return result;
        }

        // Returns the credential's non-secret m_Params unchanged.  Historical name
        // ("ResolveProviderParams") predates the typed credential threading — secret
        // AWS material now flows through QueryData::m_AwsCredential, see
        // ResolveAwsCredentialSnapshot below.  Kept as a thin wrapper so call sites
        // don't grow an inline lambda.
        std::unordered_map<std::string, std::string> ResolveProviderParams(
            ConfigParser::EngineConfig::ApiInterface const& api)
        {
            if (Core::g_Core == nullptr) { return {}; }
            std::unordered_map<std::string, std::string> params;
            auto extract = [&](ICredential const& cred)
            {
                params = cred.m_Params;
            };
            auto& keyManager = Core::g_Core->GetKeyManager();
            if (api.m_KeyName.empty())
            {
                keyManager.WithDefaultCredential(extract);
            }
            else
            {
                keyManager.WithCredential(api.m_KeyName, extract);
            }
            return params;
        }

        // Typed AwsCredential snapshot for SigV4 paths.  Deep-copies the credential
        // under KeyManager's lock so the request's view stays stable across concurrent
        // RemoveProvider / SetDefaultProvider mutations.  Returns nullptr when the
        // resolved credential isn't an AwsCredential (caller checks AuthStyle ==
        // AwsSigV4 before calling — non-SigV4 paths get nullptr without a wasted
        // lookup).
        std::shared_ptr<AwsCredential const> ResolveAwsCredentialSnapshot(
            ConfigParser::EngineConfig::ApiInterface const& api)
        {
            if (Core::g_Core == nullptr) { return {}; }
            std::shared_ptr<AwsCredential> snap;
            auto extract = [&](ICredential const& cred)
            {
                auto const* aws = dynamic_cast<AwsCredential const*>(&cred);
                if (aws == nullptr) { return; }
                snap = std::make_shared<AwsCredential>();
                snap->m_Name         = aws->m_Name;
                snap->m_DisplayName  = aws->m_DisplayName;
                snap->m_Endpoint     = aws->m_Endpoint;
                snap->m_DefaultModel = aws->m_DefaultModel;
                snap->m_ApiType      = aws->m_ApiType;
                snap->m_Params       = aws->m_Params;
                snap->m_AccessKeyId  = aws->m_AccessKeyId;
                snap->m_SecretAccessKey.Set(aws->m_SecretAccessKey.Get());
                snap->m_SessionToken.Set(aws->m_SessionToken.Get());
                snap->m_Region       = aws->m_Region;
            };
            auto& keyManager = Core::g_Core->GetKeyManager();
            if (api.m_KeyName.empty())
            {
                keyManager.WithDefaultCredential(extract);
            }
            else
            {
                keyManager.WithCredential(api.m_KeyName, extract);
            }
            return snap;
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
        // these markers as literal substrings.  Empty strings for non-workflow
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
            // Use ConfineUnderProjectRoot for the lookup so the canonical
            // form matches RegisterPendingWorkflowTask's insertion form.
            // A symlinked queue path that resolves differently under the
            // two normalizations would otherwise cause a binding-lookup
            // miss and downgrade fail-path logs to empty run='' attribution.
            fs::path const outputPath =
                envelope.m_QueueFolder / (fs::path(envelope.m_ProbName).stem().string() + outputSuffix);
            fs::path const confinedOutputPath = ConfineUnderProjectRoot(outputPath);
            if (!confinedOutputPath.empty())
            {
                std::string const lookupKey = confinedOutputPath.generic_string();
                std::scoped_lock<std::mutex> const lock(m_OutputPathMutex);
                auto const it = m_PendingByOutputPath.find(lookupKey);
                if (it != m_PendingByOutputPath.end() && it->second != nullptr)
                {
                    runIdForLog = it->second->m_Context.m_RunId;
                    workflowIdForLog = it->second->m_Context.m_WorkflowId;
                    taskIdForLog = it->second->m_Context.m_TaskId;
                }
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

        // Locked-in variant count.  All variants dispatch generically through
        // IRequestBuilder::Create + IRateLimitStrategy::Get; mock-flagged
        // interfaces route through MockTransport at the dispatcher layer
        // (transparent to this code).  Adding a new variant: bump this
        // assertion and verify the new variant works through the generic path.
        static_assert(ConfigParser::EngineConfig::NumAPIs == 6,
                      "InterfaceType variant count changed — verify generic dispatch covers the new variant, "
                      "then bump this assertion");
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

        SecureString apiKey = ResolveApiKey(*api);
        if (apiKey.IsEmpty())
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
        std::string host;
        std::string quotaKey;
        {
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

        // Per-dispatch audit trail for plain-HTTP dispatches.  ConfigParser
        // + REST gates already enforce loopback-only for http://, so this
        // line should only ever appear for local-LLM (ollama / llama.cpp)
        // dispatch — but logging it anyway means a future regression that
        // weakens the gate is greppable in the security log.  INFO level —
        // this is noted-and-allowed, not an error.
        if (UrlPolicy::IsPlaintextHttpUrl(queryUrl))
        {
            LOG_SECURITY_INFO("[security] ai_dispatch_plaintext_http host='{}' run='{}' workflow='{}' "
                              "task='{}' interface='{}'",
                              host, runIdForLog, workflowIdForLog, taskIdForLog, api->m_Name);
        }

        int64_t const estimatedInputTokens =
            IRateLimitStrategy::Get(api->m_InterfaceType).EstimateInputTokens(combinedPrompt);

        // Size-aware in-flight budget.  Computed here, enforced by curl's
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
            // Scale by the interface's configured concurrency ceiling — covers
            // worst-case queue depth on serializing backends (ollama / local
            // LLMs) where N inflight requests take ~N× single-stream time per
            // task.  Harmless over-allocation on truly-parallel backends
            // (OpenAI / Anthropic / etc.) — the only side effect there is
            // slower detection of genuinely hung requests, which is rare and
            // worth trading for never-timeouts-from-contention reliability.
            // Removes the user-visible knob "safety_margin_factor doesn't
            // scale with concurrency" that previously required manual tuning.
            int const concurrencyFactor = std::max(1, api->m_RateLimit.m_MaxConcurrency);
            seconds *= static_cast<double>(concurrencyFactor);
            seconds *= budget.m_SafetyMarginFactor;
            seconds = std::clamp(seconds, budget.m_MinSeconds, budget.m_MaxSeconds);
            timeoutMs = static_cast<long>(seconds * 1000.0);
            LOG_APP_INFO("AiRequestPool::Submit: budget run='{}' workflow='{}' task='{}' inTok={} outTok={} "
                         "concurrencyFactor={} timeoutMs={}",
                         runIdForLog, workflowIdForLog, taskIdForLog, estimatedInputTokens, maxOutTokens,
                         concurrencyFactor, timeoutMs);
        }

        CurlWrapper::QueryData queryData{.m_Url = queryUrl, .m_Data = requestBody,
                                          .m_ApiKey = std::move(apiKey),
                                          .m_AuthStyle = authStyle, .m_TimeoutMs = timeoutMs,
                                          .m_Params = ResolveProviderParams(*api),
                                          .m_InterfaceType = static_cast<int>(api->m_InterfaceType),
                                          .m_QuotaKey = quotaKey,
                                          .m_EstimatedInputTokens = estimatedInputTokens,
                                          .m_CancelKey = {}, // populated below once expectedOutputPath is computed
                                          .m_MaxConcurrency = api->m_RateLimit.m_MaxConcurrency,
                                          .m_MaxRetries429 = api->m_RateLimit.m_MaxRetries429,
                                          .m_MaxRetriesTransient = api->m_RateLimit.m_MaxRetriesTransient,
                                          .m_BaseRetryMs = api->m_RateLimit.m_BaseRetryMs,
                                          .m_IsMock = api->m_IsMock,
                                          .m_FixturePath = api->m_FixturePath,
                                          .m_AwsCredential = (authStyle == CurlWrapper::AuthStyle::AwsSigV4)
                                              ? ResolveAwsCredentialSnapshot(*api)
                                              : std::shared_ptr<AwsCredential const>{},
                                          .m_AmzDateOverride = {}};

        JarvisAgent* jarvisAgent = dynamic_cast<JarvisAgent*>(App::g_App.load(std::memory_order_acquire));
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
            // OnRequestFailed find the registered entry.  Use ConfineUnderProjectRoot
            // for the same canonical form as RegisterPendingWorkflowTask's insert
            // (line ~346).  Empty result = path didn't resolve under project root,
            // which would also have caused the registration to fail; the empty
            // string flows through downstream checks as "no expected path".
            fs::path outputPath = queueFolder / (fs::path(probName).stem().string() + outputSuffix);
            fs::path const confinedOutputPath = ConfineUnderProjectRoot(outputPath);
            expectedOutputPath = confinedOutputPath.empty() ? std::string{} : confinedOutputPath.generic_string();
        }
        // Cancel key = expectedOutputPath (unique per workflow task per run).
        // Used by AiRequestPool::CancelRequestsForRun → dispatcher CancelByCancelKey
        // to abort in-flight HTTP requests when the calling workflow terminates.
        queryData.m_CancelKey = expectedOutputPath;

        // Inflight counter discipline (safety HIGH).  Pre-fix code used
        // `if (m_DirectDispatchInflight.load() > 0) { --m_DirectDispatchInflight; }`
        // at both decrement sites — that's a check-then-act race: two
        // concurrent decrement attempts could both observe count == 1 and
        // both decrement, underflowing the unsigned counter to SIZE_MAX.
        // Fix: capture a per-submission `decrementOnce` flag in the callback;
        // the first decrement wins (test_and_set returns false), subsequent
        // attempts no-op cleanly.  This is defense in depth — a synchronously-
        // invoked curlCallback (mock dispatcher / dispatcher error path) and
        // the schema-retry decrement-then-recurse pattern both stay balanced.
        m_DirectDispatchInflight.fetch_add(1, std::memory_order_acq_rel);
        auto decrementOnce = std::make_shared<std::atomic_flag>();

        fs::path transcriptPath;
        if (!queueFolder.empty() && !probName.empty())
        {
            transcriptPath = queueFolder / (fs::path(probName).stem().string() + ".transcript.json");
            if (!AiTranscript::AppendRequest(transcriptPath, envelope, modelCapture))
            {
                // logged inside AiTranscript
            }
        }

        if (Core::g_Core != nullptr)
        {
            auto startedEvent = std::make_shared<AiCallStartedEvent>(probName, api->m_Name);
            Core::g_Core->PushEvent(startedEvent, ProducerId::AiRequestPool);
        }

        AiInvocation envelopeForRetry = envelope;
        // Capture by value — `api` is a pointer into the config registry; the
        // callback fires asynchronously on the I/O thread, possibly after the
        // registry has been replaced by a config reload.  Per
        // feedback_capture_by_value_async.
        std::string const interfaceNameForEvent = api->m_Name;
        // Quota key feeds the Sitting-8 per-interface health snapshot's
        // `m_QuotaKey` field — lets the dashboard popover detect rows that
        // share a controller (two interfaces routing to the same host|model).
        std::string const quotaKeyForHealth = quotaKey;

        auto curlCallback = [this, interfaceType, queueFolder, probName, modelCapture, expectedOutputPath,
                             transcriptPath, callbackCopy, envelopeForRetry, decrementOnce,
                             runIdForLog, workflowIdForLog,
                             taskIdForLog, interfaceNameForEvent, quotaKeyForHealth](
                                QueryResult curlResult, std::string responseBody,
                                std::optional<int> retryAfterSeconds) mutable
        {
            AiReply aiReply;

            try
            {
                if (!curlResult.m_Ok)
                {
                    aiReply.m_Kind = AiReply::Kind::Error;
                    aiReply.m_Error.m_Kind = AiError::Kind::Http;
                    aiReply.m_Error.m_Message = curlResult.m_ErrorMessage;
                    // QueryResult::m_ErrorCode carries the HTTP status when the dispatcher
                    // landed in the "HTTP >= 400" branch, OR a CURLE_* code on transport
                    // failure (timeout, DNS, TLS handshake).  Only the former is a real
                    // HTTP status; the latter would mislead the WS payload if propagated
                    // as m_HttpStatus.
                    if (curlResult.m_ErrorCode >= 100 && curlResult.m_ErrorCode <= 599)
                    {
                        aiReply.m_Error.m_HttpStatus = curlResult.m_ErrorCode;
                    }
                    // Even when curl reports failure (HTTP 4xx/5xx), providers usually
                    // include a structured error body — parse it so the body's
                    // discriminator (insufficient_quota / rate_limit_error / etc.) and
                    // semantic category reach the WS payload + Workstream-A log line.
                    // Without this, the dispatcher short-circuit on HTTP errors hides
                    // billing-vs-throttle classification end-to-end.
                    if (!responseBody.empty())
                    {
                        auto errorParser = ReplyParser::Create(interfaceType, responseBody);
                        if (errorParser && errorParser->HasError())
                        {
                            AiError const parsedError = errorParser->GetError();
                            aiReply.m_Error.m_ProviderErrorCode = parsedError.m_ProviderErrorCode;
                            aiReply.m_Error.m_ProviderErrorType = parsedError.m_ProviderErrorType;
                            aiReply.m_Error.m_Category          = parsedError.m_Category;
                            // Prefer the parser's message when present — it's the
                            // provider's free-form body text rather than curl's
                            // generic "Bad Request" / "Too Many Requests" describe.
                            if (!parsedError.m_Message.empty())
                            {
                                aiReply.m_Error.m_Message = parsedError.m_Message;
                            }
                        }
                    }
                    aiReply.m_Error.m_RetryAfterSeconds = retryAfterSeconds;
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
                        aiReply.m_Error.m_RetryAfterSeconds = retryAfterSeconds;
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
                                    if (!AiTranscript::AppendResponse(transcriptPath, aiReply))
                                    {
                                        // logged inside AiTranscript
                                    }
                                }

                                // Schema-retry: decrement THIS submission's inflight slot
                                // (gated by decrementOnce so the eventual end-of-callback
                                // decrement won't fire again) before the recursive Submit
                                // takes a fresh slot.  Net inflight is unchanged across
                                // the retry handoff.
                                if (!decrementOnce->test_and_set(std::memory_order_acq_rel))
                                {
                                    m_DirectDispatchInflight.fetch_sub(1, std::memory_order_acq_rel);
                                }

                                if (!Submit(retryEnvelope, callbackCopy))
                                {
                                    LOG_APP_ERROR("AiRequestPool::Submit: schema-retry submission failed run='{}' "
                                                  "workflow='{}' task='{}' prob='{}'",
                                                  runIdForLog, workflowIdForLog, taskIdForLog, probName);
                                    if (!expectedOutputPath.empty())
                                    {
                                        AiError schemaRetryError;
                                        schemaRetryError.m_Kind = AiError::Kind::SchemaValidation;
                                        schemaRetryError.m_Message = "schema-retry submission failed";
                                        (void)OnRequestFailed(expectedOutputPath, schemaRetryError);
                                    }
                                }
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
                    // Best-effort signal — a missing pending entry just means the binding
                    // already completed via another path; not an error.
                    (void)OnOutputFileCreated(normalizedPath);
                }
                else if (aiReply.m_Kind == AiReply::Kind::Error && !isChunked && !expectedOutputPath.empty())
                {
                    // No .output.* file is written for error replies; without this signal the
                    // workflow runtime would stay parked in waiting_external until the ai_call
                    // deadline fires. Symmetric to the success path above.
                    (void)OnRequestFailed(expectedOutputPath, aiReply.m_Error);
                }

                if (!transcriptPath.empty())
                {
                    if (!AiTranscript::AppendResponse(transcriptPath, aiReply))
                    {
                        // logged inside AiTranscript
                    }
                }

                // Sitting-8 Workstream D: per-interface health tracking that
                // feeds /api/providers/health + the dashboard's AI Health LED.
                // Updates happen here (not inside event handlers) because the
                // event bus is fire-and-forget and the health snapshot needs
                // to be authoritative the moment any pending /api/providers/health
                // request lands.  Single short critical section per call.
                {
                    std::scoped_lock<std::mutex> const lock(m_HealthMutex);
                    InterfaceHealthState& health = m_HealthPerInterface[interfaceNameForEvent];
                    health.m_QuotaKey = quotaKeyForHealth;
                    if (aiReply.m_Kind == AiReply::Kind::Error)
                    {
                        health.m_LastErrorAt          = std::chrono::system_clock::now();
                        health.m_LastErrorCode        = aiReply.m_Error.m_ProviderErrorCode;
                        health.m_LastErrorType        = aiReply.m_Error.m_ProviderErrorType;
                        health.m_LastErrorMessage     = aiReply.m_Error.m_Message;
                        health.m_LastErrorCategory    = aiReply.m_Error.m_Category;
                        health.m_LastHttpStatus       = aiReply.m_Error.m_HttpStatus;
                        health.m_RetryAfterSeconds    = aiReply.m_Error.m_RetryAfterSeconds;
                        health.m_ConsecutiveErrors    += 1;
                        health.m_SuccessStreakSinceLastError = 0;
                    }
                    else
                    {
                        health.m_ConsecutiveErrors           = 0;
                        health.m_SuccessStreakSinceLastError += 1;
                    }
                }

                // Cap-pinned-at-floor timestamp (Sitting-8 close-out): consult
                // the dispatcher's current cap for this interface's quotaKey
                // and flip the pin timestamp at floor-boundary crossings.
                // Survives cross-refresh — the dashboard's frontend pin
                // tracker is now just a safety net for the dispatcher-down
                // case.  Done outside the m_HealthMutex critical section
                // above so we don't hold our mutex while calling into the
                // dispatcher (avoids a future lock-order hazard if the
                // dispatcher ever calls back into AiRequestPool).
                if (!quotaKeyForHealth.empty())
                {
                    JarvisAgent* japp = dynamic_cast<JarvisAgent*>(
                        App::g_App.load(std::memory_order_acquire));
                    CurlMultiDispatcher* disp = (japp != nullptr) ? japp->GetCurlMultiDispatcher() : nullptr;
                    if (disp != nullptr)
                    {
                        // Lightweight cap getter — locks ONLY m_DebugMutex (recursive),
                        // never m_InboxMutex.  This call runs inside OnTransportComplete
                        // which already holds m_DebugMutex on this thread; routing through
                        // GetDebugSnapshot would acquire m_InboxMutex while holding
                        // m_DebugMutex, opening an AB-BA deadlock with any other
                        // GetDebugSnapshot caller (web thread, /api/debug/signals).
                        int const currentCap = disp->GetCurrentConcurrencyCap(quotaKeyForHealth);
                        // Floor is hard-coded to 1 in RateLimitController
                        // (the AIMD halve-on-429 / floor-at-1 contract).
                        constexpr int kFloor = 1;
                        if (currentCap > 0)
                        {
                            std::scoped_lock<std::mutex> const lock(m_HealthMutex);
                            InterfaceHealthState& health = m_HealthPerInterface[interfaceNameForEvent];
                            auto const epoch = std::chrono::system_clock::time_point{};
                            bool const wasPinned = (health.m_CapPinnedAtFloorSince != epoch);
                            bool const isPinned  = (currentCap <= kFloor);
                            if (isPinned && !wasPinned)
                            {
                                health.m_CapPinnedAtFloorSince = std::chrono::system_clock::now();
                            }
                            else if (!isPinned && wasPinned)
                            {
                                health.m_CapPinnedAtFloorSince = epoch;
                            }
                        }
                    }
                }

                if (Core::g_Core != nullptr)
                {
                    if (aiReply.m_Kind == AiReply::Kind::Error)
                    {
                        auto failedEvent = std::make_shared<AiCallFailedEvent>(probName, interfaceNameForEvent,
                                                                               aiReply.m_Error);
                        Core::g_Core->PushEvent(failedEvent, ProducerId::AiRequestPool);
                    }
                    else
                    {
                        auto completedEvent =
                            std::make_shared<AiCallCompletedEvent>(probName, interfaceNameForEvent,
                                                                   aiReply.m_Usage, aiReply.m_FinishReason);
                        Core::g_Core->PushEvent(completedEvent, ProducerId::AiRequestPool);
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
                    // Best-effort failure signal — exception path; pending entry may
                    // already have been resolved by the prior success branch.
                    (void)OnRequestFailed(expectedOutputPath, errorReply.m_Error);
                }
                if (callbackCopy)
                {
                    callbackCopy(errorReply);
                }
            }

            // Final inflight decrement (gated by decrementOnce — see Submit-side
            // comment).  Atomic flag ensures at-most-once dec per submission even
            // if the callback is somehow invoked twice.
            if (!decrementOnce->test_and_set(std::memory_order_acq_rel))
            {
                m_DirectDispatchInflight.fetch_sub(1, std::memory_order_acq_rel);
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

    // ------------------------------------------------------------------------
    // Sitting-8 Workstream D: per-interface health snapshot
    // ------------------------------------------------------------------------
    //
    // Joins three sources:
    //   - config.m_ApiInterfaces       (identity: name, type, mock flag)
    //   - m_HealthPerInterface         (last-error + streak counters)
    //   - dispatcher GetDebugSnapshot  (per-quotaKey AIMD cap state)
    //
    // The join key from interface → controller is `quotaKey`.  Remembered
    // per-interface from the most recent Submit() call (stored in
    // InterfaceHealthState::m_QuotaKey).  Interfaces that have never
    // dispatched have no quotaKey yet — they appear in the snapshot with
    // cap fields = -1 (UI renders "—").
    std::vector<ProviderHealthSnapshot> AiRequestPool::SnapshotProviderHealth() const
    {
        std::vector<ProviderHealthSnapshot> result;

        if (Core::g_Core == nullptr)
        {
            return result;
        }
        auto const& config = Core::g_Core->GetConfig();
        result.reserve(config.m_ApiInterfaces.size());

        // Pull dispatcher controller state once (avoids re-locking per interface).
        std::unordered_map<std::string, CurlMultiDispatcher::DebugSnapshot::ControllerEntry> controllerByKey;
        JarvisAgent* jarvisAgent = dynamic_cast<JarvisAgent*>(App::g_App.load(std::memory_order_acquire));
        CurlMultiDispatcher* dispatcher = (jarvisAgent != nullptr) ? jarvisAgent->GetCurlMultiDispatcher() : nullptr;
        if (dispatcher != nullptr)
        {
            CurlMultiDispatcher::DebugSnapshot const snap = dispatcher->GetDebugSnapshot();
            for (auto const& entry : snap.m_Controllers)
            {
                controllerByKey.emplace(entry.m_QuotaKey, entry);
            }
        }

        // Snapshot the health map under lock — short critical section, no I/O
        // inside.  Joining with config + dispatcher happens after release.
        std::unordered_map<std::string, InterfaceHealthState> healthCopy;
        {
            std::scoped_lock<std::mutex> const lock(m_HealthMutex);
            healthCopy = m_HealthPerInterface;
        }

        auto const interfaceTypeName = [](ConfigParser::EngineConfig::InterfaceType type) -> char const* {
            switch (type)
            {
                case ConfigParser::EngineConfig::InterfaceType::API1: return "API1";
                case ConfigParser::EngineConfig::InterfaceType::API2: return "API2";
                case ConfigParser::EngineConfig::InterfaceType::API3: return "API3";
                case ConfigParser::EngineConfig::InterfaceType::API4: return "API4";
                case ConfigParser::EngineConfig::InterfaceType::API5: return "API5";
                case ConfigParser::EngineConfig::InterfaceType::API6: return "API6";
                case ConfigParser::EngineConfig::InterfaceType::NumAPIs:
                case ConfigParser::EngineConfig::InterfaceType::InvalidAPI:
                    return "InvalidAPI";
            }
            return "InvalidAPI";
        };

        for (auto const& api : config.m_ApiInterfaces)
        {
            ProviderHealthSnapshot snap;
            snap.m_InterfaceName     = api.m_Name;
            snap.m_InterfaceTypeName = interfaceTypeName(api.m_InterfaceType);
            snap.m_IsMock            = api.m_IsMock;
            snap.m_MaxCap            = api.m_RateLimit.m_MaxConcurrency;
            snap.m_FloorCap          = 1;   // RateLimitController hard-codes floor=1

            auto const healthIt = healthCopy.find(api.m_Name);
            if (healthIt != healthCopy.end())
            {
                snap.m_QuotaKey                    = healthIt->second.m_QuotaKey;
                snap.m_LastErrorAt                 = healthIt->second.m_LastErrorAt;
                snap.m_LastErrorCode               = healthIt->second.m_LastErrorCode;
                snap.m_LastErrorType               = healthIt->second.m_LastErrorType;
                snap.m_LastErrorMessage            = healthIt->second.m_LastErrorMessage;
                snap.m_LastErrorCategory           = healthIt->second.m_LastErrorCategory;
                snap.m_LastHttpStatus              = healthIt->second.m_LastHttpStatus;
                snap.m_RetryAfterSeconds           = healthIt->second.m_RetryAfterSeconds;
                snap.m_ConsecutiveErrors           = healthIt->second.m_ConsecutiveErrors;
                snap.m_SuccessStreakSinceLastError = healthIt->second.m_SuccessStreakSinceLastError;
                snap.m_CapPinnedAtFloorSince       = healthIt->second.m_CapPinnedAtFloorSince;
            }

            auto const ctrlIt = controllerByKey.find(snap.m_QuotaKey);
            if (ctrlIt != controllerByKey.end())
            {
                snap.m_CurrentCap = ctrlIt->second.m_CurrentConcurrencyCap;
            }

            result.push_back(std::move(snap));
        }
        return result;
    }

    bool AiRequestPool::TestInterface(size_t interfaceIndex, std::string& outResponsePreview, std::string& outError,
                                       int64_t& outLatencyMs)
    {
        outResponsePreview.clear();
        outError.clear();
        outLatencyMs = 0;

        static constexpr long kTestTimeoutMs = 90000; // 90 seconds — covers real-cloud variance,
                                                     // LocalStack Bedrock's first-call cold path, and
                                                     // — the binding case — a local Ollama/vLLM endpoint
                                                     // cold-loading a large model into VRAM/RAM on the
                                                     // first probe.  A 32B model that spills to CPU on a
                                                     // modest GPU can take well over 30 s to load; shipped
                                                     // users run slower hardware than the dev box, so the
                                                     // probe must not false-fail a healthy local interface.

        auto const& config = Core::g_Core->GetConfig();
        if (interfaceIndex >= config.m_ApiInterfaces.size())
        {
            outError = "Interface index " + std::to_string(interfaceIndex) + " out of range (have " +
                       std::to_string(config.m_ApiInterfaces.size()) + ")";
            return false;
        }

        auto const& iface = config.m_ApiInterfaces[interfaceIndex];

        // Resolve API key + provider params from KeyManager. Params carry SigV4
        // material (region + secret_access_key + session_token) for AWS providers
        // and any future per-provider extras.
        SecureString apiKey;
        std::unordered_map<std::string, std::string> providerParams;
        {
            auto extract = [&](ICredential const& cred)
            {
                // ApiKeyCredential is the bearer-secret case (OpenAI, Anthropic, Gemini, Azure).
                // OAuthCredential carries the cached access token in m_AccessToken (rotated by
                // OAuthTokenManager's hydrate / refresh paths; the cache is what gets persisted
                // into KeyManager and read here).  Other subtypes don't fit AI dispatch and
                // leave apiKey empty — the empty-check below produces a clear error.
                if (auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred))
                {
                    apiKey.Set(api->m_ApiKey.Get());
                }
                else if (auto const* oauth = dynamic_cast<OAuthCredential const*>(&cred))
                {
                    apiKey.Set(oauth->m_AccessToken.Get());
                }
                providerParams = cred.m_Params;
            };
            auto& keyManager = Core::g_Core->GetKeyManager();
            if (iface.m_KeyName.empty())
            {
                keyManager.WithDefaultCredential(extract);
            }
            else
            {
                keyManager.WithCredential(iface.m_KeyName, extract);
            }
        }

        if (apiKey.IsEmpty())
        {
            outError = "No API key configured for key_name '" + iface.m_KeyName + "'";
            return false;
        }

        // Delegate request-body assembly to IRequestBuilder so every interface type
        // (including API4 Anthropic and the Test fixture) is handled uniformly.
        std::string const prompt = "Say hello";
        AiInvocation probeEnvelope;
        probeEnvelope.m_InterfaceName = iface.m_Name;
        Message probeMessage;
        probeMessage.m_Role = MessageRole::User;
        probeMessage.m_Content = prompt;
        probeEnvelope.m_Messages.push_back(std::move(probeMessage));

        auto const probeBuilder = IRequestBuilder::Create(iface.m_InterfaceType);
        if (!probeBuilder)
        {
            outError = "No request builder available for interface type (index " + std::to_string(interfaceIndex) + ")";
            return false;
        }

        std::string const requestData = probeBuilder->BuildBody(probeEnvelope, iface.m_Model);
        std::string const queryUrl = probeBuilder->ResolveUrl(iface.m_Url, iface.m_Model);
        CurlWrapper::AuthStyle const authStyle = probeBuilder->GetAuthStyle();

        // Direct curl POST with a short timeout for the connectivity probe.
        // Test-connection path: synchronous CurlManager::Query, not the
        // dispatcher's adaptive controller — controller / retry-budget fields
        // are unused here, set to defaults so the dispatcher's per-field
        // fallbacks apply if this QueryData ever did flow through.
        CurlWrapper::QueryData queryData = {
            .m_Url = queryUrl,
            .m_Data = requestData,
            .m_ApiKey = std::move(apiKey),
            .m_AuthStyle = authStyle,
            .m_TimeoutMs = kTestTimeoutMs,
            .m_Params = std::move(providerParams),
            .m_InterfaceType = -1,
            .m_QuotaKey = {},
            .m_EstimatedInputTokens = -1,
            .m_CancelKey = {},
            .m_MaxConcurrency = -1,
            .m_MaxRetries429 = -1,
            .m_MaxRetriesTransient = -1,
            .m_BaseRetryMs = -1,
            .m_IsMock = false,
            .m_FixturePath = {},
            .m_AwsCredential = (authStyle == CurlWrapper::AuthStyle::AwsSigV4)
                ? ResolveAwsCredentialSnapshot(iface)
                : std::shared_ptr<AwsCredential const>{},
            .m_AmzDateOverride = {},
        };

        auto const startTime = std::chrono::steady_clock::now();

        auto& curl = CurlManager::GetThreadCurl();
        curl.Clear();
        QueryResult result = curl.Query(queryData);

        auto const endTime = std::chrono::steady_clock::now();
        outLatencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

        if (!result.m_Ok)
        {
            outError = result.m_ErrorMessage.empty() ? QueryErrorCode::Describe(result.m_ErrorCode) : result.m_ErrorMessage;
            LOG_APP_INFO("[AiRequestPool] TestInterface: index={} name='{}' FAILED latency={}ms error='{}'",
                         interfaceIndex, iface.m_Name, outLatencyMs, outError);
            return false;
        }

        // Parse response to extract a preview.
        auto parser = ReplyParser::Create(iface.m_InterfaceType, curl.GetBuffer());
        if (parser && parser->HasContent() > 0)
        {
            std::string content = parser->GetContent(0);
            constexpr size_t kPreviewLen = 200;
            outResponsePreview = content.size() > kPreviewLen ? content.substr(0, kPreviewLen) + "..." : content;
        }
        else
        {
            outResponsePreview = "(response received, " + std::to_string(curl.GetBuffer().size()) + " bytes)";
        }

        LOG_APP_INFO("[AiRequestPool] TestInterface: index={} name='{}' OK latency={}ms responseLen={}", interfaceIndex,
                     iface.m_Name, outLatencyMs, curl.GetBuffer().size());

        return true;
    }
} // namespace AIAssistant
