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

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace AIAssistant
{
    class KeyManager;

    // Manages OAuth 2.0 token lifecycle: stores access/refresh tokens, tracks expiry,
    // and runs a background refresh loop. Thread-safe for concurrent token access.
    class OAuthTokenManager
    {
    public:
        explicit OAuthTokenManager(KeyManager& keyManager);
        ~OAuthTokenManager();

        // Start/stop the background refresh thread.
        void Start();
        void Stop();

        // Get a valid access token for the named credential.
        // Blocks briefly if a refresh is in progress.
        // Returns empty string and populates errorMessage on failure.
        std::string GetAccessToken(std::string const& keyName, std::string& errorMessage);

        // Store initial tokens after OAuth consent flow completes.
        void StoreTokens(std::string const& keyName, std::string const& accessToken,
                         std::string const& refreshToken, int64_t expiresInSeconds);

        // Check if a credential has valid (non-expired) tokens.
        bool HasValidToken(std::string const& keyName) const;

    private:
        struct TokenEntry
        {
            std::string m_AccessToken;
            std::string m_RefreshToken;
            int64_t m_ExpiresAt{0};    // Unix timestamp (seconds)
            bool m_Refreshing{false};  // True while a refresh is in flight
        };

        // Background thread: refreshes tokens expiring within 5 minutes.
        void RefreshLoop();

        // Refresh a single token entry. Returns true on success.
        bool RefreshToken(std::string const& keyName, TokenEntry& entry);

        KeyManager& m_KeyManager;

        mutable std::mutex m_Mutex;
        std::condition_variable m_Cv;
        std::unordered_map<std::string, TokenEntry> m_Tokens;

        std::thread m_RefreshThread;
        std::atomic<bool> m_Running{false};
    };
} // namespace AIAssistant
