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
#include <expected>
#include <string>
#include <string_view>

namespace AIAssistant::UrlPolicy
{
    // Typed error returned by AI-interface URL validation.  Pairs with
    // `std::expected<void, UrlPolicyError>` so callers are compiler-forced to
    // handle every failure category, and each rejection site picks one Code
    // intentionally (no default: arm — -Wswitch catches new variants).
    //
    // Scope is intentionally narrow: this enum covers the AI-interface URL +
    // key_name validation only.  The SSRF gate cluster (`IsCallbackUrlAllowed`
    // for completion callbacks, `ValidatePublicHttpEndpoint` for cloud
    // connectors) carries its own typed errors — they reject different ranges
    // (any-internal vs any-non-loopback) and the wrong-enum mixup would
    // produce confusing log lines.
    enum class UrlPolicyErrorCode
    {
        // URL is empty, exceeds the size cap, has no scheme separator, or has
        // a host component the parser can't extract.  The pre-DNS reject.
        MalformedUrl,

        // Scheme is neither http:// nor https://.  ws:// / file:// / ftp:// /
        // etc. land here.  Distinct from MalformedUrl so dashboard / log
        // readers can distinguish "URL was structurally invalid" from "scheme
        // was structurally valid but disallowed".
        SchemeRejected,

        // http:// scheme with at least one resolved address outside the
        // loopback range (127.0.0.0/8 for IPv4, ::1 for IPv6).  The DNS-level
        // rejection.  Plain HTTP is loopback-only — local LLM dispatch
        // (ollama, llama.cpp, vLLM) is the supported case; anything else
        // would carry prompts in clear text over the wire.
        NonLoopbackHttp,

        // getaddrinfo returned no results.  Distinct from NonLoopbackHttp so
        // operators can tell "I have a typo / no network" from "policy
        // rejected my non-loopback address".
        UnresolvedHost,

        // http:// scheme combined with a non-empty key_name.  Local-loopback
        // ollama / llama.cpp don't require auth; the only reason to attach a
        // key_name to a plain-HTTP interface is operator confusion (likely
        // copy-pasted from a cloud interface), and that confusion would
        // silently leak the Bearer token in transit on every dispatch.
        // Checked before DNS to avoid leaking the typo + the credential
        // together in the rejection log.
        CredentialedPlaintextHttp,

        // Backstop.  Use sparingly; prefer adding a discrete variant above.
        UnknownError,
    };

    struct UrlPolicyError
    {
        UrlPolicyErrorCode m_Code{UrlPolicyErrorCode::UnknownError};
        std::string m_Details;

        static UrlPolicyError Make(UrlPolicyErrorCode code, std::string details)
        {
            return UrlPolicyError{code, std::move(details)};
        }
    };

    // Stable machine-readable label for the Code itself (NOT the full Details).
    // Used in log lines and REST error responses so clients can branch.
    std::string_view Describe(UrlPolicyErrorCode code);

    // Validate the URL + key_name pair for an AI interface.
    //
    // Returns success for:
    //   - https:// URLs (scheme provides transport encryption; further
    //     SSRF / endpoint validation lives in the cloud-connector gate)
    //   - http:// URLs where EVERY resolved address is loopback (127.0.0.0/8
    //     or ::1, IPv4-mapped IPv6 normalised), AND keyName is empty
    //
    // keyName may be empty.  When http:// + keyName non-empty, returns
    // CredentialedPlaintextHttp BEFORE the DNS resolve.
    //
    // Pure-function shape: no state, no logging, no rejection counters.
    // Callers log the typed error + bump the appropriate counter at their
    // site (config-load uses LOG_CORE_ERROR via ConfigParser; REST uses
    // LOG_SECURITY_WARN via WebServer).
    [[nodiscard]] std::expected<void, UrlPolicyError>
    ValidateAiInterfaceUrl(std::string const& url, std::string const& keyName);

    // Counter accessors for /api/debug/signals.  Each rejection site bumps
    // the matching counter at the point it rejects so the debug endpoint can
    // surface cumulative reject totals separated by category.  Two counters
    // (not one) because operators reading the dashboard want to distinguish
    // "we rejected N non-loopback http:// URLs" from "we rejected N
    // credentialed-plaintext URLs" — the second one indicates an operator
    // configured a credential they intended to use, which is the higher-
    // priority signal.
    void RecordUrlPolicyRejection() noexcept;
    void RecordCredentialedPlaintextHttpRejection() noexcept;
    std::uint64_t GetUrlPolicyRejectionCount() noexcept;
    std::uint64_t GetCredentialedPlaintextHttpRejectionCount() noexcept;

    // Lightweight scheme check used by dashboards / per-dispatch audit logs.
    // Returns true iff the URL begins with `http://` (case-sensitive — we
    // canonicalise scheme to lowercase at parse time elsewhere).  Cheap; no
    // DNS, no allocation.
    [[nodiscard]] bool IsPlaintextHttpUrl(std::string_view url) noexcept;

} // namespace AIAssistant::UrlPolicy
