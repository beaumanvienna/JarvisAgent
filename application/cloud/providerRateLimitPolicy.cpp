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

#include "cloud/providerRateLimitPolicy.h"

namespace AIAssistant
{
    void ProviderRateLimitPolicy::RegisterProvider(std::string const& providerType, ProviderConfig config)
    {
        std::lock_guard lock(m_Mutex);
        auto& state = m_Providers[providerType];
        state.m_Config = std::move(config);
    }

    CloudRetryPolicy::Config ProviderRateLimitPolicy::GetRetryConfig(std::string const& providerType) const
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Providers.find(providerType);
        if (it != m_Providers.end())
        {
            return it->second.m_Config.m_RetryConfig;
        }
        return {}; // Default config
    }

    int ProviderRateLimitPolicy::GetThrottleDelayMs(std::string const& providerType)
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Providers.find(providerType);
        if (it == m_Providers.end())
        {
            return 0;
        }

        auto& state = it->second;
        auto now = std::chrono::steady_clock::now();

        // Check minimum request interval
        if (state.m_Config.m_MinRequestIntervalMs > 0)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.m_LastRequestTime);
            int remaining = state.m_Config.m_MinRequestIntervalMs - static_cast<int>(elapsed.count());
            if (remaining > 0)
            {
                return remaining;
            }
        }

        // Check burst limit
        if (state.m_Config.m_BurstLimit > 0)
        {
            auto windowElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.m_BurstWindowStart);
            if (windowElapsed.count() > state.m_Config.m_BurstWindowMs)
            {
                // Reset burst window
                state.m_BurstCount = 0;
                state.m_BurstWindowStart = now;
            }

            if (state.m_BurstCount >= state.m_Config.m_BurstLimit)
            {
                // Must wait until current window expires
                int remaining = state.m_Config.m_BurstWindowMs - static_cast<int>(windowElapsed.count());
                return std::max(remaining, 1);
            }
        }

        return 0;
    }

    void ProviderRateLimitPolicy::RecordRequest(std::string const& providerType)
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Providers.find(providerType);
        if (it == m_Providers.end())
        {
            return;
        }

        auto& state = it->second;
        state.m_LastRequestTime = std::chrono::steady_clock::now();
        ++state.m_BurstCount;
    }
} // namespace AIAssistant
