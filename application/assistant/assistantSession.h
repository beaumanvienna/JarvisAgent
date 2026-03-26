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

#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace AIAssistant
{
    // A single conversation turn in the assistant session.
    struct AssistantTurn
    {
        std::string role;      // "user", "assistant", or "system"
        std::string text;      // message content
        std::string timestamp; // ISO 8601
    };

    // Manages a single assistant conversation session.
    // Persists history as an append-only JSONL file under assistant/sessions/.
    // Thread-safe: all public methods are guarded by a mutex.
    class AssistantSession
    {
    public:
        // Create a new session (generates a fresh session ID).
        explicit AssistantSession(std::filesystem::path const& sessionsDir);

        // Resume an existing session from a JSONL file.
        AssistantSession(std::filesystem::path const& sessionsDir, std::string const& sessionId);

        std::string const& GetSessionId() const { return m_SessionId; }
        std::string const& GetStartedAt() const { return m_StartedAt; }
        size_t GetTurnCount() const;

        void AddUserMessage(std::string const& text);
        void AddAssistantMessage(std::string const& text);

        // Return the most recent turns that fit within a rough token budget.
        // Token estimate: chars / 4.
        std::vector<AssistantTurn> GetRecentTurns(size_t maxTokens) const;

        // Return ALL turns (for session list summary etc.).
        std::vector<AssistantTurn> GetAllTurns() const;

        // Scan a sessions directory and return available session IDs (newest first).
        static std::vector<std::string> ListSessions(std::filesystem::path const& sessionsDir);

    private:
        void AppendTurn(AssistantTurn const& turn);
        void LoadFromFile();
        static std::string GenerateSessionId();
        static std::string NowIso8601();

        std::string m_SessionId;
        std::string m_StartedAt;
        std::filesystem::path m_FilePath;
        std::vector<AssistantTurn> m_Turns;
        mutable std::mutex m_Mutex;
    };
} // namespace AIAssistant
