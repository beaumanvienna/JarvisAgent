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
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "curlWrapper.h"
#include "curlWrapper/interfaceTransport.h"
#include "rateLimitController.h"

namespace AIAssistant
{
    // CurlMultiDispatcher — orchestrator that sits ABOVE an IInterfaceTransport.
    // Owns the inbox, AIMD admission gate, per-(host, modelFamily) controllers,
    // retry queue, cascade-cancellation queue, debug counters, and the I/O
    // thread.  The actual HTTP machinery (curl easy/multi handles, auth signer
    // integration) lives in LiveTransport — selected by default — or in a
    // fixture-driven MockTransport (Sitting 2).
    //
    // Thread safety: Submit() is callable from any thread.
    // Callbacks fire on the I/O thread — keep them short (file writes are fine).
    class CurlMultiDispatcher
    {
    public:
        // callback(result, responseBody, retryAfterSeconds) fires on the I/O thread when the
        // request completes.  retryAfterSeconds is populated from the provider's Retry-After
        // response header when present (any HTTP status — 429, 503, or even a 200 with a
        // forward-looking hint); std::nullopt otherwise.  Cancellation / shutdown / pre-flight
        // failure paths fire with nullopt — there's no provider response to parse.
        using Callback = std::function<void(QueryResult, std::string /*responseBody*/,
                                            std::optional<int> /*retryAfterSeconds*/)>;

        // Sitting-8 Workstream D close-out: fired on the I/O thread after a
        // RateLimitController observation that mutated m_CurrentConcurrencyCap.
        // No payload — receivers refetch /api/providers/health for the
        // authoritative state.  Wired by JarvisAgent to push an
        // AiCapChangedEvent that the WS layer broadcasts as {"type":"cap-changed"}.
        using OnCapChangedCallback = std::function<void()>;
        void SetOnCapChangedCallback(OnCapChangedCallback cb);

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

        // Lightweight cap probe used by AiRequestPool's curl-completion callback
        // (cap-pinned-at-floor tracking).  Locks only m_DebugMutex (recursive),
        // never m_InboxMutex — that distinction matters because the callback
        // runs inside OnTransportComplete which already holds m_DebugMutex.
        // Routing through GetDebugSnapshot would acquire m_InboxMutex while
        // holding m_DebugMutex, opening an AB-BA deadlock against any other
        // GetDebugSnapshot caller (web thread on /api/debug/signals).
        // Returns the controller's current cap for the given quotaKey, or -1
        // if no controller exists yet for that key.
        int GetCurrentConcurrencyCap(std::string const& quotaKey) const;

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
        struct PendingRequest
        {
            CurlWrapper::QueryData m_QueryData;
            Callback m_Callback;
            int m_RetryCount{0};  // Preserved across retry → inbox round-trips so retries re-enter the throttle gate.
        };

        // Dispatcher-side per-request state.  Lives in m_Active from Submit
        // (to the transport) until the transport delivers a final completion
        // OR the request enters the retry queue.  Holds the QueryData copy
        // needed for a possible re-Submit on retry.
        struct PendingDispatch
        {
            IInterfaceTransport::RequestId m_RequestId{0};
            CurlWrapper::QueryData m_QueryData;
            Callback m_Callback;
            int m_RetryCount{0};
            int m_InterfaceType{-1};  // forwarded from QueryData; selects rate-limit strategy
            std::string m_Url;        // cached for AIMD logging / host extraction without a QueryData lookup
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
        void DrainRetryQueue();
        // Process pending cancellation requests pushed via CancelByCancelKey.
        // I/O thread only — delegates active-set aborts to the transport.
        void DrainPendingCancellations();

        // Completion handler invoked by the transport when a request finishes
        // (success, transport error, HTTP error, cancellation, or pre-flight
        // failure).  Runs on the I/O thread inside m_Transport->Pump().
        void OnTransportComplete(IInterfaceTransport::RequestId id,
                                 IInterfaceTransport::Response response);

        // Find-or-create a RateLimitController for the given quotaKey.  Initial
        // cap is read from the per-interface strategy (or 4 if interfaceType is
        // unknown); hard cap from queryData.m_MaxConcurrency or kMaxActivePerHost.
        // Caller must hold m_DebugMutex.  Centralises the construction logic so
        // DrainInbox (admission gate) and ParseRateLimitHeaders (controller
        // observation) can't drift apart on probe / hardCap derivation.
        std::unordered_map<std::string, RateLimitController>::iterator
        EnsureController(std::string const& quotaKey,
                         CurlWrapper::QueryData const& queryData);

        // Parse rate limit headers from the captured header buffer, merge them
        // into the legacy HostRateLimitState (for /api/debug/signals) AND feed
        // the per-(host, modelFamily) controller's Observe().  httpCode drives
        // the controller's AIMD signal: 429 halves the cap, any other clean
        // completion advances the streak counter.
        //
        // Returns the Retry-After value (in seconds) when the provider sent
        // one; std::nullopt otherwise.  OnTransportComplete threads this into
        // the user callback so AiRequestPool can stamp AiError::m_RetryAfterSeconds
        // for the WS payload + dashboard popover countdown.
        std::optional<int> ParseRateLimitHeaders(CurlWrapper::QueryData const& queryData,
                                                 std::string const& headerBuffer,
                                                 std::string const& body,
                                                 std::string& host,
                                                 long httpCode);

        // Fallback constants used only when QueryData doesn't pre-resolve a
        // per-interface override (rare — only legacy callers like assistant /
        // jcwfService Test-connection that bypass AiRequestPool::Submit).
        // The values come from config.api_interfaces[i].rate_limit.
        static constexpr int kDefaultMaxRetries429 = 10;
        static constexpr int kDefaultMaxRetriesTransient = 2;
        static constexpr int kDefaultBaseRetryMs = 1000;
        static constexpr size_t kMaxActivePerHost = 48; // hard ceiling on HTTP/2 streams per host (independent of provider quota)

        // Two transports: LiveTransport is the production HTTP/2 path; MockTransport
        // replays config-supplied fixtures.  Per-request selection in DrainInbox
        // routes QueryData::m_IsMock=true requests to the mock, everything else
        // to live.  Both share the dispatcher's RequestId counter so completions
        // can fire through the same OnTransportComplete sink — the dispatcher
        // doesn't need to remember which transport carried which id.
        std::unique_ptr<IInterfaceTransport> m_LiveTransport;
        std::unique_ptr<IInterfaceTransport> m_MockTransport;
        std::thread m_IoThread;
        std::atomic<bool> m_Stopping{false};

        // Monotonic request-id allocator (I/O thread only).  Carried through
        // m_Transport->Submit so completions correlate back to PendingDispatch.
        IInterfaceTransport::RequestId m_NextRequestId{0};

        std::queue<PendingRequest> m_Inbox;
        mutable std::mutex m_InboxMutex;

        // Per-request dispatch state, keyed by RequestId.  Mutated only from
        // the I/O thread; protected by m_DebugMutex for cross-thread debug
        // snapshot reads.
        std::unordered_map<IInterfaceTransport::RequestId, std::unique_ptr<PendingDispatch>> m_Active;

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

        // Sitting-8 Workstream D close-out: cap-changed wake signal.  Fired
        // from the I/O thread under m_DebugMutex after a controller observation
        // that changes m_CurrentConcurrencyCap.  Nullable; callback set once
        // by JarvisAgent.  Atomic store via the SetOnCapChangedCallback setter
        // (single-writer at construct time, single-reader on the I/O thread).
        OnCapChangedCallback m_OnCapChanged;

        // Recursive so the same I/O-thread call chain can lock at multiple nested levels
        // (e.g. OnTransportComplete holds the lock and calls ParseRateLimitHeaders, which
        // also wants it). API-thread snapshot reads contend rarely.
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
        // (where transport mutations are safe).
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
