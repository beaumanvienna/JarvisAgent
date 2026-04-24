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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <memory>
#include <queue>
#include <vector>

#include "workflow/aiInvocation.h"
#include "workflow/aiReply.h"

namespace AIAssistant
{
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

    // Tracks pending ai_call requests and delivers completions back to the workflow
    // runtime (non-blocking) or directly to the caller via Submit's reply callback.
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

        void Shutdown();

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

        // Path-based completion: called by Submit's reply callback once the .output.*
        // artifact has been written.  Looks up the pending entry by canonical path
        // and signals completion to blocking waiters / the workflow runtime queue.
        // Returns true if the path matched a registered expected output.
        bool OnOutputFileCreated(std::string const& fullFilePath);

        // Resets the file-activity watchdog for the given request.
        // Call after each queue-file write so the watchdog knows the executor is still making progress.
        // The watchdog fires if no file activity AND no curl dispatch happens within the watchdog window.
        void KickFileActivityWatchdog(AiRequestHandle const& requestHandle);

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

        // Direct envelope-driven dispatch.  Builds the HTTP body via the per-provider
        // IRequestBuilder, submits to the shared CurlMultiDispatcher, and invokes `onReply`
        // on the I/O thread when the response arrives.
        //
        // Writes <prob>.output.{txt,json} on success and calls OnOutputFileCreated so any
        // workflow binding registered via RegisterPendingWorkflowTask completes deterministically.
        //
        // Returns false if dispatch could not be submitted (no dispatcher, invalid interface,
        // empty body). On false, `onReply` is not invoked.
        using ReplyCallback = std::function<void(AiReply const&)>;
        bool Submit(AiInvocation const& envelope, ReplyCallback onReply);

        // Count of envelope-based dispatches currently in flight.  Consumed by the TUI/dashboard
        // "queries in flight" LED and the `/api/status` endpoint.
        size_t GetDirectDispatchInflight() const { return m_DirectDispatchInflight.load(); }

        // Observability counters for /api/debug/signals — confirm that schema
        // enforcement and chunking are actually firing in live runs without
        // needing to parse transcripts or logs.
        uint64_t GetStructuredSubmissions() const { return m_StructuredSubmissions.load(); }
        uint64_t GetSchemaValidationRetries() const { return m_SchemaValidationRetries.load(); }
        uint64_t GetSchemaValidationFailures() const { return m_SchemaValidationFailures.load(); }
        uint64_t GetChunkedDispatches() const { return m_ChunkedDispatches.load(); }
        uint64_t GetFenceStrips() const { return m_FenceStrips.load(); }

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

            // Path of the .output.{txt,json} file that triggered completion (set by OnOutputFileCreated).
            std::string m_SourceOutputFilePath;

            bool m_HasDeadline = false;
            std::chrono::steady_clock::time_point m_Deadline;
            uint64_t m_TimeoutMs = 0; // stored so the deadline can be deferred until curl dispatch

            bool m_CurlDispatched = false;

            // File-activity watchdog.  Active from registration until Submit hands
            // off to curl.  Fires if no file writes and no curl dispatch happen
            // within the watchdog window.
            bool m_FileActivityWatchdogActive = true;
            std::chrono::steady_clock::time_point m_FileActivityDeadline;

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

        // Disarm the file-activity watchdog and activate the main deadline for the
        // workflow-bound entry registered at `outputAbsolutePath`.  No-op when no
        // binding exists (e.g. non-workflow AiJcwfService / assistant calls).
        void ActivateDeadlineForOutputPath(std::string const& outputAbsolutePath);

    private:
        std::mutex m_MapMutex;
        std::unordered_map<RequestKey, std::shared_ptr<PendingEntry>, RequestKeyHasher> m_PendingRequests;

        std::mutex m_CompletedMutex;
        std::queue<AiRequestCompletion> m_Completed;

        std::mutex m_OutputPathMutex;
        std::unordered_map<std::string, std::shared_ptr<PendingEntry>> m_PendingByOutputPath;

        std::atomic<size_t> m_DirectDispatchInflight{0};

        // Observability counters — lifetime-monotonic increments.
        std::atomic<uint64_t> m_StructuredSubmissions{0};
        std::atomic<uint64_t> m_SchemaValidationRetries{0};
        std::atomic<uint64_t> m_SchemaValidationFailures{0};
        std::atomic<uint64_t> m_ChunkedDispatches{0};
        std::atomic<uint64_t> m_FenceStrips{0};

        std::mutex m_IdMutex;
        int64_t m_NextRequestId = 1;

        std::atomic<bool> m_ShuttingDown{false};
    };
} // namespace AIAssistant
