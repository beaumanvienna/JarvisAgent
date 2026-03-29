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
            LOG_CORE_INFO("query {} used {} (HTTP {})", qnum, versionLabel, httpCode);

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
                LOG_CORE_ERROR("HTTP error {} for query {}", httpCode, qnum);
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
            }
            else
            {
                DrainInbox();
            }

            int running = 0;
            curl_multi_perform(m_MultiHandle, &running);
            DrainCompleted();

            if (stopping && m_Active.empty())
            {
                break;
            }

            // Sleep until socket activity or curl_multi_wakeup() (from Submit/SignalStop).
            long const timeout_ms = m_Active.empty() ? 1000L : 50L;
            curl_multi_poll(m_MultiHandle, nullptr, 0, timeout_ms, nullptr);
        }

        LOG_CORE_INFO("CurlMultiDispatcher: I/O thread exiting");
    }

} // namespace AIAssistant
