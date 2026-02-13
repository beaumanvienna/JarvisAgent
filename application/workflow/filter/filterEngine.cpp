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

#include "workflow/filter/filterEngine.h"

#include <filesystem>
#include <fstream>

#include "workflow/filter/polarionClient.h"
#include "workflow/filter/queryParser.h"

#include "engine.h"

namespace AIAssistant
{

    // -----------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------

    std::vector<FilterItem> FilterEngine::Evaluate(FilterDef const& filter, std::string const& workflowBaseDir,
                                                   std::string& errorMessage) const
    {
        std::string const& kind = filter.m_Source.m_Kind;

        if (kind == "csv")
        {
            std::string resolvedPath = filter.m_Source.m_Path;
            if (std::filesystem::path(resolvedPath).is_relative())
            {
                resolvedPath = (std::filesystem::path(workflowBaseDir) / resolvedPath).string();
            }

            auto items = EvaluateCsv(filter, resolvedPath, errorMessage);
            AddPaddedIndices(items);
            return items;
        }

        if (kind == "text_lines")
        {
            std::string resolvedPath = filter.m_Source.m_Path;
            if (std::filesystem::path(resolvedPath).is_relative())
            {
                resolvedPath = (std::filesystem::path(workflowBaseDir) / resolvedPath).string();
            }

            auto items = EvaluateTextLines(filter, resolvedPath, errorMessage);
            AddPaddedIndices(items);
            return items;
        }

        if (kind == "query")
        {
            auto items = EvaluateQuery(filter, workflowBaseDir, errorMessage);
            AddPaddedIndices(items);
            return items;
        }

        if (kind == "polarion_query")
        {
            auto items = EvaluatePolarionQuery(filter, workflowBaseDir, errorMessage);
            AddPaddedIndices(items);
            return items;
        }

        errorMessage = "filter '" + filter.m_Id + "': unknown source kind '" + kind + "'";
        return {};
    }

    void FilterEngine::AddPaddedIndices(std::vector<FilterItem>& items)
    {
        if (items.empty())
        {
            return;
        }

        // Determine padding width from the largest index and row_number
        size_t maxIndex = items.back().m_Index;
        size_t maxRowNumber = 0;
        for (auto const& item : items)
        {
            if (item.m_SourceIndex > maxRowNumber)
            {
                maxRowNumber = item.m_SourceIndex;
            }
        }

        auto zeroPad = [](size_t value, int width) -> std::string
        {
            std::string s = std::to_string(value);
            if (static_cast<int>(s.size()) < width)
            {
                s.insert(0, width - static_cast<int>(s.size()), '0');
            }
            return s;
        };

        int const indexWidth = static_cast<int>(std::to_string(maxIndex).size());
        int const rowWidth = static_cast<int>(std::to_string(maxRowNumber).size());

        // Use at least 3 digits for readability
        int const padIndex = std::max(indexWidth, 3);
        int const padRow = std::max(rowWidth, 3);

        for (auto& item : items)
        {
            item.m_Values["index_padded"] = zeroPad(item.m_Index, padIndex);

            auto rowIt = item.m_Values.find("row_number");
            if (rowIt != item.m_Values.end())
            {
                size_t rowNum = item.m_SourceIndex;
                item.m_Values["row_number_padded"] = zeroPad(rowNum, padRow);
            }
        }
    }

    // -----------------------------------------------------------------
    // CSV evaluation
    // -----------------------------------------------------------------

    std::vector<FilterItem> FilterEngine::EvaluateCsv(FilterDef const& filter, std::string const& resolvedPath,
                                                      std::string& errorMessage) const
    {
        std::ifstream file(resolvedPath);
        if (!file.is_open())
        {
            errorMessage = "filter '" + filter.m_Id + "': cannot open CSV file '" + resolvedPath + "'";
            return {};
        }

        FilterSource const& source = filter.m_Source;
        char const delimiter = source.m_Delimiter.empty() ? ',' : source.m_Delimiter[0];

        // Parse range (1-based inclusive)
        size_t rangeStart = 1;
        size_t rangeEnd = SIZE_MAX;
        if (!source.m_Range.empty())
        {
            if (!ParseRange(source.m_Range, rangeStart, rangeEnd))
            {
                errorMessage = "filter '" + filter.m_Id + "': malformed range '" + source.m_Range + "'";
                return {};
            }
        }

        // Read header if present
        std::vector<std::string> headerNames;
        std::string line;

        if (source.m_HasHeader)
        {
            if (!std::getline(file, line))
            {
                errorMessage = "filter '" + filter.m_Id + "': CSV file is empty (expected header)";
                return {};
            }

            headerNames = SplitCsvLine(line, delimiter);
        }

        // Read data rows
        std::vector<FilterItem> items;
        size_t resultIndex = 0;
        size_t dataRowNumber = 1; // 1-based row number (first data row = 1, regardless of header)

        while (std::getline(file, line))
        {
            // Apply range filter (1-based)
            if (dataRowNumber < rangeStart)
            {
                ++dataRowNumber;
                continue;
            }

            if (dataRowNumber > rangeEnd)
            {
                break;
            }

            // Enforce max_items (0 = unlimited)
            if (filter.m_MaxItems > 0 && resultIndex >= filter.m_MaxItems)
            {
                LOG_CORE_WARN("filter '{}': max_items ({}) reached, stopping CSV read", filter.m_Id, filter.m_MaxItems);
                break;
            }

            std::vector<std::string> columns = SplitCsvLine(line, delimiter);

            FilterItem item;
            item.m_Index = resultIndex;
            item.m_SourceIndex = dataRowNumber;
            item.m_SourcePath = resolvedPath;

            // Populate values
            item.m_Values["index"] = std::to_string(resultIndex);
            item.m_Values["row_number"] = std::to_string(dataRowNumber);
            item.m_Values["line"] = line;

            // By header name
            for (size_t col = 0; col < columns.size(); ++col)
            {
                std::string positionalKey = "col_" + std::to_string(col);
                item.m_Values[positionalKey] = columns[col];

                if (col < headerNames.size())
                {
                    item.m_Values[headerNames[col]] = columns[col];
                }
            }

            // Stable identity: first column value (or row number if empty)
            item.m_Key = columns.empty() ? std::to_string(dataRowNumber) : columns[0];

            items.push_back(std::move(item));
            ++resultIndex;
            ++dataRowNumber;
        }

        LOG_APP_INFO("[filter] CSV filter '{}' produced {} items from '{}'", filter.m_Id, items.size(), resolvedPath);

        return items;
    }

    // -----------------------------------------------------------------
    // Text lines evaluation
    // -----------------------------------------------------------------

    std::vector<FilterItem> FilterEngine::EvaluateTextLines(FilterDef const& filter, std::string const& resolvedPath,
                                                            std::string& errorMessage) const
    {
        std::ifstream file(resolvedPath);
        if (!file.is_open())
        {
            errorMessage = "filter '" + filter.m_Id + "': cannot open text file '" + resolvedPath + "'";
            return {};
        }

        FilterSource const& source = filter.m_Source;

        std::vector<FilterItem> items;
        std::string line;
        size_t lineNumber = 0; // 0-based
        size_t resultIndex = 0;

        while (std::getline(file, line))
        {
            // Optionally skip empty lines
            if (source.m_SkipEmpty)
            {
                std::string trimmed = Trim(line);
                if (trimmed.empty())
                {
                    ++lineNumber;
                    continue;
                }
            }

            // Enforce max_items (0 = unlimited)
            if (filter.m_MaxItems > 0 && resultIndex >= filter.m_MaxItems)
            {
                LOG_CORE_WARN("filter '{}': max_items ({}) reached, stopping text read", filter.m_Id, filter.m_MaxItems);
                break;
            }

            FilterItem item;
            item.m_Index = resultIndex;
            item.m_SourceIndex = lineNumber;
            item.m_SourcePath = resolvedPath;

            item.m_Values["index"] = std::to_string(resultIndex);
            item.m_Values["text"] = line;

            // Stable identity: the line content itself
            item.m_Key = line;

            items.push_back(std::move(item));
            ++resultIndex;
            ++lineNumber;
        }

        LOG_APP_INFO("[filter] text_lines filter '{}' produced {} items from '{}'", filter.m_Id, items.size(), resolvedPath);

        return items;
    }

    // -----------------------------------------------------------------
    // Query evaluation
    //
    // Reads a simple line-delimited index file where each line is a
    // tab-separated record: field1<TAB>value1<TAB>field2<TAB>value2...
    // The query is parsed into an AST and evaluated against each record
    // as a post-filter.  For full Lucene index support (Whoosh/pylucene)
    // see the Python bridge path (future).
    // -----------------------------------------------------------------

    std::vector<FilterItem> FilterEngine::EvaluateQuery(FilterDef const& filter, std::string const& workflowBaseDir,
                                                        std::string& errorMessage) const
    {
        FilterSource const& source = filter.m_Source;

        // Parse the query expression
        QueryParser parser;
        auto queryAst = parser.Parse(source.m_Query, errorMessage);
        if (!queryAst)
        {
            errorMessage = "filter '" + filter.m_Id + "': query parse error: " + errorMessage;
            return {};
        }

        // Resolve index path
        std::string indexPath = source.m_IndexPath;
        if (std::filesystem::path(indexPath).is_relative())
        {
            indexPath = (std::filesystem::path(workflowBaseDir) / indexPath).string();
        }

        std::ifstream file(indexPath);
        if (!file.is_open())
        {
            errorMessage = "filter '" + filter.m_Id + "': cannot open index file '" + indexPath + "'";
            return {};
        }

        // Determine which fields to extract (empty = all fields in record)
        std::vector<std::string> const& requestedFields = source.m_Fields;

        std::vector<FilterItem> items;
        std::string line;
        size_t lineNumber = 0;
        size_t resultIndex = 0;

        while (std::getline(file, line))
        {
            if (line.empty())
            {
                ++lineNumber;
                continue;
            }

            // Parse tab-separated key-value pairs: field1\tvalue1\tfield2\tvalue2...
            QueryDocument doc;
            {
                size_t pos = 0;
                while (pos < line.size())
                {
                    size_t tabKey = line.find('\t', pos);
                    if (tabKey == std::string::npos)
                    {
                        break;
                    }

                    std::string fieldName = line.substr(pos, tabKey - pos);
                    pos = tabKey + 1;

                    size_t tabVal = line.find('\t', pos);
                    std::string fieldValue;
                    if (tabVal == std::string::npos)
                    {
                        fieldValue = line.substr(pos);
                        pos = line.size();
                    }
                    else
                    {
                        fieldValue = line.substr(pos, tabVal - pos);
                        pos = tabVal + 1;
                    }

                    doc[fieldName] = fieldValue;
                }
            }

            // Evaluate query against this document
            if (!QueryParser::Matches(*queryAst, doc))
            {
                ++lineNumber;
                continue;
            }

            // Enforce max_items
            if (filter.m_MaxItems > 0 && resultIndex >= filter.m_MaxItems)
            {
                LOG_CORE_WARN("filter '{}': max_items ({}) reached, stopping query read", filter.m_Id, filter.m_MaxItems);
                break;
            }

            FilterItem item;
            item.m_Index = resultIndex;
            item.m_SourceIndex = lineNumber;
            item.m_SourcePath = indexPath;

            item.m_Values["index"] = std::to_string(resultIndex);

            // Extract requested fields (or all if none specified)
            if (requestedFields.empty())
            {
                for (auto const& [k, v] : doc)
                {
                    item.m_Values[k] = v;
                }
            }
            else
            {
                for (auto const& fieldName : requestedFields)
                {
                    auto it = doc.find(fieldName);
                    if (it != doc.end())
                    {
                        item.m_Values[fieldName] = it->second;
                    }
                }
            }

            // doc_path: if the document has a "doc_path" field, use it for freshness
            auto docPathIt = doc.find("doc_path");
            if (docPathIt != doc.end())
            {
                item.m_SourcePath = docPathIt->second;
                item.m_Values["doc_path"] = docPathIt->second;
            }

            // Stable identity: "id" field if present, else line number
            auto idIt = doc.find("id");
            item.m_Key = (idIt != doc.end()) ? idIt->second : std::to_string(lineNumber);

            items.push_back(std::move(item));
            ++resultIndex;
            ++lineNumber;
        }

        LOG_APP_INFO("[filter] query filter '{}' produced {} items from '{}'", filter.m_Id, items.size(), indexPath);

        return items;
    }

    // -----------------------------------------------------------------
    // Polarion query evaluation — delegates to PolarionClient
    // -----------------------------------------------------------------

    std::vector<FilterItem> FilterEngine::EvaluatePolarionQuery(FilterDef const& filter, std::string const& workflowBaseDir,
                                                                std::string& errorMessage) const
    {
        PolarionClient client;
        return client.FetchAll(filter, workflowBaseDir, errorMessage);
    }

    // -----------------------------------------------------------------
    // Range parser
    // -----------------------------------------------------------------

    bool FilterEngine::ParseRange(std::string const& range, size_t& outStart, size_t& outEnd)
    {
        // Formats: "10-20" (rows 10–20), "5-" (row 5 to end), "-50" (first 50 rows)
        // All values are 1-based inclusive.
        outStart = 1;
        outEnd = SIZE_MAX;

        if (range.empty())
        {
            return true;
        }

        auto const dashPos = range.find('-');
        if (dashPos == std::string::npos)
        {
            // Single number: treat as "N-N"
            try
            {
                size_t val = std::stoull(range);
                if (val == 0)
                {
                    return false;
                }

                outStart = val;
                outEnd = val;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        std::string const leftStr = range.substr(0, dashPos);
        std::string const rightStr = range.substr(dashPos + 1);

        try
        {
            if (leftStr.empty() && rightStr.empty())
            {
                // "-" alone is invalid
                return false;
            }

            if (leftStr.empty())
            {
                // "-50" → first 50 rows (rows 1 to 50)
                outStart = 1;
                outEnd = std::stoull(rightStr);
                if (outEnd == 0)
                {
                    return false;
                }
            }
            else if (rightStr.empty())
            {
                // "5-" → row 5 to end
                outStart = std::stoull(leftStr);
                outEnd = SIZE_MAX;
                if (outStart == 0)
                {
                    return false;
                }
            }
            else
            {
                // "10-20"
                outStart = std::stoull(leftStr);
                outEnd = std::stoull(rightStr);
                if (outStart == 0 || outEnd == 0 || outStart > outEnd)
                {
                    return false;
                }
            }
        }
        catch (...)
        {
            return false;
        }

        return true;
    }

    // -----------------------------------------------------------------
    // CSV line splitter (respects double-quote escaping)
    // -----------------------------------------------------------------

    std::vector<std::string> FilterEngine::SplitCsvLine(std::string const& line, char delimiter)
    {
        std::vector<std::string> fields;
        std::string current;
        bool inQuotes = false;

        for (size_t i = 0; i < line.size(); ++i)
        {
            char c = line[i];

            if (inQuotes)
            {
                if (c == '"')
                {
                    // Check for escaped quote ("")
                    if (i + 1 < line.size() && line[i + 1] == '"')
                    {
                        current += '"';
                        ++i; // skip the second quote
                    }
                    else
                    {
                        inQuotes = false;
                    }
                }
                else
                {
                    current += c;
                }
            }
            else
            {
                if (c == '"')
                {
                    inQuotes = true;
                }
                else if (c == delimiter)
                {
                    fields.push_back(current);
                    current.clear();
                }
                else
                {
                    current += c;
                }
            }
        }

        fields.push_back(current);
        return fields;
    }

    // -----------------------------------------------------------------
    // Trim
    // -----------------------------------------------------------------

    std::string FilterEngine::Trim(std::string const& s)
    {
        auto const start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
        {
            return "";
        }

        auto const end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

} // namespace AIAssistant
