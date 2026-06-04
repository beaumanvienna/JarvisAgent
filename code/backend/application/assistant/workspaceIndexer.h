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

#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    // A single indexed file in the workspace.
    struct FileIndexEntry
    {
        std::string relativePath; // relative to workspace root
        std::string extension;    // e.g. ".cpp", ".h", ".ts"
        uint64_t sizeBytes{0};
        int64_t mtimeNs{0};        // filesystem modification time (ns since epoch)
        std::string summary;       // AI-generated summary (empty if not yet generated)
        int64_t summaryMtimeNs{0}; // mtime when summary was generated (0 = no summary)
    };

    // Indexes source files in the workspace for fast lookup and summary caching.
    //
    // On startup, scans configured directories and writes `assistant/index/file_index.jsonl`.
    // Subsequent startups load the existing index and only re-scan changed files (by mtime).
    // AI-generated summaries are cached per-file and invalidated when the file changes.
    //
    // Thread-safe: all public methods acquire m_Mutex once and hold it for the whole operation.
    class WorkspaceIndexer
    {
    public:
        // Captures the workspace root from `fs::current_path()` at construction time.
        // Every path-bearing operation anchors against this root via weakly_canonical;
        // changes to cwd after construction do NOT shift the security boundary.
        explicit WorkspaceIndexer(std::filesystem::path indexDir);

        WorkspaceIndexer(WorkspaceIndexer const&) = delete;
        WorkspaceIndexer& operator=(WorkspaceIndexer const&) = delete;
        WorkspaceIndexer(WorkspaceIndexer&&) = delete;
        WorkspaceIndexer& operator=(WorkspaceIndexer&&) = delete;

        // Full workspace scan.  Loads existing index first, then scans directories
        // and adds/updates/removes entries as needed.
        void ScanWorkspace();

        // Get the cached summary for a file (empty string if not cached or stale).
        [[nodiscard]] std::string GetFileSummary(std::string const& relativePath) const;

        // Store an AI-generated summary for a file.  The summary is clamped to
        // kMaxSummaryBytes; oversized inputs are truncated rather than rejected.
        void SetFileSummary(std::string const& relativePath, std::string const& summary);

        // Read file content (for summarization by the AI tool).
        // Returns empty string on error or if the path resolves outside the workspace
        // root captured at construction (defends against `..` traversal, absolute paths,
        // and symlinks pointing out of tree).  Note: this is NOT a security gate against
        // sensitive files — callers (ExecGetFileSummary etc.) must additionally apply
        // ToolRegistry::IsPathDenied for that.  This method only enforces workspace
        // confinement.
        [[nodiscard]] std::string ReadFileContent(std::string const& relativePath,
                                                  size_t maxBytes = 32768) const;

        // Get relevant file entries whose summaries match query keywords.
        // Returns at most maxResults entries sorted by relevance score.
        [[nodiscard]] std::vector<FileIndexEntry> GetRelevantFiles(std::string const& query, int maxResults = 5) const;

        // Stats
        [[nodiscard]] size_t FileCount() const;
        [[nodiscard]] size_t SummaryCount() const;
        [[nodiscard]] std::chrono::steady_clock::time_point LastScanTime() const;

        // Get all entries (for /index command).
        [[nodiscard]] std::vector<FileIndexEntry> GetAllEntries() const;

        // Hard limits — enforced during LoadIndex and SetFileSummary.
        static constexpr size_t kMaxIndexEntries = 100000;
        static constexpr size_t kMaxSummaryBytes = 8 * 1024;

    private:
        void LoadIndex();
        void SaveIndex() const;
        void ScanDirectory(std::filesystem::path const& dir, int maxDepth);

        // Validate a relative path against the captured workspace root.
        // Returns the resolved absolute path on success; empty path on rejection
        // (`..` escape, absolute path, or path outside workspace root).
        [[nodiscard]] std::filesystem::path ResolveAndConfine(std::string const& relativePath) const;

        static bool IsIndexableExtension(std::string const& ext);
        [[nodiscard]] static int ScoreMatch(FileIndexEntry const& entry,
                                            std::vector<std::string> const& queryWords);

        std::filesystem::path m_IndexDir;       // e.g. assistant/index/
        std::filesystem::path m_IndexPath;      // e.g. assistant/index/file_index.jsonl
        std::filesystem::path m_WorkspaceRoot;  // canonical project-root path captured at ctor.
        mutable std::mutex m_Mutex;
        std::vector<FileIndexEntry> m_Entries;
        std::unordered_map<std::string, size_t> m_PathToIndex; // relativePath → index in m_Entries

        std::chrono::steady_clock::time_point m_LastScanTime{};
    };
} // namespace AIAssistant
