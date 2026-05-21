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
#include "workflow/providerHealth.h"

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
    //
    // --- Threading & lifetime contract -------------------------------------
    // Mutex layout:
    //   m_MapMutex         guards m_PendingRequests
    //   m_CompletedMutex   guards m_Completed
    //   m_OutputPathMutex  guards m_PendingByOutputPath
    //   m_IdMutex          guards m_NextRequestId
    //   PendingEntry::mutex guards per-entry mutable state (response/error/flags)
    //
    // Lock-acquire order when nesting is required (top → bottom):
    //   m_OutputPathMutex → PendingEntry::mutex
    //   m_MapMutex        → PendingEntry::mutex
    // The two pool-level mutexes (m_OutputPathMutex, m_MapMutex) are never
    // held simultaneously; deadlock-free by construction.
    //
    // Path containment:
    //   Every filesystem-touching path here (filePath into WriteTextFile,
    //   fullFilePath into OnOutputFileCreated, expectedOutputPath into
    //   RegisterPendingWorkflowTask / OnRequestFailed / Submit's cancel-key
    //   builder) is gated by `application/file/pathConfinement.h::
    //   ConfineUnderProjectRoot()`.  The same canonical form is used for
    //   m_PendingByOutputPath insertion and lookup, so a hostile JCWF
    //   `expected_output` field can't escape the project tree on either side
    //   of the comparison.
    //
    // Erasure invariant for m_PendingByOutputPath:
    //   Each entry is inserted exactly once (in RegisterPendingWorkflowTask)
    //   and erased by exactly one of OnOutputFileCreated() / OnRequestFailed()
    //   — whichever fires first.  The OTHER call's lookup returns end() and
    //   short-circuits via `return false`, so a "double-remove" is structurally
    //   impossible (it's a "second-lookup-misses-and-bails" pattern).  Do not
    //   add new erasure sites without re-checking this invariant.
    //
    // App::g_App downcast:
    //   `dynamic_cast<JarvisAgent*>(App::g_App.load(std::memory_order_acquire))`
    //   appears at the dispatcher resolution sites (Submit, CancelRequestsForRun).
    //   The runtime contract is that the application's global is always
    //   JarvisAgent in production builds, but defense in depth: the nullptr
    //   branch is handled at every call site (ERROR-log + early return without
    //   touching the inflight counter).  The acquire load pairs with the
    //   release store in JarvisAgent::OnStart so background workers that race
    //   the engine startup either observe a fully-constructed JarvisAgent or
    //   nullptr — never a torn pointer.  If a future refactor wants to remove
    //   dynamic_cast, the replacement should still preserve both the atomic
    //   load and the nullptr-recovery behaviour.
    class AiRequestPool final
    {
    public:
        AiRequestPool() = default;
        ~AiRequestPool() = default;

        void Shutdown();

        AiRequestPool(AiRequestPool const&) = delete;
        AiRequestPool& operator=(AiRequestPool const&) = delete;

        int64_t AllocateRequestId();

        // Registers a pending request.  Per-attempt timeout enforcement now
        // lives at curl level (CURLOPT_TIMEOUT_MS, set from the size-aware
        // budget in Submit) — AiRequestPool only retains the file-activity
        // watchdog (catches "executor wrote queue files but Submit was never
        // called") and the workflow-binding bookkeeping.
        AiRequestHandle RegisterPending(AiRequestHandle const& requestHandle);

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
                                                    std::string const& expectedOutputPath = "");

        // Path-based completion: called by Submit's reply callback once the .output.*
        // artifact has been written.  Looks up the pending entry by canonical path
        // and signals completion to blocking waiters / the workflow runtime queue.
        // Returns true if the path matched a registered expected output.
        [[nodiscard]] bool OnOutputFileCreated(std::string const& fullFilePath);

        // Symmetric failure path: called when the AI request errored before any
        // .output.* file could be written (HTTP failure, parse error, transport
        // exception). Marks the matching pending entry failed and queues a failed
        // completion so the workflow runtime transitions out of waiting_external.
        // Returns true if the path matched a registered expected output.
        //
        // `error` carries the rich `AiError` (provider code/type, semantic
        // category, Retry-After hint, HTTP status) and is logged at ERROR with
        // run/workflow/task IDs in scope — this is the line the dashboard run
        // analyzer surfaces.  Callers without an `AiError` handy (e.g. exception
        // paths) construct an `AiError{Kind::Transport, 0, e.what()}` so the
        // log line still gets a kind + message.
        [[nodiscard]] bool OnRequestFailed(std::string const& expectedOutputPath, AiError const& error);

        // Resets the file-activity watchdog for the given request.
        // Call after each queue-file write so the watchdog knows the executor is still making progress.
        // The watchdog fires if no file activity AND no curl dispatch happens within the watchdog window.
        void KickFileActivityWatchdog(AiRequestHandle const& requestHandle);

        // Non-blocking: if the request is completed (success or failure), returns true and consumes the result.
        // If not completed yet, returns false.
        [[nodiscard]] bool TryConsumeResult(AiRequestHandle const& requestHandle, bool& outWasFailed,
                                            std::string& outResponseText, std::string& outErrorMessage);

        // Non-blocking: pop the next queued workflow completion (if any).
        //
        // NOTE:
        // - This is intended for the tick-based workflow runtime.
        // - The consumer should call Forget(handle) when it is done with the request entry,
        //   OR use TryConsumeResult() instead of this queue mechanism (not both).
        [[nodiscard]] bool TryPopCompletion(AiRequestCompletion& outCompletion);

        // Periodic maintenance: applies timeouts to pending requests.
        // Should be called from JarvisAgent::OnUpdate().
        void Update();

        // Waits for completion (or timeout). Returns true on success, false on failure/timeout.
        // NOTE: Prefer TryPopCompletion()+Update() for workflow runtime (non-blocking).
        [[nodiscard]] bool WaitForCompletion(AiRequestHandle const& requestHandle, uint64_t const timeoutMs,
                                             std::string& outResponseText, std::string& outErrorMessage);

        // Removes any pending entry (safe to call even if the entry does not exist).
        void Forget(AiRequestHandle const& requestHandle);

        // Cascade cancellation: when a workflow run terminates (failed,
        // cancelled, completed-with-failures), abort any in-flight HTTP
        // requests bound to that run.  Without this, dispatched curl requests
        // continue against the AI provider after the calling workflow is gone
        // — burning tokens with no consumer.  Iterates pending entries with
        // m_Context.m_RunId == runId and forwards each to the dispatcher's
        // CancelByCancelKey path.  Idempotent; safe to call from any thread.
        void CancelRequestsForRun(std::string const& runId);

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
        [[nodiscard]] bool Submit(AiInvocation const& envelope, ReplyCallback onReply);

        // Synchronous "Say hello" connectivity probe for a configured AI
        // interface.  Used by the dashboard Test button + Engine's
        // /api/ai/interfaces/<n>/test route.  Resolves the credential through
        // KeyManager, builds the request body via the per-provider
        // IRequestBuilder, and posts directly through CurlManager (NOT the
        // async dispatcher — this is a one-shot operational verification, not
        // a workflow ai_call).  Returns true on HTTP success; populates
        // `outResponsePreview` with up to 200 chars of parsed content,
        // `outError` on failure, and `outLatencyMs` for both paths.
        [[nodiscard]] bool TestInterface(size_t interfaceIndex, std::string& outResponsePreview, std::string& outError,
                                          int64_t& outLatencyMs);

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

        // Per-interface health snapshot consumed by /api/providers/health
        // (Sitting-8 Workstream D).  Joins the pool's per-interface
        // last-error tracking with the dispatcher's per-`quotaKey` controller
        // cap state to produce one entry per configured `api_interfaces[]`.
        // Single critical section per `m_HealthMutex` for internal consistency
        // — the LED can't render a torn read of cap-from-now + error-from-last-tick.
        [[nodiscard]] std::vector<ProviderHealthSnapshot> SnapshotProviderHealth() const;

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

            // Set by OnCurlDispatchedForOutputPath when curl_multi_add_handle
            // fires.  Used by the file-activity watchdog to know it can stop
            // tracking inactivity (the per-attempt timeout is owned by curl
            // via CURLOPT_TIMEOUT_MS once the request is on the wire).
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

        // Disarms the file-activity watchdog for the workflow-bound entry at
        // `outputAbsolutePath`.  Called from Submit at the moment of handoff
        // to the dispatcher: from this point the per-attempt timeout is owned
        // by curl (CURLOPT_TIMEOUT_MS), and any further wait (controller
        // throttle, retry-queue backoff) is legitimate and must not trigger
        // the watchdog.  No-op when no binding exists (e.g. non-workflow
        // AiJcwfService / assistant calls).
        void OnSubmitHandoff(std::string const& outputAbsolutePath);

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

        // Per-interface health tracking (Sitting-8 Workstream D) — updated by
        // the curl callback on every completion (success bumps the success
        // streak; failure records last_error_* + bumps consecutive_errors).
        // SnapshotProviderHealth() joins this with the dispatcher's controller
        // state to produce the wire-format snapshot.  Mutex covers both the
        // map AND the entries (writes happen on the I/O thread; reads on the
        // web thread).
        mutable std::mutex m_HealthMutex;
        std::unordered_map<std::string, InterfaceHealthState> m_HealthPerInterface;

        std::mutex m_IdMutex;
        int64_t m_NextRequestId = 1;

        std::atomic<bool> m_ShuttingDown{false};
    };
} // namespace AIAssistant
