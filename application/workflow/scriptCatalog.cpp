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

#include "workflow/scriptCatalog.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "engine.h"

namespace AIAssistant
{
    namespace
    {
        constexpr int kMaxMetadataLines = 60;

        // Strip leading whitespace + optional comment chars (#, --) so we can
        // match `# @short: foo` regardless of formatting style.
        std::string StripCommentPrefix(std::string const& line)
        {
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i < line.size() && line[i] == '#') { ++i; }
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            return line.substr(i);
        }

        std::string TrimRight(std::string s)
        {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            {
                s.pop_back();
            }
            return s;
        }

        bool StartsWith(std::string const& haystack, std::string_view needle)
        {
            return haystack.size() >= needle.size() &&
                   std::equal(needle.begin(), needle.end(), haystack.begin());
        }

        std::vector<std::string> SplitParams(std::string const& value)
        {
            std::vector<std::string> out;
            std::string current;
            for (char ch : value)
            {
                if (ch == ',' || ch == ' ' || ch == '\t')
                {
                    if (!current.empty()) { out.push_back(current); current.clear(); }
                }
                else
                {
                    current.push_back(ch);
                }
            }
            if (!current.empty()) out.push_back(current);
            return out;
        }

        bool IsExecutable(std::filesystem::path const& p)
        {
            std::error_code ec;
            auto s = std::filesystem::status(p, ec);
            if (ec) return false;
            auto const perms = s.permissions();
            return (perms & (std::filesystem::perms::owner_exec |
                             std::filesystem::perms::group_exec |
                             std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
        }
    } // namespace

    ScriptCatalog::Entry ScriptCatalog::ParseFile(std::filesystem::path const& absolutePath,
                                                  std::filesystem::path const& rootDir)
    {
        Entry entry;
        entry.m_Executable = IsExecutable(absolutePath);

        std::string const ext = absolutePath.extension().string();
        if (ext == ".py")
        {
            entry.m_Type = "python";
            // scripts/<stem>.py → module "scripts.<stem>"; nested packages folded via '.'
            auto rel = std::filesystem::relative(absolutePath, rootDir.parent_path());
            std::string mod = rel.generic_string();
            // Trim ".py"
            if (mod.size() > 3 && mod.substr(mod.size() - 3) == ".py")
            {
                mod = mod.substr(0, mod.size() - 3);
            }
            // __init__ files describe the package, not a standalone module.
            if (mod.size() > 9 && mod.substr(mod.size() - 9) == "/__init__")
            {
                mod = mod.substr(0, mod.size() - 9);
            }
            std::replace(mod.begin(), mod.end(), '/', '.');
            entry.m_Module = mod;
        }
        else
        {
            entry.m_Type = "shell";
        }

        // Store path relative to the launch CWD so JCWF references can use the
        // same string the scripts/ security boundary check expects.
        auto rel = std::filesystem::relative(absolutePath, rootDir.parent_path());
        entry.m_Path = rel.generic_string();

        std::ifstream is(absolutePath);
        if (!is) return entry;

        std::string line;
        std::string currentTag;           // "description" while we're accumulating continuation lines
        std::string descriptionBuffer;
        int lineNo = 0;
        while (lineNo < kMaxMetadataLines && std::getline(is, line))
        {
            ++lineNo;
            line = TrimRight(line);
            if (lineNo == 1 && StartsWith(line, "#!"))
            {
                entry.m_HasShebang = true;
                continue;
            }

            std::string const stripped = StripCommentPrefix(line);
            if (stripped.empty())
            {
                // Blank comment separator — end of any active continuation.
                currentTag.clear();
                continue;
            }

            if (stripped == "@jarvis-script" || StartsWith(stripped, "@jarvis-script "))
            {
                entry.m_HasJarvisMarker = true;
                currentTag.clear();
                continue;
            }

            auto const startsWithTag = [&](std::string_view tag) {
                return StartsWith(stripped, tag);
            };

            if (startsWithTag("@short:"))
            {
                currentTag = "short";
                std::string value = stripped.substr(std::string("@short:").size());
                if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                entry.m_Short = value;
            }
            else if (startsWithTag("@params:"))
            {
                currentTag = "params";
                std::string value = stripped.substr(std::string("@params:").size());
                if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                entry.m_Params = SplitParams(value);
            }
            else if (startsWithTag("@description:"))
            {
                currentTag = "description";
                std::string value = stripped.substr(std::string("@description:").size());
                if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                descriptionBuffer = value;
            }
            else if (startsWithTag("@outputs:"))
            {
                currentTag = "outputs";
                std::string value = stripped.substr(std::string("@outputs:").size());
                if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                entry.m_Outputs = value;
            }
            else if (!currentTag.empty() && (line.empty() || line[0] == '#'))
            {
                // Continuation line — belongs to the most recent tag.
                if (currentTag == "description")
                {
                    if (!descriptionBuffer.empty()) descriptionBuffer.push_back(' ');
                    descriptionBuffer += stripped;
                }
                else if (currentTag == "outputs")
                {
                    if (!entry.m_Outputs.empty()) entry.m_Outputs.push_back(' ');
                    entry.m_Outputs += stripped;
                }
                // short + params are single-line by convention.
            }
            else
            {
                currentTag.clear();
            }
        }

        entry.m_Description = descriptionBuffer;
        return entry;
    }

    void ScriptCatalog::Refresh(std::filesystem::path const& scriptsBaseFolder)
    {
        std::lock_guard lock(m_Mutex);
        m_RootDirectory = scriptsBaseFolder;
        m_Entries.clear();

        std::error_code ec;
        if (!std::filesystem::exists(m_RootDirectory, ec)) return;

        for (auto const& e : std::filesystem::recursive_directory_iterator(m_RootDirectory, ec))
        {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            std::string const ext = e.path().extension().string();
            if (ext != ".sh" && ext != ".py" && ext != ".ps1") continue;
            // Skip python package markers — the package itself is the module
            // and a standalone __init__ rarely carries jarvis-script metadata.
            if (e.path().filename() == "__init__.py") continue;

            Entry entry = ParseFile(e.path(), m_RootDirectory);
            m_Entries.push_back(std::move(entry));
        }

        std::sort(m_Entries.begin(), m_Entries.end(),
                  [](Entry const& a, Entry const& b) { return a.m_Path < b.m_Path; });

        LOG_APP_INFO("[scripts] catalog refreshed: {} entries under '{}'",
                     m_Entries.size(), m_RootDirectory.string());
    }

    std::vector<ScriptCatalog::Entry>
    ScriptCatalog::List(std::string const& typeFilter) const
    {
        std::lock_guard lock(m_Mutex);
        if (typeFilter.empty()) return m_Entries;
        std::vector<Entry> filtered;
        for (auto const& e : m_Entries)
        {
            if (e.m_Type == typeFilter) filtered.push_back(e);
        }
        return filtered;
    }

    std::optional<ScriptCatalog::Entry>
    ScriptCatalog::GetByPath(std::string const& relativePath) const
    {
        std::lock_guard lock(m_Mutex);
        for (auto const& e : m_Entries)
        {
            if (e.m_Path == relativePath) return e;
        }
        return std::nullopt;
    }

    std::optional<ScriptCatalog::Entry>
    ScriptCatalog::GetByModule(std::string const& moduleName) const
    {
        std::lock_guard lock(m_Mutex);
        for (auto const& e : m_Entries)
        {
            if (e.m_Module == moduleName) return e;
        }
        return std::nullopt;
    }

    std::filesystem::path ScriptCatalog::GetRootDirectory() const
    {
        std::lock_guard lock(m_Mutex);
        return m_RootDirectory;
    }

    size_t ScriptCatalog::Size() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Entries.size();
    }
} // namespace AIAssistant
