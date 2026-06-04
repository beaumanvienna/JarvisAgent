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
#include "curlWrapper/credValidation.h"
#include "engine.h"

#include <stdexcept>
#include <string>

namespace AIAssistant
{
    namespace
    {
        // Validate the API key for static-header signers (Bearer, XGoog, Anthropic, Azure).
        // Centralised so a future signer addition automatically benefits.
        bool ValidateApiKey(CurlWrapper::QueryData const& q, char const* styleName, std::string& errorMessage)
        {
            if (IsBlank(q.m_ApiKey.Get()))
            {
                errorMessage = std::string(styleName) + ": API key is empty or whitespace";
                return false;
            }
            return true;
        }

        class BearerSigner final : public IAuthSigner
        {
        public:
            [[nodiscard]] bool Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& /*publicHeaders*/,
                                     SecureString& secretHeader, std::string& errorMessage) const override
            {
                if (!ValidateApiKey(q, "Bearer", errorMessage)) return false;
                secretHeader.Format("Authorization: Bearer ", q.m_ApiKey.Get());
                return true;
            }
        };

        class XGoogApiKeySigner final : public IAuthSigner
        {
        public:
            [[nodiscard]] bool Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& /*publicHeaders*/,
                                     SecureString& secretHeader, std::string& errorMessage) const override
            {
                if (!ValidateApiKey(q, "XGoogApiKey", errorMessage)) return false;
                secretHeader.Format("x-goog-api-key: ", q.m_ApiKey.Get());
                return true;
            }
        };

        class AnthropicXApiKeySigner final : public IAuthSigner
        {
        public:
            [[nodiscard]] bool Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& publicHeaders,
                                     SecureString& secretHeader, std::string& errorMessage) const override
            {
                if (!ValidateApiKey(q, "AnthropicXApiKey", errorMessage)) return false;
                // Build the secret header first (mlock'd buffer); only after that
                // succeeds do we push the non-secret version header.  Order matters
                // for the "all-or-nothing" promise — if Format throws bad_alloc the
                // publicHeaders vector is still unmodified.
                secretHeader.Format("x-api-key: ", q.m_ApiKey.Get());
                publicHeaders.push_back("anthropic-version: 2023-06-01");
                return true;
            }
        };

        class AzureApiKeySigner final : public IAuthSigner
        {
        public:
            [[nodiscard]] bool Apply(CurlWrapper::QueryData const& q, std::vector<std::string>& /*publicHeaders*/,
                                     SecureString& secretHeader, std::string& errorMessage) const override
            {
                if (!ValidateApiKey(q, "AzureApiKey", errorMessage)) return false;
                secretHeader.Format("api-key: ", q.m_ApiKey.Get());
                return true;
            }
        };

    } // namespace

    IAuthSigner const& IAuthSigner::Get(CurlWrapper::AuthStyle style)
    {
        static BearerSigner const s_Bearer;
        static XGoogApiKeySigner const s_XGoog;
        static AnthropicXApiKeySigner const s_Anthropic;
        static AzureApiKeySigner const s_Azure;
        static SigV4Signer const s_SigV4;

        switch (style)
        {
            case CurlWrapper::AuthStyle::Bearer: return s_Bearer;
            case CurlWrapper::AuthStyle::XGoogApiKey: return s_XGoog;
            case CurlWrapper::AuthStyle::AnthropicXApiKey: return s_Anthropic;
            case CurlWrapper::AuthStyle::AzureApiKey: return s_Azure;
            case CurlWrapper::AuthStyle::AwsSigV4: return s_SigV4;
        }
        // Reachable only if a new AuthStyle variant is added without extending the
        // switch above.  -Wswitch catches the missing case at compile time on most
        // builds; this throw is the runtime backstop for the rest.  Pre-fix the
        // function silently fell back to Bearer here, which masked the bug as a
        // 401 from upstream — the same anti-debugging armor pattern called out
        // in CLAUDE.md's CurlMultiDispatcher silent-Bearer-fallback example.
        throw std::logic_error("IAuthSigner::Get: unhandled AuthStyle " +
                               std::to_string(static_cast<int>(style)));
    }
} // namespace AIAssistant
