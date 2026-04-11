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
    // Google Sheets connector via Google Sheets API v4.
    //
    // CloudConnection.m_Params keys:
    //   "spreadsheet_id"  — default Google Sheets spreadsheet ID
    //
    // CloudConnection.m_Endpoint — Sheets API base URL
    //   (default: "https://sheets.googleapis.com/v4/spreadsheets")
    // CloudConnection.m_KeyName  — KeyManager credential
    //   ApiKeyCredential (read-only public sheets, sent as ?key= query param)
    //   or OAuth2 via OAuthTokenManager (read/write private sheets)
    // CloudConnection.m_AuthType — BearerToken (API key) or OAuth2
    class GoogleSheetsConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        bool TestConnection(CloudConnection const& connection, std::string& errorMessage) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        static std::string GetApiBaseUrl(CloudConnection const& connection);

        static constexpr char const* DEFAULT_API_BASE_URL = "https://sheets.googleapis.com/v4/spreadsheets";
    };
} // namespace AIAssistant
