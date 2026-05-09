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

#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "curlWrapper.h"
#include "rateLimitController.h"

// Opaque libcurl types — avoid pulling in curl headers in this header.
typedef void CURLM;
struct curl_slist;

namespace AIAssistant
{
    // CurlMultiDispatcher — a single dedicated I/O thread that drives all AI HTTP/2
    // requests via libcurl's multi interface. All concurrent requests to the same host
    // share one TCP/TLS connection via HTTP/2 stream multiplexing; no thread-pool
    // threads are blocked on network I/O.
    //
    // Thread safety: Submit() is callable from any thread.
    // Callbacks fire on the I/O thread — keep them short (file writes are fine).
    class CurlMultiDispatcher
    {
    public:
        // callback(result, responseBody) fires on the I/O thread when the request completes.
        using Callback = std::function<void(QueryResult, std::string /*responseBody*/)>;

        CurlMultiDispatcher();
        ~CurlMultiDispatcher();

        CurlMultiDispatcher(CurlMultiDispatcher const&) = delete;
        CurlMultiDispatcher& operator=(CurlMultiDispatcher const&) = delete;

        // Enqueue a request. Thread-safe. Callback fires on the I/O thread.
        void Submit(CurlWrapper::QueryData const& data, Callback callback);

        // Cascade cancellation: abort every request whose QueryData::m_CancelKey
        // matches `cancelKey` — across the inbox, retry queue, AND active set.
        // Used by AiRequestPool::CancelRequestsForRun to abort in-flight HTTP
        // requests whose calling workflow has terminated, so we don't keep
        // burning tokens against the AI provider for a run that already failed.
        // Thread-safe; the actual aborts run on the I/O thread (curl handle
        // mutations must be single-threaded relative to curl_multi_perform).
        // Each cancelled request fires its callback with QueryResult::Fail.
        void CancelByCancelKey(std::string const& cancelKey);

        // Phase-1 shutdown: signal the I/O thread to stop accepting new requests.
        void SignalStop();
        // Phase-2 shutdown: block until the I/O thread has exited.
        void WaitStop();

        // Debug introspection. Cheap, lock-acquired-briefly. Surfaced via /api/debug/signals.
        struct DebugSnapshot
        {
            uint64_t m_TotalDispatched{0};
            uint64_t m_TotalThrottled{0};
            uint64_t m_Total429s{0};
            uint64_t m_TotalRetriesExhausted{0};
            uint64_t m_TotalCompleted{0};
            uint64_t m_TotalCancelled{0};
            size_t m_InboxSize{0};
            size_t m_ActiveCount{0};
            size_t m_RetryQueueSize{0};

            struct HostEntry
            {
                std::string m_Host;
                int m_RemainingRequests{-1};
                int m_RemainingTokens{-1};
                long m_ReqResetInSec{-1};
                long m_TokResetInSec{-1};
                size_t m_ActiveCount{0};
            };
            std::vector<HostEntry> m_Hosts;

            // Per-(host, modelFamily) RateLimitController state.  Keyed by the
            // QuotaKey set by AiRequestPool::Submit.  Reflects AIMD cap +
            // streak counter + last observation merged into the controller.
            struct ControllerEntry
            {
                std::string m_QuotaKey;
                int m_CurrentConcurrencyCap{0};
                int m_StreakSinceLast429{0};
                int64_t m_RemainingRequests{-1};
                int64_t m_RemainingTokens{-1};
                long m_ReqResetInSec{-1};
                long m_TokResetInSec{-1};
                int64_t m_LastConsumedInputTokens{-1};
                int64_t m_LastConsumedOutputTokens{-1};
            };
            std::vector<ControllerEntry> m_Controllers;
        };
        DebugSnapshot GetDebugSnapshot() const;

        // Per-Submit() snapshot used by hermetic size-aware-budget tests to
        // verify the timeout formula without scraping logs.  Captured at the
        // dispatcher boundary so tests see exactly the QueryData the
        // dispatcher received.  Bounded ring; the oldest entries roll off
        // once kRecentSubmissionsCapacity is exceeded.
        struct RecentSubmission
        {
            std::string m_QuotaKey;
            std::string m_Url;
            long m_TimeoutMs{0};
            int64_t m_EstimatedInputTokens{-1};
            int m_InterfaceType{-1};
            std::chrono::steady_clock::time_point m_SubmittedAt{};
        };
        // Returns up to `maxCount` most recent submissions, newest first.
        // Caller-supplied bound clamps to kRecentSubmissionsCapacity.
        std::vector<RecentSubmission> GetRecentSubmissions(size_t maxCount = 64) const;

#ifdef DEBUG
        // Hermetic-test isolation: clears controllers + host rate-limit state
        // + recent-submissions ring.  Lets repeated test runs start from a
        // clean slate without restarting j9t.  Does NOT touch m_Active /
        // m_Inbox / m_RetryQueue — those carry live in-flight work.  Debug
        // builds only — Release strips the symbol entirely.
        void ResetTestState();
#endif

    private:
        // Closed set of failure modes for SetupEasyHandle.  Replaces a fragile
        // string-prefix match on the error message that used to map to
        // QueryErrorCode at the call site.  Adding a variant triggers -Wswitch
        // at every consumer.
        enum class SetupError
        {
            None,
            CurlInit,    // curl_easy_init() returned NULL
            AuthSigner,  // IAuthSigner::Apply rejected the request
        };

        struct PendingRequest
        {
            CurlWrapper::QueryData m_QueryData;
            Callback m_Callback;
            int m_RetryCount{0};  // Preserved across retry → inbox round-trips so retries re-enter the throttle gate.
        };

        // All data kept alive for the duration of one in-flight easy handle.
        // Accessed only from the I/O thread after being inserted into m_Active.
        struct ActiveRequest
        {
            CurlWrapper::QueryData m_QueryData;
            Callback m_Callback;
            std::string m_ReadBuffer;   // response body accumulates here
            std::string m_HeaderBuffer; // response headers accumulate here
            std::string m_Url;          // stable storage — CURLOPT_URL pointer target
            std::string m_PostData;     // stable storage — CURLOPT_POSTFIELDS pointer target
            struct curl_slist* m_Headers{nullptr};
            int m_RetryCount{0};        // number of 429 retries already attempted
            int m_InterfaceType{-1};    // forwarded from QueryData; selects rate-limit strategy
        };

        // A request waiting for its retry delay to expire (I/O thread only).
        struct RetryEntry
        {
            std::chrono::steady_clock::time_point m_ReadyAt;
            PendingRequest m_Request;
            int m_RetryCount;
        };

        // Per-host adaptive rate limit state (I/O thread only).
        // Requests and tokens have independent quotas + reset windows on Anthropic.
        // Tracking them separately lets the throttle gate hold on the exhausted
        // dimension while still using the right reset time.
        struct HostRateLimitState
        {
            int m_RemainingRequests{-1};
            int m_RemainingTokens{-1};
            std::chrono::steady_clock::time_point m_RequestsResetAt;
            std::chrono::steady_clock::time_point m_TokensResetAt;
            std::chrono::steady_clock::time_point m_LastUpdated;
            std::chrono::steady_clock::time_point m_LastThrottleLog;
        };

        void IoThreadFunc();
        void DrainInbox();
        void DrainCompleted();
        void DrainRetryQueue();
        // Process pending cancellation requests pushed via CancelByCancelKey.
        // I/O thread only — mutates curl handles.
        void DrainPendingCancellations();
        // Returns the configured easy handle, or nullptr on failure.
        // On nullptr: errorKind names the closed-set failure mode and errorMessage
        // carries the human-readable reason; caller maps both into a QueryResult::Fail.
        [[nodiscard]] CURL* SetupEasyHandle(ActiveRequest& req,
                                            SetupError& errorKind,
                                            std::string& errorMessage);

        // Find-or-create a RateLimitController for the given quotaKey.  Initial
        // cap is read from the per-interface strategy (or 4 if interfaceType is
        // unknown); hard cap from queryData.m_MaxConcurrency or kMaxActivePerHost.
        // Caller must hold m_DebugMutex.  Centralises the construction logic so
        // DrainInbox (admission gate) and ParseRateLimitHeaders (controller
        // observation) can't drift apart on probe / hardCap derivation.
        std::unordered_map<std::string, RateLimitController>::iterator
        EnsureController(std::string const& quotaKey,
                         CurlWrapper::QueryData const& queryData);

        // Parse rate limit headers from the accumulated header buffer, merge
        // them into the legacy HostRateLimitState (for /api/debug/signals)
        // AND feed the per-(host, modelFamily) controller's Observe().
        // httpCode drives the controller's AIMD signal: 429 halves the cap,
        // any other clean completion advances the streak counter.
        void ParseRateLimitHeaders(ActiveRequest const& req, std::string& host, long httpCode);
        // Extract host from URL (e.g. "api.openai.com" from "https://api.openai.com/v1/...").
        static std::string ExtractHost(std::string const& url);

        // Fallback constants used only when QueryData doesn't pre-resolve a
        // per-interface override (rare — only legacy callers like assistant /
        // jcwfService Test-connection that bypass AiRequestPool::Submit).
        // The values come from config.api_interfaces[i].rate_limit.
        static constexpr int kDefaultMaxRetries429 = 10;
        static constexpr int kDefaultMaxRetriesTransient = 2;
        static constexpr int kDefaultBaseRetryMs = 1000;
        static constexpr size_t kMaxActivePerHost = 48; // hard ceiling on HTTP/2 streams per host (independent of provider quota)

        CURLM* m_MultiHandle{nullptr};
        std::thread m_IoThread;
        std::atomic<bool> m_Stopping{false};

        std::queue<PendingRequest> m_Inbox;
        mutable std::mutex m_InboxMutex;

        // Keyed by CURL* easy handle. Mutated only from the I/O thread; protected by
        // m_DebugMutex for cross-thread debug snapshot reads.
        std::unordered_map<CURL*, std::unique_ptr<ActiveRequest>> m_Active;

        // Retry queue — sorted by ready-at time. Same threading rules as m_Active.
        std::vector<RetryEntry> m_RetryQueue;

        // Per-host rate limit tracking. Same threading rules as m_Active.
        // Phase 2: kept for /api/debug/signals snapshot only — gating moved to
        // m_Controllers below.  Phase 5 will retire this entirely.
        std::unordered_map<std::string, HostRateLimitState> m_HostRateLimits;

        // Per-(host, modelFamily) adaptive controllers (Phase 2).  Key is
        // QuotaKey, "<host>|<family>", computed by AiRequestPool::Submit and
        // carried in QueryData::m_QuotaKey.  Anthropic Sonnet and Opus get
        // independent AIMD signals despite sharing api.anthropic.com.
        // Same threading rules as m_Active.
        std::unordered_map<std::string, RateLimitController> m_Controllers;

        // Recursive so the same I/O-thread call chain can lock at multiple nested levels
        // (e.g. DrainCompleted holds the lock and calls ParseRateLimitHeaders, which also
        // wants it). API-thread snapshot reads contend rarely.
        mutable std::recursive_mutex m_DebugMutex;

        // Lifetime counters — atomic so debug reads are wait-free.
        std::atomic<uint64_t> m_TotalDispatched{0};
        std::atomic<uint64_t> m_TotalThrottled{0};
        std::atomic<uint64_t> m_Total429s{0};
        std::atomic<uint64_t> m_TotalRetriesExhausted{0};
        std::atomic<uint64_t> m_TotalCompleted{0};
        std::atomic<uint64_t> m_TotalCancelled{0};

        // Cascade-cancellation queue — populated by CancelByCancelKey on any
        // thread, drained by the I/O thread in DrainPendingCancellations
        // (where curl handle mutations are safe).
        std::vector<std::string> m_PendingCancellations;
        mutable std::mutex m_PendingCancellationsMutex;

        // Bounded ring of recent submissions for hermetic size-aware-budget
        // tests.  Captured at Submit() boundary so tests see the
        // QueryData::m_TimeoutMs the dispatcher actually received.
        static constexpr size_t kRecentSubmissionsCapacity = 64;
        std::deque<RecentSubmission> m_RecentSubmissions;
        mutable std::mutex m_RecentSubmissionsMutex;
    };
} // namespace AIAssistant
