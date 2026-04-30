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

#include "assistant/assistantHelpers.h"
#include "engine.h"
#include "json/jsonHelper.h"
#include "simdjson/simdjson.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {
        std::string ClampLen(std::string const& s, size_t maxBytes)
        {
            if (s.size() <= maxBytes)
                return s;
            return s.substr(0, maxBytes);
        }
    } // namespace

    // -----------------------------------------------------------------
    // Construction / persistence
    // -----------------------------------------------------------------

    MemoryStore::MemoryStore(fs::path storePath) : m_StorePath(std::move(storePath)) { LoadFromDiskLocked(); }

    void MemoryStore::LoadFromDiskLocked()
    {
        // Open without an exists() pre-check — distinguishes "absent" (silent first-run)
        // from "present but unreadable" (genuine I/O error → ERROR-level log).
        std::ifstream ifs(m_StorePath);
        if (!ifs)
        {
            std::error_code ec;
            if (fs::exists(m_StorePath, ec))
            {
                LOG_APP_ERROR("[memory] Memory file present but unreadable: {}", m_StorePath.string());
                m_FileBroken = true;
            }
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
            LOG_APP_ERROR("[memory] Memory file parse failed: {}", simdjson::error_message(err));
            m_FileBroken = true;
            return;
        }

        simdjson::ondemand::array arr;
        if (doc.get_array().get(arr))
        {
            LOG_APP_ERROR("[memory] Memory file root is not a JSON array — refusing to load");
            m_FileBroken = true;
            return;
        }

        for (auto element : arr)
        {
            simdjson::ondemand::object obj;
            if (element.get_object().get(obj))
                continue;

            MemoryEntry entry;
            std::string_view sv;

            // Each get_string().get(sv) populates a view into `padded`'s buffer; we
            // copy into std::string immediately, so the view's borrowed lifetime ends
            // at the call site.  Do not store sv beyond this scope.
            if (!obj["id"].get_string().get(sv))
                entry.id = std::string(sv);
            if (!obj["key"].get_string().get(sv))
                entry.key = ClampLen(std::string(sv), kMaxKeyBytes);
            if (!obj["value"].get_string().get(sv))
                entry.value = ClampLen(std::string(sv), kMaxValueBytes);
            if (!obj["createdAt"].get_string().get(sv))
                entry.createdAt = std::string(sv);
            if (!obj["sourceSessionId"].get_string().get(sv))
                entry.sourceSessionId = std::string(sv);

            simdjson::ondemand::array tagsArr;
            if (!obj["tags"].get_array().get(tagsArr))
            {
                for (auto tag : tagsArr)
                {
                    if (entry.tags.size() >= kMaxTagsPerEntry)
                        break;
                    if (!tag.get_string().get(sv))
                        entry.tags.emplace_back(ClampLen(std::string(sv), kMaxTagBytes));
                }
            }

            if (entry.key.empty())
                continue;

            m_Entries.push_back(std::move(entry));
            if (m_Entries.size() >= kMaxEntries)
            {
                LOG_APP_ERROR("[memory] LoadFromDisk: entry cap ({}) reached — truncating", kMaxEntries);
                m_FileBroken = true;
                break;
            }
        }

        LOG_APP_INFO("[memory] Loaded {} memories from {}", m_Entries.size(), m_StorePath.string());
    }

    bool MemoryStore::SaveToDiskLocked()
    {
        std::error_code ec;
        fs::create_directories(m_StorePath.parent_path(), ec);
        if (ec)
        {
            LOG_APP_ERROR("[memory] Memory dir create failed: {} (path='{}')", ec.message(),
                          m_StorePath.parent_path().string());
            m_FileBroken = true;
            return false;
        }

        std::ostringstream oss;
        oss << "[\n";
        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            auto const& e = m_Entries[i];
            oss << "  {\n"
                << "    \"id\": \"" << JsonHelper::EscapeJsonString(e.id) << "\",\n"
                << "    \"key\": \"" << JsonHelper::EscapeJsonString(e.key) << "\",\n"
                << "    \"value\": \"" << JsonHelper::EscapeJsonString(e.value) << "\",\n"
                << "    \"tags\": [";
            for (size_t t = 0; t < e.tags.size(); ++t)
            {
                if (t > 0)
                    oss << ", ";
                oss << "\"" << JsonHelper::EscapeJsonString(e.tags[t]) << "\"";
            }
            oss << "],\n"
                << "    \"createdAt\": \"" << JsonHelper::EscapeJsonString(e.createdAt) << "\",\n"
                << "    \"sourceSessionId\": \"" << JsonHelper::EscapeJsonString(e.sourceSessionId) << "\"\n"
                << "  }";
            if (i + 1 < m_Entries.size())
                oss << ",";
            oss << "\n";
        }
        oss << "]\n";

        std::ofstream ofs(m_StorePath, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!ofs)
        {
            LOG_APP_ERROR("[memory] Memory file open-for-write failed: {}", m_StorePath.string());
            m_FileBroken = true;
            return false;
        }

        std::string const buf = oss.str();
        ofs.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        ofs.flush();
        if (!ofs.good())
        {
            LOG_APP_ERROR("[memory] Memory file write/flush failed: {}", m_StorePath.string());
            m_FileBroken = true;
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------

    std::string MemoryStore::Save(std::string const& key, std::string const& value, std::vector<std::string> const& tags,
                                  std::string const& sessionId)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_FileBroken)
        {
            LOG_APP_ERROR("[memory] Save refused: store in degraded state, key_len={}", key.size());
            return {};
        }

        std::string const clampedKey = ClampLen(key, kMaxKeyBytes);
        if (clampedKey.empty())
        {
            LOG_APP_ERROR("[memory] Save refused: empty key");
            return {};
        }

        std::string const clampedValue = ClampLen(value, kMaxValueBytes);

        std::vector<std::string> clampedTags;
        clampedTags.reserve(std::min(tags.size(), kMaxTagsPerEntry));
        for (auto const& t : tags)
        {
            if (clampedTags.size() >= kMaxTagsPerEntry)
                break;
            clampedTags.push_back(ClampLen(t, kMaxTagBytes));
        }

        // Update existing key if present.
        for (auto& entry : m_Entries)
        {
            if (entry.key == clampedKey)
            {
                entry.value = clampedValue;
                entry.tags = std::move(clampedTags);
                entry.createdAt = NowUtcIso8601();
                if (!sessionId.empty())
                    entry.sourceSessionId = sessionId;
                if (!SaveToDiskLocked())
                    return {};
                LOG_APP_INFO("[memory] Updated memory: key={}", LogSafeKey(clampedKey));
                return entry.id;
            }
        }

        if (m_Entries.size() >= kMaxEntries)
        {
            LOG_APP_ERROR("[memory] Save refused: entry cap ({}) reached", kMaxEntries);
            return {};
        }

        MemoryEntry entry;
        entry.id = GenerateId();
        entry.key = clampedKey;
        entry.value = clampedValue;
        entry.tags = std::move(clampedTags);
        entry.createdAt = NowUtcIso8601();
        entry.sourceSessionId = sessionId;

        std::string id = entry.id;
        m_Entries.push_back(std::move(entry));
        if (!SaveToDiskLocked())
        {
            // Roll back the in-memory mutation so the on-disk and in-memory state stay consistent.
            m_Entries.pop_back();
            return {};
        }

        LOG_APP_INFO("[memory] Saved new memory: key={} entries={}", LogSafeKey(clampedKey), m_Entries.size());
        return id;
    }

    bool MemoryStore::Delete(std::string const& key)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_FileBroken)
        {
            LOG_APP_ERROR("[memory] Delete refused: store in degraded state, key_len={}", key.size());
            return false;
        }

        auto it = std::remove_if(m_Entries.begin(), m_Entries.end(),
                                 [&key](MemoryEntry const& e) { return e.key == key; });

        if (it == m_Entries.end())
            return false;

        m_Entries.erase(it, m_Entries.end());
        if (!SaveToDiskLocked())
            return false; // m_Entries already mutated; sticky m_FileBroken prevents further writes.
        LOG_APP_INFO("[memory] Deleted memory: key={}", LogSafeKey(key));
        return true;
    }

    std::vector<MemoryEntry> MemoryStore::Recall(std::string const& query) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return RecallLocked(query);
    }

    std::vector<MemoryEntry> MemoryStore::RecallLocked(std::string const& query) const
    {
        if (query.empty())
            return m_Entries;

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
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                        word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (word.size() >= 2)
                    queryWords.push_back(word);
            }
        }

        if (queryWords.empty())
            return m_Entries;

        // Score by index — never store raw pointers into m_Entries.  An audit-level
        // lifetime hazard if any caller drops the lock between scoring and dereferencing.
        struct Scored
        {
            int score;
            size_t idx;
        };
        std::vector<Scored> scored;

        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            int s = ScoreMatch(m_Entries[i], queryWords);
            if (s > 0)
                scored.push_back({s, i});
        }

        std::sort(scored.begin(), scored.end(),
                  [](Scored const& a, Scored const& b) { return a.score > b.score; });

        std::vector<MemoryEntry> results;
        results.reserve(scored.size());
        for (auto const& s : scored)
            results.push_back(m_Entries[s.idx]);

        return results;
    }

    std::vector<MemoryEntry> MemoryStore::ListAll() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Entries;
    }

    std::vector<MemoryEntry> MemoryStore::GetRelevant(std::string const& userMessage, int maxResults) const
    {
        // Acquire the lock once and call RecallLocked, fixing the prior atomicity gap
        // where Recall released its lock before we trimmed the result.  Same lock is
        // also non-recursive, so a future inline of Recall here would no longer
        // deadlock — RecallLocked is the lock-free helper.
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto results = RecallLocked(userMessage);
        if (maxResults < 0)
            maxResults = 0;
        if (static_cast<int>(results.size()) > maxResults)
            results.resize(static_cast<size_t>(maxResults));
        return results;
    }

    void MemoryStore::ClearAll()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Entries.clear();
        if (!m_FileBroken)
            (void)SaveToDiskLocked();
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
        // 16 bytes → 32 hex chars.  Cryptographically secure (RAND_bytes), thread-safe,
        // unpredictable.  Replaces the prior process-local mt19937 race.
        std::string const r = RandomHex(16);
        if (r.empty())
        {
            // RAND_bytes failure is logged at ERROR by RandomHex.  Degraded fallback:
            // monotonic counter under the static mutex so collisions are bounded.
            static std::mutex s_Mutex;
            static uint64_t s_Counter = 0;
            std::lock_guard<std::mutex> lk(s_Mutex);
            return "mem_fallback_" + std::to_string(++s_Counter);
        }
        return "mem_" + r;
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

    std::string MemoryStore::LogSafeKey(std::string const& key)
    {
        // Keys are user-supplied — they may carry secrets, PII, or log-injection chars.
        // Strip newlines, cap length, and indicate truncation.  Never log raw values.
        constexpr size_t kMaxLogChars = 64;
        std::string out;
        out.reserve(std::min(key.size(), kMaxLogChars) + 4);
        for (char c : key)
        {
            unsigned char const u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F)
                out += '?';
            else
                out += c;
            if (out.size() >= kMaxLogChars)
            {
                out += "...";
                break;
            }
        }
        return "'" + out + "'(" + std::to_string(key.size()) + ")";
    }

    int MemoryStore::ScoreMatch(MemoryEntry const& entry, std::vector<std::string> const& queryWords)
    {
        std::string haystack;
        haystack.reserve(entry.key.size() + entry.value.size() + 128);

        // Key gets extra weight by being included twice.
        haystack += entry.key + " " + entry.key + " ";
        haystack += entry.value + " ";
        for (auto const& tag : entry.tags)
            haystack += tag + " ";

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

} // namespace AIAssistant
