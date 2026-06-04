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

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace AIAssistant
{
    // Scrubs registered secret values from log output.
    // Thread-safe: secrets can be added/removed from any thread while
    // Redact() is called from the logging pipeline.
    class SecretRedactor
    {
    public:
        static SecretRedactor& Get();

        // Register a secret value to be redacted from all log output.
        // Secrets shorter than 8 characters are ignored (too likely to cause false positives);
        // a WARN is logged in that case so developers can spot the silent skip.
        // Accepts `string_view` so callers holding a `SecureString` can pass `Get()` directly.
        void AddSecret(std::string_view secret);

        // Remove a previously registered secret (e.g., after token rotation).
        void RemoveSecret(std::string_view secret);

        // Scrub all registered secrets from a message, replacing with [REDACTED].
        std::string Redact(std::string const& message) const;

        // Returns true if any secrets are registered.  Lock-free hot path so
        // the logger's per-message check doesn't contend on the mutex when no
        // secrets are registered (the common case at server startup).
        bool HasSecrets() const { return m_HasSecretsHint.load(std::memory_order_acquire); }

    private:
        SecretRedactor() = default;

        mutable std::mutex m_Mutex;
        std::vector<std::string> m_Secrets;

        // Mirror of `!m_Secrets.empty()` updated under m_Mutex but readable
        // without the lock.  Used by HasSecrets() to skip mutex acquisition on
        // every formatted log line.
        std::atomic<bool> m_HasSecretsHint{false};
    };
} // namespace AIAssistant
