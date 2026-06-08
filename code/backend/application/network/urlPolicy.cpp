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

#include "network/urlPolicy.h"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

namespace AIAssistant::UrlPolicy
{
    namespace
    {
        // Rejection counters surfaced via /api/debug/signals.  Relaxed ordering
        // is sufficient — the counter is for operator visibility, not for
        // synchronisation between the rejection site and the reader.
        std::atomic<std::uint64_t> g_UrlPolicyRejectionCount{0};
        std::atomic<std::uint64_t> g_CredentialedPlaintextHttpRejectionCount{0};

        // Defense-in-depth ceiling on URL length.  Mirrors the 2048-char cap
        // in ValidatePublicHttpEndpoint — well above legitimate AI endpoint
        // URLs (the longest shipped example is ~120 chars) and well below the
        // 8 KB getaddrinfo input cap typical on POSIX.
        constexpr std::size_t kMaxUrlBytes = 2048;

        // Cap a host string before it is embedded in an error / log detail.
        // The host derives from an externally-supplied URL (up to kMaxUrlBytes);
        // echoing it unbounded would let a hostile interface URL bloat log
        // lines and error responses.  256 is well above any real hostname.
        std::string CapForLog(std::string const& host)
        {
            constexpr std::size_t kMaxHostLogBytes = 256;
            if (host.size() <= kMaxHostLogBytes)
            {
                return host;
            }
            return host.substr(0, kMaxHostLogBytes) + "…(truncated)";
        }

        // IPv4 loopback = 127.0.0.0/8.
        bool IsIp4Loopback(std::uint32_t ip4HostOrder) noexcept
        {
            std::uint8_t const a = static_cast<std::uint8_t>((ip4HostOrder >> 24) & 0xFFu);
            return a == 127;
        }

        // IPv6 loopback = ::1.  IPv4-mapped IPv6 (::ffff:a.b.c.d) is normalised
        // here so an address that resolves as ::ffff:127.0.0.1 counts as
        // loopback for policy purposes — mirrors the IsIp6Rejected pattern in
        // workflowRuntimeManager.cpp.
        bool IsIp6Loopback(in6_addr const& addr) noexcept
        {
            static std::uint8_t const kLoopback[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
            if (std::memcmp(addr.s6_addr, kLoopback, 16) == 0)
            {
                return true;
            }
            // IPv4-mapped ::ffff:0:0/96.
            static std::uint8_t const kV4MapPrefix[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
            if (std::memcmp(addr.s6_addr, kV4MapPrefix, 12) == 0)
            {
                std::uint32_t const v4 =
                    (std::uint32_t(addr.s6_addr[12]) << 24) |
                    (std::uint32_t(addr.s6_addr[13]) << 16) |
                    (std::uint32_t(addr.s6_addr[14]) <<  8) |
                     std::uint32_t(addr.s6_addr[15]);
                return IsIp4Loopback(v4);
            }
            return false;
        }

        // Extract the host substring from a URL with a known scheme prefix
        // already consumed.  Returns empty string on parse failure.  Handles
        // optional `userinfo@` (stripped), optional `:port` (stripped), and
        // bracketed IPv6 literals (`[::1]:8080`).  Stops at the first `/`,
        // `?`, `#`, or end-of-string.
        std::string ExtractHost(std::string_view afterScheme)
        {
            if (afterScheme.empty())
            {
                return {};
            }

            // Slice host[:port] up to the first /, ?, # or end of string.
            std::size_t pathStart = afterScheme.size();
            for (std::size_t i = 0; i < afterScheme.size(); ++i)
            {
                char const c = afterScheme[i];
                if (c == '/' || c == '?' || c == '#')
                {
                    pathStart = i;
                    break;
                }
            }
            std::string_view authority = afterScheme.substr(0, pathStart);
            if (authority.empty())
            {
                return {};
            }

            // Strip optional userinfo (user:pass@).
            std::size_t const atPos = authority.find('@');
            if (atPos != std::string_view::npos)
            {
                authority.remove_prefix(atPos + 1);
            }
            if (authority.empty())
            {
                return {};
            }

            // Bracketed IPv6: [::1]:8080.
            if (authority.front() == '[')
            {
                std::size_t const closeBracket = authority.find(']');
                if (closeBracket == std::string_view::npos)
                {
                    return {};
                }
                return std::string(authority.substr(1, closeBracket - 1));
            }

            // Otherwise strip trailing :port if present.
            std::size_t const colonPos = authority.find(':');
            if (colonPos == std::string_view::npos)
            {
                return std::string(authority);
            }
            return std::string(authority.substr(0, colonPos));
        }
    } // namespace

    std::string_view Describe(UrlPolicyErrorCode code)
    {
        switch (code)
        {
            case UrlPolicyErrorCode::MalformedUrl:               return "url_policy_violation";
            case UrlPolicyErrorCode::SchemeRejected:             return "url_policy_violation";
            case UrlPolicyErrorCode::NonLoopbackHttp:            return "url_policy_violation";
            case UrlPolicyErrorCode::UnresolvedHost:             return "url_policy_violation";
            case UrlPolicyErrorCode::CredentialedPlaintextHttp:  return "credentialed_plaintext_http";
            case UrlPolicyErrorCode::UnknownError:               return "unknown_error";
        }
        return "unhandled_code";
    }

    bool IsPlaintextHttpUrl(std::string_view url) noexcept
    {
        // 7 == strlen("http://"); using a literal compare avoids a strncmp
        // call on the hot dispatch path.  Case-sensitive on purpose — every
        // URL we emit / accept canonicalises scheme to lowercase elsewhere.
        return url.size() >= 7 &&
               url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' &&
               url[4] == ':' && url[5] == '/' && url[6] == '/';
    }

    std::expected<void, UrlPolicyError>
    ValidateAiInterfaceUrl(std::string const& url, std::string const& keyName)
    {
        if (url.empty())
        {
            return std::unexpected(UrlPolicyError::Make(
                UrlPolicyErrorCode::MalformedUrl,
                "url is empty"));
        }
        if (url.size() > kMaxUrlBytes)
        {
            return std::unexpected(UrlPolicyError::Make(
                UrlPolicyErrorCode::MalformedUrl,
                "url exceeds 2048 bytes"));
        }

        // Scheme split: only http:// and https:// are valid; everything else
        // (ws://, file://, ftp://, missing scheme) is SchemeRejected so the
        // operator gets "scheme is not allowed" rather than a misleading
        // "malformed".
        bool isHttps = false;
        std::string_view afterScheme;
        if (url.size() >= 8 && url.compare(0, 8, "https://") == 0)
        {
            isHttps = true;
            afterScheme = std::string_view(url).substr(8);
        }
        else if (url.size() >= 7 && url.compare(0, 7, "http://") == 0)
        {
            isHttps = false;
            afterScheme = std::string_view(url).substr(7);
        }
        else
        {
            // No scheme separator at all → MalformedUrl (operator typo).
            // Recognisable scheme but not in the allowlist → SchemeRejected.
            if (url.find("://") == std::string::npos)
            {
                return std::unexpected(UrlPolicyError::Make(
                    UrlPolicyErrorCode::MalformedUrl,
                    "url missing '://' separator"));
            }
            return std::unexpected(UrlPolicyError::Make(
                UrlPolicyErrorCode::SchemeRejected,
                "scheme must be https:// or http:// (loopback only)"));
        }

        if (isHttps)
        {
            // HTTPS: scheme provides transport encryption.  Further endpoint
            // validation (SSRF gate, host charset, etc.) lives in
            // ConnectorHttp::ValidatePublicHttpEndpoint for connector URLs;
            // AI interface URLs hit that gate at dispatch time via libcurl's
            // own DNS / TLS path.  No further work here.
            return {};
        }

        // Plain http:// from here on.  Reject credentialed-plaintext BEFORE
        // DNS — cheap, and we don't want the typo + credential to leak
        // together in the rejection log when the URL happens to resolve to
        // something that would also be NonLoopbackHttp.
        if (!keyName.empty())
        {
            return std::unexpected(UrlPolicyError::Make(
                UrlPolicyErrorCode::CredentialedPlaintextHttp,
                "interfaces with a key_name require https:// — "
                "plain http:// would expose the credential in transit"));
        }

        std::string const host = ExtractHost(afterScheme);
        if (host.empty())
        {
            return std::unexpected(UrlPolicyError::Make(
                UrlPolicyErrorCode::MalformedUrl,
                "url has empty host"));
        }
        // Bounded copy for any error/log detail below (the raw `host` is still
        // used verbatim for getaddrinfo).
        std::string const hostForLog = CapForLog(host);

        // DNS resolve + per-address loopback check.  Allowlist posture: every
        // resolved address MUST be loopback or the URL is rejected.  Closes
        // the DNS-rebinding-style attack where a hostname returns both
        // 127.0.0.1 and a routable IP.
        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICSERV;
        struct addrinfo* results = nullptr;
        int const gai = ::getaddrinfo(host.c_str(), "80", &hints, &results);
        if (gai != 0 || results == nullptr)
        {
#if defined(_WIN32)
            char const* const gaiMessage = (gai == 0) ? "no addresses" : ::gai_strerrorA(gai);
#else
            char const* const gaiMessage = (gai == 0) ? "no addresses" : ::gai_strerror(gai);
#endif
            if (results != nullptr) { ::freeaddrinfo(results); }
            return std::unexpected(UrlPolicyError::Make(
                UrlPolicyErrorCode::UnresolvedHost,
                std::string("DNS resolution failed for host '") + hostForLog + "': " + gaiMessage));
        }

        for (struct addrinfo* it = results; it != nullptr; it = it->ai_next)
        {
            if (it->ai_family == AF_INET)
            {
                auto const* sin = reinterpret_cast<struct sockaddr_in const*>(it->ai_addr);
                std::uint32_t const ip4 = ntohl(sin->sin_addr.s_addr);
                if (!IsIp4Loopback(ip4))
                {
                    char buf[INET_ADDRSTRLEN] = {};
                    ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
                    ::freeaddrinfo(results);
                    return std::unexpected(UrlPolicyError::Make(
                        UrlPolicyErrorCode::NonLoopbackHttp,
                        std::string("plain http:// host '") + hostForLog +
                        "' resolves to non-loopback address " + buf +
                        " — http:// is loopback-only"));
                }
            }
            else if (it->ai_family == AF_INET6)
            {
                auto const* sin6 = reinterpret_cast<struct sockaddr_in6 const*>(it->ai_addr);
                if (!IsIp6Loopback(sin6->sin6_addr))
                {
                    char buf[INET6_ADDRSTRLEN] = {};
                    ::inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
                    ::freeaddrinfo(results);
                    return std::unexpected(UrlPolicyError::Make(
                        UrlPolicyErrorCode::NonLoopbackHttp,
                        std::string("plain http:// host '") + hostForLog +
                        "' resolves to non-loopback IPv6 " + buf +
                        " — http:// is loopback-only"));
                }
            }
            // Address families other than AF_INET / AF_INET6 (e.g. AF_UNIX
            // for `nss-myhostname`) shouldn't appear with AI_NUMERICSERV +
            // SOCK_STREAM, but if one slips through we treat it as a non-
            // loopback rejection rather than silently accepting it.
            else
            {
                ::freeaddrinfo(results);
                return std::unexpected(UrlPolicyError::Make(
                    UrlPolicyErrorCode::NonLoopbackHttp,
                    std::string("plain http:// host '") + hostForLog +
                    "' resolved to unsupported address family"));
            }
        }
        ::freeaddrinfo(results);
        return {};
    }

    void RecordUrlPolicyRejection() noexcept
    {
        g_UrlPolicyRejectionCount.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordCredentialedPlaintextHttpRejection() noexcept
    {
        g_CredentialedPlaintextHttpRejectionCount.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t GetUrlPolicyRejectionCount() noexcept
    {
        return g_UrlPolicyRejectionCount.load(std::memory_order_relaxed);
    }

    std::uint64_t GetCredentialedPlaintextHttpRejectionCount() noexcept
    {
        return g_CredentialedPlaintextHttpRejectionCount.load(std::memory_order_relaxed);
    }

} // namespace AIAssistant::UrlPolicy
