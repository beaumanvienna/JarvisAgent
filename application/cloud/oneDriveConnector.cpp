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
#include "cloud/cloudTaskExecutor.h"
#include "cloud/connectorHttp.h"

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

    bool OneDriveConnector::GetOAuth2ProviderInfo(CloudConnection const& connection,
                                                  OAuth2ProviderInfo& info) const
    {
        auto tenantIdIt = connection.m_Params.find("tenant_id");
        std::string const tenantId =
            (tenantIdIt != connection.m_Params.end() && !tenantIdIt->second.empty()) ? tenantIdIt->second
                                                                                      : std::string(DEFAULT_TENANT_ID);
        info.m_AuthorizeUrl = GetAuthorizeUrl(tenantId);
        info.m_TokenUrl = GetTokenUrl(tenantId);
        info.m_DefaultScopes = DEFAULT_SCOPES;
        info.m_ExtraAuthorizeParams = {{"response_mode", "query"}};
        info.m_RequiresClientSecret = false; // PKCE public client
        return true;
    }

    std::expected<void, ConnectorError> OneDriveConnector::TestConnection(CloudConnection const& connection)
    {
        auto clientIdIt = connection.m_Params.find("client_id");
        if (clientIdIt == connection.m_Params.end() || clientIdIt->second.empty())
        {
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::InvalidConfig, "OneDrive connection requires 'client_id' parameter"));
        }

        if (!connection.m_Endpoint.empty())
        {
            if (auto r = ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint); !r)
            {
                LOG_SECURITY_WARN("[security] onedrive_endpoint_rejected connection='{}' reason='{}'",
                                  connection.m_Name, r.error().m_Details);
                return std::unexpected(std::move(r.error()));
            }
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
            LOG_SECURITY_WARN("[security] onedrive_test_bearer_crlf_rejected connection='{}'", connection.m_Name);
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::CredentialInvalid, "OneDrive bearer token contains CR/LF — refusing to send"));
        }

        // GET /me/drive — verifies token and returns drive info
        std::string url = GetGraphBaseUrl(connection) + "/me/drive";

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
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::NetworkError,
                std::string("OneDrive test failed: ") + curl_easy_strerror(res)));
        }

        if (httpCode == 401 || httpCode == 403)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::AuthFailure,
                "OneDrive test failed: HTTP " + std::to_string(httpCode) + " — token may be expired or invalid"));
        }

        if (httpCode >= 400)
        {
            std::string details = "OneDrive test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                details += ": " + responseBody;
            }
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::HttpError, std::move(details)));
        }

        return {};
    }
} // namespace AIAssistant
