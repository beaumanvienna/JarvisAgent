/* Copyright (c) 2026 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace AIAssistant
{
    // Server-side session store for the dashboard login flow. Sessions are
    // created on successful MCP-key or gateway-header auth; the session id
    // travels as a HttpOnly+Secure+SameSite=Strict cookie. Sessions are not
    // persisted to disk — restarting j9t invalidates every session.
    //
    // Naming: this is the *web* session manager (cookie-backed dashboard
    // logins). It is deliberately distinct from the unrelated
    // code/backend/application/session/sessionManager.{h,cpp}, which manages AI query
    // session state for the workflow runtime.
    class WebSessionManager
    {
    public:
        struct Session
        {
            std::string m_SessionId;              // 256-bit random hex
            std::string m_User;                   // identity from MCP key or gateway header
            std::string m_Role;                   // "admin" | "operator" | "viewer"
            std::chrono::steady_clock::time_point m_CreatedAt;
            std::chrono::steady_clock::time_point m_LastActivity;
        };

        struct LoginResult
        {
            std::string m_SessionId;
            std::string m_User;
            std::string m_Role;
        };

        // Create and register a new session. Returns session id + identity.
        LoginResult Create(std::string const& user, std::string const& role);

        // Validate a session id from the request cookie. Returns the session
        // and slides the expiry window forward; returns nullopt if expired
        // or unknown.
        std::optional<Session> Validate(std::string const& sessionId);

        // Destroy one session (logout).
        void Destroy(std::string const& sessionId);

        // Drop all expired sessions. Safe to call from a background tick.
        void PurgeExpired();

        void SetTimeoutHours(int hours);
        int GetTimeoutHours() const;

    private:
        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, Session> m_Sessions;
        int m_TimeoutHours{8};
    };
} // namespace AIAssistant
