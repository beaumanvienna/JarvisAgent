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

#pragma once
#include "json/replyParser.h"
#include "simdjson/simdjson.h"

namespace AIAssistant
{
    class ReplyParserAPI1 : public ReplyParser
    {
    public:
        // reply handling
        // ------------------------------------------------------------------
        // Example response (GPT-4 format):
        //
        // {
        //  "id": "chatcmpl-abc123",
        //  "object": "chat.completion",
        //  "created": 1677858242,
        //  "model": "gpt-4.1",
        //  "choices": [
        //    {
        //      "index": 0,
        //      "message": {
        //        "role": "assistant",
        //        "content": "The engine meets the specified power output requirements."
        //      },
        //      "finish_reason": "stop"
        //    }
        //  ],
        //  "usage": {
        //    "prompt_tokens": 55,
        //    "completion_tokens": 17,
        //    "total_tokens": 72
        //  }
        struct Reply
        {
            struct Choice
            {
                struct Message
                {
                    std::string m_Role;
                    std::string m_Content;
                };
                uint64_t m_Index{0};
                Message m_Message;
                std::string m_FinishReason;
            };
            struct Usage
            {
                uint64_t m_PromptTokens{0};
                uint64_t m_CompletionTokens{0};
                uint64_t m_TotalTokens{0};
            };
            std::string m_Id;
            std::string m_Object;
            int m_Created;
            std::string m_Model;
            std::vector<Choice> m_Choices;
            Usage m_Usage;
        };

        // error handling
        struct ErrorInfo
        {
            std::string m_Message;
            std::string m_Type;
            std::string m_Code;
            std::string m_Param;
        };

        enum class ErrorType
        {
            Unknown = 0,
            InvalidRequestError,
            AuthenticationError,
            PermissionError,
            RateLimitError,
            ServerError,
            InsufficientQuota
        };

    public:
        ReplyParserAPI1(std::string const& jsonString);
        virtual ~ReplyParserAPI1() = default;

        ErrorInfo const& GetErrorInfo() const;
        ErrorType GetErrorType() const;

        virtual size_t HasContent() const override;
        virtual std::string GetContent(size_t index = 0) const override;

    private:
        void Parse();
        void ParseContent(simdjson::ondemand::array, Reply&);
        void ParseUsage(simdjson::ondemand::object);
        void ParseError(simdjson::ondemand::object);
        ReplyParserAPI1::ErrorType ParseErrorType(std::string_view type);

    private:
        // good reply
        Reply m_Reply;

        // bad reply
        ErrorInfo m_ErrorInfo;
        ErrorType m_ErrorType{ErrorType::Unknown};
    };
} // namespace AIAssistant