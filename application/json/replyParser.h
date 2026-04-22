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
#include <memory>
#include <optional>
#include <string>
#include "json/configParser.h"
#include "workflow/aiReply.h"

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

    public:
        ReplyParser(std::string const& jsonString);
        virtual ~ReplyParser() = default;

        bool HasError() const;
        virtual size_t HasContent() const = 0;
        virtual std::string GetContent(size_t index = 0) const = 0;

        // Provider-agnostic accessors.  Concrete parsers override when the provider
        // exposes the concept; otherwise defaults apply.
        virtual AiError GetError() const;
        virtual AiUsage GetUsage() const;
        virtual std::string GetFinishReason() const;
        virtual std::string GetSystemFingerprint() const;
        virtual std::optional<std::string> GetStructuredOutput() const;

        static std::unique_ptr<ReplyParser> Create(ConfigParser::EngineConfig::InterfaceType const& interfaceType,
                                                   std::string const& jsonString);

    protected:
        ReplyParser::State m_State;
        std::string m_JsonString;

        // bad reply
        bool m_HasError{false};
    };
} // namespace AIAssistant
