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

#include <string>
#include <string_view>

namespace AIAssistant
{
    namespace EngineCore
    {
        // SHA-256(data), lowercase hex.  Returns the 64-char digest, or empty
        // string on the impossible OpenSSL-NULL path (logs LOG_CORE_ERROR
        // before returning empty so callers without run context still get a
        // breadcrumb; subsystems with run/workflow context add their own ERROR
        // line on top of an empty return).
        //
        // string_view input lets callers hand in a SecureString::Get() view
        // (e.g. the SigV4 canonical-request body containing the STS session
        // token) without a std::string materialisation step.
        std::string Sha256Hex(std::string_view data);
    }
}
