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
#include <vector>

namespace AIAssistant
{
    // AES-256-GCM encryption/decryption with PBKDF2 key derivation.
    // File format:
    //   [4-byte magic "JKEY"] [1-byte version] [16-byte salt] [12-byte IV]
    //   [ciphertext] [16-byte GCM tag]
    class KeyEncryption
    {
    public:
        // Encrypt plaintext JSON string with a master password.
        // Returns the complete encrypted blob (header + ciphertext + tag).
        static std::vector<uint8_t> Encrypt(std::string const& plaintext, std::string const& masterPassword);

        // Decrypt an encrypted blob back to plaintext JSON.
        // Returns empty string on failure (wrong password, corrupted data, etc.).
        static std::string Decrypt(std::vector<uint8_t> const& encryptedBlob, std::string const& masterPassword);

    private:
        static constexpr uint8_t MAGIC[4] = {'J', 'K', 'E', 'Y'};
        static constexpr uint8_t VERSION = 0x01;
        static constexpr int SALT_SIZE = 16;
        static constexpr int IV_SIZE = 12;
        static constexpr int TAG_SIZE = 16;
        static constexpr int HEADER_SIZE = 4 + 1 + SALT_SIZE + IV_SIZE; // 33 bytes
        static constexpr int PBKDF2_ITERATIONS = 100000;
        static constexpr int KEY_SIZE = 32; // AES-256
    };
} // namespace AIAssistant
