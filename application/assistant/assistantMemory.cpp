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

#include "assistant/assistantMemory.h"

#include "engine.h"
#include "simdjson/simdjson.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace AIAssistant
{
    // -----------------------------------------------------------------
    // JSON string escaping (for manual serialization)
    // -----------------------------------------------------------------

    static std::string JsonEscapeMem(std::string const& input)
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
    // Construction / persistence
    // -----------------------------------------------------------------

    MemoryStore::MemoryStore(fs::path storePath) : m_StorePath(std::move(storePath)) { LoadFromDisk(); }

    void MemoryStore::LoadFromDisk()
    {
        std::error_code ec;
        if (!fs::exists(m_StorePath, ec))
            return;

        // Read entire file into a padded string for simdjson.
        std::ifstream ifs(m_StorePath);
        if (!ifs)
        {
            LOG_APP_WARN("[memory] Failed to open memory file: {}", m_StorePath.string());
            return;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (content.empty())
            return;

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(content);
        simdjson::ondemand::document doc;
        auto err = parser.iterate(padded).get(doc);
        if (err)
        {
            LOG_APP_WARN("[memory] Failed to parse memory file: {}", simdjson::error_message(err));
            return;
        }

        simdjson::ondemand::array arr;
        if (doc.get_array().get(arr))
        {
            LOG_APP_WARN("[memory] Memory file is not a JSON array — ignoring");
            return;
        }

        for (auto element : arr)
        {
            simdjson::ondemand::object obj;
            if (element.get_object().get(obj))
                continue;

            MemoryEntry entry;
            std::string_view sv;

            if (!obj["id"].get_string().get(sv))
                entry.id = std::string(sv);
            if (!obj["key"].get_string().get(sv))
                entry.key = std::string(sv);
            if (!obj["value"].get_string().get(sv))
                entry.value = std::string(sv);
            if (!obj["createdAt"].get_string().get(sv))
                entry.createdAt = std::string(sv);
            if (!obj["sourceSessionId"].get_string().get(sv))
                entry.sourceSessionId = std::string(sv);

            simdjson::ondemand::array tagsArr;
            if (!obj["tags"].get_array().get(tagsArr))
            {
                for (auto tag : tagsArr)
                {
                    if (!tag.get_string().get(sv))
                        entry.tags.emplace_back(sv);
                }
            }

            if (!entry.key.empty())
                m_Entries.push_back(std::move(entry));
        }

        LOG_APP_INFO("[memory] Loaded {} memories from {}", m_Entries.size(), m_StorePath.string());
    }

    void MemoryStore::SaveToDisk() const
    {
        // Ensure parent directory exists.
        std::error_code ec;
        fs::create_directories(m_StorePath.parent_path(), ec);

        // Manual JSON serialization.
        std::ostringstream oss;
        oss << "[\n";
        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            auto const& e = m_Entries[i];
            oss << "  {\n"
                << "    \"id\": \"" << JsonEscapeMem(e.id) << "\",\n"
                << "    \"key\": \"" << JsonEscapeMem(e.key) << "\",\n"
                << "    \"value\": \"" << JsonEscapeMem(e.value) << "\",\n"
                << "    \"tags\": [";
            for (size_t t = 0; t < e.tags.size(); ++t)
            {
                if (t > 0)
                    oss << ", ";
                oss << "\"" << JsonEscapeMem(e.tags[t]) << "\"";
            }
            oss << "],\n"
                << "    \"createdAt\": \"" << JsonEscapeMem(e.createdAt) << "\",\n"
                << "    \"sourceSessionId\": \"" << JsonEscapeMem(e.sourceSessionId) << "\"\n"
                << "  }";
            if (i + 1 < m_Entries.size())
                oss << ",";
            oss << "\n";
        }
        oss << "]\n";

        std::ofstream ofs(m_StorePath, std::ios::out | std::ios::trunc);
        if (!ofs)
        {
            LOG_APP_WARN("[memory] Failed to write memory file: {}", m_StorePath.string());
            return;
        }
        ofs << oss.str();
    }

    // -----------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------

    std::string MemoryStore::Save(std::string const& key, std::string const& value, std::vector<std::string> const& tags,
                                  std::string const& sessionId)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        // Check if a memory with the same key already exists — update it.
        for (auto& entry : m_Entries)
        {
            if (entry.key == key)
            {
                entry.value = value;
                entry.tags = tags;
                entry.createdAt = NowUtcIso8601();
                if (!sessionId.empty())
                    entry.sourceSessionId = sessionId;
                SaveToDisk();
                LOG_APP_INFO("[memory] Updated memory: key='{}'", key);
                return entry.id;
            }
        }

        // Create new entry.
        MemoryEntry entry;
        entry.id = GenerateId();
        entry.key = key;
        entry.value = value;
        entry.tags = tags;
        entry.createdAt = NowUtcIso8601();
        entry.sourceSessionId = sessionId;

        std::string id = entry.id;
        m_Entries.push_back(std::move(entry));
        SaveToDisk();

        LOG_APP_INFO("[memory] Saved new memory: key='{}' id='{}'", key, id);
        return id;
    }

    bool MemoryStore::Delete(std::string const& key)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        auto it = std::remove_if(m_Entries.begin(), m_Entries.end(), [&key](MemoryEntry const& e) { return e.key == key; });

        if (it == m_Entries.end())
            return false;

        m_Entries.erase(it, m_Entries.end());
        SaveToDisk();
        LOG_APP_INFO("[memory] Deleted memory: key='{}'", key);
        return true;
    }

    std::vector<MemoryEntry> MemoryStore::Recall(std::string const& query) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (query.empty())
            return m_Entries;

        // Tokenize query into lowercase words, stripping punctuation.
        std::vector<std::string> queryWords;
        {
            std::istringstream iss(query);
            std::string raw;
            while (iss >> raw)
            {
                // Strip leading/trailing punctuation.
                std::string word;
                word.reserve(raw.size());
                for (char c : raw)
                {
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                        word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (word.size() >= 2) // skip very short words
                    queryWords.push_back(word);
            }
        }

        if (queryWords.empty())
            return m_Entries;

        // Score and filter.
        struct Scored
        {
            int score;
            MemoryEntry const* entry;
        };
        std::vector<Scored> scored;

        for (auto const& entry : m_Entries)
        {
            int s = ScoreMatch(entry, queryWords);
            if (s > 0)
                scored.push_back({s, &entry});
        }

        std::sort(scored.begin(), scored.end(), [](auto const& a, auto const& b) { return a.score > b.score; });

        std::vector<MemoryEntry> results;
        results.reserve(scored.size());
        for (auto const& s : scored)
            results.push_back(*s.entry);

        return results;
    }

    std::vector<MemoryEntry> MemoryStore::ListAll() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Entries;
    }

    std::vector<MemoryEntry> MemoryStore::GetRelevant(std::string const& userMessage, int maxResults) const
    {
        auto results = Recall(userMessage);
        if (static_cast<int>(results.size()) > maxResults)
            results.resize(maxResults);
        return results;
    }

    void MemoryStore::ClearAll()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Entries.clear();
        SaveToDisk();
        LOG_APP_INFO("[memory] Cleared all memories");
    }

    size_t MemoryStore::Size() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Entries.size();
    }

    // -----------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------

    std::string MemoryStore::GenerateId()
    {
        static std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
        std::uniform_int_distribution<uint64_t> dist;
        std::ostringstream oss;
        oss << "mem_" << std::hex << dist(rng);
        return oss.str();
    }

    std::string MemoryStore::NowUtcIso8601()
    {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &tt);
#else
        gmtime_r(&tt, &utc);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buf;
    }

    int MemoryStore::ScoreMatch(MemoryEntry const& entry, std::vector<std::string> const& queryWords)
    {
        // Build a lowercase haystack from key + value + tags.
        std::string haystack;
        haystack.reserve(entry.key.size() + entry.value.size() + 128);

        // Key gets extra weight by being included twice.
        haystack += entry.key + " " + entry.key + " ";
        haystack += entry.value + " ";
        for (auto const& tag : entry.tags)
            haystack += tag + " ";

        std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);

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

} // namespace AIAssistant
