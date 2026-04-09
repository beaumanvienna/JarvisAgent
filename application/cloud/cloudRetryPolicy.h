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

#include <functional>
#include <set>
#include <string>

namespace AIAssistant
{
    class CloudRetryPolicy
    {
    public:
        struct Config
        {
            int m_MaxRetries{3};
            int m_InitialBackoffMs{1000};
            int m_MaxBackoffMs{30000};
            double m_JitterFactor{0.2};                                       // +/- 20% randomization
            std::set<int> m_RetryableHttpCodes{429, 500, 502, 503, 504};
        };

        // Result of a single attempt returned by the caller's function.
        struct AttemptResult
        {
            bool m_Success{false};
            int m_HttpStatusCode{0};
            int m_RetryAfterSeconds{0}; // From Retry-After header; 0 = not present
            std::string m_ErrorMessage;
        };

        // Execute fn() with retries according to config.
        // fn must return an AttemptResult. Returns the result of the last attempt.
        static AttemptResult Execute(std::function<AttemptResult()> const& fn, Config const& config);

        // Convenience overload with default config.
        static AttemptResult Execute(std::function<AttemptResult()> const& fn);

    private:
        static int ComputeBackoffMs(int attempt, Config const& config);
    };
} // namespace AIAssistant
