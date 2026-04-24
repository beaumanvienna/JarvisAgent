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
#include <vector>

namespace AIAssistant
{
    // Structure-aware chunk planner (§8 Phase 6).
    //
    // Given a body, a context budget (in estimated tokens), and the prompt overhead,
    // returns N chunks that each fit inside (budget - overhead).  The planner prefers
    // whole markdown sections (split at `#`, then `##`, then `###`) and only subdivides
    // within a section when the section alone exceeds the budget.
    //
    // Token estimation is the standard chars ÷ 4 heuristic for English text.
    //
    // If the entire body fits in one chunk, returns a single-element vector containing
    // the unchanged body.
    class ChunkPlanner
    {
    public:
        static uint64_t EstimateTokens(std::string const& text);

        static std::vector<std::string> Plan(std::string const& body, uint64_t maxContextTokens,
                                              uint64_t promptOverheadTokens);
    };
} // namespace AIAssistant
