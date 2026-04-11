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

#include <curl/curl.h>

#include "engine.h"
#include "keys/oauthTokenManager.h"
#include "keys/keyManager.h"
#include "log/secretRedactor.h"
#include "curlWrapper/curlWrapper.h"
#include "simdjson/simdjson.h"

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
                                         std::string const& refreshToken, int64_t expiresInSeconds,
                                         std::string const& tokenEndpoint, std::string const& clientId)
    {
        std::lock_guard lock(m_Mutex);

        TokenEntry entry;
        entry.m_AccessToken = accessToken;
        entry.m_RefreshToken = refreshToken;
        entry.m_TokenEndpoint = tokenEndpoint;
        entry.m_ClientId = clientId;
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
        if (entry.m_TokenEndpoint.empty())
        {
            LOG_CORE_WARN("OAuthTokenManager: no token endpoint configured for '{}', cannot refresh", keyName);
            return false;
        }

        if (entry.m_ClientId.empty())
        {
            LOG_CORE_WARN("OAuthTokenManager: no client_id configured for '{}', cannot refresh", keyName);
            return false;
        }

        // Build refresh request body
        std::string postBody = "grant_type=refresh_token"
                               "&refresh_token=" + std::string(entry.m_RefreshToken) +
                               "&client_id=" + std::string(entry.m_ClientId);

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            LOG_CORE_ERROR("OAuthTokenManager: curl_easy_init() failed for '{}'", keyName);
            return false;
        }

        std::string responseBody;
        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            buf->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, entry.m_TokenEndpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            LOG_CORE_ERROR("OAuthTokenManager: refresh request failed for '{}': {}", keyName,
                           curl_easy_strerror(res));
            return false;
        }

        if (httpCode != 200)
        {
            LOG_CORE_ERROR("OAuthTokenManager: refresh for '{}' returned HTTP {} — {}", keyName, httpCode,
                           responseBody.size() < 500 ? responseBody : responseBody.substr(0, 500));
            return false;
        }

        // Parse the token response
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(responseBody);
        simdjson::ondemand::document doc;
        auto parseError = parser.iterate(paddedJson).get(doc);
        if (parseError)
        {
            LOG_CORE_ERROR("OAuthTokenManager: failed to parse refresh response for '{}': {}", keyName,
                           simdjson::error_message(parseError));
            return false;
        }

        std::string_view newAccessToken;
        if (doc["access_token"].get_string().get(newAccessToken))
        {
            LOG_CORE_ERROR("OAuthTokenManager: refresh response for '{}' missing access_token", keyName);
            return false;
        }

        // Remove old secrets before registering new ones
        SecretRedactor::Get().RemoveSecret(entry.m_AccessToken);

        entry.m_AccessToken = std::string(newAccessToken);
        SecretRedactor::Get().AddSecret(entry.m_AccessToken);

        // Update refresh token if a new one was provided (token rotation)
        std::string_view newRefreshToken;
        if (!doc["refresh_token"].get_string().get(newRefreshToken))
        {
            SecretRedactor::Get().RemoveSecret(entry.m_RefreshToken);
            entry.m_RefreshToken = std::string(newRefreshToken);
            SecretRedactor::Get().AddSecret(entry.m_RefreshToken);
        }

        // Update expiry
        int64_t expiresIn = 3600; // Default 1 hour
        int64_t parsedExpiry;
        if (!doc["expires_in"].get_int64().get(parsedExpiry))
        {
            expiresIn = parsedExpiry;
        }
        entry.m_ExpiresAt = NowUnixSeconds() + expiresIn;

        LOG_CORE_INFO("OAuthTokenManager: successfully refreshed token for '{}' (expires in {} s)", keyName,
                       expiresIn);
        return true;
    }
} // namespace AIAssistant
