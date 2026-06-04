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

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace AIAssistant
{
    struct MemoryEntry
    {
        std::string id;              // Unique identifier (auto-generated, "mem_" + 16 random hex bytes).
        std::string key;             // Short label / topic.
        std::string value;           // Content.
        std::vector<std::string> tags;
        std::string createdAt;       // ISO-8601 UTC.
        std::string sourceSessionId; // Session that created it.
    };

    // Persistent memory store backed by a JSON file.
    // Thread-safe — all public methods acquire m_Mutex once and hold it for the whole operation.
    //
    // Construction-time thread-safety contract: the constructor does NOT acquire m_Mutex;
    // it loads from disk before any other thread can observe `this`.  Callers must
    // fully construct before sharing.
    class MemoryStore
    {
    public:
        explicit MemoryStore(std::filesystem::path storePath);

        MemoryStore(MemoryStore const&) = delete;
        MemoryStore& operator=(MemoryStore const&) = delete;
        MemoryStore(MemoryStore&&) = delete;
        MemoryStore& operator=(MemoryStore&&) = delete;

        // Save or update a memory.  If a memory with the same key exists, it is updated.
        // Returns the entry id, or an empty string on rejection (over caps, file degraded).
        // Inputs are clamped: key to kMaxKeyBytes, value to kMaxValueBytes,
        // tags to kMaxTagsPerEntry × kMaxTagBytes.
        [[nodiscard]] std::string Save(std::string const& key, std::string const& value,
                                       std::vector<std::string> const& tags = {},
                                       std::string const& sessionId = {});

        // Delete a memory by key.  Returns true if found and deleted.
        [[nodiscard]] bool Delete(std::string const& key);

        // Search memories by keyword (matches key, value, and tags).
        [[nodiscard]] std::vector<MemoryEntry> Recall(std::string const& query) const;

        // List all memories (key + createdAt only, for brief display).
        [[nodiscard]] std::vector<MemoryEntry> ListAll() const;

        // Get memories relevant to a user message (simple keyword overlap).
        // Returns at most maxResults entries, sorted by relevance score.
        [[nodiscard]] std::vector<MemoryEntry> GetRelevant(std::string const& userMessage, int maxResults = 5) const;

        // Clear all memories.
        void ClearAll();

        // Number of stored memories.
        [[nodiscard]] size_t Size() const;

        // Hard limits — enforced at every Save and during LoadFromDisk to bound memory.
        static constexpr size_t kMaxEntries = 10000;
        static constexpr size_t kMaxKeyBytes = 256;
        static constexpr size_t kMaxValueBytes = 64 * 1024;
        static constexpr size_t kMaxTagBytes = 256;
        static constexpr size_t kMaxTagsPerEntry = 32;

    private:
        // Locked variants — caller must already hold m_Mutex.
        void LoadFromDiskLocked();
        bool SaveToDiskLocked(); // returns true iff serialized + flushed to disk.
        std::vector<MemoryEntry> RecallLocked(std::string const& query) const;

        static std::string GenerateId();
        static std::string NowUtcIso8601();
        static std::string LogSafeKey(std::string const& key);
        static int ScoreMatch(MemoryEntry const& entry, std::vector<std::string> const& queryWords);

        std::filesystem::path m_StorePath;
        mutable std::mutex m_Mutex;
        std::vector<MemoryEntry> m_Entries;
        bool m_FileBroken{false};
    };
} // namespace AIAssistant
