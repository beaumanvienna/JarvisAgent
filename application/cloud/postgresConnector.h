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
    // PostgreSQL database connector using the libpq C API.
    //
    // CloudConnection.m_Endpoint — connection string or host:port (e.g. "myhost:5432")
    // CloudConnection.m_KeyName  — KeyManager credential (BasicAuth: username/password)
    //
    // CloudConnection.m_Params keys:
    //   "database"   — database name (required)
    //   "sslmode"    — "disable", "require", "verify-ca", "verify-full" (optional, default "prefer")
    //
    // The connector builds a libpq connection string from these fields.
    class PostgresConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        bool TestConnection(CloudConnection const& connection, std::string& errorMessage) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        // Build a libpq connection string from connection config + credentials.
        static std::string BuildConnectionString(CloudConnection const& connection,
                                                  CloudCredentials const& credentials);
    };
} // namespace AIAssistant
