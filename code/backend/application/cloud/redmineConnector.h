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
    // Redmine connector via Redmine REST API.
    //
    // CloudConnection.m_Endpoint -- Redmine instance URL (e.g. "http://localhost:3000")
    // CloudConnection.m_KeyName  -- KeyManager credential holding the Redmine API key
    //                               (the value of the "API access key" field on a user's My Account page)
    // CloudConnection.m_AuthType -- ApiKey (Redmine sends key via X-Redmine-API-Key header)
    //
    // CloudConnection.m_Params keys:
    //   "project_identifier" -- default project identifier (e.g. "j9t-demo"), used by list operations
    class RedmineConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        [[nodiscard]] std::expected<void, ConnectorError> TestConnection(CloudConnection const& connection) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        static std::string GetBaseUrl(CloudConnection const& connection);
    };
} // namespace AIAssistant
