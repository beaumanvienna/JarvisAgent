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

#pragma once

#include "cloud/cloudConnector.h"

namespace AIAssistant
{
    // Azure Blob Storage connector.
    //
    // CloudConnection.m_Params keys:
    //   "account_name"      — Azure Storage account name (required)
    //   "container"         — default blob container (can be overridden per-task)
    //
    // CloudConnection.m_Endpoint — Azure Blob endpoint URL.
    //   Production: "https://{account_name}.blob.core.windows.net"
    //   Azurite:    "http://127.0.0.1:10000/devstoreaccount1"
    // CloudConnection.m_KeyName  — KeyManager credential:
    //   - For Shared Key auth: api_key = Base64-encoded account key
    //   - For OAuth2 auth: OAuthCredential with Azure AD tokens
    // CloudConnection.m_AuthType — AzureSharedKey or OAuth2
    class AzureBlobConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        bool TestConnection(CloudConnection const& connection, std::string& errorMessage) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        // Build the Azure Blob endpoint URL for a given account.
        // If connection has a custom endpoint, uses that; otherwise constructs the default.
        static std::string BuildEndpointUrl(CloudConnection const& connection);
    };
} // namespace AIAssistant
