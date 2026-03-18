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

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "workflowTypes.h"

namespace AIAssistant
{
    class TaskPathResolver
    {
    public:
        // Resolve the workflow base directory from a WorkflowDefinition.
        // Fallback chain: m_WorkflowBaseDirectoryAbsolute → m_WorkflowBaseDirectory
        //   → m_WorkflowFileDirectoryAbsolute → m_WorkflowFileDirectory
        //   → m_WorkflowFilePathAbsolute.parent_path() → m_WorkflowFilePath.parent_path()
        // Relative results are resolved against launchCWD.  Returns empty path only if all fields are empty.
        static std::filesystem::path ResolveWorkflowBaseDirectory(WorkflowDefinition const& workflowDefinition);

        // Resolve absolute working directory for a task.
        static std::filesystem::path ResolveTaskWorkingDirectoryPath(std::filesystem::path const& workflowBaseDirectoryPath,
                                                                     std::string const& taskWorkingDirectoryText);

        // Resolve a path text relative to the task working directory (if relative).
        static std::filesystem::path ResolveTaskScopedPath(std::filesystem::path const& taskWorkingDirectoryPath,
                                                           std::string const& pathText);

        // Resolve a filesystem path relative to a base directory (if relative).
        static std::filesystem::path ResolvePath(std::filesystem::path const& baseDirectoryPath,
                                                 std::filesystem::path const& path);

        // Build a map of output slot name -> value (from task state), constrained to declared output fields.
        static void BuildOutputSlotMap(TaskDef const& taskDefinition, TaskInstanceState const& taskState,
                                       std::unordered_map<std::string, std::string>& outputSlotMapOut);

        // Compatibility helper used by orchestrator/runtime manager for freshness:
        // Resolves task file input/output paths to absolute paths (including template expansion via DataflowResolver).
        static bool ResolveFreshnessPathsForTask(WorkflowDefinition const& workflowDefinition,
                                                 WorkflowRun const& workflowRun, TaskDef const& taskDefinition,
                                                 std::string const& taskId,
                                                 std::vector<std::filesystem::path>& inputPathsOut,
                                                 std::vector<std::filesystem::path>& outputPathsOut);
    };
} // namespace AIAssistant
