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

#include "cloud/cloudRetryPolicy.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace AIAssistant
{
    // Extends CloudRetryPolicy with provider-specific rate-limit semantics.
    //
    // Some providers need awareness beyond simple Retry-After backoff:
    //  - Slack: 429 + Retry-After header, plus secondary rate limits
    //  - GitHub: secondary rate limits (separate from primary)
    //  - Graph API: throttling windows per tenant
    //  - Snowflake: async query polling cadence
    //
    // Each provider type can register its specific config. The policy tracks
    // per-provider request windows and enforces minimum intervals.
    class ProviderRateLimitPolicy
    {
    public:
        struct ProviderConfig
        {
            int m_MinRequestIntervalMs{0};       // Minimum milliseconds between requests to this provider
            int m_BurstLimit{0};                  // Max requests in a burst window (0 = unlimited)
            int m_BurstWindowMs{60000};           // Burst window in milliseconds
            CloudRetryPolicy::Config m_RetryConfig; // Base retry config with provider-specific retry codes
        };

        // Register rate-limit config for a provider type.
        void RegisterProvider(std::string const& providerType, ProviderConfig config);

        // Get the retry config for a provider type. Returns default if not registered.
        CloudRetryPolicy::Config GetRetryConfig(std::string const& providerType) const;

        // Check if a request should be throttled for this provider. Returns the number of
        // milliseconds to wait before sending (0 = send immediately).
        int GetThrottleDelayMs(std::string const& providerType);

        // Record that a request was made to this provider.
        void RecordRequest(std::string const& providerType);

    private:
        struct ProviderState
        {
            ProviderConfig m_Config;
            std::chrono::steady_clock::time_point m_LastRequestTime{};
            int m_BurstCount{0};
            std::chrono::steady_clock::time_point m_BurstWindowStart{};
        };

        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, ProviderState> m_Providers;
    };
} // namespace AIAssistant
