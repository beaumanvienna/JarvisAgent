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

#include "simdjson/simdjson.h"

#include "engine.h"
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

        constexpr std::array<InterfaceTypeMapping, 7> kInterfaceTypeMappings = {{
            {"API1", ConfigParser::EngineConfig::InterfaceType::API1},
            {"API2", ConfigParser::EngineConfig::InterfaceType::API2},
            {"API3", ConfigParser::EngineConfig::InterfaceType::API3},
            {"API4", ConfigParser::EngineConfig::InterfaceType::API4},
            {"Test", ConfigParser::EngineConfig::InterfaceType::Test},
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
            static_assert(static_cast<int>(ConfigParser::EngineConfig::InterfaceType::NumAPIs) == 7,
                          "InterfaceType count changed; update kInterfaceTypeMappings and this switch");
            switch (type)
            {
                case ConfigParser::EngineConfig::InterfaceType::API1: return "API1";
                case ConfigParser::EngineConfig::InterfaceType::API2: return "API2";
                case ConfigParser::EngineConfig::InterfaceType::API3: return "API3";
                case ConfigParser::EngineConfig::InterfaceType::API4: return "API4";
                case ConfigParser::EngineConfig::InterfaceType::Test: return "Test";
                case ConfigParser::EngineConfig::InterfaceType::API5: return "API5";
                case ConfigParser::EngineConfig::InterfaceType::API6: return "API6";
                case ConfigParser::EngineConfig::InterfaceType::NumAPIs:
                case ConfigParser::EngineConfig::InterfaceType::InvalidAPI:
                    break;
            }
            return "";
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
                std::string_view description;
                if (jsonObject.value().get_string().get(description) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("description: {}", description);
                ++fieldOccurances[ConfigFields::Description];
            }
            else if (jsonObjectKey == "author")
            {
                std::string_view author;
                if (jsonObject.value().get_string().get(author) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("author: {}", author);
                ++fieldOccurances[ConfigFields::Author];
            }
            else if (jsonObjectKey == "queue folder")
            {
                std::string_view queueFolderFilepath;
                if (jsonObject.value().get_string().get(queueFolderFilepath) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("queue folder: {}", queueFolderFilepath);
                engineConfig.m_QueueFolderFilepath = queueFolderFilepath;
                ++fieldOccurances[ConfigFields::QueueFolder];
            }
            else if (jsonObjectKey == "workflows folder")
            {
                std::string_view workflowsFolder;
                if (jsonObject.value().get_string().get(workflowsFolder) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("workflows folder: {}", workflowsFolder);
                engineConfig.m_WorkflowsFolderFilepath = workflowsFolder;
                ++fieldOccurances[ConfigFields::WorkflowsFolder];
            }
            else if (jsonObjectKey == "max threads")
            {
                int64_t maxThreads = 0;
                if (jsonObject.value().get_int64().get(maxThreads) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("max threads: {}", maxThreads);
                if (maxThreads < 0)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' is negative ({}), ignoring; configChecker will assign default",
                                   jsonObjectKey, maxThreads);
                    continue;
                }
                engineConfig.m_MaxThreads = static_cast<uint32_t>(maxThreads);
                ++fieldOccurances[ConfigFields::MaxThreads];
            }
            else if (jsonObjectKey == "engine sleep time in run loop in ms")
            {
                int64_t sleepTime = 0;
                if (jsonObject.value().get_int64().get(sleepTime) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("engine sleep time in run loop in ms: {}", sleepTime);
                engineConfig.m_SleepDuration = std::chrono::milliseconds(sleepTime);
                ++fieldOccurances[ConfigFields::SleepTime];
            }
            else if (jsonObjectKey == "max file size in kB")
            {
                int64_t maxFileSizekB = 0;
                if (jsonObject.value().get_int64().get(maxFileSizekB) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("max file size in kB: {}", maxFileSizekB);
                if (maxFileSizekB < 0)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' is negative ({}), ignoring; configChecker will assign default",
                                   jsonObjectKey, maxFileSizekB);
                    continue;
                }
                engineConfig.m_MaxFileSizekB = static_cast<size_t>(maxFileSizekB);
                ++fieldOccurances[ConfigFields::MaxFileSizekB];
            }
            else if (jsonObjectKey == "verbose")
            {
                bool verbose = false;
                if (jsonObject.value().get_bool().get(verbose) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a boolean", jsonObjectKey);
                    continue;
                }
                engineConfig.m_Verbose = verbose;
                LOG_CORE_INFO("verbose: {}", verbose);
                ++fieldOccurances[ConfigFields::Verbose];
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
                int64_t apiIndex = 0;
                if (jsonObject.value().get_int64().get(apiIndex) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                if (apiIndex < 0)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' is negative ({}), ignoring", jsonObjectKey, apiIndex);
                    continue;
                }
                engineConfig.m_ApiIndex = static_cast<size_t>(apiIndex);
                LOG_CORE_INFO("API index: {}", engineConfig.m_ApiIndex);
                ++fieldOccurances[ConfigFields::ApiIndex];
            }
            else if (jsonObjectKey == "jcwf batch size")
            {
                int64_t batchSize = 0;
                if (jsonObject.value().get_int64().get(batchSize) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("jcwf batch size: {}", batchSize);
                if (batchSize > 0)
                {
                    engineConfig.m_JcwfBatchSize = static_cast<size_t>(batchSize);
                }
                ++fieldOccurances[ConfigFields::JcwfBatchSize];
            }
            else if (jsonObjectKey == "jcwf AI interface")
            {
                int64_t ifaceIndex = 0;
                if (jsonObject.value().get_int64().get(ifaceIndex) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("jcwf AI interface: {}", ifaceIndex);
                engineConfig.m_JcwfAiInterfaceIndex = static_cast<int>(ifaceIndex);
                ++fieldOccurances[ConfigFields::JcwfAiInterface];
            }
            else if (jsonObjectKey == "keys_file")
            {
                std::string_view keysFile;
                if (jsonObject.value().get_string().get(keysFile) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("keys_file: {}", keysFile);
                engineConfig.m_KeysFilePath = keysFile;
                ++fieldOccurances[ConfigFields::KeysFile];
            }
            else if (jsonObjectKey == "use_bash")
            {
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
                std::string_view tlsCert;
                if (jsonObject.value().get_string().get(tlsCert) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                engineConfig.m_TlsCert = std::string(tlsCert);
                LOG_CORE_INFO("TlsCert: {}", engineConfig.m_TlsCert);
                ++fieldOccurances[ConfigFields::TlsCert];
            }
            else if (jsonObjectKey == "TlsKey")
            {
                std::string_view tlsKey;
                if (jsonObject.value().get_string().get(tlsKey) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                engineConfig.m_TlsKey = std::string(tlsKey);
                LOG_CORE_INFO("TlsKey: {}", engineConfig.m_TlsKey);
                ++fieldOccurances[ConfigFields::TlsKey];
            }
            else if (jsonObjectKey == "TrustedProxyHeader")
            {
                std::string_view header;
                if (jsonObject.value().get_string().get(header) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                engineConfig.m_TrustedProxyHeader = std::string(header);
                LOG_CORE_INFO("TrustedProxyHeader: {}", engineConfig.m_TrustedProxyHeader);
                ++fieldOccurances[ConfigFields::TrustedProxyHeader];
            }
            else if (jsonObjectKey == "TrustedRoleHeader")
            {
                std::string_view header;
                if (jsonObject.value().get_string().get(header) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                engineConfig.m_TrustedRoleHeader = std::string(header);
                LOG_CORE_INFO("TrustedRoleHeader: {}", engineConfig.m_TrustedRoleHeader);
                ++fieldOccurances[ConfigFields::TrustedRoleHeader];
            }
            else if (jsonObjectKey == "MaxRequestBodyMB")
            {
                uint64_t maxBody = 0;
                if (jsonObject.value().get_uint64().get(maxBody) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a non-negative number", jsonObjectKey);
                    continue;
                }
                engineConfig.m_MaxRequestBodyMB = static_cast<size_t>(maxBody);
                LOG_CORE_INFO("MaxRequestBodyMB: {}", engineConfig.m_MaxRequestBodyMB);
                ++fieldOccurances[ConfigFields::MaxRequestBodyMB];
            }
            else if (jsonObjectKey == "max inflight ai calls")
            {
                int64_t maxInflight = 0;
                if (jsonObject.value().get_int64().get(maxInflight) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("max inflight ai calls: {}", maxInflight);
                if (maxInflight < 0)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' is negative ({}), ignoring; configChecker will assign default",
                                   jsonObjectKey, maxInflight);
                    continue;
                }
                engineConfig.m_MaxInflightAiCalls = static_cast<size_t>(maxInflight);
                ++fieldOccurances[ConfigFields::MaxInflightAiCalls];
            }
            else if (jsonObjectKey == "max_ai_calls_per_jcwf")
            {
                int64_t cap = 0;
                if (jsonObject.value().get_int64().get(cap) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                if (cap < 0) cap = 0;
                LOG_CORE_INFO("max_ai_calls_per_jcwf: {}", cap);
                engineConfig.m_MaxAiCallsPerJcwf = static_cast<size_t>(cap);
                ++fieldOccurances[ConfigFields::MaxAiCallsPerJcwf];
            }
            else if (jsonObjectKey == "max_per_item_fan_out")
            {
                int64_t cap = 0;
                if (jsonObject.value().get_int64().get(cap) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                if (cap < 0) cap = 0;
                LOG_CORE_INFO("max_per_item_fan_out: {}", cap);
                engineConfig.m_MaxPerItemFanOut = static_cast<size_t>(cap);
                ++fieldOccurances[ConfigFields::MaxPerItemFanOut];
            }
            else if (jsonObjectKey == "python engines")
            {
                int64_t count = 0;
                if (jsonObject.value().get_int64().get(count) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                LOG_CORE_INFO("python engines: {}", count);
                if (count < 0)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' is negative ({}), ignoring; configChecker will assign default",
                                   jsonObjectKey, count);
                    continue;
                }
                engineConfig.m_PythonEngines = static_cast<size_t>(count);
                ++fieldOccurances[ConfigFields::PythonEngines];
            }
            else if (jsonObjectKey == "port")
            {
                int64_t port = 0;
                if (jsonObject.value().get_int64().get(port) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                if (port < 0 || port > 65535)
                {
                    LOG_CORE_WARN("port {} out of range [0, 65535], defaulting to 0 (auto)", port);
                    port = 0;
                }
                LOG_CORE_INFO("port: {}", port);
                engineConfig.m_Port = static_cast<uint16_t>(port);
                ++fieldOccurances[ConfigFields::Port];
            }
            else if (jsonObjectKey == "mcp_keys_file")
            {
                std::string_view mcpKeysFile;
                if (jsonObject.value().get_string().get(mcpKeysFile) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a string", jsonObjectKey);
                    continue;
                }
                engineConfig.m_McpKeysFilePath = std::string(mcpKeysFile);
                LOG_CORE_INFO("mcp_keys_file: {}", engineConfig.m_McpKeysFilePath);
                ++fieldOccurances[ConfigFields::McpKeysFile];
            }
            else if (jsonObjectKey == "session_timeout_hours")
            {
                int64_t hours = 0;
                if (jsonObject.value().get_int64().get(hours) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("ConfigParser: '{}' must be a number", jsonObjectKey);
                    continue;
                }
                if (hours < 1 || hours > 168)
                {
                    LOG_CORE_WARN("session_timeout_hours {} out of range [1, 168], defaulting to 8", hours);
                    hours = 8;
                }
                LOG_CORE_INFO("session_timeout_hours: {}", hours);
                engineConfig.m_SessionTimeoutHours = static_cast<int>(hours);
                ++fieldOccurances[ConfigFields::SessionTimeoutHours];
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
                    std::string_view name;
                    if (field.value().get_string().get(name) != simdjson::SUCCESS)
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface '{}' must be a string", jsonObjectKey);
                        continue;
                    }
                    LOG_CORE_INFO("name: {}", name);
                    apiInterface.m_Name = name;
                    ++fieldOccurances[ConfigFields::InterfaceName];
                }
                else if (jsonObjectKey == "description")
                {
                    std::string_view description;
                    if (field.value().get_string().get(description) != simdjson::SUCCESS)
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface '{}' must be a string", jsonObjectKey);
                        continue;
                    }
                    LOG_CORE_INFO("description: {}", description);
                    apiInterface.m_Description = description;
                    ++fieldOccurances[ConfigFields::InterfaceDescription];
                }
                else if (jsonObjectKey == "key_name")
                {
                    std::string_view keyName;
                    if (field.value().get_string().get(keyName) != simdjson::SUCCESS)
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface '{}' must be a string", jsonObjectKey);
                        continue;
                    }
                    LOG_CORE_INFO("key_name: {}", keyName);
                    apiInterface.m_KeyName = keyName;
                    ++fieldOccurances[ConfigFields::InterfaceKeyName];
                }
                else if (jsonObjectKey == "url")
                {
                    std::string_view url;
                    if (field.value().get_string().get(url) != simdjson::SUCCESS)
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface '{}' must be a string", jsonObjectKey);
                        continue;
                    }
                    LOG_CORE_INFO("url: {}", url);
                    apiInterface.m_Url = url;
                    ++fieldOccurances[ConfigFields::Url];
                }
                else if (jsonObjectKey == "model")
                {
                    std::string_view model;
                    if (field.value().get_string().get(model) != simdjson::SUCCESS)
                    {
                        LOG_CORE_ERROR("ConfigParser: API interface '{}' must be a string", jsonObjectKey);
                        continue;
                    }
                    LOG_CORE_INFO("model: {}", model);
                    apiInterface.m_Model = model;
                    ++fieldOccurances[ConfigFields::Model];
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
                    apiInterface.m_InterfaceType = ParseInterfaceType(api);
                    if (apiInterface.m_InterfaceType == EngineConfig::InterfaceType::InvalidAPI)
                    {
                        LOG_CORE_ERROR("ConfigParser: unknown API '{}' in interface (expected API1-API6 or Test); "
                                       "interface will be marked InvalidAPI and skipped by configChecker",
                                       api);
                    }
                    ++fieldOccurances[ConfigFields::InterfaceType];
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
