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
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

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
            std::string m_ReadBuffer; // response body accumulates here
            std::string m_Url;        // stable storage — CURLOPT_URL pointer target
            std::string m_PostData;   // stable storage — CURLOPT_POSTFIELDS pointer target
            struct curl_slist* m_Headers{nullptr};
        };

        void IoThreadFunc();
        void DrainInbox();
        void DrainCompleted();
        CURL* SetupEasyHandle(ActiveRequest& req);

        CURLM* m_MultiHandle{nullptr};
        std::thread m_IoThread;
        std::atomic<bool> m_Stopping{false};

        std::queue<PendingRequest> m_Inbox;
        std::mutex m_InboxMutex;

        // Keyed by CURL* easy handle. Accessed only from the I/O thread.
        std::unordered_map<CURL*, std::unique_ptr<ActiveRequest>> m_Active;
    };
} // namespace AIAssistant
