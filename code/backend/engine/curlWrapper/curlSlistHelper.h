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

#include "keys/secureString.h"

#include <string_view>

struct curl_slist;

namespace AIAssistant
{
    // Helper that builds a complete "Name: value" header containing secret material
    // and appends it to a curl slist — without ever materialising the secret into a
    // plain std::string heap allocation.
    //
    //   scratch.Format(prefix, secret.Get())  -> single mlock'd allocation
    //   curl_slist_append(list, scratch.CStr())  -> libcurl strdups its own copy
    //
    // The caller owns `scratch` (reusable across multiple secret-header builds in
    // the same HTTP request scope); it stays alive until curl_slist_free_all has
    // consumed the slist.  scratch wipes its mlock'd buffer on destruct (or on the
    // next Format call).
    //
    // Use sites: cloud connectors building "Authorization: Bearer <secret>" headers
    // (and similar Bearer-ish prefixes like "X-Redmine-API-Key: <secret>") that
    // currently do `"prefix" + credentials.m_Token` concatenation.  The concat
    // pattern allocates a heap-resident std::string containing the full secret;
    // the slab is not zeroed when the string destructs, leaving secret residue
    // recoverable from heap forensics.  This helper closes that window.
    //
    // Note: `prefix` is a public string view (header name + delimiter, e.g.
    // "Authorization: Bearer ") — never carries secret material.  Only the second
    // arg may contain secret bytes.  Returns true on success, false if
    // curl_slist_append returns nullptr (out-of-memory inside curl); the helper
    // already logs CRITICAL on failure so callers may discard the return value
    // for parity with the existing `headers = curl_slist_append(headers, ...)`
    // pattern where nullptr just propagates.
    bool AppendSecretHeader(curl_slist*& list,
                            std::string_view prefix,
                            SecureString const& secret,
                            SecureString& scratch);
} // namespace AIAssistant
