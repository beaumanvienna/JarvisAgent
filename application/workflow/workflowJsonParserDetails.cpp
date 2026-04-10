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

#include "workflow/workflowJsonParser.h"
#include "workflow/workflowJsonParserDetails.h"

#include <optional>
#include <string_view>

#include "engine.h"

namespace AIAssistant
{

    namespace
    {
        bool RequireObject(simdjson::ondemand::value& value, std::string const& context, std::string& errorMessage)
        {
            auto typeResult = value.type();
            if (typeResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to get type for ";
                errorMessage += context;
                errorMessage += ": ";
                errorMessage += simdjson::error_message(typeResult.error());
                return false;
            }

            if (typeResult.value() != simdjson::ondemand::json_type::object)
            {
                errorMessage = context;
                errorMessage += " must be an object";
                return false;
            }

            return true;
        }

        bool RequireArray(simdjson::ondemand::value& value, std::string const& context, std::string& errorMessage)
        {
            auto typeResult = value.type();
            if (typeResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to get type for ";
                errorMessage += context;
                errorMessage += ": ";
                errorMessage += simdjson::error_message(typeResult.error());
                return false;
            }

            if (typeResult.value() != simdjson::ondemand::json_type::array)
            {
                errorMessage = context;
                errorMessage += " must be an array";
                return false;
            }

            return true;
        }
    } // anonymous namespace

    // ---------------------------------------------------------------------
    // Utility helpers
    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // Triggers
    // ---------------------------------------------------------------------

    bool WorkflowJsonParser::ParseTriggers(simdjson::ondemand::value& jsonValue, std::vector<WorkflowTrigger>& triggersOut,
                                           std::string& errorMessage) const
    {
        if (!RequireArray(jsonValue, "triggers", errorMessage))
        {
            return false;
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'triggers' array: ";
            errorMessage += simdjson::error_message(arrayResult.error());
            return false;
        }

        simdjson::ondemand::array triggerArray = arrayResult.value();

        for (simdjson::ondemand::value triggerValue : triggerArray)
        {
            WorkflowTrigger trigger;

            auto objectResult = triggerValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "trigger entry must be an object: ";
                errorMessage += simdjson::error_message(objectResult.error());
                return false;
            }

            simdjson::ondemand::object triggerObject = objectResult.value();

            if (!ParseTrigger(triggerObject, trigger, errorMessage))
            {
                return false;
            }

            triggersOut.push_back(trigger);
        }

        return true;
    }

    // ---------------------------------------------------------------------
    // Control-flow graph extensions (branching)
    // ---------------------------------------------------------------------

    namespace
    {
        ControlNodeType StringToControlNodeType(std::string const& typeString)
        {
            if (typeString == "branch")
            {
                return ControlNodeType::Branch;
            }
            return ControlNodeType::Unknown;
        }

        ControlflowKind StringToControlflowKind(std::string const& kindString)
        {
            if (kindString == "normal")
            {
                return ControlflowKind::Normal;
            }
            if (kindString == "error_signal")
            {
                return ControlflowKind::ErrorSignal;
            }
            if (kindString == "on_error")
            {
                return ControlflowKind::OnError;
            }
            return ControlflowKind::Unknown;
        }
    } // namespace

    bool WorkflowJsonParser::ParseControlNodes(simdjson::ondemand::value& jsonValue,
                                               std::vector<ControlNodeDef>& controlNodesOut,
                                               std::string& errorMessage) const
    {
        controlNodesOut.clear();

        if (!RequireArray(jsonValue, "control_nodes", errorMessage))
        {
            return false;
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'control_nodes' array: ";
            errorMessage += simdjson::error_message(arrayResult.error());
            return false;
        }

        simdjson::ondemand::array nodeArray = arrayResult.value();
        for (simdjson::ondemand::value entryValue : nodeArray)
        {
            auto objectResult = entryValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "control_nodes entry must be an object: ";
                errorMessage += simdjson::error_message(objectResult.error());
                return false;
            }

            simdjson::ondemand::object obj = objectResult.value();

            ControlNodeDef node;
            bool hasId = false;
            bool hasType = false;

            for (auto field : obj)
            {
                auto keyResult = field.unescaped_key();
                if (keyResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "failed to read control_nodes field key: ";
                    errorMessage += simdjson::error_message(keyResult.error());
                    return false;
                }

                std::string_view keyView = keyResult.value();
                std::string key(keyView.begin(), keyView.end());
                simdjson::ondemand::value value = field.value();

                if (key == "id")
                {
                    if (!ElementToString(value, node.m_Id))
                    {
                        errorMessage = "control_nodes field 'id' must be string";
                        return false;
                    }
                    hasId = true;
                }
                else if (key == "type")
                {
                    std::string typeString;
                    if (!ElementToString(value, typeString))
                    {
                        errorMessage = "control_nodes field 'type' must be string";
                        return false;
                    }
                    node.m_Type = StringToControlNodeType(typeString);
                    hasType = true;
                }
                else if (key == "label")
                {
                    ElementToString(value, node.m_Label);
                }
                else
                {
                    LOG_CORE_WARN("Unknown field in control_nodes: {}", key);
                }
            }

            if (!hasId)
            {
                errorMessage = "control_nodes entry missing required field: id";
                return false;
            }

            if (!hasType)
            {
                errorMessage = "control_nodes entry missing required field: type";
                return false;
            }

            controlNodesOut.push_back(std::move(node));
        }

        return true;
    }

    bool WorkflowJsonParser::ParseControlflow(simdjson::ondemand::value& jsonValue,
                                              std::vector<ControlflowEdgeDef>& controlflowOut,
                                              std::string& errorMessage) const
    {
        controlflowOut.clear();

        if (!RequireArray(jsonValue, "controlflow", errorMessage))
        {
            return false;
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'controlflow' array: ";
            errorMessage += simdjson::error_message(arrayResult.error());
            return false;
        }

        simdjson::ondemand::array edgeArray = arrayResult.value();
        for (simdjson::ondemand::value entryValue : edgeArray)
        {
            auto objectResult = entryValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "controlflow entry must be an object: ";
                errorMessage += simdjson::error_message(objectResult.error());
                return false;
            }

            simdjson::ondemand::object obj = objectResult.value();

            ControlflowEdgeDef edge;
            bool hasFrom = false;
            bool hasTo = false;
            bool hasKind = false;

            for (auto field : obj)
            {
                auto keyResult = field.unescaped_key();
                if (keyResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "failed to read controlflow field key: ";
                    errorMessage += simdjson::error_message(keyResult.error());
                    return false;
                }

                std::string_view keyView = keyResult.value();
                std::string key(keyView.begin(), keyView.end());
                simdjson::ondemand::value value = field.value();

                if (key == "from")
                {
                    if (!ElementToString(value, edge.m_From))
                    {
                        errorMessage = "controlflow field 'from' must be string";
                        return false;
                    }
                    hasFrom = true;
                }
                else if (key == "to")
                {
                    if (!ElementToString(value, edge.m_To))
                    {
                        errorMessage = "controlflow field 'to' must be string";
                        return false;
                    }
                    hasTo = true;
                }
                else if (key == "kind")
                {
                    std::string kindString;
                    if (!ElementToString(value, kindString))
                    {
                        errorMessage = "controlflow field 'kind' must be string";
                        return false;
                    }
                    edge.m_Kind = StringToControlflowKind(kindString);
                    hasKind = true;
                }
                else if (key == "from_port")
                {
                    ElementToString(value, edge.m_FromPort);
                }
                else if (key == "to_port")
                {
                    ElementToString(value, edge.m_ToPort);
                }
                else
                {
                    LOG_CORE_WARN("Unknown field in controlflow: {}", key);
                }
            }

            if (!hasFrom || !hasTo || !hasKind)
            {
                errorMessage = "controlflow entry missing required fields (from, to, kind)";
                return false;
            }

            controlflowOut.push_back(std::move(edge));
        }

        return true;
    }

    // ---------------------------------------------------------------------
    // Tasks
    // ---------------------------------------------------------------------

    bool WorkflowJsonParser::ParseTasks(simdjson::ondemand::value& jsonValue,
                                        std::unordered_map<std::string, TaskDef>& tasksOut, std::string& errorMessage) const
    {
        if (!RequireObject(jsonValue, "tasks", errorMessage))
        {
            return false;
        }

        auto objectResult = jsonValue.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'tasks' object: ";
            errorMessage += simdjson::error_message(objectResult.error());
            return false;
        }

        simdjson::ondemand::object tasksObject = objectResult.value();

        for (auto field : tasksObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read task key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string taskKey(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            auto taskObjectResult = value.get_object();
            if (taskObjectResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "task entry must be an object: ";
                errorMessage += simdjson::error_message(taskObjectResult.error());
                return false;
            }

            simdjson::ondemand::object taskObject = taskObjectResult.value();

            TaskDef task;
            if (!ParseTask(taskObject, task, errorMessage))
            {
                return false;
            }

            if (task.m_Id.empty())
            {
                // If the task does not have an explicit "id", use the key from the map.
                task.m_Id = taskKey;
            }

            tasksOut[taskKey] = task;
        }

        return true;
    }

    bool WorkflowJsonParser::ParseTaskInputs(simdjson::ondemand::value& jsonValue, TaskIOMap& inputsOut,
                                             std::string& errorMessage) const
    {
        if (!RequireObject(jsonValue, "task.inputs", errorMessage))
        {
            return false;
        }

        auto objectResult = jsonValue.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'inputs' object: ";
            errorMessage += simdjson::error_message(objectResult.error());
            return false;
        }

        simdjson::ondemand::object inputsObject = objectResult.value();

        for (auto field : inputsObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read input key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (!RequireObject(value, "task.inputs entry", errorMessage))
            {
                return false;
            }

            auto subObjectResult = value.get_object();
            if (subObjectResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read task input definition object: ";
                errorMessage += simdjson::error_message(subObjectResult.error());
                return false;
            }

            simdjson::ondemand::object subObject = subObjectResult.value();

            TaskIOField ioField;

            for (auto subField : subObject)
            {
                auto subKeyResult = subField.unescaped_key();
                if (subKeyResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "failed to read task input field key: ";
                    errorMessage += simdjson::error_message(subKeyResult.error());
                    return false;
                }

                std::string_view subKeyView = subKeyResult.value();
                std::string subKey(subKeyView.begin(), subKeyView.end());

                simdjson::ondemand::value subValue = subField.value();

                if (subKey == "type")
                {
                    if (!ElementToString(subValue, ioField.m_Type))
                    {
                        errorMessage = "task input field 'type' must be string";
                        return false;
                    }
                }
                else if (subKey == "required")
                {
                    auto boolResult = subValue.get_bool();
                    if (boolResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "task input field 'required' must be bool";
                        return false;
                    }

                    ioField.m_IsRequired = boolResult.value();
                }
                else if (subKey == "default")
                {
                    if (!ElementToString(subValue, ioField.m_Default))
                    {
                        errorMessage = "task input field 'default' must be string";
                        return false;
                    }
                }
                else
                {
                    LOG_CORE_WARN("Unknown field in workflow task input '{}': {}", key, subKey);
                }
            }

            inputsOut[key] = ioField;
        }

        return true;
    }

    bool WorkflowJsonParser::ParseTaskOutputs(simdjson::ondemand::value& jsonValue, TaskIOMap& outputsOut,
                                              std::string& errorMessage) const
    {
        if (!RequireObject(jsonValue, "task.outputs", errorMessage))
        {
            return false;
        }

        auto objectResult = jsonValue.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'outputs' object: ";
            errorMessage += simdjson::error_message(objectResult.error());
            return false;
        }

        simdjson::ondemand::object outputsObject = objectResult.value();

        for (auto field : outputsObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read output key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (!RequireObject(value, "task.outputs entry", errorMessage))
            {
                return false;
            }

            auto subObjectResult = value.get_object();
            if (subObjectResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read task output definition object: ";
                errorMessage += simdjson::error_message(subObjectResult.error());
                return false;
            }

            simdjson::ondemand::object subObject = subObjectResult.value();

            TaskIOField ioField;

            for (auto subField : subObject)
            {
                auto subKeyResult = subField.unescaped_key();
                if (subKeyResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "failed to read task output field key: ";
                    errorMessage += simdjson::error_message(subKeyResult.error());
                    return false;
                }

                std::string_view subKeyView = subKeyResult.value();
                std::string subKey(subKeyView.begin(), subKeyView.end());

                simdjson::ondemand::value subValue = subField.value();

                if (subKey == "type")
                {
                    if (!ElementToString(subValue, ioField.m_Type))
                    {
                        errorMessage = "task output field 'type' must be string";
                        return false;
                    }
                }
                else if (subKey == "required")
                {
                    auto boolResult = subValue.get_bool();
                    if (boolResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "task output field 'required' must be bool";
                        return false;
                    }

                    ioField.m_IsRequired = boolResult.value();
                }
                else
                {
                    LOG_CORE_WARN("Unknown field in workflow task output '{}': {}", key, subKey);
                }
            }

            outputsOut[key] = ioField;
        }

        return true;
    }

    bool WorkflowJsonParser::ParseTaskEnvironment(simdjson::ondemand::value& jsonValue, TaskEnvironment& environmentOut,
                                                  std::string& errorMessage) const
    {
        if (!RequireObject(jsonValue, "task.environment", errorMessage))
        {
            return false;
        }

        auto objectResult = jsonValue.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'environment' object: ";
            errorMessage += simdjson::error_message(objectResult.error());
            return false;
        }

        simdjson::ondemand::object envObject = objectResult.value();

        for (auto field : envObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read environment field key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "name")
            {
                ElementToString(value, environmentOut.m_Name);
            }
            else if (key == "assistant_id")
            {
                ElementToString(value, environmentOut.m_AssistantId);
            }
            else if (key == "variables")
            {
                if (!RequireObject(value, "task.environment.variables", errorMessage))
                {
                    return false;
                }

                auto varsObjectResult = value.get_object();
                if (varsObjectResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "failed to read 'variables' object: ";
                    errorMessage += simdjson::error_message(varsObjectResult.error());
                    return false;
                }

                simdjson::ondemand::object varsObject = varsObjectResult.value();

                for (auto variableField : varsObject)
                {
                    auto varKeyResult = variableField.unescaped_key();
                    if (varKeyResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "failed to read environment variable key: ";
                        errorMessage += simdjson::error_message(varKeyResult.error());
                        return false;
                    }

                    std::string_view varKeyView = varKeyResult.value();
                    std::string variableKey(varKeyView.begin(), varKeyView.end());

                    simdjson::ondemand::value variableValue = variableField.value();

                    auto jsonResult = simdjson::to_json_string(variableValue);
                    if (jsonResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "failed to serialize environment variable value";
                        return false;
                    }

                    std::string_view jsonView = jsonResult.value();
                    std::string variableStringValue(jsonView.begin(), jsonView.end());

                    environmentOut.m_Variables[variableKey] = variableStringValue;
                }
            }
            else
            {
                LOG_CORE_WARN("Unknown field in task environment: {}", key);
            }
        }

        return true;
    }

    bool WorkflowJsonParser::ParseTaskQueueBinding(simdjson::ondemand::value& jsonValue, QueueBinding& bindingOut,
                                                   std::string& errorMessage) const
    {
        return ::AIAssistant::ParseTaskQueueBinding(jsonValue, bindingOut, errorMessage);
    }

    // ---------------------------------------------------------------------
    // Dataflow
    // ---------------------------------------------------------------------

    bool WorkflowJsonParser::ParseDataflow(simdjson::ondemand::value& jsonValue, std::vector<DataflowDef>& dataflowsOut,
                                           std::string& errorMessage) const
    {
        if (!RequireArray(jsonValue, "dataflow", errorMessage))
        {
            return false;
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'dataflow' array: ";
            errorMessage += simdjson::error_message(arrayResult.error());
            return false;
        }

        simdjson::ondemand::array dataflowArray = arrayResult.value();

        for (simdjson::ondemand::value entryValue : dataflowArray)
        {
            DataflowDef dataflowDefinition;

            auto objectResult = entryValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "dataflow entry must be an object: ";
                errorMessage += simdjson::error_message(objectResult.error());
                return false;
            }

            simdjson::ondemand::object entryObject = objectResult.value();

            if (!ParseSingleDataflow(entryObject, dataflowDefinition, errorMessage))
            {
                return false;
            }

            dataflowsOut.push_back(dataflowDefinition);
        }

        return true;
    }

    bool WorkflowJsonParser::ParseSingleDataflow(simdjson::ondemand::object& jsonObject, DataflowDef& dataflowOut,
                                                 std::string& errorMessage) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read dataflow field key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "from_task")
            {
                if (!ElementToString(value, dataflowOut.m_FromTask))
                {
                    errorMessage = "dataflow field 'from_task' must be string";
                    return false;
                }
            }
            else if (key == "from_output")
            {
                if (!ElementToString(value, dataflowOut.m_FromOutput))
                {
                    errorMessage = "dataflow field 'from_output' must be string";
                    return false;
                }
            }
            else if (key == "to_task")
            {
                if (!ElementToString(value, dataflowOut.m_ToTask))
                {
                    errorMessage = "dataflow field 'to_task' must be string";
                    return false;
                }
            }
            else if (key == "to_input")
            {
                if (!ElementToString(value, dataflowOut.m_ToInput))
                {
                    errorMessage = "dataflow field 'to_input' must be string";
                    return false;
                }
            }
            else if (key == "mapping")
            {
                if (!RequireObject(value, "dataflow.mapping", errorMessage))
                {
                    return false;
                }

                auto mappingObjectResult = value.get_object();
                if (mappingObjectResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "failed to read 'mapping' object: ";
                    errorMessage += simdjson::error_message(mappingObjectResult.error());
                    return false;
                }

                simdjson::ondemand::object mappingObject = mappingObjectResult.value();

                for (auto mappingField : mappingObject)
                {
                    auto mappingKeyResult = mappingField.unescaped_key();
                    if (mappingKeyResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "failed to read dataflow mapping key: ";
                        errorMessage += simdjson::error_message(mappingKeyResult.error());
                        return false;
                    }

                    std::string_view mappingKeyView = mappingKeyResult.value();
                    std::string mappingKey(mappingKeyView.begin(), mappingKeyView.end());

                    simdjson::ondemand::value mappingValue = mappingField.value();

                    auto jsonResult = simdjson::to_json_string(mappingValue);
                    if (jsonResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "failed to serialize dataflow mapping value";
                        return false;
                    }

                    std::string_view jsonView = jsonResult.value();
                    std::string mappingStringValue(jsonView.begin(), jsonView.end());

                    // Strip surrounding JSON quotes from string values so they are stored clean.
                    if (mappingStringValue.size() >= 2 && mappingStringValue.front() == '"' &&
                        mappingStringValue.back() == '"')
                    {
                        mappingStringValue = mappingStringValue.substr(1, mappingStringValue.size() - 2);
                    }

                    dataflowOut.m_Mapping[mappingKey] = mappingStringValue;
                }
            }
            else
            {
                LOG_CORE_WARN("Unknown field in dataflow: {}", key);
            }
        }

        if (dataflowOut.m_FromTask.empty() || dataflowOut.m_FromOutput.empty() || dataflowOut.m_ToTask.empty() ||
            dataflowOut.m_ToInput.empty())
        {
            errorMessage = "dataflow entry missing required fields (from_task, from_output, to_task, to_input)";
            return false;
        }

        return true;
    }

    // ---------------------------------------------------------------------
    // Retries
    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // Filters (v1.1)
    // ---------------------------------------------------------------------

    bool WorkflowJsonParser::ParseFilters(simdjson::ondemand::value& jsonValue, std::vector<FilterDef>& filtersOut,
                                          std::string& errorMessage) const
    {
        if (!RequireArray(jsonValue, "filters", errorMessage))
        {
            return false;
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            errorMessage = "failed to read 'filters' array: ";
            errorMessage += simdjson::error_message(arrayResult.error());
            return false;
        }

        simdjson::ondemand::array filterArray = arrayResult.value();

        for (simdjson::ondemand::value filterValue : filterArray)
        {
            FilterDef filter;

            auto objectResult = filterValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "filter entry must be an object: ";
                errorMessage += simdjson::error_message(objectResult.error());
                return false;
            }

            simdjson::ondemand::object filterObject = objectResult.value();

            if (!ParseFilter(filterObject, filter, errorMessage))
            {
                return false;
            }

            filtersOut.push_back(std::move(filter));
        }

        return true;
    }

    bool WorkflowJsonParser::ParseFilter(simdjson::ondemand::object& jsonObject, FilterDef& filterOut,
                                         std::string& errorMessage) const
    {
        bool hasId = false;
        bool hasSource = false;
        bool hasBinding = false;

        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read filter field key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "id")
            {
                if (!ElementToString(value, filterOut.m_Id))
                {
                    errorMessage = "filter field 'id' must be string";
                    return false;
                }

                hasId = true;
            }
            else if (key == "source")
            {
                if (!RequireObject(value, "filter.source", errorMessage))
                {
                    return false;
                }

                auto sourceObjectResult = value.get_object();
                if (sourceObjectResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "failed to read filter 'source' object: ";
                    errorMessage += simdjson::error_message(sourceObjectResult.error());
                    return false;
                }

                simdjson::ondemand::object sourceObject = sourceObjectResult.value();

                if (!ParseFilterSource(sourceObject, filterOut.m_Source, errorMessage))
                {
                    return false;
                }

                hasSource = true;
            }
            else if (key == "binding")
            {
                if (!ElementToString(value, filterOut.m_Binding))
                {
                    errorMessage = "filter field 'binding' must be string";
                    return false;
                }

                hasBinding = true;
            }
            else if (key == "max_items")
            {
                auto maxItemsResult = value.get_int64();
                if (maxItemsResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "filter field 'max_items' must be integer";
                    return false;
                }

                filterOut.m_MaxItems = static_cast<uint32_t>(maxItemsResult.value());
            }
            else
            {
                LOG_CORE_WARN("Unknown field in filter '{}': {}", filterOut.m_Id, key);
            }
        }

        if (!hasId)
        {
            errorMessage = "filter missing required field: id";
            return false;
        }

        if (!hasSource)
        {
            errorMessage = "filter '" + filterOut.m_Id + "' missing required field: source";
            return false;
        }

        if (!hasBinding)
        {
            errorMessage = "filter '" + filterOut.m_Id + "' missing required field: binding";
            return false;
        }

        if (filterOut.m_Source.m_Kind.empty())
        {
            errorMessage = "filter '" + filterOut.m_Id + "' source missing required field: kind";
            return false;
        }

        LOG_APP_INFO("[filter] parsed filter id={} kind={} binding={} maxItems={}", filterOut.m_Id,
                     filterOut.m_Source.m_Kind, filterOut.m_Binding, filterOut.m_MaxItems);

        return true;
    }

    bool WorkflowJsonParser::ParseFilterSource(simdjson::ondemand::object& jsonObject, FilterSource& sourceOut,
                                               std::string& errorMessage) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read filter source field key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "kind")
            {
                if (!ElementToString(value, sourceOut.m_Kind))
                {
                    errorMessage = "filter source field 'kind' must be string";
                    return false;
                }
            }
            else if (key == "path")
            {
                if (!ElementToString(value, sourceOut.m_Path))
                {
                    errorMessage = "filter source field 'path' must be string";
                    return false;
                }
            }
            else if (key == "delimiter")
            {
                if (!ElementToString(value, sourceOut.m_Delimiter))
                {
                    errorMessage = "filter source field 'delimiter' must be string";
                    return false;
                }
            }
            else if (key == "has_header")
            {
                auto boolResult = value.get_bool();
                if (boolResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "filter source field 'has_header' must be bool";
                    return false;
                }

                sourceOut.m_HasHeader = boolResult.value();
            }
            else if (key == "range")
            {
                if (!ElementToString(value, sourceOut.m_Range))
                {
                    errorMessage = "filter source field 'range' must be string";
                    return false;
                }
            }
            else if (key == "skip_empty")
            {
                auto boolResult = value.get_bool();
                if (boolResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "filter source field 'skip_empty' must be bool";
                    return false;
                }

                sourceOut.m_SkipEmpty = boolResult.value();
            }
            else if (key == "index_path")
            {
                if (!ElementToString(value, sourceOut.m_IndexPath))
                {
                    errorMessage = "filter source field 'index_path' must be string";
                    return false;
                }
            }
            else if (key == "query")
            {
                if (!ElementToString(value, sourceOut.m_Query))
                {
                    errorMessage = "filter source field 'query' must be string";
                    return false;
                }
            }
            else if (key == "fields")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "filter source field 'fields' must be array of strings";
                    return false;
                }

                simdjson::ondemand::array fieldsArray = arrayResult.value();
                for (simdjson::ondemand::value fieldValue : fieldsArray)
                {
                    auto stringResult = fieldValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "filter source field 'fields' must be array of strings";
                        return false;
                    }

                    std::string_view fieldView = stringResult.value();
                    sourceOut.m_Fields.emplace_back(fieldView.begin(), fieldView.end());
                }
            }
            else if (key == "connection")
            {
                if (!ElementToString(value, sourceOut.m_Connection))
                {
                    errorMessage = "filter source field 'connection' must be string";
                    return false;
                }
            }
            else if (key == "base_url")
            {
                if (!ElementToString(value, sourceOut.m_BaseUrl))
                {
                    errorMessage = "filter source field 'base_url' must be string";
                    return false;
                }
            }
            else if (key == "project_id")
            {
                if (!ElementToString(value, sourceOut.m_ProjectId))
                {
                    errorMessage = "filter source field 'project_id' must be string";
                    return false;
                }
            }
            else if (key == "key_name")
            {
                if (!ElementToString(value, sourceOut.m_KeyName))
                {
                    errorMessage = "filter source field 'key_name' must be string";
                    return false;
                }
            }
            else if (key == "page_size")
            {
                auto pageSizeResult = value.get_int64();
                if (pageSizeResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "filter source field 'page_size' must be integer";
                    return false;
                }

                sourceOut.m_PageSize = static_cast<uint32_t>(pageSizeResult.value());
            }
            else
            {
                LOG_CORE_WARN("Unknown field in filter source: {}", key);
            }
        }

        return true;
    }

} // namespace AIAssistant
