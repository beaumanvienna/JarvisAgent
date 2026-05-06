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

#include "cloud/googleSheetsConnector.h"

#include <curl/curl.h>

#include "core.h"
#include "engine.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "keys/oauthTokenManager.h"
#include "curlWrapper/curlWrapper.h"
#include "cloud/cloudTaskExecutor.h"
#include "cloud/connectorHttp.h"

namespace AIAssistant
{
    std::string GoogleSheetsConnector::GetType() const
    {
        return "google_sheets";
    }

    std::string GoogleSheetsConnector::GetApiBaseUrl(CloudConnection const& connection)
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

    bool GoogleSheetsConnector::GetOAuth2ProviderInfo(CloudConnection const& connection,
                                                      OAuth2ProviderInfo& info) const
    {
        (void)connection;
        info.m_AuthorizeUrl = "https://accounts.google.com/o/oauth2/v2/auth";
        info.m_TokenUrl = "https://oauth2.googleapis.com/token";
        info.m_DefaultScopes = DEFAULT_OAUTH_SCOPES;
        // access_type=offline + prompt=consent ensures Google returns a refresh_token even
        // when the user has previously authorized the app.  Without this Google only returns
        // a short-lived access_token on subsequent consents.
        info.m_ExtraAuthorizeParams = {{"access_type", "offline"}, {"prompt", "consent"},
                                        {"include_granted_scopes", "true"}};
        info.m_RequiresClientSecret = true; // Google Web clients require client_secret for token exchange
        return true;
    }

    bool GoogleSheetsConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                                   std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Google Sheets connection '" + connection.m_Name + "'";
            return false;
        }

        // Try OAuth2 first (for private sheets)
        if (connection.m_AuthType == CloudAuthType::OAuth2)
        {
            auto& oauthManager = Core::g_Core->GetOAuthTokenManager();
            std::string accessToken = oauthManager.GetAccessToken(connection.m_KeyName, errorMessage);
            if (!accessToken.empty())
            {
                credentials.m_AuthType = CloudAuthType::OAuth2;
                credentials.m_Token = std::move(accessToken);
                return true;
            }
            // Fall through to API key check if OAuth not configured
            errorMessage.clear();
        }

        // API key auth (for public sheets, read-only)
        auto const* cred = Core::g_Core->GetKeyManager().GetCredential(connection.m_KeyName);
        if (!cred)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }
        auto const* api = dynamic_cast<ApiKeyCredential const*>(cred);
        if (api && !api->m_ApiKey.IsEmpty())
        {
            credentials.m_AuthType = CloudAuthType::BearerToken;
            credentials.m_Token = std::string(api->m_ApiKey.Get());
            return true;
        }

        errorMessage = "Credential '" + connection.m_KeyName +
                       "' has no API key or OAuth tokens — Google Sheets requires an API key or OAuth2";
        return false;
    }

    bool GoogleSheetsConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        auto spreadsheetIdIt = connection.m_Params.find("spreadsheet_id");
        if (spreadsheetIdIt == connection.m_Params.end() || spreadsheetIdIt->second.empty())
        {
            errorMessage = "Google Sheets connection requires 'spreadsheet_id' parameter";
            return false;
        }

        if (!connection.m_Endpoint.empty() &&
            !ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint, errorMessage))
        {
            LOG_SECURITY_WARN("[security] google_sheets_endpoint_rejected connection='{}' reason='{}'",
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
            errorMessage = "Google Sheets credential contains CR/LF — refusing to send";
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] google_sheets_test_token_crlf_rejected connection='{}'", connection.m_Name);
            return false;
        }

        // GET /{spreadsheet_id}?fields=properties.title
        std::string apiBase = GetApiBaseUrl(connection);
        std::string url = apiBase + "/" + spreadsheetIdIt->second + "?fields=properties.title";

        // For API key auth, append as query parameter instead of Bearer header
        bool useApiKeyParam = (credentials.m_AuthType == CloudAuthType::BearerToken &&
                               connection.m_AuthType != CloudAuthType::OAuth2);
        if (useApiKeyParam)
        {
            url += "&key=" + credentials.m_Token;
        }

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
        if (!useApiKeyParam)
        {
            std::string authHeader = "Authorization: Bearer " + credentials.m_Token;
            headers = curl_slist_append(headers, authHeader.c_str());
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
            errorMessage = std::string("Google Sheets test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "Google Sheets test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
