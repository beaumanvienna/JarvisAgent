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

        // Hard cap on catalog size — defends against a directory that has
        // grown out of bounds (or been weaponised with thousands of crafted
        // small files) from exhausting heap memory in the parsing thread.
        constexpr std::size_t kMaxEntries = 10'000;

        // Returns true if the canonicalised absolute path lies inside the
        // canonicalised root directory.  Resolves symlinks before comparing,
        // so a symlink under root/ that points outside root is rejected even
        // though its directory entry sits "inside" root from the iterator's
        // perspective.  Fails closed on any canonicalisation error.
        [[nodiscard]] bool PathIsUnderRoot(std::filesystem::path const& absolutePath,
                                            std::filesystem::path const& rootDir) noexcept
        {
            std::error_code rootError;
            std::filesystem::path const canonicalRoot = std::filesystem::weakly_canonical(rootDir, rootError);
            if (rootError || canonicalRoot.empty())
            {
                return false;
            }
            std::error_code pathError;
            std::filesystem::path const canonicalPath = std::filesystem::weakly_canonical(absolutePath, pathError);
            if (pathError || canonicalPath.empty())
            {
                return false;
            }
            // lexically_relative produces a leading ".." segment when the
            // candidate escapes the base — that's the canonical containment
            // signal in C++17/20.
            std::filesystem::path const rel = canonicalPath.lexically_relative(canonicalRoot);
            if (rel.empty())
            {
                return false;
            }
            auto const it = rel.begin();
            return it != rel.end() && *it != "..";
        }

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
            // scripts/<stem>.py → module "scripts.<stem>"; nested packages folded via '.'.
            // The Refresh-side filter excludes __init__.py before this point,
            // so no /__init__ stripping is needed here.
            auto rel = std::filesystem::relative(absolutePath, rootDir.parent_path());
            std::string mod = rel.generic_string();
            if (mod.size() > 3 && mod.substr(mod.size() - 3) == ".py")
            {
                mod = mod.substr(0, mod.size() - 3);
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
            else if (!currentTag.empty() && !stripped.empty() && stripped[0] != '@')
            {
                // Continuation line — belongs to the most recent tag.  The
                // previous check used `line[0] == '#'` after TrimRight, which
                // wrongly rejected indented continuations like "   # foo"
                // (the first char is space, not '#').  Test the stripped
                // form instead — that's the format the tag-handlers above
                // already operate on.
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
        // Build the new entry vector entirely OUTSIDE the lock so concurrent
        // List / GetByPath / GetByModule callers are not blocked for the full
        // I/O duration of the scan (which can be hundreds of milliseconds for
        // a large scripts directory).  Lock is acquired only for the swap.
        std::vector<Entry> newEntries;
        std::error_code iterError;
        // Drop the explicit exists() pre-check — race-prone, and the iterator
        // constructor already reports "no such directory" via its own ec
        // out-parameter.  Single atomic operation, single error path.
        std::filesystem::recursive_directory_iterator it(scriptsBaseFolder, iterError);
        if (iterError)
        {
            // Missing directory is the common case on a fresh checkout — keep
            // it quiet (TRACE).  Other errors (permission, I/O) deserve ERROR.
            if (iterError == std::errc::no_such_file_or_directory)
            {
                LOG_APP_TRACE("[scripts] root '{}' does not exist; catalog cleared",
                              scriptsBaseFolder.string());
            }
            else
            {
                LOG_APP_ERROR("[scripts] cannot open root '{}': {}",
                              scriptsBaseFolder.string(), iterError.message());
            }
            std::lock_guard const lock(m_Mutex);
            m_RootDirectory = scriptsBaseFolder;
            m_Entries.clear();
            return;
        }

        std::error_code stepError;
        std::filesystem::recursive_directory_iterator const end;
        for (; it != end; it.increment(stepError))
        {
            if (stepError)
            {
                LOG_APP_ERROR("[scripts] directory scan stepped on error under '{}': {}; "
                              "returning partial catalog",
                              scriptsBaseFolder.string(), stepError.message());
                break;
            }
            if (newEntries.size() >= kMaxEntries)
            {
                LOG_APP_ERROR("[scripts] catalog cap {} reached under '{}'; remaining entries ignored",
                              kMaxEntries, scriptsBaseFolder.string());
                break;
            }
            auto const& entry = *it;
            // Skip symlinks — their target may resolve outside the scripts
            // root.  PathIsUnderRoot below catches this defensively even if
            // a future refactor relaxes the symlink skip; both gates run.
            if (entry.is_symlink()) continue;
            if (!entry.is_regular_file()) continue;
            std::string const ext = entry.path().extension().string();
            if (ext != ".sh" && ext != ".py" && ext != ".ps1") continue;
            // Python package markers — `__init__.py` describes the package,
            // not a standalone module.  The corresponding stripping branch
            // in ParseFile is dead code now that the exclusion lives here.
            if (entry.path().filename() == "__init__.py") continue;
            // Path-traversal gate: reject any entry whose canonical path
            // does not lie under the canonical scripts root.  Defends
            // against a symlink-bait race we missed and against ANY future
            // relaxation of the symlink skip above.
            if (!PathIsUnderRoot(entry.path(), scriptsBaseFolder))
            {
                LOG_APP_ERROR("[scripts] rejecting entry '{}': resolves outside scripts root '{}'",
                              entry.path().string(), scriptsBaseFolder.string());
                continue;
            }

            newEntries.push_back(ParseFile(entry.path(), scriptsBaseFolder));
        }

        std::sort(newEntries.begin(), newEntries.end(),
                  [](Entry const& a, Entry const& b) { return a.m_Path < b.m_Path; });

        std::size_t const finalCount = newEntries.size();
        {
            std::lock_guard const lock(m_Mutex);
            m_RootDirectory = scriptsBaseFolder;
            m_Entries = std::move(newEntries);
        }
        LOG_APP_INFO("[scripts] catalog refreshed: {} entries under '{}'",
                     finalCount, scriptsBaseFolder.string());
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
