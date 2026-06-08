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
    // Composes three mechanisms:
    //   - Token-bucket mirror: never overshoots the provider's stated quota.
    //   - AIMD concurrency cap: finds the sustainable concurrency in-bucket.
    //   - Server-directed waits: Retry-After is a floor on next admission.
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

        // Cheap accessors for the dispatcher's debug snapshot.  CurrentConcurrencyCap
        // first applies any pending time-based recovery so a read taken while the
        // controller is idle reflects the cap climbing back toward the ceiling.
        int CurrentConcurrencyCap() const
        {
            ApplyTimeRecovery(std::chrono::steady_clock::now());
            return m_CurrentConcurrencyCap;
        }
        int StreakSinceLast429() const { return m_StreakSinceLast429; }
        RateLimitObservation const& LastObservation() const { return m_LastObservation; }

        // Observability for the dashboard / `/api/debug/signals`.
        int HardCap() const { return m_HardCap; }                         // the AIMD ceiling
        // Wall-clock of the most recent 429 (epoch == never throttled) — this is
        // the authoritative "was actually rate-limited" signal (every 429 lands
        // here via Observe, even ones that later succeed on retry), distinct from
        // "cap below ceiling" which is just a not-yet-ramped AIMD state.
        std::chrono::system_clock::time_point Last429At() const { return m_Last429At; }
        // Seconds until idle recovery would lift the cap back to the ceiling.
        // 0 once already at the ceiling.  Lets an operator watch the countdown.
        int CapRecoveryEtaSeconds() const;

    private:
        // AIMD parameters.  Clean completions accumulate in m_StreakSinceLast429;
        // every kStreakForIncrease completions, m_CurrentConcurrencyCap += 1.
        // On any 429, cap halves (floor 1) and streak resets.
        static constexpr int kStreakForIncrease = 5;

        // Time-based recovery — the missing half of AIMD (cf. RFC 5681 "Restart
        // of Idle Connections").  Event-driven additive increase only fires on
        // live successful traffic, so a cap reduced by a 429 would otherwise
        // stay frozen below the ceiling forever once the load stops —
        // penalising the next burst.  Recovery is a pure function of the cap as
        // of the last Observe and the wall-clock elapsed since it: the cap
        // regains one slot per kRecoveryInterval, and a full quiet period of
        // kIdleRestartWindow restores it straight to the ceiling.  Crucially the
        // recovery baseline (m_LastActivityAt / m_CapAtIdleStart) is advanced
        // ONLY by Observe, never by a read — otherwise frequent dashboard polls
        // would keep nudging the baseline forward and the idle-restart would
        // never fire.  Under load Observe runs constantly so elapsed stays below
        // kRecoveryInterval → pure AIMD, no time term; recovery only bites once
        // traffic actually stops.
        static constexpr std::chrono::seconds kRecoveryInterval{10};
        static constexpr std::chrono::seconds kIdleRestartWindow{60};

        // Recomputes m_CurrentConcurrencyCap from (m_CapAtIdleStart,
        // m_LastActivityAt, now).  const + mutable cap: it lazily evolves the
        // time-derived cap on read (ShouldAdmit / CurrentConcurrencyCap), the
        // only place idle recovery can happen since Observe runs only under
        // traffic.  Every caller serialises on the dispatcher's m_DebugMutex, so
        // the mutation is race-free.  Never moves the recovery baseline.
        void ApplyTimeRecovery(std::chrono::steady_clock::time_point now) const;

        int const m_HardCap;                          // upper bound (HTTP/2 stream cap or config)
        mutable int m_CurrentConcurrencyCap{1};       // grows on streak/time, halves on 429
        int m_StreakSinceLast429{0};
        // Recovery baseline, set ONLY by Observe: the cap value and the moment
        // of the last observed activity.  Idle recovery climbs from here.
        std::chrono::steady_clock::time_point m_LastActivityAt{};
        int m_CapAtIdleStart{1};
        // Wall-clock of the most recent 429 (epoch == never).  The honest
        // "actually throttled" signal surfaced to the dashboard.
        std::chrono::system_clock::time_point m_Last429At{};

        RateLimitObservation m_LastObservation{};
    };
} // namespace AIAssistant
