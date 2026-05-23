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

namespace AIAssistant
{
    // SecureString-only HTTP-path audit.  Plants a unique 64-byte hex-printable
    // nonce as the secret credential for each auth style, drives the auth-build
    // path end-to-end, tears down all locals, defragments the allocator, then
    // scans /proc/self/mem for the nonce.  Verifies the invariant: between
    // SecureString and curl_slist_append, no plain std::string heap allocation
    // contains the raw secret.
    //
    // Two scan phases per scenario:
    //   - Smoke: only the [heap] mapping (glibc main arena, sbrk-backed).
    //   - Deep:  every private rw-p mapping (heap + anon mmap + named arenas).
    //            Catches large-allocation slabs that bypass sbrk.
    //
    // Reports per-scenario hit counts and a final aggregate.  Scenarios marked
    // "expected residual" track known architectural floors (libcurl strdup
    // inside curl_slist_append) — the test passes when their hit counts match
    // expectation, failing when the count regresses in either direction so
    // future hardening trips the test.
    //
    // Compiled to a no-op stub returning 0 unless J9T_HEAPSCAN_BUILD is defined.
    // When the audit code is live, this function exits the process on completion
    // (the binary is not meant to come up as a server in audit builds).
    //
    // See doc/cyber security.md "Empirical verification — heap-scan audit".
    int RunHeapScanAudit();
} // namespace AIAssistant
