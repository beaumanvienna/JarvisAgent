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

#include "core.h"
#include "engine.h"

namespace AIAssistant
{
    // ---------------------------------------------------------------------------
    // Static callbacks (no class state needed)
    // ---------------------------------------------------------------------------

    static size_t MultiWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        size_t const totalSize = size * nmemb;
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), totalSize);
        return totalSize;
    }

    static size_t MultiHeaderCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        size_t const totalSize = size * nmemb;
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), totalSize);
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
        {
            std::lock_guard<std::mutex> lock(m_InboxMutex);
            m_Inbox.push({data, std::move(callback)});
        }
        curl_multi_wakeup(m_MultiHandle);
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

    // ---------------------------------------------------------------------------
    // Private helpers (I/O thread only)
    // ---------------------------------------------------------------------------

    CURL* CurlMultiDispatcher::SetupEasyHandle(ActiveRequest& req)
    {
        CURL* easy = curl_easy_init();
        if (easy == nullptr)
        {
            LOG_CORE_ERROR("CurlMultiDispatcher: curl_easy_init() failed");
            return nullptr;
        }

        // Build authentication header based on provider style.
        std::string authHeader;
        if (req.m_QueryData.m_AuthStyle == CurlWrapper::AuthStyle::Bearer)
        {
            authHeader = "Authorization: Bearer " + req.m_QueryData.m_ApiKey;
        }
        else // XGoogApiKey (Gemini native)
        {
            authHeader = "x-goog-api-key: " + req.m_QueryData.m_ApiKey;
        }
        req.m_Headers = curl_slist_append(req.m_Headers, authHeader.c_str());
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

        if (req.m_QueryData.m_TimeoutMs > 0)
        {
            curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, req.m_QueryData.m_TimeoutMs);
        }

        std::string const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(easy, CURLOPT_CAINFO, caBundle.c_str());
        }

        return easy;
    }

    void CurlMultiDispatcher::DrainInbox()
    {
        std::queue<PendingRequest> local;
        {
            std::lock_guard<std::mutex> lock(m_InboxMutex);
            local.swap(m_Inbox);
        }

        while (!local.empty())
        {
            auto& pending = local.front();

            auto req = std::make_unique<ActiveRequest>();
            req->m_QueryData = pending.m_QueryData;
            req->m_Callback  = std::move(pending.m_Callback);
            req->m_Url       = pending.m_QueryData.m_Url;
            req->m_PostData  = pending.m_QueryData.m_Data;

            CURL* easy = SetupEasyHandle(*req);
            if (easy != nullptr)
            {
                curl_multi_add_handle(m_MultiHandle, easy);
                m_Active[easy] = std::move(req);
            }
            else
            {
                req->m_Callback(QueryResult::Fail(QueryErrorCode::CurlNotInitialized, "curl_easy_init() failed"), {});
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
        size_t start = url.find("://");
        if (start == std::string::npos)
            return {};
        start += 3;
        size_t end = url.find('/', start);
        if (end == std::string::npos)
            end = url.size();
        // Strip port if present
        size_t colon = url.find(':', start);
        if (colon != std::string::npos && colon < end)
            end = colon;
        return url.substr(start, end - start);
    }

    void CurlMultiDispatcher::ParseRateLimitHeaders(ActiveRequest const& req, std::string& host)
    {
        // Headers arrive as "Name: Value\r\n" lines concatenated in m_HeaderBuffer.
        auto& state = m_HostRateLimits[host];
        state.m_LastUpdated = std::chrono::steady_clock::now();

        size_t pos = 0;
        while (pos < req.m_HeaderBuffer.size())
        {
            size_t eol = req.m_HeaderBuffer.find('\n', pos);
            if (eol == std::string::npos)
                eol = req.m_HeaderBuffer.size();

            std::string line = req.m_HeaderBuffer.substr(pos, eol - pos);
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Case-insensitive prefix match
            auto startsWith = [&](std::string const& prefix) -> bool
            {
                if (line.size() < prefix.size())
                    return false;
                for (size_t i = 0; i < prefix.size(); ++i)
                {
                    if (std::tolower(static_cast<unsigned char>(line[i])) !=
                        std::tolower(static_cast<unsigned char>(prefix[i])))
                        return false;
                }
                return true;
            };

            auto valueAfterColon = [&]() -> std::string
            {
                size_t c = line.find(':');
                if (c == std::string::npos)
                    return {};
                size_t v = c + 1;
                while (v < line.size() && line[v] == ' ')
                    ++v;
                return line.substr(v);
            };

            if (startsWith("x-ratelimit-remaining-requests:"))
            {
                try { state.m_RemainingRequests = std::stoi(valueAfterColon()); } catch (...) {}
            }
            else if (startsWith("x-ratelimit-remaining-tokens:"))
            {
                try { state.m_RemainingTokens = std::stoi(valueAfterColon()); } catch (...) {}
            }
            else if (startsWith("x-ratelimit-reset-requests:"))
            {
                // Value is like "200ms" or "6s" or "1m30s"
                std::string val = valueAfterColon();
                int ms = 0;
                // Parse duration: combinations of Nm, Ns, Nms
                size_t i = 0;
                while (i < val.size())
                {
                    if (!std::isdigit(static_cast<unsigned char>(val[i])))
                    {
                        ++i;
                        continue;
                    }
                    int num = 0;
                    while (i < val.size() && std::isdigit(static_cast<unsigned char>(val[i])))
                    {
                        num = num * 10 + (val[i] - '0');
                        ++i;
                    }
                    if (i + 1 < val.size() && val[i] == 'm' && val[i + 1] == 's')
                    {
                        ms += num;
                        i += 2;
                    }
                    else if (i < val.size() && val[i] == 'm')
                    {
                        ms += num * 60000;
                        ++i;
                    }
                    else if (i < val.size() && val[i] == 's')
                    {
                        ms += num * 1000;
                        ++i;
                    }
                }
                if (ms > 0)
                {
                    state.m_ResetAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                }
            }

            pos = eol + 1;
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
        // Process entries whose delay has expired.
        // Iterate backwards so we can erase without invalidating indices.
        for (int i = static_cast<int>(m_RetryQueue.size()) - 1; i >= 0; --i)
        {
            if (m_RetryQueue[static_cast<size_t>(i)].m_ReadyAt <= now)
            {
                RetryEntry entry = std::move(m_RetryQueue[static_cast<size_t>(i)]);
                m_RetryQueue.erase(m_RetryQueue.begin() + i);

                // Re-submit as a new active request.
                auto req = std::make_unique<ActiveRequest>();
                req->m_QueryData  = entry.m_Request.m_QueryData;
                req->m_Callback   = std::move(entry.m_Request.m_Callback);
                req->m_Url        = entry.m_Request.m_QueryData.m_Url;
                req->m_PostData   = entry.m_Request.m_QueryData.m_Data;
                req->m_RetryCount = entry.m_RetryCount;

                CURL* easy = SetupEasyHandle(*req);
                if (easy != nullptr)
                {
                    curl_multi_add_handle(m_MultiHandle, easy);
                    m_Active[easy] = std::move(req);
                }
                else
                {
                    req->m_Callback(QueryResult::Fail(QueryErrorCode::CurlNotInitialized, "curl_easy_init() failed"), {});
                }
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
                ParseRateLimitHeaders(req, host);
            }

            // --- Handle 429 with auto-retry ---
            if (res == CURLE_OK && httpCode == 429 && req.m_RetryCount < kMaxRetries && !m_Stopping.load())
            {
                int retryCount = req.m_RetryCount + 1;

                // Determine delay: prefer Retry-After header, fall back to exponential backoff.
                int delayMs = kBaseRetryMs * (1 << (retryCount - 1)); // exponential: 1s, 2s, 4s, 8s, 16s

                // Check if the host state has a reset-at time that's more informative.
                auto hostIt = m_HostRateLimits.find(host);
                if (hostIt != m_HostRateLimits.end())
                {
                    auto msUntilReset = std::chrono::duration_cast<std::chrono::milliseconds>(
                        hostIt->second.m_ResetAt - std::chrono::steady_clock::now()).count();
                    if (msUntilReset > 0 && msUntilReset < 120000) // cap at 2 minutes
                    {
                        delayMs = static_cast<int>(msUntilReset) + 100; // small buffer
                    }
                }

                LOG_CORE_WARN("HTTP 429 rate limit for query {} (host: {}) — auto-retrying in {}ms (attempt {}/{})",
                              qnum, host, delayMs, retryCount, kMaxRetries);

                // Build retry entry.
                PendingRequest pendingRetry;
                pendingRetry.m_QueryData = std::move(req.m_QueryData);
                pendingRetry.m_Callback  = std::move(req.m_Callback);

                RetryEntry entry;
                entry.m_ReadyAt    = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
                entry.m_Request    = std::move(pendingRetry);
                entry.m_RetryCount = retryCount;

                m_RetryQueue.push_back(std::move(entry));

                // Clean up the completed easy handle.
                curl_multi_remove_handle(m_MultiHandle, easy);
                curl_slist_free_all(req.m_Headers);
                curl_easy_cleanup(easy);
                m_Active.erase(it);
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
                    LOG_CORE_ERROR("curl error (code {}): {}", curlCode, errMsg);
                }
                result = QueryResult::Fail(curlCode, std::move(errMsg));
            }
            else if (httpCode >= 400)
            {
                std::string errMsg = QueryErrorCode::Describe(static_cast<int>(httpCode));
                if (httpCode == 429)
                {
                    LOG_CORE_ERROR("HTTP 429 rate limit for query {} — AI provider rejected the request; retries exhausted ({}x)",
                                   qnum, req.m_RetryCount);
                }
                else
                {
                    LOG_CORE_ERROR("HTTP error {} for query {}", httpCode, qnum);
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

            curl_multi_remove_handle(m_MultiHandle, easy);
            curl_slist_free_all(hdrs);
            curl_easy_cleanup(easy);
            m_Active.erase(it);

            callback(result, std::move(responseBody));
        }
    }

    // ---------------------------------------------------------------------------
    // I/O thread loop
    // ---------------------------------------------------------------------------

    void CurlMultiDispatcher::IoThreadFunc()
    {
        while (true)
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

        LOG_CORE_INFO("CurlMultiDispatcher: I/O thread exiting (rate limit retries served: {})",
                      m_RetryQueue.empty() ? "all" : std::to_string(m_RetryQueue.size()) + " cancelled");
    }

} // namespace AIAssistant
