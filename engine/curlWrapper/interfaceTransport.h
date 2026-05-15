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

#include <cstdint>
#include <functional>
#include <string>

#include "curlWrapper.h"

namespace AIAssistant
{
    // IInterfaceTransport — abstract bottom-half of the AI dispatch pipeline.
    //
    // The transport owns the per-request I/O backend (curl easy/multi handles for
    // LiveTransport; fixture replay for MockTransport).  Everything above the
    // transport — admission gate, AIMD controllers, retry queue, cascade-cancel
    // queue, inbox, debug counters, the I/O thread loop — stays in
    // CurlMultiDispatcher, which drives the transport through Submit / Pump /
    // Wait / Wakeup / CancelByCancelKey.
    //
    // Completion delivery: all responses (success, transport failure, setup
    // failure, cancellation) are routed back to the dispatcher via the
    // per-request CompletionCallback supplied at Submit time.  Completions
    // fire ONLY from Pump() — never synchronously from Submit or
    // CancelByCancelKey — so the dispatcher can safely populate its in-flight
    // map before any callback runs.
    //
    // Threading: Submit / CancelByCancelKey / Pump / Wait are called from the
    // dispatcher's I/O thread only.  Wakeup is thread-safe and may be called
    // from any thread (it interrupts a blocking Wait).
    class IInterfaceTransport
    {
    public:
        // Dispatcher-allocated correlation id.  Carried through Submit →
        // CompletionCallback so the dispatcher can match a completion back to
        // the PendingDispatch entry it stored at Submit time.
        using RequestId = uint64_t;

        struct Response
        {
            QueryResult m_Result;             // ok, transport-level error, or pre-flight failure
            std::string m_Body;               // response body bytes (capped at the body limit)
            std::string m_RawHeaders;         // unparsed header block — fed into ParseRateLimitHeaders
            long m_HttpStatus{0};             // CURLINFO_RESPONSE_CODE (0 if no HTTP exchange happened)
            std::string m_HttpVersionLabel;   // "HTTP/2", "HTTP/1.1", "HTTP/1.0", or empty
        };

        using CompletionCallback = std::function<void(RequestId, Response)>;

        virtual ~IInterfaceTransport() = default;

        // Enqueue a request for dispatch.  The dispatcher allocates `id`
        // before this call and pre-populates its in-flight map so a same-tick
        // completion (e.g. curl_easy_init / auth-signer failure surfaced on
        // the next Pump) finds its routing entry.  Implementations MUST NOT
        // fire `callback` synchronously from Submit — defer to Pump.
        virtual void Submit(RequestId id,
                            CurlWrapper::QueryData queryData,
                            CompletionCallback callback) = 0;

        // Abort every in-flight request whose QueryData.m_CancelKey matches
        // `cancelKey`.  Silent at the transport layer — handles are cleaned up
        // (curl_multi_remove_handle + slist free + curl_easy_cleanup) and the
        // entries are removed from the transport's in-flight map, but the
        // CompletionCallback is NOT fired.  The dispatcher synchronously
        // fires its own user callback with "request cancelled (run terminated)"
        // before calling this, so a transport-side completion would be a
        // double-fire.  Empty cancelKey is a no-op.
        virtual void CancelByCancelKey(std::string const& cancelKey) = 0;

        // Drive synchronous I/O once.  Runs curl_multi_perform, harvests any
        // completed transfers via curl_multi_info_read, and fires the
        // corresponding completion callbacks.  Does not block; the dispatcher
        // pairs this with Wait() in its I/O loop.
        virtual void Pump() = 0;

        // Block until socket activity or Wakeup(), bounded by `timeoutMs`.
        // Returns when there is work to do or the timeout expires.
        virtual void Wait(long timeoutMs) = 0;

        // Interrupt a blocked Wait() from any thread.  Thread-safe.
        virtual void Wakeup() = 0;

        // Count of requests currently in flight inside the transport (handles
        // added to curl_multi but not yet completed, plus any deferred
        // setup-failure completions waiting for the next Pump).  Used by the
        // dispatcher's shutdown loop to decide when in-flight work has drained.
        virtual size_t ActiveCount() const = 0;
    };

    // Shared host extraction.  Used by both the transport (DEBUG-only
    // localhost TLS-verify suppression) and the dispatcher (AIMD throttle
    // logging, per-host quota state).  Kept here so a single implementation
    // covers both files — the IPv6-literal branch was added once in response
    // to a bug and shouldn't be re-implemented per use site.
    //
    //   "https://api.openai.com/v1/chat/completions" → "api.openai.com"
    //   "https://[::1]:8443/path"                    → "::1"
    std::string ExtractHostFromUrl(std::string const& url);
} // namespace AIAssistant
