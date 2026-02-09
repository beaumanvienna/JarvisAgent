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

#include "dataflowResolver.h"

#include "engine.h"

namespace AIAssistant
{

    std::optional<TaskResolvedInputs> DataflowResolver::ResolveInputsForTask(WorkflowDefinition const& workflowDefinition,
                                                                             WorkflowRun const& workflowRun,
                                                                             TaskDef const& taskDefinition,
                                                                             std::string const& taskId) const
    {
        TaskResolvedInputs resolvedInputs;

        // Step 1: resolve each declared input.
        for (auto const& inputPair : taskDefinition.m_Inputs)
        {
            std::string const& inputName = inputPair.first;
            TaskIOField const& inputField = inputPair.second;

            std::string resolvedValue;

            // 1) Try explicit dataflow edges.
            if (TryResolveFromDataflowEdges(workflowDefinition, workflowRun, taskId, inputName, resolvedValue))
            {
                resolvedInputs.m_StringValues[inputName] = resolvedValue;
                continue;
            }

            // 2) TODO: context-based resolution from workflowRun.m_Context.

            // 3) TODO: defaults / literals (JCWF-level) if introduced.

            // If not resolved by any mechanism:
            //  - required input -> fail
            //  - optional input -> simply omit it
            if (inputField.m_IsRequired)
            {
                LOG_APP_ERROR("DataflowResolver: Missing required input '{}' for task '{}'", inputName, taskId);
                resolvedInputs.m_ErrorMessage =
                    "required input '" + inputName + "' for task '" + taskId + "' could not be resolved";
                return resolvedInputs;
            }
        }

        // Step 2: expand templates inside resolved values (e.g. ${inputs.section_title})
        for (auto& inputValuePair : resolvedInputs.m_StringValues)
        {
            std::string expandedValue;
            if (!ExpandTemplates(inputValuePair.second, resolvedInputs.m_StringValues, expandedValue))
            {
                LOG_APP_ERROR("DataflowResolver: Template expansion failed for task '{}' value '{}'", taskId,
                              inputValuePair.second);
                return std::nullopt;
            }

            inputValuePair.second = expandedValue;
        }

        return resolvedInputs;
    }

    bool DataflowResolver::TryResolveFromDataflowEdges(WorkflowDefinition const& workflowDefinition,
                                                       WorkflowRun const& workflowRun, std::string const& targetTaskId,
                                                       std::string const& targetInputName,
                                                       std::string& resolvedValueOut) const
    {
        // Each DataflowEdge is an alias for DataflowDef.
        for (auto const& dataflowEdge : workflowDefinition.m_Dataflows)
        {
            if (dataflowEdge.m_ToTask != targetTaskId || dataflowEdge.m_ToInput != targetInputName)
            {
                continue;
            }

            // Found an edge that targets our input.
            auto stateIterator = workflowRun.m_TaskStates.find(dataflowEdge.m_FromTask);
            if (stateIterator == workflowRun.m_TaskStates.end())
            {
                LOG_APP_ERROR("DataflowResolver: dataflow references unknown task '{}' → '{}.{}'", dataflowEdge.m_FromTask,
                              dataflowEdge.m_ToTask, dataflowEdge.m_ToInput);
                return false;
            }

            TaskInstanceState const& sourceTaskState = stateIterator->second;

            auto outputIterator = sourceTaskState.m_OutputValues.find(dataflowEdge.m_FromOutput);
            if (outputIterator == sourceTaskState.m_OutputValues.end())
            {
                LOG_APP_ERROR("DataflowResolver: output '{}' not found in task '{}' for dataflow into '{}.{}'",
                              dataflowEdge.m_FromOutput, dataflowEdge.m_FromTask, dataflowEdge.m_ToTask,
                              dataflowEdge.m_ToInput);
                return false;
            }

            resolvedValueOut = outputIterator->second;
            return true;
        }

        // No matching dataflow edge for this input.
        return false;
    }

    bool DataflowResolver::ExpandTemplates(std::string const& rawValue,
                                           std::unordered_map<std::string, std::string> const& inputValues,
                                           std::string& expandedOut) const
    {
        expandedOut.clear();

        std::string::size_type currentIndex = 0;
        while (currentIndex < rawValue.size())
        {
            std::string::size_type startIndex = rawValue.find("${inputs.", currentIndex);
            if (startIndex == std::string::npos)
            {
                // No more templates, append remaining literal text.
                expandedOut += rawValue.substr(currentIndex);
                break;
            }

            // Append literal text before the template.
            expandedOut += rawValue.substr(currentIndex, startIndex - currentIndex);

            std::string::size_type closeBraceIndex = rawValue.find('}', startIndex + 1);
            if (closeBraceIndex == std::string::npos)
            {
                // Malformed template.
                return false;
            }

            // "${inputs." is 9 characters.
            std::string key = rawValue.substr(startIndex + 9, closeBraceIndex - (startIndex + 9));

            auto valueIterator = inputValues.find(key);
            if (valueIterator == inputValues.end())
            {
                // Reference to unknown input key.
                return false;
            }

            expandedOut += valueIterator->second;
            currentIndex = closeBraceIndex + 1;
        }

        return true;
    }

} // namespace AIAssistant
