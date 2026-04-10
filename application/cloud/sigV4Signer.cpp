/* Copyright (c) 2026 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "cloud/sigV4Signer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace AIAssistant
{
    // =====================================================================
    // SHA-256 and HMAC-SHA256
    // =====================================================================

    std::string SigV4Signer::Sha256Hex(std::string const& data)
    {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen = 0;

        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, data.data(), data.size());
        EVP_DigestFinal_ex(ctx, hash, &hashLen);
        EVP_MD_CTX_free(ctx);

        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < hashLen; ++i)
        {
            hex << std::setw(2) << static_cast<int>(hash[i]);
        }
        return hex.str();
    }

    std::string const& SigV4Signer::EmptyPayloadHash()
    {
        static std::string const hash = Sha256Hex("");
        return hash;
    }

    std::string SigV4Signer::HmacSha256(std::string const& key, std::string const& data)
    {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int resultLen = 0;

        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<unsigned char const*>(data.data()), data.size(), result, &resultLen);

        return std::string(reinterpret_cast<char const*>(result), resultLen);
    }

    std::string SigV4Signer::HmacSha256Hex(std::string const& key, std::string const& data)
    {
        std::string raw = HmacSha256(key, data);

        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (unsigned char c : raw)
        {
            hex << std::setw(2) << static_cast<int>(c);
        }
        return hex.str();
    }

    // =====================================================================
    // URL parsing and encoding
    // =====================================================================

    SigV4Signer::UrlParts SigV4Signer::ParseUrl(std::string const& url)
    {
        UrlParts parts;

        // Skip scheme (http:// or https://)
        size_t start = url.find("://");
        if (start == std::string::npos)
        {
            start = 0;
        }
        else
        {
            start += 3;
        }

        // Find host end
        size_t pathStart = url.find('/', start);
        size_t queryStart = url.find('?', start);

        if (pathStart == std::string::npos)
        {
            parts.m_Host = url.substr(start);
            parts.m_Path = "/";
        }
        else
        {
            parts.m_Host = url.substr(start, pathStart - start);

            if (queryStart != std::string::npos && queryStart > pathStart)
            {
                parts.m_Path = url.substr(pathStart, queryStart - pathStart);
                parts.m_Query = url.substr(queryStart + 1);
            }
            else
            {
                parts.m_Path = url.substr(pathStart);
            }
        }

        // Strip port from host for the Host header
        size_t colonPos = parts.m_Host.find(':');
        if (colonPos != std::string::npos)
        {
            std::string port = parts.m_Host.substr(colonPos + 1);
            if (port == "443" || port == "80")
            {
                parts.m_Host = parts.m_Host.substr(0, colonPos);
            }
        }

        return parts;
    }

    std::string SigV4Signer::UriEncode(std::string const& value, bool encodeSlash)
    {
        std::ostringstream encoded;
        encoded << std::hex << std::uppercase << std::setfill('0');

        for (unsigned char c : value)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~')
            {
                encoded << c;
            }
            else if (c == '/' && !encodeSlash)
            {
                encoded << c;
            }
            else
            {
                encoded << '%' << std::setw(2) << static_cast<int>(c);
            }
        }

        return encoded.str();
    }

    std::string SigV4Signer::CanonicalQueryString(std::string const& query)
    {
        if (query.empty())
        {
            return "";
        }

        // Parse key=value pairs and sort
        std::vector<std::pair<std::string, std::string>> params;
        std::istringstream iss(query);
        std::string pair;
        while (std::getline(iss, pair, '&'))
        {
            size_t eq = pair.find('=');
            if (eq != std::string::npos)
            {
                params.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
            }
            else
            {
                params.emplace_back(pair, "");
            }
        }

        std::sort(params.begin(), params.end());

        std::ostringstream canonical;
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i > 0)
            {
                canonical << '&';
            }
            canonical << UriEncode(params[i].first) << '=' << UriEncode(params[i].second);
        }

        return canonical.str();
    }

    // =====================================================================
    // Time helpers
    // =====================================================================

    std::string SigV4Signer::GetAmzDate()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t nowT = std::chrono::system_clock::to_time_t(now);
        std::tm utcTm{};
        gmtime_r(&nowT, &utcTm);

        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utcTm);
        return buf;
    }

    std::string SigV4Signer::GetDateStamp()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t nowT = std::chrono::system_clock::to_time_t(now);
        std::tm utcTm{};
        gmtime_r(&nowT, &utcTm);

        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &utcTm);
        return buf;
    }

    // =====================================================================
    // Sign — the main SigV4 signing method
    // =====================================================================

    SigV4Signer::SignedRequest SigV4Signer::Sign(std::string const& method, std::string const& url,
                                                  std::string const& region, std::string const& service,
                                                  std::string const& accessKeyId, std::string const& secretKey,
                                                  std::string const& payloadHash,
                                                  std::map<std::string, std::string> const& extraHeaders)
    {
        UrlParts urlParts = ParseUrl(url);
        std::string amzDate = GetAmzDate();
        std::string dateStamp = amzDate.substr(0, 8); // YYYYMMDD

        // Step 1: Build canonical headers (must include host and x-amz-date)
        std::map<std::string, std::string> headersToSign;
        headersToSign["host"] = urlParts.m_Host;
        headersToSign["x-amz-date"] = amzDate;
        headersToSign["x-amz-content-sha256"] = payloadHash;

        for (auto const& [key, value] : extraHeaders)
        {
            std::string lowerKey = key;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            headersToSign[lowerKey] = value;
        }

        // Build canonical headers string and signed headers list
        std::string canonicalHeaders;
        std::string signedHeaders;
        for (auto const& [key, value] : headersToSign)
        {
            canonicalHeaders += key + ":" + value + "\n";
            if (!signedHeaders.empty())
            {
                signedHeaders += ";";
            }
            signedHeaders += key;
        }

        // Step 2: Canonical request
        std::string canonicalUri = UriEncode(urlParts.m_Path, false);
        std::string canonicalQueryString = CanonicalQueryString(urlParts.m_Query);

        std::string canonicalRequest = method + "\n" + canonicalUri + "\n" + canonicalQueryString + "\n" +
                                       canonicalHeaders + "\n" + signedHeaders + "\n" + payloadHash;

        // Step 3: String to sign
        std::string credentialScope = dateStamp + "/" + region + "/" + service + "/aws4_request";
        std::string stringToSign =
            "AWS4-HMAC-SHA256\n" + amzDate + "\n" + credentialScope + "\n" + Sha256Hex(canonicalRequest);

        // Step 4: Signing key (derived from secret)
        std::string kDate = HmacSha256("AWS4" + secretKey, dateStamp);
        std::string kRegion = HmacSha256(kDate, region);
        std::string kService = HmacSha256(kRegion, service);
        std::string kSigning = HmacSha256(kService, "aws4_request");

        // Step 5: Signature
        std::string signature = HmacSha256Hex(kSigning, stringToSign);

        // Step 6: Authorization header
        std::string authorization = "AWS4-HMAC-SHA256 Credential=" + accessKeyId + "/" + credentialScope +
                                    ", SignedHeaders=" + signedHeaders + ", Signature=" + signature;

        // Build output headers
        SignedRequest result;
        result.m_Headers["Authorization"] = authorization;
        result.m_Headers["X-Amz-Date"] = amzDate;
        result.m_Headers["X-Amz-Content-Sha256"] = payloadHash;
        result.m_Headers["Host"] = urlParts.m_Host;

        return result;
    }
} // namespace AIAssistant
