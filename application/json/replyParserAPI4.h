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

#pragma once

#include <string>
#include <vector>

#include "json/replyParser.h"
#include "simdjson/simdjson.h"

namespace AIAssistant
{
    // Anthropic /v1/messages response parser.
    //
    // Success shape (abridged):
    //   {
    //     "id": "msg_...",
    //     "type": "message",
    //     "role": "assistant",
    //     "model": "claude-sonnet-4-...",
    //     "content": [{"type": "text", "text": "..."}, ...],
    //     "stop_reason": "end_turn" | "max_tokens" | "stop_sequence" | "tool_use",
    //     "usage": {"input_tokens": N, "output_tokens": M}
    //   }
    //
    // Error shape:
    //   {"type": "error", "error": {"type": "...", "message": "..."}}
    class ReplyParserAPI4 final : public ReplyParser
    {
    public:
        struct Reply
        {
            struct ContentBlock
            {
                std::string m_Type; // "text" | "tool_use" | ...
                std::string m_Text;
            };

            struct Usage
            {
                uint64_t m_InputTokens{0};
                uint64_t m_OutputTokens{0};
            };

            std::string m_Id;
            std::string m_Model;
            std::vector<ContentBlock> m_Content;
            std::string m_StopReason;
            Usage m_Usage;
        };

        struct ErrorInfo
        {
            std::string m_Type;
            std::string m_Message;
        };

    public:
        explicit ReplyParserAPI4(std::string const& jsonString);
        virtual ~ReplyParserAPI4() = default;

        ErrorInfo const& GetErrorInfo() const { return m_ErrorInfo; }

        virtual size_t HasContent() const override;
        virtual std::string GetContent(size_t index = 0) const override;
        virtual AiError GetError() const override;
        virtual AiUsage GetUsage() const override;
        virtual std::string GetFinishReason() const override;

    private:
        void Parse();

    private:
        Reply m_Reply;
        ErrorInfo m_ErrorInfo;
    };
} // namespace AIAssistant
