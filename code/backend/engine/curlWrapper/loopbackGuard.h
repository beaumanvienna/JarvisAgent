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

#pragma once

#include <cstdint>
#include <string>

#include <curl/curl.h>

namespace AIAssistant
{
    // Connect-time loopback enforcement for plain-`http://` AI dispatch.
    //
    // `AIAssistant::UrlPolicy::ValidateAiInterfaceUrl` proves a plain-http
    // interface host resolves to loopback at create/update time, but libcurl
    // re-resolves the name at dispatch — a hostname whose DNS flips to a
    // routable IP between the two (classic DNS rebinding) would otherwise let a
    // plaintext request reach a non-loopback service.  This installs a
    // `CURLOPT_OPENSOCKETFUNCTION` that inspects the address libcurl actually
    // resolved to, just before the TCP connect, and aborts (CURL_SOCKET_BAD)
    // any non-loopback peer.  It is the inverse of ConnectorHttp's
    // OpensocketStrictCallback (which rejects local-net for public https).
    //
    // Installed ONLY for plain-`http://` URLs — `https://` carries transport
    // encryption and is covered by TLS verification + the connector SSRF
    // posture, so the guard is a no-op for it.
    void InstallLoopbackGuardForPlaintextHttp(CURL* easy, std::string const& url);

    // Count of connections the guard aborted because the resolved peer was not
    // loopback.  Surfaced via /api/debug/signals for operator visibility.
    std::uint64_t GetLoopbackGuardRejectionCount() noexcept;

} // namespace AIAssistant
