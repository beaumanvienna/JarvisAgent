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

#include "cloud/jiraConnector.h"

#include <curl/curl.h>

#include "core.h"
#include "engine.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "curlWrapper/curlSlistHelper.h"
#include "curlWrapper/curlWrapper.h"
#include "cloud/cloudTaskExecutor.h"
#include "cloud/connectorHttp.h"

namespace AIAssistant
{
    std::string JiraConnector::GetType() const
    {
        return "jira";
    }

    std::string JiraConnector::GetBaseUrl(CloudConnection const& connection)
    {
        std::string base = connection.m_Endpoint;
        if (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }
        return base;
    }

    bool JiraConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                           std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Jira connection '" + connection.m_Name + "'";
            return false;
        }

        // Jira Cloud uses email + API token (BasicAuthCredential); Jira Data Center uses
        // a Personal Access Token (ApiKeyCredential).  Both shapes are valid here — try
        // the type-specific casts in order, fail closed if neither matches.
        bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
            [&](ICredential const& cred)
            {
                if (auto const* basic = dynamic_cast<BasicAuthCredential const*>(&cred))
                {
                    credentials.m_AuthType = CloudAuthType::BasicAuth;
                    credentials.m_Username = basic->m_Username;
                    credentials.m_Password.Set(basic->m_Password.Get());
                }
                else if (auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred))
                {
                    credentials.m_AuthType = CloudAuthType::BearerToken;
                    credentials.m_Token.Set(api->m_ApiKey.Get());
                }
                else
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' must be BasicAuthCredential (Jira Cloud) or "
                                   "ApiKeyCredential (Jira Data Center)";
                }
            });
        if (!found)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }
        return errorMessage.empty();
    }

    std::expected<void, ConnectorError> JiraConnector::TestConnection(CloudConnection const& connection)
    {
        if (connection.m_Endpoint.empty())
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::InvalidConfig,
                "Jira connection requires an endpoint (e.g. 'https://mycompany.atlassian.net')"));
        }

        if (auto r = ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint); !r)
        {
            LOG_SECURITY_WARN("[security] jira_endpoint_rejected connection='{}' reason='{}'",
                              connection.m_Name, r.error().m_Details);
            return std::unexpected(std::move(r.error()));
        }

        CloudCredentials credentials;
        std::string credErr;
        if (!ResolveCredentials(connection, credentials, credErr))
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::CredentialMissing, std::move(credErr)));
        }

        if (credentials.m_AuthType != CloudAuthType::BasicAuth &&
            ICloudTaskExecutor::ContainsCrlf(credentials.m_Token.Get()))
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] jira_test_bearer_crlf_rejected connection='{}'", connection.m_Name);
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::CredentialInvalid, "Jira bearer token contains CR/LF — refusing to send"));
        }

        // GET /rest/api/3/myself — verifies credentials
        std::string url = GetBaseUrl(connection) + "/rest/api/3/myself";

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
        if (credentials.m_AuthType == CloudAuthType::BasicAuth)
        {
            curl_easy_setopt(curl, CURLOPT_USERNAME, credentials.m_Username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, credentials.m_Password.CStr());
        }
        else
        {
            SecureString authScratch;
            AppendSecretHeader(headers, "Authorization: Bearer ", credentials.m_Token, authScratch);
        }
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
                std::string("Jira test failed: ") + curl_easy_strerror(res)));
        }

        if (httpCode == 401 || httpCode == 403)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::AuthFailure,
                "Jira test failed: HTTP " + std::to_string(httpCode) + " — check credentials"));
        }

        if (httpCode >= 400)
        {
            std::string details = "Jira test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                details += ": " + responseBody;
            }
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::HttpError, std::move(details)));
        }

        return {};
    }
} // namespace AIAssistant
