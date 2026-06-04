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

#include <cstdint>
#include <string>

#include "json/configParser.h"
#include "rateLimitObservation.h"

namespace AIAssistant
{
    // Per-provider strategy for extracting rate-limit feedback from a response
    // and for the controller-side metadata (quota key, initial concurrency,
    // input-token estimation) needed to gate dispatch predictively.
    //
    // All methods are stateless / pure so the dispatcher can call them from
    // the I/O thread without locking.  Concrete strategies are singletons in
    // rateLimitStrategy.cpp, returned by Get() per InterfaceType.
    class IRateLimitStrategy
    {
    public:
        virtual ~IRateLimitStrategy() = default;

        // Parse what we got back from this provider.  Headers are the raw
        // accumulated header buffer ("Name: Value\r\n" lines).  Body is the
        // response body (may be empty when only headers are needed).  Status
        // is the HTTP code (used to interpret retry-after on 429s).
        virtual RateLimitObservation Parse(std::string const& headerBuffer, std::string const& responseBody,
                                           int httpStatus) const = 0;

        // Map a model id ("claude-sonnet-4-6", "gpt-4o-mini", "gemini-2.0-pro")
        // to the family that shares one quota bucket on this provider.
        // Anthropic and OpenAI publish per-model-family quotas; the controller
        // is keyed by (host, family) to keep their AIMD signals independent.
        // Strategies that don't need the split (e.g. Test, providers with one
        // bucket per host) return an empty string — controllers treat that as
        // "one bucket per host."
        virtual std::string DeriveQuotaKey(std::string const& model) const = 0;

        // Hard upper bound on initial concurrency for this provider until the
        // first observation lands.  Different providers have different "safe
        // to probe" levels (Anthropic Tier 1 = single-digit; gpt-4o-mini =
        // dozens).  AIMD grows from this value on clean completions, halves
        // on 429.
        virtual int InitialConcurrencyProbe() const = 0;

        // Predict the input-token cost of a request before sending it, so
        // ShouldAdmit() can decide whether the request fits in the remaining
        // token bucket.  Phase 2: char/4 heuristic on the concatenated request
        // body.  Per-provider tokenizers can swap in by overriding this.
        // `concatenatedPrompt` is the union of all messages' content; the
        // caller (AiRequestPool::Submit) does the concat once and reuses it.
        virtual int64_t EstimateInputTokens(std::string const& concatenatedPrompt) const;

        // Factory: returns a reference to the singleton strategy for an
        // interface type.  Switch over InterfaceType is exhaustive per the
        // C++ discipline rule (no default: arm); enum additions break the
        // build until handled here.
        static IRateLimitStrategy const& Get(ConfigParser::EngineConfig::InterfaceType interfaceType);
    };
} // namespace AIAssistant
