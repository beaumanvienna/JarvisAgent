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

    // After a failed refresh, reject re-attempts (both on-demand and from the background
    // loop) for this many seconds.  Prevents a refresh storm against the OAuth provider
    // when the refresh token has been revoked at the provider side.
    static constexpr int BACKOFF_AFTER_FAILURE_SECONDS = 60;

    // Reject server-supplied expires_in values <= 0 (would set m_ExpiresAt in the past
    // and trigger immediate re-refresh on the next GetAccessToken call) or absurdly
    // small values that would cause refresh thrashing.  60 seconds is a generous floor;
    // real-world tokens are 300s+ everywhere we've seen.
    static constexpr int64_t MIN_EXPIRES_IN_SECONDS = 60;

    // Default expiry to use when the server response omits or fails to parse expires_in.
    static constexpr int64_t DEFAULT_EXPIRES_IN_SECONDS = 3600;

    static int64_t NowUnixSeconds()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    namespace
    {
        // RAII for a curl_easy handle's escape allocations.  curl_easy_escape returns
        // a heap-allocated C string that must be released via curl_free.  Holding the
        // raw pointer across allocating string ops would leak on std::bad_alloc.
        struct CurlEscapedString
        {
            char* m_Ptr{nullptr};
            CurlEscapedString() = default;
            CurlEscapedString(CURL* curl, std::string_view src)
                : m_Ptr(curl ? curl_easy_escape(curl, src.data(), static_cast<int>(src.size())) : nullptr)
            {
            }
            ~CurlEscapedString() { if (m_Ptr) curl_free(m_Ptr); }
            CurlEscapedString(CurlEscapedString const&) = delete;
            CurlEscapedString& operator=(CurlEscapedString const&) = delete;

            // Returns the escaped value as a string_view (no std::string materialisation),
            // or the caller-supplied fallback on allocation failure (rare; the form-encoded
            // body is still syntactically valid for the common case).  The returned view is
            // valid until ~CurlEscapedString frees m_Ptr OR until the fallback source goes
            // out of scope — whichever comes first.  Used by PerformRefresh to build the
            // postBody directly into a SecureString via SecureString::Build.
            std::string_view view(std::string_view fallback) const
            {
                return m_Ptr ? std::string_view(m_Ptr) : fallback;
            }
        };
    } // namespace

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
        // Race-safe: two concurrent Start calls cannot both pass the check and both
        // spawn threads.  The earlier `if (m_Running.load()) return;` pattern was
        // racy — both threads could observe false, both call HydrateFromKeyManager,
        // and the second `m_RefreshThread = std::thread(...)` assignment to a still-
        // joinable thread would call std::terminate.
        bool expected = false;
        if (!m_Running.compare_exchange_strong(expected, true))
        {
            return;
        }
        // Roll back the m_Running flag if either Hydrate or thread construction throws.
        // Without rollback, Stop() would observe m_Running=true and try to join a
        // default-constructed (non-joinable) thread → std::system_error → terminate.
        try
        {
            HydrateFromKeyManager();
            m_RefreshThread = std::thread(&OAuthTokenManager::RefreshLoop, this);
        }
        catch (...)
        {
            m_Running.store(false);
            throw;
        }
        LOG_CORE_INFO("OAuthTokenManager: refresh loop started");
    }

    void OAuthTokenManager::HydrateFromKeyManager()
    {
        size_t hydrated = 0;
        auto const names = m_KeyManager.GetProviderNames();
        for (auto const& name : names)
        {
            // Pull the typed credential — only OAuthCredential entries are hydrated.
            // Non-OAuth subtypes are skipped silently.  Build a TokenEntry while we hold
            // the KeyManager's read-lock (inside the callback), then insert into m_Tokens
            // after release.  Two-step: first decide whether to hydrate + copy fields out,
            // then take m_Mutex and insert.
            TokenEntry entry;
            bool shouldHydrate = false;
            m_KeyManager.WithCredential(name,
                [&](ICredential const& cred)
                {
                    auto const* oauth = dynamic_cast<OAuthCredential const*>(&cred);
                    if (!oauth || oauth->m_RefreshToken.IsEmpty())
                    {
                        return;
                    }
                    // m_AccessToken intentionally left empty — forces refresh on first use.
                    entry.m_RefreshToken.Set(oauth->m_RefreshToken.Get());
                    entry.m_TokenEndpoint = oauth->m_TokenEndpoint;
                    entry.m_ClientId      = oauth->m_ClientId;
                    if (!oauth->m_ClientSecret.IsEmpty())
                    {
                        entry.m_ClientSecret.Set(oauth->m_ClientSecret.Get());
                    }
                    shouldHydrate = true;
                });
            if (!shouldHydrate)
            {
                continue;
            }

            std::lock_guard lock(m_Mutex);
            if (m_Tokens.find(name) != m_Tokens.end())
            {
                continue; // already hydrated (e.g. from an in-session OAuth callback)
            }
            entry.m_ExpiresAt = 0;                 // expired → next GetAccessToken triggers refresh

            SecretRedactor::Get().AddSecret(entry.m_RefreshToken.Get());
            if (!entry.m_ClientSecret.IsEmpty())
            {
                SecretRedactor::Get().AddSecret(entry.m_ClientSecret.Get());
            }

            m_Tokens[name] = std::move(entry);
            ++hydrated;
        }

        if (hydrated > 0)
        {
            LOG_CORE_INFO("OAuthTokenManager: hydrated {} OAuth token entry(ies) from KeyManager", hydrated);
        }
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

    bool OAuthTokenManager::GetAccessToken(std::string const& keyName, SecureString& outToken,
                                            std::string& errorMessage)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Tokens.find(keyName);
        if (it == m_Tokens.end())
        {
            errorMessage = "No OAuth tokens stored for '" + keyName + "'";
            return false;
        }

        // Wait if another thread is already refreshing this entry.  Re-find on each wake
        // because the entry could have been removed (RemoveTokens) while we waited.
        m_Cv.wait(lock,
                  [this, &keyName]()
                  {
                      auto inner = m_Tokens.find(keyName);
                      return inner == m_Tokens.end() || !inner->second.m_Refreshing;
                  });

        it = m_Tokens.find(keyName);
        if (it == m_Tokens.end())
        {
            errorMessage = "OAuth tokens for '" + keyName + "' were removed while waiting for refresh";
            return false;
        }

        TokenEntry& entry = it->second;

        int64_t const now = NowUnixSeconds();
        bool const tokenMissing = entry.m_AccessToken.IsEmpty();
        bool const tokenExpired = entry.m_ExpiresAt > 0 && now >= entry.m_ExpiresAt;
        bool const tokenUnset = entry.m_ExpiresAt == 0;
        bool const inBackoff = entry.m_LastRefreshFailureAt > 0 &&
                               now - entry.m_LastRefreshFailureAt < BACKOFF_AFTER_FAILURE_SECONDS;

        // Trigger a synchronous refresh if the access token is empty, unset, or already
        // expired.  This covers two cases:
        //   1. Hydrated-from-disk entries (empty access_token, ExpiresAt=0) on the first
        //      GetAccessToken call after startup.
        //   2. Any entry the background refresh loop has not yet picked up (e.g. the
        //      access_token expired between refresh loop ticks).
        // Backoff suppression: after a failed refresh we wait BACKOFF_AFTER_FAILURE_SECONDS
        // before attempting again — refresh-token revocation at the provider would otherwise
        // turn every GetAccessToken call into a network round-trip.
        if ((tokenMissing || tokenExpired || tokenUnset) && !entry.m_RefreshToken.IsEmpty() && !inBackoff)
        {
            LOG_CORE_INFO("OAuthTokenManager: on-demand refresh for '{}' (access_token {}, expires_at={})", keyName,
                          tokenMissing ? "empty" : "set", entry.m_ExpiresAt);

            // Snapshot the inputs RefreshToken needs.  After we release the lock, another
            // thread can call StoreTokens / RemoveTokens and the entry reference becomes
            // stale (StoreTokens overwrites the fields; RemoveTokens erases the entry).
            // unordered_map element references are stable across insertions, but the
            // CONTENTS at the reference can change — passing snapshots by value is the
            // only race-free way to do the network call without holding the lock.
            //
            // The two secret-bearing snapshots (clientSecret, refreshToken) are SecureString
            // so the secret bytes stay in mlock'd / zero-on-destruct memory for the duration
            // of the network call.
            std::string const snapTokenEndpoint = entry.m_TokenEndpoint;
            std::string const snapClientId     = entry.m_ClientId;
            SecureString snapClientSecret;
            SecureString snapRefreshToken;
            snapClientSecret.Set(entry.m_ClientSecret.Get());
            snapRefreshToken.Set(entry.m_RefreshToken.Get());
            entry.m_Refreshing = true;
            lock.unlock();

            RefreshResult result;
            std::string refreshErr;
            bool const success = RefreshToken(keyName, snapTokenEndpoint, snapClientId, snapClientSecret,
                                              snapRefreshToken, result, refreshErr);

            lock.lock();
            // The entry might have been removed while we were doing the network call.
            auto const reIt = m_Tokens.find(keyName);
            if (reIt != m_Tokens.end())
            {
                reIt->second.m_Refreshing = false;
                if (success)
                {
                    ApplyRefreshResult(keyName, result);
                }
                else
                {
                    reIt->second.m_LastRefreshFailureAt = NowUnixSeconds();
                }
            }
            m_Cv.notify_all();

            if (!success)
            {
                errorMessage = "On-demand refresh of OAuth token for '" + keyName + "' failed: " + refreshErr;
                return false;
            }

            // Re-find after apply (ApplyRefreshResult relookups internally; we need a
            // fresh iterator to read m_AccessToken below since the apply may have
            // happened against a re-found entry).
            it = m_Tokens.find(keyName);
            if (it == m_Tokens.end())
            {
                errorMessage = "OAuth tokens for '" + keyName + "' were removed mid-refresh";
                return false;
            }
        }
        else if (inBackoff && (tokenMissing || tokenExpired || tokenUnset))
        {
            errorMessage = "OAuth refresh for '" + keyName + "' is in backoff after recent failure";
            return false;
        }

        if (it->second.m_AccessToken.IsEmpty())
        {
            errorMessage = "No access token available for '" + keyName + "' (refresh token missing?)";
            return false;
        }

        // Copy the SecureString contents into the caller-owned SecureString.  No
        // std::string materialisation in either direction — the secret bytes stay in
        // mlock'd, zero-on-destruct memory the whole way through.
        outToken.Set(it->second.m_AccessToken.Get());
        return true;
    }

    void OAuthTokenManager::StoreTokens(std::string const& keyName, SecureString const& accessToken,
                                         SecureString const& refreshToken, int64_t expiresInSeconds,
                                         std::string const& tokenEndpoint, std::string const& clientId,
                                         SecureString const& clientSecret)
    {
        std::lock_guard lock(m_Mutex);

        TokenEntry entry;
        if (!accessToken.IsEmpty())  entry.m_AccessToken.Set(accessToken.Get());
        if (!refreshToken.IsEmpty()) entry.m_RefreshToken.Set(refreshToken.Get());
        if (!clientSecret.IsEmpty()) entry.m_ClientSecret.Set(clientSecret.Get());
        entry.m_TokenEndpoint = tokenEndpoint;
        entry.m_ClientId      = clientId;
        entry.m_ExpiresAt     = NowUnixSeconds() + expiresInSeconds;

        // Register tokens with SecretRedactor.  AddSecret takes string_view so the
        // SecureString::Get() views feed directly without materialising into a heap
        // std::string.
        SecretRedactor::Get().AddSecret(accessToken.Get());
        if (!refreshToken.IsEmpty()) SecretRedactor::Get().AddSecret(refreshToken.Get());
        if (!clientSecret.IsEmpty()) SecretRedactor::Get().AddSecret(clientSecret.Get());

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

    void OAuthTokenManager::RemoveTokens(std::string const& keyName)
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Tokens.find(keyName);
        if (it == m_Tokens.end())
        {
            return;
        }

        // Unregister secrets from the redactor.  This is best-effort cleanup; if any
        // of these strings happen to be substrings of other registered secrets the
        // redactor's internal dedupe already handles uniqueness.  Note: the redactor
        // intentionally never removes secrets that were leaked into logs already, so
        // any existing log lines that referenced these tokens stay redacted forever
        // (which is the right thing — a leaked stale token is still a leaked token).
        auto& entry = it->second;
        if (!entry.m_AccessToken.IsEmpty())  SecretRedactor::Get().RemoveSecret(entry.m_AccessToken.Get());
        if (!entry.m_RefreshToken.IsEmpty()) SecretRedactor::Get().RemoveSecret(entry.m_RefreshToken.Get());
        if (!entry.m_ClientSecret.IsEmpty()) SecretRedactor::Get().RemoveSecret(entry.m_ClientSecret.Get());

        m_Tokens.erase(it);
        // Wake any thread waiting on m_Refreshing for this entry — they'll re-find
        // the entry, see it's gone, and return an appropriate error.
        m_Cv.notify_all();
        LOG_CORE_INFO("OAuthTokenManager: removed tokens for '{}'", keyName);
    }

    void OAuthTokenManager::ApplyRefreshResult(std::string const& keyName, RefreshResult const& result)
    {
        // Caller holds m_Mutex.  Re-find the entry — it may have been removed by a
        // concurrent RemoveTokens / StoreTokens between the snapshot capture and now.
        auto it = m_Tokens.find(keyName);
        if (it == m_Tokens.end())
        {
            LOG_CORE_INFO("OAuthTokenManager: discarding refresh result for '{}' (entry removed)", keyName);
            return;
        }
        TokenEntry& entry = it->second;

        // Wiring sandwich: unregister old, Set new (zeroes prior buffer), register new.
        // result fields are SecureString; .Get() yields a string_view that Set() copies
        // into the entry's mlock'd buffer without a std::string step.
        if (!entry.m_AccessToken.IsEmpty()) SecretRedactor::Get().RemoveSecret(entry.m_AccessToken.Get());
        entry.m_AccessToken.Set(result.m_NewAccessToken.Get());
        SecretRedactor::Get().AddSecret(entry.m_AccessToken.Get());

        if (!result.m_NewRefreshToken.IsEmpty())
        {
            if (!entry.m_RefreshToken.IsEmpty()) SecretRedactor::Get().RemoveSecret(entry.m_RefreshToken.Get());
            entry.m_RefreshToken.Set(result.m_NewRefreshToken.Get());
            SecretRedactor::Get().AddSecret(entry.m_RefreshToken.Get());
        }

        entry.m_ExpiresAt = NowUnixSeconds() + result.m_ExpiresInSeconds;
        entry.m_LastRefreshFailureAt = 0; // Clear backoff on success
    }

    void OAuthTokenManager::RefreshLoop()
    {
        // Defensive try/catch wrapping the entire loop body.  Any std::bad_alloc or other
        // exception escaping this function would terminate the process via the unhandled
        // exception path on a detached/joined std::thread.  RefreshToken returns errors
        // by code so it shouldn't throw, but std::string ops in the snapshot capture or
        // the simdjson parser inside RefreshToken can.
        while (m_Running.load())
        {
            try
            {
                // Snapshot the work to do under lock.  We CANNOT range-iterate m_Tokens
                // and drop the lock mid-iteration: a concurrent StoreTokens that triggers
                // a rehash invalidates the for-loop iterator (unordered_map: insert may
                // invalidate iterators if rehash occurs; element references stay valid
                // but the iterator itself does not).  The pre-fix code dropped the lock
                // for the network call inside the loop body — UB on the next iteration
                // if a rehash had occurred.
                // m_ClientSecret and m_RefreshToken are SecureString so the secret bytes
                // stay in mlock'd / zero-on-destruct memory across the lock-release +
                // network-call window.  PendingRefresh is move-only (SecureString is
                // non-copyable, movable), so the std::vector<PendingRefresh>
                // push_back(std::move(...)) idiom below works.
                struct PendingRefresh
                {
                    std::string  m_KeyName;
                    std::string  m_TokenEndpoint;
                    std::string  m_ClientId;
                    SecureString m_ClientSecret;
                    SecureString m_RefreshToken;
                    int64_t      m_SecondsUntilExpiry{0};
                };
                std::vector<PendingRefresh> pending;

                {
                    std::lock_guard lock(m_Mutex);
                    int64_t const now = NowUnixSeconds();

                    for (auto& [keyName, entry] : m_Tokens)
                    {
                        if (entry.m_ExpiresAt == 0 || entry.m_Refreshing)
                        {
                            continue;
                        }

                        int64_t const secondsUntilExpiry = entry.m_ExpiresAt - now;
                        if (secondsUntilExpiry > REFRESH_BEFORE_EXPIRY_SECONDS)
                        {
                            continue;
                        }

                        if (entry.m_RefreshToken.IsEmpty())
                        {
                            LOG_CORE_WARN("OAuthTokenManager: token for '{}' expires in {} s but no refresh token",
                                          keyName, secondsUntilExpiry);
                            continue;
                        }

                        // Honor backoff window: skip refresh attempts within
                        // BACKOFF_AFTER_FAILURE_SECONDS of the last failure.
                        if (entry.m_LastRefreshFailureAt > 0 &&
                            now - entry.m_LastRefreshFailureAt < BACKOFF_AFTER_FAILURE_SECONDS)
                        {
                            continue;
                        }

                        // Mark refreshing under the same lock as the snapshot capture,
                        // so GetAccessToken sees the in-flight flag and waits.
                        entry.m_Refreshing = true;

                        PendingRefresh task;
                        task.m_KeyName        = keyName;
                        task.m_TokenEndpoint  = entry.m_TokenEndpoint;
                        task.m_ClientId       = entry.m_ClientId;
                        // Snapshot SecureString → SecureString for the network call (lock
                        // is dropped before RefreshToken runs).
                        task.m_ClientSecret.Set(entry.m_ClientSecret.Get());
                        task.m_RefreshToken.Set(entry.m_RefreshToken.Get());
                        task.m_SecondsUntilExpiry = secondsUntilExpiry;
                        pending.push_back(std::move(task));
                    }
                }

                // Network calls happen with the lock released.  Each call is independent;
                // we serialize them here for simplicity (parallel refresh would require
                // a worker pool and isn't worth the complexity at the current OAuth
                // provider count of 2).
                for (auto const& task : pending)
                {
                    LOG_CORE_INFO("OAuthTokenManager: refreshing token for '{}' (expires in {} s)", task.m_KeyName,
                                  task.m_SecondsUntilExpiry);

                    RefreshResult result;
                    std::string refreshErr;
                    bool const success = RefreshToken(task.m_KeyName, task.m_TokenEndpoint, task.m_ClientId,
                                                      task.m_ClientSecret, task.m_RefreshToken, result, refreshErr);

                    std::lock_guard lock(m_Mutex);
                    auto it = m_Tokens.find(task.m_KeyName);
                    if (it == m_Tokens.end())
                    {
                        // Entry was removed mid-refresh; discard the result.
                        continue;
                    }
                    it->second.m_Refreshing = false;
                    if (success)
                    {
                        ApplyRefreshResult(task.m_KeyName, result);
                    }
                    else
                    {
                        it->second.m_LastRefreshFailureAt = NowUnixSeconds();
                        LOG_CORE_ERROR("OAuthTokenManager: refresh failed for '{}': {}", task.m_KeyName, refreshErr);
                    }
                    m_Cv.notify_all();
                }
            }
            catch (std::exception const& e)
            {
                LOG_CORE_ERROR("OAuthTokenManager: RefreshLoop caught exception: {}", e.what());
            }
            catch (...)
            {
                LOG_CORE_ERROR("OAuthTokenManager: RefreshLoop caught unknown exception");
            }

            // Sleep until next check or shutdown
            std::unique_lock lock(m_Mutex);
            m_Cv.wait_for(lock, std::chrono::seconds(REFRESH_CHECK_INTERVAL_SECONDS),
                          [this]() { return !m_Running.load(); });
        }
    }

    bool OAuthTokenManager::RefreshToken(std::string const& keyName, std::string const& tokenEndpoint,
                                         std::string const& clientId, SecureString const& clientSecret,
                                         SecureString const& refreshToken, RefreshResult& out,
                                         std::string& errorMessage)
    {
        // This function holds NO lock and does not touch m_Tokens.  All inputs are
        // pass-by-const-reference snapshots taken by the caller under lock.  The
        // result lands in `out` for the caller to apply via ApplyRefreshResult.

        if (tokenEndpoint.empty())
        {
            errorMessage = "no token endpoint configured";
            LOG_CORE_WARN("OAuthTokenManager: no token endpoint configured for '{}', cannot refresh", keyName);
            return false;
        }

        if (clientId.empty())
        {
            errorMessage = "no client_id configured";
            LOG_CORE_WARN("OAuthTokenManager: no client_id configured for '{}', cannot refresh", keyName);
            return false;
        }

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            LOG_CORE_ERROR("OAuthTokenManager: curl_easy_init() failed for '{}'", keyName);
            return false;
        }

        // URL-encode every form field.  RFC 6749 doesn't restrict the refresh-token
        // charset; a token containing & or = (rare but legal) would otherwise break
        // the form-encoded body and produce a corrupted request the provider would
        // reject with an opaque "invalid_request" error.  CurlEscapedString ctor takes
        // std::string_view, so SecureString::Get() views feed directly without a
        // std::string materialisation step.
        CurlEscapedString const escRefreshToken(curl, refreshToken.Get());
        CurlEscapedString const escClientId(curl, clientId);
        CurlEscapedString const escClientSecret(curl, clientSecret.Get());

        // Build the form-urlencoded body directly into a SecureString — the body
        // contains the refresh_token AND (for confidential-client providers) the
        // client_secret, both of which are secret material.
        // CURLOPT_POSTFIELDS does NOT copy by default (per libcurl docs); the
        // SecureString must outlive curl_easy_perform — kept in this scope until
        // curl_easy_cleanup runs below.
        SecureString postBody;
        if (!clientSecret.IsEmpty())
        {
            // Confidential-client providers (Google) need client_secret; public clients
            // (Microsoft PKCE) do not.
            postBody.Build({
                "grant_type=refresh_token&refresh_token=", escRefreshToken.view(refreshToken.Get()),
                "&client_id=", escClientId.view(clientId),
                "&client_secret=", escClientSecret.view(clientSecret.Get()),
            });
        }
        else
        {
            postBody.Build({
                "grant_type=refresh_token&refresh_token=", escRefreshToken.view(refreshToken.Get()),
                "&client_id=", escClientId.view(clientId),
            });
        }

        std::string responseBody;
        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            buf->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, tokenEndpoint.c_str());
        // CURLOPT_POSTFIELDS is a no-copy pointer in libcurl — postBody must outlive
        // curl_easy_perform below.  postBody is a SecureString local to this function,
        // so its destruction at scope-end (after curl_easy_cleanup) satisfies that.
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.CStr());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody.Size()));
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
            errorMessage = std::string("curl_easy_perform: ") + curl_easy_strerror(res);
            LOG_CORE_ERROR("OAuthTokenManager: refresh request failed for '{}': {}", keyName,
                           curl_easy_strerror(res));
            return false;
        }

        if (httpCode != 200)
        {
            errorMessage = "HTTP " + std::to_string(httpCode);
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
            errorMessage = std::string("simdjson: ") + simdjson::error_message(parseError);
            LOG_CORE_ERROR("OAuthTokenManager: failed to parse refresh response for '{}': {}", keyName,
                           simdjson::error_message(parseError));
            return false;
        }

        std::string_view newAccessToken;
        if (doc["access_token"].get_string().get(newAccessToken))
        {
            errorMessage = "response missing access_token";
            LOG_CORE_ERROR("OAuthTokenManager: refresh response for '{}' missing access_token", keyName);
            return false;
        }
        // out.m_NewAccessToken / m_NewRefreshToken are SecureString — Set() copies
        // directly from the simdjson string_view into mlock'd memory without a
        // std::string intermediate.
        out.m_NewAccessToken.Set(newAccessToken);

        // Refresh-token rotation is optional — provider may or may not return a new one.
        std::string_view newRefreshToken;
        if (!doc["refresh_token"].get_string().get(newRefreshToken))
        {
            out.m_NewRefreshToken.Set(newRefreshToken);
        }

        // Validate expires_in: server-supplied <= 0 (or absurdly small) values would
        // set m_ExpiresAt in the past or near-past, triggering immediate re-refresh
        // on the next GetAccessToken call → refresh storm.  Floor at MIN_EXPIRES_IN_SECONDS
        // and fall back to DEFAULT_EXPIRES_IN_SECONDS if absent.
        int64_t expiresIn = DEFAULT_EXPIRES_IN_SECONDS;
        int64_t parsedExpiry = 0;
        if (!doc["expires_in"].get_int64().get(parsedExpiry))
        {
            expiresIn = parsedExpiry;
        }
        if (expiresIn < MIN_EXPIRES_IN_SECONDS)
        {
            LOG_CORE_WARN("OAuthTokenManager: refresh for '{}' returned expires_in={} below floor {}; "
                          "clamping to floor to prevent refresh storm",
                          keyName, expiresIn, MIN_EXPIRES_IN_SECONDS);
            expiresIn = MIN_EXPIRES_IN_SECONDS;
        }
        out.m_ExpiresInSeconds = expiresIn;

        LOG_CORE_INFO("OAuthTokenManager: successfully refreshed token for '{}' (expires in {} s)", keyName,
                       expiresIn);
        return true;
    }
} // namespace AIAssistant
