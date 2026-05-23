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

        ConfigParser::EngineConfig::InterfaceType ParseInterfaceType(std::string_view name)
        {
            for (auto const& mapping : kInterfaceTypeMappings)
            {
                if (mapping.m_Name == name) return mapping.m_Type;
            }
            return ConfigParser::EngineConfig::InterfaceType::InvalidAPI;
        }

        std::string_view InterfaceTypeName(ConfigParser::EngineConfig::InterfaceType type)
        {
            static_assert(static_cast<int>(ConfigParser::EngineConfig::InterfaceType::NumAPIs) == 6,
                          "InterfaceType count changed; update kInterfaceTypeMappings and this switch");
            switch (type)
            {
                case ConfigParser::EngineConfig::InterfaceType::API1: return "API1";
                case ConfigParser::EngineConfig::InterfaceType::API2: return "API2";
                case ConfigParser::EngineConfig::InterfaceType::API3: return "API3";
                case ConfigParser::EngineConfig::InterfaceType::API4: return "API4";
                case ConfigParser::EngineConfig::InterfaceType::API5: return "API5";
                case ConfigParser::EngineConfig::InterfaceType::API6: return "API6";
                case ConfigParser::EngineConfig::InterfaceType::NumAPIs:
                case ConfigParser::EngineConfig::InterfaceType::InvalidAPI:
                    break;
            }
            return "";
        }

        // -----------------------------------------------------------------------
        // Field-parse boilerplate helpers — one per JSON value type.
        //
        // Every config field used to be 5–7 lines of `get_X().get(target)` ->
        // LOG_CORE_ERROR -> store -> ++counter, repeated 29 times across
        // Parse() and ParseInterfaces().  These helpers collapse that to a
        // single line per site so the parser body reads as a field table.
        //
        // Contract for every helper:
        //   - Extract once via simdjson; on type mismatch log
        //     `LOG_CORE_ERROR("{} '{}' must be a <type>", logPrefix, key)`
        //     and return without touching the target or counter.
        //   - On success: store into the caller's target, log
        //     `LOG_CORE_INFO("{}: {}", key, value)`, and (if `counter`
        //     is non-null) `++(*counter)`.
        //
        // The `logPrefix` parameter exists because the top-level Parse()
        // says "ConfigParser:" while ParseInterfaces() says
        // "ConfigParser: API interface" — same shape, different scope.
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
                              uint32_t* counter,
                              char const* logPrefix = "ConfigParser:")
        {
            std::string_view sv;
            if (value.get_string().get(sv) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("{} '{}' must be a string", logPrefix, key);
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
                                     uint32_t* counter,
                                     char const* logPrefix = "ConfigParser:")
        {
            std::string_view sv;
            if (value.get_string().get(sv) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("{} '{}' must be a string", logPrefix, key);
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
                             uint32_t* counter,
                             char const* logPrefix = "ConfigParser:")
        {
            int64_t v = 0;
            if (value.get_int64().get(v) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("{} '{}' must be a number", logPrefix, key);
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
                        LOG_CORE_ERROR("{} '{}' is negative ({}), ignoring; configChecker will assign default",
                                       logPrefix, key, v);
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
                              uint32_t* counter,
                              char const* logPrefix = "ConfigParser:")
        {
            uint64_t v = 0;
            if (value.get_uint64().get(v) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("{} '{}' must be a non-negative number", logPrefix, key);
                return;
            }
            target = static_cast<TargetT>(v);
            LOG_CORE_INFO("{}: {}", key, target);
            if (counter) ++(*counter);
        }

        void ParseBoolField(simdjson::ondemand::value value,
                            std::string_view key,
                            bool& target,
                            uint32_t* counter,
                            char const* logPrefix = "ConfigParser:")
        {
            bool v = false;
            if (value.get_bool().get(v) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("{} '{}' must be a boolean", logPrefix, key);
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
                                       char const* warnSuffix = "",
                                       char const* logPrefix = "ConfigParser:")
        {
            int64_t v = 0;
            if (value.get_int64().get(v) != simdjson::SUCCESS)
            {
                LOG_CORE_ERROR("{} '{}' must be a number", logPrefix, key);
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
            else if (jsonObjectKey == "API interfaces")
            {
                ondemand::array array;
                if (jsonObject.value().get_array().get(array) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be an array", jsonObjectKey);
                    continue;
                }
                ParseInterfaces(array, engineConfig, fieldOccurances);
            }
            else if (jsonObjectKey == "API index")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_ApiIndex,
                                NumericPolicy::RejectNegative, &fieldOccurances[ConfigFields::ApiIndex]);
            }
            else if (jsonObjectKey == "jcwf batch size")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_JcwfBatchSize,
                                NumericPolicy::StoreOnlyIfPositive,
                                &fieldOccurances[ConfigFields::JcwfBatchSize]);
            }
            else if (jsonObjectKey == "jcwf AI interface")
            {
                ParseInt64Field(jsonObject.value(), jsonObjectKey, engineConfig.m_JcwfAiInterfaceIndex,
                                NumericPolicy::AcceptAny, &fieldOccurances[ConfigFields::JcwfAiInterface]);
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

        // declare it ok if queue folder filepath and url were found
        if ((fieldOccurances[ConfigFields::QueueFolder] > 0) && (fieldOccurances[ConfigFields::Url] > 0))
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

    void ConfigParser::ParseInterfaces(simdjson::ondemand::array jsonArray, EngineConfig& engineConfig,
                                       FieldOccurances& fieldOccurances)
    {
        using namespace simdjson;

        for (auto element : jsonArray)
        {
            ondemand::object interface = element.get_object();

            EngineConfig::ApiInterface apiInterface;

            for (auto field : interface)
            {
                std::string_view jsonObjectKey = field.unescaped_key();

                if (jsonObjectKey == "name")
                {
                    ParseStringField(field.value(), jsonObjectKey, apiInterface.m_Name,
                                     &fieldOccurances[ConfigFields::InterfaceName],
                                     "ConfigParser: API interface");
                }
                else if (jsonObjectKey == "description")
                {
                    ParseStringField(field.value(), jsonObjectKey, apiInterface.m_Description,
                                     &fieldOccurances[ConfigFields::InterfaceDescription],
                                     "ConfigParser: API interface");
                }
                else if (jsonObjectKey == "key_name")
                {
                    ParseStringField(field.value(), jsonObjectKey, apiInterface.m_KeyName,
                                     &fieldOccurances[ConfigFields::InterfaceKeyName],
                                     "ConfigParser: API interface");
                }
                else if (jsonObjectKey == "url")
                {
                    ParseStringField(field.value(), jsonObjectKey, apiInterface.m_Url,
                                     &fieldOccurances[ConfigFields::Url],
                                     "ConfigParser: API interface");
                }
                else if (jsonObjectKey == "model")
                {
                    ParseStringField(field.value(), jsonObjectKey, apiInterface.m_Model,
                                     &fieldOccurances[ConfigFields::Model],
                                     "ConfigParser: API interface");
                }
                else if (jsonObjectKey == "max_context_tokens")
                {
                    uint64_t value = 0;
                    if (field.value().get_uint64().get(value) == simdjson::SUCCESS)
                    {
                        apiInterface.m_MaxContextTokens = value;
                        LOG_CORE_INFO("max_context_tokens: {}", value);
                    }
                }
                else if (jsonObjectKey == "default_output_tokens")
                {
                    int64_t value = 0;
                    if (field.value().get_int64().get(value) == simdjson::SUCCESS && value > 0)
                    {
                        apiInterface.m_DefaultOutputTokens = static_cast<int32_t>(value);
                        LOG_CORE_INFO("default_output_tokens: {}", value);
                    }
                }
                else if (jsonObjectKey == "rate_limit")
                {
                    // Adaptive rate-limit + size-aware budget block.  All sub-fields
                    // optional; missing fields keep RateLimit/RequestBudget defaults.
                    ondemand::object rateLimitObject;
                    if (field.value().get_object().get(rateLimitObject) == simdjson::SUCCESS)
                    {
                        auto& rateLimit = apiInterface.m_RateLimit;
                        auto& budget = rateLimit.m_RequestBudget;
                        for (auto rlField : rateLimitObject)
                        {
                            std::string_view rlKey = rlField.unescaped_key();
                            if (rlKey == "initial_concurrency_probe")
                            {
                                int64_t v = 0;
                                if (rlField.value().get_int64().get(v) == simdjson::SUCCESS)
                                    rateLimit.m_InitialConcurrencyProbe = static_cast<int>(v);
                            }
                            else if (rlKey == "max_concurrency")
                            {
                                int64_t v = 0;
                                if (rlField.value().get_int64().get(v) == simdjson::SUCCESS && v > 0)
                                    rateLimit.m_MaxConcurrency = static_cast<int>(v);
                            }
                            else if (rlKey == "max_retries_429")
                            {
                                int64_t v = 0;
                                if (rlField.value().get_int64().get(v) == simdjson::SUCCESS && v >= 0)
                                    rateLimit.m_MaxRetries429 = static_cast<int>(v);
                            }
                            else if (rlKey == "max_retries_transient")
                            {
                                int64_t v = 0;
                                if (rlField.value().get_int64().get(v) == simdjson::SUCCESS && v >= 0)
                                    rateLimit.m_MaxRetriesTransient = static_cast<int>(v);
                            }
                            else if (rlKey == "base_retry_ms")
                            {
                                int64_t v = 0;
                                if (rlField.value().get_int64().get(v) == simdjson::SUCCESS && v > 0)
                                    rateLimit.m_BaseRetryMs = static_cast<int>(v);
                            }
                            else if (rlKey == "request_budget")
                            {
                                ondemand::object budgetObject;
                                if (rlField.value().get_object().get(budgetObject) == simdjson::SUCCESS)
                                {
                                    for (auto bField : budgetObject)
                                    {
                                        std::string_view bKey = bField.unescaped_key();
                                        double v = 0.0;
                                        if (bField.value().get_double().get(v) != simdjson::SUCCESS)
                                            continue;
                                        if (bKey == "per_1k_input_token_seconds")
                                            budget.m_Per1kInputTokenSeconds = v;
                                        else if (bKey == "per_1k_output_token_seconds")
                                            budget.m_Per1kOutputTokenSeconds = v;
                                        else if (bKey == "fixed_overhead_seconds")
                                            budget.m_FixedOverheadSeconds = v;
                                        else if (bKey == "safety_margin_factor")
                                            budget.m_SafetyMarginFactor = v;
                                        else if (bKey == "min_seconds")
                                            budget.m_MinSeconds = v;
                                        else if (bKey == "max_seconds")
                                            budget.m_MaxSeconds = v;
                                    }
                                }
                            }
                        }
                        LOG_CORE_INFO("rate_limit: maxConcurrency={} budget=[in={}s/1k out={}s/1k overhead={}s margin=x{} "
                                      "min={}s max={}s]",
                                      rateLimit.m_MaxConcurrency, budget.m_Per1kInputTokenSeconds,
                                      budget.m_Per1kOutputTokenSeconds, budget.m_FixedOverheadSeconds,
                                      budget.m_SafetyMarginFactor, budget.m_MinSeconds, budget.m_MaxSeconds);
                    }
                }
                else if (jsonObjectKey == "API")
                {
                    std::string_view api;
                    if (field.value().get_string().get(api) != simdjson::SUCCESS)
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface '{}' must be a string", jsonObjectKey);
                        continue;
                    }
                    LOG_CORE_INFO("API: {}", api);
                    // Legacy "Test" api_type — superseded by the is_mock flag.
                    // Reject with explicit migration guidance so the operator
                    // knows what to change.
                    if (api == "Test")
                    {
                        LOG_CORE_ERROR("ConfigParser: api_type 'Test' has been removed; migrate this interface to "
                                       "api_type: '<API1..API6>' + is_mock: true + fixture_path: '<path>'.  See "
                                       "doc/jarvisagent.md is_mock section.  Interface will be marked InvalidAPI "
                                       "and skipped by configChecker.");
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::InvalidAPI;
                        ++fieldOccurances[ConfigFields::InterfaceType];
                        continue;
                    }
                    apiInterface.m_InterfaceType = ParseInterfaceType(api);
                    if (apiInterface.m_InterfaceType == EngineConfig::InterfaceType::InvalidAPI)
                    {
                        LOG_CORE_ERROR("ConfigParser: unknown API '{}' in interface (expected API1-API6); "
                                       "interface will be marked InvalidAPI and skipped by configChecker",
                                       api);
                    }
                    ++fieldOccurances[ConfigFields::InterfaceType];
                }
                else if (jsonObjectKey == "is_mock")
                {
                    // Dispatch selection on this flag routes to MockTransport
                    // instead of LiveTransport.  Fixture-path containment + size cap +
                    // status/header allowlist enforced at consume time.
                    bool value = false;
                    if (field.value().get_bool().get(value) == simdjson::SUCCESS)
                    {
                        apiInterface.m_IsMock = value;
                        LOG_CORE_INFO("is_mock: {}", value);
                    }
                    else
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface 'is_mock' must be a bool");
                    }
                }
                else if (jsonObjectKey == "fixture_path")
                {
                    std::string_view fixturePath;
                    if (field.value().get_string().get(fixturePath) == simdjson::SUCCESS)
                    {
                        apiInterface.m_FixturePath = fixturePath;
                        LOG_CORE_INFO("fixture_path: {}", fixturePath);
                    }
                    else
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface 'fixture_path' must be a string");
                    }
                }
            }

            // is_mock + fixture_path coupling: when is_mock is true the
            // fixture_path field must be present AND resolve under the
            // project root.  Defense-in-depth — MockTransport re-checks at
            // load time, but rejecting at config parse fails the interface
            // up front and gives the operator a clear error message.
            if (apiInterface.m_IsMock)
            {
                if (apiInterface.m_FixturePath.empty())
                {
                    LOG_CORE_ERROR("ConfigParser: API interface has is_mock=true but no fixture_path; mock dispatch "
                                   "requires a non-empty fixture_path.  Marking interface InvalidAPI.");
                    apiInterface.m_InterfaceType = EngineConfig::InterfaceType::InvalidAPI;
                }
                else
                {
                    std::filesystem::path const confined = ConfineUnderProjectRoot(apiInterface.m_FixturePath);
                    if (confined.empty())
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface fixture_path '{}' rejected by "
                                       "ConfineUnderProjectRoot (outside project root, symlink escape, or "
                                       "unresolvable).  Marking interface InvalidAPI.",
                                       apiInterface.m_FixturePath);
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::InvalidAPI;
                    }
                }
            }

            // Auto-generate name from URL domain + model + API type if not provided
            if (apiInterface.m_Name.empty())
            {
                std::string_view const apiTypeStr = InterfaceTypeName(apiInterface.m_InterfaceType);
                apiInterface.m_Name =
                    EngineConfig::GenerateInterfaceName(apiInterface.m_Url, apiInterface.m_Model, std::string(apiTypeStr));
                LOG_CORE_INFO("auto-generated interface name: {}", apiInterface.m_Name);
            }

            // If config.json didn't set max_context_tokens explicitly, resolve it
            // from a curated model-name table.  Unknown models fall back to a
            // conservative 50 K limit so the chunker fires aggressively rather
            // than dispatching an oversized request that the provider may reject.
            if (apiInterface.m_MaxContextTokens == 0 && !apiInterface.m_Model.empty())
            {
                uint64_t const resolved = ResolveMaxContextTokensFromModelImpl(apiInterface.m_Model);
                apiInterface.m_MaxContextTokens = resolved;
                bool const matched = (resolved != kUnknownModelFallbackTokens);
                LOG_CORE_INFO("max_context_tokens for '{}' model='{}': {} (source: {})",
                              apiInterface.m_Name, apiInterface.m_Model, resolved,
                              matched ? "model-name fallback table" : "unknown-model default 50000");
            }

            engineConfig.m_ApiInterfaces.push_back(std::move(apiInterface));
        }
    }
} // namespace AIAssistant
