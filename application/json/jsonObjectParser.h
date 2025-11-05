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
    class JsonObjectParser
    {
    public:
        JsonObjectParser(std::string_view key, simdjson::ondemand::value value, std::string const& warningText,
                         uint32_t indentLevel = 0);
        ~JsonObjectParser() = default;

        bool HasError() const;

    private:
        void Parse();

        void ParseObject(simdjson::ondemand::object obj);
        void ParseArray(simdjson::ondemand::array arr);

        std::string PrintIndented() const;

    private:
        bool m_HasError{false};
        std::string m_Key;                 // safe copy of key
        simdjson::ondemand::value m_Value; // view (valid within parent loop)
        std::string m_WarningText;
        uint32_t m_IndentLevel{0};
    };
} // namespace AIAssistant
