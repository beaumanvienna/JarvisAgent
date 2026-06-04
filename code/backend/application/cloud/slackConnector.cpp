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

#include "cloud/slackConnector.h"

#include <curl/curl.h>

#include "simdjson/simdjson.h"

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
    std::string SlackConnector::GetType() const
    {
        return "slack";
    }

    std::string SlackConnector::GetApiBaseUrl(CloudConnection const& connection)
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

    bool SlackConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                            std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Slack connection '" + connection.m_Name + "'";
            return false;
        }

        bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
            [&](ICredential const& cred)
            {
                auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred);
                if (!api)
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' must be ApiKeyCredential — Slack requires a Bot token (xoxb-...)";
                    return;
                }
                if (api->m_ApiKey.IsEmpty())
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' has no API key — Slack requires a Bot token (xoxb-...)";
                    return;
                }
                credentials.m_AuthType = CloudAuthType::BearerToken;
                credentials.m_Token.Set(api->m_ApiKey.Get());
            });
        if (!found)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }
        return errorMessage.empty();
    }

    std::expected<void, ConnectorError> SlackConnector::TestConnection(CloudConnection const& connection)
    {
        // Validate user-supplied endpoint override (if any) before any credential
        // resolution / network I/O.  Default endpoint is the trusted vendor URL.
        if (!connection.m_Endpoint.empty())
        {
            if (auto r = ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint); !r)
            {
                LOG_SECURITY_WARN("[security] slack_endpoint_rejected connection='{}' reason='{}'",
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

        // Reject CR/LF in bearer token before splicing into the Authorization header.
        if (ICloudTaskExecutor::ContainsCrlf(credentials.m_Token.Get()))
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] slack_test_bearer_crlf_rejected connection='{}'", connection.m_Name);
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::CredentialInvalid, "Slack bearer token contains CR/LF — refusing to send"));
        }

        // POST /api/auth.test — verifies token and returns workspace info
        std::string url = GetApiBaseUrl(connection) + "/auth.test";

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::NetworkError, "curl_easy_init() failed"));
        }

        std::string responseBody;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ConnectorHttp::BoundedStringWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        SecureString authScratch;
        AppendSecretHeader(headers, "Authorization: Bearer ", credentials.m_Token, authScratch);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::NetworkError,
                std::string("Slack test failed: ") + curl_easy_strerror(res)));
        }

        if (httpCode == 401 || httpCode == 403)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::AuthFailure,
                "Slack test failed: HTTP " + std::to_string(httpCode)));
        }

        if (httpCode >= 400)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::HttpError,
                "Slack test failed: HTTP " + std::to_string(httpCode)));
        }

        // Slack returns {"ok": true/false} even on HTTP 200
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(responseBody);
        simdjson::ondemand::document doc;

        if (parser.iterate(paddedJson).get(doc))
        {
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::HttpError, "Slack test: failed to parse response"));
        }

        bool ok = false;
        if (doc["ok"].get_bool().get(ok) || !ok)
        {
            std::string_view slackError;
            std::string details = "Slack auth.test failed: ";
            if (!doc["error"].get_string().get(slackError))
            {
                details += std::string(slackError);
            }
            else
            {
                details += "invalid token";
            }
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::AuthFailure, std::move(details)));
        }

        return {};
    }
} // namespace AIAssistant
