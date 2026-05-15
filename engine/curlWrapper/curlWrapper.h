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
#include <string>
#include <unordered_map>

typedef void CURL;
struct curl_slist;

namespace AIAssistant
{
    // Unified error code scheme for AI query results:
    //   0        = success
    //   1-99     = CURLcode (libcurl transport errors, used as-is)
    //   100-599  = HTTP status codes (RFC 9110)
    //   1000+    = custom AI provider / pre-flight errors
    namespace QueryErrorCode
    {
        constexpr int Success = 0;

        // Pre-flight / internal errors (1000+)
        constexpr int NoApiKey = 1000;
        constexpr int InvalidQueryData = 1001;
        constexpr int CurlNotInitialized = 1002;
        constexpr int ParserError = 1003;
        constexpr int EmptyResponse = 1004;
        constexpr int ExceptionThrown = 1005;

        // AI provider errors (1100+) — reserved for future use
        constexpr int QuotaExceeded = 1100;
        constexpr int ContextWindowExceeded = 1101;
        constexpr int ContentPolicyViolation = 1102;
        constexpr int ModelNotFound = 1103;

        // Returns a human-readable label for the error code.
        std::string Describe(int code);
    } // namespace QueryErrorCode

    struct QueryResult
    {
        bool m_Ok{false};
        int m_ErrorCode{0};         // 0=success, 1-99=CURLcode, 100-599=HTTP, 1000+=custom
        std::string m_ErrorMessage; // human-readable description

        static QueryResult Ok() { return {true, 0, {}}; }
        static QueryResult Fail(int code, std::string message) { return {false, code, std::move(message)}; }
    };

    class CurlWrapper
    {
    public:
        enum class AuthStyle
        {
            Bearer = 0,          // Authorization: Bearer <key> (OpenAI chat+Responses APIs)
            XGoogApiKey,         // x-goog-api-key: <key> (Google Gemini native)
            AnthropicXApiKey,    // x-api-key: <key> + anthropic-version: 2023-06-01 (Anthropic Messages)
            AzureApiKey,         // api-key: <key> (Azure OpenAI deployment URLs)
            AwsSigV4             // AWS SigV4: signs the request body, emits Authorization+X-Amz-Date+X-Amz-Content-Sha256
        };

        struct QueryData
        {
            std::string m_Url;
            std::string m_Data;
            std::string m_ApiKey;
            AuthStyle m_AuthStyle{AuthStyle::Bearer};
            long m_TimeoutMs{0}; // 0 = no timeout (default); >0 = max transfer time in ms
            // Per-provider auxiliary fields read by signers (e.g. SigV4 needs region;
            // Bedrock dual-secret puts secret_access_key + session_token here).
            std::unordered_map<std::string, std::string> m_Params{};
            // ConfigParser::EngineConfig::InterfaceType encoded as int to keep this
            // header free of a configParser.h dependency.  -1 = unknown / not set;
            // matches ConfigParser::EngineConfig::InterfaceType::InvalidAPI semantics.
            // CurlMultiDispatcher uses this to dispatch to the right
            // IRateLimitStrategy when parsing response headers.
            int m_InterfaceType{-1};
            // Opaque "<host>|<modelFamily>" composed by AiRequestPool::Submit.
            // Used as the dispatcher's controller-map key so per-(host, modelFamily)
            // AIMD signals stay independent (Anthropic Sonnet vs Opus on same host).
            // Empty string = "one bucket per host" (controller derives from URL).
            std::string m_QuotaKey;
            // strategy.EstimateInputTokens(prompt) computed once at submit time.
            // Used by the controller's token-bucket projection.  -1 = unknown.
            int64_t m_EstimatedInputTokens{-1};
            // Stable per-request identifier used by CancelByCancelKey to abort
            // matching requests across the dispatcher's inbox / retry queue /
            // active set.  AiRequestPool::Submit fills this with the request's
            // expectedOutputPath (unique per workflow task).  Empty = request
            // is not cancellable through the cascade path (legacy callers).
            std::string m_CancelKey;
            // Per-interface rate-limit knobs resolved at submit time from
            // config.rate_limit.  Forwarded to the dispatcher to drive the
            // controller's hardCap, the 429 retry budget, transient retry
            // budget, and base backoff delay.  Sentinel discipline:
            //   -1  (or any negative) = "unset, dispatcher uses its default"
            //   0                     = "explicit zero" (no retries, no concurrency)
            //   >0                    = explicit value
            // Keeps QueryData usable for legacy callers (assistant, jcwfService)
            // that don't pre-resolve config — they leave the field at -1.
            int m_MaxConcurrency{-1};
            int m_MaxRetries429{-1};
            int m_MaxRetriesTransient{-1};
            int m_BaseRetryMs{-1};
            // Mock-transport routing.  Set by AiRequestPool::Submit when the
            // resolved ApiInterface has is_mock=true; the dispatcher routes
            // through MockTransport instead of LiveTransport when both are
            // present.  fixture_path is the on-disk path to the canned
            // response body (a sibling `<fixture>.meta.json` may override
            // HTTP status + headers).  ConfineUnderProjectRoot enforcement
            // happens at the transport boundary; defense in depth against
            // the config-parse-time check.
            bool m_IsMock{false};
            std::string m_FixturePath;

            bool IsValid() const;
        };

        // type alias for curl write callback
        using CurlWriteCallback = size_t (*)(void*, size_t, size_t, void*);

        // RAII for curl_slist
        class CurlSlist
        {
        public:
            CurlSlist() = default;
            ~CurlSlist();

            void Append(std::string const& str);
            struct curl_slist* Get();

        private:
            struct curl_slist* m_List{nullptr};
        };

    public:
        CurlWrapper();
        ~CurlWrapper();

        // avoid accidental copies (which would double free CURL*)
        CurlWrapper(CurlWrapper const&) = delete;
        CurlWrapper& operator=(CurlWrapper const&) = delete;

        // move semantics: default
        CurlWrapper(CurlWrapper&&) = default;
        CurlWrapper& operator=(CurlWrapper&&) noexcept = default;

        bool IsInitialized() const;
        QueryResult Query(QueryData const& queryData);
        std::string& GetBuffer();
        void Clear();

        static void GlobalCleanup();
        static std::string const& GetCaBundlePath();

    private:
        bool m_Initialized{false};
        static std::atomic<uint32_t> m_QueryCounter;
        CURL* m_Curl{nullptr};
        std::string m_ReadBuffer;
    };
} // namespace AIAssistant
