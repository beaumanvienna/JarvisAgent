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

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <memory>
#include <queue>
#include <vector>

namespace AIAssistant
{
    namespace ProbUtils
    {
        struct ProbFileInfo;
    }

    struct AiRequestHandle
    {
        int64_t requestId = 0;
        int64_t requestTimestampNs = 0;

        bool IsValid() const { return (requestId > 0) && (requestTimestampNs > 0); }
    };

    struct AiRequestCompletion
    {
        std::string m_WorkflowId;
        std::string m_RunId;
        std::string m_TaskId;

        bool m_WasFailed = false;
        std::string m_ResponseText;
        std::string m_ErrorMessage;

        // Deterministic outputs for downstream dataflow.
        // Values are always file paths (never raw text in memory).
        std::unordered_map<std::string, std::string> m_OutputValues;
    };

    // Tracks pending ai_call requests and allows reacting to their corresponding
    // output artifacts.  Supports two completion mechanisms:
    //
    //  1. PROB_<id>_<ts> naming — legacy path via OnProbFileEvent().
    //  2. Path-based matching  — new path via OnOutputFileCreated().
    //     The executor registers the expected output path (e.g. PROB_NVDA.output.txt);
    //     the pool matches incoming file events by canonical path.
    //
    // IMPORTANT:
    //  - This pool must not require blocking waits for the workflow runtime.
    //  - Timeouts are handled via Update(), which should be called periodically.
    //  - Completions are queued and can be drained non-blocking via TryPopCompletion().
    class AiRequestPool final
    {
    public:
        AiRequestPool() = default;
        ~AiRequestPool() = default;

        AiRequestPool(AiRequestPool const&) = delete;
        AiRequestPool& operator=(AiRequestPool const&) = delete;

        int64_t AllocateRequestId();

        // Registers a pending request with a default timeout (currently 5 minutes).
        AiRequestHandle RegisterPending(AiRequestHandle const& requestHandle);

        // Registers a pending request with an explicit timeout.
        // timeoutMs == 0 means "use default".
        AiRequestHandle RegisterPending(AiRequestHandle const& requestHandle, uint64_t const timeoutMs);

        // Registers a pending request and associates it with a specific workflow task instance.
        // This is the preferred path for workflow ai_call tasks.
        //
        // outputFilePaths:
        //  - If non-empty: response is written to these paths on completion, and output values map to paths.
        //  - If empty: the source .output.txt path (created by the core engine) is used as the default.
        //
        // outputSlotNames:
        //  - Used to deterministically map slots to file output paths.
        AiRequestHandle RegisterPendingWorkflowTask(AiRequestHandle const& requestHandle, std::string const& workflowId,
                                                    std::string const& runId, std::string const& taskId,
                                                    std::vector<std::string> const& outputFilePaths,
                                                    std::vector<std::string> const& outputSlotNames,
                                                    uint64_t const timeoutMs, std::string const& expectedOutputPath = "");

        // Returns true if the event was consumed by this pool.
        bool OnProbFileEvent(ProbUtils::ProbFileInfo const& probFileInfo, std::string const& fullFilePath);

        // Path-based completion: called for any .output.txt file event.
        // Returns true if the path matches a registered expected output.
        bool OnOutputFileCreated(std::string const& fullFilePath);

        // Signal that the SessionManager has dispatched a curl request for a PROB file.
        // Confirms the handoff from file-placement to HTTP dispatch actually happened.
        // probFilePath: absolute path of the PROB file being sent.
        void OnCurlDispatched(std::string const& probFilePath);

        // Non-blocking: if the request is completed (success or failure), returns true and consumes the result.
        // If not completed yet, returns false.
        bool TryConsumeResult(AiRequestHandle const& requestHandle, bool& outWasFailed, std::string& outResponseText,
                              std::string& outErrorMessage);

        // Non-blocking: pop the next queued workflow completion (if any).
        //
        // NOTE:
        // - This is intended for the tick-based workflow runtime.
        // - The consumer should call Forget(handle) when it is done with the request entry,
        //   OR use TryConsumeResult() instead of this queue mechanism (not both).
        bool TryPopCompletion(AiRequestCompletion& outCompletion);

        // Periodic maintenance: applies timeouts to pending requests.
        // Should be called from JarvisAgent::OnUpdate().
        void Update();

        // Waits for completion (or timeout). Returns true on success, false on failure/timeout.
        // NOTE: Prefer TryPopCompletion()+Update() for workflow runtime (non-blocking).
        bool WaitForCompletion(AiRequestHandle const& requestHandle, uint64_t const timeoutMs, std::string& outResponseText,
                               std::string& outErrorMessage);

        // Removes any pending entry (safe to call even if the entry does not exist).
        void Forget(AiRequestHandle const& requestHandle);

    private:
        struct RequestContext
        {
            bool m_HasWorkflowBinding = false;

            std::string m_WorkflowId;
            std::string m_RunId;
            std::string m_TaskId;

            std::vector<std::string> m_OutputFilePaths;
            std::vector<std::string> m_OutputSlotNames;

            std::string m_ExpectedOutputPath;
        };

        struct PendingEntry
        {
            std::mutex mutex;
            std::condition_variable conditionVariable;

            AiRequestHandle m_Handle{};

            bool m_IsCompleted = false;
            bool m_IsFailed = false;

            std::string m_ResponseText;
            std::string m_ErrorMessage;

            // Path of the .output.txt file that triggered completion (set by OnProbFileEvent).
            std::string m_SourceOutputFilePath;

            bool m_HasDeadline = false;
            std::chrono::steady_clock::time_point m_Deadline;

            bool m_CurlDispatched = false;

            bool m_HasQueuedCompletion = false;
            RequestContext m_Context;
        };

        struct RequestKey
        {
            int64_t requestId = 0;
            int64_t requestTimestampNs = 0;

            bool operator==(RequestKey const& other) const
            {
                return (requestId == other.requestId) && (requestTimestampNs == other.requestTimestampNs);
            }
        };

        struct RequestKeyHasher
        {
            std::size_t operator()(RequestKey const& requestKey) const noexcept
            {
                std::size_t const h1 = std::hash<int64_t>{}(requestKey.requestId);
                std::size_t const h2 = std::hash<int64_t>{}(requestKey.requestTimestampNs);
                return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
            }
        };

        static RequestKey MakeKey(AiRequestHandle const& requestHandle);

        void QueueCompletionIfNeeded(std::shared_ptr<PendingEntry> const& pendingEntry);

    private:
        std::mutex m_MapMutex;
        std::unordered_map<RequestKey, std::shared_ptr<PendingEntry>, RequestKeyHasher> m_PendingRequests;

        std::mutex m_CompletedMutex;
        std::queue<AiRequestCompletion> m_Completed;

        std::mutex m_OutputPathMutex;
        std::unordered_map<std::string, std::shared_ptr<PendingEntry>> m_PendingByOutputPath;

        std::mutex m_IdMutex;
        int64_t m_NextRequestId = 1;
    };
} // namespace AIAssistant
