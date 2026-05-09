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

#include <cstdint>
#include <iterator>
#include <limits>

#include "core.h"
#include "engine.h"
#include "json/replyParserAPI1.h"
#include "json/jsonObjectParser.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    namespace
    {
        // Cap on the number of choices materialised from a single provider
        // reply.  In practice n=1 is what we ever request; this guards the
        // parsing thread against a hostile or MITM response inflating the
        // array to exhaust heap memory.
        constexpr std::size_t kMaxChoices = 64;

        // Cap applied to externally-sourced strings before they reach the
        // structured log.  Keeps log lines bounded and prevents log/dashboard
        // poisoning through megabyte-scale field values.
        constexpr std::size_t kLogStringCap = 256;

        // Tighter cap for short identifier-like fields (id, type, code, model,
        // role, finish_reason).  These should never carry free-form content;
        // if a provider sends one that exceeds this, log the truncated form.
        constexpr std::size_t kLogIdCap = 128;
    } // namespace

    ReplyParserAPI1::ReplyParserAPI1(std::string const& jsonString) : ReplyParser(jsonString) { Parse(); }

    ReplyParserAPI1::ErrorInfo const& ReplyParserAPI1::GetErrorInfo() const { return m_ErrorInfo; }

    ReplyParserAPI1::ErrorType ReplyParserAPI1::GetErrorType() const { return m_ErrorType; }

    size_t ReplyParserAPI1::HasContent() const { return m_Reply.m_Choices.size(); }

    std::string ReplyParserAPI1::GetContent(size_t index) const
    {
        if (index < m_Reply.m_Choices.size())
        {
            return m_Reply.m_Choices[index].m_Message.m_Content;
        }
        LOG_APP_ERROR("ReplyParserAPI1::GetContent: index out of range, index: {}", index);
        return {};
    }

    AiError ReplyParserAPI1::GetError() const
    {
        AiError error;
        if (!m_HasError)
        {
            return error;
        }
        error.m_Kind = AiError::Kind::Provider;
        error.m_Message = m_ErrorInfo.m_Message;

        switch (m_ErrorType)
        {
            case ErrorType::Unknown:             error.m_HttpStatus = 0;   break;
            case ErrorType::InvalidRequestError: error.m_HttpStatus = 400; break;
            case ErrorType::AuthenticationError: error.m_HttpStatus = 401; break;
            case ErrorType::PermissionError:     error.m_HttpStatus = 403; break;
            case ErrorType::RateLimitError:      error.m_HttpStatus = 429; break;
            case ErrorType::ServerError:         error.m_HttpStatus = 500; break;
            case ErrorType::InsufficientQuota:   error.m_HttpStatus = 429; break;
        }
        return error;
    }

    AiUsage ReplyParserAPI1::GetUsage() const
    {
        constexpr uint64_t kCap = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());

        auto clamp = [](uint64_t value, char const* fieldName) -> int32_t
        {
            if (value > kCap)
            {
                LOG_APP_WARN("ReplyParserAPI1::GetUsage: {} exceeds INT32_MAX ({}), clamping",
                             fieldName, value);
                return std::numeric_limits<int32_t>::max();
            }
            return static_cast<int32_t>(value);
        };

        AiUsage usage;
        usage.m_InputTokens  = clamp(m_Reply.m_Usage.m_PromptTokens,     "prompt_tokens");
        usage.m_OutputTokens = clamp(m_Reply.m_Usage.m_CompletionTokens, "completion_tokens");
        usage.m_TotalTokens  = clamp(m_Reply.m_Usage.m_TotalTokens,      "total_tokens");
        return usage;
    }

    std::string ReplyParserAPI1::GetFinishReason() const
    {
        if (!m_Reply.m_Choices.empty())
        {
            return m_Reply.m_Choices.front().m_FinishReason;
        }
        return {};
    }

    std::string ReplyParserAPI1::GetSystemFingerprint() const { return {}; }

    void ReplyParserAPI1::Parse()
    {
        using namespace simdjson;
        ondemand::parser parser;

        // padded_string is a local; every string_view extracted below points
        // into its backing buffer and MUST be copied to std::string before
        // this function returns.  Do not call ParseContent / ParseUsage /
        // ParseError asynchronously, store views as members, or move json
        // elsewhere — the borrowed views dangle the moment json is destroyed.
        padded_string json = padded_string(m_JsonString.data(), m_JsonString.size());

        ondemand::document responseDocument;
        if (auto err = parser.iterate(json).get(responseDocument); err)
        {
            LOG_APP_ERROR("ReplyParserAPI1::Parse: iterate failed: {}", error_message(err));
            m_State = ReplyParser::State::ParseFailure;
            return;
        }

        ondemand::object jsonObjects;
        if (auto err = responseDocument.get_object().get(jsonObjects); err)
        {
            LOG_APP_ERROR("ReplyParserAPI1::Parse: top-level get_object failed: {}", error_message(err));
            m_State = ReplyParser::State::ParseFailure;
            return;
        }

        Reply reply{};

        for (auto jsonObject : jsonObjects)
        {
            std::string_view key;
            if (auto err = jsonObject.unescaped_key().get(key); err)
            {
                LOG_APP_ERROR("ReplyParserAPI1::Parse: unescaped_key failed: {}", error_message(err));
                continue;
            }

            ondemand::value val;
            if (auto err = jsonObject.value().get(val); err)
            {
                LOG_APP_ERROR("ReplyParserAPI1::Parse: value() for key '{}' failed: {}",
                              key, error_message(err));
                continue;
            }

            if (key == "id")
            {
                std::string_view id;
                if (auto err = val.get_string().get(id); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'id' not a string: {}", error_message(err));
                    continue;
                }
                reply.m_Id = std::string(id);
                LOG_APP_TRACE("ReplyParserAPI1::Parse: id='{}'",
                              TruncateUtf8Safe(SanitizeUtf8(reply.m_Id), kLogIdCap));
            }
            else if (key == "object")
            {
                std::string_view object;
                if (auto err = val.get_string().get(object); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'object' not a string: {}", error_message(err));
                    continue;
                }
                reply.m_Object = std::string(object);
                LOG_APP_TRACE("ReplyParserAPI1::Parse: object='{}'",
                              TruncateUtf8Safe(SanitizeUtf8(reply.m_Object), kLogIdCap));
            }
            else if (key == "created")
            {
                uint64_t created = 0;
                if (auto err = val.get_uint64().get(created); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'created' not a number: {}", error_message(err));
                    continue;
                }
                reply.m_Created = created;
                LOG_APP_TRACE("ReplyParserAPI1::Parse: created={}", created);
            }
            else if (key == "model")
            {
                std::string_view model;
                if (auto err = val.get_string().get(model); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'model' not a string: {}", error_message(err));
                    continue;
                }
                reply.m_Model = std::string(model);
                LOG_APP_TRACE("ReplyParserAPI1::Parse: model='{}'",
                              TruncateUtf8Safe(SanitizeUtf8(reply.m_Model), kLogIdCap));
            }
            else if (key == "choices")
            {
                ondemand::array choicesArr;
                if (auto err = val.get_array().get(choicesArr); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'choices' not an array: {}", error_message(err));
                    m_State = ReplyParser::State::ParseFailure;
                    continue;
                }
                ParseContent(choicesArr, reply);
                m_State = ReplyParser::State::ReplyOk;
            }
            else if (key == "usage")
            {
                ondemand::object usageObj;
                if (auto err = val.get_object().get(usageObj); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'usage' not an object: {}", error_message(err));
                    continue;
                }
                ParseUsage(usageObj, reply);
            }
            else if (key == "error")
            {
                ondemand::json_type t;
                if (auto err = val.type().get(t); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'error' type() failed: {}", error_message(err));
                    continue;
                }
                if (t == ondemand::json_type::null)
                {
                    continue;
                }
                ondemand::object errorObj;
                if (auto err = val.get_object().get(errorObj); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'error' not an object: {}", error_message(err));
                    m_State = ReplyParser::State::ParseFailure;
                    continue;
                }
                ParseError(errorObj);
                m_HasError = true;
                m_State = ReplyParser::State::ReplyError;

                // One consolidated log line at the right severity, with both
                // type and message bounded.  Recoverable errors (rate-limit,
                // quota) are WARN; the rest are ERROR.
                std::string const truncatedType =
                    TruncateUtf8Safe(SanitizeUtf8(m_ErrorInfo.m_Type), kLogIdCap);
                std::string const truncatedMsg =
                    TruncateUtf8Safe(SanitizeUtf8(m_ErrorInfo.m_Message), kLogStringCap);
                if (m_ErrorType == ErrorType::RateLimitError ||
                    m_ErrorType == ErrorType::InsufficientQuota)
                {
                    LOG_APP_WARN("ReplyParserAPI1::Parse: provider returned recoverable error "
                                 "type='{}' message='{}'",
                                 truncatedType, truncatedMsg);
                }
                else
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: provider returned error "
                                  "type='{}' message='{}'",
                                  truncatedType, truncatedMsg);
                }
            }
            else if (key == "requestId")
            {
                std::string_view str;
                if (auto err = val.get_string().get(str); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'requestId' not a string: {}",
                                  error_message(err));
                    continue;
                }
                LOG_APP_TRACE("ReplyParserAPI1::Parse: requestId='{}'",
                              TruncateUtf8Safe(SanitizeUtf8(std::string(str)), kLogIdCap));
            }
            else if (key == "statusCode")
            {
                int64_t num = 0;
                if (auto err = val.get_int64().get(num); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'statusCode' not an integer: {}",
                                  error_message(err));
                    continue;
                }
                LOG_APP_TRACE("ReplyParserAPI1::Parse: statusCode={}", num);
            }
            else if (key == "timestamp")
            {
                std::string_view str;
                if (auto err = val.get_string().get(str); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: 'timestamp' not a string: {}",
                                  error_message(err));
                    continue;
                }
                LOG_APP_TRACE("ReplyParserAPI1::Parse: timestamp='{}'",
                              TruncateUtf8Safe(SanitizeUtf8(std::string(str)), kLogIdCap));
            }
            else if (key == "message")
            {
                // top-level "message" is the provider's free-form text and may
                // contain user-influenced fragments — log only its length.
                std::string_view message;
                if (auto err = val.get_string().get(message); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::Parse: top-level 'message' not a string: {}",
                                  error_message(err));
                    continue;
                }
                LOG_APP_TRACE("ReplyParserAPI1::Parse: top-level message bytes={}", message.size());
            }
            else
            {
                JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in main reply");
            }
        }

        if (!m_HasError)
        {
            m_Reply = std::move(reply);
        }
        else
        {
            // Discarding the reply payload on a provider error is the expected
            // behaviour, not a critical event — the consolidated ERROR/WARN log
            // above already surfaced the type+message at the right severity.
            LOG_APP_TRACE("ReplyParserAPI1::Parse: reply payload discarded (provider error)");
        }
    }

    void ReplyParserAPI1::ParseContent(simdjson::ondemand::array jsonArray, Reply& reply)
    {
        using namespace simdjson;

        for (auto element : jsonArray)
        {
            if (reply.m_Choices.size() >= kMaxChoices)
            {
                LOG_APP_ERROR("ReplyParserAPI1::ParseContent: choices count exceeds cap ({}); "
                              "remaining elements ignored",
                              kMaxChoices);
                break;
            }

            ondemand::object choiceObj;
            if (auto err = element.get_object().get(choiceObj); err)
            {
                LOG_APP_ERROR("ReplyParserAPI1::ParseContent: choice element not an object: {}",
                              error_message(err));
                continue;
            }

            Reply::Choice choice{};

            for (auto field : choiceObj)
            {
                std::string_view key;
                if (auto err = field.unescaped_key().get(key); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseContent: choice unescaped_key failed: {}",
                                  error_message(err));
                    continue;
                }

                ondemand::value val;
                if (auto err = field.value().get(val); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseContent: value() for key '{}' failed: {}",
                                  key, error_message(err));
                    continue;
                }

                if (key == "index")
                {
                    uint64_t index = 0;
                    if (auto err = val.get_uint64().get(index); err)
                    {
                        LOG_APP_ERROR("ReplyParserAPI1::ParseContent: 'index' not an integer: {}",
                                      error_message(err));
                        continue;
                    }
                    choice.m_Index = index;
                    LOG_APP_TRACE("ReplyParserAPI1::ParseContent: index={}", index);
                }
                else if (key == "message")
                {
                    ondemand::object messageObj;
                    if (auto err = val.get_object().get(messageObj); err)
                    {
                        LOG_APP_ERROR("ReplyParserAPI1::ParseContent: 'message' not an object: {}",
                                      error_message(err));
                        continue;
                    }

                    for (auto messageField : messageObj)
                    {
                        std::string_view msgKey;
                        if (auto err = messageField.unescaped_key().get(msgKey); err)
                        {
                            LOG_APP_ERROR("ReplyParserAPI1::ParseContent: message unescaped_key failed: {}",
                                          error_message(err));
                            continue;
                        }

                        ondemand::value msgVal;
                        if (auto err = messageField.value().get(msgVal); err)
                        {
                            LOG_APP_ERROR("ReplyParserAPI1::ParseContent: message value() for '{}' failed: {}",
                                          msgKey, error_message(err));
                            continue;
                        }

                        if (msgKey == "role")
                        {
                            std::string_view role;
                            if (auto err = msgVal.get_string().get(role); err)
                            {
                                LOG_APP_ERROR("ReplyParserAPI1::ParseContent: 'role' not a string: {}",
                                              error_message(err));
                                continue;
                            }
                            choice.m_Message.m_Role = std::string(role);
                            LOG_APP_TRACE("ReplyParserAPI1::ParseContent: role='{}'",
                                          TruncateUtf8Safe(SanitizeUtf8(choice.m_Message.m_Role), kLogIdCap));
                        }
                        else if (msgKey == "content")
                        {
                            std::string_view content;
                            if (auto err = msgVal.get_string().get(content); err)
                            {
                                LOG_APP_ERROR("ReplyParserAPI1::ParseContent: 'content' not a string: {}",
                                              error_message(err));
                                continue;
                            }
                            choice.m_Message.m_Content = SanitizeUtf8(std::string(content));
                            // content may carry user PII or prompt-injection
                            // payloads; log only its byte length, never the value.
                            LOG_APP_TRACE("ReplyParserAPI1::ParseContent: content bytes={}",
                                          choice.m_Message.m_Content.size());
                        }
                        else
                        {
                            JsonObjectParser jsonObjectParser(msgKey, msgVal,
                                                              "Uncaught json field in message object");
                        }
                    }
                }
                else if (key == "finish_reason")
                {
                    std::string_view reason;
                    if (auto err = val.get_string().get(reason); err)
                    {
                        LOG_APP_ERROR("ReplyParserAPI1::ParseContent: 'finish_reason' not a string: {}",
                                      error_message(err));
                        continue;
                    }
                    choice.m_FinishReason = std::string(reason);
                    LOG_APP_TRACE("ReplyParserAPI1::ParseContent: finish_reason='{}'",
                                  TruncateUtf8Safe(SanitizeUtf8(choice.m_FinishReason), kLogIdCap));
                }
                else
                {
                    JsonObjectParser jsonObjectParser(key, val, "uncaught json field in choice object");
                }
            }

            reply.m_Choices.push_back(std::move(choice));
        }
    }

    void ReplyParserAPI1::ParseUsage(simdjson::ondemand::object jsonObjects, Reply& reply)
    {
        using namespace simdjson;

        for (auto jsonObject : jsonObjects)
        {
            std::string_view key;
            if (auto err = jsonObject.unescaped_key().get(key); err)
            {
                LOG_APP_ERROR("ReplyParserAPI1::ParseUsage: unescaped_key failed: {}", error_message(err));
                continue;
            }

            ondemand::value val;
            if (auto err = jsonObject.value().get(val); err)
            {
                LOG_APP_ERROR("ReplyParserAPI1::ParseUsage: value() for key '{}' failed: {}",
                              key, error_message(err));
                continue;
            }

            if (key == "prompt_tokens")
            {
                uint64_t tokens = 0;
                if (auto err = val.get_uint64().get(tokens); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseUsage: 'prompt_tokens' not a number: {}",
                                  error_message(err));
                    continue;
                }
                reply.m_Usage.m_PromptTokens = tokens;
                LOG_APP_TRACE("ReplyParserAPI1::ParseUsage: prompt_tokens={}", tokens);
            }
            else if (key == "completion_tokens")
            {
                uint64_t tokens = 0;
                if (auto err = val.get_uint64().get(tokens); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseUsage: 'completion_tokens' not a number: {}",
                                  error_message(err));
                    continue;
                }
                reply.m_Usage.m_CompletionTokens = tokens;
                LOG_APP_TRACE("ReplyParserAPI1::ParseUsage: completion_tokens={}", tokens);
            }
            else if (key == "total_tokens")
            {
                uint64_t tokens = 0;
                if (auto err = val.get_uint64().get(tokens); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseUsage: 'total_tokens' not a number: {}",
                                  error_message(err));
                    continue;
                }
                reply.m_Usage.m_TotalTokens = tokens;
                LOG_APP_TRACE("ReplyParserAPI1::ParseUsage: total_tokens={}", tokens);
            }
            else
            {
                JsonObjectParser jsonObjectParser(key, val,
                                                  "uncaught json field in server error reply (usage field)");
            }
        }
    }

    void ReplyParserAPI1::ParseError(simdjson::ondemand::object jsonObjects)
    {
        using namespace simdjson;
        ReplyParserAPI1::ErrorInfo errorInfo;

        for (auto jsonObject : jsonObjects)
        {
            std::string_view key;
            if (auto err = jsonObject.unescaped_key().get(key); err)
            {
                LOG_APP_ERROR("ReplyParserAPI1::ParseError: unescaped_key failed: {}", error_message(err));
                continue;
            }

            ondemand::value val;
            if (auto err = jsonObject.value().get(val); err)
            {
                LOG_APP_ERROR("ReplyParserAPI1::ParseError: value() for key '{}' failed: {}",
                              key, error_message(err));
                continue;
            }

            if (key == "message")
            {
                std::string_view message;
                if (auto err = val.get_string().get(message); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseError: 'message' not a string: {}",
                                  error_message(err));
                    continue;
                }
                // Provider error message may echo back fragments of the request
                // or an API-key suffix.  Sanitize but defer logging — the
                // caller emits a single consolidated, length-capped log line.
                errorInfo.m_Message = SanitizeUtf8(std::string(message));
            }
            else if (key == "type")
            {
                std::string_view type;
                if (auto err = val.get_string().get(type); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseError: 'type' not a string: {}",
                                  error_message(err));
                    continue;
                }
                errorInfo.m_Type = std::string(type);
            }
            else if (key == "code")
            {
                std::string_view code;
                if (auto err = val.get_string().get(code); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseError: 'code' not a string: {}",
                                  error_message(err));
                    continue;
                }
                errorInfo.m_Code = std::string(code);
            }
            else if (key == "param")
            {
                ondemand::json_type t;
                if (auto err = val.type().get(t); err)
                {
                    LOG_APP_ERROR("ReplyParserAPI1::ParseError: 'param' type() failed: {}",
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
                    LOG_APP_ERROR("ReplyParserAPI1::ParseError: 'param' not a string: {}",
                                  error_message(err));
                    continue;
                }
                errorInfo.m_Param = std::string(param);
            }
            else
            {
                JsonObjectParser jsonObjectParser(key, val,
                                                  "uncaught json field in server error reply");
            }
        }
        m_ErrorInfo = errorInfo;
        m_ErrorType = ParseErrorType(m_ErrorInfo.m_Type);
    }

    ReplyParserAPI1::ErrorType ReplyParserAPI1::ParseErrorType(std::string_view type)
    {
        struct Entry
        {
            std::string_view m_Name;
            ErrorType m_Type;
        };
        static constexpr Entry kErrorTypes[] = {
            {"invalid_request_error", ErrorType::InvalidRequestError},
            {"authentication_error",  ErrorType::AuthenticationError},
            {"permission_error",      ErrorType::PermissionError},
            {"rate_limit_error",      ErrorType::RateLimitError},
            {"server_error",          ErrorType::ServerError},
            {"insufficient_quota",    ErrorType::InsufficientQuota},
        };
        // Lock the table to the enum: changing either side without updating
        // the other becomes a compile error.  Adding a new variant or renaming
        // InsufficientQuota's underlying value will break this assertion and
        // force a deliberate update.
        static_assert(std::size(kErrorTypes) == 6 &&
                          static_cast<int>(ErrorType::InsufficientQuota) == 6,
                      "ErrorType variants changed — extend kErrorTypes (and bump these constants) to match");

        for (auto const& entry : kErrorTypes)
        {
            if (type == entry.m_Name)
                return entry.m_Type;
        }
        return ErrorType::Unknown;
    }
} // namespace AIAssistant
