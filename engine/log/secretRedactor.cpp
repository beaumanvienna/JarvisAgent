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

#include <algorithm>

#include "log/secretRedactor.h"

namespace AIAssistant
{
    static constexpr char REDACTED[] = "[REDACTED]";
    static constexpr size_t REDACTED_LEN = sizeof(REDACTED) - 1;
    static constexpr size_t MIN_SECRET_LENGTH = 4;

    SecretRedactor& SecretRedactor::Get()
    {
        static SecretRedactor instance;
        return instance;
    }

    void SecretRedactor::AddSecret(std::string const& secret)
    {
        if (secret.size() < MIN_SECRET_LENGTH)
        {
            return;
        }

        std::lock_guard lock(m_Mutex);

        // Avoid duplicates
        for (auto const& existing : m_Secrets)
        {
            if (existing == secret)
            {
                return;
            }
        }

        m_Secrets.push_back(secret);
    }

    void SecretRedactor::RemoveSecret(std::string const& secret)
    {
        std::lock_guard lock(m_Mutex);
        m_Secrets.erase(std::remove(m_Secrets.begin(), m_Secrets.end(), secret), m_Secrets.end());
    }

    std::string SecretRedactor::Redact(std::string const& message) const
    {
        std::lock_guard lock(m_Mutex);

        if (m_Secrets.empty())
        {
            return message;
        }

        std::string result = message;
        for (auto const& secret : m_Secrets)
        {
            size_t pos = 0;
            while ((pos = result.find(secret, pos)) != std::string::npos)
            {
                result.replace(pos, secret.size(), REDACTED);
                pos += REDACTED_LEN;
            }
        }
        return result;
    }

    bool SecretRedactor::HasSecrets() const
    {
        std::lock_guard lock(m_Mutex);
        return !m_Secrets.empty();
    }
} // namespace AIAssistant
