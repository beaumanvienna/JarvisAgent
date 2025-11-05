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
#include "json/replyParserAPI2.h"
#include "json/jsonObjectParser.h"

namespace AIAssistant
{
    ReplyParserAPI2::ReplyParserAPI2(std::string const& jsonString) : ReplyParser(jsonString) { Parse(); }

    ReplyParserAPI2::ErrorInfo const& ReplyParserAPI2::GetErrorInfo() const { return m_ErrorInfo; }

    ReplyParserAPI2::ErrorType ReplyParserAPI2::GetErrorType() const { return m_ErrorType; }

    size_t ReplyParserAPI2::HasContent() const
    {
        size_t returnValue = m_Reply.m_Output.size();
        return returnValue;
    }

    std::string ReplyParserAPI2::GetContent(size_t index) const
    {
        if (index < m_Reply.m_Output.size())
        {
            const auto& output = m_Reply.m_Output[index];
            for (const auto& content : output.m_Content)
            {
                if (content.m_Type == "output_text" && !content.m_Text.empty())
                    return content.m_Text;
            }
        }
        LOG_APP_ERROR("ReplyParserAPI2::GetContent: index out of range or no text content, index: {}", index);
        return {};
    }

    void ReplyParserAPI2::Parse()
    {
        using namespace simdjson;
        ondemand::parser parser;

        padded_string json = padded_string(m_JsonString.data(), m_JsonString.size());

        ondemand::document doc;
        auto error = parser.iterate(json).get(doc);

        if (error)
        {
            LOG_APP_ERROR("ReplyParserAPI2::Parse: Error parsing JSON: {}", error_message(error));
            m_State = ReplyParser::State::ParseFailure;
            return;
        }

        ondemand::document responseDocument = parser.iterate(json);
        ondemand::object jsonObjects = responseDocument.get_object();

        Reply reply{};

        for (auto jsonObject : jsonObjects)
        {
            std::string_view key = jsonObject.unescaped_key();

            if (key == "id")
            {
                std::string_view id = jsonObject.value().get_string();
                LOG_APP_INFO("id: {}", id);
                reply.m_Id = id;
            }
            else if (key == "object")
            {
                std::string_view object = jsonObject.value().get_string();
                LOG_APP_INFO("object: {}", object);
                reply.m_Object = object;
            }
            else if (key == "created_at")
            {
                uint64_t created = jsonObject.value().get_uint64();
                LOG_APP_INFO("created_at: {}", created);
                reply.m_CreatedAt = created;
            }
            else if (key == "status")
            {
                std::string_view status = jsonObject.value().get_string();
                LOG_APP_INFO("status: {}", status);
                reply.m_Status = status;
            }
            else if (key == "model")
            {
                std::string_view model = jsonObject.value().get_string();
                LOG_APP_INFO("model: {}", model);
                reply.m_Model = model;
            }
            else if (key == "output")
            {
                LOG_APP_INFO("Parsing output...");
                ParseOutput(jsonObject.value(), reply);
                m_State = ReplyParser::State::ReplyOk;
            }
            else if (key == "usage")
            {
                ParseUsage(jsonObject.value());
            }
            else if (key == "error")
            {
                if (!jsonObject.value().is_null())
                {
                    LOG_APP_ERROR("Error object present, parsing...");
                    ParseError(jsonObject.value());
                    m_HasError = true;
                    m_State = ReplyParser::State::ReplyError;
                }
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
            LOG_APP_CRITICAL("void ReplyParserAPI2::Parse(): reply discarded");
        }
    }

    void ReplyParserAPI2::ParseOutput(simdjson::ondemand::array outputArray, Reply& reply)
    {
        using namespace simdjson;

        for (auto outputItem : outputArray)
        {
            ondemand::object obj = outputItem.get_object();
            Reply::Output output{};

            for (auto field : obj)
            {
                std::string_view key = field.unescaped_key();

                if (key == "id")
                {
                    std::string_view id = field.value().get_string();
                    output.m_Id = std::string(id);
                }
                else if (key == "type")
                {
                    std::string_view type = field.value().get_string();
                    output.m_Type = std::string(type);
                }
                else if (key == "status")
                {
                    std::string_view status = field.value().get_string();
                    output.m_Status = std::string(status);
                }
                else if (key == "role")
                {
                    std::string_view role = field.value().get_string();
                    output.m_Role = std::string(role);
                }
                else if (key == "content")
                {
                    ondemand::array contentArray = field.value().get_array();
                    for (auto contentElement : contentArray)
                    {
                        ondemand::object contentObj = contentElement.get_object();
                        Reply::Output::Content content;

                        for (auto contentField : contentObj)
                        {
                            std::string_view ck = contentField.unescaped_key();
                            if (ck == "type")
                            {
                                std::string_view type = contentField.value().get_string();
                                content.m_Type = std::string(type);
                            }
                            else if (ck == "text")
                            {
                                std::string_view text = contentField.value().get_string();
                                content.m_Text = std::string(text);
                            }
                        }
                        if (!content.m_Text.empty())
                        {
                            output.m_Content.push_back(std::move(content));
                        }
                        else
                        {
                            LOG_APP_WARN("text was empty");
                        }
                    }
                }
                else
                {
                    simdjson::ondemand::value val = field.value();
                    JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in main reply");
                }
            }

            if (output.m_Content.size())
            {
                reply.m_Output.push_back(std::move(output));
            }
            else
            {
                LOG_APP_WARN("void ReplyParserAPI2::ParseOutput: output discarded because it had no content");
            }
        }
    }

    void ReplyParserAPI2::ParseUsage(simdjson::ondemand::object usageObj)
    {
        using namespace simdjson;

        for (auto field : usageObj)
        {
            std::string_view key = field.unescaped_key();
            if (key == "input_tokens")
            {
                m_Reply.m_Usage.m_InputTokens = field.value().get_uint64();
                LOG_APP_INFO("input_tokens: {}", m_Reply.m_Usage.m_InputTokens);
            }
            else if (key == "output_tokens")
            {
                m_Reply.m_Usage.m_OutputTokens = field.value().get_uint64();
                LOG_APP_INFO("output_tokens: {}", m_Reply.m_Usage.m_OutputTokens);
            }
            else if (key == "total_tokens")
            {
                m_Reply.m_Usage.m_TotalTokens = field.value().get_uint64();
                LOG_APP_INFO("total_tokens: {}", m_Reply.m_Usage.m_TotalTokens);
            }
            else
            {
                simdjson::ondemand::value val = field.value();
                JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in usage parser");
            }
        }
    }

    void ReplyParserAPI2::ParseError(simdjson::ondemand::object errorObj)
    {
        using namespace simdjson;

        ReplyParserAPI2::ErrorInfo errorInfo;

        for (auto field : errorObj)
        {
            std::string_view key = field.unescaped_key();
            if (key == "message")
            {
                std::string_view message = field.value().get_string();
                errorInfo.m_Message = std::string(message);
            }
            else if (key == "type")
            {
                std::string_view type = field.value().get_string();
                errorInfo.m_Type = std::string(type);
            }
            else if (key == "code")
            {
                std::string_view code = field.value().get_string();
                errorInfo.m_Code = std::string(code);
            }
            else if (key == "param")
            {
                if (!field.value().is_null())
                {
                    std::string_view param = field.value().get_string();
                    errorInfo.m_Param = std::string(param);
                }
            }
            else
            {
                simdjson::ondemand::value val = field.value();
                JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in error parser");
            }
        }

        m_ErrorInfo = errorInfo;
        m_ErrorType = ParseErrorType(m_ErrorInfo.m_Type);
    }

    ReplyParserAPI2::ErrorType ReplyParserAPI2::ParseErrorType(std::string_view type)
    {
        if (type == "invalid_request_error")
        {
            LOG_APP_CRITICAL("Invalid request error.");
            return ErrorType::InvalidRequestError;
        }
        if (type == "authentication_error")
        {
            LOG_APP_CRITICAL("Authentication error.");
            return ErrorType::AuthenticationError;
        }
        if (type == "permission_error")
        {
            LOG_APP_CRITICAL("Permission error.");
            return ErrorType::PermissionError;
        }
        if (type == "rate_limit_error")
        {
            LOG_APP_CRITICAL("Rate limit error.");
            return ErrorType::RateLimitError;
        }
        if (type == "server_error")
        {
            LOG_APP_CRITICAL("Server error.");
            return ErrorType::ServerError;
        }
        if (type == "insufficient_quota")
        {
            LOG_APP_CRITICAL("Insufficient quota.");
            return ErrorType::InsufficientQuota;
        }

        return ErrorType::Unknown;
    }

} // namespace AIAssistant
