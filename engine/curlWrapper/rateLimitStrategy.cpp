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

#include "rateLimitStrategy.h"

#include <cctype>
#include <cstdio>
#include <ctime>

namespace AIAssistant
{
    namespace
    {
        // Iterate the header buffer line-by-line, calling visitor(name, value).
        // Names are lower-cased; values are trimmed of leading whitespace.  The
        // caller does case-insensitive prefix match against the lower-cased name.
        // Header buffer format: "Name: Value\r\n" lines concatenated.
        template <typename Visitor> void IterateHeaderLines(std::string const& headerBuffer, Visitor visitor)
        {
            size_t pos = 0;
            while (pos < headerBuffer.size())
            {
                size_t eol = headerBuffer.find('\n', pos);
                if (eol == std::string::npos)
                    eol = headerBuffer.size();

                std::string line = headerBuffer.substr(pos, eol - pos);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                size_t const colon = line.find(':');
                if (colon != std::string::npos)
                {
                    std::string name;
                    name.reserve(colon);
                    for (size_t i = 0; i < colon; ++i)
                    {
                        name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(line[i]))));
                    }

                    size_t valueStart = colon + 1;
                    while (valueStart < line.size() && line[valueStart] == ' ')
                        ++valueStart;

                    std::string value = line.substr(valueStart);
                    visitor(name, value);
                }

                pos = eol + 1;
            }
        }

        // Parse OpenAI-style ratelimit-reset duration: "200ms" / "6s" / "1m30s".
        // Returns total milliseconds; 0 on parse failure.
        int ParseOpenAIDurationMs(std::string const& value)
        {
            int ms = 0;
            size_t i = 0;
            while (i < value.size())
            {
                if (!std::isdigit(static_cast<unsigned char>(value[i])))
                {
                    ++i;
                    continue;
                }
                int num = 0;
                while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i])))
                {
                    num = num * 10 + (value[i] - '0');
                    ++i;
                }
                if (i + 1 < value.size() && value[i] == 'm' && value[i + 1] == 's')
                {
                    ms += num;
                    i += 2;
                }
                else if (i < value.size() && value[i] == 'm')
                {
                    ms += num * 60000;
                    ++i;
                }
                else if (i < value.size() && value[i] == 's')
                {
                    ms += num * 1000;
                    ++i;
                }
            }
            return ms;
        }

        // Parse ISO 8601 UTC timestamp like "2025-01-01T12:34:56Z" into a
        // delta-from-now in seconds.  Returns -1 on parse failure or when the
        // delta is negative or implausibly large (>2h, treated as garbage).
        long ParseIso8601DeltaSec(std::string const& value)
        {
            int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
            if (std::sscanf(value.c_str(), "%d-%d-%dT%d:%d:%dZ", &year, &mon, &day, &hour, &min, &sec) != 6)
                return -1;

            std::tm tm{};
            tm.tm_year = year - 1900;
            tm.tm_mon = mon - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_min = min;
            tm.tm_sec = sec;
            std::time_t const resetWallT = timegm(&tm);
            std::time_t const nowWallT = std::time(nullptr);
            long const deltaSec = static_cast<long>(resetWallT - nowWallT);
            if (deltaSec <= 0 || deltaSec >= 7200)
                return -1;
            return deltaSec;
        }

        // ----------------------------------------------------------------
        // Empty strategy — providers without proactive rate-limit feedback.
        // Used for InterfaceType::API3 (Gemini), API5 (Bedrock), Test until
        // §2 verification confirms what (if anything) they expose.  Phase 3
        // will refine these with proper provider-specific parsing.
        // ----------------------------------------------------------------
        class RateLimitStrategyEmpty final : public IRateLimitStrategy
        {
        public:
            RateLimitObservation Parse(std::string const&, std::string const&, int) const override
            {
                return {};
            }

            // No per-model-family split known; one bucket per host.
            std::string DeriveQuotaKey(std::string const&) const override { return {}; }

            // Conservative starting point — providers without proactive feedback
            // can only learn about their cap via 429s, so probe slowly.
            int InitialConcurrencyProbe() const override { return 4; }
        };

        // ----------------------------------------------------------------
        // OpenAI strategy — API1 (Chat Completions), API2 (Responses), API6
        // (Azure OpenAI shares the x-ratelimit-* family per Microsoft docs;
        // verify in Phase 3, may need a dedicated strategy for AOAI deployments
        // that report TPM/RPM separately).
        // ----------------------------------------------------------------
        class RateLimitStrategyOpenAI final : public IRateLimitStrategy
        {
        public:
            RateLimitObservation Parse(std::string const& headerBuffer, std::string const&, int) const override
            {
                RateLimitObservation observation;
                auto const now = std::chrono::steady_clock::now();

                IterateHeaderLines(headerBuffer, [&](std::string const& name, std::string const& value) {
                    if (name == "x-ratelimit-remaining-requests")
                    {
                        try
                        {
                            observation.m_RemainingRequests = std::stoll(value);
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (name == "x-ratelimit-remaining-tokens")
                    {
                        try
                        {
                            observation.m_RemainingCombinedTokens = std::stoll(value);
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (name == "x-ratelimit-reset-requests")
                    {
                        int const ms = ParseOpenAIDurationMs(value);
                        if (ms > 0)
                            observation.m_RequestsResetAt = now + std::chrono::milliseconds(ms);
                    }
                    else if (name == "x-ratelimit-reset-tokens")
                    {
                        int const ms = ParseOpenAIDurationMs(value);
                        if (ms > 0)
                            observation.m_TokensResetAt = now + std::chrono::milliseconds(ms);
                    }
                    else if (name == "retry-after")
                    {
                        try
                        {
                            int const seconds = std::stoi(value);
                            if (seconds > 0 && seconds < 7200)
                                observation.m_RetryAfter = std::chrono::seconds(seconds);
                        }
                        catch (...)
                        {
                        }
                    }
                });

                return observation;
            }

            // OpenAI publishes per-model RPM/TPM ceilings (gpt-4o-mini differs
            // from gpt-4o).  Split by model family — first segment of the
            // model id, normalized.  "gpt-4o-mini" -> "gpt-4o", "gpt-5-nano" ->
            // "gpt-5", "o1-preview" -> "o1".
            std::string DeriveQuotaKey(std::string const& model) const override
            {
                if (model.empty())
                    return {};
                // Strip trailing version / variant suffixes after the second hyphen.
                // gpt-4o, gpt-4o-mini, gpt-4-turbo, gpt-3.5, gemini-2.5 (when
                // accessed via OpenAI compat shim), o1, o1-mini.
                size_t firstHyphen = model.find('-');
                if (firstHyphen == std::string::npos)
                    return model;
                size_t secondHyphen = model.find('-', firstHyphen + 1);
                if (secondHyphen == std::string::npos)
                    return model;
                return model.substr(0, secondHyphen);
            }

            // OpenAI accounts on tier 1+ comfortably handle 8 concurrent
            // gpt-4o-mini calls; gpt-4o tier 1 is tighter.  4 is a safe
            // starting point; AIMD grows from there.
            int InitialConcurrencyProbe() const override { return 8; }
        };

        // ----------------------------------------------------------------
        // Anthropic strategy — API4 (Messages).  Splits input/output token
        // quotas; reset times are ISO 8601 wall-clock UTC; retry-after on 429
        // is in seconds.
        // ----------------------------------------------------------------
        class RateLimitStrategyAnthropic final : public IRateLimitStrategy
        {
        public:
            RateLimitObservation Parse(std::string const& headerBuffer, std::string const&, int) const override
            {
                RateLimitObservation observation;
                auto const now = std::chrono::steady_clock::now();

                IterateHeaderLines(headerBuffer, [&](std::string const& name, std::string const& value) {
                    if (name == "anthropic-ratelimit-requests-remaining")
                    {
                        try
                        {
                            observation.m_RemainingRequests = std::stoll(value);
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (name == "anthropic-ratelimit-tokens-remaining")
                    {
                        try
                        {
                            observation.m_RemainingCombinedTokens = std::stoll(value);
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (name == "anthropic-ratelimit-input-tokens-remaining")
                    {
                        try
                        {
                            observation.m_RemainingInputTokens = std::stoll(value);
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (name == "anthropic-ratelimit-output-tokens-remaining")
                    {
                        try
                        {
                            observation.m_RemainingOutputTokens = std::stoll(value);
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (name == "anthropic-ratelimit-requests-reset")
                    {
                        long const deltaSec = ParseIso8601DeltaSec(value);
                        if (deltaSec > 0)
                            observation.m_RequestsResetAt = now + std::chrono::seconds(deltaSec);
                    }
                    else if (name == "anthropic-ratelimit-tokens-reset" ||
                             name == "anthropic-ratelimit-input-tokens-reset" ||
                             name == "anthropic-ratelimit-output-tokens-reset")
                    {
                        // Take the LATEST reset across the three token buckets so we
                        // wait for the slowest-refilling one — matches existing behavior.
                        long const deltaSec = ParseIso8601DeltaSec(value);
                        if (deltaSec > 0)
                        {
                            auto const candidate = now + std::chrono::seconds(deltaSec);
                            if (!observation.m_TokensResetAt.has_value() || candidate > *observation.m_TokensResetAt)
                                observation.m_TokensResetAt = candidate;
                        }
                    }
                    else if (name == "retry-after")
                    {
                        try
                        {
                            int const seconds = std::stoi(value);
                            if (seconds > 0 && seconds < 7200)
                                observation.m_RetryAfter = std::chrono::seconds(seconds);
                        }
                        catch (...)
                        {
                        }
                    }
                });

                return observation;
            }

            // Anthropic publishes per-model-family quotas: claude-haiku /
            // claude-sonnet / claude-opus each have their own RPM + TPM on the
            // same account.  "claude-sonnet-4-6" -> "claude-sonnet",
            // "claude-haiku-4-5-20251001" -> "claude-haiku".
            std::string DeriveQuotaKey(std::string const& model) const override
            {
                if (model.empty())
                    return {};
                size_t firstHyphen = model.find('-');
                if (firstHyphen == std::string::npos)
                    return model;
                size_t secondHyphen = model.find('-', firstHyphen + 1);
                if (secondHyphen == std::string::npos)
                    return model;
                return model.substr(0, secondHyphen);
            }

            // Anthropic Tier 1 caps RPM at single digits for Sonnet/Opus and
            // is generous for Haiku.  Start conservative; AIMD grows on clean
            // streaks and halves on 429.  Tier-3+ accounts can override via
            // config.json rate_limit.initial_concurrency_probe (Phase 4).
            int InitialConcurrencyProbe() const override { return 4; }
        };

        // Singleton strategy instances.  Stateless, so one per type is reused
        // across all calls (and threads — Parse is pure).
        RateLimitStrategyEmpty const s_Empty{};
        RateLimitStrategyOpenAI const s_OpenAI{};
        RateLimitStrategyAnthropic const s_Anthropic{};
    } // anonymous namespace

    int64_t IRateLimitStrategy::EstimateInputTokens(std::string const& concatenatedPrompt) const
    {
        // chars / 4 — good enough at the 80% level for English text.  Real
        // usage from the response body's `usage` field overrides the estimate
        // via Observe() once the request completes.  Per-provider tokenizers
        // (tiktoken, sentencepiece) can swap in by overriding this.
        return static_cast<int64_t>(concatenatedPrompt.size()) / 4;
    }

    IRateLimitStrategy const& IRateLimitStrategy::Get(ConfigParser::EngineConfig::InterfaceType interfaceType)
    {
        // Exhaustive switch — no default: arm.  Adding a new InterfaceType
        // breaks the build here until handled, per CLAUDE.md discipline.
        // NumAPIs / InvalidAPI are sentinel values, not real providers.
        static_assert(ConfigParser::EngineConfig::InterfaceType::NumAPIs == 7,
                      "extend IRateLimitStrategy::Get when adding a new InterfaceType");

        switch (interfaceType)
        {
            case ConfigParser::EngineConfig::InterfaceType::API1:
                return s_OpenAI;
            case ConfigParser::EngineConfig::InterfaceType::API2:
                return s_OpenAI;
            case ConfigParser::EngineConfig::InterfaceType::API3:
                return s_Empty;
            case ConfigParser::EngineConfig::InterfaceType::API4:
                return s_Anthropic;
            case ConfigParser::EngineConfig::InterfaceType::Test:
                return s_Empty;
            case ConfigParser::EngineConfig::InterfaceType::API5:
                return s_Empty;
            case ConfigParser::EngineConfig::InterfaceType::API6:
                return s_OpenAI;
            case ConfigParser::EngineConfig::InterfaceType::NumAPIs:
            case ConfigParser::EngineConfig::InterfaceType::InvalidAPI:
                return s_Empty;
        }
        return s_Empty;
    }
} // namespace AIAssistant
