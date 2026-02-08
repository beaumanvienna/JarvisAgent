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

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstring>

#include "engine.h"
#include "keys/keyEncryption.h"

namespace AIAssistant
{
    std::vector<uint8_t> KeyEncryption::Encrypt(std::string const& plaintext, std::string const& masterPassword)
    {
        // Generate random salt and IV
        uint8_t salt[SALT_SIZE];
        uint8_t iv[IV_SIZE];
        if (RAND_bytes(salt, SALT_SIZE) != 1 || RAND_bytes(iv, IV_SIZE) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: RAND_bytes failed");
            return {};
        }

        // Derive 256-bit key from master password via PBKDF2-HMAC-SHA256
        uint8_t key[KEY_SIZE];
        if (PKCS5_PBKDF2_HMAC(masterPassword.c_str(), static_cast<int>(masterPassword.size()),
                               salt, SALT_SIZE, PBKDF2_ITERATIONS, EVP_sha256(), KEY_SIZE, key) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: PBKDF2 key derivation failed");
            return {};
        }

        // Encrypt with AES-256-GCM
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_CIPHER_CTX_new failed");
            return {};
        }

        std::vector<uint8_t> result;

        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptInit_ex (cipher) failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, nullptr) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_CIPHER_CTX_ctrl (IV len) failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptInit_ex (key/iv) failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        // Allocate output: header + ciphertext (same size as plaintext) + tag
        size_t const plaintextSize = plaintext.size();
        result.resize(HEADER_SIZE + plaintextSize + TAG_SIZE);

        // Write header
        std::memcpy(result.data(), MAGIC, 4);
        result[4] = VERSION;
        std::memcpy(result.data() + 5, salt, SALT_SIZE);
        std::memcpy(result.data() + 5 + SALT_SIZE, iv, IV_SIZE);

        // Encrypt
        int outLen = 0;
        if (EVP_EncryptUpdate(ctx, result.data() + HEADER_SIZE, &outLen,
                              reinterpret_cast<uint8_t const*>(plaintext.data()),
                              static_cast<int>(plaintextSize)) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptUpdate failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        int finalLen = 0;
        if (EVP_EncryptFinal_ex(ctx, result.data() + HEADER_SIZE + outLen, &finalLen) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptFinal_ex failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        // Get GCM tag
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE,
                                result.data() + HEADER_SIZE + outLen + finalLen) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_CIPHER_CTX_ctrl (get tag) failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        // Resize to actual size (should match, but be safe)
        result.resize(HEADER_SIZE + outLen + finalLen + TAG_SIZE);

        EVP_CIPHER_CTX_free(ctx);

        // Clear key material from stack
        OPENSSL_cleanse(key, KEY_SIZE);

        return result;
    }

    std::string KeyEncryption::Decrypt(std::vector<uint8_t> const& encryptedBlob, std::string const& masterPassword)
    {
        // Minimum size: header + tag (no ciphertext = empty plaintext)
        if (encryptedBlob.size() < static_cast<size_t>(HEADER_SIZE + TAG_SIZE))
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: blob too small ({} bytes)", encryptedBlob.size());
            return {};
        }

        // Validate magic
        if (std::memcmp(encryptedBlob.data(), MAGIC, 4) != 0)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: invalid magic bytes");
            return {};
        }

        // Validate version
        if (encryptedBlob[4] != VERSION)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: unsupported version {}", encryptedBlob[4]);
            return {};
        }

        // Extract salt, IV, ciphertext, tag
        uint8_t const* salt = encryptedBlob.data() + 5;
        uint8_t const* iv = encryptedBlob.data() + 5 + SALT_SIZE;
        uint8_t const* ciphertext = encryptedBlob.data() + HEADER_SIZE;
        size_t const ciphertextSize = encryptedBlob.size() - HEADER_SIZE - TAG_SIZE;
        uint8_t const* tag = encryptedBlob.data() + HEADER_SIZE + ciphertextSize;

        // Derive key from master password
        uint8_t key[KEY_SIZE];
        if (PKCS5_PBKDF2_HMAC(masterPassword.c_str(), static_cast<int>(masterPassword.size()),
                               salt, SALT_SIZE, PBKDF2_ITERATIONS, EVP_sha256(), KEY_SIZE, key) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: PBKDF2 key derivation failed");
            return {};
        }

        // Decrypt with AES-256-GCM
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_CIPHER_CTX_new failed");
            OPENSSL_cleanse(key, KEY_SIZE);
            return {};
        }

        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_DecryptInit_ex (cipher) failed");
            EVP_CIPHER_CTX_free(ctx);
            OPENSSL_cleanse(key, KEY_SIZE);
            return {};
        }

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, nullptr) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_CIPHER_CTX_ctrl (IV len) failed");
            EVP_CIPHER_CTX_free(ctx);
            OPENSSL_cleanse(key, KEY_SIZE);
            return {};
        }

        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_DecryptInit_ex (key/iv) failed");
            EVP_CIPHER_CTX_free(ctx);
            OPENSSL_cleanse(key, KEY_SIZE);
            return {};
        }

        // Decrypt ciphertext
        std::string plaintext;
        plaintext.resize(ciphertextSize);

        int outLen = 0;
        if (EVP_DecryptUpdate(ctx, reinterpret_cast<uint8_t*>(plaintext.data()), &outLen,
                              ciphertext, static_cast<int>(ciphertextSize)) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_DecryptUpdate failed");
            EVP_CIPHER_CTX_free(ctx);
            OPENSSL_cleanse(key, KEY_SIZE);
            return {};
        }

        // Set expected GCM tag (const_cast is safe — OpenSSL only reads the tag during verify)
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                                const_cast<uint8_t*>(tag)) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_CIPHER_CTX_ctrl (set tag) failed");
            EVP_CIPHER_CTX_free(ctx);
            OPENSSL_cleanse(key, KEY_SIZE);
            return {};
        }

        int finalLen = 0;
        if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<uint8_t*>(plaintext.data()) + outLen, &finalLen) != 1)
        {
            // Authentication failed — wrong password or corrupted data
            LOG_CORE_ERROR("KeyEncryption::Decrypt: authentication failed (wrong password or corrupted data)");
            EVP_CIPHER_CTX_free(ctx);
            OPENSSL_cleanse(key, KEY_SIZE);
            return {};
        }

        plaintext.resize(outLen + finalLen);

        EVP_CIPHER_CTX_free(ctx);
        OPENSSL_cleanse(key, KEY_SIZE);

        return plaintext;
    }
} // namespace AIAssistant
