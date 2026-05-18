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

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
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

        // Run a callback with the held master password as a std::string_view.
        // Returns true if a master password is held (callback was invoked) and
        // false otherwise (callback was not invoked).
        //
        // The master password is held in mlock-locked SecureString memory from
        // the moment Unlock (or first-run Save) succeeds until shutdown.  This
        // is the single in-process copy: callers MUST NOT make plaintext
        // std::string copies of the view (which would defeat the swap-protection
        // guarantee).  Use the view only inside the callback; do not store it.
        //
        // Implementation note: the held password is copied into a local
        // SecureString under a brief lock and the lock is released before the
        // callback runs.  This makes callback re-entry into KeyManager (e.g.
        // calling Save) safe without lock ordering hazards.  The local copy is
        // zeroed on destruction at scope exit.
        template <typename F>
        bool WithMasterPassword(F&& fn) const
        {
            SecureString localCopy;
            {
                std::shared_lock lock(m_MasterPasswordMutex);
                std::string_view view = m_MasterPassword.Get();
                if (view.empty())
                {
                    return false;
                }
                localCopy.Set(view);
            }
            std::forward<F>(fn)(localCopy.Get());
            return true;
        }

        // Dirty flag: true when in-memory state differs from on-disk state
        bool IsDirty() const { return m_Dirty.load(); }

        // ---- Provider registry access (thread-safe) ----
        // The typed `ICredential` hierarchy is the single source of truth; secret-bearing
        // fields are stored in `SecureString` (mlock'd, zero-on-destruct).  Callers
        // resolve a credential by name and `dynamic_cast` to the expected concrete subtype
        // (`ApiKeyCredential`, `OAuthCredential`, `KeyPairCredential`, `BasicAuthCredential`,
        // `AwsCredential`).  See `engine/keys/credential.h` for the hierarchy.
        //
        // Read access is via callback (WithCredential / WithDefaultCredential): the
        // callback runs while the shared_lock is held, so the `ICredential const&`
        // reference is guaranteed live for the call's duration.  Callers MUST NOT store
        // the reference (or pointers to its fields) beyond the callback — a subsequent
        // RemoveProvider would invalidate it.  Callbacks also MUST NOT call back into
        // write-side KeyManager methods (AddCredential / RemoveProvider /
        // ModifyCredential / UpsertCredential / SetDefaultProvider) from inside the
        // callback — that would deadlock against the held shared_lock.  Read-side calls
        // (other With* / Has*) are safe (shared+shared).

        // Run fn(ICredential const&) while holding shared_lock(m_Mutex).
        // Returns true if `name` exists (fn was invoked), false otherwise.
        template <typename F>
        bool WithCredential(std::string const& name, F&& fn) const
        {
            std::shared_lock lock(m_Mutex);
            auto const it = m_Credentials.find(name);
            if (it == m_Credentials.end())
            {
                return false;
            }
            std::forward<F>(fn)(*it->second);
            return true;
        }

        // Like WithCredential but uses the configured default provider.
        // Returns false if no default is set, the default name has no credential, or
        // the default credential entry is null.
        template <typename F>
        bool WithDefaultCredential(F&& fn) const
        {
            std::shared_lock lock(m_Mutex);
            if (m_DefaultProviderName.empty())
            {
                return false;
            }
            auto const it = m_Credentials.find(m_DefaultProviderName);
            if (it == m_Credentials.end() || !it->second)
            {
                return false;
            }
            std::forward<F>(fn)(*it->second);
            return true;
        }

        // Existence checks (cheap shared_lock + map lookup).
        bool HasCredential(std::string const& name) const;
        bool HasDefaultCredential() const;

        std::string GetDefaultProviderName() const;
        std::vector<std::string> GetProviderNames() const;
        bool HasProviders() const;

        // CRUD (thread-safe, write-locked).  Take ownership of a pre-built `ICredential`
        // (typically built by `CredentialFactory::CreateFromJson` from a REST request body
        // or by `CredentialFactory::CloneAndPatch` for partial-update flows).
        bool AddCredential(std::string const& name, std::unique_ptr<ICredential> cred);
        bool RemoveProvider(std::string const& name);
        void SetDefaultProvider(std::string const& name);

        // Atomic read-modify-write — looks up `name`, invokes `mutator(existing)` under
        // unique_lock, stores the returned credential in place.  Mutator receives
        // `ICredential const&` and returns a new `std::unique_ptr<ICredential>` (or
        // nullptr to abort the update).  Returns true if the credential existed and was
        // replaced; false if not found or mutator returned nullptr.
        //
        // Use for partial-update flows that read existing fields, derive a patched
        // credential, and must not race against a concurrent RemoveProvider between
        // the read and the write.
        template <typename F>
        bool ModifyCredential(std::string const& name, F&& mutator)
        {
            std::unique_lock lock(m_Mutex);
            auto it = m_Credentials.find(name);
            if (it == m_Credentials.end())
            {
                return false;
            }
            std::unique_ptr<ICredential> updated = std::forward<F>(mutator)(*it->second);
            if (!updated)
            {
                return false;
            }
            updated->m_Name = name;
            updated->RegisterSecrets();
            it->second = std::move(updated);
            m_Dirty.store(true);
            return true;
        }

        // Atomic add-or-update — invokes `builder(existing)` under unique_lock, where
        // `existing` is a pointer to the current credential or `nullptr` if `name` is
        // new.  Builder returns the credential to store; returning nullptr is a no-op
        // (the registry is left untouched).
        //
        // Use when a flow may legitimately create the credential OR update an existing
        // one in a single atomic operation (e.g. OAuth callback persisting freshly-
        // issued tokens whether or not the user pre-created a same-named provider).
        template <typename F>
        void UpsertCredential(std::string const& name, F&& builder)
        {
            std::unique_lock lock(m_Mutex);
            auto it = m_Credentials.find(name);
            ICredential const* existing = (it != m_Credentials.end()) ? it->second.get() : nullptr;
            std::unique_ptr<ICredential> updated = std::forward<F>(builder)(existing);
            if (!updated)
            {
                return;
            }
            updated->m_Name = name;
            updated->RegisterSecrets();
            bool const isInsert = (existing == nullptr);
            m_Credentials[name] = std::move(updated);
            m_Dirty.store(true);
            if (isInsert && m_Credentials.size() == 1)
            {
                m_DefaultProviderName = name;
            }
        }

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

        // Master password held in mlock-locked memory after Unlock succeeds.
        // Read via WithMasterPassword(); written by Load/Save under
        // m_MasterPasswordMutex.  Separate from m_Mutex so callbacks invoked
        // via WithMasterPassword can re-enter the KeyManager safely.
        mutable std::shared_mutex m_MasterPasswordMutex;
        SecureString m_MasterPassword;

        std::atomic<bool> m_Dirty{false};
    };
} // namespace AIAssistant
