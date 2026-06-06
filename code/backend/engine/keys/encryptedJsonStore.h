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

#include <cstddef>
#include <expected>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace AIAssistant
{
    // Typed error for the master-password-encrypted JSON stores.  Pairs with
    // `std::expected<void, StoreError>` so callers are compiler-forced to handle
    // the failure path (the `[[nodiscard]] bool + log` shape let load failures
    // slip past unnoticed).  `m_Code` is the machine-actionable category;
    // `m_Details` is the human-readable detail composed at the failure site for
    // the ERROR log.  The enum is `default:`-free per CLAUDE.md discipline —
    // adding a variant trips `-Wswitch` at every `switch (err.m_Code)` site.
    enum class StoreErrorCode
    {
        FileOpenFailed, // cannot open the path for reading
        FileEmpty,      // file exists but holds zero bytes
        TooLarge,       // encrypted blob exceeds the store's MaxFileBytes cap
        DecryptFailed,  // wrong password / truncated / GCM tag mismatch / unknown version
        ParseFailed,    // decrypted JSON malformed or violates the store's schema
        EncryptFailed,  // KeyEncryption::Encrypt returned empty (RAND/PBKDF2/EVP)
        WriteFailed,    // atomic write of the encrypted blob failed
        UnknownError,   // backstop — add a variant rather than collapse into this
    };

    struct StoreError
    {
        StoreErrorCode m_Code{StoreErrorCode::UnknownError};
        std::string m_Details;

        static StoreError Make(StoreErrorCode code, std::string details)
        {
            return StoreError{code, std::move(details)};
        }
    };

    // Stable label for the code itself (NOT the variable detail) for log lines.
    std::string_view Describe(StoreErrorCode code);

    // Shared crypto + IO primitives for master-password-encrypted JSON stores.
    // EncryptedJsonStore uses these; managers that keep their own locking model
    // and therefore can't derive the base (e.g. CloudConnectionManager, whose
    // parse/serialize already lock) call them directly so the encrypt/decrypt +
    // size-cap + atomic-write dance lives in exactly one place.
    //
    // DecryptStoreFile: size-cap → read → KeyEncryption::Decrypt; returns the
    // decrypted plaintext JSON.  EncryptAndWriteStoreFile: KeyEncryption::Encrypt
    // → EngineCore::AtomicWriteFile.
    [[nodiscard]] std::expected<std::string, StoreError> DecryptStoreFile(std::filesystem::path const& path,
                                                                         std::string_view masterPassword,
                                                                         std::size_t maxBytes);

    [[nodiscard]] std::expected<void, StoreError> EncryptAndWriteStoreFile(std::filesystem::path const& path,
                                                                          std::string_view masterPassword,
                                                                          std::string_view plaintext);

    // Base for stores persisted as a single master-password-encrypted JSON
    // document (AES-256-GCM + PBKDF2 via KeyEncryption, atomic-rename writes via
    // EngineCore::AtomicWriteFile).  The base owns the file IO, the size cap, the
    // encrypt/decrypt round-trip, the `m_Loaded` flag, and the data mutex.
    // Derived classes implement ONLY the record model:
    //   * SerializeToJson()  — build the plaintext document (runs under m_Mutex held by Save)
    //   * ParseFromJson(...)  — parse a decrypted document into in-memory state (under m_Mutex held by Load)
    // and may override MaxFileBytes().  Derived public mutators/readers lock
    // m_Mutex themselves; SerializeToJson/ParseFromJson must NOT re-lock (the
    // base already holds it when invoking them).
    class EncryptedJsonStore
    {
    public:
        virtual ~EncryptedJsonStore() = default;

        // Read + decrypt + parse the store from disk.  On success the in-memory
        // state is replaced and IsLoaded() becomes true.  On failure the prior
        // in-memory state is left untouched.
        [[nodiscard]] std::expected<void, StoreError> Load(std::filesystem::path const& path,
                                                           std::string_view masterPassword);

        // Serialize + encrypt + atomically write the store to disk.
        [[nodiscard]] std::expected<void, StoreError> Save(std::filesystem::path const& path,
                                                          std::string_view masterPassword);

        bool IsLoaded() const;

    protected:
        virtual std::string SerializeToJson() const = 0;
        [[nodiscard]] virtual std::expected<void, StoreError> ParseFromJson(std::string_view json) = 0;

        // Defence-in-depth size cap on the encrypted blob (rejected before the
        // whole file is read into memory).  4 MiB matches KeyManager's cap.
        virtual std::size_t MaxFileBytes() const { return 4u * 1024u * 1024u; }

        mutable std::shared_mutex m_Mutex;
        bool m_Loaded{false};
    };
} // namespace AIAssistant
