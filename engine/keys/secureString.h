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

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

namespace AIAssistant
{
    // RAII buffer for sensitive strings (master password, etc.).
    // The backing memory is locked with mlock()/VirtualLock() so it is not
    // written to swap, and is zeroed with explicit_bzero()/SecureZeroMemory()
    // on destruction or reassignment.
    //
    // A process with ptrace or kernel access can still read this memory;
    // the goal is to defend against cold-boot/swap-file forensics and
    // accidental leakage through heap reuse.
    class SecureString
    {
    public:
        SecureString() = default;
        ~SecureString();

        SecureString(SecureString const&) = delete;
        SecureString& operator=(SecureString const&) = delete;

        SecureString(SecureString&& other) noexcept;
        SecureString& operator=(SecureString&& other) noexcept;

        // Copy the bytes of value into a locked buffer, replacing any prior contents.
        // The buffer is always allocated with room for a trailing '\0' after Size()
        // bytes, so CStr() is well-defined immediately after Set().
        void Set(std::string_view value);

        // Concatenate prefix || secretView || suffix into a single mlock'd buffer,
        // replacing any prior contents.  Used at the curl_slist_append boundary to
        // build "Authorization: Bearer <secret>" headers without ever materialising
        // the secret into a plain std::string heap allocation.
        //
        // Strong exception guarantee: on bad_alloc the prior contents are preserved
        // (copy-and-swap on a temporary internal buffer).  Like Set(), the buffer is
        // null-terminated past Size() so CStr() is well-defined on return.
        void Format(std::string_view prefix, std::string_view secretView,
                    std::string_view suffix = {});

        // N-piece variant of Format: concatenate the given pieces in order into a
        // single mlock'd buffer, replacing any prior contents.  Used at the curl
        // CURLOPT_POSTFIELDS boundary to build secret-bearing form-urlencoded bodies
        // (OAuth refresh request body, JWT-bearer grant body, etc.) without ever
        // materialising the secret into a plain std::string heap allocation.
        //
        // Strong exception guarantee: on bad_alloc the prior contents are preserved
        // (copy-and-swap on a temporary internal buffer).  Like Set() and Format(),
        // the buffer is null-terminated past Size() so CStr() is well-defined on
        // return.  Empty pieces (size 0) are handled correctly (no-op for that
        // piece).
        void Build(std::initializer_list<std::string_view> pieces);

        // View into the locked buffer. Valid until the next Set()/Format()/Clear()/destruction.
        // Returns an empty view when the string is empty.
        std::string_view Get() const;

        // Null-terminated pointer into the locked buffer (or "" when empty).  Valid
        // until the next Set()/Format()/Clear()/destruction.  Used to hand the
        // contents to C APIs that require a NUL-terminated const char* (libcurl's
        // curl_slist_append etc.).  Internally relies on the trailing-NUL invariant
        // maintained by Set() and Format().
        char const* CStr() const;

        bool IsEmpty() const { return m_Size == 0; }
        size_t Size() const { return m_Size; }

        // Zero the buffer and release the lock.
        void Clear();

    private:
        void Release();

        char* m_Buffer{nullptr};
        size_t m_Size{0};
        size_t m_Capacity{0};
    };
} // namespace AIAssistant
