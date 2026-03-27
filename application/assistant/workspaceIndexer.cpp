/* Copyright (c) 2025 JC TechnoLabs

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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "assistant/workspaceIndexer.h"

#include "engine.h"
#include "simdjson/simdjson.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace AIAssistant
{
    // -----------------------------------------------------------------
    // JSON string escaping (for manual serialization)
    // -----------------------------------------------------------------

    static std::string JsonEscapeIdx(std::string const& input)
    {
        std::string out;
        out.reserve(input.size() + 16);
        for (char c : input)
        {
            switch (c)
            {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += c;
                    break;
            }
        }
        return out;
    }

    // -----------------------------------------------------------------
    // Extensions we index
    // -----------------------------------------------------------------

    static std::unordered_set<std::string> const kIndexableExtensions = {
        ".h",    ".cpp",  ".c",   ".hpp",  ".ts",   ".tsx", ".js",  ".jsx",
        ".py",   ".lua",  ".sh",  ".md",   ".jcwf", ".json", ".css", ".html",
    };

    // Directories to scan (relative to workspace root).
    static std::vector<std::pair<std::string, int>> const kScanDirs = {
        {"application", 10},
        {"engine", 10},
        {"workflow-editor/ui/src", 10},
        {"scripts", 5},
        {"workflows", 5},
    };

    // Directories to skip during scan.
    static std::unordered_set<std::string> const kSkipDirNames = {
        "node_modules", ".git", "bin", "bin-int", "vendor", "__pycache__", ".cache",
    };

    // -----------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------

    WorkspaceIndexer::WorkspaceIndexer(fs::path indexDir) : m_IndexDir(std::move(indexDir))
    {
        m_IndexPath = m_IndexDir / "file_index.jsonl";
    }

    // -----------------------------------------------------------------
    // ScanWorkspace
    // -----------------------------------------------------------------

    void WorkspaceIndexer::ScanWorkspace()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        // Load existing index to preserve cached summaries.
        LoadIndex();

        // Build set of paths we'll see during scan.
        std::unordered_set<std::string> seenPaths;

        // Save old index for summary preservation.
        auto oldPathToIndex = m_PathToIndex;
        auto oldEntries = m_Entries;

        m_Entries.clear();
        m_PathToIndex.clear();

        for (auto const& [dir, maxDepth] : kScanDirs)
        {
            fs::path dirPath = fs::path(dir);
            std::error_code ec;
            if (fs::exists(dirPath, ec) && fs::is_directory(dirPath, ec))
            {
                ScanDirectory(dirPath, maxDepth);
            }
        }

        // Also index top-level files (premake5.lua, Makefile, ai-assistant-plan.md, etc.)
        {
            std::error_code ec;
            for (auto const& entry : fs::directory_iterator(".", ec))
            {
                if (!entry.is_regular_file(ec))
                    continue;
                std::string ext = entry.path().extension().string();
                if (!IsIndexableExtension(ext))
                    continue;

                std::string relPath = entry.path().lexically_normal().string();
                // Remove leading "./" if present.
                if (relPath.size() > 2 && relPath[0] == '.' && relPath[1] == '/')
                    relPath = relPath.substr(2);

                if (m_PathToIndex.count(relPath))
                    continue;

                auto fileSize = fs::file_size(entry.path(), ec);
                auto mtime = fs::last_write_time(entry.path(), ec);
                int64_t mtimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();

                FileIndexEntry fie;
                fie.relativePath = relPath;
                fie.extension = ext;
                fie.sizeBytes = fileSize;
                fie.mtimeNs = mtimeNs;

                // Preserve summary from old index if file hasn't changed.
                auto oldIt = oldPathToIndex.find(relPath);
                if (oldIt != oldPathToIndex.end())
                {
                    auto const& old = oldEntries[oldIt->second];
                    if (old.summaryMtimeNs == mtimeNs && !old.summary.empty())
                    {
                        fie.summary = old.summary;
                        fie.summaryMtimeNs = old.summaryMtimeNs;
                    }
                }

                m_PathToIndex[relPath] = m_Entries.size();
                m_Entries.push_back(std::move(fie));
            }
        }

        // Rebuild PathToIndex (ScanDirectory already populated it, but let's be safe).
        m_PathToIndex.clear();
        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            m_PathToIndex[m_Entries[i].relativePath] = i;
        }

        // Preserve summaries from old index for files that haven't changed.
        for (auto const& [oldPath, oldIdx] : oldPathToIndex)
        {
            auto newIt = m_PathToIndex.find(oldPath);
            if (newIt == m_PathToIndex.end())
                continue;

            auto& newEntry = m_Entries[newIt->second];
            auto const& oldEntry = oldEntries[oldIdx];

            if (newEntry.summary.empty() && !oldEntry.summary.empty() && oldEntry.summaryMtimeNs == newEntry.mtimeNs)
            {
                newEntry.summary = oldEntry.summary;
                newEntry.summaryMtimeNs = oldEntry.summaryMtimeNs;
            }
        }

        SaveIndex();
        m_LastScanTime = std::chrono::steady_clock::now();

        size_t summaryCount = 0;
        for (auto const& e : m_Entries)
        {
            if (!e.summary.empty())
                ++summaryCount;
        }

        LOG_APP_INFO("[indexer] Workspace scan complete: {} files indexed, {} cached summaries", m_Entries.size(),
                     summaryCount);
    }

    // -----------------------------------------------------------------
    // ScanDirectory — recursive
    // -----------------------------------------------------------------

    void WorkspaceIndexer::ScanDirectory(fs::path const& dir, int maxDepth)
    {
        if (maxDepth <= 0)
            return;

        std::error_code ec;
        for (auto const& entry : fs::directory_iterator(dir, ec))
        {
            if (entry.is_directory(ec))
            {
                std::string dirName = entry.path().filename().string();
                if (kSkipDirNames.count(dirName))
                    continue;
                ScanDirectory(entry.path(), maxDepth - 1);
            }
            else if (entry.is_regular_file(ec))
            {
                std::string ext = entry.path().extension().string();
                if (!IsIndexableExtension(ext))
                    continue;

                std::string relPath = entry.path().lexically_normal().string();
                // Remove leading "./" if present.
                if (relPath.size() > 2 && relPath[0] == '.' && relPath[1] == '/')
                    relPath = relPath.substr(2);

                if (m_PathToIndex.count(relPath))
                    continue;

                auto fileSize = fs::file_size(entry.path(), ec);
                auto mtime = fs::last_write_time(entry.path(), ec);
                int64_t mtimeNs =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();

                FileIndexEntry fie;
                fie.relativePath = relPath;
                fie.extension = ext;
                fie.sizeBytes = fileSize;
                fie.mtimeNs = mtimeNs;

                m_PathToIndex[relPath] = m_Entries.size();
                m_Entries.push_back(std::move(fie));
            }
        }
    }

    bool WorkspaceIndexer::IsIndexableExtension(std::string const& ext) const
    {
        return kIndexableExtensions.count(ext) > 0;
    }

    // -----------------------------------------------------------------
    // Summary access
    // -----------------------------------------------------------------

    std::string WorkspaceIndexer::GetFileSummary(std::string const& relativePath) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_PathToIndex.find(relativePath);
        if (it == m_PathToIndex.end())
            return {};

        auto const& entry = m_Entries[it->second];
        // Summary is stale if file has been modified since it was generated.
        if (entry.summaryMtimeNs != entry.mtimeNs)
            return {};
        return entry.summary;
    }

    void WorkspaceIndexer::SetFileSummary(std::string const& relativePath, std::string const& summary)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_PathToIndex.find(relativePath);
        if (it == m_PathToIndex.end())
            return;

        auto& entry = m_Entries[it->second];
        entry.summary = summary;
        entry.summaryMtimeNs = entry.mtimeNs;
        SaveIndex();
        LOG_APP_INFO("[indexer] Cached summary for '{}' ({} chars)", relativePath, summary.size());
    }

    std::string WorkspaceIndexer::ReadFileContent(std::string const& relativePath, size_t maxBytes)
    {
        fs::path filePath = fs::path(relativePath).lexically_normal();

        // Security: block paths that escape the workspace.
        std::string normalized = filePath.string();
        if (normalized.find("..") != std::string::npos)
            return {};
        if (normalized.starts_with("/"))
            return {};

        std::error_code ec;
        if (!fs::exists(filePath, ec) || !fs::is_regular_file(filePath, ec))
            return {};

        auto fileSize = fs::file_size(filePath, ec);
        if (fileSize > maxBytes)
            fileSize = maxBytes;

        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs)
            return {};

        std::string content(fileSize, '\0');
        ifs.read(content.data(), static_cast<std::streamsize>(fileSize));
        content.resize(static_cast<size_t>(ifs.gcount()));

        if (fileSize < fs::file_size(filePath, ec))
        {
            content += "\n... [truncated at " + std::to_string(maxBytes) + " bytes]";
        }

        return content;
    }

    // -----------------------------------------------------------------
    // Relevance search
    // -----------------------------------------------------------------

    std::vector<FileIndexEntry> WorkspaceIndexer::GetRelevantFiles(std::string const& query, int maxResults) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (query.empty())
            return {};

        // Tokenize query into lowercase words, stripping punctuation.
        std::vector<std::string> queryWords;
        {
            std::istringstream iss(query);
            std::string raw;
            while (iss >> raw)
            {
                std::string word;
                word.reserve(raw.size());
                for (char c : raw)
                {
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')
                        word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (word.size() >= 2)
                    queryWords.push_back(word);
            }
        }

        if (queryWords.empty())
            return {};

        // Score each entry that has a summary.
        struct Scored
        {
            int score;
            FileIndexEntry const* entry;
        };
        std::vector<Scored> scored;

        for (auto const& entry : m_Entries)
        {
            if (entry.summary.empty())
                continue;
            int s = ScoreMatch(entry, queryWords);
            if (s > 0)
                scored.push_back({s, &entry});
        }

        std::sort(scored.begin(), scored.end(), [](auto const& a, auto const& b) { return a.score > b.score; });

        std::vector<FileIndexEntry> results;
        int count = std::min(maxResults, static_cast<int>(scored.size()));
        results.reserve(count);
        for (int i = 0; i < count; ++i)
            results.push_back(*scored[i].entry);

        return results;
    }

    int WorkspaceIndexer::ScoreMatch(FileIndexEntry const& entry, std::vector<std::string> const& queryWords)
    {
        // Build haystack from path + summary.
        std::string haystack;
        haystack.reserve(entry.relativePath.size() + entry.summary.size() + 32);

        // Path gets extra weight (included twice).
        haystack += entry.relativePath + " " + entry.relativePath + " ";
        haystack += entry.summary + " ";

        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        int score = 0;
        for (auto const& word : queryWords)
        {
            size_t pos = 0;
            while ((pos = haystack.find(word, pos)) != std::string::npos)
            {
                ++score;
                pos += word.size();
            }
        }
        return score;
    }

    // -----------------------------------------------------------------
    // Stats
    // -----------------------------------------------------------------

    size_t WorkspaceIndexer::FileCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Entries.size();
    }

    size_t WorkspaceIndexer::SummaryCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        size_t count = 0;
        for (auto const& e : m_Entries)
        {
            if (!e.summary.empty())
                ++count;
        }
        return count;
    }

    std::chrono::steady_clock::time_point WorkspaceIndexer::LastScanTime() const { return m_LastScanTime; }

    std::vector<FileIndexEntry> WorkspaceIndexer::GetAllEntries() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Entries;
    }

    // -----------------------------------------------------------------
    // Persistence — JSONL format
    // -----------------------------------------------------------------

    void WorkspaceIndexer::LoadIndex()
    {
        m_Entries.clear();
        m_PathToIndex.clear();

        std::error_code ec;
        if (!fs::exists(m_IndexPath, ec))
            return;

        std::ifstream ifs(m_IndexPath);
        if (!ifs)
        {
            LOG_APP_WARN("[indexer] Failed to open index file: {}", m_IndexPath.string());
            return;
        }

        simdjson::ondemand::parser parser;
        std::string line;
        while (std::getline(ifs, line))
        {
            if (line.empty())
                continue;

            simdjson::padded_string padded(line);
            simdjson::ondemand::document doc;
            auto err = parser.iterate(padded).get(doc);
            if (err)
                continue;

            simdjson::ondemand::object obj;
            if (doc.get_object().get(obj))
                continue;

            FileIndexEntry entry;
            std::string_view sv;

            if (!obj["path"].get_string().get(sv))
                entry.relativePath = std::string(sv);
            if (!obj["ext"].get_string().get(sv))
                entry.extension = std::string(sv);

            uint64_t u64;
            if (!obj["size"].get_uint64().get(u64))
                entry.sizeBytes = u64;

            int64_t i64;
            if (!obj["mtime"].get_int64().get(i64))
                entry.mtimeNs = i64;

            if (!obj["summary"].get_string().get(sv))
                entry.summary = std::string(sv);

            if (!obj["summary_mtime"].get_int64().get(i64))
                entry.summaryMtimeNs = i64;

            if (!entry.relativePath.empty())
            {
                m_PathToIndex[entry.relativePath] = m_Entries.size();
                m_Entries.push_back(std::move(entry));
            }
        }

        LOG_APP_INFO("[indexer] Loaded {} entries from index", m_Entries.size());
    }

    void WorkspaceIndexer::SaveIndex() const
    {
        // Ensure directory exists.
        std::error_code ec;
        fs::create_directories(m_IndexDir, ec);

        std::ofstream ofs(m_IndexPath, std::ios::out | std::ios::trunc);
        if (!ofs)
        {
            LOG_APP_WARN("[indexer] Failed to write index file: {}", m_IndexPath.string());
            return;
        }

        for (auto const& e : m_Entries)
        {
            ofs << "{\"path\":\"" << JsonEscapeIdx(e.relativePath) << "\",\"ext\":\"" << JsonEscapeIdx(e.extension)
                << "\",\"size\":" << e.sizeBytes << ",\"mtime\":" << e.mtimeNs << ",\"summary\":\""
                << JsonEscapeIdx(e.summary) << "\",\"summary_mtime\":" << e.summaryMtimeNs << "}\n";
        }
    }

} // namespace AIAssistant
