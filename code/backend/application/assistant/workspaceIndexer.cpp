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

#include "auxiliary/file.h"
#include "engine.h"
#include "json/jsonHelper.h"
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
    // Extensions / scan config
    // -----------------------------------------------------------------

    static std::unordered_set<std::string> const kIndexableExtensions = {
        ".h",  ".cpp",  ".c",   ".hpp",  ".ts",   ".tsx", ".js",  ".jsx",
        ".py", ".lua",  ".sh",  ".md",   ".jcwf", ".json", ".css", ".html",
    };

    static std::vector<std::pair<std::string, int>> const kScanDirs = {
        {"application", 10},
        {"engine", 10},
        {"code/frontend/workflow-editor/ui/src", 10},
        {"scripts", 5},
        {"workflows", 5},
    };

    static std::unordered_set<std::string> const kSkipDirNames = {
        "node_modules", ".git", "bin", "bin-int", "vendor", "__pycache__", ".cache",
    };

    // -----------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------

    WorkspaceIndexer::WorkspaceIndexer(fs::path indexDir) : m_IndexDir(std::move(indexDir))
    {
        m_IndexPath = m_IndexDir / "file_index.jsonl";

        // Capture the workspace root at construction (typically right after main()
        // has set cwd to the project root).  All later path resolution anchors
        // against this snapshot, so a future cwd change cannot widen access.
        std::error_code ec;
        m_WorkspaceRoot = fs::weakly_canonical(fs::current_path(ec), ec);
        if (ec)
        {
            // Fall through with an empty root — every ResolveAndConfine will then
            // fail closed.  Real production j9t will never hit this branch.
            LOG_APP_ERROR("[indexer] Failed to capture workspace root: {}", ec.message());
            m_WorkspaceRoot.clear();
        }
    }

    // -----------------------------------------------------------------
    // Path confinement
    // -----------------------------------------------------------------

    fs::path WorkspaceIndexer::ResolveAndConfine(std::string const& relativePath) const
    {
        if (relativePath.empty() || m_WorkspaceRoot.empty())
            return {};
        // Reject absolute paths up front — even if they happen to resolve under the
        // workspace root, callers using this method are passing relative paths by contract.
        fs::path raw(relativePath);
        if (raw.is_absolute())
            return {};

        std::error_code ec;
        fs::path const resolved = fs::weakly_canonical(m_WorkspaceRoot / raw, ec);
        if (ec || resolved.empty())
            return {};

        // Containment check: lexically_relative returns "..", "../foo", or empty
        // if `resolved` does not live under m_WorkspaceRoot.
        fs::path const rel = resolved.lexically_relative(m_WorkspaceRoot);
        std::string const relGeneric = rel.generic_string();
        if (rel.empty() || relGeneric == ".." || relGeneric.rfind("../", 0) == 0)
            return {};
        return resolved;
    }

    // -----------------------------------------------------------------
    // ScanWorkspace
    // -----------------------------------------------------------------

    void WorkspaceIndexer::ScanWorkspace()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        // Load existing index to preserve cached summaries.
        LoadIndex();

        // Snapshot the loaded state for summary preservation, then clear the
        // working set so ScanDirectory rebuilds from the live filesystem.
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
                if (relPath.size() > 2 && relPath[0] == '.' && relPath[1] == '/')
                    relPath = relPath.substr(2);

                if (m_PathToIndex.count(relPath))
                    continue;

                auto fileSize = fs::file_size(entry.path(), ec);
                if (ec)
                    continue;
                auto mtime = fs::last_write_time(entry.path(), ec);
                if (ec)
                    continue;
                int64_t mtimeNs =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();

                FileIndexEntry fie;
                fie.relativePath = relPath;
                fie.extension = ext;
                fie.sizeBytes = fileSize;
                fie.mtimeNs = mtimeNs;

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

        // Rebuild m_PathToIndex once at the end (ScanDirectory builds it incrementally
        // for duplicate detection during the walk; the rebuild ensures the final state
        // matches the final m_Entries layout).
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
        if (m_Entries.size() >= kMaxIndexEntries)
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
                if (relPath.size() > 2 && relPath[0] == '.' && relPath[1] == '/')
                    relPath = relPath.substr(2);

                if (m_PathToIndex.count(relPath))
                    continue;

                std::error_code ecFile;
                auto fileSize = fs::file_size(entry.path(), ecFile);
                if (ecFile)
                    continue;
                auto mtime = fs::last_write_time(entry.path(), ecFile);
                if (ecFile)
                    continue;
                int64_t mtimeNs =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();

                FileIndexEntry fie;
                fie.relativePath = relPath;
                fie.extension = ext;
                fie.sizeBytes = fileSize;
                fie.mtimeNs = mtimeNs;

                m_PathToIndex[relPath] = m_Entries.size();
                m_Entries.push_back(std::move(fie));
                if (m_Entries.size() >= kMaxIndexEntries)
                {
                    LOG_APP_ERROR("[indexer] Index entry cap ({}) reached during scan — truncating",
                                  kMaxIndexEntries);
                    return;
                }
            }
        }
    }

    bool WorkspaceIndexer::IsIndexableExtension(std::string const& ext)
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

        auto const& entry = m_Entries.at(it->second);
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

        auto& entry = m_Entries.at(it->second);
        // Cap the cached summary so a runaway provider response (or a prompt-injected
        // mega-summary) can't bloat the index file unboundedly.
        if (summary.size() > kMaxSummaryBytes)
            entry.summary = summary.substr(0, kMaxSummaryBytes);
        else
            entry.summary = summary;
        entry.summaryMtimeNs = entry.mtimeNs;
        SaveIndex();
        LOG_APP_INFO("[indexer] Cached summary for '{}' ({} chars)", relativePath, entry.summary.size());
    }

    std::string WorkspaceIndexer::ReadFileContent(std::string const& relativePath, size_t maxBytes) const
    {
        // Anchor against the captured workspace root.  `..` traversal, absolute paths,
        // and symlink targets outside the project root all resolve to empty here.
        fs::path const filePath = ResolveAndConfine(relativePath);
        if (filePath.empty())
            return {};

        // Drop the redundant exists/is_regular_file pre-check — the open below either
        // succeeds (regular file or symlink to one we've already confined) or fails.
        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs)
            return {};

        // Get the original size before clamping so the truncation marker reflects
        // the *original* file vs. the read window — not the post-clamp value
        // compared against itself (the prior bug).
        std::error_code ec;
        auto const originalSize = fs::file_size(filePath, ec);
        if (ec)
            return {};
        size_t const readSize = static_cast<size_t>(std::min<uintmax_t>(originalSize, maxBytes));

        std::string content(readSize, '\0');
        ifs.read(content.data(), static_cast<std::streamsize>(readSize));
        content.resize(static_cast<size_t>(ifs.gcount()));

        if (originalSize > maxBytes)
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

        // Score by index — never store raw pointers into m_Entries.  The lock is held
        // for the whole call, but storing indices removes the lifetime hazard so the
        // pattern survives a future refactor that drops the lock early.
        struct Scored
        {
            int score;
            size_t idx;
        };
        std::vector<Scored> scored;

        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            auto const& entry = m_Entries[i];
            if (entry.summary.empty())
                continue;
            int s = ScoreMatch(entry, queryWords);
            if (s > 0)
                scored.push_back({s, i});
        }

        std::sort(scored.begin(), scored.end(),
                  [](Scored const& a, Scored const& b) { return a.score > b.score; });

        std::vector<FileIndexEntry> results;
        int count = std::min(maxResults, static_cast<int>(scored.size()));
        results.reserve(count);
        for (int i = 0; i < count; ++i)
            results.push_back(m_Entries[scored[i].idx]);

        return results;
    }

    int WorkspaceIndexer::ScoreMatch(FileIndexEntry const& entry, std::vector<std::string> const& queryWords)
    {
        std::string haystack;
        haystack.reserve(entry.relativePath.size() + entry.summary.size() + 32);

        haystack += entry.relativePath + " " + entry.relativePath + " ";
        haystack += entry.summary + " ";

        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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

    std::chrono::steady_clock::time_point WorkspaceIndexer::LastScanTime() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_LastScanTime;
    }

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

        // Open without exists() pre-check.  Distinguish missing-file (silent first run)
        // from present-but-unreadable (genuine I/O error).
        std::ifstream ifs(m_IndexPath);
        if (!ifs)
        {
            std::error_code ec;
            if (fs::exists(m_IndexPath, ec))
                LOG_APP_ERROR("[indexer] Index file present but unreadable: {}", m_IndexPath.string());
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

            // Each std::string(sv) below copies out of `padded`'s buffer immediately;
            // the view is invalid past this iteration scope, so do not store sv anywhere.
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
            {
                std::string s(sv);
                if (s.size() > kMaxSummaryBytes)
                    s.resize(kMaxSummaryBytes);
                entry.summary = std::move(s);
            }

            if (!obj["summary_mtime"].get_int64().get(i64))
                entry.summaryMtimeNs = i64;

            if (entry.relativePath.empty())
                continue;

            // Re-validate the path against the workspace root before trusting it.
            // An attacker who can write to file_index.jsonl could otherwise inject
            // `../../etc/shadow` and have the AI tool happily summarise it later.
            if (ResolveAndConfine(entry.relativePath).empty())
            {
                LOG_SECURITY_WARN("[security] indexer_index_path_escape len={}", entry.relativePath.size());
                continue;
            }

            m_PathToIndex[entry.relativePath] = m_Entries.size();
            m_Entries.push_back(std::move(entry));
            if (m_Entries.size() >= kMaxIndexEntries)
            {
                LOG_APP_ERROR("[indexer] Index entry cap ({}) reached during load — truncating", kMaxIndexEntries);
                break;
            }
        }

        LOG_APP_INFO("[indexer] Loaded {} entries from index", m_Entries.size());
    }

    void WorkspaceIndexer::SaveIndex() const
    {
        std::ostringstream body;
        for (auto const& e : m_Entries)
        {
            body << "{\"path\":\"" << JsonHelper::EscapeJsonString(e.relativePath) << "\",\"ext\":\""
                 << JsonHelper::EscapeJsonString(e.extension) << "\",\"size\":" << e.sizeBytes
                 << ",\"mtime\":" << e.mtimeNs << ",\"summary\":\""
                 << JsonHelper::EscapeJsonString(e.summary) << "\",\"summary_mtime\":" << e.summaryMtimeNs << "}\n";
        }

        std::string writeError;
        if (!EngineCore::AtomicWriteFile(m_IndexPath, body.str(), writeError))
        {
            LOG_APP_ERROR("[indexer] Index file write failed: {} path='{}'", writeError, m_IndexPath.string());
        }
    }

} // namespace AIAssistant
