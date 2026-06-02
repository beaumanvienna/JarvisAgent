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

#include "auxiliary/sha256.h"
#include "curlWrapper/credValidation.h"
#include "engine.h"
#include "keys/credential.h"
#include "keys/scopedSecretBytes.h"

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

        // Returns the MAC bytes, or empty vector on OpenSSL failure.  Caller
        // detects failure via .empty() and propagates up to Sign().  Data is
        // string_view so callers can pass std::string / std::string_view /
        // SecureString::Get() views without a defensive copy — consistent
        // with cloud-side AzureSharedKey + the rest of the SecureString-only
        // HTTP path.
        std::vector<unsigned char> HmacSha256(std::vector<unsigned char> const& key, std::string_view data)
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

        // Payload hash: caller may override (S3 UNSIGNED-PAYLOAD or pre-hashed upload
        // body); otherwise derive from m_Body.  The Bedrock dispatch path always
        // leaves the override empty so the derived hash matches the body bytes the
        // upstream caller will actually send.
        out.m_ContentSha256 = in.m_ContentSha256Override.empty()
                                  ? EngineCore::Sha256Hex(in.m_Body)
                                  : in.m_ContentSha256Override;
        if (out.m_ContentSha256.empty())
        {
            // SHA256 failure — leave Authorization empty so Apply() detects and rejects.
            return out;
        }
        out.m_SecurityToken.Set(in.m_SessionToken.Get());

        // Canonical headers: lowercase name + ':' + trimmed value + '\n', sorted by
        // name.  Build the sorted set as std::map<string, string_view> — the map's
        // alphabetical ordering is the AWS canonical-request requirement.  Values
        // are string_views so the session token (a SecureString::Get() view) and
        // the extras (caller-owned strings) don't need a defensive copy.
        //
        // The session-token VALUE (when present) is the only secret in this set;
        // the final canonicalHeaders string is built into a SecureString
        // (mlock'd, zero-on-destruct) so the secret bytes don't sit in a heap-
        // resident std::string for the duration of the Sign() call.
        std::string const trimmedHost = TrimSpace(out.m_Host);
        bool const hasSessionToken = !in.m_SessionToken.IsEmpty();
        std::string_view const sessionTokenView =
            hasSessionToken ? in.m_SessionToken.Get() : std::string_view{};

        std::map<std::string, std::string_view> canonicalHeaderMap;
        canonicalHeaderMap["host"] = trimmedHost;
        canonicalHeaderMap["x-amz-content-sha256"] = out.m_ContentSha256;
        canonicalHeaderMap["x-amz-date"] = out.m_AmzDate;
        if (hasSessionToken)
        {
            canonicalHeaderMap["x-amz-security-token"] = sessionTokenView;
        }

        // Extra headers (Content-Type for S3 PUT, etc.) are lowercased and AWS-
        // trimmed.  Storage for the lowercased keys + trimmed values is in
        // parallel vectors so the map's string_view values remain valid for the
        // lifetime of the build below.
        std::vector<std::string> extraKeys;
        std::vector<std::string> extraValues;
        extraKeys.reserve(in.m_ExtraHeadersToSign.size());
        extraValues.reserve(in.m_ExtraHeadersToSign.size());
        for (auto const& [k, v] : in.m_ExtraHeadersToSign)
        {
            std::string lowerKey = k;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            extraKeys.push_back(std::move(lowerKey));
            extraValues.push_back(TrimSpace(v));
            canonicalHeaderMap[extraKeys.back()] = extraValues.back();
        }

        // Emit canonical-headers + signed-headers list in map order.  Pieces vector
        // is sized 4 per header (name + ':' + value + '\n').
        std::vector<std::string_view> pieces;
        pieces.reserve(canonicalHeaderMap.size() * 4);
        std::string signedHeaders;
        for (auto const& [k, v] : canonicalHeaderMap)
        {
            pieces.push_back(k);
            pieces.push_back(":");
            pieces.push_back(v);
            pieces.push_back("\n");
            if (!signedHeaders.empty()) signedHeaders += ';';
            signedHeaders += k;
        }
        SecureString canonicalHeaders;
        canonicalHeaders.Build(pieces);

        // canonicalRequest contains canonicalHeaders, so when the session token is present
        // canonicalRequest carries the secret too.  Build it as a SecureString so the
        // request string itself is mlock'd + zero-on-destruct.  Sha256Hex accepts a
        // string_view so canonicalRequest.Get() feeds the hash function directly
        // without materialising.
        std::string const uriEncoded = UriEncode(url.m_Path, false);
        std::string const canonicalQuery = CanonicalQuery(url.m_Query);
        SecureString canonicalRequest;
        canonicalRequest.Build({
            in.m_Method, "\n",
            uriEncoded, "\n",
            canonicalQuery, "\n",
            canonicalHeaders.Get(), "\n",
            signedHeaders, "\n",
            out.m_ContentSha256,
        });

        std::string const credentialScope = dateStamp + "/" + in.m_Region + "/" + in.m_Service + "/aws4_request";

        std::string const canonicalRequestHash = EngineCore::Sha256Hex(canonicalRequest.Get());
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
        //
        // Build kSecret as a raw byte vector to avoid the std::string heap intermediate
        // that "AWS4" + in.m_SecretKey would create — that string would contain the full
        // raw secret on the heap until ScopedSecretBytes is constructed.
        std::vector<unsigned char> kSecretBytes;
        {
            auto const prefix = std::string_view{"AWS4"};
            auto const secret = in.m_SecretKey.Get();
            kSecretBytes.reserve(prefix.size() + secret.size());
            kSecretBytes.insert(kSecretBytes.end(), prefix.begin(), prefix.end());
            kSecretBytes.insert(kSecretBytes.end(), secret.begin(), secret.end());
        }
        ScopedSecretBytes const kSecret{std::move(kSecretBytes)};
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
        if (EngineCore::Sha256Hex("") != kEmptyHash)
        {
            LOG_CORE_ERROR("SigV4 self-test (1) Sha256Hex(\"\"): got '{}', expected '{}'", EngineCore::Sha256Hex(""), kEmptyHash);
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
            in.m_SecretKey.Set("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
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

    bool SigV4Signer::Apply(CurlWrapper::QueryData const& q,
                            std::vector<std::string>& publicHeaders,
                            SecureString& secretHeader,
                            std::string& errorMessage) const
    {
        // Typed credential is mandatory on the SigV4 path — AiRequestPool populates
        // it from the resolved AwsCredential under KeyManager's lock; a null pointer
        // here means either the caller forgot to populate it or the AuthStyle was
        // routed to SigV4 without an AwsCredential underneath.  Either way, fail
        // closed before we even try to read the secret material.
        if (q.m_AwsCredential == nullptr)
        {
            errorMessage = "SigV4: QueryData::m_AwsCredential is null (caller must populate before dispatch)";
            return false;
        }

        Inputs in;
        in.m_Method = "POST";
        in.m_Url = q.m_Url;
        in.m_Body = q.m_Data;
        in.m_AccessKey = q.m_AwsCredential->m_AccessKeyId;
        in.m_SecretKey.Set(q.m_AwsCredential->m_SecretAccessKey.Get());
        in.m_SessionToken.Set(q.m_AwsCredential->m_SessionToken.Get());
        in.m_Region = q.m_AwsCredential->m_Region;
        // Mock paths (test_bedrock_sigv4) override the AmzDate from the fixture's
        // .meta.json so the captured Authorization matches a locked KAT value.
        // Empty on live paths — Sign() falls back to FormatAmzDateNow().
        in.m_AmzDate = q.m_AmzDateOverride;

        // Service defaults to bedrock; can be overridden via the non-secret
        // m_Params map for future AWS services.  Not on the typed credential
        // because service is a request-target attribute, not a secret.
        auto serviceIt = q.m_Params.find("service");
        in.m_Service = (serviceIt != q.m_Params.end() && !serviceIt->second.empty())
                           ? serviceIt->second
                           : "bedrock";

        // Reject empty OR whitespace-only on each required field.  Pre-fix the only
        // gate was `.empty()` and the failure was masked by an inline LOG_CORE_ERROR
        // plus a sentinel "Authorization: AWS4-HMAC-SHA256 MISSING-CREDENTIALS"
        // header — anti-debugging armor that turned a local config bug into an
        // opaque 401 from AWS.
        if (IsBlank(in.m_AccessKey))
        {
            errorMessage = "SigV4: AwsCredential::m_AccessKeyId is empty or whitespace";
            return false;
        }
        if (IsBlank(in.m_SecretKey.Get()))
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
        publicHeaders.push_back("Host: " + out.m_Host);
        publicHeaders.push_back("X-Amz-Date: " + out.m_AmzDate);
        publicHeaders.push_back("X-Amz-Content-Sha256: " + out.m_ContentSha256);
        publicHeaders.push_back("Authorization: " + out.m_Authorization);
        if (!out.m_SecurityToken.IsEmpty())
        {
            // X-Amz-Security-Token IS the raw STS session token on the wire.  Route through
            // the SecureString secretHeader slot so the secret never appears in a plain
            // std::string heap allocation between Sign() and curl_slist_append.
            secretHeader.Format("X-Amz-Security-Token: ", out.m_SecurityToken.Get());
        }
        return true;
    }
} // namespace AIAssistant
