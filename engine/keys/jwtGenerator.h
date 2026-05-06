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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AIAssistant
{
    // RSA RS256 JWT creation via OpenSSL EVP_DigestSign.
    // Reusable for Snowflake, Google service accounts, and any RS256-signed JWT.
    //
    // Algorithm is fixed at RS256 (RSA + SHA-256, PKCS#1 v1.5 padding).  The header is
    // built internally — callers cannot pass a header that lies about the algorithm.
    // Private keys must be RSA (EC / DSA / Ed25519 keys are rejected) and at least 2048
    // bits.
    class JwtGenerator
    {
    public:
        // Generate an RS256-signed JWT for the given payload claims.
        // The header `{"alg":"RS256","typ":"JWT"}` is built internally.
        // privateKeyPem must be an RSA private key in PEM format (minimum 2048 bits).
        // Returns the signed JWT string (header.payload.signature) or empty string on failure.
        static std::string Generate(std::string const& payloadJson, std::string const& privateKeyPem,
                                    std::string& errorMessage);

        // Convenience: generate a Snowflake-specific JWT.
        // Account and user are uppercased per Snowflake convention.
        // The JWT has a 1-hour expiry.
        static std::string GenerateSnowflakeJwt(std::string const& account, std::string const& user,
                                                 std::string const& privateKeyPem, std::string& errorMessage);

    private:
        static std::string Base64UrlEncode(std::vector<uint8_t> const& data);
        static std::string Base64UrlEncode(std::string const& data);

        // Compute the SHA-256 fingerprint of an RSA public key (for Snowflake JWT "sub" claim).
        static std::string ComputePublicKeyFingerprint(std::string const& privateKeyPem, std::string& errorMessage);
    };
} // namespace AIAssistant
