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
#include "curlWrapper/curlSlistHelper.h"
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
            if (oauthManager.GetAccessToken(connection.m_KeyName, credentials.m_Token, errorMessage))
            {
                credentials.m_AuthType = CloudAuthType::OAuth2;
                return true;
            }
            // Fall through to API key check if OAuth not configured.  credentials.m_Token
            // may have been touched by an early failure path inside GetAccessToken — clear
            // it defensively so the API-key branch starts with a clean SecureString.
            credentials.m_Token.Clear();
            errorMessage.clear();
        }

        // API key auth (for public sheets, read-only)
        bool resolved = false;
        bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
            [&](ICredential const& cred)
            {
                auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred);
                if (api && !api->m_ApiKey.IsEmpty())
                {
                    credentials.m_AuthType = CloudAuthType::BearerToken;
                    credentials.m_Token.Set(api->m_ApiKey.Get());
                    resolved = true;
                }
            });
        if (!found)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }
        if (resolved)
        {
            return true;
        }

        errorMessage = "Credential '" + connection.m_KeyName +
                       "' has no API key or OAuth tokens — Google Sheets requires an API key or OAuth2";
        return false;
    }

    std::expected<void, ConnectorError> GoogleSheetsConnector::TestConnection(CloudConnection const& connection)
    {
        auto spreadsheetIdIt = connection.m_Params.find("spreadsheet_id");
        if (spreadsheetIdIt == connection.m_Params.end() || spreadsheetIdIt->second.empty())
        {
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::InvalidConfig, "Google Sheets connection requires 'spreadsheet_id' parameter"));
        }

        if (!connection.m_Endpoint.empty())
        {
            if (auto r = ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint); !r)
            {
                LOG_SECURITY_WARN("[security] google_sheets_endpoint_rejected connection='{}' reason='{}'",
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

        if (ICloudTaskExecutor::ContainsCrlf(credentials.m_Token.Get()))
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] google_sheets_test_token_crlf_rejected connection='{}'", connection.m_Name);
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::CredentialInvalid, "Google Sheets credential contains CR/LF — refusing to send"));
        }

        // GET /{spreadsheet_id}?fields=properties.title
        std::string apiBase = GetApiBaseUrl(connection);
        std::string url = apiBase + "/" + spreadsheetIdIt->second + "?fields=properties.title";

        // API-key auth uses the X-Goog-Api-Key HTTP header (semantically equivalent
        // to a ?key=<token> URL parameter, but the header form keeps the secret out
        // of the URL); OAuth uses Authorization: Bearer.  Both flow through
        // AppendSecretHeader so the secret bytes never appear in a std::string heap
        // allocation between SecureString and curl_slist_append.
        bool const useApiKeyHeader = (credentials.m_AuthType == CloudAuthType::BearerToken &&
                                      connection.m_AuthType != CloudAuthType::OAuth2);

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
        SecureString authScratch;
        if (useApiKeyHeader)
        {
            AppendSecretHeader(headers, "X-Goog-Api-Key: ", credentials.m_Token, authScratch);
        }
        else
        {
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
                std::string("Google Sheets test failed: ") + curl_easy_strerror(res)));
        }

        if (httpCode == 401 || httpCode == 403)
        {
            std::string details = "Google Sheets test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                details += ": " + responseBody;
            }
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::AuthFailure, std::move(details)));
        }

        if (httpCode >= 400)
        {
            std::string details = "Google Sheets test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                details += ": " + responseBody;
            }
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::HttpError, std::move(details)));
        }

        return {};
    }
} // namespace AIAssistant
