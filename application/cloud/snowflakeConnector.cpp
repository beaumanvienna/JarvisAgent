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

#include "cloud/snowflakeConnector.h"
#include "cloud/connectorHttp.h"

#include <curl/curl.h>
#include <cctype>

#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "json/jsonHelper.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "keys/jwtGenerator.h"
#include "curlWrapper/curlSlistHelper.h"
#include "curlWrapper/curlWrapper.h"

namespace AIAssistant
{
    namespace
    {
        // Local response-body cap for the connector's TestConnection writeCallback.
        // Test connection responses are tiny (a single SELECT 1 result), so the
        // 1 MB cap is generous.  Set independently of the executor's larger cap
        // because the test connection's working set is much smaller.
        constexpr size_t kMaxConnectorResponseBytes = 1 * 1024 * 1024;
    } // namespace

    std::string SnowflakeConnector::GetType() const
    {
        return "snowflake";
    }

    std::string SnowflakeConnector::BuildApiBaseUrl(std::string const& endpoint)
    {
        std::string base = endpoint;
        // Strip trailing slashes
        while (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }
        // Snowflake account locator allowlist: alphanumeric + `.` + `-` + `_`.
        // Reject empty, oversized, scheme-prefixed (no `http://` or `https://`
        // — caller is supposed to pass an account locator like
        // `xy12345.us-east-1`, NOT a full URL), and any URL-meaningful or
        // protocol-injection chars.  This closes the SSRF vector where
        // `m_Endpoint = "http://evil.com/path?x="` would otherwise sail
        // through the previous "if it looks like a URL, use as-is" branch.
        if (base.empty() || base.size() > 128)
        {
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] snowflake_invalid_endpoint reason=size endpoint_length={}", endpoint.size());
            return {};
        }
        for (char c : base)
        {
            unsigned char const uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc))
            {
                continue;
            }
            if (c == '.' || c == '-' || c == '_')
            {
                continue;
            }
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] snowflake_invalid_endpoint reason=charset endpoint_length={}",
                              endpoint.size());
            return {};
        }
        return "https://" + base + ".snowflakecomputing.com";
    }

    bool SnowflakeConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                                std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Snowflake connection '" + connection.m_Name + "'";
            return false;
        }

        // Generate a Snowflake JWT (1-hour expiry).  PEM is materialised into a request-
        // scoped std::string for the JwtGenerator call; the SecureString remains intact in
        // KeyManager storage.
        std::string privateKeyPem;
        bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
            [&](ICredential const& cred)
            {
                auto const* keyPair = dynamic_cast<KeyPairCredential const*>(&cred);
                if (!keyPair)
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' must be KeyPairCredential — Snowflake requires a key_pair credential";
                    return;
                }
                if (keyPair->m_PrivateKeyPem.IsEmpty())
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' has no RSA private key — Snowflake requires a key_pair credential";
                    return;
                }
                privateKeyPem.assign(keyPair->m_PrivateKeyPem.Get());
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

        auto accountIt = connection.m_Params.find("account");
        if (accountIt == connection.m_Params.end() || accountIt->second.empty())
        {
            errorMessage = "Snowflake connection '" + connection.m_Name + "' requires 'account' parameter";
            return false;
        }

        auto userIt = connection.m_Params.find("user");
        if (userIt == connection.m_Params.end() || userIt->second.empty())
        {
            errorMessage = "Snowflake connection '" + connection.m_Name + "' requires 'user' parameter";
            return false;
        }

        credentials.m_AuthType = CloudAuthType::JwtRsa;
        if (!JwtGenerator::GenerateSnowflakeJwt(accountIt->second, userIt->second, privateKeyPem,
                                                  credentials.m_Token, errorMessage))
        {
            if (errorMessage.empty())
            {
                errorMessage = "Failed to generate Snowflake JWT for '" + connection.m_KeyName + "'";
            }
            return false;
        }
        return true;
    }

    std::expected<void, ConnectorError> SnowflakeConnector::TestConnection(CloudConnection const& connection)
    {
        if (connection.m_Endpoint.empty())
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::InvalidConfig,
                "Snowflake connection requires an endpoint (account locator, e.g. 'xy12345.us-east-1')"));
        }

        CloudCredentials credentials;
        std::string credErr;
        if (!ResolveCredentials(connection, credentials, credErr))
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::CredentialMissing, std::move(credErr)));
        }

        // POST /api/v2/statements with SELECT 1.  BuildApiBaseUrl validates the
        // endpoint allowlist and returns "" on rejection — already logged at
        // the helper.
        std::string apiBase = BuildApiBaseUrl(connection.m_Endpoint);
        if (apiBase.empty())
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::InvalidEndpoint,
                "Snowflake endpoint rejected: invalid account locator (see security log)"));
        }
        // Reject CR/LF in the JWT before splicing into the Authorization header
        // (parallel to the executor's check).
        if (credentials.m_Token.Get().find('\r') != std::string::npos ||
            credentials.m_Token.Get().find('\n') != std::string::npos)
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] snowflake_test_jwt_crlf_rejected connection='{}'", connection.m_Name);
            return std::unexpected(ConnectorError::Make(
                ConnectorErrorCode::CredentialInvalid, "Snowflake JWT contains CR/LF — refusing to send"));
        }
        std::string url = apiBase + "/api/v2/statements";

        // Build request body
        std::string requestBody = R"({"statement":"SELECT 1","timeout":10})";

        // Add warehouse/database/schema context if available
        auto warehouseIt = connection.m_Params.find("warehouse");
        auto databaseIt = connection.m_Params.find("database");
        auto schemaIt = connection.m_Params.find("schema");

        if (warehouseIt != connection.m_Params.end() || databaseIt != connection.m_Params.end() ||
            schemaIt != connection.m_Params.end())
        {
            requestBody = "{\"statement\":\"SELECT 1\",\"timeout\":10";
            // Escape warehouse/database/schema before splicing — same JSON injection
            // vector the executor closes.
            if (warehouseIt != connection.m_Params.end() && !warehouseIt->second.empty())
            {
                requestBody += ",\"warehouse\":\"" + JsonHelper::EscapeJsonString(warehouseIt->second) + "\"";
            }
            if (databaseIt != connection.m_Params.end() && !databaseIt->second.empty())
            {
                requestBody += ",\"database\":\"" + JsonHelper::EscapeJsonString(databaseIt->second) + "\"";
            }
            if (schemaIt != connection.m_Params.end() && !schemaIt->second.empty())
            {
                requestBody += ",\"schema\":\"" + JsonHelper::EscapeJsonString(schemaIt->second) + "\"";
            }
            requestBody += "}";
        }

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::NetworkError, "curl_easy_init() failed"));
        }

        std::string responseBody;
        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            size_t const incoming = size * nmemb;
            // Cap the response buffer (matches the executor's pattern; this
            // connector's responses are tiny, 1 MB is generous).
            if (buf->size() + incoming > kMaxConnectorResponseBytes)
            {
                return 0;
            }
            buf->append(static_cast<char*>(contents), incoming);
            return incoming;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        // Hardened TLS + no redirect-following + DNS post-resolve check —
        // Snowflake's API never legitimately redirects (hostile redirect →
        // attacker-controlled pivot) and is always reached via
        // `https://*.snowflakecomputing.com` (per BuildApiBaseUrl), so the
        // post-resolve callback installs and rejects any DNS-time SSRF.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        SecureString authScratch;
        AppendSecretHeader(headers, "Authorization: Bearer ", credentials.m_Token, authScratch);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "X-Snowflake-Authorization-Token-Type: KEYPAIR_JWT");
        headers = curl_slist_append(headers, "User-Agent: j9t/1.0");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::NetworkError,
                std::string("Snowflake test failed: ") + curl_easy_strerror(res)));
        }

        if (httpCode == 401 || httpCode == 403)
        {
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::AuthFailure,
                "Snowflake test failed: authentication error (HTTP " + std::to_string(httpCode) +
                    ") — check RSA key pair and user assignment"));
        }

        if (httpCode >= 400)
        {
            // Don't embed the raw response body in errorMessage.  Same rationale
            // as the executor's submit/poll error paths: Snowflake error responses
            // can include schema names + partial query data that shouldn't leak
            // into persisted error state.
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::HttpError,
                "Snowflake test failed: HTTP " + std::to_string(httpCode)));
        }

        return {};
    }
} // namespace AIAssistant
