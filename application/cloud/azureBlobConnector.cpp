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

#include "cloud/azureBlobConnector.h"
#include "cloud/azureSharedKeySigner.h"

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
    std::string AzureBlobConnector::GetType() const
    {
        return "azure_blob";
    }

    bool AzureBlobConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                                std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Azure Blob connection '" + connection.m_Name + "'";
            return false;
        }

        if (connection.m_AuthType == CloudAuthType::OAuth2)
        {
            // Azure AD OAuth2 — use OAuthTokenManager
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

        // Default: Azure Shared Key auth
        auto const* provider = Core::g_Core->GetKeyManager().GetProvider(connection.m_KeyName);
        if (!provider)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }

        credentials.m_AuthType = CloudAuthType::AzureSharedKey;

        // The account key is stored as api_key (Base64-encoded Azure Storage key)
        if (!provider->m_ApiKey.empty())
        {
            credentials.m_SecretKey = provider->m_ApiKey;
        }
        else if (!provider->m_Password.empty())
        {
            // Also support BasicAuth: username = account_name, password = account key
            credentials.m_SecretKey = provider->m_Password;
        }
        else
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' has no account key — use api_key (Base64 account key) or username/password";
            return false;
        }

        // Account name from connection params
        auto accountIt = connection.m_Params.find("account_name");
        if (accountIt != connection.m_Params.end())
        {
            credentials.m_AccessKeyId = accountIt->second;
        }
        else
        {
            errorMessage = "Azure Blob connection '" + connection.m_Name + "' missing 'account_name' parameter";
            return false;
        }

        return true;
    }

    std::string AzureBlobConnector::BuildEndpointUrl(CloudConnection const& connection)
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

        auto accountIt = connection.m_Params.find("account_name");
        std::string accountName = (accountIt != connection.m_Params.end()) ? accountIt->second : "unknown";
        return "https://" + accountName + ".blob.core.windows.net";
    }

    bool AzureBlobConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        auto accountIt = connection.m_Params.find("account_name");
        if (accountIt == connection.m_Params.end() || accountIt->second.empty())
        {
            errorMessage = "Azure Blob connection requires 'account_name' parameter";
            return false;
        }

        auto containerIt = connection.m_Params.find("container");
        if (containerIt == connection.m_Params.end() || containerIt->second.empty())
        {
            errorMessage = "Azure Blob connection requires 'container' parameter";
            return false;
        }

        if (!connection.m_Endpoint.empty() &&
            !ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint, errorMessage))
        {
            LOG_SECURITY_WARN("[security] azure_blob_endpoint_rejected connection='{}' reason='{}'",
                              connection.m_Name, errorMessage);
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        if (credentials.m_AuthType == CloudAuthType::OAuth2 &&
            ICloudTaskExecutor::ContainsCrlf(credentials.m_Token))
        {
            errorMessage = "Azure Blob bearer token contains CR/LF — refusing to send";
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] azure_blob_test_bearer_crlf_rejected connection='{}'", connection.m_Name);
            return false;
        }

        // Test: GET /{container}?restype=container — checks container exists and credentials work
        std::string endpointUrl = BuildEndpointUrl(connection);
        std::string url = endpointUrl + "/" + containerIt->second + "?restype=container";

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

        if (credentials.m_AuthType == CloudAuthType::AzureSharedKey)
        {
            auto signed_ = AzureSharedKeySigner::Sign("GET", url, credentials.m_AccessKeyId, credentials.m_SecretKey);
            for (auto const& [key, value] : signed_.m_Headers)
            {
                headers = curl_slist_append(headers, (key + ": " + value).c_str());
            }
        }
        else if (credentials.m_AuthType == CloudAuthType::OAuth2)
        {
            headers = curl_slist_append(headers, ("Authorization: Bearer " + credentials.m_Token).c_str());
            headers = curl_slist_append(headers, "x-ms-version: 2024-11-04");
        }

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("Azure Blob test failed: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "Azure Blob test failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                errorMessage += ": " + responseBody;
            }
            return false;
        }

        return true;
    }
} // namespace AIAssistant
