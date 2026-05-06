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

#include "keys/secureString.h"

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

        // Seed in-memory token entries from persisted KeyManager provider configs.
        // Any provider with credential_type == "oauth" and a non-empty refresh_token is
        // hydrated with an empty access_token + m_ExpiresAt = 0 so the next GetAccessToken
        // call triggers a refresh using the stored refresh_token + client credentials.
        void HydrateFromKeyManager();

        // Get a valid access token for the named credential.
        // Blocks briefly if a refresh is in progress.
        // Returns empty string and populates errorMessage on failure.
        std::string GetAccessToken(std::string const& keyName, std::string& errorMessage);

        // Store initial tokens after OAuth consent flow completes.
        // tokenEndpoint: provider's token URL (e.g., "https://login.microsoftonline.com/.../token")
        // clientId: OAuth application client ID (needed for refresh requests)
        // clientSecret: OAuth client secret (required by confidential-client providers like Google)
        void StoreTokens(std::string const& keyName, std::string const& accessToken,
                         std::string const& refreshToken, int64_t expiresInSeconds,
                         std::string const& tokenEndpoint = {}, std::string const& clientId = {},
                         std::string const& clientSecret = {});

        // Check if a credential has valid (non-expired) tokens.
        bool HasValidToken(std::string const& keyName) const;

        // Drop a token entry and unregister its secrets from the redactor.  Intended
        // to be called when a credential is deleted from KeyManager so the in-memory
        // state stays in sync with on-disk state.  Idempotent (no-op if absent).
        void RemoveTokens(std::string const& keyName);

    private:
        struct TokenEntry
        {
            // Secret-bearing fields wrapped in SecureString so the plaintext lives in
            // mlock'd, zero-on-destruct memory.  Non-copyable, movable.  Per-field
            // mutations from RefreshLoop / StoreTokens / RemoveTokens follow the
            // unregister-old → Set(new) → register-new pattern with the redactor.
            SecureString m_AccessToken;
            SecureString m_RefreshToken;
            SecureString m_ClientSecret;     // OAuth client secret (confidential clients only)

            std::string m_TokenEndpoint;     // Provider's token URL for refresh — non-secret
            std::string m_ClientId;          // OAuth client ID for refresh — public per OAuth conventions

            int64_t m_ExpiresAt{0};          // Unix timestamp (seconds)
            int64_t m_LastRefreshFailureAt{0}; // Unix timestamp; 0 = no failure recorded.  Backoff window.
            bool m_Refreshing{false};        // True while a refresh is in flight
        };

        // Result of a successful network refresh — kept on the stack outside the lock
        // and applied back to the entry under lock by the caller.
        struct RefreshResult
        {
            std::string m_NewAccessToken;
            std::string m_NewRefreshToken; // Empty if server did not rotate the refresh token
            int64_t m_ExpiresInSeconds{0};
        };

        // Background thread: refreshes tokens expiring within 5 minutes.
        void RefreshLoop();

        // Perform the network refresh using snapshot inputs (taken under lock by the
        // caller, then passed in by value so this function holds NO lock and does not
        // touch the m_Tokens map).  Result lands in `out` on success.  Returns true
        // on success; populates errorMessage on failure (logged by caller).
        bool RefreshToken(std::string const& keyName, std::string const& tokenEndpoint,
                          std::string const& clientId, std::string const& clientSecret,
                          std::string const& refreshToken, RefreshResult& out, std::string& errorMessage);

        // Apply a successful refresh result to the named entry under lock.
        // Updates secret-redactor registrations atomically with the field updates.
        void ApplyRefreshResult(std::string const& keyName, RefreshResult const& result);

        KeyManager& m_KeyManager;

        mutable std::mutex m_Mutex;
        std::condition_variable m_Cv;
        std::unordered_map<std::string, TokenEntry> m_Tokens;

        std::thread m_RefreshThread;
        std::atomic<bool> m_Running{false};
    };
} // namespace AIAssistant
