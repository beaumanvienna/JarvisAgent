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

#include "cloud/redmineConnector.h"

#include <curl/curl.h>

#include "core.h"
#include "engine.h"
#include "keys/keyManager.h"
#include "curlWrapper/curlWrapper.h"
#include "cloud/cloudTaskExecutor.h"
#include "cloud/connectorHttp.h"

namespace AIAssistant
{
    std::string RedmineConnector::GetType() const
    {
        return "redmine";
    }

    std::string RedmineConnector::GetBaseUrl(CloudConnection const& connection)
    {
        std::string base = connection.m_Endpoint;
        if (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }
        return base;
    }

    bool RedmineConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                              std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Redmine connection '" + connection.m_Name + "'";
            return false;
        }

        auto const* provider = Core::g_Core->GetKeyManager().GetProvider(connection.m_KeyName);
        if (!provider)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }

        if (provider->m_ApiKey.empty())
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' has no api_key — Redmine requires an API access key";
            return false;
        }

        // Redmine API key is sent via the X-Redmine-API-Key header (not Bearer). We store the
        // key in m_Token; the executor reads it directly and builds the X-Redmine-API-Key header.
        credentials.m_AuthType = CloudAuthType::BearerToken;
        credentials.m_Token = provider->m_ApiKey;
        return true;
    }

    bool RedmineConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        if (connection.m_Endpoint.empty())
        {
            errorMessage = "Redmine connection requires an endpoint (e.g. 'http://localhost:3000')";
            return false;
        }

        // Note: Redmine commonly runs on local-network hosts in dev (e.g.
        // http://localhost:3000) — ValidatePublicHttpEndpoint allows local-net
        // for the http scheme, blocks it for https.  Same posture as email.
        if (!ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint, errorMessage))
        {
            LOG_SECURITY_WARN("[security] redmine_endpoint_rejected connection='{}' reason='{}'",
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
            errorMessage = "Redmine API key contains CR/LF — refusing to send";
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] redmine_test_apikey_crlf_rejected connection='{}'", connection.m_Name);
            return false;
        }

        // GET /users/current.json -- verifies the API key and returns the current user
        std::string url = GetBaseUrl(connection) + "/users/current.json";

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
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        std::string apiKeyHeader = "X-Redmine-API-Key: " + credentials.m_Token;
        headers = curl_slist_append(headers, apiKeyHeader.c_str());
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("Redmine test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode == 401)
        {
            errorMessage = "Redmine test failed: unauthorized (HTTP 401) — check API key";
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "Redmine test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
