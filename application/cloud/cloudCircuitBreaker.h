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
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cloud/connectorError.h"

namespace AIAssistant
{
    // Per-connection circuit breaker to prevent failure cascading during cloud provider outages.
    //
    // State machine: Closed -> Open -> HalfOpen -> Closed (on success) or Open (on failure)
    //
    // Closed:   normal operation, requests pass through. Consecutive failures are counted.
    //           After m_FailureThreshold consecutive failures, transitions to Open.
    // Open:     all requests are short-circuited (fail immediately). After m_CooldownSeconds,
    //           transitions to HalfOpen.
    // HalfOpen: allows m_HalfOpenProbeCount requests through as probes. If a probe succeeds,
    //           transitions to Closed. If a probe fails, transitions back to Open.
    class CloudCircuitBreaker
    {
    public:
        enum class State
        {
            Closed,
            Open,
            HalfOpen
        };

        struct Config
        {
            int m_FailureThreshold{5};    // Consecutive failures before opening
            int m_CooldownSeconds{60};    // Seconds in Open state before transitioning to HalfOpen
            int m_HalfOpenProbeCount{1};  // Probes allowed in HalfOpen before deciding
        };

        CloudCircuitBreaker() = default;
        explicit CloudCircuitBreaker(Config config);

        // Check if a request should be allowed for the named connection.
        // Returns true if the request can proceed, false if the circuit is open.
        bool AllowRequest(std::string const& connectionName);

        // Record a successful operation for the named connection.
        void RecordSuccess(std::string const& connectionName);

        // Record a failed operation for the named connection.  `errorCode` (when
        // provided) is stored as the latest failure category for this circuit
        // and surfaced via `GetHealthSummary().m_LastFailureCode`.  Callers
        // without a typed code (legacy JCWF cloud tasks) leave it nullopt.
        void RecordFailure(std::string const& connectionName,
                           std::optional<ConnectorErrorCode> errorCode = std::nullopt);

        // Get the current state for a named connection.
        State GetState(std::string const& connectionName) const;

        // Get a human-readable state string.
        static std::string StateToString(State state);

        // Get health summary for all tracked connections.
        struct ConnectionHealth
        {
            std::string m_Name;
            State m_State{State::Closed};
            int m_ConsecutiveFailures{0};
            // `true` once RecordSuccess has fired at least once for this connection
            // — lit by a successful Test button click or a JCWF cloud task.  Drives
            // the dashboard Cloud LED so a merely-configured-but-never-proved
            // connection is reported as "unknown" instead of "healthy".
            bool m_EverSucceeded{false};
            // Latest typed failure code (set by `RecordFailure(name, code)`).
            // nullopt = no failure recorded with a typed code yet (either the
            // connection has only seen successes, or only legacy-shape failures).
            // Surfaced as `/api/status::connection_health[].last_failure_code`
            // so the dashboard can show a category beyond just consecutive-fail
            // counts.
            std::optional<ConnectorErrorCode> m_LastFailureCode;
        };
        std::vector<ConnectionHealth> GetHealthSummary() const;

        // Does the named connection have at least one recorded success?
        bool HasEverSucceeded(std::string const& connectionName) const;

    private:
        struct CircuitState
        {
            State m_State{State::Closed};
            int m_ConsecutiveFailures{0};
            int m_HalfOpenProbes{0};
            std::chrono::steady_clock::time_point m_OpenedAt{};
            bool m_EverSucceeded{false};
            std::optional<ConnectorErrorCode> m_LastFailureCode;
        };

        Config m_Config;
        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, CircuitState> m_Circuits;
    };
} // namespace AIAssistant
