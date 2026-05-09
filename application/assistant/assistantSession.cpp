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
#include "assistant/assistantHelpers.h"
#include "engine.h"
#include "json/jsonHelper.h"
#include "simdjson/simdjson.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifndef _WIN32
    #include <sys/stat.h>
#endif

namespace AIAssistant
{
    namespace fs = std::filesystem;

    // -----------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------

    AssistantSession::AssistantSession(fs::path const& sessionsDir)
        : m_SessionId(GenerateSessionId())
        , m_StartedAt(NowIso8601())
    {
        std::error_code ec;
        fs::create_directories(sessionsDir, ec);
        if (ec)
        {
            LOG_APP_ERROR("[assistant] Sessions dir create failed: {} (path='{}')", ec.message(),
                          sessionsDir.string());
            m_FileBroken = true;
        }
        m_FilePath = sessionsDir / (m_SessionId + ".jsonl");
        LOG_APP_INFO("[assistant] New session created: {}", LogSafeSessionId(m_SessionId));
    }

    AssistantSession::AssistantSession(fs::path const& sessionsDir, std::string const& sessionId)
    {
        // Defense in depth: AssistantController::GetSession is the production caller and
        // already gates on IsValidOpaqueId, but this constructor is part of the public
        // surface — re-validating here means a future direct caller cannot smuggle a
        // path-traversal sessionId into m_FilePath.
        if (!IsValidOpaqueId(sessionId))
        {
            LOG_SECURITY_WARN("[security] assistant_session_resume_invalid_id length={}", sessionId.size());
            m_SessionId.clear();
            m_StartedAt = NowIso8601();
            m_FileBroken = true;
            return;
        }

        m_SessionId = sessionId;
        m_FilePath = sessionsDir / (sessionId + ".jsonl");

        LoadFromFileLocked();
        if (!m_Turns.empty())
        {
            m_StartedAt = m_Turns.front().timestamp;
        }
        else
        {
            m_StartedAt = NowIso8601();
        }
        LOG_APP_INFO("[assistant] Resumed session: {} ({} turns)", LogSafeSessionId(m_SessionId), m_Turns.size());
    }

    // -----------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------

    size_t AssistantSession::GetTurnCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Turns.size();
    }

    bool AssistantSession::AddUserMessage(std::string const& text)
    {
        AssistantTurn turn;
        turn.role = "user";
        turn.text = text;
        turn.timestamp = NowIso8601();
        std::lock_guard<std::mutex> lock(m_Mutex);
        return AppendTurnLocked(turn);
    }

    bool AssistantSession::AddAssistantMessage(std::string const& text)
    {
        AssistantTurn turn;
        turn.role = "assistant";
        turn.text = text;
        turn.timestamp = NowIso8601();
        std::lock_guard<std::mutex> lock(m_Mutex);
        return AppendTurnLocked(turn);
    }

    std::vector<AssistantTurn> AssistantSession::GetRecentTurns(size_t maxTokens) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        std::vector<AssistantTurn> result;
        if (maxTokens == 0)
            return result;

        size_t tokenCount = 0;

        // Walk backwards from newest turn, collecting until budget is exhausted.
        // A previous `!result.empty()` guard always returned at least one turn
        // even if it alone exceeded maxTokens — removed; budget is honoured strictly.
        for (auto it = m_Turns.rbegin(); it != m_Turns.rend(); ++it)
        {
            size_t turnTokens = it->text.size() / 4; // rough estimate
            if (tokenCount + turnTokens > maxTokens)
                break;
            result.push_back(*it);
            tokenCount += turnTokens;
        }

        std::reverse(result.begin(), result.end());
        return result;
    }

    std::vector<AssistantTurn> AssistantSession::GetAllTurns() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Turns;
    }

    // -----------------------------------------------------------------
    // Persistence
    // -----------------------------------------------------------------

    bool AssistantSession::AppendTurnLocked(AssistantTurn const& turn)
    {
        if (m_FileBroken)
        {
            LOG_APP_ERROR("[assistant] AppendTurn refused: session in degraded state, sid={}",
                          LogSafeSessionId(m_SessionId));
            return false;
        }
        if (m_Turns.size() >= kMaxTurnsPerSession)
        {
            LOG_APP_ERROR("[assistant] AppendTurn refused: session at turn cap ({}) sid={}", kMaxTurnsPerSession,
                          LogSafeSessionId(m_SessionId));
            return false;
        }

        // Write to disk first; only commit to in-memory history on success.
        std::ofstream ofs(m_FilePath, std::ios::app | std::ios::binary);
        if (!ofs)
        {
            m_FileBroken = true;
            LOG_APP_ERROR("[assistant] AppendTurn open failed: sid={} path='{}'", LogSafeSessionId(m_SessionId),
                          m_FilePath.string());
            return false;
        }

        // Restrict permissions on first write (best-effort, no error path).
        RestrictFilePermissions(m_FilePath);

        ofs << "{\"role\":\"" << JsonHelper::EscapeJsonString(turn.role) << "\",\"text\":\""
            << JsonHelper::EscapeJsonString(turn.text) << "\",\"ts\":\""
            << JsonHelper::EscapeJsonString(turn.timestamp) << "\"}\n";
        ofs.flush();
        if (!ofs.good())
        {
            m_FileBroken = true;
            LOG_APP_ERROR("[assistant] AppendTurn write/flush failed: sid={} path='{}'",
                          LogSafeSessionId(m_SessionId), m_FilePath.string());
            return false;
        }

        m_Turns.push_back(turn);
        return true;
    }

    void AssistantSession::LoadFromFileLocked()
    {
        m_Turns.clear();

        // Open without an `exists` pre-check — the open succeeds-or-fails atomically
        // and we can distinguish "absent" (genuine new session) from "present but unreadable".
        std::ifstream ifs(m_FilePath);
        if (!ifs)
        {
            std::error_code ec;
            if (fs::exists(m_FilePath, ec))
            {
                LOG_APP_ERROR("[assistant] LoadFromFile: file present but unreadable: sid={} path='{}'",
                              LogSafeSessionId(m_SessionId), m_FilePath.string());
            }
            return;
        }

        std::string line;
        line.reserve(1024);
        size_t lineCount = 0;
        while (std::getline(ifs, line))
        {
            ++lineCount;
            if (line.empty())
                continue;
            if (line.size() > kMaxLineBytes)
            {
                LOG_APP_ERROR("[assistant] LoadFromFile: line {} exceeds {} bytes — truncating session load: sid={}",
                              lineCount, kMaxLineBytes, LogSafeSessionId(m_SessionId));
                m_FileBroken = true;
                break;
            }

            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(line);
            simdjson::ondemand::document doc;
            if (parser.iterate(padded).get(doc))
                continue;
            simdjson::ondemand::object obj;
            if (doc.get_object().get(obj))
                continue;

            AssistantTurn turn;
            std::string_view sv;
            if (!obj["role"].get_string().get(sv))
                turn.role = std::string(sv);
            if (!obj["text"].get_string().get(sv))
                turn.text = std::string(sv);
            if (!obj["ts"].get_string().get(sv))
                turn.timestamp = std::string(sv);

            // Validate role against the closed set.  An adversarial JSONL with
            // `"role":"badvalue"` is silently dropped rather than fed to downstream
            // AI provider formatters that switch on role strings.
            if (turn.role != "user" && turn.role != "assistant" && turn.role != "system")
                continue;

            if (turn.text.size() > kMaxTurnTextBytes)
                turn.text.resize(kMaxTurnTextBytes);

            m_Turns.push_back(std::move(turn));
            if (m_Turns.size() >= kMaxTurnsPerSession)
            {
                LOG_APP_ERROR("[assistant] LoadFromFile: turn cap ({}) reached — truncating: sid={}",
                              kMaxTurnsPerSession, LogSafeSessionId(m_SessionId));
                m_FileBroken = true;
                break;
            }
        }
    }

    // -----------------------------------------------------------------
    // ID / time helpers
    // -----------------------------------------------------------------

    std::string AssistantSession::GenerateSessionId()
    {
        // 16 random bytes → 32 hex chars (128 bits of entropy).  No timestamp / counter:
        // the prior scheme leaked process-start time and was guessable across resets.
        std::string const r = RandomHex(16);
        if (r.empty())
        {
            // RAND_bytes failure already logged at ERROR by RandomHex; fall through to a
            // PID+time fallback so the session has SOME unique-ish ID rather than empty
            // (which would collide).  This branch is degraded mode only.
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            return "sess_fallback_" + std::to_string(ms);
        }
        return "sess_" + r;
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

    std::string AssistantSession::LogSafeSessionId(std::string const& id)
    {
        // Session IDs are security-sensitive (they're file basenames and conversation handles).
        // Logs are aggregated into log/log.txt and the dashboard; emit only an 8-char prefix.
        if (id.size() <= 8)
            return id;
        return id.substr(0, 8) + "...";
    }

    void AssistantSession::RestrictFilePermissions(fs::path const& path)
    {
#ifndef _WIN32
        std::error_code ec;
        // 0600 — session contains conversation history including any secrets
        // the user typed.  Best-effort: never throws, never fails the write path.
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        // ec ignored intentionally — already-correct permissions are normal,
        // and a permissions-set failure shouldn't tank the write that just succeeded.
#else
        (void)path; // Windows: rely on inherited NTFS DACLs from the parent dir.
#endif
    }

    // -----------------------------------------------------------------
    // Static utilities
    // -----------------------------------------------------------------

    std::vector<std::string> AssistantSession::ListSessions(fs::path const& sessionsDir)
    {
        std::vector<std::string> ids;

        // Construct the iterator directly with an error_code — the prior `exists` pre-check
        // was both racy (TOCTOU between exists and iterator construction) and noisy
        // (couldn't distinguish "no sessions" from "directory removed mid-call").
        std::error_code ec;
        fs::directory_iterator it(sessionsDir, ec);
        if (ec)
        {
            // Distinguish missing-on-first-run (silent) from permission/IO error (warn).
            if (ec != std::errc::no_such_file_or_directory)
            {
                LOG_APP_WARN("[assistant] ListSessions iterator failed: {} (path='{}')", ec.message(),
                             sessionsDir.string());
            }
            return ids;
        }

        for (auto const& entry : it)
        {
            if (entry.path().extension() == ".jsonl")
            {
                std::string stem = entry.path().stem().string();
                // Accept only IDs that match our allowlist.  A foreign .jsonl placed
                // in the directory by another tool shouldn't surface as a session.
                if (IsValidOpaqueId(stem))
                    ids.push_back(std::move(stem));
            }
        }

        // Sort newest first (random hex IDs aren't time-ordered, so sort by mtime
        // would be more correct; keep the lexical sort for now to preserve behavior).
        std::sort(ids.begin(), ids.end(), std::greater<>());
        return ids;
    }
} // namespace AIAssistant
