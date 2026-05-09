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

#include "engine.h"
#include "log/secretRedactor.h"

namespace AIAssistant
{
    static constexpr char REDACTED[] = "[REDACTED]";
    static constexpr size_t REDACTED_LEN = sizeof(REDACTED) - 1;

    // Minimum length for a value to register as a secret.  Below 8 chars,
    // common dev-mock passwords ("test", "demo", "1234") collide with normal
    // log substrings and the redactor over-redacts every line containing them.
    static constexpr size_t MIN_SECRET_LENGTH = 8;

    SecretRedactor& SecretRedactor::Get()
    {
        static SecretRedactor instance;
        return instance;
    }

    void SecretRedactor::AddSecret(std::string_view secret)
    {
        if (secret.size() < MIN_SECRET_LENGTH)
        {
            // Length is logged but value is not — devs need to spot the silent
            // skip, but we can't reveal the secret itself.
            LOG_CORE_WARN("SecretRedactor::AddSecret: secret too short (length {} < {}), not registered — log "
                          "redaction will not protect this value",
                          secret.size(), MIN_SECRET_LENGTH);
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

        m_Secrets.emplace_back(secret);

        // Sort longest-first so a longer secret matches before any shorter
        // secret that happens to be a prefix.  Without this, registering a
        // short secret first and a longer secret second leaves the longer
        // secret's tail bytes leaking when it appears in a log line —
        // Redact()'s first pass redacts the short prefix, the second pass
        // can't match because the prefix has already been replaced.
        // The vector is small (< 100 entries in practice) and AddSecret is
        // rare (per-credential, at registration time), so an O(N log N)
        // sort here is cheap and keeps Redact()'s loop straightforward.
        std::sort(m_Secrets.begin(), m_Secrets.end(),
                  [](std::string const& a, std::string const& b) { return a.size() > b.size(); });

        m_HasSecretsHint.store(true, std::memory_order_release);
    }

    void SecretRedactor::RemoveSecret(std::string_view secret)
    {
        std::lock_guard lock(m_Mutex);
        m_Secrets.erase(std::remove_if(m_Secrets.begin(), m_Secrets.end(),
                                       [&](std::string const& s) { return s == secret; }),
                        m_Secrets.end());
        m_HasSecretsHint.store(!m_Secrets.empty(), std::memory_order_release);
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
} // namespace AIAssistant
