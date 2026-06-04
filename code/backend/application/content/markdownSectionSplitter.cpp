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

#include "content/markdownSectionSplitter.h"

#include <sstream>

namespace AIAssistant
{
    namespace
    {
        int DetectHeadingLevel(std::string const& line, std::string& outText)
        {
            size_t cursor = 0;
            while (cursor < line.size() && line[cursor] == '#')
            {
                ++cursor;
            }
            if (cursor == 0 || cursor > 6)
            {
                return 0;
            }
            if (cursor >= line.size())
            {
                outText.clear();
                return static_cast<int>(cursor);
            }
            if (line[cursor] != ' ' && line[cursor] != '\t')
            {
                return 0;
            }
            size_t textStart = cursor + 1;
            while (textStart < line.size() && (line[textStart] == ' ' || line[textStart] == '\t'))
            {
                ++textStart;
            }
            outText = line.substr(textStart);
            while (!outText.empty() && (outText.back() == ' ' || outText.back() == '\t' || outText.back() == '\r'))
            {
                outText.pop_back();
            }
            return static_cast<int>(cursor);
        }

        std::vector<MarkdownSection> SplitAtLevel(std::string const& markdown, int targetLevel)
        {
            std::vector<MarkdownSection> sections;
            MarkdownSection current;
            current.m_HeadingLevel = 0;

            std::istringstream stream(markdown);
            std::string line;
            bool insideFence = false;

            while (std::getline(stream, line))
            {
                if (line.rfind("```", 0) == 0 || line.rfind("~~~", 0) == 0)
                {
                    insideFence = !insideFence;
                    current.m_Body += line + "\n";
                    continue;
                }
                if (!insideFence)
                {
                    std::string headingText;
                    int const level = DetectHeadingLevel(line, headingText);
                    if (level > 0 && level <= targetLevel)
                    {
                        if (!current.m_Body.empty() || current.m_HeadingLevel > 0)
                        {
                            sections.push_back(std::move(current));
                        }
                        current = MarkdownSection{};
                        current.m_HeadingLevel = level;
                        current.m_HeadingText = headingText;
                        current.m_Body = line + "\n";
                        continue;
                    }
                }
                current.m_Body += line + "\n";
            }
            if (!current.m_Body.empty() || current.m_HeadingLevel > 0)
            {
                sections.push_back(std::move(current));
            }
            return sections;
        }
    } // anonymous namespace

    std::vector<MarkdownSection> MarkdownSectionSplitter::Split(std::string const& markdown, int maxHeadingLevel)
    {
        if (markdown.empty())
        {
            return {};
        }
        int const clampedMax = (maxHeadingLevel < 1) ? 1 : (maxHeadingLevel > 6 ? 6 : maxHeadingLevel);
        for (int level = 1; level <= clampedMax; ++level)
        {
            std::vector<MarkdownSection> sections = SplitAtLevel(markdown, level);
            if (sections.size() > 1)
            {
                return sections;
            }
        }
        MarkdownSection whole;
        whole.m_HeadingLevel = 0;
        whole.m_Body = markdown;
        return {whole};
    }
} // namespace AIAssistant
