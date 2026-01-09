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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
   OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.*/

#pragma once

#include <optional>
#include <string>

#include "workflow/taskExecutor.h"

namespace AIAssistant
{
    class AiCallTaskExecutor final : public ITaskExecutor
    {
    public:
        AiCallTaskExecutor() = default;
        virtual ~AiCallTaskExecutor() = default;

        bool Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                     TaskInstanceState& taskState) override;

        static bool WriteTextFile(std::string const& filePath, std::string const& fileContent, std::string& outErrorMessage);

    private:
        static std::string BuildProbFilename(int64_t const requestId, int64_t const timestampNs);

        static std::optional<std::string> TryExtractStringParam(std::string const& rawParamsJson,
                                                                std::string const& fieldName, std::string& outErrorMessage);

        static std::string ApplySimpleTemplate(std::string const& templateText, TaskInstanceState const& taskState);
        static std::string TryBuildPromptFromParams(TaskDef const& taskDefinition, TaskInstanceState const& taskState);

        static bool WriteInlineQueueBindingFiles(QueueBinding const& queueBinding, std::string& outErrorMessage);
    };
} // namespace AIAssistant