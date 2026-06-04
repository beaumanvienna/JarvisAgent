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

#include "cloud/connectorHttp.h"

#include <cctype>

#include <curl/curl.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <atomic>

#include "engine.h"

#include "curlWrapper/curlWrapper.h"

namespace AIAssistant::ConnectorHttp
{
    namespace
    {
        // Lifetime counter: incremented every time `OpensocketStrictCallback`
        // refuses a connection because the resolved IP is local-network.
        // Surfaced via `/api/debug/signals` (admin-gated, DEBUG-only) for live
        // monitoring — operators investigating "are we seeing DNS-time SSRF
        // attempts?" can read this counter without grepping `log/log.txt`.
        // Per-instance forensic detail (timestamp + actual IP) stays in the
        // security log, which the dashboard's log view already surfaces.
        std::atomic<std::uint64_t> g_DnsResolvedIpRejectionCount{0};
        std::atomic<std::uint64_t> g_EndpointSsrfRejectionCount{0};
        std::atomic<std::uint64_t> g_CredentialCrlfRejectionCount{0};
        std::atomic<std::uint64_t> g_InputValidationRejectionCount{0};
        std::atomic<std::uint64_t> g_PostgresInvalidSslmodeRejectionCount{0};
        std::atomic<std::uint64_t> g_PostgresForbiddenParamRejectionCount{0};
    } // namespace

    std::uint64_t GetDnsResolvedIpRejectionCount()
    {
        return g_DnsResolvedIpRejectionCount.load(std::memory_order_relaxed);
    }
    std::uint64_t GetEndpointSsrfRejectionCount()
    {
        return g_EndpointSsrfRejectionCount.load(std::memory_order_relaxed);
    }
    std::uint64_t GetCredentialCrlfRejectionCount()
    {
        return g_CredentialCrlfRejectionCount.load(std::memory_order_relaxed);
    }
    std::uint64_t GetInputValidationRejectionCount()
    {
        return g_InputValidationRejectionCount.load(std::memory_order_relaxed);
    }
    std::uint64_t GetPostgresInvalidSslmodeRejectionCount()
    {
        return g_PostgresInvalidSslmodeRejectionCount.load(std::memory_order_relaxed);
    }
    std::uint64_t GetPostgresForbiddenParamRejectionCount()
    {
        return g_PostgresForbiddenParamRejectionCount.load(std::memory_order_relaxed);
    }

    void IncrementEndpointSsrfRejection()
    {
        g_EndpointSsrfRejectionCount.fetch_add(1, std::memory_order_relaxed);
    }
    void IncrementCredentialCrlfRejection()
    {
        g_CredentialCrlfRejectionCount.fetch_add(1, std::memory_order_relaxed);
    }
    void IncrementInputValidationRejection()
    {
        g_InputValidationRejectionCount.fetch_add(1, std::memory_order_relaxed);
    }
    void IncrementPostgresInvalidSslmodeRejection()
    {
        g_PostgresInvalidSslmodeRejectionCount.fetch_add(1, std::memory_order_relaxed);
    }
    void IncrementPostgresForbiddenParamRejection()
    {
        g_PostgresForbiddenParamRejectionCount.fetch_add(1, std::memory_order_relaxed);
    }

    std::size_t BoundedStringWriteCallback(void* contents, std::size_t size, std::size_t nmemb, void* userp)
    {
        auto* buf = static_cast<std::string*>(userp);
        std::size_t const incoming = size * nmemb;
        if (buf->size() + incoming > kMaxConnectorResponseBytes)
        {
            return 0;
        }
        buf->append(static_cast<char*>(contents), incoming);
        return incoming;
    }

    namespace
    {
        // Post-resolve SSRF callback for HTTPS URLs.  libcurl invokes this
        // after DNS resolution, before the TCP connection — `address` carries
        // the resolved IP.  We stringify it and run IsLocalNetworkHost; if the
        // resolved IP is in the loopback / RFC1918 / link-local / cloud-metadata
        // ranges, we return CURL_SOCKET_BAD which aborts the connection.
        //
        // Closes the SSRF vector where a public DNS name (`evil.example.com`)
        // resolves to an internal IP at attacker-controlled DNS time —
        // ValidatePublicHttpEndpoint is purely syntactic and can't catch this.
        //
        // Installed only on `https://` URLs.  For `http://` URLs (dev-mode
        // local-net opt-in like MinIO / Azurite / local Mailpit), this
        // callback is NOT installed — the user opted into local-net by
        // choosing http.  Mirrors email's `allowLocal = !useSsl` posture.
        curl_socket_t OpensocketStrictCallback(void* /*clientp*/, curlsocktype purpose,
                                                struct curl_sockaddr* address)
        {
            if (purpose != CURLSOCKTYPE_IPCXN)
            {
                // Non-TCP-connect path (e.g. accept).  Pass through.
                return socket(address->family, address->socktype, address->protocol);
            }

            char ipBuf[INET6_ADDRSTRLEN] = {};
            if (address->family == AF_INET)
            {
                auto const* sin = reinterpret_cast<sockaddr_in const*>(&address->addr);
                if (!inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf)))
                {
                    // inet_ntop failure on a 4-byte sockaddr is pathological;
                    // fail closed.
                    return CURL_SOCKET_BAD;
                }
            }
            else if (address->family == AF_INET6)
            {
                auto const* sin6 = reinterpret_cast<sockaddr_in6 const*>(&address->addr);
                if (!inet_ntop(AF_INET6, &sin6->sin6_addr, ipBuf, sizeof(ipBuf)))
                {
                    return CURL_SOCKET_BAD;
                }
            }
            else
            {
                // Unknown address family.  Fail closed.
                return CURL_SOCKET_BAD;
            }

            std::string const ip(ipBuf);
            if (IsLocalNetworkHost(ip))
            {
                g_DnsResolvedIpRejectionCount.fetch_add(1, std::memory_order_relaxed);
                LOG_SECURITY_WARN("[security] dns_resolved_ip_local_network_rejected resolved_ip='{}'", ip);
                return CURL_SOCKET_BAD;
            }

            return socket(address->family, address->socktype, address->protocol);
        }

        bool ShouldInstallPostResolveCheck(std::string_view url)
        {
            return url.starts_with("https://");
        }
    } // namespace

    std::string UrlEncodeComponent(std::string_view value)
    {
        if (value.empty())
        {
            return {};
        }
        CURL* encoder = curl_easy_init();
        if (!encoder)
        {
            return {};
        }
        char* escaped = curl_easy_escape(encoder, value.data(), static_cast<int>(value.size()));
        std::string out;
        if (escaped)
        {
            out.assign(escaped);
            curl_free(escaped);
        }
        curl_easy_cleanup(encoder);
        return out;
    }

    std::string UrlEncodePathSegments(std::string_view path, std::string& outError)
    {
        if (path.empty())
        {
            outError = "empty path";
            return {};
        }
        if (path.find('\0') != std::string_view::npos)
        {
            outError = "embedded NUL byte";
            return {};
        }

        CURL* encoder = curl_easy_init();
        if (!encoder)
        {
            outError = "curl_easy_init failed";
            return {};
        }

        std::string out;
        out.reserve(path.size() + 16);

        // Preserve a single leading '/' if present — vendor APIs like GitHub
        // expose paths like `/repos/owner/repo/contents/<path>` where the
        // <path> portion arrives without a leading slash; if a caller passes
        // a leading slash we keep it rather than dropping it silently.
        size_t pos = 0;
        if (path.front() == '/')
        {
            out += '/';
            pos = 1;
        }

        bool firstSegment = true;
        while (pos <= path.size())
        {
            size_t const sep = path.find('/', pos);
            std::string_view const segment = (sep == std::string_view::npos)
                                                 ? path.substr(pos)
                                                 : path.substr(pos, sep - pos);

            if (segment.empty())
            {
                outError = "empty path segment";
                curl_easy_cleanup(encoder);
                return {};
            }
            if (segment == ".." || segment == ".")
            {
                outError = "parent-directory or current-directory segment ('" +
                           std::string(segment) + "')";
                curl_easy_cleanup(encoder);
                return {};
            }

            if (!firstSegment)
            {
                out += '/';
            }
            firstSegment = false;

            char* escaped = curl_easy_escape(encoder, segment.data(), static_cast<int>(segment.size()));
            if (!escaped)
            {
                outError = "curl_easy_escape failed for segment";
                curl_easy_cleanup(encoder);
                return {};
            }
            out += escaped;
            curl_free(escaped);

            if (sep == std::string_view::npos)
            {
                break;
            }
            pos = sep + 1;
        }

        curl_easy_cleanup(encoder);
        return out;
    }

    void ApplyHardenedDefaults(CURL* handle, std::string_view url)
    {
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(handle, CURLOPT_CAINFO, caBundle.c_str());
        }

        if (ShouldInstallPostResolveCheck(url))
        {
            curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, OpensocketStrictCallback);
        }
    }

    void ApplyExecutorRedirectDefaults(CURL* handle, std::string_view url)
    {
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        // Restrict redirect targets to https only — prevents an http-downgrade
        // attack where a 30x response from a compromised endpoint redirects to
        // `http://attacker.com/...` and leaks the bearer/PAT in the
        // Authorization header on the unencrypted follow request.
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        // Cap follow depth so an attacker-controlled redirect loop can't
        // pin a worker thread indefinitely.
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(handle, CURLOPT_CAINFO, caBundle.c_str());
        }

        if (ShouldInstallPostResolveCheck(url))
        {
            curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, OpensocketStrictCallback);
        }
    }

    namespace
    {
        // Cheap classifier: "this string looks like an IPv6 literal" (hex digits +
        // colons, must contain at least one colon).  Used by IsLocalNetworkHost to
        // gate the unique-local / link-local prefix check so a public hostname
        // like `fc-acme.example.com` (starts with "fc" but contains hyphen + dot)
        // is NOT flagged as local.  Real IPv6 literals — `fc00::1`, `fe80::1` —
        // contain only hex + colons and pass.  Brackets (`[fc00::1]`) are handled
        // upstream: the URL-host charset rule rejects `[` / `]` so bracketed
        // IPv6 literals never reach this function via `ValidatePublicHttpEndpoint`.
        // Postgres-style host extraction in `ParseHostPort` may leave a bracketed
        // form, but the leading `[` causes IsIPv6Literal to return false there
        // too — bracketed IPv6 in postgres connections needs a separate fix.
        bool IsIPv6Literal(std::string const& host)
        {
            if (host.empty())
            {
                return false;
            }
            bool hasColon = false;
            for (char c : host)
            {
                unsigned char const uc = static_cast<unsigned char>(c);
                if (c == ':')
                {
                    hasColon = true;
                    continue;
                }
                if (std::isdigit(uc))
                {
                    continue;
                }
                if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                {
                    continue;
                }
                return false;
            }
            return hasColon;
        }
    } // namespace

    bool IsLocalNetworkHost(std::string const& host)
    {
        if (host == "localhost" || host == "127.0.0.1" || host == "::1")
        {
            return true;
        }
        if (host.starts_with("127."))
        {
            return true;
        }
        // Link-local includes the cloud metadata IP 169.254.169.254 used by AWS / GCP / Azure.
        if (host.starts_with("169.254."))
        {
            return true;
        }
        if (host.starts_with("10."))
        {
            return true;
        }
        if (host.starts_with("192.168."))
        {
            return true;
        }
        // 172.16.0.0/12 — second octet in [16, 31].
        if (host.starts_with("172."))
        {
            std::size_t const dot = host.find('.', 4);
            if (dot != std::string::npos)
            {
                // Explicit-loop digit parse — avoids std::stoi's exceptions on hostile
                // input (sweep #6 discipline).  Bound at 4 chars so we cap before
                // overflow even on a value like "172.99999.x.x".
                std::string const secondOctet = host.substr(4, dot - 4);
                if (!secondOctet.empty() && secondOctet.size() <= 3)
                {
                    int n = 0;
                    bool digitsOnly = true;
                    for (char c : secondOctet)
                    {
                        if (!std::isdigit(static_cast<unsigned char>(c)))
                        {
                            digitsOnly = false;
                            break;
                        }
                        n = n * 10 + (c - '0');
                    }
                    if (digitsOnly && n >= 16 && n <= 31)
                    {
                        return true;
                    }
                }
            }
        }
        // IPv6 unique-local fc00::/7 (matches "fc"/"fd" prefix on a real IPv6
        // literal — note the prefix is loosely 7 bits, so any hex byte starting
        // with `fc` or `fd` falls in the range) and link-local fe80::/10.
        // Gated on IsIPv6Literal so a public hostname starting with these
        // letters — e.g. `fc-acme.example.com` — is NOT flagged as local.
        // Sitting-27 fix; before, the prefix check ran unconditionally on the
        // raw host string.
        if (IsIPv6Literal(host) &&
            (host.starts_with("fc") || host.starts_with("fd") || host.starts_with("fe80")))
        {
            return true;
        }
        return false;
    }

    std::expected<void, ConnectorError> ValidatePublicHttpEndpoint(std::string const& url)
    {
        // Wrap the inner check so every false-return path bumps the counter
        // exactly once.  Avoids 5 increment-then-return sites in the body.
        auto inner = [](std::string const& innerUrl, std::string& innerErr) -> bool
        {
            if (innerUrl.empty() || innerUrl.size() > 2048)
            {
                innerErr = "endpoint URL empty or exceeds 2048 chars";
                return false;
            }

            std::string scheme;
            std::size_t hostStart = 0;
            if (innerUrl.starts_with("https://"))
            {
                scheme = "https";
                hostStart = 8;
            }
            else if (innerUrl.starts_with("http://"))
            {
                scheme = "http";
                hostStart = 7;
            }
            else
            {
                innerErr = "endpoint scheme must be http:// or https://";
                return false;
            }

            // Slice host[:port] up to the first /, ?, # or end of string.
            std::size_t hostEnd = innerUrl.size();
            for (std::size_t i = hostStart; i < innerUrl.size(); ++i)
            {
                char const c = innerUrl[i];
                if (c == '/' || c == '?' || c == '#')
                {
                    hostEnd = i;
                    break;
                }
            }
            std::string const hostPort = innerUrl.substr(hostStart, hostEnd - hostStart);
            if (hostPort.empty())
            {
                innerErr = "endpoint host is empty";
                return false;
            }

            // Strip optional :port.
            std::string host;
            std::size_t const colonPos = hostPort.find(':');
            if (colonPos != std::string::npos)
            {
                host = hostPort.substr(0, colonPos);
            }
            else
            {
                host = hostPort;
            }
            if (host.empty() || host.size() > 253)
            {
                innerErr = "endpoint host empty or exceeds 253 chars";
                return false;
            }

            // Conservative host charset.  Reject URL-meaningful chars (`@` for
            // userinfo, `%` for percent-encoded smuggle, etc.) and whitespace.
            for (char c : host)
            {
                unsigned char const uc = static_cast<unsigned char>(c);
                if (std::isalnum(uc))
                {
                    continue;
                }
                if (c == '.' || c == '-')
                {
                    continue;
                }
                innerErr = "endpoint host contains invalid character";
                return false;
            }

            // Production posture: https → no local net (SSRF gate).
            // Plaintext-http (dev mode) → local net allowed (parallel of email's
            // `allowLocal = !useSsl` heuristic).
            if (scheme == "https" && IsLocalNetworkHost(host))
            {
                innerErr = "https endpoint resolves to local-network host (loopback/private/link-local/cloud-metadata)";
                return false;
            }

            return true;
        };

        std::string innerErr;
        if (!inner(url, innerErr))
        {
            g_EndpointSsrfRejectionCount.fetch_add(1, std::memory_order_relaxed);
            return std::unexpected(ConnectorError::Make(ConnectorErrorCode::InvalidEndpoint, std::move(innerErr)));
        }
        return {};
    }
} // namespace AIAssistant::ConnectorHttp
