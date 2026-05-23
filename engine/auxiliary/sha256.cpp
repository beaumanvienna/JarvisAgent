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

#include "auxiliary/sha256.h"

#include <openssl/sha.h>

#include "engine.h"

namespace AIAssistant
{
    namespace EngineCore
    {
        std::string Sha256Hex(std::string_view data)
        {
            unsigned char hash[SHA256_DIGEST_LENGTH];
            unsigned char const* result =
                SHA256(reinterpret_cast<unsigned char const*>(data.data()), data.size(), hash);
            if (result == nullptr)
            {
                LOG_CORE_ERROR("Sha256Hex: SHA256() returned NULL — output would be uninitialised, returning empty");
                return {};
            }

            static constexpr char const* kHex = "0123456789abcdef";
            std::string out(SHA256_DIGEST_LENGTH * 2, '0');
            for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i)
            {
                out[2 * i]     = kHex[(hash[i] >> 4) & 0x0F];
                out[2 * i + 1] = kHex[hash[i] & 0x0F];
            }
            return out;
        }
    }
}
