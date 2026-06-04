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

#include "curlWrapper/curlSlistHelper.h"

#include "engine.h"

#include <curl/curl.h>

namespace AIAssistant
{
    bool AppendSecretHeader(curl_slist*& list,
                            std::string_view prefix,
                            SecureString const& secret,
                            SecureString& scratch)
    {
        scratch.Format(prefix, secret.Get());
        curl_slist* updated = curl_slist_append(list, scratch.CStr());
        if (!updated)
        {
            LOG_CORE_CRITICAL("AppendSecretHeader: curl_slist_append failed");
            return false;
        }
        list = updated;
        return true;
    }
} // namespace AIAssistant
