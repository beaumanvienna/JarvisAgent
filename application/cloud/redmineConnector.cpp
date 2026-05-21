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
#include "keys/credential.h"
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

        // Redmine API key is sent via the X-Redmine-API-Key header (not Bearer). We store the
        // key in m_Token; the executor reads it directly and builds the X-Redmine-API-Key header.
        bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
            [&](ICredential const& cred)
            {
                auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred);
                if (!api)
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' must be ApiKeyCredential — Redmine requires an API access key";
                    return;
                }
                if (api->m_ApiKey.IsEmpty())
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' has no api_key — Redmine requires an API access key";
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
        return errorMessage.empty();
    }

    std::expected<void, ConnectorError> RedmineConnector::TestConnection(CloudConnection const& connection)
    {
        if (connection.m_Endpoint.empty())
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::InvalidConfig,
                "Redmine connection requires an endpoint (e.g. 'http://localhost:3000')"));
        }

        // Note: Redmine commonly runs on local-network hosts in dev (e.g.
        // http://localhost:3000) — ValidatePublicHttpEndpoint allows local-net
        // for the http scheme, blocks it for https.  Same posture as email.
        if (auto r = ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint); !r)
        {
            LOG_SECURITY_WARN("[security] redmine_endpoint_rejected connection='{}' reason='{}'",
                              connection.m_Name, r.error().m_Details);
            return std::unexpected(std::move(r.error()));
        }

        CloudCredentials credentials;
        std::string credErr;
        if (!ResolveCredentials(connection, credentials, credErr))
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::CredentialMissing, std::move(credErr)));
        }

        if (ICloudTaskExecutor::ContainsCrlf(credentials.m_Token))
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] redmine_test_apikey_crlf_rejected connection='{}'", connection.m_Name);
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::CredentialInvalid, "Redmine API key contains CR/LF — refusing to send"));
        }

        // GET /users/current.json -- verifies the API key and returns the current user
        std::string url = GetBaseUrl(connection) + "/users/current.json";

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::NetworkError, "curl_easy_init() failed"));
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
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::NetworkError,
                std::string("Redmine test failed: ") + curl_easy_strerror(res)));
        }

        if (httpCode == 401 || httpCode == 403)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::AuthFailure,
                "Redmine test failed: HTTP " + std::to_string(httpCode) + " — check API key"));
        }

        if (httpCode >= 400)
        {
            std::string details = "Redmine test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                details += ": " + responseBody;
            }
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::HttpError, std::move(details)));
        }

        return {};
    }
} // namespace AIAssistant
