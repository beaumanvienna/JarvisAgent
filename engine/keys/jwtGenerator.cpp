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

#include <algorithm>
#include <chrono>
#include <sstream>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include "engine.h"
#include "keys/jwtGenerator.h"
#include "log/secretRedactor.h"

namespace AIAssistant
{
    static constexpr int MIN_RSA_KEY_BITS = 2048;
    static constexpr int SNOWFLAKE_JWT_EXPIRY_SECONDS = 3600; // 1 hour

    std::string JwtGenerator::Base64UrlEncode(std::vector<uint8_t> const& data)
    {
        // Standard base64 table
        static char const* const TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string result;
        result.reserve(((data.size() + 2) / 3) * 4);

        for (size_t i = 0; i < data.size(); i += 3)
        {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
            if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

            result += TABLE[(n >> 18) & 0x3F];
            result += TABLE[(n >> 12) & 0x3F];
            result += (i + 1 < data.size()) ? TABLE[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < data.size()) ? TABLE[n & 0x3F] : '=';
        }

        // Convert to URL-safe: + → -, / → _, strip padding =
        for (char& c : result)
        {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        result.erase(std::remove(result.begin(), result.end(), '='), result.end());

        return result;
    }

    std::string JwtGenerator::Base64UrlEncode(std::string const& data)
    {
        return Base64UrlEncode(std::vector<uint8_t>(data.begin(), data.end()));
    }

    std::string JwtGenerator::Generate(std::string const& headerJson, std::string const& payloadJson,
                                        std::string const& privateKeyPem, std::string& errorMessage)
    {
        // Load RSA private key from PEM
        BIO* bio = BIO_new_mem_buf(privateKeyPem.data(), static_cast<int>(privateKeyPem.size()));
        if (!bio)
        {
            errorMessage = "Failed to create BIO for private key";
            return {};
        }

        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);

        if (!pkey)
        {
            errorMessage = "Failed to parse RSA private key PEM";
            return {};
        }

        // Enforce minimum key size
        int keyBits = EVP_PKEY_bits(pkey);
        if (keyBits < MIN_RSA_KEY_BITS)
        {
            errorMessage = "RSA key too small (" + std::to_string(keyBits) + " bits, minimum " +
                           std::to_string(MIN_RSA_KEY_BITS) + ")";
            EVP_PKEY_free(pkey);
            return {};
        }

        // Build the signing input: base64url(header).base64url(payload)
        std::string signingInput = Base64UrlEncode(headerJson) + "." + Base64UrlEncode(payloadJson);

        // Sign with RS256 (RSA + SHA-256)
        EVP_MD_CTX* mdCtx = EVP_MD_CTX_new();
        if (!mdCtx)
        {
            errorMessage = "Failed to create EVP_MD_CTX";
            EVP_PKEY_free(pkey);
            return {};
        }

        std::string jwt;

        if (EVP_DigestSignInit(mdCtx, nullptr, EVP_sha256(), nullptr, pkey) != 1)
        {
            errorMessage = "EVP_DigestSignInit failed";
        }
        else if (EVP_DigestSignUpdate(mdCtx, signingInput.data(), signingInput.size()) != 1)
        {
            errorMessage = "EVP_DigestSignUpdate failed";
        }
        else
        {
            // Get signature length
            size_t sigLen = 0;
            if (EVP_DigestSignFinal(mdCtx, nullptr, &sigLen) != 1)
            {
                errorMessage = "EVP_DigestSignFinal (length query) failed";
            }
            else
            {
                std::vector<uint8_t> signature(sigLen);
                if (EVP_DigestSignFinal(mdCtx, signature.data(), &sigLen) != 1)
                {
                    errorMessage = "EVP_DigestSignFinal (sign) failed";
                }
                else
                {
                    signature.resize(sigLen);
                    jwt = signingInput + "." + Base64UrlEncode(signature);
                }
            }
        }

        EVP_MD_CTX_free(mdCtx);

        // Clear private key material
        EVP_PKEY_free(pkey);

        if (!jwt.empty())
        {
            SecretRedactor::Get().AddSecret(jwt);
        }

        return jwt;
    }

    std::string JwtGenerator::ComputePublicKeyFingerprint(std::string const& privateKeyPem, std::string& errorMessage)
    {
        // Load private key
        BIO* bio = BIO_new_mem_buf(privateKeyPem.data(), static_cast<int>(privateKeyPem.size()));
        if (!bio)
        {
            errorMessage = "Failed to create BIO for fingerprint";
            return {};
        }

        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey)
        {
            errorMessage = "Failed to parse private key for fingerprint";
            return {};
        }

        // Extract DER-encoded public key
        int derLen = i2d_PUBKEY(pkey, nullptr);
        if (derLen <= 0)
        {
            errorMessage = "Failed to get public key DER length";
            EVP_PKEY_free(pkey);
            return {};
        }

        std::vector<uint8_t> derBuf(derLen);
        uint8_t* derPtr = derBuf.data();
        i2d_PUBKEY(pkey, &derPtr);
        EVP_PKEY_free(pkey);

        // SHA-256 hash of the DER-encoded public key
        std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
        SHA256(derBuf.data(), derBuf.size(), hash.data());

        return "SHA256:" + Base64UrlEncode(hash);
    }

    std::string JwtGenerator::GenerateSnowflakeJwt(std::string const& account, std::string const& user,
                                                    std::string const& privateKeyPem, std::string& errorMessage)
    {
        // Compute public key fingerprint for the "sub" claim
        std::string fingerprint = ComputePublicKeyFingerprint(privateKeyPem, errorMessage);
        if (fingerprint.empty())
        {
            return {};
        }

        // Uppercase account and user per Snowflake convention
        std::string upperAccount = account;
        std::string upperUser = user;
        std::transform(upperAccount.begin(), upperAccount.end(), upperAccount.begin(), ::toupper);
        std::transform(upperUser.begin(), upperUser.end(), upperUser.begin(), ::toupper);

        // Strip region suffix if present (e.g., "XY12345.us-east-1" → "XY12345")
        auto dotPos = upperAccount.find('.');
        std::string accountLocator = (dotPos != std::string::npos) ? upperAccount.substr(0, dotPos) : upperAccount;

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        int64_t expiry = now + SNOWFLAKE_JWT_EXPIRY_SECONDS;

        std::string qualifiedUser = accountLocator + "." + upperUser;

        // Build header
        std::string header = R"({"alg":"RS256","typ":"JWT"})";

        // Build payload
        std::ostringstream payload;
        payload << "{";
        payload << "\"iss\":\"" << qualifiedUser << "." << fingerprint << "\",";
        payload << "\"sub\":\"" << qualifiedUser << "\",";
        payload << "\"iat\":" << now << ",";
        payload << "\"exp\":" << expiry;
        payload << "}";

        return Generate(header, payload.str(), privateKeyPem, errorMessage);
    }
} // namespace AIAssistant
