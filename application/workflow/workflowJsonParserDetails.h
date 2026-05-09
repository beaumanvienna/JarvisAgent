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
#include <cstdint>
#include <filesystem>
#include <string>

#include "simdjson/simdjson.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    // Hard caps applied during workflow JSON parsing.  These bound the heap
    // pressure a malicious or malformed JCWF can put on the parser thread,
    // and bound the size of secondary structures that downstream subsystems
    // assume are reasonable (run-state maps, log lines, dashboard payloads).
    // All deliberately generous — real workflows are far below.
    namespace WorkflowParserLimits
    {
        constexpr std::size_t kMaxTasks = 1'000;
        constexpr std::size_t kMaxTriggers = 100;
        constexpr std::size_t kMaxDataflows = 10'000;
        constexpr std::size_t kMaxFilters = 100;
        constexpr std::size_t kMaxControlNodes = 1'000;
        constexpr std::size_t kMaxControlflowEdges = 10'000;
        constexpr std::size_t kMaxFileInputsPerTask = 1'000;
        constexpr std::size_t kMaxFileOutputsPerTask = 1'000;
        constexpr std::size_t kMaxQueueFilesPerSection = 1'000;
        constexpr std::size_t kMaxInlineContentBytes = 1ULL * 1024ULL * 1024ULL; // 1 MB
        constexpr std::size_t kMaxPathLength = 1'024;
        constexpr std::size_t kMaxDependsOnPerTask = 1'000;
        constexpr std::size_t kMaxFieldsPerCollection = 10'000;
        constexpr std::uint32_t kMaxRetryAttempts = 100;
        constexpr std::uint32_t kMaxBackoffMs = 3'600'000;             // 1 hour
        constexpr std::uint64_t kMaxTimeoutMs = 7ULL * 24 * 3600 * 1000; // 7 days
    } // namespace WorkflowParserLimits

    // Syntactic path-acceptance check applied at parse time.  Rejects empty,
    // overlength, and absolute (POSIX or Windows) paths.  `..` segments are
    // **allowed** here because shipped JCWFs use the convention
    // `working_directory: "../../queue/<workflow>/<task>"` to navigate from
    // `workflows/<id>/` up to the project-root-anchored queue tree.  The
    // deeper canonical-containment check is the consumer-side gate
    // (`ConfineUnderProjectRoot` — see use-site list in
    // `application/file/README.md`), which is the authoritative defense
    // against `..`-traversal escapes.  This parse-time gate is the size-
    // and-shape filter; the consumer-side gate is the canonical-form
    // filter; both fail closed.
    [[nodiscard]] inline bool IsAcceptedRelativePath(std::string const& path) noexcept
    {
        using namespace WorkflowParserLimits;
        if (path.empty() || path.size() > kMaxPathLength)
        {
            return false;
        }
        std::filesystem::path const p(path);
        if (p.is_absolute())
        {
            return false;
        }
        return true;
    }

    bool ParseTaskQueueBinding(simdjson::ondemand::value& value, QueueBinding& binding, std::string& errorMessage);
}
