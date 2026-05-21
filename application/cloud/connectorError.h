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

#include <string>
#include <string_view>

namespace AIAssistant
{
    // Typed error returned by cloud-connector entry points (`TestConnection`,
    // `ValidatePublicHttpEndpoint`, `ValidatePostgresParams`).  Pairs with
    // `std::expected<T, ConnectorError>` so the caller is compiler-forced to
    // handle the failure path instead of forgetting an `if (!ok)` check after
    // the legacy `bool + std::string& errorMessage` shape.
    //
    // `m_Code` is the machine-actionable category (callers that branch on
    // failure type — e.g. circuit-breaker recording, retry policy — switch on
    // this).  `m_Details` is the human-readable detail string composed at the
    // failure site, suitable for echoing to the REST response or the ERROR
    // log.  Connectors emit it via the same idioms the legacy `errorMessage`
    // outparam carried (provider name, identifiers, HTTP status, etc.).
    //
    // The `Code` enum is intentionally subsystem-scoped (per-cloud not
    // per-method) rather than a single mega-enum across the codebase — the
    // parser + registry surfaces get their own.  Adding a new variant
    // triggers `-Wswitch` at every `switch (err.m_Code)` site so we never
    // silently drop a category.
    enum class ConnectorErrorCode
    {
        // Configuration shape problem on the connection itself — missing
        // required field, blank value, malformed inline param.  Predates the
        // call to KeyManager / curl.
        InvalidConfig,

        // Endpoint URL failed `ValidatePublicHttpEndpoint`'s SSRF gate
        // (scheme not in allowlist, DNS resolves to forbidden IP, etc.).
        // Distinct from `InvalidConfig` so callers can route security-class
        // failures separately (security log line vs operator-error message).
        InvalidEndpoint,

        // Credential not found in KeyManager (either `key_name` is empty
        // and there's no default, or the named provider doesn't exist).
        CredentialMissing,

        // Credential exists but is structurally wrong: dynamic_cast to the
        // expected subtype failed, secret material is empty / blank, the
        // api_key is in the wrong format (e.g. S3 expects "ID:SECRET"),
        // OR the bearer token contains a CR/LF injection byte.
        CredentialInvalid,

        // OAuth flow (refresh, token acquire, redirect handling) failed —
        // returned by connectors that route through `OAuthTokenManager`.
        OAuthError,

        // libcurl-level error: `curl_easy_init()` returned null, DNS
        // resolution failed, TLS handshake failed, write callback aborted.
        // The Details field carries `curl_easy_strerror(res)`.
        NetworkError,

        // Provider returned HTTP 401 / 403.  Distinct from `HttpError` so
        // callers can prompt "credential expired / scope insufficient"
        // remediation copy in the dashboard.
        AuthFailure,

        // Provider returned HTTP >= 400 other than 401/403.  Details carries
        // the status code + (truncated) response body when available.
        HttpError,

        // Backstop for code paths the discrete variants above don't cover.
        // Use sparingly — when in doubt, add a new variant rather than
        // collapse into this.  `-Wswitch` over a `default:`-free switch is
        // the anti-debugging-armor guarantee we lose with frequent UnknownError.
        UnknownError,
    };

    struct ConnectorError
    {
        ConnectorErrorCode m_Code{ConnectorErrorCode::UnknownError};
        std::string m_Details;

        // Convenience factory — keeps call sites terse:
        //   return std::unexpected(ConnectorError::Make(Code::InvalidConfig,
        //                                                "missing api_key for S3"));
        static ConnectorError Make(ConnectorErrorCode code, std::string details)
        {
            return ConnectorError{code, std::move(details)};
        }
    };

    // Human-readable label for the code itself (NOT the full Details).
    // Used in log lines as a stable identifier before the variable Details.
    std::string_view Describe(ConnectorErrorCode code);

} // namespace AIAssistant
