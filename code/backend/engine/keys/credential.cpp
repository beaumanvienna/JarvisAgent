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

#include <sstream>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "json/jsonHelper.h"
#include "keys/credential.h"
#include "log/secretRedactor.h"

namespace AIAssistant
{
    namespace
    {
        // Read a JSON string field directly into a SecureString.  Returns true on success;
        // leaves `target` unchanged on missing-field or wrong-type so callers can pre-seed
        // defaults.  The temporary `string_view` from simdjson points into the parser buffer
        // and is copied into the SecureString's locked memory by `Set()`.
        bool ReadSecureString(simdjson::ondemand::object& obj, char const* fieldName, SecureString& target)
        {
            std::string_view sv;
            if (obj[fieldName].get_string().get(sv) != simdjson::SUCCESS) return false;
            target.Set(sv);
            return true;
        }

        // Read a JSON string field into a std::string.  Same semantics as ReadSecureString
        // but for non-secret metadata.
        bool ReadString(simdjson::ondemand::object& obj, char const* fieldName, std::string& target)
        {
            std::string_view sv;
            if (obj[fieldName].get_string().get(sv) != simdjson::SUCCESS) return false;
            target = std::string(sv);
            return true;
        }

        // Read a JSON int64 field.  Returns true on success; leaves target unchanged otherwise.
        bool ReadInt64(simdjson::ondemand::object& obj, char const* fieldName, int64_t& target)
        {
            int64_t v = 0;
            if (obj[fieldName].get_int64().get(v) != simdjson::SUCCESS) return false;
            target = v;
            return true;
        }

        // Read the optional `params` sub-object into the credential's m_Params map.
        // Each value must be a string; non-string values are skipped with a WARN.
        void ReadParams(simdjson::ondemand::object& obj, std::unordered_map<std::string, std::string>& target)
        {
            simdjson::ondemand::object paramsObj;
            if (obj["params"].get_object().get(paramsObj) != simdjson::SUCCESS) return;
            for (auto field : paramsObj)
            {
                std::string_view paramKey = field.unescaped_key();
                std::string_view paramVal;
                if (field.value().get_string().get(paramVal) == simdjson::SUCCESS)
                {
                    target[std::string(paramKey)] = std::string(paramVal);
                }
                else
                {
                    LOG_CORE_WARN("CredentialFactory: params.{} is not a string; skipping", paramKey);
                }
            }
        }

        // Populate the common metadata fields (name is set by the caller from the JSON key).
        void ReadCommonMetadata(simdjson::ondemand::object& obj, ICredential& cred)
        {
            ReadString(obj, "display_name", cred.m_DisplayName);
            ReadString(obj, "endpoint", cred.m_Endpoint);
            ReadString(obj, "default_model", cred.m_DefaultModel);
            ReadString(obj, "api_type", cred.m_ApiType);
            ReadParams(obj, cred.m_Params);
        }

        // Write a JSON `"key": "value",` line, escaping the value for JSON-safe embedding.
        // Always writes a trailing comma; the caller is responsible for the final field
        // (or for accepting a trailing comma — our serialiser writes a sentinel last field).
        void WriteJsonStringField(std::ostringstream& os, char const* indent, std::string_view key,
                                  std::string_view value)
        {
            os << indent << "\"" << key << "\": \"" << JsonHelper::EscapeJsonString(value) << "\",\n";
        }

        // Write a JSON `"key": <int>,` line.
        void WriteJsonInt64Field(std::ostringstream& os, char const* indent, std::string_view key, int64_t value)
        {
            os << indent << "\"" << key << "\": " << value << ",\n";
        }

        // Write the common metadata block + opening of the params block (if non-empty).
        // Returns the field count written (used by the caller to decide on trailing commas).
        void WriteCommonMetadata(std::ostringstream& os, char const* indent, ICredential const& cred)
        {
            WriteJsonStringField(os, indent, "credential_type", cred.GetType());
            if (!cred.m_DisplayName.empty())  WriteJsonStringField(os, indent, "display_name",   cred.m_DisplayName);
            if (!cred.m_Endpoint.empty())     WriteJsonStringField(os, indent, "endpoint",       cred.m_Endpoint);
            if (!cred.m_DefaultModel.empty()) WriteJsonStringField(os, indent, "default_model",  cred.m_DefaultModel);
            if (!cred.m_ApiType.empty())      WriteJsonStringField(os, indent, "api_type",       cred.m_ApiType);
        }

        // Write the params object as `"params": { "k1": "v1", "k2": "v2" },`.  Skipped
        // entirely when m_Params is empty so we don't emit `"params": {},` lines.
        void WriteParams(std::ostringstream& os, char const* indent,
                         std::unordered_map<std::string, std::string> const& params)
        {
            if (params.empty()) return;
            os << indent << "\"params\": {";
            bool first = true;
            for (auto const& [k, v] : params)
            {
                if (!first) os << ",";
                os << " \"" << JsonHelper::EscapeJsonString(k) << "\": \""
                   << JsonHelper::EscapeJsonString(v) << "\"";
                first = false;
            }
            os << " },\n";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // RegisterSecrets implementations
    // -------------------------------------------------------------------------

    void ApiKeyCredential::RegisterSecrets() const
    {
        if (!m_ApiKey.IsEmpty()) SecretRedactor::Get().AddSecret(m_ApiKey.Get());
    }

    void OAuthCredential::RegisterSecrets() const
    {
        if (!m_AccessToken.IsEmpty())  SecretRedactor::Get().AddSecret(m_AccessToken.Get());
        if (!m_RefreshToken.IsEmpty()) SecretRedactor::Get().AddSecret(m_RefreshToken.Get());
        if (!m_ClientSecret.IsEmpty()) SecretRedactor::Get().AddSecret(m_ClientSecret.Get());
    }

    void KeyPairCredential::RegisterSecrets() const
    {
        if (!m_PrivateKeyPem.IsEmpty()) SecretRedactor::Get().AddSecret(m_PrivateKeyPem.Get());
    }

    void BasicAuthCredential::RegisterSecrets() const
    {
        if (!m_Password.IsEmpty()) SecretRedactor::Get().AddSecret(m_Password.Get());
    }

    void AwsCredential::RegisterSecrets() const
    {
        // m_AccessKeyId is intentionally NOT registered — AWS treats it as public
        // (CloudTrail logs it for audit).  The dual-secret material is what gets scrubbed.
        if (!m_SecretAccessKey.IsEmpty()) SecretRedactor::Get().AddSecret(m_SecretAccessKey.Get());
        if (!m_SessionToken.IsEmpty())    SecretRedactor::Get().AddSecret(m_SessionToken.Get());
    }

    // -------------------------------------------------------------------------
    // CredentialFactory::CreateFromJson
    // -------------------------------------------------------------------------
    std::unique_ptr<ICredential> CredentialFactory::CreateFromJson(simdjson::ondemand::object& providerObj)
    {
        // Read credential_type — absent means "api_key" for backward-compat with
        // pre-hierarchy keys.json.enc files that didn't write the discriminator.
        std::string credentialType = "api_key";
        ReadString(providerObj, "credential_type", credentialType);

        std::unique_ptr<ICredential> cred;

        if (credentialType == "api_key")
        {
            auto typed = std::make_unique<ApiKeyCredential>();
            ReadSecureString(providerObj, "api_key", typed->m_ApiKey);
            cred = std::move(typed);
        }
        else if (credentialType == "oauth")
        {
            auto typed = std::make_unique<OAuthCredential>();
            // Cached access token may be persisted between rotations.  Older keys.json
            // files may have stored it under "api_key" — read both keys for compat;
            // explicit "access_token" wins if both are present.
            ReadSecureString(providerObj, "api_key", typed->m_AccessToken);
            ReadSecureString(providerObj, "access_token", typed->m_AccessToken);

            ReadSecureString(providerObj, "refresh_token", typed->m_RefreshToken);
            ReadSecureString(providerObj, "client_secret", typed->m_ClientSecret);
            ReadInt64(providerObj, "expires_at", typed->m_ExpiresAt);
            ReadString(providerObj, "scopes", typed->m_Scopes);
            ReadString(providerObj, "token_endpoint", typed->m_TokenEndpoint);
            ReadString(providerObj, "client_id", typed->m_ClientId);
            cred = std::move(typed);
        }
        else if (credentialType == "key_pair")
        {
            auto typed = std::make_unique<KeyPairCredential>();
            ReadSecureString(providerObj, "private_key_pem", typed->m_PrivateKeyPem);
            cred = std::move(typed);
        }
        else if (credentialType == "credentials")
        {
            auto typed = std::make_unique<BasicAuthCredential>();
            ReadString(providerObj, "username", typed->m_Username);
            ReadSecureString(providerObj, "password", typed->m_Password);
            cred = std::move(typed);
        }
        else if (credentialType == "aws")
        {
            auto typed = std::make_unique<AwsCredential>();
            // AccessKeyId is non-secret.  Historical keys.json stored it in "api_key";
            // new format uses "access_key_id".  Read both for compat; explicit wins.
            ReadString(providerObj, "api_key", typed->m_AccessKeyId);
            ReadString(providerObj, "access_key_id", typed->m_AccessKeyId);

            // Region historically lived under params; promote to typed field when present.
            // The params map is also read so providers that only set "region" via params
            // still work; the typed field takes precedence.
            std::string region;
            ReadString(providerObj, "region", region);
            if (!region.empty()) typed->m_Region = region;

            ReadSecureString(providerObj, "secret_access_key", typed->m_SecretAccessKey);
            ReadSecureString(providerObj, "session_token", typed->m_SessionToken);
            cred = std::move(typed);
        }
        else
        {
            LOG_CORE_ERROR("CredentialFactory::CreateFromJson: unknown credential_type '{}'; provider will not load",
                           credentialType);
            return nullptr;
        }

        // Common metadata applies to every subtype.
        ReadCommonMetadata(providerObj, *cred);

        // For AWS, lift `region` out of params if it landed there (legacy path).
        if (auto* aws = dynamic_cast<AwsCredential*>(cred.get()))
        {
            if (aws->m_Region.empty())
            {
                auto it = aws->m_Params.find("region");
                if (it != aws->m_Params.end())
                {
                    aws->m_Region = it->second;
                    aws->m_Params.erase(it);
                }
            }
        }

        return cred;
    }

    // -------------------------------------------------------------------------
    // CredentialFactory::CloneAndPatch
    // -------------------------------------------------------------------------
    // Build a same-subtype clone of `existing`, then overlay optional patch fields
    // present in `patch`.  REST UPDATE handler's "partial-patch" semantics: every
    // field is optional in the request body; missing keys preserve the existing
    // credential's value.
    //
    // `credential_type` in the patch is intentionally ignored — changing a credential's
    // subtype via UPDATE would require throwing away the type-specific fields, and the
    // contract is cleaner if callers DELETE + CREATE for that case.
    std::unique_ptr<ICredential> CredentialFactory::CloneAndPatch(ICredential const& existing,
                                                                 simdjson::ondemand::object& patch)
    {
        std::unique_ptr<ICredential> cloned;

        // Build same-subtype clone with type-specific fields.  Patch arms inside each
        // branch overlay the optional REST request fields appropriate for that subtype.
        if (auto const* api = dynamic_cast<ApiKeyCredential const*>(&existing))
        {
            auto typed = std::make_unique<ApiKeyCredential>();
            if (!api->m_ApiKey.IsEmpty()) typed->m_ApiKey.Set(api->m_ApiKey.Get());
            ReadSecureString(patch, "api_key", typed->m_ApiKey);
            cloned = std::move(typed);
        }
        else if (auto const* oauth = dynamic_cast<OAuthCredential const*>(&existing))
        {
            auto typed = std::make_unique<OAuthCredential>();
            if (!oauth->m_AccessToken.IsEmpty())  typed->m_AccessToken.Set(oauth->m_AccessToken.Get());
            if (!oauth->m_RefreshToken.IsEmpty()) typed->m_RefreshToken.Set(oauth->m_RefreshToken.Get());
            if (!oauth->m_ClientSecret.IsEmpty()) typed->m_ClientSecret.Set(oauth->m_ClientSecret.Get());
            typed->m_ExpiresAt     = oauth->m_ExpiresAt;
            typed->m_Scopes        = oauth->m_Scopes;
            typed->m_TokenEndpoint = oauth->m_TokenEndpoint;
            typed->m_ClientId      = oauth->m_ClientId;
            // Patch arms.  `api_key` is the legacy alias for the cached access token.
            ReadSecureString(patch, "api_key",        typed->m_AccessToken);
            ReadSecureString(patch, "access_token",   typed->m_AccessToken);
            ReadSecureString(patch, "refresh_token",  typed->m_RefreshToken);
            ReadSecureString(patch, "client_secret",  typed->m_ClientSecret);
            ReadInt64(patch,        "expires_at",     typed->m_ExpiresAt);
            ReadString(patch,       "scopes",         typed->m_Scopes);
            ReadString(patch,       "token_endpoint", typed->m_TokenEndpoint);
            ReadString(patch,       "client_id",      typed->m_ClientId);
            cloned = std::move(typed);
        }
        else if (auto const* kp = dynamic_cast<KeyPairCredential const*>(&existing))
        {
            auto typed = std::make_unique<KeyPairCredential>();
            if (!kp->m_PrivateKeyPem.IsEmpty()) typed->m_PrivateKeyPem.Set(kp->m_PrivateKeyPem.Get());
            ReadSecureString(patch, "private_key_pem", typed->m_PrivateKeyPem);
            cloned = std::move(typed);
        }
        else if (auto const* basic = dynamic_cast<BasicAuthCredential const*>(&existing))
        {
            auto typed = std::make_unique<BasicAuthCredential>();
            typed->m_Username = basic->m_Username;
            if (!basic->m_Password.IsEmpty()) typed->m_Password.Set(basic->m_Password.Get());
            ReadString(patch,       "username", typed->m_Username);
            ReadSecureString(patch, "password", typed->m_Password);
            cloned = std::move(typed);
        }
        else if (auto const* aws = dynamic_cast<AwsCredential const*>(&existing))
        {
            auto typed = std::make_unique<AwsCredential>();
            typed->m_AccessKeyId = aws->m_AccessKeyId;
            if (!aws->m_SecretAccessKey.IsEmpty()) typed->m_SecretAccessKey.Set(aws->m_SecretAccessKey.Get());
            if (!aws->m_SessionToken.IsEmpty())    typed->m_SessionToken.Set(aws->m_SessionToken.Get());
            typed->m_Region = aws->m_Region;
            // Patch arms.  `api_key` is the legacy alias for access_key_id (public).
            ReadString(patch,       "api_key",           typed->m_AccessKeyId);
            ReadString(patch,       "access_key_id",     typed->m_AccessKeyId);
            ReadSecureString(patch, "secret_access_key", typed->m_SecretAccessKey);
            ReadSecureString(patch, "session_token",     typed->m_SessionToken);
            ReadString(patch,       "region",            typed->m_Region);
            cloned = std::move(typed);
        }
        else
        {
            LOG_CORE_ERROR("CredentialFactory::CloneAndPatch: existing credential '{}' has unrecognised subtype '{}'",
                           existing.m_Name, existing.GetType());
            return nullptr;
        }

        // Common metadata: clone existing, overlay patch.
        cloned->m_Name         = existing.m_Name;
        cloned->m_DisplayName  = existing.m_DisplayName;
        cloned->m_Endpoint     = existing.m_Endpoint;
        cloned->m_DefaultModel = existing.m_DefaultModel;
        cloned->m_ApiType      = existing.m_ApiType;
        cloned->m_Params       = existing.m_Params;

        ReadString(patch, "display_name",  cloned->m_DisplayName);
        ReadString(patch, "endpoint",      cloned->m_Endpoint);
        ReadString(patch, "default_model", cloned->m_DefaultModel);
        ReadString(patch, "api_type",      cloned->m_ApiType);

        // Params: REPLACE wholesale if the field is present (UI sends the full edited
        // map); leave existing untouched if absent.  Matches the legacy UPDATE handler.
        simdjson::ondemand::object paramsObj;
        if (patch["params"].get_object().get(paramsObj) == simdjson::SUCCESS)
        {
            cloned->m_Params.clear();
            for (auto field : paramsObj)
            {
                std::string_view paramKey = field.unescaped_key();
                std::string_view paramVal;
                if (field.value().get_string().get(paramVal) == simdjson::SUCCESS)
                {
                    cloned->m_Params[std::string(paramKey)] = std::string(paramVal);
                }
            }
        }

        // For AWS credentials, lift `region` out of params if it landed there (legacy UI
        // path that puts region under params).  Mirrors the behaviour in CreateFromJson;
        // without this, an UPDATE with `params: {"region": "us-west-1"}` would put region
        // in m_Params but leave the typed m_Region (which the SigV4 signer reads) stale.
        if (auto* aws = dynamic_cast<AwsCredential*>(cloned.get()))
        {
            auto it = aws->m_Params.find("region");
            if (it != aws->m_Params.end())
            {
                aws->m_Region = it->second;
                aws->m_Params.erase(it);
            }
        }

        return cloned;
    }

    // -------------------------------------------------------------------------
    // CredentialFactory::SerializeToJson
    // -------------------------------------------------------------------------
    std::string CredentialFactory::SerializeToJson(ICredential const& cred)
    {
        constexpr char const* kIndent = "            "; // 12 spaces (matches keyManager.cpp legacy)
        std::ostringstream os;

        WriteCommonMetadata(os, kIndent, cred);
        WriteParams(os, kIndent, cred.m_Params);

        // Type-specific fields.  Serialiser is exhaustive: every concrete subtype defined
        // in credential.h has a branch.  An unknown subtype (i.e. someone added a new
        // class but forgot to extend the serialiser) is logged loudly so the gap is
        // obvious; the partial JSON still contains valid common metadata.
        // Empty-field policy: every type-specific field is guarded by an emptiness check
        // before writing.  Skipping empty fields keeps the on-disk JSON tight (no
        // `"api_key": ""` lines for unset credentials) AND preserves round-trip semantics
        // — `ReadSecureString` / `ReadString` both leave the target unchanged when the
        // field is absent, which means "absent" and "empty string" load identically.
        if (auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred))
        {
            if (!api->m_ApiKey.IsEmpty()) WriteJsonStringField(os, kIndent, "api_key", api->m_ApiKey.Get());
        }
        else if (auto const* oauth = dynamic_cast<OAuthCredential const*>(&cred))
        {
            if (!oauth->m_AccessToken.IsEmpty())  WriteJsonStringField(os, kIndent, "access_token",   oauth->m_AccessToken.Get());
            if (!oauth->m_RefreshToken.IsEmpty()) WriteJsonStringField(os, kIndent, "refresh_token",  oauth->m_RefreshToken.Get());
            if (!oauth->m_ClientSecret.IsEmpty()) WriteJsonStringField(os, kIndent, "client_secret",  oauth->m_ClientSecret.Get());
            if (oauth->m_ExpiresAt > 0)           WriteJsonInt64Field(os,  kIndent, "expires_at",     oauth->m_ExpiresAt);
            if (!oauth->m_Scopes.empty())         WriteJsonStringField(os, kIndent, "scopes",         oauth->m_Scopes);
            if (!oauth->m_TokenEndpoint.empty())  WriteJsonStringField(os, kIndent, "token_endpoint", oauth->m_TokenEndpoint);
            if (!oauth->m_ClientId.empty())       WriteJsonStringField(os, kIndent, "client_id",      oauth->m_ClientId);
        }
        else if (auto const* kp = dynamic_cast<KeyPairCredential const*>(&cred))
        {
            if (!kp->m_PrivateKeyPem.IsEmpty()) WriteJsonStringField(os, kIndent, "private_key_pem", kp->m_PrivateKeyPem.Get());
        }
        else if (auto const* basic = dynamic_cast<BasicAuthCredential const*>(&cred))
        {
            if (!basic->m_Username.empty())   WriteJsonStringField(os, kIndent, "username", basic->m_Username);
            if (!basic->m_Password.IsEmpty()) WriteJsonStringField(os, kIndent, "password", basic->m_Password.Get());
        }
        else if (auto const* aws = dynamic_cast<AwsCredential const*>(&cred))
        {
            if (!aws->m_AccessKeyId.empty())       WriteJsonStringField(os, kIndent, "access_key_id",     aws->m_AccessKeyId);
            if (!aws->m_SecretAccessKey.IsEmpty()) WriteJsonStringField(os, kIndent, "secret_access_key", aws->m_SecretAccessKey.Get());
            if (!aws->m_SessionToken.IsEmpty())    WriteJsonStringField(os, kIndent, "session_token",     aws->m_SessionToken.Get());
            if (!aws->m_Region.empty())            WriteJsonStringField(os, kIndent, "region",            aws->m_Region);
        }
        else
        {
            LOG_CORE_ERROR("CredentialFactory::SerializeToJson: unhandled credential subtype '{}' — extend the "
                           "serialiser to cover this type",
                           cred.GetType());
        }

        // Trim the trailing comma+newline that every WriteJson*Field appends.  The output
        // is the body of an object literal; the caller wraps it in `{ ... }`.
        std::string result = os.str();
        if (result.size() >= 2 && result[result.size() - 2] == ',' && result[result.size() - 1] == '\n')
        {
            result.erase(result.size() - 2, 1); // drop the comma; keep the newline
        }
        return result;
    }
} // namespace AIAssistant
