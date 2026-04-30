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
    //
    // Construction-time thread-safety contract: the constructors do NOT acquire
    // m_Mutex.  The object must be fully constructed before being shared across
    // threads (factory pattern); this is what AssistantController already does
    // by holding m_SessionsMutex around CreateSession/GetSession.
    class AssistantSession
    {
    public:
        // Create a new session (generates a fresh, cryptographically random session ID).
        explicit AssistantSession(std::filesystem::path const& sessionsDir);

        // Resume an existing session from a JSONL file.
        // sessionId MUST satisfy IsValidOpaqueId — the constructor logs at SECURITY-WARN
        // and yields an empty session if the input fails the allowlist.
        AssistantSession(std::filesystem::path const& sessionsDir, std::string const& sessionId);

        AssistantSession(AssistantSession const&) = delete;
        AssistantSession& operator=(AssistantSession const&) = delete;
        AssistantSession(AssistantSession&&) = delete;
        AssistantSession& operator=(AssistantSession&&) = delete;

        std::string const& GetSessionId() const { return m_SessionId; }
        std::string const& GetStartedAt() const { return m_StartedAt; }
        size_t GetTurnCount() const;

        // Returns true iff the turn was both pushed in-memory and durably written to disk.
        // Callers should check; on false the session is in a degraded (memory-only) state.
        [[nodiscard]] bool AddUserMessage(std::string const& text);
        [[nodiscard]] bool AddAssistantMessage(std::string const& text);

        // Return the most recent turns that fit within a rough token budget.
        // Token estimate: chars / 4.  maxTokens == 0 returns an empty vector.
        std::vector<AssistantTurn> GetRecentTurns(size_t maxTokens) const;

        // Return ALL turns (for session list summary etc.).
        std::vector<AssistantTurn> GetAllTurns() const;

        // Scan a sessions directory and return available session IDs (newest first).
        static std::vector<std::string> ListSessions(std::filesystem::path const& sessionsDir);

        // Hard limits enforced on disk/memory load — prevents OOM via crafted JSONL.
        static constexpr size_t kMaxTurnsPerSession = 10000;
        static constexpr size_t kMaxLineBytes = 1 * 1024 * 1024; // 1 MiB
        static constexpr size_t kMaxTurnTextBytes = 256 * 1024;  // 256 KiB

    private:
        // Locked variant — caller must already hold m_Mutex.  Returns false on file error.
        [[nodiscard]] bool AppendTurnLocked(AssistantTurn const& turn);

        // Locked variant — caller must already hold m_Mutex (or be still inside the
        // ctor where no other thread can observe `this`).  Populates m_Turns.
        void LoadFromFileLocked();

        static std::string GenerateSessionId();
        static std::string NowIso8601();
        static std::string LogSafeSessionId(std::string const& id);
        // Apply 0600 permissions to the JSONL file, ignoring failure (best-effort).
        static void RestrictFilePermissions(std::filesystem::path const& path);

        std::string m_SessionId;
        std::string m_StartedAt;
        std::filesystem::path m_FilePath;
        std::vector<AssistantTurn> m_Turns;
        bool m_FileBroken{false}; // sticky: set after a persistence failure; further writes fail fast.
        mutable std::mutex m_Mutex;
    };
} // namespace AIAssistant
