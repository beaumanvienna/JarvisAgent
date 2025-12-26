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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "workflowTypes.h"

namespace AIAssistant
{
    // -------------------------------------------------------------
    // Holds resolved input key/value pairs for a given task instance
    // -------------------------------------------------------------
    struct TaskResolvedInputs
    {
        std::unordered_map<std::string, std::string> m_StringValues;
    };

    // -------------------------------------------------------------
    // DataflowResolver
    // -------------------------------------------------------------
    // Responsible for resolving all input values for a task:
    //   * explicit dataflow rules ("from_task", "from_output")
    //   * workflow run context (future extension)
    //   * literal/default values (future extension)
    //   * template expansion (${inputs.x})
    //
    // Does not execute tasks; only collects input values.
    // -------------------------------------------------------------
    class DataflowResolver
    {
    public:
        DataflowResolver() = default;
        ~DataflowResolver() = default;

        // Resolve inputs for a task instance.
        // Returns std::nullopt on missing wiring or unresolved input.
        std::optional<TaskResolvedInputs> ResolveInputsForTask(WorkflowDefinition const& workflowDefinition,
                                                               WorkflowRun const& workflowRun, TaskDef const& taskDefinition,
                                                               std::string const& taskId) const;

        // Look up whether any dataflow edge maps to this input field.
        bool TryResolveFromDataflowEdges(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                         std::string const& targetTaskId, std::string const& targetInputName,
                                         std::string& resolvedValueOut) const;

        // Template expansion: resolve ${inputs.x} inside a string.
        bool ExpandTemplates(std::string const& rawValue, std::unordered_map<std::string, std::string> const& inputValues,
                             std::string& expandedOut) const;
    };

} // namespace AIAssistant
