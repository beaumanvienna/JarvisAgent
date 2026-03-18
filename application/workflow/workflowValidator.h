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
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <string>
#include <vector>

#include "workflowTypes.h"

namespace AIAssistant
{
    class ScriptRegistry;
    class WorkflowFileIndex;

    struct GeneratedScript
    {
        std::string path;
        std::string content;
        bool executable{false};
    };
} // namespace AIAssistant

namespace AIAssistant
{
    enum class WorkflowValidationSeverity
    {
        Error = 0,
        Warning = 1,
        Info = 2
    };

    enum class WorkflowValidationTier
    {
        A = 0,
        B = 1,
        C = 2,
        D = 3
    };

    struct WorkflowValidationIssue
    {
        WorkflowValidationSeverity m_Severity{WorkflowValidationSeverity::Error};
        WorkflowValidationTier m_Tier{WorkflowValidationTier::B};
        std::string m_Code;
        std::string m_Message;

        // JSON-ish path into the submitted workflow (best-effort, for UI highlighting).
        // Examples:
        // - "$.id"
        // - "$.tasks.myTask"
        // - "$.tasks.myTask.depends_on[0]"
        std::string m_Path;

        // Optional task id if issue is task-related.
        std::string m_TaskId;

        // AI-friendly fix suggestion (Stage 5 uses this to correct the JCWF).
        std::string m_SuggestedFix;

        // Extra context (e.g. available scripts, expected paths).
        std::string m_Context;
    };

    class WorkflowValidator
    {
    public:
        static void Validate(WorkflowDefinition const& workflow, std::vector<WorkflowValidationIssue>& issues);

        static void Validate(WorkflowDefinition const& workflow, ScriptRegistry const* scriptRegistry,
                             std::vector<GeneratedScript> const* pendingScripts,
                             std::vector<WorkflowValidationIssue>& issues,
                             WorkflowFileIndex const* workflowFileIndex = nullptr);

        static bool HasErrors(std::vector<WorkflowValidationIssue> const& issues);
    };

} // namespace AIAssistant
