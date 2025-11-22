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

#include "core.h"
#include "engine.h"
#include "json/replyParserAPI1.h"
#include "json/jsonObjectParser.h"

namespace AIAssistant
{
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

    void ReplyParserAPI1::Parse()
    {
        using namespace simdjson;
        ondemand::parser parser;

        padded_string json = padded_string(m_JsonString.data(), m_JsonString.size());

        ondemand::document doc;
        auto error = parser.iterate(json).get(doc);

        if (error)
        {
            LOG_APP_ERROR("ReplyParserAPI1::Parse: An error occurred during parsing: {}", error_message(error));
            m_State = ReplyParser::State::ParseFailure;
            return;
        }

        ondemand::document sceneDocument = parser.iterate(json);
        ondemand::object jsonObjects = sceneDocument.get_object();

        Reply reply{};

        for (auto jsonObject : jsonObjects)
        {
            std::string_view key = jsonObject.unescaped_key();

            if (key == "id")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "id must be string");
                std::string_view id = jsonObject.value().get_string();
                LOG_APP_INFO("id: {}", id);
                m_Reply.m_Id = id;
            }
            else if (key == "object")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "object must be string");
                std::string_view object = jsonObject.value().get_string();
                LOG_APP_INFO("object: {}", object);
                m_Reply.m_Object = object;
            }
            else if (key == "created")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "created must be integer");
                uint64_t created = jsonObject.value().get_uint64();
                LOG_APP_INFO("created: {}", created);
                m_Reply.m_Created = created;
            }
            else if (key == "model")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "model must be string");
                std::string_view model = jsonObject.value().get_string();
                LOG_APP_INFO("model: {}", model);
                m_Reply.m_Model = model;
            }
            else if (key == "choices")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::array), "type must be array");
                LOG_APP_INFO("parsing content: ");
                ParseContent(jsonObject.value(), reply);
                m_State = ReplyParser::State::ReplyOk;
            }
            else if (key == "usage")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::object), "type must be object");
                ParseUsage(jsonObject.value());
            }
            else if (key == "error")
            {
                if (jsonObject.value().type() != ondemand::json_type::null)
                {
                    CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::object), "type must be object");
                    LOG_APP_ERROR("error: ");
                    ParseError(jsonObject.value());
                    m_HasError = true;
                    m_State = ReplyParser::State::ReplyError;
                }
            }
            else if (key == "requestId")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "requestID must be string");
                std::string_view str = jsonObject.value().get_string();
                LOG_APP_INFO("Request ID: {}", str);
            }
            else if (key == "statusCode")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "status code must be a number");
                int64_t num = jsonObject.value().get_int64();
                LOG_APP_INFO("Status code: {}", num);
            }
            else if (key == "timestamp")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "timestamp must be string");
                std::string_view str = jsonObject.value().get_string();
                LOG_APP_INFO("TimeStamp: {}", str);
            }
            else if (key == "message")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "message must be a string");
                std::string_view message = jsonObject.value().get_string();
                LOG_APP_INFO("The server says: \"{}\"", message);
            }
            else
            {
                simdjson::ondemand::value val = jsonObject.value();
                JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in main reply");
            }
        }

        if (!m_HasError)
        {
            m_Reply = std::move(reply);
        }
        else
        {
            LOG_APP_CRITICAL("void ReplyParserAPI1::Parse(): reply discarded");
        }
    }

    void ReplyParserAPI1::ParseContent(simdjson::ondemand::array jsonArray, Reply& reply)
    {
        using namespace simdjson;

        for (auto element : jsonArray)
        {
            ondemand::object choiceObj = element.get_object();
            Reply::Choice choice{};

            for (auto field : choiceObj)
            {
                std::string_view key = field.unescaped_key();

                if (key == "index")
                {
                    CORE_ASSERT((field.value().type() == ondemand::json_type::number), "index must be integer");
                    uint64_t index = field.value().get_uint64();
                    LOG_APP_INFO("index: {}", index);
                    choice.m_Index = index;
                }
                else if (key == "message")
                {
                    CORE_ASSERT((field.value().type() == ondemand::json_type::object), "message must be object");
                    ondemand::object messageObj = field.value().get_object();

                    for (auto messageField : messageObj)
                    {
                        std::string_view msgKey = messageField.unescaped_key();

                        if (msgKey == "role")
                        {
                            CORE_ASSERT((messageField.value().type() == ondemand::json_type::string), "role must be string");
                            std::string_view role = messageField.value().get_string();
                            LOG_APP_INFO("role: {}", role);
                            choice.m_Message.m_Role = role;
                        }
                        else if (msgKey == "content")
                        {
                            CORE_ASSERT((messageField.value().type() == ondemand::json_type::string),
                                        "content must be string");
                            std::string_view content = messageField.value().get_string();
                            LOG_APP_INFO("content:");
                            std::cout << content << "\n";
                            choice.m_Message.m_Content = content;
                        }
                        else
                        {
                            simdjson::ondemand::value val = messageField.value();
                            JsonObjectParser jsonObjectParser(msgKey, val, "Uncaught json field in message object");
                        }
                    }
                }
                else if (key == "finish_reason")
                {
                    CORE_ASSERT((field.value().type() == ondemand::json_type::string), "finish_reason must be string");
                    std::string_view reason = field.value().get_string();
                    LOG_APP_INFO("finish_reason: {}", reason);
                    choice.m_FinishReason = reason;
                }
                else
                {
                    simdjson::ondemand::value val = field.value();
                    JsonObjectParser jsonObjectParser(key, val, "uncaught json field in choice object");
                }
            }

            reply.m_Choices.push_back(std::move(choice));
        }
    }

    void ReplyParserAPI1::ParseUsage(simdjson::ondemand::object jsonObjects)
    {
        using namespace simdjson;

        for (auto jsonObject : jsonObjects)
        {
            std::string_view key = jsonObject.unescaped_key();
            if (key == "prompt_tokens")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be a number");
                uint64_t tokens = jsonObject.value().get_uint64();
                LOG_APP_INFO("prompt_tokens: {}", tokens);
                m_Reply.m_Usage.m_PromptTokens = tokens;
            }
            else if (key == "completion_tokens")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be a number");
                uint64_t tokens = jsonObject.value().get_uint64();
                LOG_APP_INFO("completion_tokens: {}", tokens);
                m_Reply.m_Usage.m_CompletionTokens = tokens;
            }
            else if (key == "total_tokens")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::number), "type must be a number");
                uint64_t tokens = jsonObject.value().get_uint64();
                LOG_APP_INFO("total_tokens: {}", tokens);
                m_Reply.m_Usage.m_TotalTokens = tokens;
            }
            else
            {
                simdjson::ondemand::value val = jsonObject.value();
                JsonObjectParser jsonObjectParser(key, val, "uncaught json field in server error reply (usage field)");
            }
        }
    }

    void ReplyParserAPI1::ParseError(simdjson::ondemand::object jsonObjects)
    {
        using namespace simdjson;
        ReplyParserAPI1::ErrorInfo errorInfo;

        for (auto jsonObject : jsonObjects)
        {
            std::string_view key = jsonObject.unescaped_key();
            if (key == "message")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view message = jsonObject.value().get_string();
                LOG_APP_INFO("message:");
                std::cout << message << "\n";
                errorInfo.m_Message = message;
            }
            else if (key == "type")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view type = jsonObject.value().get_string();
                LOG_APP_INFO("type: {}", type);
                errorInfo.m_Type = type;
            }
            else if (key == "code")
            {
                CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view code = jsonObject.value().get_string();
                LOG_APP_INFO("code: {}", code);
                errorInfo.m_Code = code;
            }
            else if (key == "param")
            {
                if (!jsonObject.value().is_null())
                {
                    CORE_ASSERT((jsonObject.value().type() == ondemand::json_type::string), "type must be string");
                    std::string_view param = jsonObject.value().get_string();
                    LOG_APP_INFO("parameter: {}", param);
                    errorInfo.m_Param = param;
                }
            }
            else
            {
                simdjson::ondemand::value val = jsonObject.value();
                JsonObjectParser jsonObjectParser(key, val, "uncaught json field in server error reply");
            }
        }
        m_ErrorInfo = errorInfo;
        m_ErrorType = ParseErrorType(m_ErrorInfo.m_Type);
    }

    ReplyParserAPI1::ErrorType ReplyParserAPI1::ParseErrorType(std::string_view type)
    {
        if (type == "invalid_request_error")
        {
            LOG_APP_CRITICAL("There was a invalid request error.");
            return ErrorType::InvalidRequestError;
        }
        if (type == "authentication_error")
        {
            LOG_APP_CRITICAL("There was an authentication error.");
            return ErrorType::AuthenticationError;
        }
        if (type == "permission_error")
        {
            LOG_APP_CRITICAL("There was a permission error.");
            return ErrorType::PermissionError;
        }
        if (type == "rate_limit_error")
        {
            LOG_APP_CRITICAL("There was a rate limit error");
            return ErrorType::RateLimitError;
        }
        if (type == "server_error")
        {
            LOG_APP_CRITICAL("There was a server error");
            return ErrorType::ServerError;
        }
        if (type == "insufficient_quota")
        {
            LOG_APP_CRITICAL("You have insufficient quota. Try again later.");
            return ErrorType::InsufficientQuota;
        }
        return ErrorType::Unknown;
    }
} // namespace AIAssistant
