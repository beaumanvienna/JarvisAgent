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

#include "curlWrapper/authSigner.h"

#include <string>

namespace AIAssistant
{
    // AWS Signature Version 4. Hand-rolled on top of OpenSSL HMAC-SHA256/SHA256.
    // Reference: https://docs.aws.amazon.com/general/latest/gr/sigv4_signing.html
    class SigV4Signer final : public IAuthSigner
    {
    public:
        [[nodiscard]] bool Apply(CurlWrapper::QueryData const& queryData, std::vector<std::string>& outHeaders,
                                 std::string& errorMessage) const override;

        // Inputs grouped for the test seam below.
        struct Inputs
        {
            std::string m_Method;        // "POST"
            std::string m_Url;           // full URL
            std::string m_Body;          // request body bytes (may be empty)
            std::string m_AccessKey;     // AKIA...
            std::string m_SecretKey;     // 40-char secret
            std::string m_SessionToken;  // optional STS session token
            std::string m_Region;        // e.g. "us-east-1"
            std::string m_Service;       // e.g. "bedrock"
            std::string m_AmzDate;       // ISO8601 basic: YYYYMMDDTHHMMSSZ. Empty = use current UTC.
        };

        struct SignedHeaders
        {
            std::string m_Host;
            std::string m_AmzDate;
            std::string m_ContentSha256;
            std::string m_Authorization;
            std::string m_SecurityToken; // empty when no session token
        };

        // Pure function: deterministic given Inputs (including m_AmzDate). Used by
        // the known-answer unit test against AWS's published test vectors.
        static SignedHeaders Sign(Inputs const& in);

        // Self-test using AWS-published intermediate vectors (SHA256 of empty input,
        // signing-key derivation from the AWS docs example) plus a determinism check
        // on Sign(). Returns true on pass, false on fail. Logs failures.
        // Called once at engine startup in debug builds — see engine.cpp.
        static bool RunSelfTest();
    };
} // namespace AIAssistant
