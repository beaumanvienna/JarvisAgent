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

#include "cloud/cloudCircuitBreaker.h"
#include "engine.h"

namespace AIAssistant
{
    CloudCircuitBreaker::CloudCircuitBreaker(Config config)
        : m_Config(std::move(config))
    {
    }

    bool CloudCircuitBreaker::AllowRequest(std::string const& connectionName)
    {
        std::lock_guard lock(m_Mutex);
        auto& circuit = m_Circuits[connectionName];

        switch (circuit.m_State)
        {
            case State::Closed:
                return true;

            case State::Open:
            {
                auto elapsed = std::chrono::steady_clock::now() - circuit.m_OpenedAt;
                if (elapsed >= std::chrono::seconds(m_Config.m_CooldownSeconds))
                {
                    // Transition to HalfOpen
                    circuit.m_State = State::HalfOpen;
                    circuit.m_HalfOpenProbes = 0;
                    LOG_APP_INFO("[circuit-breaker] '{}' transitioning Open -> HalfOpen after {}s cooldown",
                                 connectionName, m_Config.m_CooldownSeconds);
                    return true; // Allow the first probe
                }
                return false; // Still in cooldown
            }

            case State::HalfOpen:
            {
                if (circuit.m_HalfOpenProbes < m_Config.m_HalfOpenProbeCount)
                {
                    ++circuit.m_HalfOpenProbes;
                    return true;
                }
                return false; // Waiting for probe results
            }
        }

        return true;
    }

    void CloudCircuitBreaker::RecordSuccess(std::string const& connectionName)
    {
        std::lock_guard lock(m_Mutex);
        auto& circuit = m_Circuits[connectionName];

        if (circuit.m_State == State::HalfOpen)
        {
            LOG_APP_INFO("[circuit-breaker] '{}' probe succeeded, HalfOpen -> Closed", connectionName);
        }

        circuit.m_State = State::Closed;
        circuit.m_ConsecutiveFailures = 0;
        circuit.m_HalfOpenProbes = 0;
    }

    void CloudCircuitBreaker::RecordFailure(std::string const& connectionName)
    {
        std::lock_guard lock(m_Mutex);
        auto& circuit = m_Circuits[connectionName];

        ++circuit.m_ConsecutiveFailures;

        switch (circuit.m_State)
        {
            case State::Closed:
                if (circuit.m_ConsecutiveFailures >= m_Config.m_FailureThreshold)
                {
                    circuit.m_State = State::Open;
                    circuit.m_OpenedAt = std::chrono::steady_clock::now();
                    LOG_APP_WARN("[circuit-breaker] '{}' opened after {} consecutive failures",
                                 connectionName, circuit.m_ConsecutiveFailures);
                }
                break;

            case State::HalfOpen:
                // Probe failed — go back to Open
                circuit.m_State = State::Open;
                circuit.m_OpenedAt = std::chrono::steady_clock::now();
                LOG_APP_WARN("[circuit-breaker] '{}' probe failed, HalfOpen -> Open", connectionName);
                break;

            case State::Open:
                // Already open, just update timestamp
                break;
        }
    }

    CloudCircuitBreaker::State CloudCircuitBreaker::GetState(std::string const& connectionName) const
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Circuits.find(connectionName);
        if (it == m_Circuits.end())
        {
            return State::Closed;
        }
        return it->second.m_State;
    }

    std::string CloudCircuitBreaker::StateToString(State state)
    {
        switch (state)
        {
            case State::Closed: return "closed";
            case State::Open: return "open";
            case State::HalfOpen: return "half_open";
        }
        return "unknown";
    }

    std::vector<CloudCircuitBreaker::ConnectionHealth> CloudCircuitBreaker::GetHealthSummary() const
    {
        std::lock_guard lock(m_Mutex);
        std::vector<ConnectionHealth> result;
        result.reserve(m_Circuits.size());
        for (auto const& [name, circuit] : m_Circuits)
        {
            ConnectionHealth health;
            health.m_Name = name;
            health.m_State = circuit.m_State;
            health.m_ConsecutiveFailures = circuit.m_ConsecutiveFailures;
            result.push_back(std::move(health));
        }
        return result;
    }
} // namespace AIAssistant
