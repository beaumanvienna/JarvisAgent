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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
   OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#pragma once

#include <string>
#include "taskExecutor.h"

namespace AIAssistant
{
    class ShellTaskExecutor : public ITaskExecutor
    {
    public:
        virtual ~ShellTaskExecutor() = default;

        bool Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                     TaskInstanceState& taskState) override;

    private:
        // Restrict which scripts can be invoked (e.g., must live under "scripts/").
        bool ValidateScriptPath(std::string const& path) const;

        // Conservative safety check: allow typical path / flag characters and spaces,
        // but reject characters commonly used for shell injection.
        bool IsSafeArgument(std::string const& argument) const;
    };
} // namespace AIAssistant
