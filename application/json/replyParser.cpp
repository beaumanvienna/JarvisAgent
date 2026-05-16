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

#include "engine.h"
#include "json/jsonObjectParser.h"
#include "json/replyParser.h"
#include "json/replyParserAPI1.h"
#include "json/replyParserAPI2.h"
#include "json/replyParserAPI3.h"
#include "json/replyParserAPI4.h"
#include "json/replyParserAPI5.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    OpenAiStyleErrorInfo ParseOpenAiStyleError(simdjson::ondemand::object errorObj)
    {
        using namespace simdjson;
        OpenAiStyleErrorInfo info;

        for (auto field : errorObj)
        {
            std::string_view key;
            if (auto err = field.unescaped_key().get(key); err)
            {
                LOG_APP_ERROR("ParseOpenAiStyleError: unescaped_key failed: {}", error_message(err));
                continue;
            }

            ondemand::value val;
            if (auto err = field.value().get(val); err)
            {
                LOG_APP_ERROR("ParseOpenAiStyleError: value() for key '{}' failed: {}",
                              key, error_message(err));
                continue;
            }

            if (key == "message")
            {
                std::string_view message;
                if (auto err = val.get_string().get(message); err)
                {
                    LOG_APP_ERROR("ParseOpenAiStyleError: 'message' not a string: {}",
                                  error_message(err));
                    continue;
                }
                // Provider message may echo request fragments / key suffixes;
                // sanitize at the boundary, defer length-capped logging to the
                // caller which has runId in scope.
                info.m_Message = SanitizeUtf8(std::string(message));
            }
            else if (key == "type")
            {
                std::string_view type;
                if (auto err = val.get_string().get(type); err)
                {
                    LOG_APP_ERROR("ParseOpenAiStyleError: 'type' not a string: {}",
                                  error_message(err));
                    continue;
                }
                info.m_Type = std::string(type);
            }
            else if (key == "code")
            {
                ondemand::json_type t;
                if (auto err = val.type().get(t); err)
                {
                    LOG_APP_ERROR("ParseOpenAiStyleError: 'code' type() failed: {}",
                                  error_message(err));
                    continue;
                }
                if (t == ondemand::json_type::null)
                {
                    continue;
                }
                // OpenAI returns code as string ("insufficient_quota"); some
                // edge cases (Azure, older error envelopes) emit it as a
                // number — accept both rather than dropping the discriminator.
                if (t == ondemand::json_type::string)
                {
                    std::string_view code;
                    if (auto err = val.get_string().get(code); err)
                    {
                        LOG_APP_ERROR("ParseOpenAiStyleError: 'code' not a string: {}",
                                      error_message(err));
                        continue;
                    }
                    info.m_Code = std::string(code);
                }
                else if (t == ondemand::json_type::number)
                {
                    int64_t numericCode = 0;
                    if (auto err = val.get_int64().get(numericCode); err)
                    {
                        LOG_APP_ERROR("ParseOpenAiStyleError: 'code' not an int: {}",
                                      error_message(err));
                        continue;
                    }
                    info.m_Code = std::to_string(numericCode);
                }
            }
            else if (key == "param")
            {
                ondemand::json_type t;
                if (auto err = val.type().get(t); err)
                {
                    LOG_APP_ERROR("ParseOpenAiStyleError: 'param' type() failed: {}",
                                  error_message(err));
                    continue;
                }
                if (t == ondemand::json_type::null)
                {
                    continue;
                }
                std::string_view param;
                if (auto err = val.get_string().get(param); err)
                {
                    LOG_APP_ERROR("ParseOpenAiStyleError: 'param' not a string: {}",
                                  error_message(err));
                    continue;
                }
                info.m_Param = std::string(param);
            }
            else
            {
                JsonObjectParser jsonObjectParser(key, val,
                                                  "uncaught json field in OpenAI-style error envelope");
            }
        }
        return info;
    }

    ProviderErrorCategory ClassifyOpenAiStyleErrorType(std::string_view type)
    {
        // Discriminators per OpenAI + Azure OpenAI error reference.  Maps the
        // raw provider string to the UI-facing semantic category.  Unknown
        // returns Unknown — the raw type still propagates via
        // AiError::m_ProviderErrorType for logs/debug.
        if (type == "insufficient_quota")    return ProviderErrorCategory::BillingExhausted;
        if (type == "rate_limit_error")      return ProviderErrorCategory::ThrottleRateLimit;
        if (type == "authentication_error")  return ProviderErrorCategory::AuthFailure;
        if (type == "permission_error")      return ProviderErrorCategory::AuthFailure;
        if (type == "model_not_found")       return ProviderErrorCategory::ModelNotFound;
        if (type == "server_error")          return ProviderErrorCategory::ServiceOverload;
        if (type == "invalid_request_error") return ProviderErrorCategory::InvalidRequest;
        return ProviderErrorCategory::Unknown;
    }

    ReplyParser::ReplyParser(std::string const& jsonString) : m_JsonString(jsonString) {}

    bool ReplyParser::HasError() const { return m_HasError; }

    AiError ReplyParser::GetError() const
    {
        AiError error;
        if (m_HasError)
        {
            error.m_Kind = AiError::Kind::Provider;
            error.m_Message = "reply parser reported error";
        }
        return error;
    }

    AiUsage ReplyParser::GetUsage() const { return AiUsage{}; }

    std::string ReplyParser::GetFinishReason() const { return {}; }

    std::string ReplyParser::GetSystemFingerprint() const { return {}; }

    std::optional<std::string> ReplyParser::GetStructuredOutput() const { return std::nullopt; }

    std::unique_ptr<ReplyParser> ReplyParser::Create(ConfigParser::EngineConfig::InterfaceType const& interfaceType,
                                                     std::string const& jsonString)
    {
        std::unique_ptr<ReplyParser> replyParser;

        switch (interfaceType)
        {
            case ConfigParser::EngineConfig::InterfaceType::API1:
            {
                replyParser = std::make_unique<ReplyParserAPI1>(jsonString);
                break;
            }
            case ConfigParser::EngineConfig::InterfaceType::API2:
            {
                replyParser = std::make_unique<ReplyParserAPI2>(jsonString);
                break;
            }
            case ConfigParser::EngineConfig::InterfaceType::API3:
            {
                replyParser = std::make_unique<ReplyParserAPI3>(jsonString);
                break;
            }
            case ConfigParser::EngineConfig::InterfaceType::API4:
            {
                replyParser = std::make_unique<ReplyParserAPI4>(jsonString);
                break;
            }
            case ConfigParser::EngineConfig::InterfaceType::API5:
            {
                replyParser = std::make_unique<ReplyParserAPI5>(jsonString);
                break;
            }
            case ConfigParser::EngineConfig::InterfaceType::API6:
            {
                // Azure OpenAI returns OpenAI-compatible bodies — reuse the API1 parser.
                replyParser = std::make_unique<ReplyParserAPI1>(jsonString);
                break;
            }
            default:
            {
                LOG_APP_CRITICAL("ReplyParser::Create: api not supported");
                break;
            }
        };
        return replyParser;
    }

} // namespace AIAssistant
