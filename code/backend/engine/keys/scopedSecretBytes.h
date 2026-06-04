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

#include <utility>
#include <vector>

#include <openssl/crypto.h>

namespace AIAssistant
{
    // RAII wrapper that holds secret-derived byte material in a std::vector<unsigned char>
    // and calls OPENSSL_cleanse() on the buffer in its destructor — including on throw
    // from any subsequent operation in the enclosing scope.  Sibling to SecureString: same
    // posture (zero-on-destruct, never copied), but for byte buffers (HMAC intermediates,
    // Base64-decoded keys) rather than NUL-terminated strings.
    //
    // OPENSSL_cleanse uses memory barriers to prevent the compiler from dead-store-
    // eliminating the zero — std::memset would be optimised away.  No mlock here because
    // the typical lifetime is microseconds during a single Sign() call; the swap-protection
    // story is owned by the upstream SecureString that fed the secret into this scope.
    //
    // Reference impls: code/backend/engine/curlWrapper/awsSigV4.cpp::Sign (SigV4 signing-key chain
    // kSecret → kDate → kRegion → kService → kSigning) and
    // code/backend/application/cloud/azureSharedKeySigner.cpp Sign().
    struct ScopedSecretBytes
    {
        std::vector<unsigned char> m_Data;

        ScopedSecretBytes() = default;
        explicit ScopedSecretBytes(std::vector<unsigned char>&& v) : m_Data(std::move(v)) {}

        ScopedSecretBytes(ScopedSecretBytes&&) noexcept = default;
        ScopedSecretBytes& operator=(ScopedSecretBytes&&) noexcept = default;

        ScopedSecretBytes(ScopedSecretBytes const&) = delete;
        ScopedSecretBytes& operator=(ScopedSecretBytes const&) = delete;

        ~ScopedSecretBytes()
        {
            if (!m_Data.empty())
            {
                OPENSSL_cleanse(m_Data.data(), m_Data.size());
            }
        }
    };
} // namespace AIAssistant
