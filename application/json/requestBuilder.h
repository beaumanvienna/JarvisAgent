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

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "curlWrapper/curlWrapper.h"
#include "json/configParser.h"
#include "workflow/aiInvocation.h"

namespace AIAssistant
{
    class IRequestBuilder
    {
    public:
        virtual ~IRequestBuilder() = default;

        virtual std::string BuildBody(AiInvocation const& envelope, std::string const& model) const = 0;

        virtual std::string ResolveUrl(std::string const& baseUrl, std::string const& model) const = 0;

        virtual CurlWrapper::AuthStyle GetAuthStyle() const = 0;

        virtual std::unordered_map<std::string, std::string> GetExtraHeaders() const { return {}; }

        virtual bool SupportsNativeJsonSchema() const { return false; }
        virtual bool SupportsForcedToolShim() const { return false; }

        static std::unique_ptr<IRequestBuilder> Create(ConfigParser::EngineConfig::InterfaceType interfaceType);
    };
} // namespace AIAssistant
