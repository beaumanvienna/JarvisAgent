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

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

#include "engine.h"
#include "cloud/cloudRetryPolicy.h"

namespace AIAssistant
{
    CloudRetryPolicy::AttemptResult CloudRetryPolicy::Execute(std::function<AttemptResult()> const& fn,
                                                              Config const& config)
    {
        AttemptResult result;

        for (int attempt = 0; attempt <= config.m_MaxRetries; ++attempt)
        {
            result = fn();

            if (result.m_Success)
            {
                return result;
            }

            // Check if the HTTP status code is retryable
            if (result.m_HttpStatusCode != 0 && config.m_RetryableHttpCodes.find(result.m_HttpStatusCode) ==
                                                    config.m_RetryableHttpCodes.end())
            {
                // Non-retryable status code — fail immediately
                return result;
            }

            if (attempt < config.m_MaxRetries)
            {
                int backoffMs;

                // Respect Retry-After header if present
                if (result.m_RetryAfterSeconds > 0)
                {
                    backoffMs = result.m_RetryAfterSeconds * 1000;
                    // Clamp to max backoff to avoid unbounded waits
                    backoffMs = std::min(backoffMs, config.m_MaxBackoffMs);
                }
                else
                {
                    backoffMs = ComputeBackoffMs(attempt, config);
                }

                LOG_APP_INFO("CloudRetryPolicy: attempt {}/{} failed (HTTP {}), retrying in {} ms: {}", attempt + 1,
                             config.m_MaxRetries + 1, result.m_HttpStatusCode, backoffMs, result.m_ErrorMessage);

                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            }
        }

        return result;
    }

    CloudRetryPolicy::AttemptResult CloudRetryPolicy::Execute(std::function<AttemptResult()> const& fn)
    {
        Config defaultConfig;
        return Execute(fn, defaultConfig);
    }

    int CloudRetryPolicy::ComputeBackoffMs(int attempt, Config const& config)
    {
        // Exponential backoff: initialBackoff * 2^attempt
        int baseMs = config.m_InitialBackoffMs;
        for (int i = 0; i < attempt; ++i)
        {
            baseMs *= 2;
            if (baseMs >= config.m_MaxBackoffMs)
            {
                baseMs = config.m_MaxBackoffMs;
                break;
            }
        }

        // Apply jitter: +/- jitterFactor
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> dist(1.0 - config.m_JitterFactor, 1.0 + config.m_JitterFactor);
        int jitteredMs = static_cast<int>(baseMs * dist(rng));

        return std::clamp(jitteredMs, 0, config.m_MaxBackoffMs);
    }
} // namespace AIAssistant
