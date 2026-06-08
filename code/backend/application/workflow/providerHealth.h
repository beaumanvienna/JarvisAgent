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

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "workflow/aiReply.h"

namespace AIAssistant
{
    // Per-interface health snapshot consumed by the dashboard's AI Health LED
    // (Sitting-8 Workstream D, dev plan §4-D + §3.5-C).  One entry per
    // configured `api_interfaces[]` provider; built by joining
    // `AiRequestPool::m_HealthPerInterface` (last-error info, success streak)
    // with the dispatcher's per-`quotaKey` controller cap state.
    //
    // The struct is a pure value type — assembled inside a single critical
    // section so the rendered LED can't show torn reads (cap and last-error
    // from different microseconds).
    //
    // `m_QuotaKey` lets the dashboard popover detect rows that share a cap
    // (two interfaces routing to api.openai.com/gpt-4 would both list the
    // same key); a "(shared cap)" footnote per §5.2-(c) keys off this.
    struct ProviderHealthSnapshot
    {
        // Identity — what the dashboard renders + branches on.
        std::string m_InterfaceName;        // user-configured label from config.json
        std::string m_InterfaceTypeName;    // "API1".."API6" — for the shared-cap footnote
        std::string m_QuotaKey;             // controller key (host|modelFamily); empty if interface has never dispatched
        bool m_IsMock{false};               // true if served by MockTransport — "(mocked)" badge in popover

        // AIMD cap state from the per-`quotaKey` controller.  Both default to
        // -1 when the interface hasn't dispatched yet (controller doesn't
        // exist) — UI renders "—" rather than "0/0" in that case.
        int m_CurrentCap{-1};
        int m_MaxCap{-1};                   // controller's hardCap (config rate_limit.max_concurrency)
        int m_FloorCap{1};                  // AIMD floor (always 1 today)

        // Wall-clock (Unix ms) of the most recent actual 429 for this interface's
        // controller; 0 == never throttled.  The honest "is this provider rate-
        // limiting us right now" signal — distinct from `m_CurrentCap < m_MaxCap`,
        // which is just AIMD ramping below the ceiling.  The dashboard's amber
        // "throttled" LED keys on this, not on the cap.
        int64_t m_Last429AtMs{0};

        // Per-interface last-error state from AiRequestPool.  m_LastErrorAt
        // defaults to time_point{} (epoch); a UI consumer should check
        // `m_ConsecutiveErrors > 0` or compare against epoch rather than
        // trusting "is the timestamp recent" alone.
        std::chrono::system_clock::time_point m_LastErrorAt{};
        std::string m_LastErrorCode;        // raw provider code (e.g. "insufficient_quota", "USER_PROJECT_QUOTA_EXCEEDED")
        std::string m_LastErrorType;        // raw provider type (e.g. "rate_limit_error", "RESOURCE_EXHAUSTED")
        std::string m_LastErrorMessage;     // provider's free-form message (length-capped at the point of capture)
        ProviderErrorCategory m_LastErrorCategory{ProviderErrorCategory::Unknown};
        int m_LastHttpStatus{0};
        std::optional<int> m_RetryAfterSeconds;

        // Cumulative counters since process start (or last config reload).
        uint64_t m_ConsecutiveErrors{0};            // streak of failures since last success
        uint64_t m_SuccessStreakSinceLastError{0};  // streak of successes since last failure

        // Drives the §4-D "cap pinned at floor for >60s" red rule.  Set when
        // the controller's m_CurrentConcurrencyCap first drops to floor; reset
        // when cap recovers above floor.  Default time_point{} (epoch) means
        // "not pinned" — UI checks against epoch.
        std::chrono::system_clock::time_point m_CapPinnedAtFloorSince{};
    };

    // Per-interface mutable state tracked by AiRequestPool.  One entry per
    // observed interface_name; updated on every completion (success bumps the
    // success streak + resets consecutive_errors; failure records last_error_*
    // + bumps consecutive_errors + resets success streak).
    //
    // `m_CapPinnedAtFloorSince` mirrors the per-controller pinning state;
    // updated when AiRequestPool sees a Cap-Changed signal that crosses the
    // floor boundary.
    struct InterfaceHealthState
    {
        std::chrono::system_clock::time_point m_LastErrorAt{};
        std::string m_LastErrorCode;
        std::string m_LastErrorType;
        std::string m_LastErrorMessage;
        ProviderErrorCategory m_LastErrorCategory{ProviderErrorCategory::Unknown};
        int m_LastHttpStatus{0};
        std::optional<int> m_RetryAfterSeconds;

        uint64_t m_ConsecutiveErrors{0};
        uint64_t m_SuccessStreakSinceLastError{0};

        std::chrono::system_clock::time_point m_CapPinnedAtFloorSince{};
        std::string m_QuotaKey;             // remembered from the most recent dispatch — joins to dispatcher controller
    };
} // namespace AIAssistant
