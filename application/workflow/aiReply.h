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

#include <cstdint>
#include <string>

namespace AIAssistant
{
    struct AiUsage
    {
        int32_t m_InputTokens = 0;
        int32_t m_OutputTokens = 0;
        int32_t m_TotalTokens = 0;
    };

    struct AiError
    {
        enum class Kind
        {
            None,
            Http,
            Parse,
            SchemaValidation,
            Timeout,
            Transport,
            Provider
        };

        Kind m_Kind = Kind::None;
        int m_HttpStatus = 0;
        std::string m_Message;
    };

    struct AiReply
    {
        enum class Kind
        {
            Text,
            Structured,
            Error
        };

        Kind m_Kind = Kind::Error;
        std::string m_Text;
        std::string m_StructuredJson;
        AiError m_Error;
        AiUsage m_Usage;
        std::string m_FinishReason;
        std::string m_SystemFingerprint;
    };
} // namespace AIAssistant
