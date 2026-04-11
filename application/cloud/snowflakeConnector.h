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
    // Snowflake connector via Snowflake SQL REST API with RSA JWT authentication.
    //
    // CloudConnection.m_Params keys:
    //   "account"           — Snowflake account identifier (e.g. "xy12345"), required
    //   "user"              — Snowflake user name (e.g. "SVC_JARVIS"), required
    //   "warehouse"         — Default warehouse (e.g. "COMPUTE_WH")
    //   "database"          — Default database (e.g. "ANALYTICS")
    //   "schema"            — Default schema (e.g. "PUBLIC")
    //
    // CloudConnection.m_Endpoint — Snowflake account locator with region
    //   (e.g. "xy12345.us-east-1"). Used to construct the REST API URL.
    // CloudConnection.m_KeyName  — KeyManager credential (KeyPairCredential with RSA private key PEM)
    // CloudConnection.m_AuthType — must be JwtRsa
    class SnowflakeConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        bool TestConnection(CloudConnection const& connection, std::string& errorMessage) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        // Build the Snowflake SQL REST API base URL from the endpoint (account locator).
        // Returns "https://{endpoint}.snowflakecomputing.com"
        static std::string BuildApiBaseUrl(std::string const& endpoint);
    };
} // namespace AIAssistant
