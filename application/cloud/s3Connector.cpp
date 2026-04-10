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

#include "cloud/s3Connector.h"
#include "cloud/sigV4Signer.h"

#include <curl/curl.h>

#include "core.h"
#include "engine.h"
#include "keys/keyManager.h"
#include "curlWrapper/curlWrapper.h"

namespace AIAssistant
{
    std::string S3Connector::GetType() const
    {
        return "s3";
    }

    bool S3Connector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                         std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for S3 connection '" + connection.m_Name + "'";
            return false;
        }

        auto const* provider = Core::g_Core->GetKeyManager().GetProvider(connection.m_KeyName);
        if (!provider)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }

        credentials.m_AuthType = CloudAuthType::SigV4;

        // Support two storage conventions:
        // 1. BasicAuthCredential: username = access key ID, password = secret key
        // 2. ApiKeyCredential: api_key = "ACCESS_KEY_ID:SECRET_KEY"
        if (!provider->m_Username.empty() && !provider->m_Password.empty())
        {
            credentials.m_AccessKeyId = provider->m_Username;
            credentials.m_SecretKey = provider->m_Password;
        }
        else if (!provider->m_ApiKey.empty())
        {
            size_t colonPos = provider->m_ApiKey.find(':');
            if (colonPos != std::string::npos && colonPos > 0 && colonPos < provider->m_ApiKey.size() - 1)
            {
                credentials.m_AccessKeyId = provider->m_ApiKey.substr(0, colonPos);
                credentials.m_SecretKey = provider->m_ApiKey.substr(colonPos + 1);
            }
            else
            {
                errorMessage = "Credential '" + connection.m_KeyName +
                               "' api_key must be in 'ACCESS_KEY_ID:SECRET_KEY' format for S3";
                return false;
            }
        }
        else
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' has no access key — use username/password or api_key in 'ID:SECRET' format";
            return false;
        }

        return true;
    }

    std::string S3Connector::BuildEndpointUrl(CloudConnection const& connection, std::string const& bucket)
    {
        if (!connection.m_Endpoint.empty())
        {
            // Custom endpoint (MinIO, Wasabi, R2, etc.)
            std::string base = connection.m_Endpoint;
            if (!base.empty() && base.back() == '/')
            {
                base.pop_back();
            }
            return base + "/" + bucket;
        }

        // AWS default: https://{bucket}.s3.{region}.amazonaws.com
        auto regionIt = connection.m_Params.find("region");
        std::string region = (regionIt != connection.m_Params.end()) ? regionIt->second : "us-east-1";
        return "https://" + bucket + ".s3." + region + ".amazonaws.com";
    }

    bool S3Connector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        auto regionIt = connection.m_Params.find("region");
        if (regionIt == connection.m_Params.end() || regionIt->second.empty())
        {
            errorMessage = "S3 connection requires 'region' parameter";
            return false;
        }

        auto bucketIt = connection.m_Params.find("bucket");
        if (bucketIt == connection.m_Params.end() || bucketIt->second.empty())
        {
            errorMessage = "S3 connection requires 'bucket' parameter";
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        // List objects (max 1) to verify connectivity
        std::string endpointUrl = BuildEndpointUrl(connection, bucketIt->second);
        std::string url = endpointUrl + "/?list-type=2&max-keys=1";

        auto signed_ = SigV4Signer::Sign("GET", url, regionIt->second, "s3", credentials.m_AccessKeyId,
                                          credentials.m_SecretKey, SigV4Signer::EmptyPayloadHash());

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
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        struct curl_slist* headers = nullptr;
        for (auto const& [key, value] : signed_.m_Headers)
        {
            headers = curl_slist_append(headers, (key + ": " + value).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("S3 test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "S3 test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
