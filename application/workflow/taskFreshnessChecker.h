/* Copyright (c) 2025 JC Technolabs
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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "workflowTypes.h"

namespace AIAssistant
{
    // ---------------------------------------------------------------------
    // TaskFreshnessChecker
    // ---------------------------------------------------------------------
    // Implements Makefile-style up-to-date checks:
    // - all outputs must exist
    // - oldest output timestamp must be >= newest input timestamp
    // - "inputs" includes this task's declared inputs AND upstream outputs
    // ---------------------------------------------------------------------
    class TaskFreshnessChecker
    {
    public:
        struct ResolvedPaths
        {
            std::vector<std::filesystem::path> m_InputPaths;
            std::vector<std::filesystem::path> m_OutputPaths;
        };

        using ResolveOutputPathsFn =
            std::function<bool(std::string const& taskId, std::vector<std::filesystem::path>& outPaths)>;

        bool IsTaskUpToDate(WorkflowDefinition const& workflowDefinition, std::string const& taskId,
                            ResolvedPaths const& resolvedPaths, ResolveOutputPathsFn const& resolveOutputPaths) const;

    private:
        bool CollectUpstreamOutputTimes(WorkflowDefinition const& workflowDefinition, std::string const& taskId,
                                        std::unordered_set<std::string>& visitedTasks,
                                        std::vector<std::filesystem::file_time_type>& outTimes,
                                        ResolveOutputPathsFn const& resolveOutputPaths) const;
    };

} // namespace AIAssistant
