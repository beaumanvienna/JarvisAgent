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

#include "engine.h"
#include "json/replyParser.h"
#include "json/replyParserAPI1.h"
#include "json/replyParserAPI2.h"

namespace AIAssistant
{

    ReplyParser::ReplyParser(std::string const& jsonString) : m_JsonString(jsonString) {}

    bool ReplyParser::HasError() const { return m_HasError; }

    std::unique_ptr<ReplyParser> ReplyParser::Create(ConfigParser::EngineConfig::InterfaceType const& interfaceType,
                                                     std::string const& jsonString)
    {
        std::unique_ptr<ReplyParser> replyParser;

        switch (interfaceType)
        {
            case ConfigParser::EngineConfig::InterfaceType::API1:
            {
                replyParser = std::make_unique<ReplyParserAPI1>(jsonString);
                break;
            }
            case ConfigParser::EngineConfig::InterfaceType::API2:
            {
                replyParser = std::make_unique<ReplyParserAPI2>(jsonString);
                break;
            }
            default:
            {
                LOG_APP_CRITICAL("ReplyParser::Create: api not supported");
                break;
            }
        };
        return replyParser;
    }

} // namespace AIAssistant
