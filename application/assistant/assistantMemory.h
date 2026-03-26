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
        std::string id;              // Unique identifier (auto-generated).
        std::string key;             // Short label / topic.
        std::string value;           // Content.
        std::vector<std::string> tags;
        std::string createdAt;       // ISO-8601 UTC.
        std::string sourceSessionId; // Session that created it.
    };

    // Persistent memory store backed by a JSON file.
    // Thread-safe — all public methods acquire m_Mutex.
    class MemoryStore
    {
    public:
        explicit MemoryStore(std::filesystem::path storePath);

        // Save or update a memory.  If a memory with the same key exists, it is updated.
        // Returns the entry id.
        std::string Save(std::string const& key, std::string const& value,
                         std::vector<std::string> const& tags = {},
                         std::string const& sessionId = {});

        // Delete a memory by key.  Returns true if found and deleted.
        bool Delete(std::string const& key);

        // Search memories by keyword (matches key, value, and tags).
        std::vector<MemoryEntry> Recall(std::string const& query) const;

        // List all memories (key + createdAt only, for brief display).
        std::vector<MemoryEntry> ListAll() const;

        // Get memories relevant to a user message (simple keyword overlap).
        // Returns at most maxResults entries, sorted by relevance score.
        std::vector<MemoryEntry> GetRelevant(std::string const& userMessage, int maxResults = 5) const;

        // Clear all memories.
        void ClearAll();

        // Number of stored memories.
        size_t Size() const;

    private:
        void LoadFromDisk();
        void SaveToDisk() const;

        static std::string GenerateId();
        static std::string NowUtcIso8601();
        static int ScoreMatch(MemoryEntry const& entry, std::vector<std::string> const& queryWords);

        std::filesystem::path m_StorePath;
        mutable std::mutex m_Mutex;
        std::vector<MemoryEntry> m_Entries;
    };
} // namespace AIAssistant
