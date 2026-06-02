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
#include "keys/secureString.h"

#include <map>
#include <string>

namespace AIAssistant
{
    // AWS Signature Version 4. Hand-rolled on top of OpenSSL HMAC-SHA256/SHA256.
    // Reference: https://docs.aws.amazon.com/general/latest/gr/sigv4_signing.html
    class SigV4Signer final : public IAuthSigner
    {
    public:
        // SigV4's Authorization line is a signature (HMAC-SHA256 of the canonical
        // request) — derived from but not containing the raw secret — so it goes
        // into publicHeaders.  secretHeader stays empty for SigV4.
        [[nodiscard]] bool Apply(CurlWrapper::QueryData const& queryData,
                                 std::vector<std::string>& publicHeaders,
                                 SecureString& secretHeader,
                                 std::string& errorMessage) const override;

        // Inputs grouped for the test seam below.
        // m_SecretKey + m_SessionToken are SecureString so the secret bytes don't
        // materialise into a plain std::string heap allocation between the caller
        // (typically Apply, which pulls from AwsCredential::m_SecretAccessKey /
        // m_SessionToken) and the HMAC chain.  m_AccessKey stays std::string —
        // public per AWS conventions, logged for audit.
        //
        // m_ContentSha256Override: when non-empty, used verbatim as the request
        // payload hash; when empty, Sha256Hex(m_Body) is computed.  S3 callers
        // that already have the payload hash (uploads where the body was hashed
        // up front, list/delete requests where the empty-string hash is the
        // canonical answer) pass it explicitly; Bedrock leaves it empty and the
        // signer derives it from m_Body.
        //
        // m_ExtraHeadersToSign: optional non-secret headers that must be folded
        // into the canonical-headers + SignedHeaders=... list (e.g. Content-Type
        // on S3 PUT).  Names are lowercased before sorting; values are trimmed
        // per the AWS SigV4 canonical-request spec.  Empty for Bedrock dispatch.
        struct Inputs
        {
            std::string  m_Method;        // "POST" / "GET" / "PUT" / "DELETE" / "HEAD"
            std::string  m_Url;           // full URL
            std::string  m_Body;          // request body bytes (may be empty)
            std::string  m_AccessKey;     // AKIA... (public per AWS conventions)
            SecureString m_SecretKey;     // 40-char secret access key
            SecureString m_SessionToken;  // optional STS session token (secret)
            std::string  m_Region;        // e.g. "us-east-1"
            std::string  m_Service;       // e.g. "bedrock" / "s3"
            std::string  m_AmzDate;       // ISO8601 basic: YYYYMMDDTHHMMSSZ. Empty = use current UTC.
            std::string  m_ContentSha256Override; // empty = compute from m_Body
            std::map<std::string, std::string> m_ExtraHeadersToSign;
        };

        // SignedHeaders carries the wire-format header values.  m_SecurityToken
        // is a SecureString because its value IS the raw STS session token (the
        // X-Amz-Security-Token header echoes it verbatim).  m_Authorization is
        // a derived HMAC-SHA256 signature — not the raw secret — and stays
        // std::string for downstream slist append.
        struct SignedHeaders
        {
            std::string  m_Host;
            std::string  m_AmzDate;
            std::string  m_ContentSha256;
            std::string  m_Authorization;
            SecureString m_SecurityToken; // empty when no session token
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
