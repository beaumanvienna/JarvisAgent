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
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

        struct ProviderConfig
        {
            std::string m_DisplayName;
            std::string m_Endpoint;
            std::string m_ApiKey;
            std::string m_DefaultModel;
            std::string m_ApiType; // "API1" or "API2"

            // Credential type: "api_key" (default), "oauth", "key_pair", "credentials"
            std::string m_CredentialType{"api_key"};

            // OAuth fields (credential_type == "oauth")
            std::string m_RefreshToken;
            int64_t m_ExpiresAt{0}; // Unix timestamp (seconds)
            std::string m_Scopes;

            // Key pair fields (credential_type == "key_pair")
            std::string m_PrivateKeyPem;

            // Basic auth fields (credential_type == "credentials")
            std::string m_Username;
            std::string m_Password;
        };

        KeyManager() = default;
        ~KeyManager() = default;

        // Load providers from an encrypted file using the master password.
        // Returns true if the file was successfully decrypted and parsed.
        bool Load(std::filesystem::path const& keysFilePath, std::string const& masterPassword);

        // Save current providers to an encrypted file.
        bool Save(std::filesystem::path const& keysFilePath, std::string const& masterPassword);

        // Load providers from a plaintext JSON file (development only).
        bool LoadPlaintext(std::filesystem::path const& keysFilePath);

        // Save current providers to a plaintext JSON file (development only).
        bool SavePlaintext(std::filesystem::path const& keysFilePath);

        // Backward compatibility: create a single "openai" provider from OPENAI_API_KEY env var.
        // endpoint, model, and apiType are taken from the existing config.json API interface.
        bool LoadFromEnvironment(std::string const& endpoint, std::string const& model, std::string const& apiType);

        // Runtime unlock: attempt to decrypt the stored keys file path with a password.
        // Updates m_KeyLoadStatus on success or failure.
        bool Unlock(std::string const& masterPassword);

        // Key load status
        KeyLoadStatus GetKeyLoadStatus() const { return m_KeyLoadStatus; }
        void SetKeyLoadStatus(KeyLoadStatus status) { m_KeyLoadStatus = status; }

        // Store the keys file path so Unlock() can use it at runtime
        void SetKeysFilePath(std::filesystem::path const& path) { m_KeysFilePath = path; }

        // Dirty flag: true when in-memory state differs from on-disk state
        bool IsDirty() const { return m_Dirty; }

        // Provider registry access (thread-safe, read-locked)
        ProviderConfig const* GetProvider(std::string const& name) const;
        ProviderConfig const* GetDefaultProvider() const;
        std::string GetDefaultProviderName() const;
        std::vector<std::string> GetProviderNames() const;
        bool HasProviders() const;

        // CRUD (thread-safe, write-locked)
        bool AddProvider(std::string const& name, ProviderConfig config);
        bool UpdateProvider(std::string const& name, ProviderConfig config);
        bool RemoveProvider(std::string const& name);
        void SetDefaultProvider(std::string const& name);

    private:
        // Parse a JSON string into the provider registry.
        bool ParseProvidersJson(std::string const& json);

        // Serialize the provider registry to a JSON string.
        std::string SerializeToJson() const;

        std::string m_DefaultProviderName;
        std::unordered_map<std::string, ProviderConfig> m_Providers;
        mutable std::shared_mutex m_Mutex;

        KeyLoadStatus m_KeyLoadStatus{KeyLoadStatus::NoKeysFile};
        std::filesystem::path m_KeysFilePath;
        bool m_Dirty{false};
    };
} // namespace AIAssistant
