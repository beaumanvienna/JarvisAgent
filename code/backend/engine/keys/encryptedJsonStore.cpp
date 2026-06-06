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

#include "keys/encryptedJsonStore.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <vector>

#include "auxiliary/file.h"
#include "keys/keyEncryption.h"

namespace AIAssistant
{
    std::string_view Describe(StoreErrorCode code)
    {
        switch (code)
        {
            case StoreErrorCode::FileOpenFailed: return "file_open_failed";
            case StoreErrorCode::FileEmpty:      return "file_empty";
            case StoreErrorCode::TooLarge:       return "too_large";
            case StoreErrorCode::DecryptFailed:  return "decrypt_failed";
            case StoreErrorCode::ParseFailed:    return "parse_failed";
            case StoreErrorCode::EncryptFailed:  return "encrypt_failed";
            case StoreErrorCode::WriteFailed:    return "write_failed";
            case StoreErrorCode::UnknownError:   return "unknown_error";
        }
        return "unknown_error";
    }

    std::expected<std::string, StoreError> DecryptStoreFile(std::filesystem::path const& path,
                                                            std::string_view masterPassword,
                                                            std::size_t maxBytes)
    {
        // Size cap BEFORE reading — a giant file must not be slurped into memory
        // just to be rejected.  This stat is only a hint/early-out, NOT trusted as
        // the read bound (see the post-read grow check below): a racing writer
        // could replace or grow the file between this stat and the read (TOCTOU).
        std::error_code ec;
        auto const fileSize = std::filesystem::file_size(path, ec);
        if (ec)
        {
            return std::unexpected(StoreError::Make(StoreErrorCode::FileOpenFailed,
                                                    "stat '" + path.string() + "': " + ec.message()));
        }
        if (fileSize > maxBytes)
        {
            return std::unexpected(StoreError::Make(StoreErrorCode::TooLarge,
                                                    "'" + path.string() + "' is " + std::to_string(fileSize) +
                                                        " bytes (cap " + std::to_string(maxBytes) + ")"));
        }

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return std::unexpected(StoreError::Make(StoreErrorCode::FileOpenFailed,
                                                    "cannot open '" + path.string() + "'"));
        }

        // Bounded read: allocate the (cap-verified) stat size and read exactly that.
        // If any bytes remain afterwards, the file grew past its size between the
        // stat and the read — reject rather than slurp the larger content.  This
        // bounds the in-memory blob by the read itself, not by the stale stat.
        std::vector<uint8_t> blob(fileSize);
        file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(fileSize));
        auto const bytesRead = file.gcount();
        bool const grewPastCap = (file.peek() != std::ifstream::traits_type::eof());
        file.close();
        if (grewPastCap)
        {
            return std::unexpected(StoreError::Make(
                StoreErrorCode::TooLarge,
                "'" + path.string() + "' grew past its " + std::to_string(maxBytes) +
                    "-byte cap between stat and read"));
        }
        blob.resize(static_cast<std::size_t>(bytesRead > 0 ? bytesRead : 0));

        if (blob.empty())
        {
            return std::unexpected(StoreError::Make(StoreErrorCode::FileEmpty, "'" + path.string() + "' is empty"));
        }

        std::string json = KeyEncryption::Decrypt(blob, masterPassword);
        if (json.empty())
        {
            return std::unexpected(StoreError::Make(StoreErrorCode::DecryptFailed,
                                                    "decryption failed for '" + path.string() + "'"));
        }
        return json;
    }

    std::expected<void, StoreError> EncryptAndWriteStoreFile(std::filesystem::path const& path,
                                                             std::string_view masterPassword,
                                                             std::string_view plaintext)
    {
        std::vector<uint8_t> blob = KeyEncryption::Encrypt(std::string(plaintext), masterPassword);
        if (blob.empty())
        {
            return std::unexpected(
                StoreError::Make(StoreErrorCode::EncryptFailed, "encryption failed for '" + path.string() + "'"));
        }

        // Atomic write — a truncated encrypted store breaks every subsequent
        // unlock, so the temp-file-then-rename helper is mandatory here.
        std::string_view const blobView(reinterpret_cast<char const*>(blob.data()), blob.size());
        std::string writeError;
        if (!EngineCore::AtomicWriteFile(path, blobView, writeError))
        {
            return std::unexpected(StoreError::Make(StoreErrorCode::WriteFailed,
                                                    writeError + " (path='" + path.string() + "')"));
        }
        return {};
    }

    bool EncryptedJsonStore::IsLoaded() const
    {
        std::shared_lock lock(m_Mutex);
        return m_Loaded;
    }

    std::expected<void, StoreError> EncryptedJsonStore::Load(std::filesystem::path const& path,
                                                            std::string_view masterPassword)
    {
        auto json = DecryptStoreFile(path, masterPassword, MaxFileBytes());
        if (!json.has_value())
        {
            return std::unexpected(std::move(json.error()));
        }

        std::unique_lock lock(m_Mutex);
        if (auto parsed = ParseFromJson(*json); !parsed.has_value())
        {
            return std::unexpected(std::move(parsed.error()));
        }
        m_Loaded = true;
        return {};
    }

    std::expected<void, StoreError> EncryptedJsonStore::Save(std::filesystem::path const& path,
                                                            std::string_view masterPassword)
    {
        std::string json;
        {
            std::shared_lock lock(m_Mutex);
            json = SerializeToJson();
        }

        if (auto written = EncryptAndWriteStoreFile(path, masterPassword, json); !written.has_value())
        {
            return std::unexpected(std::move(written.error()));
        }

        {
            std::unique_lock lock(m_Mutex);
            m_Loaded = true;
        }
        return {};
    }
} // namespace AIAssistant
