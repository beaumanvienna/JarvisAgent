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

#include "file/scriptRegistry.h"
#include "engine.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace AIAssistant
{
    static std::string TrimLeft(std::string const& s)
    {
        size_t start = s.find_first_not_of(" \t");
        return (start == std::string::npos) ? "" : s.substr(start);
    }

    static std::string Trim(std::string const& s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    static bool IsScriptExtension(std::filesystem::path const& p)
    {
        auto ext = p.extension().string();
        return ext == ".sh" || ext == ".py" || ext == ".ps1";
    }

    // ----------------------------------------------------------------
    // ParseHeader — reads first 50 lines looking for @jarvis-script
    // marker, then extracts @short, @params, @description, @outputs.
    // Returns false if the marker is not found within the first 20 lines.
    // ----------------------------------------------------------------
    bool ScriptRegistry::ParseHeader(std::filesystem::path const& filePath, ScriptRegistryEntry& outEntry)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        // Phase 1: find @jarvis-script marker within first 20 lines
        std::string line;
        bool markerFound = false;
        int lineNumber = 0;
        while (lineNumber < 20 && std::getline(file, line))
        {
            ++lineNumber;
            if (line.find("@jarvis-script") != std::string::npos)
            {
                markerFound = true;
                break;
            }
        }

        if (!markerFound)
        {
            return false;
        }

        outEntry.m_FilePath = filePath.string();
        outEntry.m_Short.clear();
        outEntry.m_Description.clear();
        outEntry.m_Params.clear();
        outEntry.m_Outputs.clear();

        // Phase 2: parse @-fields from remaining lines (up to line 50)
        enum class Field
        {
            None,
            Short,
            Params,
            Description,
            Outputs
        };
        Field currentField = Field::None;

        while (lineNumber < 50 && std::getline(file, line))
        {
            ++lineNumber;
            std::string trimmed = TrimLeft(line);

            // Must be a comment line (# prefix) to be part of header
            if (trimmed.empty() || trimmed[0] != '#')
            {
                break; // end of header block
            }

            // Strip leading '# ' or '#'
            std::string content;
            if (trimmed.size() > 1 && trimmed[1] == ' ')
            {
                content = trimmed.substr(2);
            }
            else
            {
                content = trimmed.substr(1);
            }

            // Check for new @-field (accept both "@field:" and "@field " formats)
            auto matchField = [&](char const* tag, size_t tagLen) -> std::string
            {
                if (content.size() >= tagLen && content.compare(0, tagLen, tag) == 0)
                {
                    size_t offset = tagLen;
                    // Skip optional colon and/or spaces after the tag name
                    if (offset < content.size() && content[offset] == ':')
                    {
                        ++offset;
                    }
                    return Trim(content.substr(offset));
                }
                return {};
            };

            if (content.compare(0, 6, "@short") == 0 && (content.size() == 6 || content[6] == ':' || content[6] == ' '))
            {
                currentField = Field::Short;
                outEntry.m_Short = matchField("@short", 6);
                continue;
            }
            if (content.compare(0, 7, "@params") == 0 && (content.size() == 7 || content[7] == ':' || content[7] == ' '))
            {
                currentField = Field::Params;
                std::string rest = matchField("@params", 7);
                if (!rest.empty())
                {
                    outEntry.m_Params.push_back(rest);
                }
                continue;
            }
            if (content.compare(0, 12, "@description") == 0 &&
                (content.size() == 12 || content[12] == ':' || content[12] == ' '))
            {
                currentField = Field::Description;
                outEntry.m_Description = matchField("@description", 12);
                continue;
            }
            if (content.compare(0, 8, "@outputs") == 0 && (content.size() == 8 || content[8] == ':' || content[8] == ' '))
            {
                currentField = Field::Outputs;
                std::string rest = matchField("@outputs", 8);
                if (!rest.empty())
                {
                    outEntry.m_Outputs.push_back(rest);
                }
                continue;
            }

            // Any other @-field we don't recognize ends parsing
            if (!content.empty() && content[0] == '@')
            {
                break;
            }

            // Continuation line — append to current field
            std::string continuation = Trim(content);

            // Handle "  - " list items (strip leading "- ")
            if (continuation.size() >= 2 && continuation[0] == '-' && continuation[1] == ' ')
            {
                continuation = continuation.substr(2);
            }

            switch (currentField)
            {
                case Field::Short:
                    if (!outEntry.m_Short.empty())
                        outEntry.m_Short += " ";
                    outEntry.m_Short += continuation;
                    break;
                case Field::Params:
                    if (!continuation.empty())
                    {
                        outEntry.m_Params.push_back(continuation);
                    }
                    break;
                case Field::Description:
                    if (!outEntry.m_Description.empty())
                        outEntry.m_Description += " ";
                    outEntry.m_Description += continuation;
                    break;
                case Field::Outputs:
                    if (!continuation.empty())
                    {
                        outEntry.m_Outputs.push_back(continuation);
                    }
                    break;
                case Field::None:
                    break;
            }
        }

        // Enforce @short as required
        if (outEntry.m_Short.empty())
        {
            LOG_CORE_WARN("Script '{}' has @jarvis-script marker but missing @short field — skipped", filePath.string());
            return false;
        }

        // Phase 3: scan remaining lines for top-level function definitions
        outEntry.m_ExportedFunctions.clear();
        bool const isPython = filePath.extension() == ".py";
        bool const isShell = filePath.extension() == ".sh";

        if (isPython)
        {
            // Scan for top-level "def funcname(" lines (no leading whitespace)
            while (std::getline(file, line))
            {
                if (line.size() >= 4 && line.compare(0, 4, "def ") == 0)
                {
                    size_t nameStart = 4;
                    size_t nameEnd = line.find('(', nameStart);
                    if (nameEnd != std::string::npos && nameEnd > nameStart)
                    {
                        std::string funcName = line.substr(nameStart, nameEnd - nameStart);
                        // Trim trailing whitespace from function name
                        size_t trimEnd = funcName.find_last_not_of(" \t");
                        if (trimEnd != std::string::npos)
                        {
                            funcName = funcName.substr(0, trimEnd + 1);
                        }
                        if (!funcName.empty())
                        {
                            outEntry.m_ExportedFunctions.push_back(funcName);
                        }
                    }
                }
            }
        }
        else if (isShell)
        {
            // Scan for top-level shell function definitions: "funcname()" or "function funcname"
            while (std::getline(file, line))
            {
                std::string trimmedLine = TrimLeft(line);
                if (trimmedLine.empty() || trimmedLine[0] == '#')
                    continue;

                // "function funcname" pattern
                if (trimmedLine.compare(0, 9, "function ") == 0)
                {
                    size_t nameStart = 9;
                    size_t nameEnd = trimmedLine.find_first_of(" \t({}", nameStart);
                    if (nameEnd == std::string::npos)
                        nameEnd = trimmedLine.size();
                    if (nameEnd > nameStart)
                    {
                        outEntry.m_ExportedFunctions.push_back(trimmedLine.substr(nameStart, nameEnd - nameStart));
                    }
                }
            }
        }

        return true;
    }

    // ----------------------------------------------------------------
    // ScanDirectory — scan all *.sh, *.py, *.ps1 files in scriptsDir
    // ----------------------------------------------------------------
    void ScriptRegistry::ScanDirectory(std::filesystem::path const& scriptsDir)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Entries.clear();

        if (!std::filesystem::exists(scriptsDir) || !std::filesystem::is_directory(scriptsDir))
        {
            LOG_CORE_WARN("ScriptRegistry: directory '{}' does not exist", scriptsDir.string());
            return;
        }

        std::error_code ec;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(scriptsDir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            if (!IsScriptExtension(entry.path()))
                continue;

            ScriptRegistryEntry registryEntry;
            if (ParseHeader(entry.path(), registryEntry))
            {
                m_Entries[registryEntry.m_FilePath] = std::move(registryEntry);
            }
        }

        LOG_CORE_INFO("ScriptRegistry: loaded {} registered scripts from '{}'", m_Entries.size(), scriptsDir.string());
    }

    // ----------------------------------------------------------------
    // AddOrUpdate — re-parse a single script file (file added/modified)
    // ----------------------------------------------------------------
    void ScriptRegistry::AddOrUpdate(std::filesystem::path const& filePath)
    {
        if (!IsScriptExtension(filePath))
        {
            return;
        }

        ScriptRegistryEntry entry;
        if (ParseHeader(filePath, entry))
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Entries[entry.m_FilePath] = std::move(entry);
            LOG_CORE_INFO("ScriptRegistry: registered '{}'", filePath.string());
        }
        else
        {
            // Marker removed or file no longer qualifies — remove from registry
            std::lock_guard<std::mutex> lock(m_Mutex);
            auto it = m_Entries.find(filePath.string());
            if (it != m_Entries.end())
            {
                m_Entries.erase(it);
                LOG_CORE_INFO("ScriptRegistry: unregistered '{}' (marker removed)", filePath.string());
            }
        }
    }

    // ----------------------------------------------------------------
    // Remove — remove a script from the registry (file deleted)
    // ----------------------------------------------------------------
    void ScriptRegistry::Remove(std::filesystem::path const& filePath)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Entries.find(filePath.string());
        if (it != m_Entries.end())
        {
            LOG_CORE_INFO("ScriptRegistry: removed '{}'", filePath.string());
            m_Entries.erase(it);
        }
    }

    // ----------------------------------------------------------------
    // SerializeMarkdownTable — for AI context injection (§11.6)
    // ----------------------------------------------------------------
    std::string ScriptRegistry::SerializeMarkdownTable() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_Entries.empty())
        {
            return "## Available Scripts\n\n(none)\n";
        }

        // Sort entries by file path for deterministic output
        std::vector<ScriptRegistryEntry const*> sorted;
        sorted.reserve(m_Entries.size());
        for (auto const& [key, entry] : m_Entries)
        {
            sorted.push_back(&entry);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](ScriptRegistryEntry const* a, ScriptRegistryEntry const* b) { return a->m_FilePath < b->m_FilePath; });

        std::ostringstream out;
        out << "## Available Scripts\n\n";
        out << "| Script | Short Description | Parameters |\n";
        out << "|--------|-------------------|------------|\n";

        for (auto const* entry : sorted)
        {
            out << "| `" << entry->m_FilePath << "` | " << entry->m_Short << " | ";

            if (entry->m_Params.empty())
            {
                out << "(none)";
            }
            else
            {
                for (size_t i = 0; i < entry->m_Params.size(); ++i)
                {
                    if (i > 0)
                        out << ", ";
                    out << "`" << entry->m_Params[i] << "`";
                }
            }

            out << " |\n";
        }

        return out.str();
    }

    // ----------------------------------------------------------------
    // GetEntries — return a snapshot of all entries (for REST API)
    // ----------------------------------------------------------------
    std::vector<ScriptRegistryEntry> ScriptRegistry::GetEntries() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::vector<ScriptRegistryEntry> result;
        result.reserve(m_Entries.size());
        for (auto const& [key, entry] : m_Entries)
        {
            result.push_back(entry);
        }
        return result;
    }

    size_t ScriptRegistry::Size() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Entries.size();
    }

    // ----------------------------------------------------------------
    // FindByModulePath — lookup by Python module path
    // e.g. "scripts.parseLog" -> finds entry with m_FilePath "scripts/parseLog.py"
    // ----------------------------------------------------------------
    ScriptRegistryEntry const* ScriptRegistry::FindByModulePath(std::string const& modulePath) const
    {
        // Convert module path to file path: dots -> slashes, append .py
        std::string filePath = modulePath;
        for (char& c : filePath)
        {
            if (c == '.')
            {
                c = '/';
            }
        }
        filePath += ".py";

        return FindByFilePath(filePath);
    }

    // ----------------------------------------------------------------
    // FindByFilePath — lookup by relative file path
    // e.g. "scripts/parseLog.py"
    // ----------------------------------------------------------------
    ScriptRegistryEntry const* ScriptRegistry::FindByFilePath(std::string const& filePath) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        // Direct match
        auto it = m_Entries.find(filePath);
        if (it != m_Entries.end())
        {
            return &it->second;
        }

        // Try suffix match (entries may have absolute or different-prefix paths)
        for (auto const& [key, entry] : m_Entries)
        {
            if (key.size() >= filePath.size())
            {
                size_t offset = key.size() - filePath.size();
                if (key.compare(offset, filePath.size(), filePath) == 0 && (offset == 0 || key[offset - 1] == '/'))
                {
                    return &entry;
                }
            }
        }

        return nullptr;
    }

} // namespace AIAssistant
