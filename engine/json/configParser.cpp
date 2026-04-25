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
        padded_string json = padded_string::load(m_ConfigFilepathAndFilename);

        ondemand::document doc;
        auto error = parser.iterate(json).get(doc);

        if (error)
        {
            LOG_CORE_ERROR("ConfigParser::Parse: An error occurred during parsing: {}", error_message(error));
            m_State = ConfigParser::State::ParseFailure;
            return m_State;
        }

        ondemand::document sceneDocument = parser.iterate(json);
        ondemand::object jsonObjects = sceneDocument.get_object();

        FieldOccurances fieldOccurances{};
        for (auto jsonObject : jsonObjects)
        {
            std::string_view jsonObjectKey = jsonObject.unescaped_key();

            if (jsonObjectKey == "file format identifier")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                ++fieldOccurances[ConfigFields::Format];
            }
            else if (jsonObjectKey == "description")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view description = jsonObject.value().get_string();
                LOG_CORE_INFO("description: {}", description);
                ++fieldOccurances[ConfigFields::Description];
            }
            else if (jsonObjectKey == "author")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view author = jsonObject.value().get_string();
                LOG_CORE_INFO("author: {}", author);
                ++fieldOccurances[ConfigFields::Author];
            }
            else if (jsonObjectKey == "queue folder")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view queueFolderFilepath = jsonObject.value().get_string();
                LOG_CORE_INFO("queue folder: {}", queueFolderFilepath);
                engineConfig.m_QueueFolderFilepath = queueFolderFilepath;
                ++fieldOccurances[ConfigFields::QueueFolder];
            }
            else if (jsonObjectKey == "workflows folder")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view workflowsFolder = jsonObject.value().get_string();
                LOG_CORE_INFO("workflows folder: {}", workflowsFolder);
                engineConfig.m_WorkflowsFolderFilepath = workflowsFolder;
                ++fieldOccurances[ConfigFields::WorkflowsFolder];
            }
            else if (jsonObjectKey == "max threads")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto maxThreads = static_cast<int64_t>(jsonObject.value().get_int64());
                LOG_CORE_INFO("max threads: {}", maxThreads);
                engineConfig.m_MaxThreads = static_cast<uint32_t>(maxThreads);
                ++fieldOccurances[ConfigFields::MaxThreads];
            }
            else if (jsonObjectKey == "engine sleep time in run loop in ms")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto sleepTime = static_cast<int64_t>(jsonObject.value().get_int64());
                LOG_CORE_INFO("engine sleep time in run loop in ms: {}", sleepTime);
                engineConfig.m_SleepDuration = std::chrono::milliseconds(sleepTime);
                ++fieldOccurances[ConfigFields::SleepTime];
            }
            else if (jsonObjectKey == "max file size in kB")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto maxFileSizekB = static_cast<int64_t>(jsonObject.value().get_int64());
                LOG_CORE_INFO("max file size in kB: {}", maxFileSizekB);
                engineConfig.m_MaxFileSizekB = maxFileSizekB;
                ++fieldOccurances[ConfigFields::MaxFileSizekB];
            }
            else if (jsonObjectKey == "verbose")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::boolean), "type must be boolean");
                engineConfig.m_Verbose = jsonObject.value().get_bool();
                LOG_CORE_INFO("verbose: {}", engineConfig.m_Verbose);
                ++fieldOccurances[ConfigFields::Verbose];
            }
            else if (jsonObjectKey == "API interfaces")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::array), "type must be array");
                ParseInterfaces(jsonObject.value(), engineConfig, fieldOccurances);
            }
            else if (jsonObjectKey == "API index")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be a number");
                engineConfig.m_ApiIndex = jsonObject.value().get_int64();
                LOG_CORE_INFO("API index: {}", engineConfig.m_ApiIndex);
                ++fieldOccurances[ConfigFields::ApiIndex];
            }
            else if (jsonObjectKey == "jcwf batch size")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto batchSize = static_cast<int64_t>(jsonObject.value().get_int64());
                LOG_CORE_INFO("jcwf batch size: {}", batchSize);
                if (batchSize > 0)
                {
                    engineConfig.m_JcwfBatchSize = static_cast<size_t>(batchSize);
                }
                ++fieldOccurances[ConfigFields::JcwfBatchSize];
            }
            else if (jsonObjectKey == "jcwf AI interface")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto ifaceIndex = static_cast<int64_t>(jsonObject.value().get_int64());
                LOG_CORE_INFO("jcwf AI interface: {}", ifaceIndex);
                engineConfig.m_JcwfAiInterfaceIndex = static_cast<int>(ifaceIndex);
                ++fieldOccurances[ConfigFields::JcwfAiInterface];
            }
            else if (jsonObjectKey == "keys_file")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view keysFile = jsonObject.value().get_string();
                LOG_CORE_INFO("keys_file: {}", keysFile);
                engineConfig.m_KeysFilePath = keysFile;
                ++fieldOccurances[ConfigFields::KeysFile];
            }
            else if (jsonObjectKey == "use_bash")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::boolean), "type must be boolean");
                engineConfig.m_UseBashOnWindows = jsonObject.value().get_bool();
#if defined(_WIN32)
                LOG_CORE_INFO("use_bash: {}", engineConfig.m_UseBashOnWindows);
#else
                LOG_CORE_INFO("use_bash: {} (Windows-only, ignored on this platform)", engineConfig.m_UseBashOnWindows);
#endif
                ++fieldOccurances[ConfigFields::UseBashOnWindows];
            }
            else if (jsonObjectKey == "TlsCert")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                engineConfig.m_TlsCert = std::string(jsonObject.value().get_string().value());
                LOG_CORE_INFO("TlsCert: {}", engineConfig.m_TlsCert);
                ++fieldOccurances[ConfigFields::TlsCert];
            }
            else if (jsonObjectKey == "TlsKey")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                engineConfig.m_TlsKey = std::string(jsonObject.value().get_string().value());
                LOG_CORE_INFO("TlsKey: {}", engineConfig.m_TlsKey);
                ++fieldOccurances[ConfigFields::TlsKey];
            }
            else if (jsonObjectKey == "TrustedProxyHeader")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                engineConfig.m_TrustedProxyHeader = std::string(jsonObject.value().get_string().value());
                LOG_CORE_INFO("TrustedProxyHeader: {}", engineConfig.m_TrustedProxyHeader);
                ++fieldOccurances[ConfigFields::TrustedProxyHeader];
            }
            else if (jsonObjectKey == "TrustedRoleHeader")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                engineConfig.m_TrustedRoleHeader = std::string(jsonObject.value().get_string().value());
                LOG_CORE_INFO("TrustedRoleHeader: {}", engineConfig.m_TrustedRoleHeader);
                ++fieldOccurances[ConfigFields::TrustedRoleHeader];
            }
            else if (jsonObjectKey == "MaxRequestBodyMB")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                engineConfig.m_MaxRequestBodyMB = static_cast<size_t>(jsonObject.value().get_uint64().value());
                LOG_CORE_INFO("MaxRequestBodyMB: {}", engineConfig.m_MaxRequestBodyMB);
                ++fieldOccurances[ConfigFields::MaxRequestBodyMB];
            }
            else if (jsonObjectKey == "max inflight ai calls")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto maxInflight = static_cast<int64_t>(jsonObject.value().get_int64());
                LOG_CORE_INFO("max inflight ai calls: {}", maxInflight);
                engineConfig.m_MaxInflightAiCalls = static_cast<size_t>(maxInflight);
                ++fieldOccurances[ConfigFields::MaxInflightAiCalls];
            }
            else if (jsonObjectKey == "max_ai_calls_per_jcwf")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto cap = static_cast<int64_t>(jsonObject.value().get_int64());
                if (cap < 0) cap = 0;
                LOG_CORE_INFO("max_ai_calls_per_jcwf: {}", cap);
                engineConfig.m_MaxAiCallsPerJcwf = static_cast<size_t>(cap);
                ++fieldOccurances[ConfigFields::MaxAiCallsPerJcwf];
            }
            else if (jsonObjectKey == "python engines")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto count = static_cast<int64_t>(jsonObject.value().get_int64());
                LOG_CORE_INFO("python engines: {}", count);
                engineConfig.m_PythonEngines = static_cast<size_t>(count);
                ++fieldOccurances[ConfigFields::PythonEngines];
            }
            else if (jsonObjectKey == "port")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto port = static_cast<int64_t>(jsonObject.value().get_int64());
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
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                engineConfig.m_McpKeysFilePath = std::string(jsonObject.value().get_string().value());
                LOG_CORE_INFO("mcp_keys_file: {}", engineConfig.m_McpKeysFilePath);
                ++fieldOccurances[ConfigFields::McpKeysFile];
            }
            else if (jsonObjectKey == "session_timeout_hours")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be number");
                auto hours = static_cast<int64_t>(jsonObject.value().get_int64());
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
                    CORE_ASSERT((field.value().type() == ondemand::json_type::string), "type must be string");
                    std::string_view name = field.value().get_string();
                    LOG_CORE_INFO("name: {}", name);
                    apiInterface.m_Name = name;
                    ++fieldOccurances[ConfigFields::InterfaceName];
                }
                else if (jsonObjectKey == "description")
                {
                    CORE_ASSERT((field.value().type() == ondemand::json_type::string), "type must be string");
                    std::string_view description = field.value().get_string();
                    LOG_CORE_INFO("description: {}", description);
                    apiInterface.m_Description = description;
                    ++fieldOccurances[ConfigFields::InterfaceDescription];
                }
                else if (jsonObjectKey == "key_name")
                {
                    CORE_ASSERT((field.value().type() == ondemand::json_type::string), "type must be string");
                    std::string_view keyName = field.value().get_string();
                    LOG_CORE_INFO("key_name: {}", keyName);
                    apiInterface.m_KeyName = keyName;
                    ++fieldOccurances[ConfigFields::InterfaceKeyName];
                }
                else if (jsonObjectKey == "url")
                {
                    CORE_ASSERT((field.value().type() == ondemand::json_type::string), "type must be string");
                    std::string_view url = field.value().get_string();
                    LOG_CORE_INFO("url: {}", url);
                    apiInterface.m_Url = url;
                    ++fieldOccurances[ConfigFields::Url];
                }
                else if (jsonObjectKey == "model")
                {
                    CORE_ASSERT((field.value().type() == ondemand::json_type::string), "type must be string");
                    std::string_view model = field.value().get_string();
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
                else if (jsonObjectKey == "API")
                {
                    CORE_ASSERT((field.value().type() == ondemand::json_type::string), "type must be string");
                    std::string_view api = field.value().get_string();
                    LOG_CORE_INFO("API: {}", api);
                    if (api == "API1")
                    {
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::API1;
                    }
                    else if (api == "API2")
                    {
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::API2;
                    }
                    else if (api == "API3")
                    {
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::API3;
                    }
                    else if (api == "API4")
                    {
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::API4;
                    }
                    else if (api == "Test")
                    {
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::Test;
                    }
                    else if (api == "API5")
                    {
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::API5;
                    }
                    else if (api == "API6")
                    {
                        apiInterface.m_InterfaceType = EngineConfig::InterfaceType::API6;
                    }
                    else
                    {
                        CORE_HARD_STOP("invalid API in config.json");
                    }
                    ++fieldOccurances[ConfigFields::InterfaceType];
                }
            }

            // Auto-generate name from URL domain + model + API type if not provided
            if (apiInterface.m_Name.empty())
            {
                std::string apiTypeStr;
                switch (apiInterface.m_InterfaceType)
                {
                    case EngineConfig::InterfaceType::API1:
                        apiTypeStr = "API1";
                        break;
                    case EngineConfig::InterfaceType::API2:
                        apiTypeStr = "API2";
                        break;
                    case EngineConfig::InterfaceType::API3:
                        apiTypeStr = "API3";
                        break;
                    case EngineConfig::InterfaceType::API4:
                        apiTypeStr = "API4";
                        break;
                    case EngineConfig::InterfaceType::Test:
                        apiTypeStr = "Test";
                        break;
                    case EngineConfig::InterfaceType::API5:
                        apiTypeStr = "API5";
                        break;
                    case EngineConfig::InterfaceType::API6:
                        apiTypeStr = "API6";
                        break;
                    default:
                        break;
                }
                apiInterface.m_Name =
                    EngineConfig::GenerateInterfaceName(apiInterface.m_Url, apiInterface.m_Model, apiTypeStr);
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
