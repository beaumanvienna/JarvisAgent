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

#include "cloud/gitHubConnector.h"

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "curlWrapper/curlWrapper.h"
#include "cloud/cloudTaskExecutor.h"
#include "cloud/connectorHttp.h"

namespace AIAssistant
{
    std::string GitHubConnector::GetType() const
    {
        return "github";
    }

    std::string GitHubConnector::GetApiBaseUrl(CloudConnection const& connection)
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
        return DEFAULT_API_BASE_URL;
    }

    bool GitHubConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                             std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for GitHub connection '" + connection.m_Name + "'";
            return false;
        }

        bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
            [&](ICredential const& cred)
            {
                auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred);
                if (!api)
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' must be ApiKeyCredential — GitHub requires a Personal Access Token";
                    return;
                }
                if (api->m_ApiKey.IsEmpty())
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' has no API key — GitHub requires a Personal Access Token";
                    return;
                }
                credentials.m_AuthType = CloudAuthType::BearerToken;
                credentials.m_Token = std::string(api->m_ApiKey.Get());
            });
        if (!found)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }
        if (!errorMessage.empty())
        {
            return false;
        }
        return true;
    }

    bool GitHubConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        if (!connection.m_Endpoint.empty() &&
            !ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint, errorMessage))
        {
            LOG_SECURITY_WARN("[security] github_endpoint_rejected connection='{}' reason='{}'",
                              connection.m_Name, errorMessage);
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        if (ICloudTaskExecutor::ContainsCrlf(credentials.m_Token))
        {
            errorMessage = "GitHub PAT contains CR/LF — refusing to send";
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] github_test_pat_crlf_rejected connection='{}'", connection.m_Name);
            return false;
        }

        // GET /user — verifies token and returns user info
        std::string url = GetApiBaseUrl(connection) + "/user";

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        std::string responseBody;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ConnectorHttp::BoundedStringWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "j9t/1.0");
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + credentials.m_Token;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("GitHub test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode == 401)
        {
            errorMessage = "GitHub test failed: unauthorized (HTTP 401) — check Personal Access Token";
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "GitHub test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
