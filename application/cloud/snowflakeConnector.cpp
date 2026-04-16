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

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "keys/keyManager.h"
#include "keys/jwtGenerator.h"
#include "curlWrapper/curlWrapper.h"

namespace AIAssistant
{
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
        // If it already looks like a full URL, use as-is
        if (base.find("https://") == 0 || base.find("http://") == 0)
        {
            return base;
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

        auto const* provider = Core::g_Core->GetKeyManager().GetProvider(connection.m_KeyName);
        if (!provider)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }

        if (provider->m_PrivateKeyPem.empty())
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' has no RSA private key — Snowflake requires a key_pair credential";
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

        // Generate a Snowflake JWT (1-hour expiry)
        std::string jwt =
            JwtGenerator::GenerateSnowflakeJwt(accountIt->second, userIt->second, provider->m_PrivateKeyPem, errorMessage);
        if (jwt.empty())
        {
            if (errorMessage.empty())
            {
                errorMessage = "Failed to generate Snowflake JWT for '" + connection.m_KeyName + "'";
            }
            return false;
        }

        credentials.m_AuthType = CloudAuthType::JwtRsa;
        credentials.m_Token = std::move(jwt);
        return true;
    }

    bool SnowflakeConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        if (connection.m_Endpoint.empty())
        {
            errorMessage = "Snowflake connection requires an endpoint (account locator, e.g. 'xy12345.us-east-1')";
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        // POST /api/v2/statements with SELECT 1
        std::string apiBase = BuildApiBaseUrl(connection.m_Endpoint);
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
            if (warehouseIt != connection.m_Params.end() && !warehouseIt->second.empty())
            {
                requestBody += ",\"warehouse\":\"" + warehouseIt->second + "\"";
            }
            if (databaseIt != connection.m_Params.end() && !databaseIt->second.empty())
            {
                requestBody += ",\"database\":\"" + databaseIt->second + "\"";
            }
            if (schemaIt != connection.m_Params.end() && !schemaIt->second.empty())
            {
                requestBody += ",\"schema\":\"" + schemaIt->second + "\"";
            }
            requestBody += "}";
        }

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        std::string responseBody;
        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            buf->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + credentials.m_Token;
        headers = curl_slist_append(headers, authHeader.c_str());
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
            errorMessage = std::string("Snowflake test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode == 401 || httpCode == 403)
        {
            errorMessage = "Snowflake test failed: authentication error (HTTP " + std::to_string(httpCode) +
                           ") — check RSA key pair and user assignment";
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "Snowflake test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
