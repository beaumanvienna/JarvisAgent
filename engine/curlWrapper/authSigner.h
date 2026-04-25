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

#include "curlWrapper/curlWrapper.h"

#include <string>
#include <vector>

namespace AIAssistant
{
    // Polymorphic auth strategy. Static-header styles (Bearer, x-api-key, etc.)
    // are trivial one-liners; SigV4 reads the full request and emits multiple
    // computed headers. Keeping them behind a single interface lets both
    // CurlWrapper::Query and CurlMultiDispatcher share auth handling without
    // either knowing about specific styles.
    //
    // Output is a list of complete header strings ("Name: value"); each caller
    // appends them to its own curl slist representation.
    class IAuthSigner
    {
    public:
        virtual ~IAuthSigner() = default;
        virtual void Apply(CurlWrapper::QueryData const& queryData, std::vector<std::string>& outHeaders) = 0;

        static IAuthSigner& Get(CurlWrapper::AuthStyle style);
    };
} // namespace AIAssistant
