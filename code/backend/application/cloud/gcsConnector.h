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

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "cloud/cloudConnector.h"

namespace AIAssistant
{
    // Google Cloud Storage connector (native JSON API).
    //
    // Uses service account JWT auth: the private key (PEM, stored as KeyPairCredential
    // in KeyManager) is used to mint a self-signed JWT via JwtGenerator, which is then
    // exchanged for an OAuth2 access token at https://oauth2.googleapis.com/token.
    //
    // CloudConnection.m_Params keys:
    //   "bucket"                   — default GCS bucket (can be overridden per-task)
    //   "service_account_email"    — service account email (used in JWT iss/sub claims)
    //
    // CloudConnection.m_Endpoint — GCS API base URL.
    //   Production: "https://storage.googleapis.com" (default)
    //   Local test: "http://localhost:4443" (fake-gcs-server)
    // CloudConnection.m_KeyName  — KeyManager credential (KeyPairCredential with PEM private key)
    // CloudConnection.m_AuthType — JwtRsa
    class GcsConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        [[nodiscard]] std::expected<void, ConnectorError> TestConnection(CloudConnection const& connection) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        // Build the GCS API base URL.
        static std::string BuildEndpointUrl(CloudConnection const& connection);

    private:
        // Exchange a self-signed JWT for an OAuth2 access token.  Both the input JWT and
        // the output access token are SecureString so the secret bytes never appear in a
        // plain std::string heap allocation between JwtGenerator's output and libcurl's
        // CURLOPT_POSTFIELDS pointer.
        static bool ExchangeJwtForAccessToken(SecureString const& jwt, std::string const& endpoint,
                                               SecureString& accessToken, std::string& errorMessage);

        // Simple token cache (one per connection key name, keyed by m_KeyName).  The
        // cached access token is SecureString so cache hits return the secret bytes via
        // a Set(view) copy into the caller's CloudCredentials::m_Token rather than
        // through a std::string materialisation step.
        struct CachedToken
        {
            SecureString m_AccessToken;
            std::chrono::steady_clock::time_point m_ExpiresAt{};
        };

        mutable std::mutex m_CacheMutex;
        mutable std::unordered_map<std::string, CachedToken> m_TokenCache;
    };
} // namespace AIAssistant
