/* Copyright (c) 2025 JC Technolabs

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

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "keys/credential.h"
#include "keys/secureString.h"

namespace AIAssistant
{
    class KeyManager
    {
    public:
        enum class KeyLoadStatus
        {
            Ok,            // Keys loaded successfully
            NoKeysFile,    // No encrypted keys file found
            NoPassword,    // Encrypted file exists but no master password provided
            WrongPassword, // Encrypted file exists but decryption failed
        };

        KeyManager() = default;
        ~KeyManager() = default;

        // Load providers from an encrypted file using the master password.
        // Returns true if the file was successfully decrypted and parsed.
        bool Load(std::filesystem::path const& keysFilePath, std::string_view masterPassword);

        // Save current providers to an encrypted file.
        bool Save(std::filesystem::path const& keysFilePath, std::string_view masterPassword);

        // Load providers from a plaintext JSON file (development only).
        bool LoadPlaintext(std::filesystem::path const& keysFilePath);

        // Save current providers to a plaintext JSON file (development only).
        bool SavePlaintext(std::filesystem::path const& keysFilePath);

        // Backward compatibility: create a single "openai" provider from OPENAI_API_KEY env var.
        // endpoint, model, and apiType are taken from the existing config.json API interface.
        bool LoadFromEnvironment(std::string const& endpoint, std::string const& model, std::string const& apiType);

        // Runtime unlock: attempt to decrypt the stored keys file path with a password.
        // Updates m_KeyLoadStatus on success or failure.
        bool Unlock(std::string_view masterPassword);

        // Key load status
        KeyLoadStatus GetKeyLoadStatus() const { return m_KeyLoadStatus; }
        void SetKeyLoadStatus(KeyLoadStatus status) { m_KeyLoadStatus = status; }

        // Store the keys file path so Unlock() can use it at runtime
        void SetKeysFilePath(std::filesystem::path const& path) { m_KeysFilePath = path; }
        std::filesystem::path const& GetKeysFilePath() const { return m_KeysFilePath; }

        // Run a callback with the cached master password as a std::string_view.
        // Returns true if a cached password exists (callback was invoked) and
        // false otherwise (callback was not invoked).  The view is only valid
        // for the duration of the call; do not store it.
        //
        // Prefer this over a getter that returns std::string: the cached password
        // lives in mlock()-locked SecureString memory, and copying it into a
        // heap-allocated std::string defeats the swap-protection guarantee.
        template <typename F>
        bool WithCachedMasterPassword(F&& fn) const
        {
            std::string_view view = m_CachedMasterPassword.Get();
            if (view.empty())
            {
                return false;
            }
            std::forward<F>(fn)(view);
            return true;
        }

        // True if a master password is currently cached (after a successful
        // Load/Unlock).  Use WithCachedMasterPassword() to actually use it.
        bool HasCachedMasterPassword() const { return !m_CachedMasterPassword.IsEmpty(); }

        // Dirty flag: true when in-memory state differs from on-disk state
        bool IsDirty() const { return m_Dirty; }

        // ---- Provider registry access (thread-safe, read-locked) ----
        // The typed `ICredential` hierarchy is the single source of truth; secret-bearing
        // fields are stored in `SecureString` (mlock'd, zero-on-destruct).  Callers
        // resolve a credential by name and `dynamic_cast` to the expected concrete subtype
        // (`ApiKeyCredential`, `OAuthCredential`, `KeyPairCredential`, `BasicAuthCredential`,
        // `AwsCredential`).  See `engine/keys/credential.h` for the hierarchy.
        ICredential const* GetCredential(std::string const& name) const;
        ICredential const* GetDefaultCredential() const;

        std::string GetDefaultProviderName() const;
        std::vector<std::string> GetProviderNames() const;
        bool HasProviders() const;

        // CRUD (thread-safe, write-locked).  Take ownership of a pre-built `ICredential`
        // (typically built by `CredentialFactory::CreateFromJson` from a REST request body
        // or by `CredentialFactory::CloneAndPatch` for partial-update flows).
        bool AddCredential(std::string const& name, std::unique_ptr<ICredential> cred);
        bool UpdateCredential(std::string const& name, std::unique_ptr<ICredential> cred);
        bool RemoveProvider(std::string const& name);
        void SetDefaultProvider(std::string const& name);

    private:
        // Parse a JSON string into the credential registry.  Dispatches each provider
        // entry through `CredentialFactory::CreateFromJson`.
        bool ParseProvidersJson(std::string const& json);

        // Serialize the credential registry to a JSON string via `CredentialFactory::SerializeToJson`.
        std::string SerializeToJson() const;

        std::string m_DefaultProviderName;

        // Source of truth: typed credentials with SecureString-protected secret fields.
        std::unordered_map<std::string, std::unique_ptr<ICredential>> m_Credentials;

        mutable std::shared_mutex m_Mutex;

        KeyLoadStatus m_KeyLoadStatus{KeyLoadStatus::NoKeysFile};
        std::filesystem::path m_KeysFilePath;
        SecureString m_CachedMasterPassword;
        bool m_Dirty{false};
    };
} // namespace AIAssistant
