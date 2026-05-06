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

        auto const* cred = Core::g_Core->GetKeyManager().GetCredential(connection.m_KeyName);
        if (!cred)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }
        auto const* api = dynamic_cast<ApiKeyCredential const*>(cred);
        if (!api)
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' must be ApiKeyCredential — Slack requires a Bot token (xoxb-...)";
            return false;
        }
        if (api->m_ApiKey.IsEmpty())
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' has no API key — Slack requires a Bot token (xoxb-...)";
            return false;
        }

        credentials.m_AuthType = CloudAuthType::BearerToken;
        credentials.m_Token = std::string(api->m_ApiKey.Get());
        return true;
    }

    bool SlackConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        // Validate user-supplied endpoint override (if any) before any credential
        // resolution / network I/O.  Default endpoint is the trusted vendor URL.
        if (!connection.m_Endpoint.empty() &&
            !ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint, errorMessage))
        {
            LOG_SECURITY_WARN("[security] slack_endpoint_rejected connection='{}' reason='{}'",
                              connection.m_Name, errorMessage);
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        // Reject CR/LF in bearer token before splicing into the Authorization header.
        if (ICloudTaskExecutor::ContainsCrlf(credentials.m_Token))
        {
            errorMessage = "Slack bearer token contains CR/LF — refusing to send";
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] slack_test_bearer_crlf_rejected connection='{}'", connection.m_Name);
            return false;
        }

        // POST /api/auth.test — verifies token and returns workspace info
        std::string url = GetApiBaseUrl(connection) + "/auth.test";

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
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
        std::string authHeader = "Authorization: Bearer " + credentials.m_Token;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("Slack test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "Slack test failed: HTTP " + std::to_string(httpCode);
            return false;
        }

        // Slack returns {"ok": true/false} even on HTTP 200
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(responseBody);
        simdjson::ondemand::document doc;

        if (parser.iterate(paddedJson).get(doc))
        {
            errorMessage = "Slack test: failed to parse response";
            return false;
        }

        bool ok = false;
        if (doc["ok"].get_bool().get(ok) || !ok)
        {
            std::string_view slackError;
            if (!doc["error"].get_string().get(slackError))
            {
                errorMessage = "Slack auth.test failed: " + std::string(slackError);
            }
            else
            {
                errorMessage = "Slack auth.test failed: invalid token";
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
