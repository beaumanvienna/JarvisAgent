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

#include "assistant/assistantHelpers.h"
#include "engine.h"

#include <openssl/rand.h>

#include <vector>

namespace AIAssistant
{
    std::string RandomHex(std::size_t numBytes)
    {
        std::vector<unsigned char> buf(numBytes);
        if (RAND_bytes(buf.data(), static_cast<int>(numBytes)) != 1)
        {
            LOG_CORE_ERROR("AssistantHelpers: RAND_bytes failed ({} bytes)", numBytes);
            return {};
        }
        static constexpr char const* hex = "0123456789abcdef";
        std::string out(numBytes * 2, '0');
        for (std::size_t i = 0; i < numBytes; ++i)
        {
            out[2 * i] = hex[(buf[i] >> 4) & 0xF];
            out[2 * i + 1] = hex[buf[i] & 0xF];
        }
        return out;
    }

    bool IsValidOpaqueId(std::string const& id)
    {
        if (id.empty() || id.size() > 128)
            return false;
        for (char c : id)
        {
            bool const ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                            c == '-';
            if (!ok)
                return false;
        }
        return true;
    }
} // namespace AIAssistant
