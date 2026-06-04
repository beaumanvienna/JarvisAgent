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
#include <string_view>
#include <unordered_map>

#include "simdjson/simdjson.h"

#include "keys/secureString.h"

namespace AIAssistant
{
    // Type-safe credential hierarchy.  Every secret-bearing field is a `SecureString`
    // so the plaintext value lives in mlock'd, zero-on-destruct memory rather than a
    // copy-prone std::string.
    //
    // Ownership: KeyManager stores `std::unique_ptr<ICredential>` in its registry.
    // Callers access credentials through callback-based APIs
    // (`KeyManager::WithCredential(name, fn)` / `WithDefaultCredential(fn)`); the
    // callback receives an `ICredential const&` that's valid only for the call's
    // duration, then dynamic_casts to the concrete subtype that matches `GetType()`.
    // Subtypes are movable but not copyable (SecureString is non-copyable by design).
    //
    // Subtype map (matches the historical `credential_type` JSON field):
    //   - "api_key"     — `ApiKeyCredential`     (OpenAI, Anthropic, Gemini, Azure, GitHub PAT, Slack bot, etc.)
    //   - "oauth"       — `OAuthCredential`      (Google OAuth, Microsoft Graph, etc.)
    //   - "key_pair"    — `KeyPairCredential`    (Snowflake JWT auth)
    //   - "credentials" — `BasicAuthCredential`  (PostgreSQL, SMTP, IMAP, etc.)
    //   - "aws"         — `AwsCredential`        (AWS Bedrock; AccessKeyId is public, SecretAccessKey/SessionToken are secrets)
    //
    // Adding a new subtype requires:
    //   1. New class deriving from ICredential.
    //   2. `GetType()` returning a unique discriminator string.
    //   3. `RegisterSecrets()` that calls `SecretRedactor::AddSecret` for every SecureString field.
    //   4. `CredentialFactory::CreateFromJson` dispatch arm in credential.cpp.
    //   5. `CredentialFactory::SerializeToJson` dispatch arm in credential.cpp.
    class ICredential
    {
    public:
        virtual ~ICredential() = default;

        // Type discriminator written to / read from `credential_type` in JSON.
        virtual std::string_view GetType() const = 0;

        // Registers every secret-bearing field on this credential with `SecretRedactor::Get()`.
        // Called from KeyManager whenever a credential is loaded, added, or updated.
        // Per-type so each subtype lists its own secrets and we never rely on a string-keyed
        // switch inside KeyManager.
        virtual void RegisterSecrets() const = 0;

        // ---- Common metadata (non-secret) ----
        std::string m_Name;          // unique key under which KeyManager indexes this credential
        std::string m_DisplayName;
        std::string m_Endpoint;      // canonical API URL
        std::string m_DefaultModel;
        std::string m_ApiType;       // "API1" / "API2" / ... / "API6"

        // Per-provider non-secret extras (Azure resource/deployment/api_version, AWS region
        // when not promoted to a typed field, etc.).  NEVER store secrets here — secrets go
        // into the SecureString fields on the concrete subtype.
        std::unordered_map<std::string, std::string> m_Params;

        // Movable but not copyable — SecureString fields are non-copyable by design.
        ICredential() = default;
        ICredential(ICredential const&) = delete;
        ICredential& operator=(ICredential const&) = delete;
        ICredential(ICredential&&) noexcept = default;
        ICredential& operator=(ICredential&&) noexcept = default;
    };

    // -------------------------------------------------------------------------
    // ApiKeyCredential
    // -------------------------------------------------------------------------
    // Bearer / x-api-key / x-goog-api-key / api-key — single secret value used as a
    // header credential.  Covers OpenAI, Anthropic, Gemini, Azure OpenAI, GitHub PAT,
    // Slack bot tokens, and any other "single token in a header" provider.  The auth
    // signer (AuthStyle dispatch) decides which header name to emit; this class only
    // owns the secret.
    class ApiKeyCredential : public ICredential
    {
    public:
        std::string_view GetType() const override { return "api_key"; }
        void RegisterSecrets() const override;

        SecureString m_ApiKey;
    };

    // -------------------------------------------------------------------------
    // OAuthCredential
    // -------------------------------------------------------------------------
    // OAuth 2.0 with refresh-token rotation.  The access token is the short-lived
    // bearer that signers use today; the refresh token + client secret are the
    // long-lived material that `OAuthTokenManager` uses to mint new access tokens.
    // ClientId is non-secret (public per OAuth 2.0 conventions); TokenEndpoint and
    // Scopes are non-secret discovery metadata.
    class OAuthCredential : public ICredential
    {
    public:
        std::string_view GetType() const override { return "oauth"; }
        void RegisterSecrets() const override;

        SecureString m_AccessToken;   // short-lived, rotated by OAuthTokenManager
        SecureString m_RefreshToken;  // long-lived, used to mint new access tokens
        SecureString m_ClientSecret;  // confidential-client only; public clients leave this empty

        int64_t m_ExpiresAt{0};       // unix seconds; access token expiry
        std::string m_Scopes;
        std::string m_TokenEndpoint;  // e.g. https://oauth2.googleapis.com/token
        std::string m_ClientId;       // public per OAuth conventions
    };

    // -------------------------------------------------------------------------
    // KeyPairCredential
    // -------------------------------------------------------------------------
    // RSA / EC private key in PEM form.  Used by `JwtGenerator` to sign short-lived
    // JWTs (Snowflake key-pair auth).  The PEM string IS the secret — the public key
    // is derived from it and registered separately with the cloud provider out of
    // band.  No corresponding "public key" field on the credential.
    class KeyPairCredential : public ICredential
    {
    public:
        std::string_view GetType() const override { return "key_pair"; }
        void RegisterSecrets() const override;

        SecureString m_PrivateKeyPem;
    };

    // -------------------------------------------------------------------------
    // BasicAuthCredential
    // -------------------------------------------------------------------------
    // Username + password.  PostgreSQL, SMTP, IMAP, and any provider that takes
    // basic-auth or username/password fields on the wire.  Username is non-secret
    // (logged for audit); password is the secret.
    class BasicAuthCredential : public ICredential
    {
    public:
        std::string_view GetType() const override { return "credentials"; }
        void RegisterSecrets() const override;

        std::string m_Username;
        SecureString m_Password;
    };

    // -------------------------------------------------------------------------
    // AwsCredential
    // -------------------------------------------------------------------------
    // AWS access-key-id + secret-access-key (+ optional session token for STS).
    // AccessKeyId is treated as public per AWS conventions (CloudTrail logs it for
    // audit), so it is NOT registered with the redactor.  SecretAccessKey and
    // SessionToken are the dual-secret material that gets scrubbed.  Region is non-
    // secret.  Used by SigV4 signers for AWS Bedrock (and any other AWS service).
    class AwsCredential : public ICredential
    {
    public:
        std::string_view GetType() const override { return "aws"; }
        void RegisterSecrets() const override;

        std::string m_AccessKeyId;       // public per AWS conventions
        SecureString m_SecretAccessKey;
        SecureString m_SessionToken;     // empty for long-lived IAM credentials; populated by STS
        std::string m_Region;
    };

    // -------------------------------------------------------------------------
    // CredentialFactory
    // -------------------------------------------------------------------------
    // JSON round-trip for the credential hierarchy.  Lives in credential.cpp so the
    // header doesn't pull in simdjson.  Both directions dispatch on `GetType()` /
    // `credential_type`; an unknown discriminator on read returns nullptr with a
    // LOG_CORE_ERROR.
    class CredentialFactory
    {
    public:
        // Construct a typed ICredential from a simdjson provider object (the value
        // side of one entry in the `"providers"` map).  The credential's `m_Name` is
        // not set by this method — KeyManager assigns it from the JSON key.
        // Returns nullptr on parse failure (with logged detail).
        static std::unique_ptr<ICredential> CreateFromJson(simdjson::ondemand::object& providerObj);

        // Build a NEW credential of the same subtype as `existing`, copying all fields,
        // then overlay any optional patch fields present in `patch` (the body of a REST
        // PUT request).  Used by the UPDATE handler.  `credential_type` in the patch is
        // intentionally ignored — to change a credential's type, DELETE + CREATE.
        // Returns nullptr if `existing` is an unrecognised subtype.
        static std::unique_ptr<ICredential> CloneAndPatch(ICredential const& existing,
                                                         simdjson::ondemand::object& patch);

        // Serialise a credential's body to a JSON object string (the value side of one
        // entry in the `"providers"` map — does NOT include the surrounding object
        // braces or the provider name key).  Common metadata fields are written first,
        // then type-specific fields.  Always writes `credential_type`.
        // String values are escaped via `JsonHelper::EscapeJsonString`.
        static std::string SerializeToJson(ICredential const& cred);
    };
} // namespace AIAssistant
