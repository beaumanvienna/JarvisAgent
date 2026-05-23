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

#include "keys/secureString.h"

#include "engine.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
#endif

namespace AIAssistant
{
    namespace
    {
        void SecureZero(void* ptr, size_t size)
        {
            if (!ptr || size == 0) return;
#ifdef _WIN32
            SecureZeroMemory(ptr, size);
#elif defined(__STDC_LIB_EXT1__)
            memset_s(ptr, size, 0, size);
#elif defined(__GLIBC__) || defined(__BIONIC__)
            explicit_bzero(ptr, size);
#else
            // Portable fallback (also used on Apple, where memset_s requires
            // __STDC_WANT_LIB_EXT1__ to be defined before <string.h>).
            // The compiler cannot optimise this away because the pointer is
            // laundered through a volatile variable.
            volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
            while (size--) *p++ = 0;
#endif
        }

        bool LockMemory(void* ptr, size_t size)
        {
            if (!ptr || size == 0) return true;
#ifdef _WIN32
            return VirtualLock(ptr, size) != 0;
#else
            return mlock(ptr, size) == 0;
#endif
        }

        void UnlockMemory(void* ptr, size_t size)
        {
            if (!ptr || size == 0) return;
#ifdef _WIN32
            VirtualUnlock(ptr, size);
#else
            munlock(ptr, size);
#endif
        }
    } // namespace

    SecureString::~SecureString() { Release(); }

    SecureString::SecureString(SecureString&& other) noexcept
        : m_Buffer(other.m_Buffer), m_Size(other.m_Size), m_Capacity(other.m_Capacity)
    {
        other.m_Buffer = nullptr;
        other.m_Size = 0;
        other.m_Capacity = 0;
    }

    SecureString& SecureString::operator=(SecureString&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_Buffer = other.m_Buffer;
            m_Size = other.m_Size;
            m_Capacity = other.m_Capacity;
            other.m_Buffer = nullptr;
            other.m_Size = 0;
            other.m_Capacity = 0;
        }
        return *this;
    }

    void SecureString::Set(std::string_view value)
    {
        // If the current buffer is big enough, reuse it; otherwise reallocate.
        // Reuse avoids leaving the old password paged anywhere after reallocation.
        size_t const needed = value.size() + 1; // room for trailing nul so Get() views are valid C strings if needed
        if (needed > m_Capacity)
        {
            Release();
            m_Buffer = static_cast<char*>(std::malloc(needed));
            if (!m_Buffer)
            {
                LOG_CORE_ERROR("SecureString::Set: allocation of {} bytes failed", needed);
                m_Capacity = 0;
                m_Size = 0;
                return;
            }
            m_Capacity = needed;
            if (!LockMemory(m_Buffer, m_Capacity))
            {
                // Non-fatal: we still have a valid buffer, it just may be swappable.
                LOG_CORE_WARN("SecureString::Set: memory lock failed ({} bytes) — swap protection disabled",
                              m_Capacity);
            }
        }
        else if (m_Size > 0)
        {
            // Reusing the buffer: wipe the previous contents so a shorter new value
            // does not leave residual bytes from the old (longer) value sitting in
            // the locked region until the next Clear()/Release().
            SecureZero(m_Buffer, m_Size);
        }
        std::memcpy(m_Buffer, value.data(), value.size());
        m_Buffer[value.size()] = '\0';
        m_Size = value.size();
    }

    void SecureString::Format(std::string_view prefix, std::string_view secretView,
                              std::string_view suffix)
    {
        Build({prefix, secretView, suffix});
    }

    void SecureString::Build(std::initializer_list<std::string_view> pieces)
    {
        size_t total = 0;
        for (auto const& p : pieces)
        {
            total += p.size();
        }
        size_t const needed = total + 1; // trailing NUL so CStr() is well-defined

        // Strong exception guarantee: assemble into a temporary mlock'd buffer
        // first, then swap.  A bad_alloc inside the allocator leaves *this
        // unchanged and the partial allocation is dropped.
        char* tmpBuffer = static_cast<char*>(std::malloc(needed));
        if (!tmpBuffer)
        {
            LOG_CORE_ERROR("SecureString::Build: allocation of {} bytes failed", needed);
            return;
        }
        if (!LockMemory(tmpBuffer, needed))
        {
            // Non-fatal: we still have a valid buffer, it just may be swappable.
            LOG_CORE_WARN("SecureString::Build: memory lock failed ({} bytes) — swap protection disabled",
                          needed);
        }
        char* cursor = tmpBuffer;
        for (auto const& p : pieces)
        {
            if (!p.empty())
            {
                std::memcpy(cursor, p.data(), p.size());
                cursor += p.size();
            }
        }
        *cursor = '\0';

        // Wipe + munlock + free the prior buffer (Release), then take ownership
        // of the freshly-built tmp.  Until this point *this was unchanged.
        Release();
        m_Buffer = tmpBuffer;
        m_Capacity = needed;
        m_Size = total;
    }

    std::string_view SecureString::Get() const
    {
        if (!m_Buffer || m_Size == 0)
        {
            return {};
        }
        return {m_Buffer, m_Size};
    }

    char const* SecureString::CStr() const
    {
        // m_Buffer is null when the SecureString has never held content.  Set()
        // and Format() always write a trailing NUL at m_Buffer[m_Size], so when
        // m_Buffer is non-null CStr() is safe to hand to C APIs even if Size()==0.
        return m_Buffer ? m_Buffer : "";
    }

    void SecureString::Clear()
    {
        if (m_Buffer && m_Capacity > 0)
        {
            SecureZero(m_Buffer, m_Capacity);
        }
        m_Size = 0;
    }

    void SecureString::Release()
    {
        if (m_Buffer)
        {
            SecureZero(m_Buffer, m_Capacity);
            UnlockMemory(m_Buffer, m_Capacity);
            std::free(m_Buffer);
            m_Buffer = nullptr;
        }
        m_Size = 0;
        m_Capacity = 0;
    }
} // namespace AIAssistant
