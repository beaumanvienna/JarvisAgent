/* Copyright (c) 2026 JC Technolabs
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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "json/replyParserAPI4.h"

#include "core.h"
#include "engine.h"

namespace AIAssistant
{
    ReplyParserAPI4::ReplyParserAPI4(std::string const& jsonString) : ReplyParser(jsonString) { Parse(); }

    size_t ReplyParserAPI4::HasContent() const
    {
        size_t count = 0;
        for (auto const& block : m_Reply.m_Content)
        {
            if (block.m_Type == "text" && !block.m_Text.empty())
            {
                ++count;
            }
        }
        return count;
    }

    std::string ReplyParserAPI4::GetContent(size_t index) const
    {
        size_t textIndex = 0;
        for (auto const& block : m_Reply.m_Content)
        {
            if (block.m_Type == "text" && !block.m_Text.empty())
            {
                if (textIndex == index)
                {
                    return block.m_Text;
                }
                ++textIndex;
            }
        }
        LOG_APP_ERROR("ReplyParserAPI4::GetContent: index out of range: {}", index);
        return {};
    }

    AiError ReplyParserAPI4::GetError() const
    {
        AiError error;
        if (!m_HasError)
        {
            return error;
        }
        error.m_Kind = AiError::Kind::Provider;
        error.m_Message = m_ErrorInfo.m_Message;
        if (m_ErrorInfo.m_Type == "rate_limit_error")
        {
            error.m_HttpStatus = 429;
        }
        else if (m_ErrorInfo.m_Type == "authentication_error")
        {
            error.m_HttpStatus = 401;
        }
        else if (m_ErrorInfo.m_Type == "permission_error")
        {
            error.m_HttpStatus = 403;
        }
        else if (m_ErrorInfo.m_Type == "overloaded_error" || m_ErrorInfo.m_Type == "api_error")
        {
            error.m_HttpStatus = 503;
        }
        return error;
    }

    AiUsage ReplyParserAPI4::GetUsage() const
    {
        AiUsage usage;
        usage.m_InputTokens = static_cast<int32_t>(m_Reply.m_Usage.m_InputTokens);
        usage.m_OutputTokens = static_cast<int32_t>(m_Reply.m_Usage.m_OutputTokens);
        usage.m_TotalTokens = usage.m_InputTokens + usage.m_OutputTokens;
        return usage;
    }

    std::string ReplyParserAPI4::GetFinishReason() const { return m_Reply.m_StopReason; }

    void ReplyParserAPI4::Parse()
    {
        using namespace simdjson;
        ondemand::parser parser;
        padded_string json = padded_string(m_JsonString.data(), m_JsonString.size());

        ondemand::document doc;
        auto error = parser.iterate(json).get(doc);
        if (error)
        {
            LOG_APP_ERROR("ReplyParserAPI4::Parse: simdjson iterate failed: {}", error_message(error));
            m_State = ReplyParser::State::ParseFailure;
            m_HasError = true;
            m_ErrorInfo.m_Type = "parse_failure";
            m_ErrorInfo.m_Message = error_message(error);
            return;
        }

        ondemand::object root;
        if (doc.get_object().get(root))
        {
            LOG_APP_ERROR("ReplyParserAPI4::Parse: root is not an object");
            m_State = ReplyParser::State::ParseFailure;
            m_HasError = true;
            return;
        }

        std::string_view topType;
        auto const topTypeError = root["type"].get_string().get(topType);
        if (!topTypeError && topType == "error")
        {
            ondemand::object errorObject;
            if (!root["error"].get_object().get(errorObject))
            {
                std::string_view errorTypeView;
                if (!errorObject["type"].get_string().get(errorTypeView))
                {
                    m_ErrorInfo.m_Type = std::string(errorTypeView);
                }
                std::string_view errorMessageView;
                if (!errorObject["message"].get_string().get(errorMessageView))
                {
                    m_ErrorInfo.m_Message = std::string(errorMessageView);
                }
            }
            m_HasError = true;
            m_State = ReplyParser::State::ReplyError;
            return;
        }

        // Reset cursor by re-iterating — simdjson ondemand is single-pass; easiest to re-parse.
        parser = ondemand::parser{};
        padded_string json2 = padded_string(m_JsonString.data(), m_JsonString.size());
        if (parser.iterate(json2).get(doc))
        {
            m_State = ReplyParser::State::ParseFailure;
            m_HasError = true;
            return;
        }
        if (doc.get_object().get(root))
        {
            m_State = ReplyParser::State::ParseFailure;
            m_HasError = true;
            return;
        }

        std::string_view idView;
        if (!root["id"].get_string().get(idView))
        {
            m_Reply.m_Id = std::string(idView);
        }
        std::string_view modelView;
        if (!root["model"].get_string().get(modelView))
        {
            m_Reply.m_Model = std::string(modelView);
        }
        std::string_view stopReasonView;
        if (!root["stop_reason"].get_string().get(stopReasonView))
        {
            m_Reply.m_StopReason = std::string(stopReasonView);
        }

        ondemand::array contentArray;
        if (!root["content"].get_array().get(contentArray))
        {
            for (auto element : contentArray)
            {
                ondemand::object blockObject;
                if (element.get_object().get(blockObject))
                {
                    continue;
                }
                ReplyParserAPI4::Reply::ContentBlock block;
                std::string_view typeView;
                if (!blockObject["type"].get_string().get(typeView))
                {
                    block.m_Type = std::string(typeView);
                }
                std::string_view textView;
                if (!blockObject["text"].get_string().get(textView))
                {
                    block.m_Text = std::string(textView);
                }
                m_Reply.m_Content.push_back(std::move(block));
            }
        }

        ondemand::object usageObject;
        if (!root["usage"].get_object().get(usageObject))
        {
            uint64_t inputTokens = 0;
            if (!usageObject["input_tokens"].get_uint64().get(inputTokens))
            {
                m_Reply.m_Usage.m_InputTokens = inputTokens;
            }
            uint64_t outputTokens = 0;
            if (!usageObject["output_tokens"].get_uint64().get(outputTokens))
            {
                m_Reply.m_Usage.m_OutputTokens = outputTokens;
            }
        }

        m_State = ReplyParser::State::ReplyOk;
    }
} // namespace AIAssistant
