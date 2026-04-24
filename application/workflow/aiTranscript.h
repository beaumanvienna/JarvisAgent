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

#include <filesystem>
#include <string>

#include "workflow/aiInvocation.h"
#include "workflow/aiReply.h"

namespace AIAssistant
{
    // Writes <prob>.transcript.json next to <prob>.output.txt.
    //
    // Format: JSON array with request + response turn entries.
    //   [
    //     {"kind":"request","interface":"...","model":"...","messages":[{"role":"user","content":"..."}],
    //      "settings":{"temperature":0.0,"seed":42},"timestamp":"..."},
    //     {"kind":"response","text":"...","usage":{"input":55,"output":1,"total":56},
    //      "finish_reason":"stop","system_fingerprint":"fp_abc","timestamp":"..."}
    //   ]
    //
    // When chunking (Phase 6) produces multiple dispatches per PROB the convention
    // will be <prob>_chunk<N>.transcript.json plus an aggregate <prob>.transcript.json.
    class AiTranscript
    {
    public:
        static bool AppendRequest(std::filesystem::path const& transcriptPath, AiInvocation const& envelope,
                                   std::string const& resolvedModel);

        static bool AppendResponse(std::filesystem::path const& transcriptPath, AiReply const& reply);
    };
} // namespace AIAssistant
