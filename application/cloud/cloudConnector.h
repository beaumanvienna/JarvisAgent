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

namespace AIAssistant
{
    // Identifies the authentication method for a cloud connection.
    enum class CloudAuthType
    {
        BearerToken, // API key or PAT as Bearer header
        OAuth2,      // OAuth 2.0 access token (auto-refreshed)
        JwtRsa,      // RSA-signed JWT (e.g., Snowflake)
        BasicAuth,   // Username + password
        SigV4        // AWS Signature V4 (S3-compatible)
    };

    // Resolved credentials ready for use in HTTP requests.
    // This is a runtime transport bundle — not a persisted type. The persisted side uses
    // the ICredential hierarchy (ApiKeyCredential, OAuthCredential, etc.). CloudCredentials
    // is what connectors produce after resolving and refreshing stored credentials into a
    // form ready for immediate use in HTTP headers / connection strings.
    struct CloudCredentials
    {
        CloudAuthType m_AuthType{CloudAuthType::BearerToken};
        std::string m_Token;       // Bearer/OAuth token or JWT
        std::string m_AccessKeyId; // SigV4
        std::string m_SecretKey;   // SigV4
        std::string m_Username;    // BasicAuth
        std::string m_Password;    // BasicAuth
    };

    // Configuration for a named cloud connection (persisted in config.json).
    struct CloudConnection
    {
        std::string m_Name;     // Unique connection name (user-defined)
        std::string m_Type;     // "polarion", "s3", "onedrive", "snowflake", etc.
        std::string m_Endpoint; // Base URL or account identifier
        std::string m_KeyName;  // Reference to KeyManager credential
        CloudAuthType m_AuthType{CloudAuthType::BearerToken};
        std::map<std::string, std::string> m_Params; // Type-specific config
    };

    // Convert CloudAuthType to/from string for serialization and REST API.
    inline std::string AuthTypeToString(CloudAuthType authType)
    {
        switch (authType)
        {
            case CloudAuthType::BearerToken: return "bearer";
            case CloudAuthType::OAuth2: return "oauth2";
            case CloudAuthType::JwtRsa: return "jwt_rsa";
            case CloudAuthType::BasicAuth: return "basic_auth";
            case CloudAuthType::SigV4: return "sigv4";
        }
        return "bearer";
    }

    inline CloudAuthType StringToAuthType(std::string_view str)
    {
        if (str == "oauth2") return CloudAuthType::OAuth2;
        if (str == "jwt_rsa") return CloudAuthType::JwtRsa;
        if (str == "basic_auth") return CloudAuthType::BasicAuth;
        if (str == "sigv4") return CloudAuthType::SigV4;
        return CloudAuthType::BearerToken;
    }

    // Abstract interface for all cloud service connectors.
    class ICloudConnector
    {
    public:
        virtual ~ICloudConnector() = default;

        // Return the connector type name (e.g., "polarion", "s3", "snowflake").
        virtual std::string GetType() const = 0;

        // Test connectivity using the given connection config.
        // Returns true on success, populates errorMessage on failure.
        virtual bool TestConnection(CloudConnection const& connection, std::string& errorMessage) = 0;

        // Resolve credentials from KeyManager for the given connection.
        // Handles OAuth refresh, JWT generation, SigV4 derivation, etc.
        virtual bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                        std::string& errorMessage) = 0;
    };
} // namespace AIAssistant
