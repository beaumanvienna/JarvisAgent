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

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "curlWrapper/interfaceTransport.h"

// Opaque libcurl types — avoid pulling in curl headers in this header.
typedef void CURL;
typedef void CURLM;
struct curl_slist;

namespace AIAssistant
{
    // LiveTransport — the production HTTP/2 backend.  Owns the curl multi
    // handle, performs IAuthSigner integration, and harvests responses for
    // delivery back to the dispatcher.  Selection between LiveTransport and
    // MockTransport happens at the dispatcher level on the per-interface
    // `is_mock` flag.
    class LiveTransport : public IInterfaceTransport
    {
    public:
        LiveTransport();
        ~LiveTransport() override;

        LiveTransport(LiveTransport const&) = delete;
        LiveTransport& operator=(LiveTransport const&) = delete;

        void Submit(RequestId id, CurlWrapper::QueryData queryData, CompletionCallback callback) override;
        void CancelByCancelKey(std::string const& cancelKey) override;
        void Pump() override;
        void Wait(long timeoutMs) override;
        void Wakeup() override;
        size_t ActiveCount() const override;

    private:
        // Closed set of failure modes for SetupEasyHandle.  Replaces a fragile
        // string-prefix match on the error message that used to map to
        // QueryErrorCode at the call site.  Adding a variant triggers -Wswitch
        // at every consumer.
        enum class SetupError
        {
            None,
            CurlInit,        // curl_easy_init() returned NULL
            AuthSigner,      // IAuthSigner::Apply rejected the request
            MalformedHeader, // a request header carried a CR/LF (injection guard)
        };

        // All data kept alive for the duration of one in-flight easy handle.
        // Accessed only from the I/O thread after being inserted into m_Active.
        struct ActiveRequest
        {
            RequestId m_RequestId{0};
            CurlWrapper::QueryData m_QueryData;
            CompletionCallback m_Callback;
            std::string m_ReadBuffer;   // response body accumulates here
            std::string m_HeaderBuffer; // response headers accumulate here
            std::string m_Url;          // stable storage — CURLOPT_URL pointer target
            std::string m_PostData;     // stable storage — CURLOPT_POSTFIELDS pointer target
            struct curl_slist* m_Headers{nullptr};
        };

        // Deferred completion: a setup-failure detected synchronously inside
        // Submit gets parked here so the callback fires from Pump() (uniform
        // delivery point — the dispatcher never sees a synchronous callback).
        struct DeferredCompletion
        {
            RequestId m_RequestId{0};
            CompletionCallback m_Callback;
            Response m_Response;
        };

        // Returns the configured easy handle, or nullptr on failure.
        // On nullptr: errorKind names the closed-set failure mode and errorMessage
        // carries the human-readable reason; caller maps both into a QueryResult::Fail.
        [[nodiscard]] CURL* SetupEasyHandle(ActiveRequest& req,
                                            SetupError& errorKind,
                                            std::string& errorMessage);

        // Drain curl_multi_info_read; for each completed handle, build a
        // Response and fire the per-request CompletionCallback.  Called from
        // Pump() on the I/O thread.
        void HarvestCompletions();

        // Fire (and clear) any deferred completions queued by Submit's
        // setup-failure path.  Called from Pump().
        void DrainDeferredCompletions();

        CURLM* m_MultiHandle{nullptr};

        // Keyed by CURL* easy handle.  Mutated only from the I/O thread.
        std::unordered_map<CURL*, std::unique_ptr<ActiveRequest>> m_Active;

        // Setup-failure completions waiting for the next Pump.  I/O thread only.
        std::vector<DeferredCompletion> m_DeferredCompletions;
    };
} // namespace AIAssistant
