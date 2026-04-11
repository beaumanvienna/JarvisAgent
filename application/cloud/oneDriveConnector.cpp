/* Copyright (c) 2026 JC Technolabs
   License: GPL-3.0

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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "cloud/oneDriveConnector.h"

#include <curl/curl.h>

#include "core.h"
#include "engine.h"
#include "keys/keyManager.h"
#include "keys/oauthTokenManager.h"
#include "curlWrapper/curlWrapper.h"

namespace AIAssistant
{
    std::string OneDriveConnector::GetType() const
    {
        return "onedrive";
    }

    bool OneDriveConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                               std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for OneDrive connection '" + connection.m_Name + "'";
            return false;
        }

        auto& oauthManager = Core::g_Core->GetOAuthTokenManager();
        std::string accessToken = oauthManager.GetAccessToken(connection.m_KeyName, errorMessage);
        if (accessToken.empty())
        {
            if (errorMessage.empty())
            {
                errorMessage = "Failed to obtain OAuth access token for '" + connection.m_KeyName + "'";
            }
            return false;
        }

        credentials.m_AuthType = CloudAuthType::OAuth2;
        credentials.m_Token = std::move(accessToken);
        return true;
    }

    std::string OneDriveConnector::GetGraphBaseUrl(CloudConnection const& connection)
    {
        if (!connection.m_Endpoint.empty())
        {
            std::string base = connection.m_Endpoint;
            if (!base.empty() && base.back() == '/')
            {
                base.pop_back();
            }
            return base;
        }
        return DEFAULT_GRAPH_BASE_URL;
    }

    std::string OneDriveConnector::GetAuthorizeUrl(std::string const& tenantId)
    {
        std::string tenant = tenantId.empty() ? DEFAULT_TENANT_ID : tenantId;
        return "https://login.microsoftonline.com/" + tenant + "/oauth2/v2.0/authorize";
    }

    std::string OneDriveConnector::GetTokenUrl(std::string const& tenantId)
    {
        std::string tenant = tenantId.empty() ? DEFAULT_TENANT_ID : tenantId;
        return "https://login.microsoftonline.com/" + tenant + "/oauth2/v2.0/token";
    }

    bool OneDriveConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        auto clientIdIt = connection.m_Params.find("client_id");
        if (clientIdIt == connection.m_Params.end() || clientIdIt->second.empty())
        {
            errorMessage = "OneDrive connection requires 'client_id' parameter";
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        // GET /me/drive — verifies token and returns drive info
        std::string url = GetGraphBaseUrl(connection) + "/me/drive";

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        std::string responseBody;
        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            buf->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        std::string authHeader = "Authorization: Bearer " + credentials.m_Token;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, authHeader.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("OneDrive test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode == 401)
        {
            errorMessage = "OneDrive test failed: unauthorized (HTTP 401) — token may be expired or invalid";
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "OneDrive test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
