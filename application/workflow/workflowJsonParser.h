/* Copyright (c) 2026 JC Technolabs

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

#include <string>
#include <unordered_map>
#include <vector>

#include "simdjson/simdjson.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    class WorkflowJsonParser
    {
    public:
        // Parse a JCWF workflow JSON document into a WorkflowDefinition.
        // Returns true on success; on failure, errorMessage is populated.
        bool ParseWorkflowJson(std::string const& jsonContent, WorkflowDefinition& workflowOut,
                               std::string& errorMessage) const;

    private:
        // Root object parser (top-level workflow object).
        bool ParseRootObject(simdjson::ondemand::object rootObject, WorkflowDefinition& workflowOut,
                             std::string& errorMessage) const;

        // Sub-parsers (implemented in workflowJsonParserDetails.cpp)
        bool ParseTriggers(simdjson::ondemand::value& jsonValue, std::vector<WorkflowTrigger>& triggersOut,
                           std::string& errorMessage) const;

        bool ParseTrigger(simdjson::ondemand::object& jsonObject, WorkflowTrigger& triggerOut,
                          std::string& errorMessage) const;

        bool ParseTasks(simdjson::ondemand::value& jsonValue, std::unordered_map<std::string, TaskDef>& tasksOut,
                        std::string& errorMessage) const;

        bool ParseTask(simdjson::ondemand::object& jsonObject, TaskDef& taskOut, std::string& errorMessage) const;

        bool ParseTaskInputs(simdjson::ondemand::value& jsonValue, TaskIOMap& inputsOut, std::string& errorMessage) const;

        bool ParseTaskOutputs(simdjson::ondemand::value& jsonValue, TaskIOMap& outputsOut, std::string& errorMessage) const;

        bool ParseTaskEnvironment(simdjson::ondemand::value& jsonValue, TaskEnvironment& environmentOut,
                                  std::string& errorMessage) const;

        bool ParseTaskQueueBinding(simdjson::ondemand::value& jsonValue, QueueBinding& bindingOut,
                                   std::string& errorMessage) const;

        bool ParseDataflow(simdjson::ondemand::value& jsonValue, std::vector<DataflowDef>& dataflowsOut,
                           std::string& errorMessage) const;

        bool ParseSingleDataflow(simdjson::ondemand::object& jsonObject, DataflowDef& dataflowOut,
                                 std::string& errorMessage) const;

        bool ParseRetries(simdjson::ondemand::object& jsonObject, RetryPolicy& retryPolicyOut,
                          std::string& errorMessage) const;

        bool ParseDefaults(simdjson::ondemand::object& jsonObject, WorkflowDefaults& defaultsOut,
                           std::string& errorMessage) const;

        // Filter parsers (v1.1)
        bool ParseFilters(simdjson::ondemand::value& jsonValue, std::vector<FilterDef>& filtersOut,
                          std::string& errorMessage) const;

        bool ParseFilter(simdjson::ondemand::object& jsonObject, FilterDef& filterOut, std::string& errorMessage) const;

        bool ParseFilterSource(simdjson::ondemand::object& jsonObject, FilterSource& sourceOut,
                               std::string& errorMessage) const;

        // Control-flow graph extensions (branching)
        bool ParseControlNodes(simdjson::ondemand::value& jsonValue, std::vector<ControlNodeDef>& controlNodesOut,
                               std::string& errorMessage) const;

        bool ParseControlflow(simdjson::ondemand::value& jsonValue, std::vector<ControlflowEdgeDef>& controlflowOut,
                              std::string& errorMessage) const;

        // Utility helpers
        bool ExtractRawJson(simdjson::ondemand::value& element, std::string& rawJsonOut) const;

        bool ElementToString(simdjson::ondemand::value& element, std::string& output) const;

        // string → enum maps
        TaskType StringToTaskType(std::string const& typeString) const;
        TaskMode StringToTaskMode(std::string const& modeString) const;
        WorkflowTriggerType StringToTriggerType(std::string const& typeString) const;
    };

} // namespace AIAssistant
