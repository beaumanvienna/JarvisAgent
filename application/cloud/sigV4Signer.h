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

#include <map>
#include <string>

namespace AIAssistant
{
    // AWS Signature V4 request signer (OpenSSL HMAC-SHA256).
    //
    // Produces the Authorization header and required X-Amz-* headers
    // for authenticating requests to S3-compatible APIs.
    class SigV4Signer
    {
    public:
        struct SignedRequest
        {
            // Headers to add to the HTTP request (Authorization, X-Amz-Date, X-Amz-Content-Sha256, Host).
            std::map<std::string, std::string> m_Headers;
        };

        // Sign an HTTP request for S3.
        //
        // method:       HTTP method (GET, PUT, DELETE, HEAD)
        // url:          Full URL (e.g., "https://bucket.s3.us-east-1.amazonaws.com/key")
        // region:       AWS region (e.g., "us-east-1")
        // service:      AWS service name (always "s3" for S3)
        // accessKeyId:  AWS access key ID
        // secretKey:    AWS secret access key
        // payloadHash:  SHA-256 hex digest of the request body ("UNSIGNED-PAYLOAD" for streaming)
        // extraHeaders: Additional headers to include in the signature (e.g., Content-Type)
        static SignedRequest Sign(std::string const& method, std::string const& url, std::string const& region,
                                  std::string const& service, std::string const& accessKeyId,
                                  std::string const& secretKey, std::string const& payloadHash,
                                  std::map<std::string, std::string> const& extraHeaders = {});

        // Compute SHA-256 hex digest of data.
        static std::string Sha256Hex(std::string const& data);

        // Compute SHA-256 hex digest of an empty string (frequently needed).
        static std::string const& EmptyPayloadHash();

    private:
        // HMAC-SHA256(key, data) → raw bytes.
        static std::string HmacSha256(std::string const& key, std::string const& data);

        // HMAC-SHA256(key, data) → hex string.
        static std::string HmacSha256Hex(std::string const& key, std::string const& data);

        // Parse URL into host, path, query components.
        struct UrlParts
        {
            std::string m_Host;
            std::string m_Path;
            std::string m_Query;
        };
        static UrlParts ParseUrl(std::string const& url);

        // URL-encode a string (AWS-style: encode everything except unreserved chars).
        static std::string UriEncode(std::string const& value, bool encodeSlash = true);

        // Build canonical query string (sorted key=value pairs).
        static std::string CanonicalQueryString(std::string const& query);

        // Get current UTC time in ISO-8601 compact format.
        static std::string GetAmzDate();

        // Get current UTC date (YYYYMMDD).
        static std::string GetDateStamp();
    };
} // namespace AIAssistant
