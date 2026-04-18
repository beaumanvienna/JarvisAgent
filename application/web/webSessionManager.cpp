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

#include "web/webSessionManager.h"

#include <openssl/rand.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "engine.h"

namespace AIAssistant
{
    namespace
    {
        std::string RandomSessionId()
        {
            uint8_t bytes[32];
            if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            {
                LOG_CORE_ERROR("WebSessionManager: RAND_bytes failed");
                return {};
            }
            static constexpr char const* hex = "0123456789abcdef";
            std::string out(sizeof(bytes) * 2, '0');
            for (size_t i = 0; i < sizeof(bytes); ++i)
            {
                out[2 * i] = hex[(bytes[i] >> 4) & 0xF];
                out[2 * i + 1] = hex[bytes[i] & 0xF];
            }
            return out;
        }
    } // namespace

    WebSessionManager::LoginResult WebSessionManager::Create(std::string const& user, std::string const& role)
    {
        Session s;
        s.m_SessionId = RandomSessionId();
        s.m_User = user;
        s.m_Role = role;
        s.m_CreatedAt = std::chrono::steady_clock::now();
        s.m_LastActivity = s.m_CreatedAt;

        LoginResult r;
        r.m_SessionId = s.m_SessionId;
        r.m_User = user;
        r.m_Role = role;

        std::lock_guard lock(m_Mutex);
        m_Sessions[s.m_SessionId] = std::move(s);
        return r;
    }

    std::optional<WebSessionManager::Session> WebSessionManager::Validate(std::string const& sessionId)
    {
        if (sessionId.empty()) return std::nullopt;

        std::lock_guard lock(m_Mutex);
        auto it = m_Sessions.find(sessionId);
        if (it == m_Sessions.end()) return std::nullopt;

        auto const now = std::chrono::steady_clock::now();
        auto const idleLimit = std::chrono::hours(m_TimeoutHours);
        if (now - it->second.m_LastActivity > idleLimit)
        {
            m_Sessions.erase(it);
            return std::nullopt;
        }

        it->second.m_LastActivity = now;
        return it->second;
    }

    void WebSessionManager::Destroy(std::string const& sessionId)
    {
        std::lock_guard lock(m_Mutex);
        m_Sessions.erase(sessionId);
    }

    void WebSessionManager::PurgeExpired()
    {
        auto const now = std::chrono::steady_clock::now();
        auto const idleLimit = std::chrono::hours(m_TimeoutHours);

        std::lock_guard lock(m_Mutex);
        for (auto it = m_Sessions.begin(); it != m_Sessions.end();)
        {
            if (now - it->second.m_LastActivity > idleLimit)
            {
                it = m_Sessions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void WebSessionManager::SetTimeoutHours(int hours)
    {
        std::lock_guard lock(m_Mutex);
        m_TimeoutHours = hours > 0 ? hours : 8;
    }

    int WebSessionManager::GetTimeoutHours() const
    {
        std::lock_guard lock(m_Mutex);
        return m_TimeoutHours;
    }
} // namespace AIAssistant
