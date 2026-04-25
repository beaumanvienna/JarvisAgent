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

#include "curlWrapper/authSigner.h"
#include "curlWrapper/awsSigV4.h"
#include "engine.h"

namespace AIAssistant
{
    namespace
    {
        class BearerSigner final : public IAuthSigner
        {
        public:
            void Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& h) override
            {
                h.push_back("Authorization: Bearer " + q.m_ApiKey);
            }
        };

        class XGoogApiKeySigner final : public IAuthSigner
        {
        public:
            void Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& h) override
            {
                h.push_back("x-goog-api-key: " + q.m_ApiKey);
            }
        };

        class AnthropicXApiKeySigner final : public IAuthSigner
        {
        public:
            void Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& h) override
            {
                h.push_back("x-api-key: " + q.m_ApiKey);
                h.push_back("anthropic-version: 2023-06-01");
            }
        };

        class AzureApiKeySigner final : public IAuthSigner
        {
        public:
            void Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& h) override
            {
                h.push_back("api-key: " + q.m_ApiKey);
            }
        };

    } // namespace

    IAuthSigner& IAuthSigner::Get(CurlWrapper::AuthStyle style)
    {
        static BearerSigner s_Bearer;
        static XGoogApiKeySigner s_XGoog;
        static AnthropicXApiKeySigner s_Anthropic;
        static AzureApiKeySigner s_Azure;
        static SigV4Signer s_SigV4;

        switch (style)
        {
            case CurlWrapper::AuthStyle::Bearer: return s_Bearer;
            case CurlWrapper::AuthStyle::XGoogApiKey: return s_XGoog;
            case CurlWrapper::AuthStyle::AnthropicXApiKey: return s_Anthropic;
            case CurlWrapper::AuthStyle::AzureApiKey: return s_Azure;
            case CurlWrapper::AuthStyle::AwsSigV4: return s_SigV4;
        }
        return s_Bearer;
    }
} // namespace AIAssistant
