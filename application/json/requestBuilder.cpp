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

#include "json/requestBuilder.h"

#include "engine.h"
#include "json/jsonHelper.h"

namespace AIAssistant
{
    namespace
    {
        std::string ConcatMessages(std::vector<Message> const& messages)
        {
            std::string combined;
            for (auto const& message : messages)
            {
                if (!combined.empty())
                {
                    combined += "\n";
                }
                combined += message.m_Content;
            }
            return combined;
        }

        class RequestBuilderAPI1 final : public IRequestBuilder
        {
        public:
            std::string BuildBody(AiInvocation const& envelope, std::string const& model) const override
            {
                std::string const sanitized = JsonHelper().SanitizeForJson(ConcatMessages(envelope.m_Messages));
                std::string json = R"({"model": ")" + model + R"(","messages": [{"role": "user", "content": ")" +
                                    sanitized + R"("}])";
                if (envelope.m_Settings.m_Temperature != 0.0)
                {
                    json += R"(,"temperature": )" + std::to_string(envelope.m_Settings.m_Temperature);
                }
                if (envelope.m_Settings.m_Seed.has_value())
                {
                    json += R"(,"seed": )" + std::to_string(envelope.m_Settings.m_Seed.value());
                }
                json += "}";
                return json;
            }

            std::string ResolveUrl(std::string const& baseUrl, std::string const& /*model*/) const override
            {
                return baseUrl;
            }

            CurlWrapper::AuthStyle GetAuthStyle() const override { return CurlWrapper::AuthStyle::Bearer; }

            bool SupportsNativeJsonSchema() const override { return true; }
            bool SupportsForcedToolShim() const override { return true; }
        };

        class RequestBuilderAPI2 final : public IRequestBuilder
        {
        public:
            std::string BuildBody(AiInvocation const& envelope, std::string const& model) const override
            {
                std::string const sanitized = JsonHelper().SanitizeForJson(ConcatMessages(envelope.m_Messages));
                std::string json = R"({"model": ")" + model + R"(", "input": ")" + sanitized + R"(", "store": false)";
                if (envelope.m_Settings.m_Temperature != 0.0)
                {
                    json += R"(, "temperature": )" + std::to_string(envelope.m_Settings.m_Temperature);
                }
                json += "}";
                return json;
            }

            std::string ResolveUrl(std::string const& baseUrl, std::string const& /*model*/) const override
            {
                return baseUrl;
            }

            CurlWrapper::AuthStyle GetAuthStyle() const override { return CurlWrapper::AuthStyle::Bearer; }

            bool SupportsNativeJsonSchema() const override { return true; }
            bool SupportsForcedToolShim() const override { return true; }
        };

        class RequestBuilderAPI3 final : public IRequestBuilder
        {
        public:
            std::string BuildBody(AiInvocation const& envelope, std::string const& /*model*/) const override
            {
                std::string const sanitized = JsonHelper().SanitizeForJson(ConcatMessages(envelope.m_Messages));
                std::string json =
                    R"({"contents": [{"parts": [{"text": ")" + sanitized + R"("}], "role": "user"}])";
                if (envelope.m_Settings.m_Temperature != 0.0)
                {
                    json += R"(, "generationConfig": {"temperature": )" +
                            std::to_string(envelope.m_Settings.m_Temperature) + "}";
                }
                json += "}";
                return json;
            }

            std::string ResolveUrl(std::string const& baseUrl, std::string const& model) const override
            {
                return baseUrl + "/models/" + model + ":generateContent";
            }

            CurlWrapper::AuthStyle GetAuthStyle() const override { return CurlWrapper::AuthStyle::XGoogApiKey; }

            bool SupportsNativeJsonSchema() const override { return true; }
            bool SupportsForcedToolShim() const override { return false; }
        };

        class RequestBuilderAPI4 final : public IRequestBuilder
        {
        public:
            std::string BuildBody(AiInvocation const& envelope, std::string const& model) const override
            {
                std::string const sanitized = JsonHelper().SanitizeForJson(ConcatMessages(envelope.m_Messages));
                int32_t const maxTokens = envelope.m_Settings.m_MaxTokens.value_or(4096);
                std::string json = R"({"model": ")" + model + R"(","max_tokens": )" + std::to_string(maxTokens) +
                                    R"(,"messages": [{"role": "user", "content": ")" + sanitized + R"("}])";
                if (envelope.m_Settings.m_Temperature != 0.0)
                {
                    json += R"(,"temperature": )" + std::to_string(envelope.m_Settings.m_Temperature);
                }
                json += "}";
                return json;
            }

            std::string ResolveUrl(std::string const& baseUrl, std::string const& /*model*/) const override
            {
                return baseUrl;
            }

            CurlWrapper::AuthStyle GetAuthStyle() const override
            {
                return CurlWrapper::AuthStyle::AnthropicXApiKey;
            }

            bool SupportsNativeJsonSchema() const override { return false; }
            bool SupportsForcedToolShim() const override { return true; }
        };
    } // anonymous namespace

    std::unique_ptr<IRequestBuilder> IRequestBuilder::Create(ConfigParser::EngineConfig::InterfaceType interfaceType)
    {
        switch (interfaceType)
        {
            case ConfigParser::EngineConfig::InterfaceType::API1:
                return std::make_unique<RequestBuilderAPI1>();
            case ConfigParser::EngineConfig::InterfaceType::API2:
                return std::make_unique<RequestBuilderAPI2>();
            case ConfigParser::EngineConfig::InterfaceType::API3:
                return std::make_unique<RequestBuilderAPI3>();
            case ConfigParser::EngineConfig::InterfaceType::API4:
                return std::make_unique<RequestBuilderAPI4>();
            default:
                LOG_APP_ERROR("IRequestBuilder::Create: unsupported interface type");
                return nullptr;
        }
    }
} // namespace AIAssistant
