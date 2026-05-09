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
#include <curl/multi.h>

#include <cstdio>
#include <ctime>
#include <exception>
#include <unordered_set>

#include "core.h"
#include "curlWrapper/authSigner.h"
#include "curlWrapper/rateLimitStrategy.h"
#include "engine.h"

namespace AIAssistant
{
    // ---------------------------------------------------------------------------
    // Static callbacks (no class state needed)
    // ---------------------------------------------------------------------------

    // Hard caps on per-request response body and header accumulation.  A buggy or
    // hostile upstream could otherwise stream gigabytes into m_ReadBuffer /
    // m_HeaderBuffer and OOM the engine.  On overflow the callback returns a
    // short-write to libcurl, which translates to CURLE_WRITE_ERROR and fails
    // the request through the existing curl-error path.
    //   Body cap   = 32 MiB.  AI responses are typically < 1 MiB; 32 MiB is
    //                generous for very long structured-output completions.
    //   Header cap =  1 MiB.  HTTP/2 header sizes are typically <16 KiB.
    static constexpr size_t kMaxResponseBodyBytes = 32ULL * 1024 * 1024;
    static constexpr size_t kMaxHeaderBytes        = 1ULL  * 1024 * 1024;

    static size_t MultiWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        size_t const totalSize = size * nmemb;
        // libcurl is C; an exception escaping this callback is UB.  Catch
        // bad_alloc / length_error from std::string::append and signal a
        // short-write so libcurl aborts the transfer cleanly.
        try
        {
            auto* buf = static_cast<std::string*>(userp);
            // Overflow-safe form: buf->size() never exceeds kMax (we enforce it
            // here), so kMax - buf->size() can't underflow.
            if (totalSize > kMaxResponseBodyBytes - buf->size())
            {
                return 0;
            }
            buf->append(static_cast<char*>(contents), totalSize);
        }
        catch (...)
        {
            return 0;
        }
        return totalSize;
    }

    static size_t MultiHeaderCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        size_t const totalSize = size * nmemb;
        try
        {
            auto* buf = static_cast<std::string*>(userp);
            if (totalSize > kMaxHeaderBytes - buf->size())
            {
                return 0;
            }
            buf->append(static_cast<char*>(contents), totalSize);
        }
        catch (...)
        {
            return 0;
        }
        return totalSize;
    }

    static int MultiProgressCallback(void* /*clientp*/, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                                     curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
    {
        // Abort in-flight transfers when the engine is shutting down.
        if (Core::g_Core != nullptr && Core::g_Core->GetThreadPool().IsStopped())
        {
            return 1; // non-zero → libcurl returns CURLE_ABORTED_BY_CALLBACK
        }
        return 0;
    }

    // ---------------------------------------------------------------------------
    // Construction / destruction
    // ---------------------------------------------------------------------------

    CurlMultiDispatcher::CurlMultiDispatcher()
    {
        m_MultiHandle = curl_multi_init();
        CORE_ASSERT(m_MultiHandle != nullptr, "curl_multi_init() failed");

        // One TCP connection per host; HTTP/2 streams are multiplexed over it.
        curl_multi_setopt(m_MultiHandle, CURLMOPT_PIPELINING,             CURLPIPE_MULTIPLEX);
        curl_multi_setopt(m_MultiHandle, CURLMOPT_MAX_HOST_CONNECTIONS,   1L);
        curl_multi_setopt(m_MultiHandle, CURLMOPT_MAX_CONCURRENT_STREAMS, 100L);

        m_IoThread = std::thread(&CurlMultiDispatcher::IoThreadFunc, this);
        LOG_CORE_INFO("CurlMultiDispatcher: I/O thread started");
    }

    CurlMultiDispatcher::~CurlMultiDispatcher()
    {
        SignalStop();
        WaitStop();
        if (m_MultiHandle != nullptr)
        {
            curl_multi_cleanup(m_MultiHandle);
            m_MultiHandle = nullptr;
        }
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
        curl_multi_wakeup(m_MultiHandle);
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
        // socket activity is in flight.  curl handle mutations must be on
        // the I/O thread (single-thread relative to curl_multi_perform).
        if (m_MultiHandle != nullptr)
        {
            curl_multi_wakeup(m_MultiHandle);
        }
    }

    void CurlMultiDispatcher::SignalStop()
    {
        m_Stopping.store(true);
        if (m_MultiHandle != nullptr)
        {
            curl_multi_wakeup(m_MultiHandle);
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
            for (auto const& [handle, req] : m_Active)
            {
                std::string host = ExtractHost(req->m_Url);
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

    CURL* CurlMultiDispatcher::SetupEasyHandle(ActiveRequest& req,
                                               SetupError& errorKind,
                                               std::string& errorMessage)
    {
        errorKind = SetupError::None;
        errorMessage.clear();

        CURL* easy = curl_easy_init();
        if (easy == nullptr)
        {
            errorKind = SetupError::CurlInit;
            errorMessage = "curl_easy_init() failed";
            LOG_CORE_ERROR("CurlMultiDispatcher: curl_easy_init() failed url='{}' cancelKey='{}' quotaKey='{}'",
                           req.m_Url, req.m_QueryData.m_CancelKey, req.m_QueryData.m_QuotaKey);
            return nullptr;
        }

        // Auth headers are produced by IAuthSigner so AwsSigV4 / AzureApiKey are
        // covered identically to CurlWrapper::Query — no per-style branching here.
        // Pre-fix the async path silently produced unsigned / sentinel-signed requests
        // when credentials were empty; AWS / OpenAI rejected them as 401 with no
        // local indication of the root cause.  Now the signer rejects locally and
        // we emit a structured ERROR with the request's CancelKey (= per-task ID)
        // for the dashboard's run analyzer.
        std::vector<std::string> authHeaders;
        std::string authError;
        if (!IAuthSigner::Get(req.m_QueryData.m_AuthStyle).Apply(req.m_QueryData, authHeaders, authError))
        {
            errorKind = SetupError::AuthSigner;
            errorMessage = authError;
            LOG_CORE_ERROR("CurlMultiDispatcher: auth signer rejected url='{}' cancelKey='{}' quotaKey='{}': {}",
                           req.m_Url, req.m_QueryData.m_CancelKey, req.m_QueryData.m_QuotaKey, authError);
            curl_easy_cleanup(easy);
            return nullptr;
        }
        for (auto const& h : authHeaders)
        {
            req.m_Headers = curl_slist_append(req.m_Headers, h.c_str());
        }
        req.m_Headers = curl_slist_append(req.m_Headers, "Content-Type: application/json");

        // HTTP/2 for HTTPS (ALPN negotiation); falls back to HTTP/1.1 if unsupported.
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,     CURL_HTTP_VERSION_2TLS);

        curl_easy_setopt(easy, CURLOPT_URL,              req.m_Url.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER,       req.m_Headers);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS,       req.m_PostData.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,    MultiWriteCallback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA,        &req.m_ReadBuffer);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,   MultiHeaderCallback);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA,       &req.m_HeaderBuffer);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS,       0L);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, MultiProgressCallback);

        // Liveness: TCP keepalive helps long-idle in-flight connections
        // notice they're dead.  Default CURLOPT_TCP_KEEPIDLE (60s) is fine for
        // AI requests that may "think" silently before emitting tokens.
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE,    1L);

        if (req.m_QueryData.m_TimeoutMs > 0)
        {
            curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, req.m_QueryData.m_TimeoutMs);
        }

        std::string const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(easy, CURLOPT_CAINFO, caBundle.c_str());
        }

#ifdef DEBUG
        // Hermetic dispatcher tests configure AI interfaces pointing at the
        // j9t server's own debug mock endpoint (https://localhost:8443/...).
        // The j9t HTTPS cert is self-signed; the system CA bundle doesn't
        // trust it, so curl returns CURLE_SSL_PEER_CERTIFICATE.  Disable
        // verification for localhost ONLY, in debug builds ONLY — production
        // paths and non-localhost URLs still verify.
        std::string const host = ExtractHost(req.m_Url);
        if (host == "localhost" || host == "127.0.0.1" || host == "::1")
        {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }
#endif

        return easy;
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
        for (auto const& [handle, req] : m_Active)
        {
            std::string const host = ExtractHost(req->m_Url);
            if (!host.empty())
                ++activePerHost[host];
            std::string const key = req->m_QueryData.m_QuotaKey.empty()
                                        ? host
                                        : req->m_QueryData.m_QuotaKey;
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
            std::string const host = ExtractHost(pending.m_QueryData.m_Url);
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

            auto req = std::make_unique<ActiveRequest>();
            req->m_QueryData     = pending.m_QueryData;
            req->m_Callback      = std::move(pending.m_Callback);
            req->m_Url           = pending.m_QueryData.m_Url;
            req->m_PostData      = pending.m_QueryData.m_Data;
            req->m_RetryCount    = pending.m_RetryCount;
            req->m_InterfaceType = pending.m_QueryData.m_InterfaceType;

            SetupError setupErrorKind = SetupError::None;
            std::string setupErrorMsg;
            CURL* easy = SetupEasyHandle(*req, setupErrorKind, setupErrorMsg);
            if (easy != nullptr)
            {
                // Check curl_multi_add_handle return — failure (e.g. CURLM_OUT_OF_MEMORY)
                // would otherwise leak the easy handle and add a stale entry to m_Active.
                // Free the slist we built in SetupEasyHandle, cleanup the easy handle,
                // and surface the failure through the request callback.
                CURLMcode const addResult = curl_multi_add_handle(m_MultiHandle, easy);
                if (addResult == CURLM_OK)
                {
                    m_Active[easy] = std::move(req);
                    if (!host.empty())
                        ++activePerHost[host];
                    if (!quotaKey.empty())
                        ++activePerKey[quotaKey];
                    ++m_TotalDispatched;
                }
                else
                {
                    LOG_CORE_ERROR("CurlMultiDispatcher: curl_multi_add_handle failed code={} url='{}' "
                                   "cancelKey='{}' quotaKey='{}'",
                                   static_cast<int>(addResult), req->m_Url,
                                   req->m_QueryData.m_CancelKey, req->m_QueryData.m_QuotaKey);
                    curl_slist_free_all(req->m_Headers);
                    req->m_Headers = nullptr;
                    curl_easy_cleanup(easy);
                    req->m_Callback(QueryResult::Fail(QueryErrorCode::CurlNotInitialized,
                                                      "curl_multi_add_handle failed"),
                                    {});
                }
            }
            else
            {
                // Map the closed-set failure mode onto the unified QueryErrorCode
                // scheme.  -Wswitch will flag this if a SetupError variant is added.
                int code = QueryErrorCode::CurlNotInitialized;
                switch (setupErrorKind)
                {
                    case SetupError::CurlInit:    code = QueryErrorCode::CurlNotInitialized; break;
                    case SetupError::AuthSigner:  code = QueryErrorCode::NoApiKey;           break;
                    case SetupError::None:        // unreachable when easy == nullptr
                        break;
                }
                req->m_Callback(QueryResult::Fail(code, setupErrorMsg), {});
            }

            local.pop();
        }
    }

    // ---------------------------------------------------------------------------
    // Rate limit helpers (I/O thread only)
    // ---------------------------------------------------------------------------

    std::string CurlMultiDispatcher::ExtractHost(std::string const& url)
    {
        // "https://api.openai.com/v1/chat/completions" → "api.openai.com"
        // "https://[::1]:8443/path"                    → "::1"
        size_t start = url.find("://");
        if (start == std::string::npos)
            return {};
        start += 3;

        // RFC 3986 IPv6 literal: bracketed host runs `[<addr>]`, optionally
        // followed by `:<port>`.  Without this branch the generic `find(':')`
        // below clips at the first `:` of `::1` and returns "[".
        if (start < url.size() && url[start] == '[')
        {
            size_t close = url.find(']', start);
            if (close == std::string::npos)
                return {};
            return url.substr(start + 1, close - start - 1);
        }

        size_t end = url.find('/', start);
        if (end == std::string::npos)
            end = url.size();
        // Strip port if present
        size_t colon = url.find(':', start);
        if (colon != std::string::npos && colon < end)
            end = colon;
        return url.substr(start, end - start);
    }

    void CurlMultiDispatcher::ParseRateLimitHeaders(ActiveRequest const& req, std::string& host, long httpCode)
    {
        // Per-provider strategy delegation (Phase 1) + controller Observe (Phase 2).
        // The strategy returns a normalized RateLimitObservation; we merge it into
        // the legacy HostRateLimitState (kept for /api/debug/signals until Phase 5)
        // AND into the per-(host, modelFamily) controller for AIMD/token-bucket
        // gating.
        ConfigParser::EngineConfig::InterfaceType const interfaceType =
            (req.m_InterfaceType >= 0 && req.m_InterfaceType < ConfigParser::EngineConfig::InterfaceType::NumAPIs)
                ? static_cast<ConfigParser::EngineConfig::InterfaceType>(req.m_InterfaceType)
                : ConfigParser::EngineConfig::InterfaceType::InvalidAPI;

        IRateLimitStrategy const& strategy = IRateLimitStrategy::Get(interfaceType);
        RateLimitObservation const observation =
            strategy.Parse(req.m_HeaderBuffer, req.m_ReadBuffer, static_cast<int>(httpCode));

        std::lock_guard<std::recursive_mutex> debugLock(m_DebugMutex);

        // ---- Controller Observe (Phase 2) ----
        // Find or create the controller for this request's QuotaKey.  Note: we
        // create on first observation (not just first dispatch) so the controller
        // exists for snapshot reads even before any ShouldAdmit() runs through
        // it for that key.  hardCap = kMaxActivePerHost; Phase 4 will swap in
        // config.rate_limit.max_concurrency.
        std::string const quotaKey = req.m_QueryData.m_QuotaKey.empty() ? host : req.m_QueryData.m_QuotaKey;
        if (!quotaKey.empty())
        {
            auto controllerIt = EnsureController(quotaKey, req.m_QueryData);
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

            // 3. Active set — abort in-flight curl handle, fire callback.
            // Iterate by collecting matches first since erasing while iterating
            // m_Active is awkward; the matches are O(<10) typically per cancel.
            std::vector<CURL*> toAbort;
            for (auto const& [easy, req] : m_Active)
            {
                if (req->m_QueryData.m_CancelKey == cancelKey)
                {
                    toAbort.push_back(easy);
                }
            }
            for (CURL* easy : toAbort)
            {
                auto it = m_Active.find(easy);
                if (it == m_Active.end())
                {
                    continue;
                }
                ActiveRequest& req = *it->second;
                Callback callback = std::move(req.m_Callback);
                struct curl_slist* hdrs = req.m_Headers;
                curl_multi_remove_handle(m_MultiHandle, easy);
                curl_slist_free_all(hdrs);
                curl_easy_cleanup(easy);
                m_Active.erase(it);
                if (callback)
                {
                    callback(QueryResult::Fail(static_cast<int>(CURLE_ABORTED_BY_CALLBACK),
                                                 "request cancelled (run terminated)"),
                             {});
                }
                ++m_TotalCancelled;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Completion handling with 429 retry
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::DrainCompleted()
    {
        static std::atomic<uint32_t> s_QueryCounter{0};

        int msgsLeft = 0;
        CURLMsg* msg = nullptr;
        while ((msg = curl_multi_info_read(m_MultiHandle, &msgsLeft)) != nullptr)
        {
            if (msg->msg != CURLMSG_DONE)
            {
                continue;
            }

            CURL* easy = msg->easy_handle;
            CURLcode res = msg->data.result;

            std::lock_guard<std::recursive_mutex> debugLock(m_DebugMutex);
            auto it = m_Active.find(easy);
            if (it == m_Active.end())
            {
                // Unexpected — clean up defensively.
                curl_multi_remove_handle(m_MultiHandle, easy);
                curl_easy_cleanup(easy);
                continue;
            }

            ActiveRequest& req = *it->second;

            long httpVersion = 0;
            curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &httpVersion);
            std::string_view const versionLabel =
                (httpVersion == CURL_HTTP_VERSION_2) ? "HTTP/2" :
                (httpVersion == CURL_HTTP_VERSION_1_1) ? "HTTP/1.1" : "HTTP/1.0";

            long httpCode = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);

            uint32_t const qnum = ++s_QueryCounter;

            // Always parse rate limit headers (even on success) to keep state fresh.
            std::string host = ExtractHost(req.m_Url);
            if (!host.empty())
            {
                ParseRateLimitHeaders(req, host, httpCode);
            }

            // --- Handle 429 with auto-retry ---
            // Sentinel discipline: -1 (or any negative) = "unset, use dispatcher
            // default"; 0 = "no retries, fail on first 429"; >0 = explicit retry
            // budget.  Conflating 0 with the unset case made hermetic tests
            // (e.g. test_aimd_concurrency_cap.py) silently inherit the default
            // 10 and fire 11× as many Observe(was_429=true) calls as expected.
            int const maxRetries429 = (req.m_QueryData.m_MaxRetries429 >= 0) ? req.m_QueryData.m_MaxRetries429
                                                                             : kDefaultMaxRetries429;
            int const baseRetryMs = (req.m_QueryData.m_BaseRetryMs > 0) ? req.m_QueryData.m_BaseRetryMs
                                                                        : kDefaultBaseRetryMs;
            int const maxRetriesTransient = (req.m_QueryData.m_MaxRetriesTransient >= 0)
                                                ? req.m_QueryData.m_MaxRetriesTransient
                                                : kDefaultMaxRetriesTransient;

            if (res == CURLE_OK && httpCode == 429 && req.m_RetryCount < maxRetries429 && !m_Stopping.load())
            {
                int retryCount = req.m_RetryCount + 1;

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
                pendingRetry.m_QueryData     = std::move(req.m_QueryData);
                pendingRetry.m_Callback      = std::move(req.m_Callback);

                RetryEntry entry;
                entry.m_ReadyAt    = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
                entry.m_Request    = std::move(pendingRetry);
                entry.m_RetryCount = retryCount;

                {
                    std::lock_guard<std::recursive_mutex> lock(m_DebugMutex);
                    m_RetryQueue.push_back(std::move(entry));
                    curl_multi_remove_handle(m_MultiHandle, easy);
                    curl_slist_free_all(req.m_Headers);
                    curl_easy_cleanup(easy);
                    m_Active.erase(it);
                }
                continue; // do NOT invoke callback — retry is pending
            }

            // --- Handle transient HTTP errors (400, 500, 502, 503) with limited auto-retry ---
            // Note: curl-level errors (timeout, couldnt-connect, SSL handshake failure)
            // do NOT enter this retry path — the `res == CURLE_OK` guard means only
            // server-side HTTP statuses retry here.  Curl-level errors typically indicate
            // persistent network/auth/cert issues where retrying inside the dispatcher
            // would burn the per-request timeout budget without changing the outcome;
            // the higher-level WorkflowRuntimeManager owns retry policy for those.
            bool const isTransientError = (httpCode == 400 || httpCode == 500 || httpCode == 502 || httpCode == 503);
            if (res == CURLE_OK && isTransientError && req.m_RetryCount < maxRetriesTransient && !m_Stopping.load())
            {
                int retryCount = req.m_RetryCount + 1;
                int delayMs = baseRetryMs * (1 << (retryCount - 1)); // 1s, 2s

                LOG_CORE_WARN("HTTP {} for query {} — transient error, auto-retrying in {}ms (attempt {}/{})",
                              httpCode, qnum, delayMs, retryCount, maxRetriesTransient);

                PendingRequest pendingRetry;
                pendingRetry.m_QueryData     = std::move(req.m_QueryData);
                pendingRetry.m_Callback      = std::move(req.m_Callback);

                RetryEntry entry;
                entry.m_ReadyAt    = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
                entry.m_Request    = std::move(pendingRetry);
                entry.m_RetryCount = retryCount;

                {
                    std::lock_guard<std::recursive_mutex> lock(m_DebugMutex);
                    m_RetryQueue.push_back(std::move(entry));
                    curl_multi_remove_handle(m_MultiHandle, easy);
                    curl_slist_free_all(req.m_Headers);
                    curl_easy_cleanup(easy);
                    m_Active.erase(it);
                }
                continue; // do NOT invoke callback — retry is pending
            }

            LOG_CORE_INFO("query {} used {} (HTTP {}){}", qnum, versionLabel, httpCode,
                          req.m_RetryCount > 0 ? " [retry " + std::to_string(req.m_RetryCount) + "]" : "");

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
            if (res != CURLE_OK)
            {
                int const curlCode = static_cast<int>(res);
                std::string errMsg = curl_easy_strerror(res);
                if (res == CURLE_ABORTED_BY_CALLBACK)
                {
                    LOG_CORE_INFO("[shutdown] curl request aborted (query {})", qnum);
                }
                else
                {
                    // cancelKey + quotaKey lets the dashboard run analyzer surface
                    // this line — it filters issues to ERROR lines containing the
                    // run's identifiers (per CLAUDE.md "Failure-path logs").
                    LOG_CORE_ERROR("curl error (code {}): {} cancelKey='{}' quotaKey='{}'",
                                   curlCode, errMsg,
                                   req.m_QueryData.m_CancelKey, req.m_QueryData.m_QuotaKey);
                }
                result = QueryResult::Fail(curlCode, std::move(errMsg));
            }
            else if (httpCode >= 400)
            {
                std::string errMsg = QueryErrorCode::Describe(static_cast<int>(httpCode));
                if (httpCode == 429)
                {
                    LOG_CORE_ERROR("HTTP 429 rate limit for query {} — AI provider rejected the request; "
                                   "retries exhausted ({}x) cancelKey='{}' quotaKey='{}'",
                                   qnum, req.m_RetryCount,
                                   req.m_QueryData.m_CancelKey, req.m_QueryData.m_QuotaKey);
                    ++m_TotalRetriesExhausted;
                }
                else
                {
                    LOG_CORE_ERROR("HTTP error {} for query {} cancelKey='{}' quotaKey='{}'",
                                   httpCode, qnum,
                                   req.m_QueryData.m_CancelKey, req.m_QueryData.m_QuotaKey);
                }
                result = QueryResult::Fail(static_cast<int>(httpCode), std::move(errMsg));
            }
            else
            {
                result = QueryResult::Ok();
            }

            // Move data out of req before erasing (callback may re-enter Submit).
            std::string responseBody  = std::move(req.m_ReadBuffer);
            Callback callback         = std::move(req.m_Callback);
            struct curl_slist* hdrs   = req.m_Headers;

            {
                std::lock_guard<std::recursive_mutex> lock(m_DebugMutex);
                curl_multi_remove_handle(m_MultiHandle, easy);
                curl_slist_free_all(hdrs);
                curl_easy_cleanup(easy);
                m_Active.erase(it);
            }
            ++m_TotalCompleted;

            callback(result, std::move(responseBody));
        }
    }

    // ---------------------------------------------------------------------------
    // I/O thread loop
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::IoThreadFunc()
    {
        // Loop body wrapped in try/catch so a transient bad_alloc anywhere
        // inside DrainInbox / DrainCompleted / DrainPendingCancellations does
        // NOT silently terminate the I/O thread (which would freeze every
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

                int running = 0;
                curl_multi_perform(m_MultiHandle, &running);
                DrainCompleted();

                if (stopping && m_Active.empty())
                {
                    break;
                }

                // Sleep until socket activity or curl_multi_wakeup() (from Submit/SignalStop).
                // Use shorter timeout when retries are pending so we wake up to process them.
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
                curl_multi_poll(m_MultiHandle, nullptr, 0, timeout_ms, nullptr);
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
