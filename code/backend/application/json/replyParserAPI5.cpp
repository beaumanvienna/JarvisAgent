/* Copyright (c) 2026 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "json/replyParserAPI5.h"

#include "engine.h"
#include "json/replyParserAPI4.h"
#include "simdjson/simdjson.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    namespace
    {
        // Llama-on-Bedrock body: {"generation":"...","stop_reason":"...","prompt_token_count":N,"generation_token_count":M}
        class LlamaBedrockReply final : public ReplyParser
        {
        public:
            explicit LlamaBedrockReply(std::string const& jsonString) : ReplyParser(jsonString) { Parse(); }

            size_t HasContent() const override { return m_Generation.empty() ? 0 : 1; }
            std::string GetContent(size_t /*index*/) const override { return m_Generation; }
            std::string GetFinishReason() const override { return m_StopReason; }

            AiUsage GetUsage() const override
            {
                AiUsage u;
                u.m_InputTokens = static_cast<int32_t>(m_PromptTokens);
                u.m_OutputTokens = static_cast<int32_t>(m_CompletionTokens);
                u.m_TotalTokens = u.m_InputTokens + u.m_OutputTokens;
                return u;
            }

        private:
            void Parse()
            {
                simdjson::ondemand::parser parser;
                simdjson::padded_string padded(m_JsonString);
                auto docResult = parser.iterate(padded);
                if (docResult.error())
                {
                    m_State = State::ParseFailure;
                    m_HasError = true;
                    return;
                }
                simdjson::ondemand::document doc = std::move(docResult.value());

                std::string_view sv;
                if (doc["generation"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    m_Generation = SanitizeUtf8(std::string(sv));
                }
                if (doc["stop_reason"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    m_StopReason.assign(sv.data(), sv.size());
                }
                {
                    int64_t v = 0;
                    if (doc["prompt_token_count"].get_int64().get(v) == simdjson::SUCCESS) m_PromptTokens = static_cast<uint64_t>(v);
                    if (doc["generation_token_count"].get_int64().get(v) == simdjson::SUCCESS) m_CompletionTokens = static_cast<uint64_t>(v);
                }
                m_State = m_Generation.empty() ? State::ReplyError : State::ReplyOk;
                m_HasError = m_Generation.empty();
            }

            std::string m_Generation;
            std::string m_StopReason;
            uint64_t m_PromptTokens{0};
            uint64_t m_CompletionTokens{0};
        };

        // Titan / Nova on Bedrock: {"results":[{"outputText":"...","completionReason":"..."}],"inputTextTokenCount":N}
        class TitanBedrockReply final : public ReplyParser
        {
        public:
            explicit TitanBedrockReply(std::string const& jsonString) : ReplyParser(jsonString) { Parse(); }

            size_t HasContent() const override { return m_OutputText.empty() ? 0 : 1; }
            std::string GetContent(size_t /*index*/) const override { return m_OutputText; }
            std::string GetFinishReason() const override { return m_CompletionReason; }

            AiUsage GetUsage() const override
            {
                AiUsage u;
                u.m_InputTokens = static_cast<int32_t>(m_PromptTokens);
                u.m_OutputTokens = static_cast<int32_t>(m_CompletionTokens);
                u.m_TotalTokens = u.m_InputTokens + u.m_OutputTokens;
                return u;
            }

        private:
            void Parse()
            {
                simdjson::ondemand::parser parser;
                simdjson::padded_string padded(m_JsonString);
                auto docResult = parser.iterate(padded);
                if (docResult.error())
                {
                    m_State = State::ParseFailure;
                    m_HasError = true;
                    return;
                }
                simdjson::ondemand::document doc = std::move(docResult.value());

                {
                    int64_t v = 0;
                    if (doc["inputTextTokenCount"].get_int64().get(v) == simdjson::SUCCESS) m_PromptTokens = static_cast<uint64_t>(v);
                }

                simdjson::ondemand::array results;
                if (doc["results"].get_array().get(results) == simdjson::SUCCESS)
                {
                    for (auto element : results)
                    {
                        simdjson::ondemand::object obj;
                        if (element.get_object().get(obj) != simdjson::SUCCESS) continue;
                        std::string_view sv;
                        if (obj["outputText"].get_string().get(sv) == simdjson::SUCCESS)
                        {
                            m_OutputText = SanitizeUtf8(std::string(sv));
                        }
                        if (obj["completionReason"].get_string().get(sv) == simdjson::SUCCESS)
                        {
                            m_CompletionReason.assign(sv.data(), sv.size());
                        }
                        int64_t v = 0;
                        if (obj["tokenCount"].get_int64().get(v) == simdjson::SUCCESS)
                        {
                            m_CompletionTokens = static_cast<uint64_t>(v);
                        }
                        break; // single-result path — Titan returns one element for non-batch invokes
                    }
                }

                m_State = m_OutputText.empty() ? State::ReplyError : State::ReplyOk;
                m_HasError = m_OutputText.empty();
            }

            std::string m_OutputText;
            std::string m_CompletionReason;
            uint64_t m_PromptTokens{0};
            uint64_t m_CompletionTokens{0};
        };

        enum class BedrockFamily
        {
            Anthropic,
            Llama,
            Titan,
            AwsError,        // {"__type": "ServiceQuotaExceededException", "message": "..."}
            Unknown
        };

        // Sniff the JSON shape to identify the model family. Bedrock returns the
        // underlying family's native body without wrapping, so structural keys
        // disambiguate cleanly.  Success bodies are checked first (`content` /
        // `generation` / `results`); the AWS `__type` error envelope is the
        // fallback before declaring Unknown shape.
        BedrockFamily DetectFamily(std::string const& jsonString)
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(jsonString);
            auto docResult = parser.iterate(padded);
            if (docResult.error()) return BedrockFamily::Unknown;
            simdjson::ondemand::document doc = std::move(docResult.value());

            simdjson::ondemand::value v;
            if (doc["content"].get(v) == simdjson::SUCCESS) return BedrockFamily::Anthropic;
            if (doc["generation"].get(v) == simdjson::SUCCESS) return BedrockFamily::Llama;
            if (doc["results"].get(v) == simdjson::SUCCESS) return BedrockFamily::Titan;
            // simdjson ondemand is single-pass — re-iterate before probing `__type`.
            simdjson::ondemand::parser parser2;
            auto docResult2 = parser2.iterate(padded);
            if (docResult2.error()) return BedrockFamily::Unknown;
            simdjson::ondemand::document doc2 = std::move(docResult2.value());
            simdjson::ondemand::value awsTypeVal;
            if (doc2["__type"].get(awsTypeVal) == simdjson::SUCCESS) return BedrockFamily::AwsError;
            return BedrockFamily::Unknown;
        }

        // Parse the AWS error envelope into __type + message.  Caller's
        // ReplyParserAPI5 stores the strings as members for GetError to
        // classify without re-parsing.  Returns false on parse failure
        // (caller falls back to the "unrecognized shape" path).
        bool ExtractAwsError(std::string const& jsonString,
                             std::string& outType,
                             std::string& outMessage)
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(jsonString);
            auto docResult = parser.iterate(padded);
            if (docResult.error()) return false;
            simdjson::ondemand::document doc = std::move(docResult.value());

            std::string_view sv;
            if (doc["__type"].get_string().get(sv) != simdjson::SUCCESS) return false;
            outType.assign(sv.data(), sv.size());

            // __type sometimes includes a leading "com.amazonaws.bedrockruntime#"
            // prefix; strip it so classification matches the short name we see
            // in fixtures + AWS reference docs.
            auto const hashPos = outType.find('#');
            if (hashPos != std::string::npos && hashPos + 1 < outType.size())
            {
                outType.erase(0, hashPos + 1);
            }

            // message is optional but expected; absent → keep empty (the log
            // line still has __type which is the primary discriminator).
            if (doc["message"].get_string().get(sv) == simdjson::SUCCESS)
            {
                outMessage = SanitizeUtf8(std::string(sv));
            }
            return true;
        }

        // Map AWS Bedrock exception names to the UI-facing semantic category.
        // Names match the BedrockRuntime API exceptions documented at
        // docs.aws.amazon.com/bedrock/.  Unknown exception names fall through
        // to Unknown — caller still propagates m_ProviderErrorType for logs.
        ProviderErrorCategory ClassifyAwsBedrockException(std::string_view awsType)
        {
            if (awsType == "ServiceQuotaExceededException")   return ProviderErrorCategory::BillingExhausted;
            if (awsType == "ThrottlingException")             return ProviderErrorCategory::ThrottleRateLimit;
            if (awsType == "AccessDeniedException")           return ProviderErrorCategory::AuthFailure;
            if (awsType == "UnauthorizedOperation")           return ProviderErrorCategory::AuthFailure;
            if (awsType == "ValidationException")             return ProviderErrorCategory::InvalidRequest;
            if (awsType == "ResourceNotFoundException")       return ProviderErrorCategory::ModelNotFound;
            if (awsType == "ModelNotReadyException")          return ProviderErrorCategory::ServiceOverload;
            if (awsType == "ModelStreamErrorException")       return ProviderErrorCategory::ServiceOverload;
            if (awsType == "ModelTimeoutException")           return ProviderErrorCategory::ServiceOverload;
            if (awsType == "InternalServerException")         return ProviderErrorCategory::ServiceOverload;
            if (awsType == "ServiceUnavailableException")     return ProviderErrorCategory::ServiceOverload;
            return ProviderErrorCategory::Unknown;
        }
    } // namespace

    ReplyParserAPI5::ReplyParserAPI5(std::string const& jsonString) : ReplyParser(jsonString)
    {
        switch (DetectFamily(jsonString))
        {
            case BedrockFamily::Anthropic:
                m_Delegate = std::make_unique<ReplyParserAPI4>(jsonString);
                break;
            case BedrockFamily::Llama:
                m_Delegate = std::make_unique<LlamaBedrockReply>(jsonString);
                break;
            case BedrockFamily::Titan:
                m_Delegate = std::make_unique<TitanBedrockReply>(jsonString);
                break;
            case BedrockFamily::AwsError:
                // Pre-family error envelope — every Bedrock model surfaces the same
                // {"__type": "...", "message": "..."} shape on quota/throttle/auth
                // failures.  No delegate; GetError returns the classified AiError
                // directly using m_AwsErrorType / m_AwsErrorMessage.
                if (ExtractAwsError(jsonString, m_AwsErrorType, m_AwsErrorMessage))
                {
                    m_HasError = true;
                    m_State = State::ReplyError;
                }
                else
                {
                    m_HasError = true;
                    m_State = State::ParseFailure;
                }
                break;
            case BedrockFamily::Unknown:
                // No standalone log here: the parser has no run context. The error message
                // propagates via GetError()->m_Message into AiRequestPool::OnRequestFailed,
                // which logs at ERROR with runId/workflowId/taskId attached so the
                // dashboard run analyzer can attribute it to this run.
                m_HasError = true;
                m_State = State::ParseFailure;
                break;
        }
    }

    size_t ReplyParserAPI5::HasContent() const { return m_Delegate ? m_Delegate->HasContent() : 0; }

    std::string ReplyParserAPI5::GetContent(size_t index) const
    {
        return m_Delegate ? m_Delegate->GetContent(index) : std::string{};
    }

    AiError ReplyParserAPI5::GetError() const
    {
        // AWS error envelope path (ctor's BedrockFamily::AwsError branch).
        // m_AwsErrorType being non-empty means the body matched {"__type": "...", ...}
        // and ExtractAwsError populated the strings — return a Provider-kind AiError
        // with the AWS exception name as m_ProviderErrorType and the classified
        // category.  m_ProviderErrorCode stays empty (AWS doesn't have a separate
        // code field; __type IS the discriminator).
        if (!m_AwsErrorType.empty())
        {
            AiError e;
            e.m_Kind = AiError::Kind::Provider;
            e.m_Message = m_AwsErrorMessage;
            e.m_ProviderErrorType = m_AwsErrorType;
            e.m_Category = ClassifyAwsBedrockException(m_AwsErrorType);
            return e;
        }

        if (!m_Delegate)
        {
            AiError e;
            e.m_Kind = AiError::Kind::Parse;
            e.m_Message = "Bedrock reply: unrecognized response shape";
            return e;
        }
        return m_Delegate->GetError();
    }

    AiUsage ReplyParserAPI5::GetUsage() const { return m_Delegate ? m_Delegate->GetUsage() : AiUsage{}; }

    std::string ReplyParserAPI5::GetFinishReason() const
    {
        return m_Delegate ? m_Delegate->GetFinishReason() : std::string{};
    }
} // namespace AIAssistant
