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

namespace AIAssistant
{
    // Structure-aware markdown splitter.  Walks a markdown document line-by-line,
    // identifies ATX-style heading lines (#, ##, ###, ...) as section boundaries,
    // and returns the document split at those boundaries.
    //
    // Headings inside fenced code blocks (``` ... ```) are ignored.  Sections
    // preserve their heading line as the first line of the section body.
    struct MarkdownSection
    {
        int m_HeadingLevel = 0;     // 0 = pre-heading preamble; 1..6 = # through ######
        std::string m_HeadingText;  // empty for preamble
        std::string m_Body;         // includes the heading line for level > 0
    };

    class MarkdownSectionSplitter
    {
    public:
        // Split at the deepest heading level <= maxHeadingLevel that produces >1 section.
        // maxHeadingLevel=1 means "split only at # headings"; maxHeadingLevel=3 means
        // "split at #, ##, or ### — whichever produces segments".
        static std::vector<MarkdownSection> Split(std::string const& markdown, int maxHeadingLevel = 6);
    };
} // namespace AIAssistant
