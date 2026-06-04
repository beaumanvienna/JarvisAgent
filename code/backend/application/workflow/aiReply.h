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
#include <optional>
#include <string>
#include <string_view>

namespace AIAssistant
{
    struct AiUsage
    {
        int32_t m_InputTokens = 0;
        int32_t m_OutputTokens = 0;
        int32_t m_TotalTokens = 0;
    };

    // Semantic classification of provider-side error bodies, populated by the
    // per-interface ReplyParsers and consumed by the dashboard UI for branching
    // on the actionable category (banner severity, hazard glyph, LED rules).
    // Raw provider strings stay on AiError (m_ProviderErrorCode /
    // m_ProviderErrorType) for logs/debugging; the UI only branches on this
    // enum so provider-specific codes never leak into React.
    enum class ProviderErrorCategory : uint8_t
    {
        Unknown = 0,        // body unparseable or no recognized discriminator
        BillingExhausted,   // OpenAI insufficient_quota, Anthropic credit_balance_too_low, Gemini BILLING_DISABLED, Bedrock ServiceQuotaExceededException
        ThrottleRateLimit,  // genuine throttle; Retry-After applies; AIMD operating normally
        AuthFailure,        // bad API key / expired credential / wrong region
        ServiceOverload,    // provider-side capacity, transient (Anthropic overloaded_error, Gemini UNAVAILABLE)
        ModelNotFound,      // model_not_found / deprecated / not-available-on-account
        InvalidRequest      // 4xx with malformed input (caller bug, won't retry)
    };

    // Stable wire string for the category, used by the WS ai-call-failed
    // payload and any structured-log site that includes the category.  The
    // values match the enum names so a renderer can branch on them directly.
    constexpr std::string_view CategoryToString(ProviderErrorCategory category)
    {
        switch (category)
        {
            case ProviderErrorCategory::Unknown:           return "Unknown";
            case ProviderErrorCategory::BillingExhausted:  return "BillingExhausted";
            case ProviderErrorCategory::ThrottleRateLimit: return "ThrottleRateLimit";
            case ProviderErrorCategory::AuthFailure:       return "AuthFailure";
            case ProviderErrorCategory::ServiceOverload:   return "ServiceOverload";
            case ProviderErrorCategory::ModelNotFound:     return "ModelNotFound";
            case ProviderErrorCategory::InvalidRequest:    return "InvalidRequest";
        }
        // Switch enumerates every variant per feedback_cpp_discipline; the
        // static_assert below force-breaks if the enum grows so the switch
        // (and its consumers) get updated in lockstep.
        return "Unknown";
    }

    static_assert(static_cast<int>(ProviderErrorCategory::InvalidRequest) == 6,
                  "ProviderErrorCategory variants changed — extend CategoryToString and downstream "
                  "switch sites (banner copy, LED rules, etc.) to match.");

    struct AiError
    {
        enum class Kind
        {
            None,
            Http,
            Parse,
            SchemaValidation,
            Timeout,
            Transport,
            Provider
        };

        Kind m_Kind = Kind::None;
        int m_HttpStatus = 0;
        std::string m_Message;

        // Provider-specific discriminators from the parsed error body.
        // Raw strings for logs/debugging only — UI branches on m_Category.
        std::string m_ProviderErrorCode;
        std::string m_ProviderErrorType;

        // UI-facing semantic classification.  Unknown when the parser
        // didn't recognise the discriminator (or the parser hasn't been
        // extended yet — API3/4/5 land full classification in Workstream E).
        ProviderErrorCategory m_Category{ProviderErrorCategory::Unknown};

        // From the provider's Retry-After header on 429/503; captured by
        // the dispatcher from the response headers and threaded through
        // the curl callback.  Drives the dashboard popover's countdown.
        std::optional<int> m_RetryAfterSeconds;
    };

    struct AiReply
    {
        enum class Kind
        {
            Text,
            Structured,
            Error
        };

        Kind m_Kind = Kind::Error;
        std::string m_Text;
        std::string m_StructuredJson;
        AiError m_Error;
        AiUsage m_Usage;
        std::string m_FinishReason;
        std::string m_SystemFingerprint;
    };
} // namespace AIAssistant
