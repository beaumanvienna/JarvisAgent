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
    // ------------------------------------------------------------------------
    // TaskPathResolver — pure path-resolution helpers.
    //
    // **Trust model:** these helpers are pass-through resolvers — they do NOT
    // enforce containment.  An attacker-supplied path with `..` segments or
    // an absolute path outside the project tree will resolve verbatim.
    //
    // Containment is the **caller's** responsibility — pass any external
    // path string through `code/backend/application/file/pathConfinement.h::
    // ConfineUnderProjectRoot` BEFORE handing the resolved value to a
    // filesystem-touching API (read / write / remove).  The caller knows
    // the intended scope (project root / scripts dir / task working dir /
    // etc.) and applies the corresponding containment gate.
    //
    // The reason this responsibility lives at the caller, not here: the
    // intended scope varies per call site (some legitimately resolve cross-
    // task input paths inside the workflow base, others stay strictly inside
    // a task working dir).  A one-size containment policy in this resolver
    // would either reject legitimate paths or fail open on hostile ones.
    // ------------------------------------------------------------------------
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
