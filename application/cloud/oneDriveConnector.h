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
    // Microsoft OneDrive connector via Microsoft Graph API.
    //
    // CloudConnection.m_Params keys:
    //   "client_id"         — Azure AD application (client) ID, required
    //   "tenant_id"         — Azure AD tenant ID (default: "common" for multi-tenant)
    //   "scopes"            — OAuth scopes (default: "Files.ReadWrite offline_access")
    //
    // CloudConnection.m_Endpoint — Graph API base URL (default: "https://graph.microsoft.com/v1.0")
    // CloudConnection.m_KeyName  — KeyManager credential (OAuthCredential with tokens managed by OAuthTokenManager)
    // CloudConnection.m_AuthType — must be OAuth2
    class OneDriveConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        [[nodiscard]] std::expected<void, ConnectorError> TestConnection(CloudConnection const& connection) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;
        bool GetOAuth2ProviderInfo(CloudConnection const& connection,
                                   OAuth2ProviderInfo& info) const override;

        // Build the Graph API base URL from connection config.
        // Returns m_Endpoint if set, otherwise the default Graph API URL.
        static std::string GetGraphBaseUrl(CloudConnection const& connection);

        // Microsoft identity platform endpoints.
        static std::string GetAuthorizeUrl(std::string const& tenantId);
        static std::string GetTokenUrl(std::string const& tenantId);

        static constexpr char const* DEFAULT_GRAPH_BASE_URL = "https://graph.microsoft.com/v1.0";
        static constexpr char const* DEFAULT_SCOPES = "Files.ReadWrite offline_access";
        static constexpr char const* DEFAULT_TENANT_ID = "common";
    };
} // namespace AIAssistant
