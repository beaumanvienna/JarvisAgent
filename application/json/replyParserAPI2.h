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
    class ReplyParserAPI2 : public ReplyParser
    {
    public:
        // reply handling
        // ------------------------------------------------------------------
        // Example response (GPT-5 format):
        //
        // {
        //   "id": "resp_0bed8dfe...",
        //   "object": "response",
        //   "created_at": 1762230881,
        //   "status": "completed",
        //   "model": "gpt-5-nano-2025-08-07",
        //   "output": [
        //     {
        //       "id": "rs_0bed8dfe...",
        //       "type": "reasoning",
        //       "summary": []
        //     },
        //     {
        //       "id": "msg_0bed8dfe...",
        //       "type": "message",
        //       "status": "completed",
        //       "content": [
        //         {
        //           "type": "output_text",
        //           "annotations": [],
        //           "logprobs": [],
        //           "text": "Silent circuits hum,\nLearning through vast data streams,\nYet I dream in code."
        //         }
        //       ],
        //       "role": "assistant"
        //     }
        //   ],
        //   "usage": {
        //     "input_tokens": 14,
        //     "output_tokens": 726,
        //     "total_tokens": 740
        //   }
        // }
        // ------------------------------------------------------------------

        struct Reply
        {
            struct Output
            {
                struct Content
                {
                    std::string m_Type; // usually "output_text"
                    std::string m_Text; // assistant text output
                };

                std::string m_Id;     // message or reasoning id
                std::string m_Type;   // "message" or "reasoning"
                std::string m_Status; // may be "completed"
                std::string m_Role;   // "assistant" or "user"
                std::vector<Content> m_Content;
            };

            struct Usage
            {
                uint64_t m_InputTokens{0};
                uint64_t m_OutputTokens{0};
                uint64_t m_TotalTokens{0};
            };

            std::string m_Id;
            std::string m_Object;
            uint64_t m_CreatedAt{0};
            std::string m_Status;
            std::string m_Model;
            std::vector<Output> m_Output;
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
        explicit ReplyParserAPI2(std::string const& jsonString);
        virtual ~ReplyParserAPI2() = default;

        ErrorInfo const& GetErrorInfo() const;
        ErrorType GetErrorType() const;

        virtual size_t HasContent() const override;
        virtual std::string GetContent(size_t index = 0) const override;

    private:
        void Parse();
        void ParseOutput(simdjson::ondemand::array, Reply&);
        void ParseUsage(simdjson::ondemand::object);
        void ParseError(simdjson::ondemand::object);
        ReplyParserAPI2::ErrorType ParseErrorType(std::string_view type);

    private:
        Reply m_Reply;
        ErrorInfo m_ErrorInfo;
        ErrorType m_ErrorType{ErrorType::Unknown};
    };
} // namespace AIAssistant
