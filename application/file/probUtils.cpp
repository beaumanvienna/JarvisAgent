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

#include "file/probUtils.h"

namespace AIAssistant
{
    namespace ProbUtils
    {
        std::optional<ProbFileInfo> ParseProbFilename(std::string const& filename)
        {
            if (!filename.starts_with("PROB_"))
            {
                return std::nullopt;
            }

            // Only accept .txt and .output.txt
            bool isOutput = filename.ends_with(".output.txt");
            bool isInput = filename.ends_with(".txt") && !isOutput;

            if (!isInput && !isOutput)
            {
                return std::nullopt;
            }

            size_t pos1 = filename.find('_');           // after PROB
            size_t pos2 = filename.find('_', pos1 + 1); // after id

            if (pos1 == std::string::npos || pos2 == std::string::npos)
            {
                return std::nullopt;
            }

            size_t tsEnd;

            if (isOutput)
            {
                // PROB_<id>_<ts>.output.txt
                tsEnd = filename.find(".output.txt", pos2 + 1);
            }
            else
            {
                // PROB_<id>_<ts>.txt
                tsEnd = filename.find(".txt", pos2 + 1);
            }

            if (tsEnd == std::string::npos)
            {
                return std::nullopt;
            }

            try
            {
                ProbFileInfo info;
                info.id = std::stoull(filename.substr(pos1 + 1, pos2 - pos1 - 1));
                info.timestamp = std::stoll(filename.substr(pos2 + 1, tsEnd - pos2 - 1));
                info.isOutput = isOutput;
                return info;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
    } // namespace ProbUtils
} // namespace AIAssistant
