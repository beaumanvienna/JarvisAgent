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
    // Polarion ALM connector — wraps the existing PolarionClient for the cloud abstraction.
    //
    // CloudConnection.m_Params keys:
    //   "project_id" — Polarion project ID (required)
    //
    // CloudConnection.m_Endpoint — Polarion server base URL (e.g. "https://polarion.example.com")
    // CloudConnection.m_KeyName  — KeyManager credential holding the PAT (api_key type)
    class PolarionConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        [[nodiscard]] std::expected<void, ConnectorError> TestConnection(CloudConnection const& connection) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;
    };
} // namespace AIAssistant
