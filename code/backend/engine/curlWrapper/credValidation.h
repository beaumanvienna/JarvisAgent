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

#include <cctype>
#include <string_view>

namespace AIAssistant
{
    // Shared by IAuthSigner implementations (authSigner.cpp, awsSigV4.cpp) for
    // pre-flight credential validation.  Catches the accidental empty-string AND
    // all-whitespace credential cases — both would otherwise produce a syntactically
    // valid request that bounces off the upstream provider as an opaque 401 with
    // no local diagnostic.
    //
    // Cast-to-unsigned-char before std::isspace avoids the cppreference UB for
    // negative chars (std::isspace requires int in [0, UCHAR_MAX] or EOF).
    inline bool IsBlank(std::string_view s)
    {
        for (char c : s)
        {
            if (!std::isspace(static_cast<unsigned char>(c)))
            {
                return false;
            }
        }
        return true;
    }
} // namespace AIAssistant
