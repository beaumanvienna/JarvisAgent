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

#include "json/jsonHelper.h"

namespace AIAssistant
{
    std::string JsonHelper::SanitizeForJson(std::string const& input)
    {
        std::string output;
        output.reserve(input.size());

        for (char c : input)
        {
            switch (c)
            {
                case '\"':
                {
                    output += "\\\"";
                    break;
                }
                case '\\':
                {
                    output += "\\\\";
                    break;
                }
                case '\n':
                {
                    output += "\\n";
                    break;
                }
                case '\r':
                {
                    output += "\\r";
                    break;
                }
                case '\t':
                {
                    output += "\\t";
                    break;
                }
                default:
                {
                    output += c;
                    break;
                }
            }
        }

        return output;
    }
} // namespace AIAssistant
