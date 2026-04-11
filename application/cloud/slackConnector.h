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
    // Slack connector via the Slack Web API.
    //
    // CloudConnection.m_Endpoint — Slack API base URL (default: "https://slack.com/api")
    // CloudConnection.m_KeyName  — KeyManager credential (ApiKeyCredential with Bot token)
    // CloudConnection.m_AuthType — must be BearerToken
    class SlackConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        bool TestConnection(CloudConnection const& connection, std::string& errorMessage) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        static std::string GetApiBaseUrl(CloudConnection const& connection);

        static constexpr char const* DEFAULT_API_BASE_URL = "https://slack.com/api";
    };
} // namespace AIAssistant
