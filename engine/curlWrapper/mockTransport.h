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

#include <string>
#include <unordered_set>
#include <vector>

#include "curlWrapper/interfaceTransport.h"

namespace AIAssistant
{
    // MockTransport — fixture-driven IInterfaceTransport for hermetic dispatch
    // tests, demo-JCWF replay, and CI without provider credit burn.  The
    // dispatcher's full code path above the transport (queueing, AIMD admission
    // gate, retry queue, parser dispatch) runs unchanged — only the bottom HTTP
    // layer is replaced with on-disk fixture replay.
    //
    // Selection is per-request, driven by `QueryData::m_IsMock`.  The dispatcher
    // routes mock-flagged requests here and live requests to `LiveTransport`.
    //
    // Cyber-sec posture is enforced at this boundary — the whole point of
    // accepting external paths (config-supplied fixture file, optional sibling
    // `.meta.json`) is that the boundary fails closed:
    //
    //   - Fixture paths are run through `ConfineUnderProjectRoot` at every load
    //     site (defense in depth — ConfigParser also validates at load time).
    //   - Fixture file size is capped at `kMaxFixtureBytes` (10 MiB).  Larger
    //     fixtures are rejected with ERROR and the request fails closed.
    //   - Optional sibling `<fixture>.meta.json` controls HTTP status + headers:
    //       * status must be in [200, 599]; rejected with ERROR otherwise
    //       * header keys are allowlisted to {Content-Type, Retry-After};
    //         non-allowlisted keys are dropped with WARN
    //   - Fixture bytes that enter log lines or the TUI go through
    //     `SanitizeUtf8` first (raw response body still propagates byte-for-byte
    //     to the parser — that's the test surface).
    //   - Operator transparency: INFO log on first call routed through
    //     MockTransport per provider after startup; PROV sidecar records
    //     `mocked: true` + the resolved `fixture_path`.
    //
    // Completion delivery follows the same Pump-only rule as LiveTransport —
    // synthetic responses queue into `m_PendingCompletions` from `Submit`, and
    // the next `Pump()` fires them.  This keeps the dispatcher's m_Active
    // population race-free even though responses are technically synchronous.
    class MockTransport : public IInterfaceTransport
    {
    public:
        // Per-fixture size cap — fixtures larger than this are rejected at
        // load time with ERROR and the request fails closed.  10 MiB is
        // generous for any realistic API response (typical AI replies are
        // <1 MiB; the largest captured Anthropic chunked output we've seen
        // is ~3 MiB).  Same order of magnitude as LiveTransport's 32 MiB
        // body cap.
        static constexpr size_t kMaxFixtureBytes = 10ULL * 1024 * 1024;

        MockTransport();
        ~MockTransport() override = default;

        MockTransport(MockTransport const&) = delete;
        MockTransport& operator=(MockTransport const&) = delete;

        void Submit(RequestId id, CurlWrapper::QueryData queryData, CompletionCallback callback) override;
        void CancelByCancelKey(std::string const& cancelKey) override;
        void Pump() override;
        void Wait(long timeoutMs) override;
        void Wakeup() override;
        size_t ActiveCount() const override;

    private:
        struct PendingCompletion
        {
            RequestId m_RequestId{0};
            std::string m_CancelKey;          // mirrored from QueryData for cancel filtering
            CompletionCallback m_Callback;
            Response m_Response;
        };

        // Load fixture bytes from a config-supplied path.  Performs (in order):
        // 1. ConfineUnderProjectRoot validation (rejects absolute escapes, symlink
        //    targets outside project root, `..` traversal that lands outside).
        // 2. File size check against kMaxFixtureBytes.
        // 3. Read the bytes into `outBody`.
        // On rejection, writes a structured ERROR log with `cancelKey` +
        // `quotaKey` substrings (so the dashboard run analyzer surfaces it) and
        // returns false.  The caller delivers QueryResult::Fail.
        [[nodiscard]] bool LoadFixtureBody(std::string const& fixturePath,
                                           std::string const& cancelKey,
                                           std::string const& quotaKey,
                                           std::string& outBody,
                                           std::string& outErrorMessage);

        // Parse optional `<fixture>.meta.json` for status + headers.  Missing
        // file is fine (defaults to status=200, no headers).  Malformed JSON,
        // out-of-allowlist http_status, or invalid types → ERROR + failure.
        // Non-allowlisted header keys are dropped with WARN but the rest of
        // the parse continues.
        [[nodiscard]] bool LoadFixtureMeta(std::string const& fixturePath,
                                           std::string const& cancelKey,
                                           std::string const& quotaKey,
                                           long& outHttpStatus,
                                           std::string& outRawHeaders,
                                           std::string& outErrorMessage);

        // Operator-transparency log: emit one INFO line per `(interfaceName,
        // fixturePath)` first-seen after startup.  Subsequent calls for the
        // same pair are silent so the log doesn't fill up under load.
        // QuotaKey is the closest stable identifier we have inside QueryData
        // for "the provider entry" — it's "<host>|<modelFamily>" composed by
        // AiRequestPool::Submit.
        void LogFirstSeenIfNeeded(std::string const& quotaKey,
                                  std::string const& fixturePath);

        // Pending completions queue.  I/O-thread-only access from the
        // dispatcher's loop — Submit pushes, Pump drains.
        std::vector<PendingCompletion> m_PendingCompletions;

        // Per-(quotaKey, fixturePath) first-seen tracker for the operator
        // transparency log.  Grows monotonically; bounded by the number of
        // distinct mock providers configured.
        std::unordered_set<std::string> m_FirstSeenKeys;
    };
} // namespace AIAssistant
