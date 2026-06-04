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
#include <memory>

#include "engine.h"
#include "keys/keyEncryption.h"

namespace AIAssistant
{
    namespace
    {
        // RAII wrapper for OpenSSL's EVP_CIPHER_CTX*.  Frees on scope exit so
        // every error path is just `return {};` — no need to remember a manual
        // EVP_CIPHER_CTX_free at every fail site.  Use ctx.get() to pass to
        // EVP_* functions.
        struct EvpCipherCtxDeleter
        {
            void operator()(EVP_CIPHER_CTX* ctx) const noexcept
            {
                if (ctx) EVP_CIPHER_CTX_free(ctx);
            }
        };
        using EvpCipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;

        // RAII wrapper for a stack-allocated symmetric key buffer.  Calls
        // OPENSSL_cleanse on destruction so the derived key never lingers in
        // the stack frame after the function returns — defends against
        // information disclosure via core dumps and freed-stack reuse.
        // Pass `key.data` to OpenSSL primitives.
        template <size_t N>
        struct ScopedKey
        {
            uint8_t data[N];
            ~ScopedKey() { OPENSSL_cleanse(data, N); }
        };
    } // namespace

    std::vector<uint8_t> KeyEncryption::Encrypt(std::string const& plaintext, std::string_view masterPassword)
    {
        // Generate random salt and IV
        uint8_t salt[SALT_SIZE];
        uint8_t iv[IV_SIZE];
        if (RAND_bytes(salt, SALT_SIZE) != 1 || RAND_bytes(iv, IV_SIZE) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: RAND_bytes failed");
            return {};
        }

        // Derive 256-bit key from master password via PBKDF2-HMAC-SHA256.
        // V2 uses 600,000 iterations per OWASP 2023+ guidance for HMAC-SHA256.
        // ScopedKey cleanses on scope exit; no manual cleanup needed at returns.
        ScopedKey<KEY_SIZE> key;
        if (PKCS5_PBKDF2_HMAC(masterPassword.data(), static_cast<int>(masterPassword.size()),
                               salt, SALT_SIZE, PBKDF2_ITERATIONS_V2, EVP_sha256(), KEY_SIZE, key.data) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: PBKDF2 key derivation failed");
            return {};
        }

        // Build the full output buffer up front, header first, so we can pass the
        // header bytes to GCM as Additional Authenticated Data (AAD) before the
        // ciphertext.  AAD binding ensures any tampering with magic/version/salt/IV
        // invalidates the GCM tag at decrypt time.
        //
        // Ordering invariant: the header MUST be fully written before the AAD
        // EVP_EncryptUpdate call below, and the symmetric AAD update on the
        // Decrypt side reads the same first HEADER_SIZE bytes from the input
        // blob.  Reorder these and AAD will bind whatever uninitialized bytes
        // happen to be there, silently breaking authentication.
        size_t const plaintextSize = plaintext.size();
        std::vector<uint8_t> result(HEADER_SIZE + plaintextSize + TAG_SIZE);
        std::memcpy(result.data(), MAGIC, 4);
        result[4] = CURRENT_VERSION;
        std::memcpy(result.data() + 5, salt, SALT_SIZE);
        std::memcpy(result.data() + 5 + SALT_SIZE, iv, IV_SIZE);

        // Encrypt with AES-256-GCM.  EvpCipherCtxPtr frees on scope exit.
        EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
        if (!ctx)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_CIPHER_CTX_new failed");
            return {};
        }

        if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptInit_ex (cipher) failed");
            return {};
        }

        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, nullptr) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_CIPHER_CTX_ctrl (IV len) failed");
            return {};
        }

        if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data, iv) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptInit_ex (key/iv) failed");
            return {};
        }

        // Bind the full header (magic + version + salt + IV) as AAD.  The OpenSSL
        // convention for AAD is a call to EVP_EncryptUpdate with `out=nullptr`.
        int aadLen = 0;
        if (EVP_EncryptUpdate(ctx.get(), nullptr, &aadLen, result.data(), HEADER_SIZE) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptUpdate (AAD) failed");
            return {};
        }

        // Encrypt
        int outLen = 0;
        if (EVP_EncryptUpdate(ctx.get(), result.data() + HEADER_SIZE, &outLen,
                              reinterpret_cast<uint8_t const*>(plaintext.data()),
                              static_cast<int>(plaintextSize)) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptUpdate failed");
            return {};
        }

        int finalLen = 0;
        if (EVP_EncryptFinal_ex(ctx.get(), result.data() + HEADER_SIZE + outLen, &finalLen) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_EncryptFinal_ex failed");
            return {};
        }

        // Get GCM tag
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, TAG_SIZE,
                                result.data() + HEADER_SIZE + outLen + finalLen) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Encrypt: EVP_CIPHER_CTX_ctrl (get tag) failed");
            return {};
        }

        // Resize to actual size (should match, but be safe)
        result.resize(HEADER_SIZE + outLen + finalLen + TAG_SIZE);
        return result;
    }

    std::string KeyEncryption::Decrypt(std::vector<uint8_t> const& encryptedBlob, std::string_view masterPassword)
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

        // Dispatch on version byte to pick the right PBKDF2 cost and AAD posture.
        // V1 (legacy): 100k iterations, no AAD.  V2 (current): 600k iterations,
        // header bound as AAD.  Reject anything else — better to fail than to
        // silently accept a downgraded blob.
        uint8_t const version = encryptedBlob[4];
        int iterations = 0;
        bool useAad = false;
        switch (version)
        {
            case VERSION_V1:
                iterations = PBKDF2_ITERATIONS_V1;
                useAad = false;
                break;
            case VERSION_V2:
                iterations = PBKDF2_ITERATIONS_V2;
                useAad = true;
                break;
            default:
                LOG_CORE_ERROR("KeyEncryption::Decrypt: unsupported version {}", version);
                return {};
        }

        // Extract salt, IV, ciphertext, tag
        uint8_t const* salt = encryptedBlob.data() + 5;
        uint8_t const* iv = encryptedBlob.data() + 5 + SALT_SIZE;
        uint8_t const* ciphertext = encryptedBlob.data() + HEADER_SIZE;
        size_t const ciphertextSize = encryptedBlob.size() - HEADER_SIZE - TAG_SIZE;
        uint8_t const* tag = encryptedBlob.data() + HEADER_SIZE + ciphertextSize;

        // Derive key from master password.  ScopedKey cleanses on scope exit.
        ScopedKey<KEY_SIZE> key;
        if (PKCS5_PBKDF2_HMAC(masterPassword.data(), static_cast<int>(masterPassword.size()),
                               salt, SALT_SIZE, iterations, EVP_sha256(), KEY_SIZE, key.data) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: PBKDF2 key derivation failed");
            return {};
        }

        // Decrypt with AES-256-GCM.  EvpCipherCtxPtr frees on scope exit.
        EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
        if (!ctx)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_CIPHER_CTX_new failed");
            return {};
        }

        if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_DecryptInit_ex (cipher) failed");
            return {};
        }

        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, nullptr) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_CIPHER_CTX_ctrl (IV len) failed");
            return {};
        }

        if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data, iv) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_DecryptInit_ex (key/iv) failed");
            return {};
        }

        // V2 binds the full header as AAD so any tampering with magic/version/
        // salt/IV invalidates the GCM tag.  V1 blobs were encrypted without AAD
        // and must be decrypted the same way (skipping this step).
        if (useAad)
        {
            int aadLen = 0;
            if (EVP_DecryptUpdate(ctx.get(), nullptr, &aadLen, encryptedBlob.data(), HEADER_SIZE) != 1)
            {
                LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_DecryptUpdate (AAD) failed");
                return {};
            }
        }

        // Decrypt ciphertext
        std::string plaintext;
        plaintext.resize(ciphertextSize);

        int outLen = 0;
        if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<uint8_t*>(plaintext.data()), &outLen,
                              ciphertext, static_cast<int>(ciphertextSize)) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_DecryptUpdate failed");
            return {};
        }

        // Set expected GCM tag (const_cast is safe — OpenSSL only reads the tag during verify)
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                                const_cast<uint8_t*>(tag)) != 1)
        {
            LOG_CORE_ERROR("KeyEncryption::Decrypt: EVP_CIPHER_CTX_ctrl (set tag) failed");
            return {};
        }

        int finalLen = 0;
        if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<uint8_t*>(plaintext.data()) + outLen, &finalLen) != 1)
        {
            // Authentication failed — wrong password or corrupted data
            LOG_CORE_ERROR("KeyEncryption::Decrypt: authentication failed (wrong password or corrupted data)");
            return {};
        }

        plaintext.resize(outLen + finalLen);
        return plaintext;
    }
} // namespace AIAssistant
