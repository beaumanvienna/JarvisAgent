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
        void Set(std::string_view value);

        // View into the locked buffer. Valid until the next Set()/Clear()/destruction.
        // Returns an empty view when the string is empty.
        std::string_view Get() const;

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
