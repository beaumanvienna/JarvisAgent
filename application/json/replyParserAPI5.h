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

#include <memory>
#include <string>

#include "json/replyParser.h"

namespace AIAssistant
{
    // AWS Bedrock response parser. Bedrock's body is the underlying model family's native
    // response (no extra envelope). Family is sniffed from the JSON shape:
    //   - Anthropic-on-Bedrock: {"type":"message","content":[...],"stop_reason":...} -> ReplyParserAPI4
    //   - Llama-on-Bedrock:     {"generation":"...","stop_reason":"..."}             -> internal LlamaReply
    //   - Titan/Nova-on-Bedrock:{"results":[{"outputText":"..."}]}                   -> internal TitanReply
    //   - AWS error envelope:   {"__type":"<Exception>","message":"..."}             -> classified directly
    //
    // The AWS error path is handled inline (no delegate) because AWS exceptions are
    // pre-family — every Bedrock family surfaces the same `__type` shape on
    // ServiceQuotaExceededException / ThrottlingException / etc.  Family detection runs
    // FIRST for success bodies; the `__type` check runs as a fallback before declaring
    // Unknown shape.
    class ReplyParserAPI5 final : public ReplyParser
    {
    public:
        explicit ReplyParserAPI5(std::string const& jsonString);
        virtual ~ReplyParserAPI5() = default;

        size_t HasContent() const override;
        std::string GetContent(size_t index = 0) const override;
        AiError GetError() const override;
        AiUsage GetUsage() const override;
        std::string GetFinishReason() const override;

    private:
        std::unique_ptr<ReplyParser> m_Delegate;

        // Set when the body is an AWS error envelope ({"__type": "...", "message": "..."}).
        // Populated directly by the ctor — no delegate — so GetError can return the
        // classified AiError without re-parsing.  Empty when the body matched a model family.
        std::string m_AwsErrorType;
        std::string m_AwsErrorMessage;
    };
} // namespace AIAssistant
