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

#include "cloud/gcsConnector.h"

#include <chrono>
#include <sstream>

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "keys/credential.h"
#include "keys/jwtGenerator.h"
#include "keys/keyManager.h"
#include "curlWrapper/curlWrapper.h"
#include "cloud/cloudTaskExecutor.h"
#include "cloud/connectorHttp.h"

namespace AIAssistant
{
    static constexpr char const* kGcsScope = "https://www.googleapis.com/auth/devstorage.read_write";
    static constexpr char const* kGoogleTokenUrl = "https://oauth2.googleapis.com/token";
    static constexpr char const* kDefaultGcsEndpoint = "https://storage.googleapis.com";
    static constexpr int kTokenLifetimeSeconds = 3600; // 1 hour
    static constexpr int kTokenRefreshMarginSeconds = 300; // refresh 5 min before expiry

    std::string GcsConnector::GetType() const
    {
        return "gcs";
    }

    std::string GcsConnector::BuildEndpointUrl(CloudConnection const& connection)
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
        return kDefaultGcsEndpoint;
    }

    bool GcsConnector::ExchangeJwtForAccessToken(std::string const& jwt, std::string const& endpoint,
                                                  std::string& accessToken, std::string& errorMessage)
    {
        // For fake-gcs-server (local testing), skip token exchange — use JWT directly as bearer token
        if (endpoint != kDefaultGcsEndpoint)
        {
            accessToken = jwt;
            return true;
        }

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        std::string postBody = "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + jwt;

        std::string responseBody;
        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            buf->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, kGoogleTokenUrl);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("Token exchange failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "Token exchange failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        // Parse access_token from response JSON
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(responseBody);
        simdjson::ondemand::document doc;

        if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
        {
            errorMessage = "Failed to parse token exchange response";
            return false;
        }

        std::string_view tokenSv;
        if (doc["access_token"].get_string().get(tokenSv) != simdjson::SUCCESS)
        {
            errorMessage = "Token exchange response missing 'access_token'";
            return false;
        }

        accessToken = std::string(tokenSv);
        return true;
    }

    bool GcsConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                           std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for GCS connection '" + connection.m_Name + "'";
            return false;
        }

        // Check token cache first
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            auto it = m_TokenCache.find(connection.m_KeyName);
            if (it != m_TokenCache.end())
            {
                auto now = std::chrono::steady_clock::now();
                if (now < it->second.m_ExpiresAt)
                {
                    credentials.m_AuthType = CloudAuthType::OAuth2;
                    credentials.m_Token = it->second.m_AccessToken;
                    return true;
                }
            }
        }

        // Mint a new JWT and exchange for access token
        auto const* cred = Core::g_Core->GetKeyManager().GetCredential(connection.m_KeyName);
        if (!cred)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }
        auto const* keyPair = dynamic_cast<KeyPairCredential const*>(cred);
        if (!keyPair)
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' must be KeyPairCredential — GCS requires a service-account key_pair credential";
            return false;
        }
        if (keyPair->m_PrivateKeyPem.IsEmpty())
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' has no private key PEM — GCS requires a KeyPairCredential with a service account key";
            return false;
        }
        // Materialise into a request-scoped std::string for JwtGenerator (request-bounded
        // plaintext; the SecureString remains intact in KeyManager storage).
        std::string const privateKeyPem(keyPair->m_PrivateKeyPem.Get());

        auto emailIt = connection.m_Params.find("service_account_email");
        if (emailIt == connection.m_Params.end() || emailIt->second.empty())
        {
            errorMessage = "GCS connection '" + connection.m_Name + "' missing 'service_account_email' parameter";
            return false;
        }
        std::string const& serviceAccountEmail = emailIt->second;

        // Build JWT claims for GCS — header is built internally by JwtGenerator::Generate.
        auto now = std::chrono::system_clock::now();
        int64_t iat = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        int64_t exp = iat + kTokenLifetimeSeconds;

        std::ostringstream payloadStream;
        payloadStream << R"({"iss":")" << serviceAccountEmail
                      << R"(","scope":")" << kGcsScope
                      << R"(","aud":")" << kGoogleTokenUrl
                      << R"(","iat":)" << iat
                      << R"(,"exp":)" << exp << "}";
        std::string payloadJson = payloadStream.str();

        std::string jwt = JwtGenerator::Generate(payloadJson, privateKeyPem, errorMessage);
        if (jwt.empty())
        {
            if (errorMessage.empty())
            {
                errorMessage = "Failed to generate JWT for GCS service account";
            }
            return false;
        }

        // Exchange JWT for access token
        std::string endpoint = BuildEndpointUrl(connection);
        std::string accessToken;
        if (!ExchangeJwtForAccessToken(jwt, endpoint, accessToken, errorMessage))
        {
            return false;
        }

        // Cache the token
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            CachedToken cached;
            cached.m_AccessToken = accessToken;
            cached.m_ExpiresAt = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(kTokenLifetimeSeconds - kTokenRefreshMarginSeconds);
            m_TokenCache[connection.m_KeyName] = std::move(cached);
        }

        credentials.m_AuthType = CloudAuthType::OAuth2;
        credentials.m_Token = std::move(accessToken);
        return true;
    }

    bool GcsConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        auto bucketIt = connection.m_Params.find("bucket");
        if (bucketIt == connection.m_Params.end() || bucketIt->second.empty())
        {
            errorMessage = "GCS connection requires 'bucket' parameter";
            return false;
        }

        auto emailIt = connection.m_Params.find("service_account_email");
        if (emailIt == connection.m_Params.end() || emailIt->second.empty())
        {
            errorMessage = "GCS connection requires 'service_account_email' parameter";
            return false;
        }

        if (!connection.m_Endpoint.empty() &&
            !ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint, errorMessage))
        {
            LOG_SECURITY_WARN("[security] gcs_endpoint_rejected connection='{}' reason='{}'",
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
            errorMessage = "GCS bearer token contains CR/LF — refusing to send";
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] gcs_test_bearer_crlf_rejected connection='{}'", connection.m_Name);
            return false;
        }

        // Test: GET /storage/v1/b/{bucket} — checks bucket exists and credentials work
        std::string endpointUrl = BuildEndpointUrl(connection);
        std::string url = endpointUrl + "/storage/v1/b/" + bucketIt->second;

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
        headers = curl_slist_append(headers, ("Authorization: Bearer " + credentials.m_Token).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("GCS test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "GCS test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
