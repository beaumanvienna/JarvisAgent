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
#include "json/replyParserAPI3.h"
#include "json/jsonObjectParser.h"

namespace AIAssistant
{
    ReplyParserAPI3::ReplyParserAPI3(std::string const& jsonString) : ReplyParser(jsonString) { Parse(); }

    ReplyParserAPI3::ErrorInfo const& ReplyParserAPI3::GetErrorInfo() const { return m_ErrorInfo; }

    size_t ReplyParserAPI3::HasContent() const { return m_Reply.m_Candidates.size(); }

    std::string ReplyParserAPI3::GetContent(size_t index) const
    {
        if (index < m_Reply.m_Candidates.size())
        {
            auto const& candidate = m_Reply.m_Candidates[index];
            for (auto const& part : candidate.m_Content.m_Parts)
            {
                if (!part.m_Text.empty())
                {
                    return part.m_Text;
                }
            }
        }
        LOG_APP_ERROR("ReplyParserAPI3::GetContent: index out of range or no text content, index: {}", index);
        return {};
    }

    AiError ReplyParserAPI3::GetError() const
    {
        AiError error;
        if (!m_HasError)
        {
            return error;
        }
        error.m_Kind = AiError::Kind::Provider;
        error.m_Message = m_ErrorInfo.m_Message;
        error.m_HttpStatus = m_ErrorInfo.m_Code;
        return error;
    }

    AiUsage ReplyParserAPI3::GetUsage() const
    {
        AiUsage usage;
        usage.m_InputTokens = static_cast<int32_t>(m_Reply.m_UsageMetadata.m_PromptTokenCount);
        usage.m_OutputTokens = static_cast<int32_t>(m_Reply.m_UsageMetadata.m_CandidatesTokenCount);
        usage.m_TotalTokens = static_cast<int32_t>(m_Reply.m_UsageMetadata.m_TotalTokenCount);
        return usage;
    }

    std::string ReplyParserAPI3::GetFinishReason() const
    {
        if (!m_Reply.m_Candidates.empty())
        {
            return m_Reply.m_Candidates.front().m_FinishReason;
        }
        return {};
    }

    void ReplyParserAPI3::Parse()
    {
        using namespace simdjson;
        ondemand::parser parser;

        padded_string json = padded_string(m_JsonString.data(), m_JsonString.size());

        ondemand::document doc;
        auto error = parser.iterate(json).get(doc);

        if (error)
        {
            LOG_APP_ERROR("ReplyParserAPI3::Parse: Error parsing JSON: {}", error_message(error));
            m_State = ReplyParser::State::ParseFailure;
            return;
        }

        ondemand::document responseDocument = parser.iterate(json);
        ondemand::object jsonObjects = responseDocument.get_object();

        Reply reply{};

        for (auto jsonObject : jsonObjects)
        {
            std::string_view key = jsonObject.unescaped_key();

            if (key == "candidates")
            {
                LOG_APP_INFO("Parsing candidates...");
                ParseCandidates(jsonObject.value(), reply);
                m_State = ReplyParser::State::ReplyOk;
            }
            else if (key == "usageMetadata")
            {
                ParseUsageMetadata(jsonObject.value());
            }
            else if (key == "modelVersion")
            {
                std::string_view modelVersion = jsonObject.value().get_string();
                LOG_APP_INFO("modelVersion: {}", modelVersion);
                reply.m_ModelVersion = modelVersion;
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
                JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in Gemini reply");
            }
        }

        if (!m_HasError)
        {
            m_Reply = std::move(reply);
        }
        else
        {
            LOG_APP_CRITICAL("void ReplyParserAPI3::Parse(): reply discarded");
        }
    }

    void ReplyParserAPI3::ParseCandidates(simdjson::ondemand::array candidatesArray, Reply& reply)
    {
        using namespace simdjson;

        for (auto candidateElement : candidatesArray)
        {
            ondemand::object candidateObj = candidateElement.get_object();
            Reply::Candidate candidate{};

            for (auto field : candidateObj)
            {
                std::string_view key = field.unescaped_key();

                if (key == "content")
                {
                    ondemand::object contentObj = field.value().get_object();

                    for (auto contentField : contentObj)
                    {
                        std::string_view ck = contentField.unescaped_key();

                        if (ck == "parts")
                        {
                            ondemand::array partsArray = contentField.value().get_array();
                            for (auto partElement : partsArray)
                            {
                                ondemand::object partObj = partElement.get_object();
                                Reply::Candidate::Content::Part part{};

                                for (auto partField : partObj)
                                {
                                    std::string_view pk = partField.unescaped_key();
                                    if (pk == "text")
                                    {
                                        std::string_view text = partField.value().get_string();
                                        LOG_APP_INFO("content:");
                                        std::cout << text << "\n";
                                        part.m_Text = text;
                                    }
                                    else
                                    {
                                        simdjson::ondemand::value val = partField.value();
                                        JsonObjectParser jsonObjectParser(pk, val,
                                                                          "Uncaught JSON field in Gemini part object");
                                    }
                                }

                                candidate.m_Content.m_Parts.push_back(std::move(part));
                            }
                        }
                        else if (ck == "role")
                        {
                            std::string_view role = contentField.value().get_string();
                            LOG_APP_INFO("role: {}", role);
                            candidate.m_Content.m_Role = role;
                        }
                        else
                        {
                            simdjson::ondemand::value val = contentField.value();
                            JsonObjectParser jsonObjectParser(ck, val, "Uncaught JSON field in Gemini content object");
                        }
                    }
                }
                else if (key == "finishReason")
                {
                    std::string_view reason = field.value().get_string();
                    LOG_APP_INFO("finishReason: {}", reason);
                    candidate.m_FinishReason = reason;
                }
                else
                {
                    simdjson::ondemand::value val = field.value();
                    JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in Gemini candidate object");
                }
            }

            if (!candidate.m_Content.m_Parts.empty())
            {
                reply.m_Candidates.push_back(std::move(candidate));
            }
            else
            {
                LOG_APP_WARN("ReplyParserAPI3::ParseCandidates: candidate discarded because it had no content parts");
            }
        }
    }

    void ReplyParserAPI3::ParseUsageMetadata(simdjson::ondemand::object usageObj)
    {
        using namespace simdjson;

        for (auto field : usageObj)
        {
            std::string_view key = field.unescaped_key();
            if (key == "promptTokenCount")
            {
                m_Reply.m_UsageMetadata.m_PromptTokenCount = field.value().get_uint64();
                LOG_APP_INFO("promptTokenCount: {}", m_Reply.m_UsageMetadata.m_PromptTokenCount);
            }
            else if (key == "candidatesTokenCount")
            {
                m_Reply.m_UsageMetadata.m_CandidatesTokenCount = field.value().get_uint64();
                LOG_APP_INFO("candidatesTokenCount: {}", m_Reply.m_UsageMetadata.m_CandidatesTokenCount);
            }
            else if (key == "totalTokenCount")
            {
                m_Reply.m_UsageMetadata.m_TotalTokenCount = field.value().get_uint64();
                LOG_APP_INFO("totalTokenCount: {}", m_Reply.m_UsageMetadata.m_TotalTokenCount);
            }
            else
            {
                simdjson::ondemand::value val = field.value();
                JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in Gemini usageMetadata");
            }
        }
    }

    void ReplyParserAPI3::ParseError(simdjson::ondemand::object errorObj)
    {
        using namespace simdjson;

        ReplyParserAPI3::ErrorInfo errorInfo;

        for (auto field : errorObj)
        {
            std::string_view key = field.unescaped_key();
            if (key == "code")
            {
                errorInfo.m_Code = static_cast<int>(field.value().get_int64());
                LOG_APP_ERROR("error code: {}", errorInfo.m_Code);
            }
            else if (key == "message")
            {
                std::string_view message = field.value().get_string();
                LOG_APP_ERROR("error message:");
                std::cout << message << "\n";
                errorInfo.m_Message = message;
            }
            else if (key == "status")
            {
                std::string_view status = field.value().get_string();
                LOG_APP_ERROR("error status: {}", status);
                errorInfo.m_Status = status;
            }
            else
            {
                simdjson::ondemand::value val = field.value();
                JsonObjectParser jsonObjectParser(key, val, "Uncaught JSON field in Gemini error object");
            }
        }

        m_ErrorInfo = errorInfo;
        LOG_APP_CRITICAL("Gemini API error: {} ({}): {}", m_ErrorInfo.m_Status, m_ErrorInfo.m_Code, m_ErrorInfo.m_Message);
    }

} // namespace AIAssistant
