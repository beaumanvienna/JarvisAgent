/* Copyright (c) 2025 JC Technolabs

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

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "file/pathConfinement.h"
#include "json/configParser.h"
#include "auxiliary/file.h"
#include "network/urlPolicy.h"

namespace AIAssistant
{
    namespace
    {
        // Fallback table for `max_context_tokens` when config.json doesn't set it
        // explicitly.  Matching is case-insensitive substring over the interface's
        // `model` field; the first matching entry wins.  Keep more-specific patterns
        // (e.g. "gpt-5") ahead of more-generic ones (e.g. "gpt-4").
        //
        // Review these when providers ship new generations with different windows.
        //   GPT-5-family:    256K (conservative placeholder — provider may publish higher)
        //   GPT-4-family:    128K (gpt-4.1 / gpt-4o / gpt-4-turbo / gpt-4-mini)
        //   Claude (any):    200K (all current Anthropic tiers — haiku/sonnet/opus)
        //   Gemini 1.5 / 2:  1M
        //   Local/homebrew (Ollama / LM Studio / llama.cpp / etc.):
        //     Llama 3.1+:    128K
        //     Mistral/Mixtral: 32K (most common variants)
        //     Qwen 2.5:      128K
        //     DeepSeek V3:   128K
        // Anything else falls through to `kUnknownModelFallbackTokens` — intentionally
        // small so the chunker fires aggressively on a truly unknown model rather
        // than happily sending an oversized request into the abyss.
        constexpr uint64_t kUnknownModelFallbackTokens = 50000;

        uint64_t ResolveMaxContextTokensFromModelImpl(std::string const& modelName)
        {
            std::string lower = modelName;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char ch) { return std::tolower(ch); });

            struct Pattern
            {
                std::string_view m_Needle;
                uint64_t m_Tokens;
            };
            static constexpr Pattern kPatterns[] = {
                // Hosted / frontier providers
                {"gpt-5",        256000},
                {"gpt-4",        128000},
                {"gpt-4.1",      128000},
                {"gpt-4o",       128000},
                {"gpt-4-turbo",  128000},
                {"claude",       200000},
                {"gemini-2",    1000000},
                {"gemini-1.5",  1000000},
                // Homebrew / local (Ollama / LM Studio / llama.cpp / vLLM / etc.)
                // These match whether the model is tagged as "llama3.1" or
                // "ollama/llama3.1:8b" or "meta-llama-3".  Order: mixtral before
                // mistral so the MoE variant wins its more-specific match.
                {"mixtral",       32000},
                {"mistral",       32000},
                {"llama",        128000},
                {"qwen",         128000},
                {"deepseek",     128000},
                {"phi",          128000},
            };

            for (auto const& pattern : kPatterns)
            {
                if (lower.find(pattern.m_Needle) != std::string::npos)
                {
                    return pattern.m_Tokens;
                }
            }
            return kUnknownModelFallbackTokens;
        }

        // String ↔ InterfaceType mapping.  Single source of truth so the parser
        // (string → enum) and the auto-name generator (enum → string) can't
        // drift when a new provider lands.
        struct InterfaceTypeMapping
        {
            std::string_view m_Name;
            ConfigParser::EngineConfig::InterfaceType m_Type;
        };

        constexpr std::array<InterfaceTypeMapping, 6> kInterfaceTypeMappings = {{
            {"API1", ConfigParser::EngineConfig::InterfaceType::API1},
            {"API2", ConfigParser::EngineConfig::InterfaceType::API2},
            {"API3", ConfigParser::EngineConfig::InterfaceType::API3},
            {"API4", ConfigParser::EngineConfig::InterfaceType::API4},
            {"API5", ConfigParser::EngineConfig::InterfaceType::API5},
            {"API6", ConfigParser::EngineConfig::InterfaceType::API6},
        }};

        // -----------------------------------------------------------------------
        // Field-parse boilerplate helpers — one per JSON value type.
        //
        // Every config field used to be 5–7 lines of `get_X().get(target)` ->
        // LOG_CORE_ERROR -> store -> ++counter, repeated across every field in
        // Parse().  These helpers collapse that to a single line per site so the
        // parser body reads as a field table.
        //
        // Contract for every helper:
        //   - Extract once via simdjson; on type mismatch log
        //     `LOG_CORE_ERROR("ConfigParser: '{}' must be a <type>", key)`
        //     and return without touching the target or counter.
        //   - On success: store into the caller's target, log
        //     `LOG_CORE_INFO("{}: {}", key, value)`, and (if `counter`
        //     is non-null) `++(*counter)`.
        //
        // Counter pointer is optional: a few ApiInterface sub-fields don't
        // track occurrences (`max_context_tokens`, `default_output_tokens`)
        // but those are kept inline anyway because they have silent-on-
        // failure semantics; the nullable-counter design is here so future
        // counter-less helpers can use the same code path without forking.
        //
        // We take a raw `uint32_t*` rather than the private `ConfigFields`
        // enum + `FieldOccurances` array so these helpers stay file-local
        // (anonymous-namespace) without needing `friend ConfigParser`.
        // Call sites pass `&fieldOccurances[ConfigFields::X]` directly.
        // -----------------------------------------------------------------------

        // Numeric-field policies — capture the per-site post-extract checks
        // that vary across the int64 sites.  AcceptAny is the default;
        // others mirror the existing pre-refactor behaviour exactly.
        enum class NumericPolicy
        {
            AcceptAny,            // store the value as-is (sign-permissive).
            RejectNegative,       // negative → log error + skip store/count.
            ClampNegativeToZero,  // negative → cap = 0 then store + count.
            StoreOnlyIfPositive,  // store only if value > 0, but always count.
        };

        void ParseStringField(simdjson::ondemand::value value,
                              std::string_view key,
                              std::string& target,
                              uint32_t* counter)
        {
            std::string_view sv;
            if (value.get_string().get(sv) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("ConfigParser: '{}' must be a string", key);
                return;
            }
            target = std::string(sv);
            LOG_CORE_INFO("{}: {}", key, sv);
            if (counter) ++(*counter);
        }

        // Metadata-only string field (no storage, just validate + log +
        // count).  Used by `description` and `author` which are informative
        // only and never end up in `EngineConfig`.
        void ParseStringFieldLogOnly(simdjson::ondemand::value value,
                                     std::string_view key,
                                     uint32_t* counter)
        {
            std::string_view sv;
            if (value.get_string().get(sv) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("ConfigParser: '{}' must be a string", key);
                return;
            }
            LOG_CORE_INFO("{}: {}", key, sv);
            if (counter) ++(*counter);
        }

        template <typename TargetT>
        void ParseInt64Field(simdjson::ondemand::value value,
                             std::string_view key,
                             TargetT& target,
                             NumericPolicy policy,
                             uint32_t* counter)
        {
            int64_t v = 0;
            if (value.get_int64().get(v) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("ConfigParser: '{}' must be a number", key);
                return;
            }
            // INFO log placement matches pre-refactor: AcceptAny / RejectNegative /
            // StoreOnlyIfPositive log the raw value first; ClampNegativeToZero
            // logs the clamped value so the INFO line reflects what was stored.
            switch (policy)
            {
                case NumericPolicy::AcceptAny:
                    LOG_CORE_INFO("{}: {}", key, v);
                    target = static_cast<TargetT>(v);
                    break;
                case NumericPolicy::RejectNegative:
                    LOG_CORE_INFO("{}: {}", key, v);
                    if (v < 0)
                    {
                        LOG_CORE_ERROR("ConfigParser: '{}' is negative ({}), ignoring; configChecker will assign default",
                                       key, v);
                        return;
                    }
                    target = static_cast<TargetT>(v);
                    break;
                case NumericPolicy::ClampNegativeToZero:
                    if (v < 0) v = 0;
                    LOG_CORE_INFO("{}: {}", key, v);
                    target = static_cast<TargetT>(v);
                    break;
                case NumericPolicy::StoreOnlyIfPositive:
                    LOG_CORE_INFO("{}: {}", key, v);
                    if (v > 0) target = static_cast<TargetT>(v);
                    break;
            }
            if (counter) ++(*counter);
        }

        template <typename TargetT>
        void ParseUint64Field(simdjson::ondemand::value value,
                              std::string_view key,
                              TargetT& target,
                              uint32_t* counter)
        {
            uint64_t v = 0;
            if (value.get_uint64().get(v) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("ConfigParser: '{}' must be a non-negative number", key);
                return;
            }
            target = static_cast<TargetT>(v);
            LOG_CORE_INFO("{}: {}", key, target);
            if (counter) ++(*counter);
        }

        void ParseBoolField(simdjson::ondemand::value value,
                            std::string_view key,
                            bool& target,
                            uint32_t* counter)
        {
            bool v = false;
            if (value.get_bool().get(v) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("ConfigParser: '{}' must be a boolean", key);
                return;
            }
            target = v;
            LOG_CORE_INFO("{}: {}", key, v);
            if (counter) ++(*counter);
        }

        // Numeric field with a closed [min, max] range and a sane default
        // applied + WARN logged when the supplied value lands outside.
        // Used by `port` (range [0, 65535], default 0, warnSuffix " (auto)")
        // and `session_timeout_hours` (range [1, 168], default 8).
        //
        // The WARN format intentionally matches the pre-refactor shape
        // byte-for-byte: `"<key> <value> out of range [<min>, <max>],
        // defaulting to <default><warnSuffix>"` — no `ConfigParser:` prefix,
        // no quotes around the key.  An optional `warnSuffix` lets port
        // keep its " (auto)" annotation without complicating callers.
        template <typename TargetT>
        void ParseInt64FieldWithBounds(simdjson::ondemand::value value,
                                       std::string_view key,
                                       TargetT& target,
                                       int64_t minInclusive, int64_t maxInclusive,
                                       int64_t defaultIfOutOfRange,
                                       uint32_t* counter,
                                       char const* warnSuffix = "")
        {
            int64_t v = 0;
            if (value.get_int64().get(v) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("ConfigParser: '{}' must be a number", key);
                return;
            }
            if (v < minInclusive || v > maxInclusive)
            {
                LOG_CORE_WARN("{} {} out of range [{}, {}], defaulting to {}{}",
                              key, v, minInclusive, maxInclusive, defaultIfOutOfRange, warnSuffix);
                v = defaultIfOutOfRange;
            }
            LOG_CORE_INFO("{}: {}", key, v);
            target = static_cast<TargetT>(v);
            if (counter) ++(*counter);
        }
    } // namespace

    ConfigParser::EngineConfig::InterfaceType
    ConfigParser::EngineConfig::InterfaceTypeFromString(std::string_view apiType)
    {
        for (auto const& mapping : kInterfaceTypeMappings)
        {
            if (mapping.m_Name == apiType) return mapping.m_Type;
        }
        return InterfaceType::InvalidAPI;
    }

    std::string_view ConfigParser::EngineConfig::InterfaceTypeToString(InterfaceType type)
    {
        static_assert(static_cast<int>(InterfaceType::NumAPIs) == 6,
                      "InterfaceType count changed; update kInterfaceTypeMappings and this switch");
        switch (type)
        {
            case InterfaceType::API1: return "API1";
            case InterfaceType::API2: return "API2";
            case InterfaceType::API3: return "API3";
            case InterfaceType::API4: return "API4";
            case InterfaceType::API5: return "API5";
            case InterfaceType::API6: return "API6";
            case InterfaceType::NumAPIs:
            case InterfaceType::InvalidAPI:
                break;
        }
        return "";
    }

    uint64_t ConfigParser::EngineConfig::ResolveMaxContextTokensFromModel(std::string const& modelName)
    {
        return ResolveMaxContextTokensFromModelImpl(modelName);
    }

    ConfigParser::ConfigParser(std::string const& filepathAndFilename)
        : m_ConfigFilepathAndFilename(filepathAndFilename), m_State{ConfigParser::State::Undefined}
    {
    }

    ConfigParser::~ConfigParser() {}

    ConfigParser::State ConfigParser::GetState() const { return m_State; }

    ConfigParser::State ConfigParser::Parse(EngineConfig& engineConfig)
    {
        m_State = ConfigParser::State::Undefined;
        engineConfig = {}; // reset all fields of engine config

        if ((!EngineCore::FileExists(m_ConfigFilepathAndFilename)) || (EngineCore::IsDirectory(m_ConfigFilepathAndFilename)))
        {
            LOG_CORE_ERROR("file {} not found", m_ConfigFilepathAndFilename);
            m_State = ConfigParser::State::FileNotFound;
            return m_State;
        }
        using namespace simdjson;
        ondemand::parser parser;
        padded_string json;
        if (auto err = padded_string::load(m_ConfigFilepathAndFilename).get(json); err != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("ConfigParser::Parse: failed to load '{}': {}", m_ConfigFilepathAndFilename,
                           error_message(err));
            m_State = ConfigParser::State::FileNotFound;
            return m_State;
        }

        ondemand::document doc;
        if (auto err = parser.iterate(json).get(doc); err != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("ConfigParser::Parse: parse failure for '{}': {}", m_ConfigFilepathAndFilename,
                           error_message(err));
            m_State = ConfigParser::State::ParseFailure;
            return m_State;
        }

        ondemand::object jsonObjects;
        if (auto err = doc.get_object().get(jsonObjects); err != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("ConfigParser::Parse: top-level value of '{}' is not a JSON object: {}",
                           m_ConfigFilepathAndFilename, error_message(err));
            m_State = ConfigParser::State::FileFormatFailure;
            return m_State;
        }

        FieldOccurances fieldOccurances{};
        for (auto jsonObject : jsonObjects)
        {
            std::string_view jsonObjectKey = jsonObject.unescaped_key();

            if (jsonObjectKey == "file format identifier")
            {
                ++fieldOccurances[ConfigFields::Format];
            }
            else if (jsonObjectKey == "description")
            {
                ParseStringFieldLogOnly(jsonObject.value(), jsonObjectKey,
                                        &fieldOccurances[ConfigFields::Description]);
            }
            else if (jsonObjectKey == "author")
            {
                ParseStringFieldLogOnly(jsonObject.value(), jsonObjectKey,
                                        &fieldOccurances[ConfigFields::Author]);
            }
            else if (jsonObjectKey == "queue folder")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey,
                                 engineConfig.m_QueueFolderFilepath,
                                 &fieldOccurances[ConfigFields::QueueFolder]);
            }
            else if (jsonObjectKey == "workflows folder")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey,
                                 engineConfig.m_WorkflowsFolderFilepath,
                                 &fieldOccurances[ConfigFields::WorkflowsFolder]);
            }
            else if (jsonObjectKey == "max threads")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_MaxThreads,
                                NumericPolicy::RejectNegative, &fieldOccurances[ConfigFields::MaxThreads]);
            }
            else if (jsonObjectKey == "engine sleep time in run loop in ms")
            {
                int64_t sleepTime = 0;
                ParseInt64Field(jsonObject.value(), jsonObjectKey, sleepTime,
                                NumericPolicy::AcceptAny, &fieldOccurances[ConfigFields::SleepTime]);
                engineConfig.m_SleepDuration = std::chrono::milliseconds(sleepTime);
            }
            else if (jsonObjectKey == "max file size in kB")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_MaxFileSizekB,
                                NumericPolicy::RejectNegative,
                                &fieldOccurances[ConfigFields::MaxFileSizekB]);
            }
            else if (jsonObjectKey == "verbose")
            {
                ParseBoolField(jsonObject.value(), jsonObjectKey, engineConfig.m_Verbose,
                               &fieldOccurances[ConfigFields::Verbose]);
            }
            else if (jsonObjectKey == "api_file")
            {
                // Path to the encrypted AI-routing store (API.json.enc).  The
                // interfaces + default/jcwf selectors themselves live inside it,
                // not in config.json — moved out so they can't be tampered with
                // at rest without the master password.
                ParseStringField(jsonObject.value(), jsonObjectKey, engineConfig.m_ApiFilePath,
                                 &fieldOccurances[ConfigFields::ApiFile]);
            }
            else if (jsonObjectKey == "connections_file")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey, engineConfig.m_ConnectionsFilePath,
                                 &fieldOccurances[ConfigFields::ConnectionsFile]);
            }
            else if (jsonObjectKey == "jcwf batch size")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_JcwfBatchSize,
                                NumericPolicy::StoreOnlyIfPositive,
                                &fieldOccurances[ConfigFields::JcwfBatchSize]);
            }
            else if (jsonObjectKey == "keys_file")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey, engineConfig.m_KeysFilePath,
                                 &fieldOccurances[ConfigFields::KeysFile]);
            }
            else if (jsonObjectKey == "use_bash")
            {
                // Kept inline because the INFO log has a platform-conditional
                // " (Windows-only, ignored on this platform)" annotation on
                // non-Windows that the generic helper can't synthesise.
                bool useBash = false;
                if (jsonObject.value().get_bool().get(useBash) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a boolean", jsonObjectKey);
                    continue;
                }
                engineConfig.m_UseBashOnWindows = useBash;
#if defined(_WIN32)
                LOG_CORE_INFO("use_bash: {}", useBash);
#else
                LOG_CORE_INFO("use_bash: {} (Windows-only, ignored on this platform)", useBash);
#endif
                ++fieldOccurances[ConfigFields::UseBashOnWindows];
            }
            else if (jsonObjectKey == "TlsCert")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey, engineConfig.m_TlsCert,
                                 &fieldOccurances[ConfigFields::TlsCert]);
            }
            else if (jsonObjectKey == "TlsKey")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey, engineConfig.m_TlsKey,
                                 &fieldOccurances[ConfigFields::TlsKey]);
            }
            else if (jsonObjectKey == "TrustedProxyHeader")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey, engineConfig.m_TrustedProxyHeader,
                                 &fieldOccurances[ConfigFields::TrustedProxyHeader]);
            }
            else if (jsonObjectKey == "TrustedRoleHeader")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey, engineConfig.m_TrustedRoleHeader,
                                 &fieldOccurances[ConfigFields::TrustedRoleHeader]);
            }
            else if (jsonObjectKey == "MaxRequestBodyMB")
            {
                ParseUint64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_MaxRequestBodyMB,
                                 &fieldOccurances[ConfigFields::MaxRequestBodyMB]);
            }
            else if (jsonObjectKey == "max inflight ai calls")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_MaxInflightAiCalls,
                                NumericPolicy::RejectNegative,
                                &fieldOccurances[ConfigFields::MaxInflightAiCalls]);
            }
            else if (jsonObjectKey == "max_ai_calls_per_jcwf")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_MaxAiCallsPerJcwf,
                                NumericPolicy::ClampNegativeToZero,
                                &fieldOccurances[ConfigFields::MaxAiCallsPerJcwf]);
            }
            else if (jsonObjectKey == "max_per_item_fan_out")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_MaxPerItemFanOut,
                                NumericPolicy::ClampNegativeToZero,
                                &fieldOccurances[ConfigFields::MaxPerItemFanOut]);
            }
            else if (jsonObjectKey == "python engines")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_PythonEngines,
                                NumericPolicy::RejectNegative,
                                &fieldOccurances[ConfigFields::PythonEngines]);
            }
            else if (jsonObjectKey == "port")
            {
                ParseInt64FieldWithBounds(jsonObject.value(), jsonObjectKey, engineConfig.m_Port,
                                          0, 65535, 0,
                                          &fieldOccurances[ConfigFields::Port], " (auto)");
            }
            else if (jsonObjectKey == "mcp_keys_file")
            {
                ParseStringField(jsonObject.value(), jsonObjectKey, engineConfig.m_McpKeysFilePath,
                                 &fieldOccurances[ConfigFields::McpKeysFile]);
            }
            else if (jsonObjectKey == "session_timeout_hours")
            {
                ParseInt64FieldWithBounds(jsonObject.value(), jsonObjectKey, engineConfig.m_SessionTimeoutHours,
                                          1, 168, 8,
                                          &fieldOccurances[ConfigFields::SessionTimeoutHours]);
            }
            else
            {
                // Try to get the value as a string for display
                try
                {
                    simdjson::ondemand::value val = jsonObject.value();
                    std::string valueString;

                    switch (val.type())
                    {
                        case simdjson::ondemand::json_type::string:
                            valueString = std::string(val.get_string().value());
                            break;
                        case simdjson::ondemand::json_type::number:
                            valueString = std::to_string(val.get_double().value());
                            break;
                        case simdjson::ondemand::json_type::boolean:
                            valueString = val.get_bool().value() ? "true" : "false";
                            break;
                        case simdjson::ondemand::json_type::null:
                            valueString = "null";
                            break;
                        default:
                            valueString = "[complex type]";
                            break;
                    }

                    LOG_CORE_INFO("{}: {}", jsonObjectKey, valueString);
                }
                catch (const simdjson::simdjson_error& e)
                {
                    LOG_CORE_WARN("uncaught json field in config: \"{}\" (failed to stringify, error: {})", jsonObjectKey,
                                  e.what());
                }
            }
        }

        // Declare it ok if the queue + workflows folders were found.  (AI
        // interfaces no longer live in config.json — they moved to the encrypted
        // API.json.enc — so the old "url was seen" gate, which only ever counted
        // interface URLs, no longer applies.)
        if ((fieldOccurances[ConfigFields::QueueFolder] > 0) &&
            (fieldOccurances[ConfigFields::WorkflowsFolder] > 0))
        {
            m_State = ConfigParser::State::ConfigOk;
        }
        else
        {
            m_State = ConfigParser::State::FileFormatFailure;
        }

        {
            LOG_CORE_INFO("format info:");
            for (uint32_t index{0}; auto& fieldOccurance : fieldOccurances)
            {
                LOG_CORE_INFO("field: {}, field occurance: {}", ConfigFieldNames[index], fieldOccurance);
                ++index;
            }
        }
        return m_State;
    }

    bool ConfigParser::ConfigParsed() const { return m_State == State::ConfigOk; }

    std::string ConfigParser::EngineConfig::GenerateInterfaceName(std::string const& url, std::string const& model,
                                                                  std::string const& apiType)
    {
        // Extract domain from URL: "https://api.openai.com/v1/..." → "api.openai.com"
        std::string domain;
        auto schemeEnd = url.find("://");
        if (schemeEnd != std::string::npos)
        {
            auto domainStart = schemeEnd + 3;
            auto domainEnd = url.find('/', domainStart);
            if (domainEnd == std::string::npos)
            {
                domainEnd = url.size();
            }
            domain = url.substr(domainStart, domainEnd - domainStart);
        }
        else
        {
            domain = url;
        }

        if (domain.empty())
        {
            domain = "unknown";
        }

        std::string name = domain;
        if (!model.empty())
        {
            name += "/" + model;
        }
        if (!apiType.empty())
        {
            name += "/" + apiType;
        }

        return name;
    }

} // namespace AIAssistant
