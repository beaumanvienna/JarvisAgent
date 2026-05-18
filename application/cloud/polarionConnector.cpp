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

#include "cloud/polarionConnector.h"
#include "workflow/filter/polarionClient.h"

#include "core.h"
#include "engine.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "cloud/cloudTaskExecutor.h"
#include "cloud/connectorHttp.h"

namespace AIAssistant
{
    std::string PolarionConnector::GetType() const
    {
        return "polarion";
    }

    bool PolarionConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        if (connection.m_Endpoint.empty())
        {
            errorMessage = "Polarion endpoint URL is required";
            return false;
        }

        if (!ConnectorHttp::ValidatePublicHttpEndpoint(connection.m_Endpoint, errorMessage))
        {
            LOG_SECURITY_WARN("[security] polarion_endpoint_rejected connection='{}' reason='{}'",
                              connection.m_Name, errorMessage);
            return false;
        }

        auto it = connection.m_Params.find("project_id");
        if (it == connection.m_Params.end() || it->second.empty())
        {
            errorMessage = "Polarion project_id is required";
            return false;
        }

        // Resolve credentials to verify the key is valid
        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        if (ICloudTaskExecutor::ContainsCrlf(credentials.m_Token))
        {
            errorMessage = "Polarion PAT contains CR/LF — refusing to send";
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] polarion_test_pat_crlf_rejected connection='{}'", connection.m_Name);
            return false;
        }

        // Perform a minimal query (page 1, size 1) to validate connectivity
        PolarionClient client;
        std::string responseBody;
        std::string url = connection.m_Endpoint;
        if (!url.empty() && url.back() == '/')
        {
            url.pop_back();
        }
        url += "/rest/v1/projects/" + PolarionClient::UrlEncode(it->second) +
               "/workitems?query=" + PolarionClient::UrlEncode("type:requirement") +
               "&page%5Bsize%5D=1&page%5Bnumber%5D=1";

        if (!client.HttpGet(url, credentials.m_Token, responseBody, errorMessage))
        {
            errorMessage = "Polarion connectivity test failed: " + errorMessage;
            return false;
        }

        return true;
    }

    bool PolarionConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                               std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Polarion connection '" + connection.m_Name + "'";
            return false;
        }

        bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
            [&](ICredential const& cred)
            {
                auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred);
                if (!api)
                {
                    errorMessage = "Credential '" + connection.m_KeyName +
                                   "' must be ApiKeyCredential — Polarion requires a Personal Access Token";
                    return;
                }
                if (api->m_ApiKey.IsEmpty())
                {
                    errorMessage = "Credential '" + connection.m_KeyName + "' has no API key (PAT)";
                    return;
                }
                credentials.m_AuthType = CloudAuthType::BearerToken;
                credentials.m_Token = std::string(api->m_ApiKey.Get());
            });
        if (!found)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }
        return errorMessage.empty();
    }
} // namespace AIAssistant
