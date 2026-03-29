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

#include <filesystem>

#include <curl/curl.h>
#include "tracy/Tracy.hpp"

#include "core.h"
#include "engine.h"
#include "curlWrapper/curlWrapper.h"
#include "json/replyParser.h"

namespace AIAssistant
{

    std::atomic<uint32_t> CurlWrapper::m_QueryCounter{0};

    CurlWrapper::CurlWrapper()
    {
        // once globally
        {
            static bool initialized{false};
            static std::mutex initMutex;

            std::lock_guard<std::mutex> lock(initMutex);
            if (!initialized)
            {
                CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
                if (res != CURLE_OK)
                {
                    LOG_CORE_CRITICAL("curl_global_init() failed: {}", curl_easy_strerror(res));
                    return;
                }
                else
                {
                    initialized = true;
                    LOG_CORE_INFO("libcurl globally initialized");

                    // Log version info for diagnostics
                    curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
                    if (ver)
                    {
                        LOG_CORE_INFO("[curl-ssl] libcurl {} ssl: {}", ver->version,
                                      ver->ssl_version ? ver->ssl_version : "(none)");
                    }
                }
            }
        }

        // per instance
        m_Curl = curl_easy_init();

        if (!m_Curl)
        {
            LOG_CORE_CRITICAL("curl_easy_init() failed");
            return;
        }
        else
        {
            std::ostringstream oss;
            oss << std::this_thread::get_id();
            LOG_CORE_INFO("thread {} got a good curl", oss.str());
        }

        // Set CA bundle path for cross-distro compatibility (Ubuntu vs Fedora vs openSUSE etc.)
        auto const& caBundle = GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(m_Curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        m_Initialized = true;
    }

    CurlWrapper::~CurlWrapper()
    {
        if (m_Curl)
        {
            curl_easy_cleanup(m_Curl);
        }
    }

    CurlWrapper::CurlSlist::~CurlSlist()
    {
        if (m_List)
        {
            curl_slist_free_all(m_List);
        }
    }

    std::string const& CurlWrapper::GetCaBundlePath()
    {
        static std::string cachedPath = []() -> std::string
        {
            // Honour environment overrides first
            if (char const* env = std::getenv("CURL_CA_BUNDLE"))
            {
                if (std::filesystem::exists(env))
                {
                    return env;
                }
            }
            if (char const* env = std::getenv("SSL_CERT_FILE"))
            {
                if (std::filesystem::exists(env))
                {
                    return env;
                }
            }

            // Probe well-known paths across Linux distros and macOS
            static constexpr char const* candidates[] = {
                "/etc/ssl/certs/ca-certificates.crt", // Debian / Ubuntu
                "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora / RHEL / Rocky
                "/etc/ssl/ca-bundle.pem",             // openSUSE
                "/etc/ssl/cert.pem",                  // Alpine / macOS
            };

            for (auto const* path : candidates)
            {
                if (std::filesystem::exists(path))
                {
                    return path;
                }
            }

            return {}; // empty → let libcurl use its compiled-in default
        }();

        return cachedPath;
    }

    void CurlWrapper::GlobalCleanup()
    {
        curl_global_cleanup();
        LOG_CORE_INFO("libcurl globally cleaned up");
    }

    void CurlWrapper::CurlSlist::Append(std::string const& str)
    {
        m_List = curl_slist_append(m_List, str.c_str());
        if (!m_List)
        {
            // if curl_slist_append fails it returns nullptr
            LOG_CORE_CRITICAL("curl_slist_append failed");
        }
    }
    struct curl_slist* CurlWrapper::CurlSlist::Get() { return m_List; }

    bool CurlWrapper::IsInitialized() const { return m_Initialized; }

    std::string& CurlWrapper::GetBuffer() { return m_ReadBuffer; }

    void CurlWrapper::Clear() { m_ReadBuffer.clear(); }

    bool CurlWrapper::QueryData::IsValid() const
    {
        bool urlEmpty = m_Url.empty();
        bool dataEmpty = m_Data.empty();
        bool keyEmpty = m_ApiKey.empty();

        if (urlEmpty)
        {
            LOG_CORE_CRITICAL("CurlWrapper::QueryData::IsValid(): url empty");
        }
        if (dataEmpty)
        {
            LOG_CORE_CRITICAL("CurlWrapper::QueryData::IsValid(): data empty");
        }
        if (keyEmpty)
        {
            LOG_CORE_CRITICAL("CurlWrapper::QueryData::IsValid(): API key empty");
        }

        return !urlEmpty && !dataEmpty && !keyEmpty;
    }

    std::string QueryErrorCode::Describe(int code)
    {
        if (code == Success)
        {
            return "Success";
        }

        // CURLcode range (1-99)
        if (code >= 1 && code <= 99)
        {
            return std::string("curl: ") + curl_easy_strerror(static_cast<CURLcode>(code));
        }

        // HTTP status code range (100-599)
        if (code >= 100 && code <= 599)
        {
            switch (code)
            {
                case 200:
                    return "200 OK";
                case 400:
                    return "400 Bad Request";
                case 401:
                    return "401 Unauthorized";
                case 403:
                    return "403 Forbidden";
                case 404:
                    return "404 Not Found";
                case 429:
                    return "429 Too Many Requests";
                case 500:
                    return "500 Internal Server Error";
                case 502:
                    return "502 Bad Gateway";
                case 503:
                    return "503 Service Unavailable";
                default:
                    return "HTTP " + std::to_string(code);
            }
        }

        // Custom codes (1000+)
        switch (code)
        {
            case NoApiKey:
                return "No API key configured";
            case InvalidQueryData:
                return "Invalid query data";
            case CurlNotInitialized:
                return "curl not initialized";
            case ParserError:
                return "Response parser error";
            case EmptyResponse:
                return "Empty response from AI";
            case ExceptionThrown:
                return "Internal exception";
            case QuotaExceeded:
                return "Quota exceeded";
            case ContextWindowExceeded:
                return "Context window exceeded";
            case ContentPolicyViolation:
                return "Content policy violation";
            case ModelNotFound:
                return "Model not found";
            default:
                return "Unknown error " + std::to_string(code);
        }
    }

    QueryResult CurlWrapper::Query(QueryData const& queryData)
    {
        if (!m_Initialized)
        {
            return QueryResult::Fail(QueryErrorCode::CurlNotInitialized, "curl not initialized");
        }

        if (queryData.m_ApiKey.empty())
        {
            LOG_CORE_CRITICAL("CurlWrapper::Query: API key empty");
            return QueryResult::Fail(QueryErrorCode::NoApiKey, "No API key configured");
        }

        if (!queryData.IsValid())
        {
            return QueryResult::Fail(QueryErrorCode::InvalidQueryData, "Invalid query data (empty URL or payload)");
        }

        CurlSlist headers;
        switch (queryData.m_AuthStyle)
        {
            case AuthStyle::XGoogApiKey:
                headers.Append("x-goog-api-key: " + queryData.m_ApiKey);
                break;
            case AuthStyle::Bearer:
            default:
                headers.Append("Authorization: Bearer " + queryData.m_ApiKey);
                break;
        }
        headers.Append("Content-Type: application/json");

        auto& url = queryData.m_Url;
        auto& data = queryData.m_Data;

        // lambda for write callback
        auto write_callback = [](void* contents, size_t size, size_t numberOfMembers, void* userPointer) -> size_t
        {
            auto* buffer = reinterpret_cast<std::string*>(userPointer);
            const size_t totalSize = size * numberOfMembers;
            buffer->append(static_cast<char*>(contents), totalSize);
            return totalSize;
        };

        // Progress callback: abort transfer if thread pool is shutting down.
        auto progressCallback = [](void* /*clientp*/, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                                   curl_off_t /*ulnow*/) -> int
        {
            if (Core::g_Core != nullptr && Core::g_Core->GetThreadPool().IsStopped())
            {
                return 1; // non-zero aborts the transfer
            }
            return 0;
        };

        // Attempt HTTP/2 for HTTPS connections; fall back to HTTP/1.1 via ALPN if unsupported.
        curl_easy_setopt(m_Curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

        curl_easy_setopt(m_Curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(m_Curl, CURLOPT_HTTPHEADER, headers.Get());
        curl_easy_setopt(m_Curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(m_Curl, CURLOPT_WRITEFUNCTION, static_cast<CurlWriteCallback>(write_callback));
        curl_easy_setopt(m_Curl, CURLOPT_WRITEDATA, &m_ReadBuffer);
        curl_easy_setopt(m_Curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(m_Curl, CURLOPT_XFERINFOFUNCTION, static_cast<curl_xferinfo_callback>(progressCallback));

        if (queryData.m_TimeoutMs > 0)
        {
            curl_easy_setopt(m_Curl, CURLOPT_TIMEOUT_MS, queryData.m_TimeoutMs);
        }
        else
        {
            curl_easy_setopt(m_Curl, CURLOPT_TIMEOUT_MS, 0L); // no timeout
        }
        if (Core::g_Core->Verbose())
        {
            curl_easy_setopt(m_Curl, CURLOPT_VERBOSE, 1L);
            LOG_CORE_INFO("url: {}, data: {}", url, data);
        }

        LOG_CORE_INFO("sending query {}", ++m_QueryCounter);
        CURLcode res;
        {
#ifdef TRACY_ENABLE
            const int blue = 0x0000ff;
            ZoneScopedNC("curl_easy_perform(m_Curl)", blue);
#endif
            res = curl_easy_perform(m_Curl);
        }

        if (res != CURLE_OK)
        {
            int const curlCode = static_cast<int>(res);
            std::string msg = curl_easy_strerror(res);
            if (res == CURLE_ABORTED_BY_CALLBACK)
            {
                LOG_CORE_INFO("[shutdown] curl request aborted (query {})", m_QueryCounter.load());
            }
            else
            {
                LOG_CORE_ERROR("curl error (code {}): {}", curlCode, msg);
            }
            return QueryResult::Fail(curlCode, std::move(msg));
        }

        // curl succeeded — check HTTP version and status code
        long httpVersion = 0;
        curl_easy_getinfo(m_Curl, CURLINFO_HTTP_VERSION, &httpVersion);
        std::string_view versionLabel =
            (httpVersion == CURL_HTTP_VERSION_2) ? "HTTP/2" :
            (httpVersion == CURL_HTTP_VERSION_1_1) ? "HTTP/1.1" : "HTTP/1.0";

        long httpCode = 0;
        curl_easy_getinfo(m_Curl, CURLINFO_RESPONSE_CODE, &httpCode);

        LOG_CORE_INFO("query {} used {} (HTTP {})", m_QueryCounter.load(), versionLabel, httpCode);

        if (httpCode >= 400)
        {
            std::string msg = QueryErrorCode::Describe(static_cast<int>(httpCode));
            LOG_CORE_ERROR("HTTP error {} for query {}", httpCode, m_QueryCounter.load());
            return QueryResult::Fail(static_cast<int>(httpCode), std::move(msg));
        }

        return QueryResult::Ok();
    }
} // namespace AIAssistant
