/* Copyright (c) 2025 JC Technolabs
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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace AIAssistant
{
    // Provider-agnostic snapshot of rate-limit state extracted from one HTTP
    // response (headers + optional body).  Each field is "unknown" when negative
    // or nullopt — the controller (Phase 2) will merge observations using
    // replacement-if-known, preservation-if-unknown so partial information from
    // a response with only some headers populated never wipes out previously
    // observed values.
    //
    // Same shape across providers: OpenAI returns request/token quotas,
    // Anthropic adds split input/output token quotas, Gemini and Bedrock
    // typically return only reactive 429s with Retry-After.  The strategy
    // (rateLimitStrategy.h) per provider populates whatever it can extract;
    // unsupported fields stay at their default sentinels.
    struct RateLimitObservation
    {
        int64_t m_RemainingRequests = -1;
        int64_t m_RemainingInputTokens = -1;
        int64_t m_RemainingOutputTokens = -1;
        int64_t m_RemainingCombinedTokens = -1;

        std::optional<std::chrono::steady_clock::time_point> m_RequestsResetAt;
        std::optional<std::chrono::steady_clock::time_point> m_TokensResetAt;

        std::optional<std::chrono::milliseconds> m_RetryAfter;

        // Tokens this request actually consumed.  Filled from the response body
        // (usage.input_tokens / usage.output_tokens) by the strategy when
        // available.  Lets the controller distinguish "what we just used" from
        // "what the provider says is left."
        int64_t m_ConsumedInputTokens = -1;
        int64_t m_ConsumedOutputTokens = -1;

        bool IsEmpty() const
        {
            return m_RemainingRequests < 0 && m_RemainingInputTokens < 0 && m_RemainingOutputTokens < 0 &&
                   m_RemainingCombinedTokens < 0 && !m_RequestsResetAt.has_value() && !m_TokensResetAt.has_value() &&
                   !m_RetryAfter.has_value() && m_ConsumedInputTokens < 0 && m_ConsumedOutputTokens < 0;
        }
    };
} // namespace AIAssistant
