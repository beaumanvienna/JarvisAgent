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

#include "cloud/azureSharedKeySigner.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace AIAssistant
{
    // =====================================================================
    // HMAC-SHA256 and Base64
    // =====================================================================

    std::string AzureSharedKeySigner::HmacSha256(std::string const& key, std::string const& data)
    {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int resultLen = 0;

        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<unsigned char const*>(data.data()), data.size(), result, &resultLen);

        return std::string(reinterpret_cast<char const*>(result), resultLen);
    }

    std::string AzureSharedKeySigner::Base64Encode(std::string const& data)
    {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, mem);

        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, data.data(), static_cast<int>(data.size()));
        (void)BIO_flush(b64);

        BUF_MEM* bufPtr = nullptr;
        BIO_get_mem_ptr(b64, &bufPtr);

        std::string encoded(bufPtr->data, bufPtr->length);
        BIO_free_all(b64);
        return encoded;
    }

    std::string AzureSharedKeySigner::Base64Decode(std::string const& encoded)
    {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
        mem = BIO_push(b64, mem);

        BIO_set_flags(mem, BIO_FLAGS_BASE64_NO_NL);

        std::vector<unsigned char> buffer(encoded.size());
        int decodedLen = BIO_read(mem, buffer.data(), static_cast<int>(buffer.size()));
        BIO_free_all(mem);

        if (decodedLen < 0)
        {
            return {};
        }

        return std::string(reinterpret_cast<char const*>(buffer.data()), static_cast<size_t>(decodedLen));
    }

    // =====================================================================
    // URL parsing
    // =====================================================================

    AzureSharedKeySigner::UrlParts AzureSharedKeySigner::ParseUrl(std::string const& url)
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

        return parts;
    }

    // =====================================================================
    // Canonicalized headers and resource
    // =====================================================================

    std::string AzureSharedKeySigner::CanonicalizedHeaders(std::map<std::string, std::string> const& headers)
    {
        // Collect all x-ms-* headers, lowercase keys, sort, join with newlines.
        std::map<std::string, std::string> msHeaders;
        for (auto const& [key, value] : headers)
        {
            std::string lowerKey = key;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lowerKey.compare(0, 5, "x-ms-") == 0)
            {
                msHeaders[lowerKey] = value;
            }
        }

        std::string result;
        for (auto const& [key, value] : msHeaders)
        {
            result += key + ":" + value + "\n";
        }
        return result;
    }

    std::string AzureSharedKeySigner::CanonicalizedResource(std::string const& accountName, std::string const& path,
                                                             std::string const& query)
    {
        std::string result = "/" + accountName + path;

        if (!query.empty())
        {
            // Parse query parameters, sort by key, append as \nkey:value
            std::map<std::string, std::string> params;
            std::istringstream iss(query);
            std::string pair;
            while (std::getline(iss, pair, '&'))
            {
                size_t eq = pair.find('=');
                if (eq != std::string::npos)
                {
                    std::string key = pair.substr(0, eq);
                    std::string value = pair.substr(eq + 1);
                    // Lowercase the parameter name for Azure canonical resource
                    std::transform(key.begin(), key.end(), key.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    params[key] = value;
                }
                else
                {
                    std::string key = pair;
                    std::transform(key.begin(), key.end(), key.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    params[key] = "";
                }
            }

            for (auto const& [key, value] : params)
            {
                result += "\n" + key + ":" + value;
            }
        }

        return result;
    }

    // =====================================================================
    // Time helper
    // =====================================================================

    std::string AzureSharedKeySigner::GetRfc1123Date()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t nowT = std::chrono::system_clock::to_time_t(now);
        std::tm utcTm{};
#ifdef _WIN32
        gmtime_s(&utcTm, &nowT);
#else
        gmtime_r(&nowT, &utcTm);
#endif

        char buf[64];
        std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &utcTm);
        return buf;
    }

    // =====================================================================
    // Sign — the main Azure Shared Key signing method
    // =====================================================================

    AzureSharedKeySigner::SignedRequest AzureSharedKeySigner::Sign(std::string const& method, std::string const& url,
                                                                   std::string const& accountName,
                                                                   std::string const& accountKey,
                                                                   std::map<std::string, std::string> const& extraHeaders,
                                                                   size_t contentLength)
    {
        UrlParts urlParts = ParseUrl(url);
        std::string msDate = GetRfc1123Date();
        static std::string const kApiVersion = "2024-11-04";

        // Build the set of headers that will be signed
        std::map<std::string, std::string> allHeaders = extraHeaders;
        allHeaders["x-ms-date"] = msDate;
        allHeaders["x-ms-version"] = kApiVersion;

        // Extract Content-* headers for the string-to-sign (empty string if not present)
        auto getHeader = [&allHeaders](std::string const& name) -> std::string
        {
            auto it = allHeaders.find(name);
            return (it != allHeaders.end()) ? it->second : "";
        };

        // Content-Length: empty string for zero (Azure spec)
        std::string contentLengthStr;
        if (contentLength > 0)
        {
            contentLengthStr = std::to_string(contentLength);
        }

        // Build string-to-sign per Azure Storage Shared Key spec
        // https://learn.microsoft.com/en-us/rest/api/storageservices/authorize-with-shared-key
        std::string stringToSign = method + "\n" +                    // VERB
                                   getHeader("Content-Encoding") + "\n" +
                                   getHeader("Content-Language") + "\n" +
                                   contentLengthStr + "\n" +          // Content-Length (empty if 0)
                                   getHeader("Content-MD5") + "\n" +
                                   getHeader("Content-Type") + "\n" +
                                   "" + "\n" +                        // Date (empty — using x-ms-date)
                                   getHeader("If-Modified-Since") + "\n" +
                                   getHeader("If-Match") + "\n" +
                                   getHeader("If-None-Match") + "\n" +
                                   getHeader("If-Unmodified-Since") + "\n" +
                                   getHeader("Range") + "\n" +
                                   CanonicalizedHeaders(allHeaders) +
                                   CanonicalizedResource(accountName, urlParts.m_Path, urlParts.m_Query);

        // Decode the Base64-encoded account key to raw bytes
        std::string rawKey = Base64Decode(accountKey);

        // HMAC-SHA256 sign and Base64-encode the signature
        std::string signature = Base64Encode(HmacSha256(rawKey, stringToSign));

        // Build output headers
        SignedRequest result;
        result.m_Headers["Authorization"] = "SharedKey " + accountName + ":" + signature;
        result.m_Headers["x-ms-date"] = msDate;
        result.m_Headers["x-ms-version"] = kApiVersion;

        return result;
    }
} // namespace AIAssistant
