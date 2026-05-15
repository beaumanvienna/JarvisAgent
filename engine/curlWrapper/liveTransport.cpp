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

#include "curlWrapper/liveTransport.h"

#include <curl/curl.h>
#include <curl/multi.h>

#include "core.h"
#include "curlWrapper/authSigner.h"
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
    // Shared host extraction (declared in interfaceTransport.h)
    // ---------------------------------------------------------------------------

    std::string ExtractHostFromUrl(std::string const& url)
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

    // ---------------------------------------------------------------------------
    // Construction / destruction
    // ---------------------------------------------------------------------------

    LiveTransport::LiveTransport()
    {
        m_MultiHandle = curl_multi_init();
        CORE_ASSERT(m_MultiHandle != nullptr, "curl_multi_init() failed");

        // One TCP connection per host; HTTP/2 streams are multiplexed over it.
        curl_multi_setopt(m_MultiHandle, CURLMOPT_PIPELINING,             CURLPIPE_MULTIPLEX);
        curl_multi_setopt(m_MultiHandle, CURLMOPT_MAX_HOST_CONNECTIONS,   1L);
        curl_multi_setopt(m_MultiHandle, CURLMOPT_MAX_CONCURRENT_STREAMS, 100L);

        LOG_CORE_INFO("LiveTransport: curl multi handle initialised");
    }

    LiveTransport::~LiveTransport()
    {
        // Any handles still in-flight at destruction were already drained by
        // the dispatcher's shutdown loop (which waits for ActiveCount() == 0
        // before joining the I/O thread).  Defensive cleanup in case a
        // shutdown path skipped that drain.
        for (auto& [easy, req] : m_Active)
        {
            if (easy != nullptr)
            {
                curl_multi_remove_handle(m_MultiHandle, easy);
                if (req && req->m_Headers != nullptr)
                {
                    curl_slist_free_all(req->m_Headers);
                    req->m_Headers = nullptr;
                }
                curl_easy_cleanup(easy);
            }
        }
        m_Active.clear();

        if (m_MultiHandle != nullptr)
        {
            curl_multi_cleanup(m_MultiHandle);
            m_MultiHandle = nullptr;
        }
    }

    // ---------------------------------------------------------------------------
    // SetupEasyHandle — configure auth, headers, callbacks, TLS opts
    // ---------------------------------------------------------------------------

    CURL* LiveTransport::SetupEasyHandle(ActiveRequest& req,
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
            LOG_CORE_ERROR("LiveTransport: curl_easy_init() failed url='{}' cancelKey='{}' quotaKey='{}'",
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
            LOG_CORE_ERROR("LiveTransport: auth signer rejected url='{}' cancelKey='{}' quotaKey='{}': {}",
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
        std::string const host = ExtractHostFromUrl(req.m_Url);
        if (host == "localhost" || host == "127.0.0.1" || host == "::1")
        {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }
#endif

        return easy;
    }

    // ---------------------------------------------------------------------------
    // IInterfaceTransport implementation
    // ---------------------------------------------------------------------------

    void LiveTransport::Submit(RequestId id,
                               CurlWrapper::QueryData queryData,
                               CompletionCallback callback)
    {
        auto req = std::make_unique<ActiveRequest>();
        req->m_RequestId = id;
        req->m_Url       = queryData.m_Url;
        req->m_PostData  = queryData.m_Data;
        req->m_QueryData = std::move(queryData);
        req->m_Callback  = std::move(callback);

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
                return;
            }

            LOG_CORE_ERROR("LiveTransport: curl_multi_add_handle failed code={} url='{}' "
                           "cancelKey='{}' quotaKey='{}'",
                           static_cast<int>(addResult), req->m_Url,
                           req->m_QueryData.m_CancelKey, req->m_QueryData.m_QuotaKey);
            curl_slist_free_all(req->m_Headers);
            req->m_Headers = nullptr;
            curl_easy_cleanup(easy);

            // Defer the completion so the dispatcher sees a uniform delivery
            // point (Pump-driven) regardless of whether the failure was
            // synchronous (setup) or asynchronous (curl I/O).
            DeferredCompletion deferred;
            deferred.m_RequestId = req->m_RequestId;
            deferred.m_Callback  = std::move(req->m_Callback);
            deferred.m_Response.m_Result = QueryResult::Fail(QueryErrorCode::CurlNotInitialized,
                                                             "curl_multi_add_handle failed");
            m_DeferredCompletions.push_back(std::move(deferred));
            return;
        }

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

        DeferredCompletion deferred;
        deferred.m_RequestId = req->m_RequestId;
        deferred.m_Callback  = std::move(req->m_Callback);
        deferred.m_Response.m_Result = QueryResult::Fail(code, std::move(setupErrorMsg));
        m_DeferredCompletions.push_back(std::move(deferred));
    }

    void LiveTransport::CancelByCancelKey(std::string const& cancelKey)
    {
        if (cancelKey.empty())
        {
            return;
        }

        // Silent cleanup: the dispatcher synchronously fires the user callback
        // with "request cancelled (run terminated)" before calling this, so we
        // must NOT fire the per-request CompletionCallback (it would race or
        // double-fire).  Just clean up the curl handles and drop the entries.
        //
        // Iterate by collecting matches first since erasing while iterating
        // m_Active is awkward; the matches are O(<10) typically per cancel.
        std::vector<CURL*> toAbort;
        for (auto const& [easy, req] : m_Active)
        {
            if (req && req->m_QueryData.m_CancelKey == cancelKey)
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
            curl_multi_remove_handle(m_MultiHandle, easy);
            if (req.m_Headers != nullptr)
            {
                curl_slist_free_all(req.m_Headers);
                req.m_Headers = nullptr;
            }
            curl_easy_cleanup(easy);
            m_Active.erase(it);
        }
    }

    void LiveTransport::Pump()
    {
        int running = 0;
        curl_multi_perform(m_MultiHandle, &running);
        HarvestCompletions();
        DrainDeferredCompletions();
    }

    void LiveTransport::Wait(long timeoutMs)
    {
        curl_multi_poll(m_MultiHandle, nullptr, 0, timeoutMs, nullptr);
    }

    void LiveTransport::Wakeup()
    {
        if (m_MultiHandle != nullptr)
        {
            curl_multi_wakeup(m_MultiHandle);
        }
    }

    size_t LiveTransport::ActiveCount() const
    {
        return m_Active.size() + m_DeferredCompletions.size();
    }

    // ---------------------------------------------------------------------------
    // Completion harvesting (I/O thread only)
    // ---------------------------------------------------------------------------

    void LiveTransport::HarvestCompletions()
    {
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
            std::string versionLabel =
                (httpVersion == CURL_HTTP_VERSION_2) ? "HTTP/2" :
                (httpVersion == CURL_HTTP_VERSION_1_1) ? "HTTP/1.1" : "HTTP/1.0";

            long httpCode = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);

            Response response;
            response.m_Body              = std::move(req.m_ReadBuffer);
            response.m_RawHeaders        = std::move(req.m_HeaderBuffer);
            response.m_HttpStatus        = httpCode;
            response.m_HttpVersionLabel  = std::move(versionLabel);

            if (res != CURLE_OK)
            {
                int const curlCode = static_cast<int>(res);
                std::string errMsg = curl_easy_strerror(res);
                response.m_Result = QueryResult::Fail(curlCode, std::move(errMsg));
            }
            else
            {
                // HTTP-status interpretation (success vs 4xx/5xx) lives in the
                // dispatcher.  Transport hands back a curl-level Ok with the
                // raw HTTP status for the dispatcher to classify.
                response.m_Result = QueryResult::Ok();
            }

            RequestId const id = req.m_RequestId;
            CompletionCallback callback = std::move(req.m_Callback);
            struct curl_slist* hdrs = req.m_Headers;

            curl_multi_remove_handle(m_MultiHandle, easy);
            curl_slist_free_all(hdrs);
            curl_easy_cleanup(easy);
            m_Active.erase(it);

            if (callback)
            {
                callback(id, std::move(response));
            }
        }
    }

    void LiveTransport::DrainDeferredCompletions()
    {
        if (m_DeferredCompletions.empty())
        {
            return;
        }
        std::vector<DeferredCompletion> local;
        local.swap(m_DeferredCompletions);
        for (auto& deferred : local)
        {
            if (deferred.m_Callback)
            {
                deferred.m_Callback(deferred.m_RequestId, std::move(deferred.m_Response));
            }
        }
    }
} // namespace AIAssistant
