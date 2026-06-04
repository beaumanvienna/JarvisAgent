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

#include "curlWrapper/mockTransport.h"

#include <curl/curl.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>

#include "simdjson/simdjson.h"

#include "curlWrapper/authSigner.h"
#include "engine.h"
#include "file/pathConfinement.h"
#include "workflow/workflowTypes.h"  // SanitizeUtf8

namespace AIAssistant
{
    namespace fs = std::filesystem;

    namespace
    {
        // Header key allowlist for `<fixture>.meta.json`.  Keep tight: each entry
        // is something the dispatcher's AIMD parsers or downstream consumers
        // actually read from the response.  Adding a key here means committing
        // to honor it in mocks identically to a real provider.
        bool IsAllowedHeaderKey(std::string const& key)
        {
            // Case-insensitive comparison against the allowlist.
            std::string lower;
            lower.reserve(key.size());
            for (char c : key)
            {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return lower == "content-type" || lower == "retry-after";
        }

        // Process-global ring buffer of captured signing outputs.  Producer is
        // the dispatcher's I/O thread (one MockTransport::Submit at a time per
        // dispatcher); consumer is the web-server's debug-signals handler thread.
        // Mutex-guarded; capped at MockTransport::kMaxCapturedSignatures (FIFO
        // eviction).  Lives at file scope so the static getter can return a
        // snapshot without needing a live MockTransport instance pointer — tests
        // run against whatever dispatcher exists in the process.
        std::mutex& CapturedMutex()
        {
            static std::mutex m;
            return m;
        }
        std::deque<MockSignatureCapture>& CapturedRing()
        {
            static std::deque<MockSignatureCapture> ring;
            return ring;
        }

    } // namespace

    std::vector<MockSignatureCapture> MockTransport::GetRecentCapturedSignatures()
    {
        std::lock_guard<std::mutex> guard(CapturedMutex());
        return {CapturedRing().begin(), CapturedRing().end()};
    }

    MockTransport::MockTransport() = default;

    void MockTransport::Submit(RequestId id,
                               CurlWrapper::QueryData queryData,
                               CompletionCallback callback)
    {
        PendingCompletion pc;
        pc.m_RequestId = id;
        pc.m_CancelKey = queryData.m_CancelKey;
        pc.m_Callback  = std::move(callback);

        // The dispatcher should only route requests with m_IsMock=true here.
        // Defensive guard: if a non-mock request arrives, fail closed.
        if (!queryData.m_IsMock || queryData.m_FixturePath.empty())
        {
            LOG_CORE_ERROR("MockTransport: request lacks is_mock=true or fixture_path "
                           "cancelKey='{}' quotaKey='{}'",
                           queryData.m_CancelKey, queryData.m_QuotaKey);
            pc.m_Response.m_Result =
                QueryResult::Fail(QueryErrorCode::InvalidQueryData,
                                  "MockTransport requires is_mock=true and a non-empty fixture_path");
            m_PendingCompletions.push_back(std::move(pc));
            return;
        }

        LogFirstSeenIfNeeded(queryData.m_QuotaKey, queryData.m_FixturePath);

        std::string body;
        std::string errMsg;
        if (!LoadFixtureBody(queryData.m_FixturePath, queryData.m_CancelKey,
                             queryData.m_QuotaKey, body, errMsg))
        {
            pc.m_Response.m_Result = QueryResult::Fail(QueryErrorCode::InvalidQueryData, std::move(errMsg));
            m_PendingCompletions.push_back(std::move(pc));
            return;
        }

        long httpStatus = 200;
        std::string rawHeaders;
        std::string amzDateOverride;
        if (!LoadFixtureMeta(queryData.m_FixturePath, queryData.m_CancelKey,
                             queryData.m_QuotaKey, httpStatus, rawHeaders, amzDateOverride, errMsg))
        {
            pc.m_Response.m_Result = QueryResult::Fail(QueryErrorCode::InvalidQueryData, std::move(errMsg));
            m_PendingCompletions.push_back(std::move(pc));
            return;
        }

        // Thread the optional AmzDate override into the QueryData copy we use
        // for signing.  Mock-only — live paths leave the field empty and the
        // SigV4 signer falls back to FormatAmzDateNow().  This is what makes
        // the captured Authorization deterministic for signature KAT tests.
        if (!amzDateOverride.empty())
        {
            queryData.m_AmzDateOverride = amzDateOverride;
        }

        // Run the SigV4 signer to capture what would have been sent on the wire.
        // Scope: AwsSigV4 AND m_AwsCredential populated.  The other auth styles
        // (Bearer / x-api-key / x-goog-api-key / api-key) just compose a single
        // header from m_ApiKey and have nothing KAT-worthy to capture.  The
        // m_AwsCredential null check is what keeps parser-only fault tests
        // (test_api5_mock_errors uses API5 with empty key_name, which leaves
        // m_AwsCredential null) working — they don't care about the request
        // side and shouldn't be gated by the signer.  When the credential IS
        // populated, the caller is exercising the typed-credential pipeline
        // and signer failure is a real error: fail closed.
        if (queryData.m_AuthStyle == CurlWrapper::AuthStyle::AwsSigV4 &&
            queryData.m_AwsCredential != nullptr)
        {
            std::vector<std::string> capturedHeaders;
            SecureString capturedSecretHeader;  // SigV4 leaves this empty; included for the interface contract.
            std::string signErrMsg;
            auto const& signer = IAuthSigner::Get(queryData.m_AuthStyle);
            if (signer.Apply(queryData, capturedHeaders, capturedSecretHeader, signErrMsg))
            {
                std::lock_guard<std::mutex> guard(CapturedMutex());
                auto& ring = CapturedRing();
                while (ring.size() >= kMaxCapturedSignatures)
                {
                    ring.pop_front();
                }
                ring.push_back(MockSignatureCapture{queryData.m_CancelKey,
                                                    queryData.m_QuotaKey,
                                                    std::move(capturedHeaders)});
            }
            else
            {
                LOG_CORE_ERROR("MockTransport: SigV4 signer rejected request cancelKey='{}' "
                               "quotaKey='{}': {}",
                               queryData.m_CancelKey, queryData.m_QuotaKey, signErrMsg);
                pc.m_Response.m_Result = QueryResult::Fail(QueryErrorCode::InvalidQueryData,
                                                           std::move(signErrMsg));
                m_PendingCompletions.push_back(std::move(pc));
                return;
            }
        }

        pc.m_Response.m_Body              = std::move(body);
        pc.m_Response.m_RawHeaders        = std::move(rawHeaders);
        pc.m_Response.m_HttpStatus        = httpStatus;
        pc.m_Response.m_HttpVersionLabel  = "HTTP/2";  // Synthetic: dispatcher logs this verbatim.
        pc.m_Response.m_Result            = QueryResult::Ok();

        m_PendingCompletions.push_back(std::move(pc));
    }

    void MockTransport::CancelByCancelKey(std::string const& cancelKey)
    {
        if (cancelKey.empty())
        {
            return;
        }
        // Silent cleanup: the dispatcher synchronously fires the user callback
        // before calling this (same contract as LiveTransport).  We just drop
        // any matching deferred completions so Pump() doesn't double-fire.
        m_PendingCompletions.erase(
            std::remove_if(m_PendingCompletions.begin(), m_PendingCompletions.end(),
                           [&cancelKey](PendingCompletion const& pc)
                           {
                               return pc.m_CancelKey == cancelKey;
                           }),
            m_PendingCompletions.end());
    }

    void MockTransport::Pump()
    {
        if (m_PendingCompletions.empty())
        {
            return;
        }
        std::vector<PendingCompletion> local;
        local.swap(m_PendingCompletions);
        for (auto& pc : local)
        {
            if (pc.m_Callback)
            {
                pc.m_Callback(pc.m_RequestId, std::move(pc.m_Response));
            }
        }
    }

    void MockTransport::Wait(long /*timeoutMs*/)
    {
        // No-op: MockTransport has no socket activity to wait on.  The
        // dispatcher's loop also calls LiveTransport->Wait when both transports
        // are active, so blocking happens on the live side.  Returning
        // immediately is correct — if there's a pending mock completion the
        // next Pump fires it; if there isn't, the dispatcher's outer loop
        // will sleep on LiveTransport.
    }

    void MockTransport::Wakeup()
    {
        // No-op: no internal wait state to interrupt.
    }

    size_t MockTransport::ActiveCount() const
    {
        return m_PendingCompletions.size();
    }

    // ---------------------------------------------------------------------------
    // Fixture loading
    // ---------------------------------------------------------------------------

    bool MockTransport::LoadFixtureBody(std::string const& fixturePath,
                                        std::string const& cancelKey,
                                        std::string const& quotaKey,
                                        std::string& outBody,
                                        std::string& outErrorMessage)
    {
        // Path confinement — fail closed on any escape attempt.
        fs::path const confined = ConfineUnderProjectRoot(fixturePath);
        if (confined.empty())
        {
            LOG_CORE_ERROR("MockTransport: fixture path rejected by ConfineUnderProjectRoot "
                           "path='{}' cancelKey='{}' quotaKey='{}'",
                           fixturePath, cancelKey, quotaKey);
            outErrorMessage = "MockTransport: fixture path rejected (outside project root or symlink escape)";
            return false;
        }

        std::error_code ec;
        if (!fs::is_regular_file(confined, ec) || ec)
        {
            LOG_CORE_ERROR("MockTransport: fixture not a regular file path='{}' cancelKey='{}' quotaKey='{}' "
                           "errno='{}'",
                           confined.string(), cancelKey, quotaKey, ec.message());
            outErrorMessage = "MockTransport: fixture not a regular file";
            return false;
        }

        auto const size = fs::file_size(confined, ec);
        if (ec)
        {
            LOG_CORE_ERROR("MockTransport: cannot stat fixture path='{}' cancelKey='{}' quotaKey='{}' errno='{}'",
                           confined.string(), cancelKey, quotaKey, ec.message());
            outErrorMessage = "MockTransport: cannot stat fixture";
            return false;
        }
        if (size > kMaxFixtureBytes)
        {
            LOG_CORE_ERROR("MockTransport: fixture size {} exceeds cap {} path='{}' cancelKey='{}' quotaKey='{}'",
                           size, kMaxFixtureBytes, confined.string(), cancelKey, quotaKey);
            outErrorMessage = "MockTransport: fixture exceeds size cap";
            return false;
        }

        std::ifstream input(confined, std::ios::binary);
        if (!input)
        {
            LOG_CORE_ERROR("MockTransport: cannot open fixture path='{}' cancelKey='{}' quotaKey='{}'",
                           confined.string(), cancelKey, quotaKey);
            outErrorMessage = "MockTransport: cannot open fixture";
            return false;
        }
        outBody.resize(static_cast<size_t>(size));
        if (size > 0)
        {
            input.read(outBody.data(), static_cast<std::streamsize>(size));
            if (!input)
            {
                LOG_CORE_ERROR("MockTransport: short read on fixture path='{}' size={} cancelKey='{}' quotaKey='{}'",
                               confined.string(), size, cancelKey, quotaKey);
                outErrorMessage = "MockTransport: short read on fixture";
                return false;
            }
        }
        return true;
    }

    bool MockTransport::LoadFixtureMeta(std::string const& fixturePath,
                                        std::string const& cancelKey,
                                        std::string const& quotaKey,
                                        long& outHttpStatus,
                                        std::string& outRawHeaders,
                                        std::string& outAmzDateOverride,
                                        std::string& outErrorMessage)
    {
        outHttpStatus = 200;
        outRawHeaders.clear();
        outAmzDateOverride.clear();

        fs::path const metaPath = fs::path(fixturePath).string() + ".meta.json";
        fs::path const confined = ConfineUnderProjectRoot(metaPath.string());
        if (confined.empty())
        {
            // ConfineUnderProjectRoot returns empty if the path doesn't resolve
            // — either it's an escape (security failure) OR the meta file just
            // doesn't exist (legitimate "no override" case).  Distinguish via
            // is_regular_file on the unconfined-but-absolute form.
            std::error_code ec;
            fs::path const abs = fs::absolute(metaPath, ec);
            if (ec || !fs::exists(abs, ec) || ec)
            {
                // Meta file doesn't exist — defaults stand.
                return true;
            }
            LOG_CORE_ERROR("MockTransport: meta.json path rejected by ConfineUnderProjectRoot "
                           "path='{}' cancelKey='{}' quotaKey='{}'",
                           metaPath.string(), cancelKey, quotaKey);
            outErrorMessage = "MockTransport: meta.json path rejected (outside project root or symlink escape)";
            return false;
        }

        std::error_code ec;
        if (!fs::is_regular_file(confined, ec) || ec)
        {
            // Confined path exists but isn't a regular file (e.g. a directory
            // with the same name).  Treat as a config error.
            return true;  // defaults stand — the path-confinement passed, the file just isn't there
        }

        auto const size = fs::file_size(confined, ec);
        if (ec)
        {
            LOG_CORE_ERROR("MockTransport: cannot stat meta.json path='{}' cancelKey='{}' quotaKey='{}'",
                           confined.string(), cancelKey, quotaKey);
            outErrorMessage = "MockTransport: cannot stat meta.json";
            return false;
        }
        if (size > kMaxFixtureBytes)
        {
            LOG_CORE_ERROR("MockTransport: meta.json size {} exceeds cap {} path='{}' cancelKey='{}' quotaKey='{}'",
                           size, kMaxFixtureBytes, confined.string(), cancelKey, quotaKey);
            outErrorMessage = "MockTransport: meta.json exceeds size cap";
            return false;
        }

        std::string raw;
        raw.resize(static_cast<size_t>(size));
        {
            std::ifstream input(confined, std::ios::binary);
            if (!input)
            {
                LOG_CORE_ERROR("MockTransport: cannot open meta.json path='{}' cancelKey='{}' quotaKey='{}'",
                               confined.string(), cancelKey, quotaKey);
                outErrorMessage = "MockTransport: cannot open meta.json";
                return false;
            }
            if (size > 0)
            {
                input.read(raw.data(), static_cast<std::streamsize>(size));
                if (!input)
                {
                    LOG_CORE_ERROR("MockTransport: short read on meta.json path='{}' cancelKey='{}' quotaKey='{}'",
                                   confined.string(), cancelKey, quotaKey);
                    outErrorMessage = "MockTransport: short read on meta.json";
                    return false;
                }
            }
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(raw);
        simdjson::ondemand::document doc;
        if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("MockTransport: meta.json malformed path='{}' cancelKey='{}' quotaKey='{}'",
                           confined.string(), cancelKey, quotaKey);
            outErrorMessage = "MockTransport: meta.json malformed";
            return false;
        }

        simdjson::ondemand::object obj;
        if (doc.get_object().get(obj) != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("MockTransport: meta.json root must be an object path='{}' cancelKey='{}' quotaKey='{}'",
                           confined.string(), cancelKey, quotaKey);
            outErrorMessage = "MockTransport: meta.json root must be an object";
            return false;
        }

        for (auto field : obj)
        {
            std::string_view key;
            if (field.unescaped_key().get(key) != simdjson::SUCCESS)
                continue;

            if (key == "http_status")
            {
                int64_t status = 0;
                if (field.value().get_int64().get(status) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("MockTransport: meta.json 'http_status' must be int path='{}' cancelKey='{}' "
                                   "quotaKey='{}'", confined.string(), cancelKey, quotaKey);
                    outErrorMessage = "MockTransport: meta.json 'http_status' must be int";
                    return false;
                }
                if (status < 200 || status > 599)
                {
                    LOG_CORE_ERROR("MockTransport: meta.json 'http_status' {} outside allowlist [200,599] "
                                   "path='{}' cancelKey='{}' quotaKey='{}'",
                                   status, confined.string(), cancelKey, quotaKey);
                    outErrorMessage = "MockTransport: meta.json 'http_status' outside [200,599]";
                    return false;
                }
                outHttpStatus = static_cast<long>(status);
            }
            else if (key == "headers")
            {
                simdjson::ondemand::object headers;
                if (field.value().get_object().get(headers) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("MockTransport: meta.json 'headers' must be an object path='{}' cancelKey='{}' "
                                   "quotaKey='{}'", confined.string(), cancelKey, quotaKey);
                    outErrorMessage = "MockTransport: meta.json 'headers' must be an object";
                    return false;
                }
                for (auto hField : headers)
                {
                    std::string_view hKey;
                    if (hField.unescaped_key().get(hKey) != simdjson::SUCCESS)
                        continue;
                    std::string_view hVal;
                    if (hField.value().get_string().get(hVal) != simdjson::SUCCESS)
                    {
                        LOG_CORE_WARN("MockTransport: meta.json header '{}' value is not a string — skipping "
                                      "path='{}' cancelKey='{}' quotaKey='{}'",
                                      hKey, confined.string(), cancelKey, quotaKey);
                        continue;
                    }
                    std::string const keyStr(hKey);
                    if (!IsAllowedHeaderKey(keyStr))
                    {
                        LOG_CORE_WARN("MockTransport: meta.json header '{}' not in allowlist "
                                      "{{Content-Type, Retry-After}} — dropping "
                                      "path='{}' cancelKey='{}' quotaKey='{}'",
                                      keyStr, confined.string(), cancelKey, quotaKey);
                        continue;
                    }
                    // Synthesize a raw header block compatible with the
                    // existing rate-limit-strategy parsers: "Key: Value\r\n".
                    outRawHeaders.append(keyStr);
                    outRawHeaders.append(": ");
                    outRawHeaders.append(hVal);
                    outRawHeaders.append("\r\n");
                }
            }
            else if (key == "x_amz_date_override")
            {
                // Optional AWS SigV4 AmzDate override for signature KAT tests.
                // Format: "YYYYMMDDTHHMMSSZ" (matches the wire shape).  The
                // signer is permissive about the format (it just substring's
                // the first 8 chars for credentialScope's dateStamp), so we
                // do a basic length+shape check here rather than a full parse.
                std::string_view dateView;
                if (field.value().get_string().get(dateView) != simdjson::SUCCESS)
                {
                    LOG_CORE_ERROR("MockTransport: meta.json 'x_amz_date_override' must be a string "
                                   "path='{}' cancelKey='{}' quotaKey='{}'",
                                   confined.string(), cancelKey, quotaKey);
                    outErrorMessage = "MockTransport: meta.json 'x_amz_date_override' must be a string";
                    return false;
                }
                if (dateView.size() != 16 || dateView[8] != 'T' || dateView.back() != 'Z')
                {
                    LOG_CORE_ERROR("MockTransport: meta.json 'x_amz_date_override' '{}' does not match "
                                   "'YYYYMMDDTHHMMSSZ' shape path='{}' cancelKey='{}' quotaKey='{}'",
                                   dateView, confined.string(), cancelKey, quotaKey);
                    outErrorMessage = "MockTransport: meta.json 'x_amz_date_override' must be YYYYMMDDTHHMMSSZ";
                    return false;
                }
                outAmzDateOverride.assign(dateView);
            }
            // Future fields (body_path, etc.) parse here.  Unknown keys are
            // silently ignored — the schema is forward-compatible.
        }

        return true;
    }

    // ---------------------------------------------------------------------------
    // Operator-transparency log
    // ---------------------------------------------------------------------------

    void MockTransport::LogFirstSeenIfNeeded(std::string const& quotaKey,
                                              std::string const& fixturePath)
    {
        std::string const key = quotaKey + "|" + fixturePath;
        if (m_FirstSeenKeys.insert(key).second)
        {
            // Use SanitizeUtf8 on the fixture-path string defensively — config
            // values reach this log line and a malformed-UTF-8 path string
            // would corrupt the ncurses TUI.
            LOG_APP_INFO("AiRequestPool: provider quotaKey='{}' configured with is_mock=true; "
                         "serving from fixture '{}'",
                         SanitizeUtf8(quotaKey), SanitizeUtf8(fixturePath));
        }
    }
} // namespace AIAssistant
