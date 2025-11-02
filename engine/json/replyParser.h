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
#include <string>
#include "simdjson/simdjson.h"

namespace AIAssistant
{
    class ReplyParser
    {
    public:
        // parser state
        enum class State
        {
            Undefined = 0,
            ParseOk,
            ParseFailure,
            ReplyOk,
            ReplyError
        };

        // reply handling
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
        ReplyParser(std::string const& jsonString);
        ~ReplyParser();

        bool HasError() const;
        ErrorInfo const& GetErrorInfo() const;
        ErrorType GetErrorType() const;

        size_t HasContent() const;
        std::string GetContent(size_t index = 0) const;

    private:
        void Parse();
        void ParseContent(simdjson::ondemand::array);
        void ParseUsage(simdjson::ondemand::object);
        void ParseError(simdjson::ondemand::object);
        ReplyParser::ErrorType ParseErrorType(std::string_view type);

    private:
        ReplyParser::State m_State;
        std::string m_JsonString;

        // good reply
        Reply m_Reply;

        // bad reply
        bool m_HasError{false};
        ErrorInfo m_ErrorInfo;
        ErrorType m_ErrorType{ErrorType::Unknown};
    };
} // namespace AIAssistant