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

#include "rateLimitController.h"

#include <algorithm>

namespace AIAssistant
{
    RateLimitController::RateLimitController(int initialConcurrencyProbe, int hardCap)
        : m_HardCap(std::max(1, hardCap)),
          m_CurrentConcurrencyCap(std::clamp(initialConcurrencyProbe, 1, std::max(1, hardCap)))
    {
    }

    RateLimitController::Decision RateLimitController::ShouldAdmit(int currentInflight,
                                                                   int64_t estimatedInputTokens) const
    {
        Decision decision;
        decision.m_Admit = true;

        auto const now = std::chrono::steady_clock::now();

        // Hard cap (HTTP/2 stream cap or config-driven max_concurrency).  Keeps
        // us under any infrastructural ceiling regardless of AIMD growth.
        if (currentInflight >= m_HardCap)
        {
            decision.m_Admit = false;
            decision.m_Reason = "hard cap";
            return decision;
        }

        // AIMD concurrency cap.  This is the predictive layer: after a 429
        // we halve cap so the next several requests don't pile up against a
        // quota we just learned is full.
        if (currentInflight >= m_CurrentConcurrencyCap)
        {
            decision.m_Admit = false;
            decision.m_Reason = "AIMD cap";
            return decision;
        }

        // Server-directed wait.  Retry-After is a floor: never re-dispatch
        // sooner than the server explicitly told us to.  The observation's
        // RetryAfter has already been folded into the *_ResetAt times by
        // Observe(), so checking those here covers both Retry-After and
        // proactive reset hints.
        if (m_LastObservation.m_RequestsResetAt.has_value() && now < *m_LastObservation.m_RequestsResetAt)
        {
            // Subtract our own in-flight from the request quota — provider's
            // remaining doesn't account for them until their responses arrive.
            int64_t const projected = m_LastObservation.m_RemainingRequests - static_cast<int64_t>(currentInflight);
            if (m_LastObservation.m_RemainingRequests >= 0 && projected <= 0)
            {
                decision.m_Admit = false;
                decision.m_NextAttemptAt = *m_LastObservation.m_RequestsResetAt;
                decision.m_Reason = "provider req quota";
                return decision;
            }
        }

        if (m_LastObservation.m_TokensResetAt.has_value() && now < *m_LastObservation.m_TokensResetAt)
        {
            // Token-bucket projection: take the tightest of input,
            // output, combined — whichever the provider exposes.  Subtract
            // the current request's estimated input cost.
            int64_t mergedRemaining = -1;
            auto const consider = [&](int64_t value) {
                if (value < 0)
                    return;
                if (mergedRemaining < 0 || value < mergedRemaining)
                    mergedRemaining = value;
            };
            consider(m_LastObservation.m_RemainingCombinedTokens);
            consider(m_LastObservation.m_RemainingInputTokens);
            consider(m_LastObservation.m_RemainingOutputTokens);

            if (mergedRemaining >= 0 && (mergedRemaining - estimatedInputTokens) <= 0)
            {
                decision.m_Admit = false;
                decision.m_NextAttemptAt = *m_LastObservation.m_TokensResetAt;
                decision.m_Reason = "provider token quota";
                return decision;
            }
        }

        return decision;
    }

    void RateLimitController::Observe(RateLimitObservation const& observation, bool was429)
    {
        // Idempotent merge: known fields overwrite, unknown fields preserve.
        // Multiple Observe() calls for the same request (the Phase 4-deferred
        // streaming case where headers and body arrive separately) produce
        // identical end state to a single combined call.
        if (observation.m_RemainingRequests >= 0)
            m_LastObservation.m_RemainingRequests = observation.m_RemainingRequests;
        if (observation.m_RemainingInputTokens >= 0)
            m_LastObservation.m_RemainingInputTokens = observation.m_RemainingInputTokens;
        if (observation.m_RemainingOutputTokens >= 0)
            m_LastObservation.m_RemainingOutputTokens = observation.m_RemainingOutputTokens;
        if (observation.m_RemainingCombinedTokens >= 0)
            m_LastObservation.m_RemainingCombinedTokens = observation.m_RemainingCombinedTokens;
        if (observation.m_RequestsResetAt.has_value())
            m_LastObservation.m_RequestsResetAt = observation.m_RequestsResetAt;
        if (observation.m_TokensResetAt.has_value())
            m_LastObservation.m_TokensResetAt = observation.m_TokensResetAt;

        if (observation.m_RetryAfter.has_value())
        {
            // Retry-After is a floor on both buckets — never re-dispatch
            // sooner than the server told us to.
            auto const candidate = std::chrono::steady_clock::now() + *observation.m_RetryAfter;
            if (!m_LastObservation.m_RequestsResetAt.has_value() || candidate > *m_LastObservation.m_RequestsResetAt)
                m_LastObservation.m_RequestsResetAt = candidate;
            if (!m_LastObservation.m_TokensResetAt.has_value() || candidate > *m_LastObservation.m_TokensResetAt)
                m_LastObservation.m_TokensResetAt = candidate;
        }

        if (observation.m_ConsumedInputTokens >= 0)
            m_LastObservation.m_ConsumedInputTokens = observation.m_ConsumedInputTokens;
        if (observation.m_ConsumedOutputTokens >= 0)
            m_LastObservation.m_ConsumedOutputTokens = observation.m_ConsumedOutputTokens;

        // AIMD update.  Multiplicative decrease on 429, additive increase
        // on streak of clean completions.  Cap floor = 1; ceiling = m_HardCap
        // (HTTP/2 stream cap or config max_concurrency).
        if (was429)
        {
            m_CurrentConcurrencyCap = std::max(1, m_CurrentConcurrencyCap / 2);
            m_StreakSinceLast429 = 0;
        }
        else
        {
            ++m_StreakSinceLast429;
            if (m_StreakSinceLast429 >= kStreakForIncrease)
            {
                m_StreakSinceLast429 = 0;
                if (m_CurrentConcurrencyCap < m_HardCap)
                    ++m_CurrentConcurrencyCap;
            }
        }
    }
} // namespace AIAssistant
