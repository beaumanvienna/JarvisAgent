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
    // S3-compatible object storage connector.
    //
    // CloudConnection.m_Params keys:
    //   "region"            — AWS region (e.g. "us-east-1"), required
    //   "bucket"            — default bucket name (can be overridden per-task)
    //
    // CloudConnection.m_Endpoint — S3 endpoint URL override for S3-compatible services
    //   (MinIO, Wasabi, R2, etc.). Empty = AWS S3 default endpoint.
    // CloudConnection.m_KeyName  — KeyManager credential with access_key_id and secret_key
    //   (stored as username/password in BasicAuthCredential or as api_key in "access_key_id:secret_key" format)
    // CloudConnection.m_AuthType — must be SigV4
    class S3Connector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        bool TestConnection(CloudConnection const& connection, std::string& errorMessage) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        // Build the S3 endpoint URL for a given bucket and region.
        // If connection has a custom endpoint, uses that; otherwise constructs the AWS default.
        static std::string BuildEndpointUrl(CloudConnection const& connection, std::string const& bucket);
    };
} // namespace AIAssistant
