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

#include "curlWrapper/loopbackGuard.h"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

#include "engine.h"

namespace AIAssistant
{
    namespace
    {
        std::atomic<std::uint64_t> g_LoopbackGuardRejections{0};

        // IPv4 loopback = 127.0.0.0/8.
        bool IsLoopbackV4(in_addr const& addr) noexcept
        {
            return (ntohl(addr.s_addr) >> 24) == 127u;
        }

        // IPv6 loopback = ::1, plus IPv4-mapped ::ffff:127.0.0.0/8 (so a host
        // that resolves as ::ffff:127.0.0.1 still counts as loopback) — mirrors
        // UrlPolicy::IsIp6Loopback.
        bool IsLoopbackV6(in6_addr const& addr) noexcept
        {
            static std::uint8_t const kLoopback[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
            if (std::memcmp(addr.s6_addr, kLoopback, 16) == 0)
            {
                return true;
            }
            static std::uint8_t const kV4MapPrefix[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
            if (std::memcmp(addr.s6_addr, kV4MapPrefix, 12) == 0)
            {
                return addr.s6_addr[12] == 127u;
            }
            return false;
        }

        // libcurl invokes this after DNS resolution, before the TCP connect;
        // `address` carries the resolved peer.  Reject anything that is not
        // loopback by returning CURL_SOCKET_BAD, which aborts the connection.
        curl_socket_t LoopbackOnlyOpensocket(void* /*clientp*/, curlsocktype purpose,
                                             struct curl_sockaddr* address)
        {
            if (purpose != CURLSOCKTYPE_IPCXN)
            {
                // Non-connect socket purpose — pass through unchanged.
                return ::socket(address->family, address->socktype, address->protocol);
            }

            bool loopback = false;
            if (address->family == AF_INET)
            {
                auto const* sin = reinterpret_cast<sockaddr_in const*>(&address->addr);
                loopback = IsLoopbackV4(sin->sin_addr);
            }
            else if (address->family == AF_INET6)
            {
                auto const* sin6 = reinterpret_cast<sockaddr_in6 const*>(&address->addr);
                loopback = IsLoopbackV6(sin6->sin6_addr);
            }
            // Any other family: not loopback → rejected below.

            if (!loopback)
            {
                char ipBuf[INET6_ADDRSTRLEN] = {};
                if (address->family == AF_INET)
                {
                    auto const* sin = reinterpret_cast<sockaddr_in const*>(&address->addr);
                    ::inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
                }
                else if (address->family == AF_INET6)
                {
                    auto const* sin6 = reinterpret_cast<sockaddr_in6 const*>(&address->addr);
                    ::inet_ntop(AF_INET6, &sin6->sin6_addr, ipBuf, sizeof(ipBuf));
                }
                g_LoopbackGuardRejections.fetch_add(1, std::memory_order_relaxed);
                LOG_SECURITY_WARN("[security] ai_dispatch_plaintext_http_nonloopback_rejected resolved_ip='{}'",
                                  ipBuf[0] != '\0' ? ipBuf : "unknown");
                return CURL_SOCKET_BAD;
            }

            return ::socket(address->family, address->socktype, address->protocol);
        }
    } // namespace

    void InstallLoopbackGuardForPlaintextHttp(CURL* easy, std::string const& url)
    {
        // Only plain http:// dispatch needs the connect-time loopback re-check.
        if (url.size() >= 7 && url.compare(0, 7, "http://") == 0)
        {
            curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, LoopbackOnlyOpensocket);
        }
    }

    std::uint64_t GetLoopbackGuardRejectionCount() noexcept
    {
        return g_LoopbackGuardRejections.load(std::memory_order_relaxed);
    }

} // namespace AIAssistant
