/* Copyright (c) 2026 JC Technolabs

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

#include <chrono>

#include "engine.h"
#include "keys/oauthTokenManager.h"
#include "keys/keyManager.h"
#include "log/secretRedactor.h"

namespace AIAssistant
{
    static constexpr int REFRESH_CHECK_INTERVAL_SECONDS = 30;
    static constexpr int REFRESH_BEFORE_EXPIRY_SECONDS = 300; // Refresh 5 minutes before expiry

    static int64_t NowUnixSeconds()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    OAuthTokenManager::OAuthTokenManager(KeyManager& keyManager)
        : m_KeyManager(keyManager)
    {
    }

    OAuthTokenManager::~OAuthTokenManager()
    {
        Stop();
    }

    void OAuthTokenManager::Start()
    {
        if (m_Running.load())
        {
            return;
        }
        m_Running.store(true);
        m_RefreshThread = std::thread(&OAuthTokenManager::RefreshLoop, this);
        LOG_CORE_INFO("OAuthTokenManager: refresh loop started");
    }

    void OAuthTokenManager::Stop()
    {
        if (!m_Running.load())
        {
            return;
        }
        m_Running.store(false);
        m_Cv.notify_all();
        if (m_RefreshThread.joinable())
        {
            m_RefreshThread.join();
        }
        LOG_CORE_INFO("OAuthTokenManager: refresh loop stopped");
    }

    std::string OAuthTokenManager::GetAccessToken(std::string const& keyName, std::string& errorMessage)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Tokens.find(keyName);
        if (it == m_Tokens.end())
        {
            errorMessage = "No OAuth tokens stored for '" + keyName + "'";
            return {};
        }

        TokenEntry& entry = it->second;

        // Wait if a refresh is in progress
        m_Cv.wait(lock, [&entry]() { return !entry.m_Refreshing; });

        int64_t now = NowUnixSeconds();
        if (entry.m_ExpiresAt > 0 && now >= entry.m_ExpiresAt)
        {
            errorMessage = "OAuth token for '" + keyName + "' has expired and refresh failed";
            return {};
        }

        return entry.m_AccessToken;
    }

    void OAuthTokenManager::StoreTokens(std::string const& keyName, std::string const& accessToken,
                                         std::string const& refreshToken, int64_t expiresInSeconds)
    {
        std::lock_guard lock(m_Mutex);

        TokenEntry entry;
        entry.m_AccessToken = accessToken;
        entry.m_RefreshToken = refreshToken;
        entry.m_ExpiresAt = NowUnixSeconds() + expiresInSeconds;

        // Register tokens with SecretRedactor
        SecretRedactor::Get().AddSecret(accessToken);
        if (!refreshToken.empty())
        {
            SecretRedactor::Get().AddSecret(refreshToken);
        }

        m_Tokens[keyName] = std::move(entry);
        LOG_CORE_INFO("OAuthTokenManager: stored tokens for '{}' (expires in {} s)", keyName, expiresInSeconds);
    }

    bool OAuthTokenManager::HasValidToken(std::string const& keyName) const
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Tokens.find(keyName);
        if (it == m_Tokens.end())
        {
            return false;
        }
        return it->second.m_ExpiresAt == 0 || NowUnixSeconds() < it->second.m_ExpiresAt;
    }

    void OAuthTokenManager::RefreshLoop()
    {
        while (m_Running.load())
        {
            {
                std::unique_lock lock(m_Mutex);
                int64_t now = NowUnixSeconds();

                for (auto& [keyName, entry] : m_Tokens)
                {
                    // Skip if no expiry set, already refreshing, or not close to expiry
                    if (entry.m_ExpiresAt == 0 || entry.m_Refreshing)
                    {
                        continue;
                    }

                    int64_t secondsUntilExpiry = entry.m_ExpiresAt - now;
                    if (secondsUntilExpiry > REFRESH_BEFORE_EXPIRY_SECONDS)
                    {
                        continue;
                    }

                    if (entry.m_RefreshToken.empty())
                    {
                        LOG_CORE_WARN("OAuthTokenManager: token for '{}' expires in {} s but no refresh token",
                                      keyName, secondsUntilExpiry);
                        continue;
                    }

                    LOG_CORE_INFO("OAuthTokenManager: refreshing token for '{}' (expires in {} s)", keyName,
                                  secondsUntilExpiry);

                    entry.m_Refreshing = true;
                    lock.unlock();

                    bool success = RefreshToken(keyName, entry);

                    lock.lock();
                    entry.m_Refreshing = false;
                    m_Cv.notify_all();

                    if (!success)
                    {
                        LOG_CORE_ERROR("OAuthTokenManager: refresh failed for '{}'", keyName);
                    }
                }
            }

            // Sleep until next check or shutdown
            std::unique_lock lock(m_Mutex);
            m_Cv.wait_for(lock, std::chrono::seconds(REFRESH_CHECK_INTERVAL_SECONDS),
                          [this]() { return !m_Running.load(); });
        }
    }

    bool OAuthTokenManager::RefreshToken(std::string const& keyName, TokenEntry& entry)
    {
        // Token refresh requires an HTTP POST to the provider's token endpoint.
        // The actual HTTP call depends on the specific OAuth provider (OneDrive, Google, etc.).
        // This is a placeholder — concrete connectors will supply their token endpoint URL
        // and client credentials when they register with the OAuthTokenManager.
        //
        // For now, log the attempt. Phase 5 (OneDrive) will implement the full refresh flow
        // with provider-specific token endpoint configuration.
        LOG_CORE_INFO("OAuthTokenManager::RefreshToken: refresh for '{}' not yet implemented (requires provider config)",
                      keyName);
        return false;
    }
} // namespace AIAssistant
