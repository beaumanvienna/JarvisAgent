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

#pragma once

#include <cstddef>
#include <string>

namespace AIAssistant
{
    // Cryptographically secure hex token of `numBytes` random bytes (output is 2*numBytes hex chars).
    // Backed by OpenSSL RAND_bytes; returns an empty string on RAND_bytes failure (logged at ERROR).
    // Callers MUST treat empty return as fail-closed.
    std::string RandomHex(std::size_t numBytes);

    // Strict allowlist for assistant-side opaque identifiers (session IDs, approval requestIds,
    // memory IDs).  Returns true iff the input matches `[A-Za-z0-9_-]{1,128}`.  Empty strings,
    // path separators, NULs, and any other byte are rejected.  Used at every site where an
    // attacker-influenced ID feeds into a filesystem path or audit-log substring.
    bool IsValidOpaqueId(std::string const& id);
} // namespace AIAssistant
