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

#include "assistant/assistantSession.h"
#include "engine.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

// Minimal JSON helpers — avoid pulling in a full JSON writer for simple JSONL.
namespace
{
    std::string JsonEscape(std::string const& input)
    {
        std::string out;
        out.reserve(input.size() + 16);
        for (char c : input)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    }

    // Tiny JSON value extractor — finds "key":"value" in a JSON line.
    // Good enough for our simple JSONL format. Not a general JSON parser.
    std::string ExtractJsonString(std::string const& json, std::string const& key)
    {
        std::string pattern = "\"" + key + "\":\"";
        auto pos = json.find(pattern);
        if (pos == std::string::npos)
            return {};
        pos += pattern.size();
        std::string result;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                char next = json[pos + 1];
                switch (next)
                {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    default: result += next; break;
                }
                pos += 2;
            }
            else
            {
                result += json[pos];
                ++pos;
            }
        }
        return result;
    }
} // namespace

namespace AIAssistant
{
    // Create a new session.
    AssistantSession::AssistantSession(std::filesystem::path const& sessionsDir)
        : m_SessionId(GenerateSessionId())
        , m_StartedAt(NowIso8601())
    {
        std::error_code ec;
        std::filesystem::create_directories(sessionsDir, ec);
        m_FilePath = sessionsDir / (m_SessionId + ".jsonl");
        LOG_APP_INFO("[assistant] New session created: {}", m_SessionId);
    }

    // Resume an existing session.
    AssistantSession::AssistantSession(std::filesystem::path const& sessionsDir, std::string const& sessionId)
        : m_SessionId(sessionId)
        , m_FilePath(sessionsDir / (sessionId + ".jsonl"))
    {
        LoadFromFile();
        if (!m_Turns.empty())
        {
            m_StartedAt = m_Turns.front().timestamp;
        }
        else
        {
            m_StartedAt = NowIso8601();
        }
        LOG_APP_INFO("[assistant] Resumed session: {} ({} turns)", m_SessionId, m_Turns.size());
    }

    size_t AssistantSession::GetTurnCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Turns.size();
    }

    void AssistantSession::AddUserMessage(std::string const& text)
    {
        AssistantTurn turn;
        turn.role = "user";
        turn.text = text;
        turn.timestamp = NowIso8601();
        AppendTurn(turn);
    }

    void AssistantSession::AddAssistantMessage(std::string const& text)
    {
        AssistantTurn turn;
        turn.role = "assistant";
        turn.text = text;
        turn.timestamp = NowIso8601();
        AppendTurn(turn);
    }

    std::vector<AssistantTurn> AssistantSession::GetRecentTurns(size_t maxTokens) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        std::vector<AssistantTurn> result;
        size_t tokenCount = 0;

        // Walk backwards from newest turn, collecting until budget is exhausted.
        for (auto it = m_Turns.rbegin(); it != m_Turns.rend(); ++it)
        {
            size_t turnTokens = it->text.size() / 4; // rough estimate
            if (tokenCount + turnTokens > maxTokens && !result.empty())
                break;
            result.push_back(*it);
            tokenCount += turnTokens;
        }

        // Reverse so oldest is first.
        std::reverse(result.begin(), result.end());
        return result;
    }

    std::vector<AssistantTurn> AssistantSession::GetAllTurns() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Turns;
    }

    void AssistantSession::AppendTurn(AssistantTurn const& turn)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Turns.push_back(turn);

        // Append to JSONL file.
        std::ofstream ofs(m_FilePath, std::ios::app);
        if (ofs)
        {
            ofs << "{\"role\":\"" << JsonEscape(turn.role)
                << "\",\"text\":\"" << JsonEscape(turn.text)
                << "\",\"ts\":\"" << JsonEscape(turn.timestamp) << "\"}\n";
        }
        else
        {
            LOG_APP_WARN("[assistant] Failed to write to session file: {}", m_FilePath.string());
        }
    }

    void AssistantSession::LoadFromFile()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Turns.clear();

        std::ifstream ifs(m_FilePath);
        if (!ifs)
            return;

        std::string line;
        while (std::getline(ifs, line))
        {
            if (line.empty())
                continue;
            AssistantTurn turn;
            turn.role = ExtractJsonString(line, "role");
            turn.text = ExtractJsonString(line, "text");
            turn.timestamp = ExtractJsonString(line, "ts");
            if (!turn.role.empty())
            {
                m_Turns.push_back(std::move(turn));
            }
        }
    }

    std::string AssistantSession::GenerateSessionId()
    {
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
        return "sess_" + std::to_string(seconds);
    }

    std::string AssistantSession::NowIso8601()
    {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &time_t_now);
#else
        gmtime_r(&time_t_now, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    std::vector<std::string> AssistantSession::ListSessions(std::filesystem::path const& sessionsDir)
    {
        std::vector<std::string> ids;
        std::error_code ec;
        if (!std::filesystem::exists(sessionsDir, ec))
            return ids;

        for (auto const& entry : std::filesystem::directory_iterator(sessionsDir, ec))
        {
            if (entry.path().extension() == ".jsonl")
            {
                ids.push_back(entry.path().stem().string());
            }
        }

        // Sort newest first (session IDs contain timestamps).
        std::sort(ids.begin(), ids.end(), std::greater<>());
        return ids;
    }
} // namespace AIAssistant
