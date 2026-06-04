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

#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include "simdjson/simdjson.h"
#include "workflow/parserError.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    class WorkflowJsonParser
    {
    public:
        // Parse a JCWF workflow JSON document into a WorkflowDefinition.
        // Returns void on success; on failure, a typed `ParserError` carries
        // the machine-actionable code + human-readable detail.
        [[nodiscard]] std::expected<void, ParserError>
            ParseWorkflowJson(std::string const& jsonContent, WorkflowDefinition& workflowOut) const;

        // Parse a global.json file (container metadata only: version, id, label, doc,
        // triggers, manual_start, defaults). Does NOT parse tasks/dataflow/etc.
        [[nodiscard]] std::expected<void, ParserError>
            ParseGlobalJson(std::string const& jsonContent, WorkflowDefinition& workflowOut) const;

        // Parse a canvas JSON file (tasks, dataflow, filters, control_nodes, controlflow).
        // Metadata fields (version, id, label, etc.) are optional and ignored if present.
        [[nodiscard]] std::expected<void, ParserError>
            ParseCanvasJson(std::string const& jsonContent, WorkflowDefinition& workflowOut) const;

    private:
        // Root object parser (top-level workflow object).  The simdjson
        // ondemand object is taken by reference — copying it is a shallow
        // copy of pointers into the document arena and would dangle if the
        // arena moves.  Every Parse* helper in this file follows the same
        // by-reference convention.
        [[nodiscard]] std::expected<void, ParserError>
            ParseRootObject(simdjson::ondemand::object& rootObject, WorkflowDefinition& workflowOut) const;

        // Sub-parsers (implemented in workflowJsonParserDetails.cpp)
        [[nodiscard]] std::expected<void, ParserError>
            ParseTriggers(simdjson::ondemand::value& jsonValue, std::vector<WorkflowTrigger>& triggersOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseTrigger(simdjson::ondemand::object& jsonObject, WorkflowTrigger& triggerOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseTasks(simdjson::ondemand::value& jsonValue, std::unordered_map<std::string, TaskDef>& tasksOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseTask(simdjson::ondemand::object& jsonObject, TaskDef& taskOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseTaskInputs(simdjson::ondemand::value& jsonValue, TaskIOMap& inputsOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseTaskOutputs(simdjson::ondemand::value& jsonValue, TaskIOMap& outputsOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseTaskEnvironment(simdjson::ondemand::value& jsonValue, TaskEnvironment& environmentOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseTaskQueueBinding(simdjson::ondemand::value& jsonValue, QueueBinding& bindingOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseDataflow(simdjson::ondemand::value& jsonValue, std::vector<DataflowDef>& dataflowsOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseSingleDataflow(simdjson::ondemand::object& jsonObject, DataflowDef& dataflowOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseRetries(simdjson::ondemand::object& jsonObject, RetryPolicy& retryPolicyOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseDefaults(simdjson::ondemand::object& jsonObject, WorkflowDefaults& defaultsOut) const;

        // Filter parsers (v1.1)
        [[nodiscard]] std::expected<void, ParserError>
            ParseFilters(simdjson::ondemand::value& jsonValue, std::vector<FilterDef>& filtersOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseFilter(simdjson::ondemand::object& jsonObject, FilterDef& filterOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseFilterSource(simdjson::ondemand::object& jsonObject, FilterSource& sourceOut) const;

        // Control-flow graph extensions (branching)
        [[nodiscard]] std::expected<void, ParserError>
            ParseControlNodes(simdjson::ondemand::value& jsonValue,
                              std::vector<ControlNodeDef>& controlNodesOut) const;

        [[nodiscard]] std::expected<void, ParserError>
            ParseControlflow(simdjson::ondemand::value& jsonValue,
                             std::vector<ControlflowEdgeDef>& controlflowOut) const;

        // Utility helpers — kept on the legacy `bool + std::string&` shape.
        // They sit one layer below the typed-error Parse* methods and don't
        // carry useful category information; parser methods bridge their
        // failures into typed `SimdjsonError` at the call site.
        bool ExtractRawJson(simdjson::ondemand::value& element, std::string& rawJsonOut) const;

        bool ElementToString(simdjson::ondemand::value& element, std::string& output) const;

        // string → enum maps
        TaskType StringToTaskType(std::string const& typeString) const;
        TaskMode StringToTaskMode(std::string const& modeString) const;
        WorkflowTriggerType StringToTriggerType(std::string const& typeString) const;
    };

} // namespace AIAssistant
