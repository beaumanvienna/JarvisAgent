/* Copyright (c) 2026 JC Technolabs

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace AIAssistant
{
    // Abstract base for all credential types stored in KeyManager.
    // Replaces the flat ProviderConfig.m_ApiKey with a type-safe hierarchy.
    class ICredential
    {
    public:
        virtual ~ICredential() = default;

        // Returns the credential type discriminator for serialization.
        virtual std::string GetType() const = 0;

        // Unique credential name (same as the provider/key name in KeyManager).
        std::string m_Name;
    };

    // Simple API key or personal access token.
    class ApiKeyCredential : public ICredential
    {
    public:
        std::string GetType() const override { return "api_key"; }

        std::string m_ApiKey;
    };

    // OAuth 2.0 credential with access/refresh tokens and expiry tracking.
    class OAuthCredential : public ICredential
    {
    public:
        std::string GetType() const override { return "oauth"; }

        std::string m_AccessToken;
        std::string m_RefreshToken;
        int64_t m_ExpiresAt{0}; // Unix timestamp (seconds)
        std::string m_Scopes;
    };

    // RSA key pair credential (e.g., Snowflake JWT auth).
    class KeyPairCredential : public ICredential
    {
    public:
        std::string GetType() const override { return "key_pair"; }

        std::string m_PrivateKeyPem; // RSA private key in PEM format
    };

    // Username + password credential (e.g., PostgreSQL, SMTP).
    class BasicAuthCredential : public ICredential
    {
    public:
        std::string GetType() const override { return "credentials"; }

        std::string m_Username;
        std::string m_Password;
    };
} // namespace AIAssistant
