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
#include <string_view>
#include <vector>

#include "keys/secureString.h"

namespace AIAssistant
{
    // Azure Storage Shared Key request signer (OpenSSL HMAC-SHA256).
    //
    // Produces the Authorization header and required x-ms-* headers
    // for authenticating requests to Azure Blob Storage REST API.
    //
    // Authorization format: SharedKey {accountName}:{Base64(HMAC-SHA256(key, StringToSign))}
    //
    // StringToSign format (Azure Storage):
    //   VERB\n
    //   Content-Encoding\n
    //   Content-Language\n
    //   Content-Length\n
    //   Content-MD5\n
    //   Content-Type\n
    //   Date\n
    //   If-Modified-Since\n
    //   If-Match\n
    //   If-None-Match\n
    //   If-Unmodified-Since\n
    //   Range\n
    //   CanonicalizedHeaders\n
    //   CanonicalizedResource
    class AzureSharedKeySigner
    {
    public:
        struct SignedRequest
        {
            // Headers to add to the HTTP request (Authorization, x-ms-date, x-ms-version).
            std::map<std::string, std::string> m_Headers;
        };

        // Sign an HTTP request for Azure Blob Storage.
        //
        // method:       HTTP method (GET, PUT, DELETE, HEAD)
        // url:          Full URL (e.g., "https://myaccount.blob.core.windows.net/container/blob")
        // accountName:  Azure Storage account name
        // accountKey:   Azure Storage account key (Base64-encoded, secret material) — held in
        //               a SecureString so the secret bytes never appear in a plain std::string
        //               heap allocation outside this signer's HMAC-SHA256 scratch buffers.
        // extraHeaders: Additional headers to include in the signature (e.g., x-ms-blob-type, Content-Type)
        // contentLength: Content-Length value for the request (0 for GET/DELETE/HEAD)
        static SignedRequest Sign(std::string const& method, std::string const& url,
                                  std::string const& accountName, SecureString const& accountKey,
                                  std::map<std::string, std::string> const& extraHeaders = {},
                                  size_t contentLength = 0);

    private:
        // HMAC-SHA256(key, data) → raw bytes.  Key is accepted as a byte vector so the
        // caller can wrap the Base64-decoded secret in ScopedSecretBytes (zero-on-destruct
        // via OPENSSL_cleanse) — no std::string heap intermediate holding the raw key.
        // The output (the signature) is public on the wire, so std::string is the right
        // return type.
        static std::string HmacSha256(std::vector<unsigned char> const& key, std::string_view data);

        // Base64-encode raw bytes.
        static std::string Base64Encode(std::string const& data);

        // Base64-decode a string to raw bytes.  Returns a byte vector (empty on decode
        // failure) so the caller can wrap the decoded secret in ScopedSecretBytes for
        // OPENSSL_cleanse-on-destruct — no std::string intermediate holds the raw key
        // bytes between decode and HMAC.
        static std::vector<unsigned char> Base64Decode(std::string_view encoded);

        // Parse URL into host, path, query components.
        struct UrlParts
        {
            std::string m_Host;
            std::string m_Path;
            std::string m_Query;
        };
        static UrlParts ParseUrl(std::string const& url);

        // Build canonicalized headers string (sorted x-ms-* headers, one per line).
        static std::string CanonicalizedHeaders(std::map<std::string, std::string> const& headers);

        // Build canonicalized resource string.
        // Format: /{accountName}{path}\n{query-param-name}:{value}\n...
        static std::string CanonicalizedResource(std::string const& accountName, std::string const& path,
                                                  std::string const& query);

        // Get current UTC time in RFC 1123 format (e.g., "Sat, 12 Apr 2026 08:30:00 GMT").
        static std::string GetRfc1123Date();
    };
} // namespace AIAssistant
