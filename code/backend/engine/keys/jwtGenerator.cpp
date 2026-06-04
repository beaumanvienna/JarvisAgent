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
#include <cctype>
#include <memory>
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

    // RSA RS256 JWT header is fixed — never accept a caller-supplied header that
    // could lie about the algorithm and induce alg-confusion at a misconfigured verifier.
    static constexpr char const* RS256_HEADER_JSON = R"({"alg":"RS256","typ":"JWT"})";

    namespace
    {
        // RAII for OpenSSL EVP_PKEY*.  Holding a raw pointer across allocating string
        // operations (Base64UrlEncode return, signingInput concat, std::vector<uint8_t>
        // construction) leaks the key on std::bad_alloc.  Same pattern as ScopedKey /
        // EvpCipherCtxPtr in keyEncryption.cpp; file-local because no third user yet.
        struct EvpPkeyDeleter
        {
            void operator()(EVP_PKEY* key) const noexcept
            {
                if (key) EVP_PKEY_free(key);
            }
        };
        using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

        struct EvpMdCtxDeleter
        {
            void operator()(EVP_MD_CTX* ctx) const noexcept
            {
                if (ctx) EVP_MD_CTX_free(ctx);
            }
        };
        using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

        // Parse a PEM private key and validate it as an RSA key of sufficient size.
        // Returns null and populates errorMessage on any failure.
        EvpPkeyPtr ParseAndValidateRsaPem(std::string const& privateKeyPem, std::string& errorMessage)
        {
            BIO* bio = BIO_new_mem_buf(privateKeyPem.data(), static_cast<int>(privateKeyPem.size()));
            if (!bio)
            {
                errorMessage = "Failed to create BIO for private key";
                return {};
            }
            EvpPkeyPtr pkey(PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr));
            BIO_free(bio);

            if (!pkey)
            {
                errorMessage = "Failed to parse private key PEM";
                return {};
            }

            // Reject EC / DSA / Ed25519 / Ed448 / X25519 etc.  EVP_DigestSign with a
            // non-RSA key would produce a signature of the wrong shape while the JWT
            // header still claims RS256 — a verifier that trusts the header would
            // accept a signature it didn't actually validate against the right scheme.
            int const keyId = EVP_PKEY_id(pkey.get());
            if (keyId != EVP_PKEY_RSA)
            {
                errorMessage = "Private key is not RSA (EVP_PKEY_id=" + std::to_string(keyId) +
                               "); RS256 requires an RSA key";
                return {};
            }

            int const keyBits = EVP_PKEY_bits(pkey.get());
            if (keyBits < MIN_RSA_KEY_BITS)
            {
                errorMessage = "RSA key too small (" + std::to_string(keyBits) + " bits, minimum " +
                               std::to_string(MIN_RSA_KEY_BITS) + ")";
                return {};
            }

            return pkey;
        }
    } // namespace

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

    bool JwtGenerator::Generate(std::string const& payloadJson, std::string const& privateKeyPem,
                                 SecureString& outJwt, std::string& errorMessage)
    {
        EvpPkeyPtr pkey = ParseAndValidateRsaPem(privateKeyPem, errorMessage);
        if (!pkey) return false;

        // Build the signing input: base64url(RS256_header).base64url(payload).
        // Header is fixed internally — no caller-supplied header that could lie about alg.
        std::string const signingInput = Base64UrlEncode(std::string(RS256_HEADER_JSON)) + "." +
                                         Base64UrlEncode(payloadJson);

        EvpMdCtxPtr mdCtx(EVP_MD_CTX_new());
        if (!mdCtx)
        {
            errorMessage = "Failed to create EVP_MD_CTX";
            return false;
        }

        if (EVP_DigestSignInit(mdCtx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1)
        {
            errorMessage = "EVP_DigestSignInit failed";
            return false;
        }
        if (EVP_DigestSignUpdate(mdCtx.get(), signingInput.data(), signingInput.size()) != 1)
        {
            errorMessage = "EVP_DigestSignUpdate failed";
            return false;
        }

        size_t sigLen = 0;
        if (EVP_DigestSignFinal(mdCtx.get(), nullptr, &sigLen) != 1)
        {
            errorMessage = "EVP_DigestSignFinal (length query) failed";
            return false;
        }

        std::vector<uint8_t> signature(sigLen);
        if (EVP_DigestSignFinal(mdCtx.get(), signature.data(), &sigLen) != 1)
        {
            errorMessage = "EVP_DigestSignFinal (sign) failed";
            return false;
        }
        signature.resize(sigLen);

        // Build the JWT directly into the SecureString output via Build — no local
        // std::string materialisation.  SecretRedactor::AddSecret takes std::string_view
        // so the SecureString::Get() view feeds the redactor without a std::string copy
        // on this side either (the redactor's internal store may copy; that's its own
        // surface).
        outJwt.Build({signingInput, ".", Base64UrlEncode(signature)});
        SecretRedactor::Get().AddSecret(outJwt.Get());
        return true;
    }

    std::string JwtGenerator::ComputePublicKeyFingerprint(std::string const& privateKeyPem, std::string& errorMessage)
    {
        // Reuse the same parse + RSA-validation gate as Generate — Snowflake's
        // fingerprint is over the RSA public key, so an EC/DSA key here is a setup
        // bug we want to catch before computing a meaningless fingerprint.
        EvpPkeyPtr pkey = ParseAndValidateRsaPem(privateKeyPem, errorMessage);
        if (!pkey) return {};

        int const derLen = i2d_PUBKEY(pkey.get(), nullptr);
        if (derLen <= 0)
        {
            errorMessage = "Failed to get public key DER length";
            return {};
        }

        std::vector<uint8_t> derBuf(static_cast<size_t>(derLen));
        uint8_t* derPtr = derBuf.data();
        int const written = i2d_PUBKEY(pkey.get(), &derPtr);
        if (written != derLen)
        {
            errorMessage = "i2d_PUBKEY second-call wrote " + std::to_string(written) + " bytes, expected " +
                           std::to_string(derLen);
            return {};
        }

        // SHA-256 hash of the DER-encoded public key
        std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
        SHA256(derBuf.data(), derBuf.size(), hash.data());

        // Snowflake expects standard Base64 (with + / =) for the fingerprint, not URL-safe.
        static char const* const TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        b64.reserve(((hash.size() + 2) / 3) * 4);
        for (size_t i = 0; i < hash.size(); i += 3)
        {
            uint32_t n = static_cast<uint32_t>(hash[i]) << 16;
            if (i + 1 < hash.size()) n |= static_cast<uint32_t>(hash[i + 1]) << 8;
            if (i + 2 < hash.size()) n |= static_cast<uint32_t>(hash[i + 2]);
            b64 += TABLE[(n >> 18) & 0x3F];
            b64 += TABLE[(n >> 12) & 0x3F];
            b64 += (i + 1 < hash.size()) ? TABLE[(n >> 6) & 0x3F] : '=';
            b64 += (i + 2 < hash.size()) ? TABLE[n & 0x3F] : '=';
        }

        return "SHA256:" + b64;
    }

    bool JwtGenerator::GenerateSnowflakeJwt(std::string const& account, std::string const& user,
                                             std::string const& privateKeyPem,
                                             SecureString& outJwt, std::string& errorMessage)
    {
        // Compute public key fingerprint for the "sub" claim
        std::string fingerprint = ComputePublicKeyFingerprint(privateKeyPem, errorMessage);
        if (fingerprint.empty())
        {
            return false;
        }

        // Uppercase account and user per Snowflake convention.
        // Cast to unsigned char before std::toupper — passing a negative signed char
        // is undefined behaviour (cppreference / std::toupper).  Snowflake account
        // identifiers are alphanumeric ASCII so it never fires today, but the cast
        // makes the contract explicit.
        auto upperAscii = [](char c) -> char
        {
            return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        };
        std::string upperAccount = account;
        std::string upperUser = user;
        std::transform(upperAccount.begin(), upperAccount.end(), upperAccount.begin(), upperAscii);
        std::transform(upperUser.begin(), upperUser.end(), upperUser.begin(), upperAscii);

        // Strip region suffix if present (e.g., "XY12345.us-east-1" → "XY12345")
        auto dotPos = upperAccount.find('.');
        std::string accountLocator = (dotPos != std::string::npos) ? upperAccount.substr(0, dotPos) : upperAccount;

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        int64_t expiry = now + SNOWFLAKE_JWT_EXPIRY_SECONDS;

        std::string qualifiedUser = accountLocator + "." + upperUser;

        // Build payload — header is built internally by Generate.
        std::ostringstream payload;
        payload << "{";
        payload << "\"iss\":\"" << qualifiedUser << "." << fingerprint << "\",";
        payload << "\"sub\":\"" << qualifiedUser << "\",";
        payload << "\"iat\":" << now << ",";
        payload << "\"exp\":" << expiry;
        payload << "}";

        return Generate(payload.str(), privateKeyPem, outJwt, errorMessage);
    }
} // namespace AIAssistant
