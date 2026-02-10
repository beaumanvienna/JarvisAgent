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
#include <unordered_map>
#include <vector>

#include "workflow/workflowTypes.h"

namespace AIAssistant
{

    // Represents a single item produced by filter evaluation.
    // Keys in m_Values do NOT include the binding prefix; the binding is
    // applied by the runtime when injecting values into task instances.
    struct FilterItem
    {
        size_t m_Index{0};       // 0-based within the result set
        size_t m_SourceIndex{0}; // original index in source (row number, line number, hit number)
        std::unordered_map<std::string, std::string> m_Values;
        std::string m_SourcePath; // filesystem path of the source (for freshness)
        std::string m_Key;        // stable identity (e.g. first field value or line content hash)
    };

    // Evaluates filter definitions to produce item lists for per_item task expansion.
    class FilterEngine
    {
    public:
        // Evaluate a filter and return the matched items.
        // On failure, returns an empty vector and populates errorMessage.
        std::vector<FilterItem> Evaluate(FilterDef const& filter, std::string const& workflowBaseDir,
                                         std::string& errorMessage) const;

    private:
        // Source-kind-specific evaluators
        std::vector<FilterItem> EvaluateCsv(FilterDef const& filter, std::string const& resolvedPath,
                                            std::string& errorMessage) const;

        std::vector<FilterItem> EvaluateTextLines(FilterDef const& filter, std::string const& resolvedPath,
                                                  std::string& errorMessage) const;

        std::vector<FilterItem> EvaluateQuery(FilterDef const& filter, std::string const& workflowBaseDir,
                                              std::string& errorMessage) const;

        std::vector<FilterItem> EvaluatePolarionQuery(FilterDef const& filter, std::string const& workflowBaseDir,
                                                      std::string& errorMessage) const;

        // Parse a row range string: "10-20", "5-", "-50", or empty (all rows).
        // outStart and outEnd are 1-based inclusive. Returns false on malformed input.
        // For "all rows", outStart=1 and outEnd=SIZE_MAX.
        static bool ParseRange(std::string const& range, size_t& outStart, size_t& outEnd);

        // Split a single CSV line on the given delimiter. Respects double-quote escaping.
        static std::vector<std::string> SplitCsvLine(std::string const& line, char delimiter);

        // Trim leading/trailing whitespace.
        static std::string Trim(std::string const& s);
    };

} // namespace AIAssistant
