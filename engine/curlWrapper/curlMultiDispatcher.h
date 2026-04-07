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
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "curlWrapper.h"

// Opaque libcurl types — avoid pulling in curl headers in this header.
typedef void CURLM;
struct curl_slist;

namespace AIAssistant
{
    // CurlMultiDispatcher — a single dedicated I/O thread that drives all AI HTTP/2
    // requests via libcurl's multi interface. All concurrent requests to the same host
    // share one TCP/TLS connection via HTTP/2 stream multiplexing; no thread-pool
    // threads are blocked on network I/O.
    //
    // Thread safety: Submit() is callable from any thread.
    // Callbacks fire on the I/O thread — keep them short (file writes are fine).
    class CurlMultiDispatcher
    {
    public:
        // callback(result, responseBody) fires on the I/O thread when the request completes.
        using Callback = std::function<void(QueryResult, std::string /*responseBody*/)>;

        CurlMultiDispatcher();
        ~CurlMultiDispatcher();

        CurlMultiDispatcher(CurlMultiDispatcher const&) = delete;
        CurlMultiDispatcher& operator=(CurlMultiDispatcher const&) = delete;

        // Enqueue a request. Thread-safe. Callback fires on the I/O thread.
        void Submit(CurlWrapper::QueryData const& data, Callback callback);

        // Phase-1 shutdown: signal the I/O thread to stop accepting new requests.
        void SignalStop();
        // Phase-2 shutdown: block until the I/O thread has exited.
        void WaitStop();

    private:
        struct PendingRequest
        {
            CurlWrapper::QueryData m_QueryData;
            Callback m_Callback;
        };

        // All data kept alive for the duration of one in-flight easy handle.
        // Accessed only from the I/O thread after being inserted into m_Active.
        struct ActiveRequest
        {
            CurlWrapper::QueryData m_QueryData;
            Callback m_Callback;
            std::string m_ReadBuffer;   // response body accumulates here
            std::string m_HeaderBuffer; // response headers accumulate here
            std::string m_Url;          // stable storage — CURLOPT_URL pointer target
            std::string m_PostData;     // stable storage — CURLOPT_POSTFIELDS pointer target
            struct curl_slist* m_Headers{nullptr};
            int m_RetryCount{0};        // number of 429 retries already attempted
        };

        // A request waiting for its retry delay to expire (I/O thread only).
        struct RetryEntry
        {
            std::chrono::steady_clock::time_point m_ReadyAt;
            PendingRequest m_Request;
            int m_RetryCount;
        };

        // Per-host adaptive rate limit state (I/O thread only).
        struct HostRateLimitState
        {
            int m_RemainingRequests{-1};  // -1 = unknown
            int m_RemainingTokens{-1};    // -1 = unknown
            std::chrono::steady_clock::time_point m_ResetAt;
            std::chrono::steady_clock::time_point m_LastUpdated;
        };

        void IoThreadFunc();
        void DrainInbox();
        void DrainCompleted();
        void DrainRetryQueue();
        CURL* SetupEasyHandle(ActiveRequest& req);

        // Parse rate limit headers from the accumulated header buffer.
        void ParseRateLimitHeaders(ActiveRequest const& req, std::string& host);
        // Extract host from URL (e.g. "api.openai.com" from "https://api.openai.com/v1/...").
        static std::string ExtractHost(std::string const& url);

        static constexpr int kMaxRetries = 5;            // max retries for 429 rate limit
        static constexpr int kMaxRetriesTransient = 2;  // max retries for transient HTTP errors (400, 500, 502, 503)
        static constexpr int kBaseRetryMs = 1000;       // 1 second base for exponential backoff
        static constexpr size_t kMaxActivePerHost = 48; // max concurrent HTTP/2 streams per host

        CURLM* m_MultiHandle{nullptr};
        std::thread m_IoThread;
        std::atomic<bool> m_Stopping{false};

        std::queue<PendingRequest> m_Inbox;
        std::mutex m_InboxMutex;

        // Keyed by CURL* easy handle. Accessed only from the I/O thread.
        std::unordered_map<CURL*, std::unique_ptr<ActiveRequest>> m_Active;

        // Retry queue — sorted by ready-at time (I/O thread only).
        std::vector<RetryEntry> m_RetryQueue;

        // Per-host rate limit tracking (I/O thread only).
        std::unordered_map<std::string, HostRateLimitState> m_HostRateLimits;
    };
} // namespace AIAssistant
