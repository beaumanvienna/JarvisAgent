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

#include "curlWrapper/awsSigV4.h"

#include "curlWrapper/credValidation.h"
#include "engine.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace AIAssistant
{
    namespace
    {
        constexpr char kAlgorithm[] = "AWS4-HMAC-SHA256";

        // RAII wrapper that zeros its buffer via OPENSSL_cleanse on scope exit.
        // Wraps the intermediate signing-key material derived from the AWS
        // secret_access_key (kSecret, kDate, kRegion, kService, kSigning) so the
        // secret-derived bytes don't linger on the heap after Sign() returns.
        // Same posture as the master-password KEK wrapping in keyEncryption.cpp
        // via ScopedKey<N>; this is the std::vector variant.
        //
        // OPENSSL_cleanse uses memory barriers to prevent the compiler from
        // dead-store-eliminating the zero — std::memset would be optimised away.
        struct ScopedSecretBytes
        {
            std::vector<unsigned char> m_Data;

            ScopedSecretBytes() = default;
            explicit ScopedSecretBytes(std::vector<unsigned char>&& v) : m_Data(std::move(v)) {}

            ScopedSecretBytes(ScopedSecretBytes&&) noexcept = default;
            ScopedSecretBytes& operator=(ScopedSecretBytes&&) noexcept = default;

            ScopedSecretBytes(ScopedSecretBytes const&) = delete;
            ScopedSecretBytes& operator=(ScopedSecretBytes const&) = delete;

            ~ScopedSecretBytes()
            {
                if (!m_Data.empty())
                {
                    OPENSSL_cleanse(m_Data.data(), m_Data.size());
                }
            }
        };

        std::string ToHex(unsigned char const* data, size_t len)
        {
            static char const* hex = "0123456789abcdef";
            std::string out(len * 2, '\0');
            for (size_t i = 0; i < len; ++i)
            {
                out[2 * i]     = hex[(data[i] >> 4) & 0x0F];
                out[2 * i + 1] = hex[data[i] & 0x0F];
            }
            return out;
        }

        // Returns the hex digest, or empty string on OpenSSL failure (rare —
        // SHA256 only returns NULL on impossible argument conditions).  Caller
        // detects failure by checking output length / emptiness.
        std::string Sha256Hex(std::string const& data)
        {
            unsigned char hash[SHA256_DIGEST_LENGTH];
            unsigned char const* result = SHA256(reinterpret_cast<unsigned char const*>(data.data()), data.size(), hash);
            if (result == nullptr)
            {
                LOG_CORE_ERROR("SigV4: SHA256() returned NULL — output would be uninitialised, returning empty");
                return {};
            }
            return ToHex(hash, SHA256_DIGEST_LENGTH);
        }

        // Returns the MAC bytes, or empty vector on OpenSSL failure.  Caller
        // detects failure via .empty() and propagates up to Sign().
        std::vector<unsigned char> HmacSha256(std::vector<unsigned char> const& key, std::string const& data)
        {
            std::vector<unsigned char> out(SHA256_DIGEST_LENGTH);
            unsigned int len = 0;
            unsigned char const* result =
                HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                     reinterpret_cast<unsigned char const*>(data.data()), data.size(), out.data(), &len);
            if (result == nullptr)
            {
                LOG_CORE_ERROR("SigV4: HMAC() returned NULL — output would be uninitialised, returning empty");
                return {};
            }
            out.resize(len);
            return out;
        }

        std::vector<unsigned char> StringToBytes(std::string const& s)
        {
            return std::vector<unsigned char>(s.begin(), s.end());
        }

        // RFC 3986 unreserved-character URI encoding for SigV4 canonical components.
        // - encodeSlash=false leaves '/' alone (used for path segments).
        // - encodeSlash=true percent-encodes '/' (used for query params).
        std::string UriEncode(std::string const& s, bool encodeSlash)
        {
            static char const* hex = "0123456789ABCDEF";
            std::string out;
            out.reserve(s.size() * 3);
            for (unsigned char c : s)
            {
                bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                                  || c == '-' || c == '_' || c == '.' || c == '~';
                if (unreserved || (c == '/' && !encodeSlash))
                {
                    out += static_cast<char>(c);
                }
                else
                {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 0x0F];
                }
            }
            return out;
        }

        struct ParsedUrl
        {
            std::string m_Host;
            std::string m_Path;
            std::string m_Query;
        };

        // Minimal URL parser: scheme://host/path?query — sufficient for AWS service URLs.
        ParsedUrl ParseUrl(std::string const& url)
        {
            ParsedUrl out;
            auto schemeEnd = url.find("://");
            size_t pos = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;

            auto pathStart = url.find('/', pos);
            auto queryStart = url.find('?', pos);

            if (pathStart == std::string::npos)
            {
                out.m_Host = url.substr(pos, (queryStart == std::string::npos) ? std::string::npos : queryStart - pos);
                out.m_Path = "/";
            }
            else
            {
                out.m_Host = url.substr(pos, pathStart - pos);
                if (queryStart == std::string::npos)
                {
                    out.m_Path = url.substr(pathStart);
                }
                else
                {
                    out.m_Path = url.substr(pathStart, queryStart - pathStart);
                    out.m_Query = url.substr(queryStart + 1);
                }
            }
            return out;
        }

        // AWS canonical query string: split on '&', URL-encode key+value separately,
        // sort by encoded key (then by encoded value for duplicate keys), rejoin.
        std::string CanonicalQuery(std::string const& query)
        {
            if (query.empty()) return {};
            std::vector<std::pair<std::string, std::string>> pairs;
            size_t i = 0;
            while (i < query.size())
            {
                size_t amp = query.find('&', i);
                std::string kv = query.substr(i, (amp == std::string::npos) ? std::string::npos : amp - i);
                size_t eq = kv.find('=');
                std::string k = (eq == std::string::npos) ? kv : kv.substr(0, eq);
                std::string v = (eq == std::string::npos) ? std::string{} : kv.substr(eq + 1);
                pairs.emplace_back(UriEncode(k, true), UriEncode(v, true));
                if (amp == std::string::npos) break;
                i = amp + 1;
            }
            std::sort(pairs.begin(), pairs.end());
            std::string out;
            for (size_t j = 0; j < pairs.size(); ++j)
            {
                if (j) out += '&';
                out += pairs[j].first;
                out += '=';
                out += pairs[j].second;
            }
            return out;
        }

        // Limited to leading/trailing space + tab.  AWS canonical-request spec also
        // requires (a) stripping CR/LF from header values and (b) collapsing runs of
        // sequential spaces inside header values to a single space.  Today's call
        // sites pass only host (no internal whitespace), ISO dates, and base64 hashes
        // — none can hit those gaps.  Extend if a future header value can carry CRLF
        // or multi-space sequences (multi-line headers, free-text headers).
        std::string TrimSpace(std::string const& s)
        {
            auto b = s.find_first_not_of(" \t");
            if (b == std::string::npos) return {};
            auto e = s.find_last_not_of(" \t");
            return s.substr(b, e - b + 1);
        }

        std::string FormatAmzDateNow()
        {
            std::time_t now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &now);
#else
            gmtime_r(&now, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
            return buf;
        }
    } // namespace

    SigV4Signer::SignedHeaders SigV4Signer::Sign(Inputs const& in)
    {
        SignedHeaders out;
        ParsedUrl const url = ParseUrl(in.m_Url);
        out.m_Host = url.m_Host;

        out.m_AmzDate = in.m_AmzDate.empty() ? FormatAmzDateNow() : in.m_AmzDate;
        std::string const dateStamp = out.m_AmzDate.substr(0, 8); // YYYYMMDD

        out.m_ContentSha256 = Sha256Hex(in.m_Body);
        if (out.m_ContentSha256.empty())
        {
            // SHA256 failure — leave Authorization empty so Apply() detects and rejects.
            return out;
        }
        out.m_SecurityToken = in.m_SessionToken;

        // Canonical headers: lowercase name + ':' + trimmed value + '\n', sorted by name.
        std::map<std::string, std::string> headerMap;
        headerMap["host"] = TrimSpace(out.m_Host);
        headerMap["x-amz-content-sha256"] = out.m_ContentSha256;
        headerMap["x-amz-date"] = out.m_AmzDate;
        if (!in.m_SessionToken.empty())
        {
            headerMap["x-amz-security-token"] = in.m_SessionToken;
        }

        std::string canonicalHeaders;
        std::string signedHeaders;
        for (auto const& [k, v] : headerMap)
        {
            canonicalHeaders += k + ":" + v + "\n";
            if (!signedHeaders.empty()) signedHeaders += ';';
            signedHeaders += k;
        }

        std::string const canonicalRequest =
            in.m_Method + "\n" +
            UriEncode(url.m_Path, false) + "\n" +
            CanonicalQuery(url.m_Query) + "\n" +
            canonicalHeaders + "\n" +
            signedHeaders + "\n" +
            out.m_ContentSha256;

        std::string const credentialScope = dateStamp + "/" + in.m_Region + "/" + in.m_Service + "/aws4_request";

        std::string const canonicalRequestHash = Sha256Hex(canonicalRequest);
        if (canonicalRequestHash.empty())
        {
            return out;
        }

        std::string const stringToSign =
            std::string(kAlgorithm) + "\n" +
            out.m_AmzDate + "\n" +
            credentialScope + "\n" +
            canonicalRequestHash;

        // Derive signing key.  Each intermediate is wrapped in ScopedSecretBytes so
        // OPENSSL_cleanse zeros the heap buffer on scope exit (including on throw
        // from any subsequent string concat or HMAC call).  The chain is:
        //   kSecret  = "AWS4" + secret_access_key
        //   kDate    = HMAC(kSecret,  YYYYMMDD)
        //   kRegion  = HMAC(kDate,    region)
        //   kService = HMAC(kRegion,  service)
        //   kSigning = HMAC(kService, "aws4_request")
        ScopedSecretBytes const kSecret{StringToBytes("AWS4" + in.m_SecretKey)};
        ScopedSecretBytes const kDate{HmacSha256(kSecret.m_Data, dateStamp)};
        if (kDate.m_Data.empty()) return out;
        ScopedSecretBytes const kRegion{HmacSha256(kDate.m_Data, in.m_Region)};
        if (kRegion.m_Data.empty()) return out;
        ScopedSecretBytes const kService{HmacSha256(kRegion.m_Data, in.m_Service)};
        if (kService.m_Data.empty()) return out;
        ScopedSecretBytes const kSigning{HmacSha256(kService.m_Data, "aws4_request")};
        if (kSigning.m_Data.empty()) return out;

        auto signature = HmacSha256(kSigning.m_Data, stringToSign);
        if (signature.empty()) return out;
        std::string const signatureHex = ToHex(signature.data(), signature.size());

        std::ostringstream auth;
        auth << kAlgorithm
             << " Credential=" << in.m_AccessKey << "/" << credentialScope
             << ", SignedHeaders=" << signedHeaders
             << ", Signature=" << signatureHex;
        out.m_Authorization = auth.str();
        return out;
    }

    bool SigV4Signer::RunSelfTest()
    {
        bool ok = true;

        // (1) SHA256 of empty string — universally known constant.
        constexpr char const* kEmptyHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        if (Sha256Hex("") != kEmptyHash)
        {
            LOG_CORE_ERROR("SigV4 self-test (1) Sha256Hex(\"\"): got '{}', expected '{}'", Sha256Hex(""), kEmptyHash);
            ok = false;
        }

        // (2) Signing-key derivation from AWS docs:
        //   https://docs.aws.amazon.com/general/latest/gr/signature-v4-examples.html
        //   secret="wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", date=20120215, region=us-east-1, service=iam
        //   kSigning hex == f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d
        {
            auto kSecret = StringToBytes("AWS4wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
            auto kDate = HmacSha256(kSecret, "20120215");
            auto kRegion = HmacSha256(kDate, "us-east-1");
            auto kService = HmacSha256(kRegion, "iam");
            auto kSigning = HmacSha256(kService, "aws4_request");
            std::string const got = ToHex(kSigning.data(), kSigning.size());
            constexpr char const* kExpected = "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d";
            if (got != kExpected)
            {
                LOG_CORE_ERROR("SigV4 self-test (2) signing-key derivation: got '{}', expected '{}'", got, kExpected);
                ok = false;
            }
        }

        // (3) Determinism: same inputs produce same outputs.
        {
            Inputs in;
            in.m_Method = "POST";
            in.m_Url = "https://bedrock-runtime.us-east-1.amazonaws.com/model/anthropic.claude-3-haiku-20240307-v1:0/invoke";
            in.m_Body = R"({"anthropic_version":"bedrock-2023-05-31","max_tokens":50,"messages":[{"role":"user","content":"hi"}]})";
            in.m_AccessKey = "AKIDEXAMPLE";
            in.m_SecretKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
            in.m_Region = "us-east-1";
            in.m_Service = "bedrock";
            in.m_AmzDate = "20240101T120000Z";

            auto a = Sign(in);
            auto b = Sign(in);
            if (a.m_Authorization != b.m_Authorization || a.m_ContentSha256 != b.m_ContentSha256)
            {
                LOG_CORE_ERROR("SigV4 self-test (3) determinism: two Sign() calls on identical inputs differ");
                ok = false;
            }
            // Spot-check the structure of the Authorization header.
            std::string const authPrefix = "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20240101/us-east-1/bedrock/aws4_request, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=";
            if (a.m_Authorization.rfind(authPrefix, 0) != 0)
            {
                LOG_CORE_ERROR("SigV4 self-test (3) authorization-header structure mismatch: '{}'", a.m_Authorization);
                ok = false;
            }

            // (4) Full-chain known-answer test: locks in the EXACT signature for the
            // determinism inputs above so any regression in canonical-request assembly,
            // UriEncode, CanonicalQuery, or stringToSign concatenation is caught.
            // The expected signature was captured from a trusted run after the
            // OPENSSL_cleanse + RAII changes landed; the inputs use the AWS-published
            // example secret (wJalrXUtn...EXAMPLEKEY) and AKIDEXAMPLE access key, so a
            // future cross-check against an independent SigV4 implementation (aws-cli,
            // boto3) using the same canonical-request shape would validate against this
            // value.  Test #2 already proves the key-derivation chain matches AWS's
            // reference; this test proves the canonical-request → string-to-sign →
            // final-signature pipeline is stable.
            constexpr char const* kExpectedSignature =
                "74fe2fddce07dc62e797273ccbb0ff5c11e9cd431afe83624c327f1c92695501";
            std::string const expectedAuth = authPrefix + kExpectedSignature;
            if (a.m_Authorization != expectedAuth)
            {
                LOG_CORE_ERROR("SigV4 self-test (4) full-chain signature mismatch:\n  got:      '{}'\n  expected: '{}'",
                               a.m_Authorization, expectedAuth);
                ok = false;
            }
        }

        if (ok)
        {
            LOG_CORE_INFO("SigV4 self-test: PASSED");
        }
        else
        {
            LOG_CORE_ERROR("SigV4 self-test: FAILED — Bedrock requests will be rejected by AWS");
        }
        return ok;
    }

    bool SigV4Signer::Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& outHeaders,
                            std::string& errorMessage) const
    {
        Inputs in;
        in.m_Method = "POST";
        in.m_Url = q.m_Url;
        in.m_Body = q.m_Data;
        in.m_AccessKey = q.m_ApiKey; // m_ApiKey is reused as access_key_id for SigV4

        auto getParam = [&](char const* key) -> std::string
        {
            auto it = q.m_Params.find(key);
            return (it != q.m_Params.end()) ? it->second : std::string{};
        };

        in.m_SecretKey = getParam("secret_access_key");
        in.m_SessionToken = getParam("session_token");
        in.m_Region = getParam("region");
        // Service defaults to bedrock; can be overridden via params for future AWS services.
        in.m_Service = getParam("service");
        if (in.m_Service.empty()) in.m_Service = "bedrock";

        // Reject empty OR whitespace-only on each required field.  Pre-fix the only
        // gate was `.empty()` and the failure was masked by an inline LOG_CORE_ERROR
        // plus a sentinel "Authorization: AWS4-HMAC-SHA256 MISSING-CREDENTIALS"
        // header — anti-debugging armor that turned a local config bug into an
        // opaque 401 from AWS.
        if (IsBlank(in.m_AccessKey))
        {
            errorMessage = "SigV4: access_key_id (m_ApiKey) is empty or whitespace";
            return false;
        }
        if (IsBlank(in.m_SecretKey))
        {
            errorMessage = "SigV4: secret_access_key is empty or whitespace";
            return false;
        }
        if (IsBlank(in.m_Region))
        {
            errorMessage = "SigV4: region is empty or whitespace";
            return false;
        }

        SignedHeaders const out = Sign(in);
        if (out.m_Authorization.empty())
        {
            // Sign() bailed because an OpenSSL primitive returned NULL.  The error
            // line was already logged inside Sha256Hex / HmacSha256; report the
            // category up so the caller's structured ERROR carries run context.
            errorMessage = "SigV4: OpenSSL HMAC/SHA256 primitive failed during signing";
            return false;
        }
        outHeaders.push_back("Host: " + out.m_Host);
        outHeaders.push_back("X-Amz-Date: " + out.m_AmzDate);
        outHeaders.push_back("X-Amz-Content-Sha256: " + out.m_ContentSha256);
        outHeaders.push_back("Authorization: " + out.m_Authorization);
        if (!out.m_SecurityToken.empty())
        {
            outHeaders.push_back("X-Amz-Security-Token: " + out.m_SecurityToken);
        }
        return true;
    }
} // namespace AIAssistant
