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

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

typedef void CURL;

namespace AIAssistant::ConnectorHttp
{
    // Response-body cap for connector TestConnection writeCallback.  TestConnection
    // responses are tiny (sub-KB typical); 1 MB is generous slack and bounds
    // memory pressure if a hostile redirect target streams a huge payload.
    inline constexpr std::size_t kMaxConnectorResponseBytes = 1 * 1024 * 1024;

    // libcurl write callback that appends to a std::string* up to the cap.
    // Pass &responseBody as CURLOPT_WRITEDATA.  Returning < incoming aborts
    // the transfer (libcurl raises CURLE_WRITE_ERROR).
    std::size_t BoundedStringWriteCallback(void* contents, std::size_t size, std::size_t nmemb, void* userp);

    // Apply hardened libcurl defaults shared across every cloud connector's
    // TestConnection path:
    //   - SSL_VERIFYPEER=1, SSL_VERIFYHOST=2  — defense-in-depth (libcurl
    //     defaults match, but explicit posture protects against a future
    //     build that silently weakens the verification defaults).
    //   - FOLLOWLOCATION=0                    — no redirect following.  A 30x
    //     to attacker-controlled host would otherwise leak the bearer/PAT in
    //     the Authorization header — the redirect-amplified SSRF vector.
    //   - CAINFO from CurlWrapper::GetCaBundlePath() if a bundle path is set.
    //   - When `url` starts with `https://`, additionally install a
    //     CURLOPT_OPENSOCKETFUNCTION callback that runs after libcurl's DNS
    //     resolve — rejects any resolved IP in the loopback / RFC1918 /
    //     link-local / cloud-metadata ranges via IsLocalNetworkHost.  Closes
    //     the SSRF vector where a public DNS name (`evil.example.com`)
    //     resolves to an internal IP at attacker-controlled DNS time.  For
    //     `http://` URLs (dev-mode local-net opt-in like MinIO / Azurite /
    //     local Mailpit), the post-resolve check is NOT installed — mirrors
    //     email's `allowLocal = !useSsl` posture.  Pass the request URL via
    //     the second arg; default empty preserves no-callback behavior.
    // Caller still sets URL, TIMEOUT, headers, write callback, etc.
    void ApplyHardenedDefaults(CURL* handle, std::string_view url = {});

    // Variant for executor data paths against vendor APIs that legitimately
    // emit 30x responses on the data path:
    //   - S3 cross-region redirects (`301 Moved Permanently` + Location to the
    //     correct region) and presigned-URL flows.
    //   - Microsoft Graph download endpoints — `GET /me/drive/items/{id}/content`
    //     returns 302 to a `download.microsoft*` / SharePoint CDN URL.
    // Same TLS-verify + CAINFO + DNS-post-resolve setup as ApplyHardenedDefaults,
    // but with FOLLOWLOCATION=1, REDIR_PROTOCOLS_STR="https" (no http
    // downgrade), and MAXREDIRS=10 (cap follow depth so an attacker-controlled
    // redirect loop can't pin a worker).  An unencrypted-protocol redirect
    // target is refused by libcurl as a hard error rather than silently
    // followed.
    //
    // Use this helper ONLY where the vendor API's legitimate behavior includes
    // 30x.  Default to ApplyHardenedDefaults (FOLLOWLOCATION=0) everywhere
    // else — the bearer-token-leak threat from following a hostile redirect
    // is real even on cloud surfaces with valid TLS certs.
    void ApplyExecutorRedirectDefaults(CURL* handle, std::string_view url = {});

    // Loopback / link-local / RFC-1918 private / cloud-metadata IP detection.
    // Used by the SSRF gate for connectors that accept a user-supplied endpoint
    // URL (jira, redmine, polarion) and by `PostgresConnector::IsValidSslMode`
    // for production-posture sslmode rules.  Pure syntactic IP-literal check —
    // does NOT do DNS resolution.  Lifted from emailConnector's anon-namespace
    // copy so the email + http + postgres connectors share one source of truth.
    //
    // The IPv6 unique-local (`fc00::/7`) and link-local (`fe80::/10`) prefix
    // check is gated on a structural IPv6-literal classifier, so public
    // hostnames starting with the same letters (`fc-acme.example.com`,
    // `fdsoftware.example.com`) are NOT flagged as local.
    //
    // Bracketed IPv6 literals in URLs (`[fc00::1]`) are handled upstream by
    // `ValidatePublicHttpEndpoint`: the host charset rejects `[` / `]` so
    // bracketed forms never reach here.  Postgres-style bare-host extraction
    // doesn't strip brackets, so an IPv6 in a postgres connection's endpoint
    // would slip through this check — separate cleanup tracked elsewhere.
    bool IsLocalNetworkHost(std::string const& host);

    // Validate a user-supplied HTTP(S) endpoint URL for SSRF safety.
    //
    //   - Scheme must be http or https.
    //   - For https, the host must NOT be loopback / link-local / RFC-1918
    //     / cloud-metadata (IsLocalNetworkHost == false).
    //   - For http (plaintext dev mode), local-network hosts are allowed —
    //     same posture as emailConnector's `allowLocal = !useSsl` heuristic.
    //   - Host charset is conservative: alphanumeric + `.` + `-` only.
    //   - Host capped at 253 chars (DNS limit); URL capped at 2048.
    //
    // Populates errorMessage on rejection.  Purely syntactic — does NOT do
    // DNS resolution, so an attacker who controls a public DNS name that
    // resolves to an internal IP is NOT blocked here.  Catching that requires
    // a resolver-time post-resolve check (CURLOPT_OPENSOCKETFUNCTION); out
    // of scope for this helper.
    bool ValidatePublicHttpEndpoint(std::string const& url, std::string& errorMessage);

    // Lifetime counters for the cloud-surface security gates.  All atomic,
    // lock-free, monotonically increasing.  Surfaced via `/api/debug/signals`
    // for live operator monitoring.  Per-instance forensic detail (timestamp,
    // task/run/connection identifiers, actual rejected value) stays in the
    // security log; these counters answer the "is this gate firing at all?"
    // question without grepping `log/log.txt`.
    //
    // Categories:
    //   - DnsResolvedIp: resolved IP fell in local-net.  Bumped by
    //     OpensocketStrictCallback before returning CURL_SOCKET_BAD.
    //   - EndpointSsrf: syntactic SSRF gate rejected the URL.  Bumped by
    //     ValidatePublicHttpEndpoint at every false-return path.
    //   - CredentialCrlf: bearer/PAT/JWT/API-key contained CR/LF.  Bumped at
    //     every connector / executor splice site that calls ContainsCrlf and
    //     emits *_crlf_rejected.
    //   - InputValidation: bucket/blob/key/range/folder/handle validator
    //     rejected user-supplied input.  Bumped at every *_invalid_* log
    //     site.
    //   - PostgresInvalidSslmode: sslmode allowlist or non-localhost
    //     production posture rejected.
    //   - PostgresForbiddenParam: libpq cert/key/file-path tripwire.
    std::uint64_t GetDnsResolvedIpRejectionCount();
    std::uint64_t GetEndpointSsrfRejectionCount();
    std::uint64_t GetCredentialCrlfRejectionCount();
    std::uint64_t GetInputValidationRejectionCount();
    std::uint64_t GetPostgresInvalidSslmodeRejectionCount();
    std::uint64_t GetPostgresForbiddenParamRejectionCount();

    // Increment helpers — call adjacent to a LOG_SECURITY_WARN at a gate
    // rejection site.  Cheap (one atomic fetch_add with relaxed ordering) and
    // safe to call from any thread.  No-op if the site already calls a
    // category-specific helper above.
    void IncrementEndpointSsrfRejection();
    void IncrementCredentialCrlfRejection();
    void IncrementInputValidationRejection();
    void IncrementPostgresInvalidSslmodeRejection();
    void IncrementPostgresForbiddenParamRejection();
} // namespace AIAssistant::ConnectorHttp
