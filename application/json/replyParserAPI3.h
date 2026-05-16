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
    class ReplyParserAPI3 : public ReplyParser
    {
    public:
        // reply handling
        // ------------------------------------------------------------------
        // Example response (Google Gemini native format):
        //
        // {
        //   "candidates": [
        //     {
        //       "content": {
        //         "parts": [
        //           {
        //             "text": "The engine meets the specified power output requirements."
        //           }
        //         ],
        //         "role": "model"
        //       },
        //       "finishReason": "STOP",
        //       "avgLogprobs": -0.123
        //     }
        //   ],
        //   "usageMetadata": {
        //     "promptTokenCount": 55,
        //     "candidatesTokenCount": 17,
        //     "totalTokenCount": 72
        //   },
        //   "modelVersion": "gemini-2.5-flash"
        // }
        // ------------------------------------------------------------------

        struct Reply
        {
            struct Candidate
            {
                struct Content
                {
                    struct Part
                    {
                        std::string m_Text;
                    };

                    std::vector<Part> m_Parts;
                    std::string m_Role; // "model"
                };

                Content m_Content;
                std::string m_FinishReason; // "STOP", "MAX_TOKENS", "SAFETY", etc.
            };

            struct UsageMetadata
            {
                uint64_t m_PromptTokenCount{0};
                uint64_t m_CandidatesTokenCount{0};
                uint64_t m_TotalTokenCount{0};
            };

            std::vector<Candidate> m_Candidates;
            UsageMetadata m_UsageMetadata;
            std::string m_ModelVersion;
        };

        // error handling
        struct ErrorInfo
        {
            int m_Code{0};                  // HTTP status code (Gemini doesn't have a string code field)
            std::string m_Message;
            std::string m_Status;           // top-level enum: RESOURCE_EXHAUSTED, UNAUTHENTICATED, UNAVAILABLE, …
            std::string m_DetailReason;     // first error.details[*].reason value (e.g. RATE_LIMIT_EXCEEDED,
                                            // USER_PROJECT_QUOTA_EXCEEDED, BILLING_DISABLED).  Disambiguates
                                            // billing vs throttle within RESOURCE_EXHAUSTED.  Empty if absent.
        };

    public:
        explicit ReplyParserAPI3(std::string const& jsonString);
        virtual ~ReplyParserAPI3() = default;

        ErrorInfo const& GetErrorInfo() const;

        virtual size_t HasContent() const override;
        virtual std::string GetContent(size_t index = 0) const override;
        virtual AiError GetError() const override;
        virtual AiUsage GetUsage() const override;
        virtual std::string GetFinishReason() const override;

    private:
        void Parse();
        void ParseCandidates(simdjson::ondemand::array candidatesArray, Reply& reply);
        void ParseUsageMetadata(simdjson::ondemand::object usageObj);
        void ParseError(simdjson::ondemand::object errorObj);

    private:
        Reply m_Reply;
        ErrorInfo m_ErrorInfo;
    };
} // namespace AIAssistant
