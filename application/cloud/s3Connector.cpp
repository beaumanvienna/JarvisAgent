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
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "curlWrapper/curlWrapper.h"
#include "cloud/connectorHttp.h"

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

        credentials.m_AuthType = CloudAuthType::SigV4;

        // Support three storage conventions, in order of preference:
        // 1. AwsCredential — typed access_key_id + secret_access_key (the new path).
        // 2. BasicAuthCredential — username = access key ID, password = secret key (legacy).
        // 3. ApiKeyCredential — api_key = "ACCESS_KEY_ID:SECRET_KEY" colon-split (legacy).
        // Fail closed if none match.
        bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
            [&](ICredential const& cred)
            {
                if (auto const* aws = dynamic_cast<AwsCredential const*>(&cred))
                {
                    credentials.m_AccessKeyId = aws->m_AccessKeyId;
                    credentials.m_SecretKey   = std::string(aws->m_SecretAccessKey.Get());
                }
                else if (auto const* basic = dynamic_cast<BasicAuthCredential const*>(&cred))
                {
                    credentials.m_AccessKeyId = basic->m_Username;
                    credentials.m_SecretKey   = std::string(basic->m_Password.Get());
                }
                else if (auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred))
                {
                    std::string_view apiKeyView = api->m_ApiKey.Get();
                    size_t const colonPos = apiKeyView.find(':');
                    if (colonPos != std::string_view::npos && colonPos > 0 && colonPos < apiKeyView.size() - 1)
                    {
                        credentials.m_AccessKeyId = std::string(apiKeyView.substr(0, colonPos));
                        credentials.m_SecretKey   = std::string(apiKeyView.substr(colonPos + 1));
                    }
                    else
                    {
                        errorMessage = "Credential '" + connection.m_KeyName +
                                       "' api_key must be in 'ACCESS_KEY_ID:SECRET_KEY' format for S3";
                    }
                }
                else
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' must be AwsCredential, BasicAuthCredential, or "
                                   "ApiKeyCredential ('ID:SECRET' format)";
                }
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

        if (credentials.m_AccessKeyId.empty() || credentials.m_SecretKey.empty())
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' has empty access key or secret";
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

        if (!connection.m_Endpoint.empty() &&
            !ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint, errorMessage))
        {
            LOG_SECURITY_WARN("[security] s3_endpoint_rejected connection='{}' reason='{}'",
                              connection.m_Name, errorMessage);
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
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ConnectorHttp::BoundedStringWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

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
