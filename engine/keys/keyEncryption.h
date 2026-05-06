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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AIAssistant
{
    // AES-256-GCM (authenticated encryption — confidentiality + integrity)
    // with PBKDF2 key derivation.  The GCM tag covers the ciphertext, and in
    // V2 also the full file header (see "Versions" below).
    // File format:
    //   [4-byte magic "JKEY"] [1-byte version] [16-byte salt] [12-byte IV]
    //   [ciphertext] [16-byte GCM tag]
    //
    // Versions:
    //   V1 (0x01) — PBKDF2-HMAC-SHA256 at 100,000 iterations, no GCM AAD.
    //               Decrypt-only (read for back-compat); Encrypt never produces V1.
    //   V2 (0x02) — PBKDF2-HMAC-SHA256 at 600,000 iterations (OWASP 2023+),
    //               full header (magic + version + salt + IV) bound as GCM AAD so
    //               header tampering invalidates the tag.  All new writes are V2.
    //
    // On-disk migration: V1 blobs decrypt at the V1 cost; the next save promotes
    // them to V2.  No explicit migration step is required.
    class KeyEncryption
    {
    public:
        // Encrypt plaintext JSON string with a master password.
        // Returns the complete encrypted blob (header + ciphertext + tag), or
        // an empty vector on failure (allocation, RAND_bytes, PBKDF2, or any
        // OpenSSL EVP step).
        // masterPassword is taken as a view so callers holding a SecureString can
        // pass SecureString::Get() directly without an intermediate std::string copy.
        // Always writes the current version (V2).
        [[nodiscard]] static std::vector<uint8_t> Encrypt(std::string const& plaintext,
                                                          std::string_view masterPassword);

        // Decrypt an encrypted blob back to plaintext JSON.
        // Dispatches on the version byte: V1 (legacy, no AAD, 100k PBKDF2) or
        // V2 (current, full-header AAD, 600k PBKDF2).  Returns empty string on
        // failure (wrong password, corrupted data, unknown version, etc.).
        [[nodiscard]] static std::string Decrypt(std::vector<uint8_t> const& encryptedBlob,
                                                 std::string_view masterPassword);

    private:
        static constexpr uint8_t MAGIC[4] = {'J', 'K', 'E', 'Y'};

        // Version byte values.  CURRENT_VERSION is what Encrypt writes; Decrypt
        // accepts any version listed here.
        static constexpr uint8_t VERSION_V1 = 0x01;
        static constexpr uint8_t VERSION_V2 = 0x02;
        static constexpr uint8_t CURRENT_VERSION = VERSION_V2;

        static constexpr int SALT_SIZE = 16;
        static constexpr int IV_SIZE = 12;
        static constexpr int TAG_SIZE = 16;
        static constexpr int HEADER_SIZE = 4 + 1 + SALT_SIZE + IV_SIZE; // 33 bytes

        // PBKDF2 iteration counts per version.  V2 = OWASP 2023+ recommendation
        // for PBKDF2-HMAC-SHA256.
        static constexpr int PBKDF2_ITERATIONS_V1 = 100000;
        static constexpr int PBKDF2_ITERATIONS_V2 = 600000;

        static constexpr int KEY_SIZE = 32; // AES-256
    };
} // namespace AIAssistant
