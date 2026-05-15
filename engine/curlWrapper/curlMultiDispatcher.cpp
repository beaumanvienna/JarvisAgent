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

#include "curlWrapper/curlMultiDispatcher.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <exception>
#include <unordered_set>

#include "core.h"
#include "curlWrapper/liveTransport.h"
#include "curlWrapper/mockTransport.h"
#include "curlWrapper/rateLimitStrategy.h"
#include "engine.h"

namespace AIAssistant
{
    // ---------------------------------------------------------------------------
    // Construction / destruction
    // ---------------------------------------------------------------------------

    CurlMultiDispatcher::CurlMultiDispatcher()
        : m_LiveTransport(std::make_unique<LiveTransport>())
        , m_MockTransport(std::make_unique<MockTransport>())
    {
        m_IoThread = std::thread(&CurlMultiDispatcher::IoThreadFunc, this);
        LOG_CORE_INFO("CurlMultiDispatcher: I/O thread started");
    }

    CurlMultiDispatcher::~CurlMultiDispatcher()
    {
        SignalStop();
        WaitStop();
        // Transport unique_ptrs destruct here — LiveTransport's dtor frees the
        // curl multi handle; MockTransport's dtor is trivial.  I/O thread has
        // already been joined above so there are no live curl calls during
        // cleanup.
    }

    // ---------------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::Submit(CurlWrapper::QueryData const& data, Callback callback)
    {
        // Atomic stopping-check + inbox push: prevents a Submit landing AFTER
        // the I/O thread's final shutdown drain, which would orphan the
        // callback (the caller would block forever).  m_Stopping is monotonic
        // (false → true once), so a check under m_InboxMutex serialises with
        // the I/O thread's drain-on-shutdown which also takes m_InboxMutex.
        bool stopping = false;
        {
            std::lock_guard<std::mutex> lock(m_InboxMutex);
            stopping = m_Stopping.load();
            if (!stopping)
            {
                PendingRequest pending;
                pending.m_QueryData = data;
                pending.m_Callback = std::move(callback);
                m_Inbox.push(std::move(pending));
            }
        }
        if (stopping)
        {
            // Fire callback after lock release — callbacks may re-enter the
            // dispatcher (e.g. file-write side effects).  `callback` was not
            // moved-from in the stopping branch above, so it's still valid.
            callback(QueryResult::Fail(static_cast<int>(CURLE_ABORTED_BY_CALLBACK),
                                       "request rejected (dispatcher stopping)"),
                     {});
            return;
        }
        // Capture submission for hermetic size-aware-budget tests.
        // Separate mutex to avoid contending with the inbox lock.
        {
            std::lock_guard<std::mutex> lock(m_RecentSubmissionsMutex);
            RecentSubmission entry;
            entry.m_QuotaKey = data.m_QuotaKey;
            entry.m_Url = data.m_Url;
            entry.m_TimeoutMs = data.m_TimeoutMs;
            entry.m_EstimatedInputTokens = data.m_EstimatedInputTokens;
            entry.m_InterfaceType = data.m_InterfaceType;
            entry.m_SubmittedAt = std::chrono::steady_clock::now();
            m_RecentSubmissions.push_back(std::move(entry));
            while (m_RecentSubmissions.size() > kRecentSubmissionsCapacity)
            {
                m_RecentSubmissions.pop_front();
            }
        }
        if (m_LiveTransport)
        {
            m_LiveTransport->Wakeup();
        }
        if (m_MockTransport)
        {
            m_MockTransport->Wakeup();
        }
    }

    std::vector<CurlMultiDispatcher::RecentSubmission>
    CurlMultiDispatcher::GetRecentSubmissions(size_t maxCount) const
    {
        std::lock_guard<std::mutex> lock(m_RecentSubmissionsMutex);
        size_t const wanted = std::min(maxCount, m_RecentSubmissions.size());
        std::vector<RecentSubmission> out;
        out.reserve(wanted);
        // Newest first.  m_RecentSubmissions is push_back-ordered (oldest at
        // front), so iterate the tail backwards.
        auto it = m_RecentSubmissions.rbegin();
        for (size_t i = 0; i < wanted; ++i, ++it)
        {
            out.push_back(*it);
        }
        return out;
    }

#ifdef DEBUG
    void CurlMultiDispatcher::ResetTestState()
    {
        // Hermetic-test isolation.  m_Controllers + m_HostRateLimits hold
        // adaptive state (AIMD cap, last observation) accumulated across
        // submissions; without resetting them, repeated Phase B test runs
        // see residue from prior runs.  m_Active / m_Inbox / m_RetryQueue
        // carry live work — those are NOT cleared here.
        {
            std::lock_guard<std::recursive_mutex> debugLock(m_DebugMutex);
            m_Controllers.clear();
            m_HostRateLimits.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_RecentSubmissionsMutex);
            m_RecentSubmissions.clear();
        }
    }
#endif

    void CurlMultiDispatcher::CancelByCancelKey(std::string const& cancelKey)
    {
        if (cancelKey.empty())
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_PendingCancellationsMutex);
            m_PendingCancellations.push_back(cancelKey);
        }
        // Wake the I/O thread so DrainPendingCancellations runs even if no
        // socket activity is in flight.  Transport handle mutations must be
        // on the I/O thread (single-thread relative to curl_multi_perform).
        if (m_LiveTransport)
        {
            m_LiveTransport->Wakeup();
        }
        if (m_MockTransport)
        {
            m_MockTransport->Wakeup();
        }
    }

    void CurlMultiDispatcher::SignalStop()
    {
        m_Stopping.store(true);
        if (m_LiveTransport)
        {
            m_LiveTransport->Wakeup();
        }
        if (m_MockTransport)
        {
            m_MockTransport->Wakeup();
        }
    }

    void CurlMultiDispatcher::WaitStop()
    {
        if (m_IoThread.joinable())
        {
            m_IoThread.join();
        }
    }

    CurlMultiDispatcher::DebugSnapshot CurlMultiDispatcher::GetDebugSnapshot() const
    {
        DebugSnapshot snap;
        snap.m_TotalDispatched        = m_TotalDispatched.load();
        snap.m_TotalThrottled         = m_TotalThrottled.load();
        snap.m_Total429s              = m_Total429s.load();
        snap.m_TotalRetriesExhausted  = m_TotalRetriesExhausted.load();
        snap.m_TotalCompleted         = m_TotalCompleted.load();
        snap.m_TotalCancelled         = m_TotalCancelled.load();
        {
            std::lock_guard<std::mutex> lock(m_InboxMutex);
            snap.m_InboxSize = m_Inbox.size();
        }
        {
            std::lock_guard<std::recursive_mutex> lock(m_DebugMutex);
            snap.m_ActiveCount     = m_Active.size();
            snap.m_RetryQueueSize  = m_RetryQueue.size();

            std::unordered_map<std::string, size_t> activePerHost;
            for (auto const& [id, pd] : m_Active)
            {
                std::string host = ExtractHostFromUrl(pd->m_Url);
                if (!host.empty())
                    ++activePerHost[host];
            }

            auto now = std::chrono::steady_clock::now();
            auto secsUntil = [&now](std::chrono::steady_clock::time_point t) -> long
            {
                if (t == std::chrono::steady_clock::time_point{}) return -1;
                return std::chrono::duration_cast<std::chrono::seconds>(t - now).count();
            };
            for (auto const& [host, state] : m_HostRateLimits)
            {
                DebugSnapshot::HostEntry e;
                e.m_Host               = host;
                e.m_RemainingRequests  = state.m_RemainingRequests;
                e.m_RemainingTokens    = state.m_RemainingTokens;
                e.m_ReqResetInSec      = secsUntil(state.m_RequestsResetAt);
                e.m_TokResetInSec      = secsUntil(state.m_TokensResetAt);
                auto it = activePerHost.find(host);
                e.m_ActiveCount        = (it != activePerHost.end()) ? it->second : 0;
                snap.m_Hosts.push_back(std::move(e));
            }

            for (auto const& [quotaKey, controller] : m_Controllers)
            {
                DebugSnapshot::ControllerEntry e;
                e.m_QuotaKey               = quotaKey;
                e.m_CurrentConcurrencyCap  = controller.CurrentConcurrencyCap();
                e.m_StreakSinceLast429     = controller.StreakSinceLast429();
                auto const& obs            = controller.LastObservation();
                e.m_RemainingRequests      = obs.m_RemainingRequests;
                int64_t mergedTok = -1;
                auto const consider = [&](int64_t v) {
                    if (v < 0) return;
                    if (mergedTok < 0 || v < mergedTok) mergedTok = v;
                };
                consider(obs.m_RemainingCombinedTokens);
                consider(obs.m_RemainingInputTokens);
                consider(obs.m_RemainingOutputTokens);
                e.m_RemainingTokens        = mergedTok;
                e.m_ReqResetInSec          = obs.m_RequestsResetAt.has_value() ? secsUntil(*obs.m_RequestsResetAt) : -1;
                e.m_TokResetInSec          = obs.m_TokensResetAt.has_value() ? secsUntil(*obs.m_TokensResetAt) : -1;
                e.m_LastConsumedInputTokens  = obs.m_ConsumedInputTokens;
                e.m_LastConsumedOutputTokens = obs.m_ConsumedOutputTokens;
                snap.m_Controllers.push_back(std::move(e));
            }
        }
        return snap;
    }

    // ---------------------------------------------------------------------------
    // Private helpers (I/O thread only)
    // ---------------------------------------------------------------------------

    std::unordered_map<std::string, RateLimitController>::iterator
    CurlMultiDispatcher::EnsureController(std::string const& quotaKey,
                                          CurlWrapper::QueryData const& queryData)
    {
        auto it = m_Controllers.find(quotaKey);
        if (it != m_Controllers.end())
        {
            return it;
        }

        // Initial probe = strategy.InitialConcurrencyProbe() when the interface
        // is known; otherwise a generic 4 (matches the pre-helper inline default
        // in DrainInbox for QueryData with InvalidAPI / -1 interfaceType).
        int initialProbe = 4;
        if (queryData.m_InterfaceType >= 0 &&
            queryData.m_InterfaceType < ConfigParser::EngineConfig::InterfaceType::NumAPIs)
        {
            auto const interfaceType =
                static_cast<ConfigParser::EngineConfig::InterfaceType>(queryData.m_InterfaceType);
            initialProbe = IRateLimitStrategy::Get(interfaceType).InitialConcurrencyProbe();
        }
        int const hardCap = (queryData.m_MaxConcurrency > 0) ? queryData.m_MaxConcurrency
                                                             : static_cast<int>(kMaxActivePerHost);
        auto inserted = m_Controllers.emplace(quotaKey, RateLimitController(initialProbe, hardCap));
        return inserted.first;
    }

    void CurlMultiDispatcher::DrainInbox()
    {
        std::queue<PendingRequest> local;
        {
            std::lock_guard<std::mutex> lock(m_InboxMutex);
            local.swap(m_Inbox);
        }

        // Hold m_DebugMutex through the whole drain so debug snapshot reads see a
        // consistent view of m_Active + controllers.
        std::lock_guard<std::recursive_mutex> debugLock(m_DebugMutex);

        // Count active requests per QuotaKey + per host.  QuotaKey-keyed counts
        // feed the controller; host-keyed counts feed the existing
        // /api/debug/signals snapshot until Phase 5 retires it.
        std::unordered_map<std::string, size_t> activePerKey;
        std::unordered_map<std::string, size_t> activePerHost;
        for (auto const& [id, pd] : m_Active)
        {
            std::string const host = ExtractHostFromUrl(pd->m_Url);
            if (!host.empty())
                ++activePerHost[host];
            std::string const key = pd->m_QueryData.m_QuotaKey.empty()
                                        ? host
                                        : pd->m_QueryData.m_QuotaKey;
            if (!key.empty())
                ++activePerKey[key];
        }

        // Selective throttle re-queue: when the controller for one QuotaKey
        // refuses admission, we used to dump the entire `local` queue back to
        // the inbox.  That blocked items targeting OTHER QuotaKeys (e.g. an
        // Anthropic Opus throttle blocked queued OpenAI requests).  Now we
        // remember the throttled keys and only defer items that hit them;
        // everything else continues through the dispatch path.
        std::unordered_set<std::string> throttledKeys;

        while (!local.empty())
        {
            auto& pending = local.front();
            std::string const host = ExtractHostFromUrl(pending.m_QueryData.m_Url);
            std::string const quotaKey = pending.m_QueryData.m_QuotaKey.empty()
                                             ? host
                                             : pending.m_QueryData.m_QuotaKey;

            // Fast-path: this iteration already throttled this key — defer
            // without re-running ShouldAdmit (the answer can't have changed
            // since the last in-iteration check; in-flight count for this key
            // hasn't moved because we haven't dispatched any same-key items).
            if (!quotaKey.empty() && throttledKeys.count(quotaKey) > 0)
            {
                std::lock_guard<std::mutex> lock(m_InboxMutex);
                m_Inbox.push(std::move(pending));
                local.pop();
                continue;
            }

            char const* throttleReason = nullptr;
            std::chrono::steady_clock::time_point nextAttemptAt{};
            int64_t const estTokens = pending.m_QueryData.m_EstimatedInputTokens >= 0
                                          ? pending.m_QueryData.m_EstimatedInputTokens
                                          : 0;
            size_t const inflightForKey = activePerKey[quotaKey];

            if (!quotaKey.empty())
            {
                auto controllerIt = EnsureController(quotaKey, pending.m_QueryData);
                RateLimitController::Decision const decision =
                    controllerIt->second.ShouldAdmit(static_cast<int>(inflightForKey), estTokens);
                if (!decision.m_Admit)
                {
                    throttleReason = decision.m_Reason;
                    nextAttemptAt = decision.m_NextAttemptAt;
                }
            }

            if (throttleReason != nullptr)
            {
                ++m_TotalThrottled;
                throttledKeys.insert(quotaKey);
                auto& state = m_HostRateLimits[host];
                auto const now = std::chrono::steady_clock::now();
                // Per-host rate limit on the throttle log (once per 5s).  Multiple
                // QuotaKeys may share a host (e.g. Anthropic Sonnet + Opus on
                // api.anthropic.com); the host-keyed rate-limiter catches them
                // all so we don't spam the log.
                if (now - state.m_LastThrottleLog >= std::chrono::seconds(5))
                {
                    auto const secsUntil = [&now](std::chrono::steady_clock::time_point t) -> long {
                        if (t == std::chrono::steady_clock::time_point{})
                            return -1;
                        return std::chrono::duration_cast<std::chrono::seconds>(t - now).count();
                    };
                    auto const cit = m_Controllers.find(quotaKey);
                    int const cap = (cit != m_Controllers.end()) ? cit->second.CurrentConcurrencyCap() : -1;
                    LOG_CORE_INFO("CurlMultiDispatcher: throttling key='{}' reason='{}' inflight={} cap={} "
                                  "nextAttemptIn={}s",
                                  quotaKey, throttleReason, inflightForKey, cap, secsUntil(nextAttemptAt));
                    state.m_LastThrottleLog = now;
                }

                std::lock_guard<std::mutex> lock(m_InboxMutex);
                m_Inbox.push(std::move(pending));
                local.pop();
                continue;
            }

            // Admit: allocate a RequestId, install the PendingDispatch entry,
            // and forward to the transport.  The transport will eventually
            // call OnTransportComplete via the completion callback we pass —
            // either with the HTTP response, a transport error, or a
            // pre-flight failure surfaced through the deferred-completion path.
            IInterfaceTransport::RequestId const id = ++m_NextRequestId;
            auto pd = std::make_unique<PendingDispatch>();
            pd->m_RequestId    = id;
            pd->m_Url          = pending.m_QueryData.m_Url;
            pd->m_RetryCount   = pending.m_RetryCount;
            pd->m_InterfaceType = pending.m_QueryData.m_InterfaceType;
            pd->m_Callback     = std::move(pending.m_Callback);
            pd->m_QueryData    = pending.m_QueryData;  // copy for retry path
            m_Active[id] = std::move(pd);

            if (!host.empty())
                ++activePerHost[host];
            if (!quotaKey.empty())
                ++activePerKey[quotaKey];
            ++m_TotalDispatched;

            // Per-request transport selection: m_IsMock flagged requests go
            // through MockTransport (fixture replay), everything else through
            // LiveTransport (real HTTPS).  Both transports share the
            // dispatcher's RequestId space + OnTransportComplete sink.
            //
            // Lifetime: `this` is safe because both transports are owned by
            // this dispatcher (unique_ptr); the I/O thread is joined in
            // WaitStop before the transports destruct.  Per
            // feedback_capture_by_value_async.
            IInterfaceTransport& transport = (pending.m_QueryData.m_IsMock && m_MockTransport)
                                                 ? *m_MockTransport
                                                 : *m_LiveTransport;
            transport.Submit(id, std::move(pending.m_QueryData),
                             [this](IInterfaceTransport::RequestId reqId,
                                    IInterfaceTransport::Response resp)
                             {
                                 OnTransportComplete(reqId, std::move(resp));
                             });

            local.pop();
        }
    }

    // ---------------------------------------------------------------------------
    // Rate limit helpers (I/O thread only)
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::ParseRateLimitHeaders(CurlWrapper::QueryData const& queryData,
                                                    std::string const& headerBuffer,
                                                    std::string const& body,
                                                    std::string& host,
                                                    long httpCode)
    {
        // Per-provider strategy delegation (Phase 1) + controller Observe (Phase 2).
        // The strategy returns a normalized RateLimitObservation; we merge it into
        // the legacy HostRateLimitState (kept for /api/debug/signals until Phase 5)
        // AND into the per-(host, modelFamily) controller for AIMD/token-bucket
        // gating.
        ConfigParser::EngineConfig::InterfaceType const interfaceType =
            (queryData.m_InterfaceType >= 0 &&
             queryData.m_InterfaceType < ConfigParser::EngineConfig::InterfaceType::NumAPIs)
                ? static_cast<ConfigParser::EngineConfig::InterfaceType>(queryData.m_InterfaceType)
                : ConfigParser::EngineConfig::InterfaceType::InvalidAPI;

        IRateLimitStrategy const& strategy = IRateLimitStrategy::Get(interfaceType);
        RateLimitObservation const observation =
            strategy.Parse(headerBuffer, body, static_cast<int>(httpCode));

        std::lock_guard<std::recursive_mutex> debugLock(m_DebugMutex);

        // ---- Controller Observe (Phase 2) ----
        // Find or create the controller for this request's QuotaKey.  Note: we
        // create on first observation (not just first dispatch) so the controller
        // exists for snapshot reads even before any ShouldAdmit() runs through
        // it for that key.  hardCap = kMaxActivePerHost; Phase 4 will swap in
        // config.rate_limit.max_concurrency.
        std::string const quotaKey = queryData.m_QuotaKey.empty() ? host : queryData.m_QuotaKey;
        if (!quotaKey.empty())
        {
            auto controllerIt = EnsureController(quotaKey, queryData);
            bool const was429 = (httpCode == 429);
            controllerIt->second.Observe(observation, was429);
        }
        auto& state = m_HostRateLimits[host];
        state.m_LastUpdated = std::chrono::steady_clock::now();

        if (observation.m_RemainingRequests >= 0)
        {
            state.m_RemainingRequests = static_cast<int>(observation.m_RemainingRequests);
        }

        // Token quota: providers report variously combined / split (input+output).
        // Track the TIGHTEST remaining so the throttle gate trips on the first
        // exhausted bucket — matches existing behavior where Anthropic's
        // input/output remaining headers each updated state.m_RemainingTokens
        // only when smaller than the prior value.
        int64_t mergedTokensRemaining = -1;
        auto const consider = [&](int64_t value) {
            if (value < 0)
                return;
            if (mergedTokensRemaining < 0 || value < mergedTokensRemaining)
                mergedTokensRemaining = value;
        };
        consider(observation.m_RemainingCombinedTokens);
        consider(observation.m_RemainingInputTokens);
        consider(observation.m_RemainingOutputTokens);
        if (mergedTokensRemaining >= 0)
        {
            state.m_RemainingTokens = static_cast<int>(mergedTokensRemaining);
        }

        if (observation.m_RequestsResetAt.has_value())
        {
            state.m_RequestsResetAt = *observation.m_RequestsResetAt;
        }
        if (observation.m_TokensResetAt.has_value())
        {
            // For token resets the strategy already takes the LATEST internally
            // (slowest-refilling bucket dominates).  Adopt directly.
            state.m_TokensResetAt = *observation.m_TokensResetAt;
        }

        if (observation.m_RetryAfter.has_value())
        {
            // retry-after is a floor on the next admission for both buckets —
            // never re-dispatch sooner than the server explicitly told us to.
            auto const candidate = std::chrono::steady_clock::now() + *observation.m_RetryAfter;
            if (candidate > state.m_RequestsResetAt)
                state.m_RequestsResetAt = candidate;
            if (candidate > state.m_TokensResetAt)
                state.m_TokensResetAt = candidate;
        }
    }

    // ---------------------------------------------------------------------------
    // Retry queue
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::DrainRetryQueue()
    {
        if (m_RetryQueue.empty())
            return;

        auto now = std::chrono::steady_clock::now();
        // Move ready entries back to the inbox so they re-enter DrainInbox's per-host
        // throttle gate. Otherwise retries bypass the gate and re-trigger 429 storms.
        for (int i = static_cast<int>(m_RetryQueue.size()) - 1; i >= 0; --i)
        {
            if (m_RetryQueue[static_cast<size_t>(i)].m_ReadyAt <= now)
            {
                RetryEntry entry;
                {
                    std::lock_guard<std::recursive_mutex> lock(m_DebugMutex);
                    entry = std::move(m_RetryQueue[static_cast<size_t>(i)]);
                    m_RetryQueue.erase(m_RetryQueue.begin() + i);
                }

                PendingRequest pending = std::move(entry.m_Request);
                pending.m_RetryCount = entry.m_RetryCount;

                std::lock_guard<std::mutex> lock(m_InboxMutex);
                m_Inbox.push(std::move(pending));
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Cascade cancellation (I/O thread only)
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::DrainPendingCancellations()
    {
        std::vector<std::string> local;
        {
            std::lock_guard<std::mutex> lock(m_PendingCancellationsMutex);
            if (m_PendingCancellations.empty())
            {
                return;
            }
            local.swap(m_PendingCancellations);
        }

        std::lock_guard<std::recursive_mutex> debugLock(m_DebugMutex);

        for (std::string const& cancelKey : local)
        {
            if (cancelKey.empty())
            {
                continue;
            }

            // 1. Inbox — drop matching, fire callback.
            {
                std::lock_guard<std::mutex> inboxLock(m_InboxMutex);
                std::queue<PendingRequest> kept;
                while (!m_Inbox.empty())
                {
                    PendingRequest pending = std::move(m_Inbox.front());
                    m_Inbox.pop();
                    if (pending.m_QueryData.m_CancelKey == cancelKey)
                    {
                        if (pending.m_Callback)
                        {
                            pending.m_Callback(QueryResult::Fail(static_cast<int>(CURLE_ABORTED_BY_CALLBACK),
                                                                  "request cancelled (run terminated)"),
                                               {});
                        }
                        ++m_TotalCancelled;
                    }
                    else
                    {
                        kept.push(std::move(pending));
                    }
                }
                m_Inbox = std::move(kept);
            }

            // 2. Retry queue — drop matching, fire callback.
            for (auto it = m_RetryQueue.begin(); it != m_RetryQueue.end();)
            {
                if (it->m_Request.m_QueryData.m_CancelKey == cancelKey)
                {
                    if (it->m_Request.m_Callback)
                    {
                        it->m_Request.m_Callback(QueryResult::Fail(static_cast<int>(CURLE_ABORTED_BY_CALLBACK),
                                                                    "request cancelled (run terminated)"),
                                                 {});
                    }
                    it = m_RetryQueue.erase(it);
                    ++m_TotalCancelled;
                }
                else
                {
                    ++it;
                }
            }

            // 3. Active set — fire user callback synchronously, drop from
            // m_Active, then ask the transport to abort the in-flight handle
            // (silent cleanup; no transport-side completion).  Collect matches
            // first since erasing while iterating m_Active is awkward.
            std::vector<IInterfaceTransport::RequestId> toErase;
            for (auto const& [id, pd] : m_Active)
            {
                if (pd && pd->m_QueryData.m_CancelKey == cancelKey)
                {
                    toErase.push_back(id);
                }
            }
            for (IInterfaceTransport::RequestId const id : toErase)
            {
                auto it = m_Active.find(id);
                if (it == m_Active.end())
                {
                    continue;
                }
                PendingDispatch& pd = *it->second;
                Callback callback = std::move(pd.m_Callback);
                m_Active.erase(it);
                if (callback)
                {
                    callback(QueryResult::Fail(static_cast<int>(CURLE_ABORTED_BY_CALLBACK),
                                                 "request cancelled (run terminated)"),
                             {});
                }
                ++m_TotalCancelled;
            }
            // Fan out to both transports — either could hold a matching
            // in-flight or pending entry.  Each is a no-op when nothing matches.
            if (m_LiveTransport)
            {
                m_LiveTransport->CancelByCancelKey(cancelKey);
            }
            if (m_MockTransport)
            {
                m_MockTransport->CancelByCancelKey(cancelKey);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Completion handling with 429 retry
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::OnTransportComplete(IInterfaceTransport::RequestId id,
                                                  IInterfaceTransport::Response response)
    {
        static std::atomic<uint32_t> s_QueryCounter{0};

        std::lock_guard<std::recursive_mutex> debugLock(m_DebugMutex);
        auto it = m_Active.find(id);
        if (it == m_Active.end())
        {
            // Already cancelled / erased synchronously by DrainPendingCancellations.
            // Drop the completion silently.
            return;
        }

        PendingDispatch& pd = *it->second;

        uint32_t const qnum = ++s_QueryCounter;
        long const httpCode = response.m_HttpStatus;
        std::string const versionLabel = response.m_HttpVersionLabel;
        QueryResult const& transportResult = response.m_Result;

        // Always parse rate limit headers (even on success) to keep state fresh.
        std::string host = ExtractHostFromUrl(pd.m_Url);
        if (!host.empty())
        {
            ParseRateLimitHeaders(pd.m_QueryData, response.m_RawHeaders, response.m_Body, host, httpCode);
        }

        // --- Handle 429 with auto-retry ---
        // Sentinel discipline: -1 (or any negative) = "unset, use dispatcher
        // default"; 0 = "no retries, fail on first 429"; >0 = explicit retry
        // budget.  Conflating 0 with the unset case made hermetic tests
        // (e.g. test_aimd_concurrency_cap.py) silently inherit the default
        // 10 and fire 11× as many Observe(was_429=true) calls as expected.
        int const maxRetries429 = (pd.m_QueryData.m_MaxRetries429 >= 0) ? pd.m_QueryData.m_MaxRetries429
                                                                        : kDefaultMaxRetries429;
        int const baseRetryMs = (pd.m_QueryData.m_BaseRetryMs > 0) ? pd.m_QueryData.m_BaseRetryMs
                                                                   : kDefaultBaseRetryMs;
        int const maxRetriesTransient = (pd.m_QueryData.m_MaxRetriesTransient >= 0)
                                            ? pd.m_QueryData.m_MaxRetriesTransient
                                            : kDefaultMaxRetriesTransient;

        bool const transportOk = transportResult.m_Ok;

        if (transportOk && httpCode == 429 && pd.m_RetryCount < maxRetries429 && !m_Stopping.load())
        {
            int retryCount = pd.m_RetryCount + 1;

            // Determine delay: prefer Retry-After header, fall back to exponential backoff.
            int delayMs = baseRetryMs * (1 << (retryCount - 1)); // exponential: 1s, 2s, 4s, 8s, 16s

            // Check if the host state has a reset-at time that's more informative —
            // wait for the LATER of req/token reset, since either could be what 429'd us.
            auto hostIt = m_HostRateLimits.find(host);
            if (hostIt != m_HostRateLimits.end())
            {
                auto laterReset = std::max(hostIt->second.m_RequestsResetAt,
                                           hostIt->second.m_TokensResetAt);
                auto msUntilReset = std::chrono::duration_cast<std::chrono::milliseconds>(
                    laterReset - std::chrono::steady_clock::now()).count();
                if (msUntilReset > 0 && msUntilReset < 120000) // cap at 2 minutes
                {
                    delayMs = static_cast<int>(msUntilReset) + 100; // small buffer
                }
            }

            LOG_CORE_WARN("HTTP 429 rate limit for query {} (host: {}) — auto-retrying in {}ms (attempt {}/{})",
                          qnum, host, delayMs, retryCount, maxRetries429);
            ++m_Total429s;

            // Build retry entry. Carry the dispatched hook forward so retries
            // re-fire it (AiRequestPool re-disarms its watchdog on the next
            // curl_multi_add_handle).  No deadline-extension hook needed —
            // each retry creates a fresh easy handle with its own
            // CURLOPT_TIMEOUT_MS budget.
            PendingRequest pendingRetry;
            pendingRetry.m_QueryData     = std::move(pd.m_QueryData);
            pendingRetry.m_Callback      = std::move(pd.m_Callback);

            RetryEntry entry;
            entry.m_ReadyAt    = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
            entry.m_Request    = std::move(pendingRetry);
            entry.m_RetryCount = retryCount;

            m_RetryQueue.push_back(std::move(entry));
            m_Active.erase(it);
            return; // do NOT invoke callback — retry is pending
        }

        // --- Handle transient HTTP errors (400, 500, 502, 503) with limited auto-retry ---
        // Note: curl-level errors (timeout, couldnt-connect, SSL handshake failure)
        // do NOT enter this retry path — the `transportOk` guard means only
        // server-side HTTP statuses retry here.  Curl-level errors typically indicate
        // persistent network/auth/cert issues where retrying inside the dispatcher
        // would burn the per-request timeout budget without changing the outcome;
        // the higher-level WorkflowRuntimeManager owns retry policy for those.
        bool const isTransientError = (httpCode == 400 || httpCode == 500 || httpCode == 502 || httpCode == 503);
        if (transportOk && isTransientError && pd.m_RetryCount < maxRetriesTransient && !m_Stopping.load())
        {
            int retryCount = pd.m_RetryCount + 1;
            int delayMs = baseRetryMs * (1 << (retryCount - 1)); // 1s, 2s

            LOG_CORE_WARN("HTTP {} for query {} — transient error, auto-retrying in {}ms (attempt {}/{})",
                          httpCode, qnum, delayMs, retryCount, maxRetriesTransient);

            PendingRequest pendingRetry;
            pendingRetry.m_QueryData     = std::move(pd.m_QueryData);
            pendingRetry.m_Callback      = std::move(pd.m_Callback);

            RetryEntry entry;
            entry.m_ReadyAt    = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
            entry.m_Request    = std::move(pendingRetry);
            entry.m_RetryCount = retryCount;

            m_RetryQueue.push_back(std::move(entry));
            m_Active.erase(it);
            return; // do NOT invoke callback — retry is pending
        }

        LOG_CORE_INFO("query {} used {} (HTTP {}){}", qnum, versionLabel, httpCode,
                      pd.m_RetryCount > 0 ? " [retry " + std::to_string(pd.m_RetryCount) + "]" : "");

        // Log rate limit state for the host.
        if (!host.empty())
        {
            auto hostIt = m_HostRateLimits.find(host);
            if (hostIt != m_HostRateLimits.end() && hostIt->second.m_RemainingRequests >= 0)
            {
                LOG_CORE_INFO("rate limit ({}): {} requests remaining, {} tokens remaining",
                              host, hostIt->second.m_RemainingRequests, hostIt->second.m_RemainingTokens);
            }
        }

        QueryResult result;
        if (!transportOk)
        {
            int const errCode = transportResult.m_ErrorCode;
            std::string errMsg = transportResult.m_ErrorMessage;
            if (errCode == static_cast<int>(CURLE_ABORTED_BY_CALLBACK))
            {
                LOG_CORE_INFO("[shutdown] curl request aborted (query {})", qnum);
            }
            else
            {
                // cancelKey + quotaKey lets the dashboard run analyzer surface
                // this line — it filters issues to ERROR lines containing the
                // run's identifiers (per CLAUDE.md "Failure-path logs").
                LOG_CORE_ERROR("curl error (code {}): {} cancelKey='{}' quotaKey='{}'",
                               errCode, errMsg,
                               pd.m_QueryData.m_CancelKey, pd.m_QueryData.m_QuotaKey);
            }
            result = QueryResult::Fail(errCode, std::move(errMsg));
        }
        else if (httpCode >= 400)
        {
            std::string errMsg = QueryErrorCode::Describe(static_cast<int>(httpCode));
            if (httpCode == 429)
            {
                LOG_CORE_ERROR("HTTP 429 rate limit for query {} — AI provider rejected the request; "
                               "retries exhausted ({}x) cancelKey='{}' quotaKey='{}'",
                               qnum, pd.m_RetryCount,
                               pd.m_QueryData.m_CancelKey, pd.m_QueryData.m_QuotaKey);
                ++m_TotalRetriesExhausted;
            }
            else
            {
                LOG_CORE_ERROR("HTTP error {} for query {} cancelKey='{}' quotaKey='{}'",
                               httpCode, qnum,
                               pd.m_QueryData.m_CancelKey, pd.m_QueryData.m_QuotaKey);
            }
            result = QueryResult::Fail(static_cast<int>(httpCode), std::move(errMsg));
        }
        else
        {
            result = QueryResult::Ok();
        }

        // Move data out of pd before erasing (callback may re-enter Submit).
        std::string responseBody = std::move(response.m_Body);
        Callback callback        = std::move(pd.m_Callback);

        m_Active.erase(it);
        ++m_TotalCompleted;

        if (callback)
        {
            callback(result, std::move(responseBody));
        }
    }

    // ---------------------------------------------------------------------------
    // I/O thread loop
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::IoThreadFunc()
    {
        // Loop body wrapped in try/catch so a transient bad_alloc anywhere
        // inside DrainInbox / Pump / DrainPendingCancellations does NOT
        // silently terminate the I/O thread (which would freeze every
        // subsequent dispatch with no log line pointing at the cause).  The
        // policy is "log loudly + continue": the next iteration retries the
        // failed work from its source state (m_Inbox / m_Active / m_RetryQueue
        // are all unchanged on throw), so transient OOM is recoverable.  For
        // unknown exception types we log and continue too — better a noisy
        // log than a silent thread death.
        while (true)
        {
            try
            {
                bool const stopping = m_Stopping.load();

                if (stopping)
                {
                    // Drain inbox and reject any queued-but-not-yet-started requests.
                    std::queue<PendingRequest> local;
                    {
                        std::lock_guard<std::mutex> lock(m_InboxMutex);
                        local.swap(m_Inbox);
                    }
                    while (!local.empty())
                    {
                        local.front().m_Callback(
                            QueryResult::Fail(static_cast<int>(CURLE_ABORTED_BY_CALLBACK),
                                              "curl request aborted (shutdown)"),
                            {});
                        local.pop();
                    }

                    // Cancel pending retries on shutdown.
                    for (auto& entry : m_RetryQueue)
                    {
                        entry.m_Request.m_Callback(
                            QueryResult::Fail(static_cast<int>(CURLE_ABORTED_BY_CALLBACK),
                                              "curl request aborted (shutdown)"),
                            {});
                    }
                    m_RetryQueue.clear();
                }
                else
                {
                    // Cancellations first so cancelled requests don't churn through
                    // the throttle gate or get re-dispatched from the retry queue
                    // before being aborted.
                    DrainPendingCancellations();
                    DrainInbox();
                    DrainRetryQueue();
                }

                // Drive both transports: LiveTransport's Pump runs
                // curl_multi_perform + harvests completions; MockTransport's
                // Pump fires any queued fixture responses.  OnTransportComplete
                // fires inside each Pump for completed requests and
                // decrements m_Active.
                if (m_LiveTransport)
                {
                    m_LiveTransport->Pump();
                }
                if (m_MockTransport)
                {
                    m_MockTransport->Pump();
                }

                if (stopping && m_Active.empty())
                {
                    break;
                }

                // Sleep until socket activity or transport Wakeup() (from
                // Submit/SignalStop/CancelByCancelKey).  Use shorter timeout
                // when retries are pending so we wake up to process them.
                long timeout_ms = 1000L;
                if (!m_Active.empty())
                {
                    timeout_ms = 50L;
                }
                else if (!m_RetryQueue.empty())
                {
                    // Wake up when the earliest retry is ready (or at least every 100ms).
                    auto earliest = m_RetryQueue.front().m_ReadyAt;
                    for (auto const& entry : m_RetryQueue)
                    {
                        if (entry.m_ReadyAt < earliest)
                            earliest = entry.m_ReadyAt;
                    }
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        earliest - std::chrono::steady_clock::now()).count();
                    timeout_ms = std::clamp(static_cast<long>(ms), 10L, 1000L);
                }
                // Only LiveTransport blocks on socket activity.  MockTransport's
                // Wait is a no-op (no I/O to wait on); calling it would just
                // return immediately.  Skip it so the I/O thread actually
                // sleeps when there's no live activity.
                if (m_LiveTransport)
                {
                    m_LiveTransport->Wait(timeout_ms);
                }
            }
            catch (std::bad_alloc const&)
            {
                LOG_CORE_ERROR("CurlMultiDispatcher: I/O thread caught std::bad_alloc; sleeping 100ms then continuing");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            catch (std::exception const& e)
            {
                LOG_CORE_ERROR("CurlMultiDispatcher: I/O thread caught std::exception: {}; continuing", e.what());
            }
            catch (...)
            {
                LOG_CORE_ERROR("CurlMultiDispatcher: I/O thread caught unknown exception; continuing");
            }
        }

        LOG_CORE_INFO("CurlMultiDispatcher: I/O thread exiting (rate limit retries served: {})",
                      m_RetryQueue.empty() ? "all" : std::to_string(m_RetryQueue.size()) + " cancelled");
    }

} // namespace AIAssistant
