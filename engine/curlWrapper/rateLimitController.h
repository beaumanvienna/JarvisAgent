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
#include <string>

#include "rateLimitObservation.h"

namespace AIAssistant
{
    // Per-(host, modelFamily) adaptive controller for AI request dispatch.
    //
    // Composes three mechanisms (per "AI call performance optimization.md" §4):
    //   §4.1  Token-bucket mirror — never overshoots provider's stated quota
    //   §4.2  AIMD concurrency cap — finds the sustainable concurrency in-bucket
    //   §4.3  Server-directed waits — Retry-After is a floor on next admission
    //
    // Threading: instances are owned by CurlMultiDispatcher and accessed only
    // while m_DebugMutex is held.  Methods are NOT internally synchronized.
    class RateLimitController
    {
    public:
        // Reason text is pointer-to-static so it costs nothing to populate
        // and never invalidates.  Used in throttle log lines.
        struct Decision
        {
            bool m_Admit{false};
            std::chrono::steady_clock::time_point m_NextAttemptAt{};
            char const* m_Reason{""};
        };

        // initialConcurrencyProbe comes from the strategy.  hardCap clamps
        // AIMD growth — set by the dispatcher to kMaxActivePerHost in Phase 2,
        // overridden by config.rate_limit.max_concurrency in Phase 4.
        explicit RateLimitController(int initialConcurrencyProbe, int hardCap);

        // Decide whether to admit one more request for this controller's key.
        // currentInflight = active requests already on the wire for this key
        // (excluding the one we're about to admit).
        // estimatedInputTokens = strategy.EstimateInputTokens(prompt) — used
        // for the token-bucket projection.
        Decision ShouldAdmit(int currentInflight, int64_t estimatedInputTokens) const;

        // Update internal state from a fresh observation.  MUST be idempotent
        // by replacement: a known field overwrites the prior value, an unknown
        // field preserves it.  This protects the future split into ParseHeaders
        // + ParseBody (for streaming) — multiple Observe() calls per request
        // produce the same state as a single combined call.
        //
        // was429 drives AIMD: true halves the cap (multiplicative decrease),
        // false advances the streak counter (additive increase every K).
        void Observe(RateLimitObservation const& observation, bool was429);

        // Cheap accessors for the dispatcher's debug snapshot.
        int CurrentConcurrencyCap() const { return m_CurrentConcurrencyCap; }
        int StreakSinceLast429() const { return m_StreakSinceLast429; }
        RateLimitObservation const& LastObservation() const { return m_LastObservation; }

    private:
        // AIMD parameters.  Clean completions accumulate in m_StreakSinceLast429;
        // every kStreakForIncrease completions, m_CurrentConcurrencyCap += 1.
        // On any 429, cap halves (floor 1) and streak resets.
        static constexpr int kStreakForIncrease = 5;

        int const m_HardCap;                  // upper bound from caller (HTTP/2 stream cap or config)
        int m_CurrentConcurrencyCap{1};       // grows on streak, halves on 429
        int m_StreakSinceLast429{0};

        RateLimitObservation m_LastObservation{};
    };
} // namespace AIAssistant
