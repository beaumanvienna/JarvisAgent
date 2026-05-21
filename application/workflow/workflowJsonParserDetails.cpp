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
#include "workflow/workflowJsonParserDetails.h"

namespace AIAssistant
{

    namespace
    {
        std::expected<void, ParserError>
            RequireObject(simdjson::ondemand::value& value, std::string const& context)
        {
            auto typeResult = value.type();
            if (typeResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    "failed to get type for " + context + ": " + simdjson::error_message(typeResult.error())));
            }

            if (typeResult.value() != simdjson::ondemand::json_type::object)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                    context + " must be an object"));
            }

            return {};
        }

        std::expected<void, ParserError>
            RequireArray(simdjson::ondemand::value& value, std::string const& context)
        {
            auto typeResult = value.type();
            if (typeResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    "failed to get type for " + context + ": " + simdjson::error_message(typeResult.error())));
            }

            if (typeResult.value() != simdjson::ondemand::json_type::array)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                    context + " must be an array"));
            }

            return {};
        }
    } // anonymous namespace

    // ---------------------------------------------------------------------
    // Utility helpers
    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // Triggers
    // ---------------------------------------------------------------------

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseTriggers(simdjson::ondemand::value& jsonValue,
                                          std::vector<WorkflowTrigger>& triggersOut) const
    {
        if (auto r = RequireArray(jsonValue, "triggers"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'triggers' array: ") + simdjson::error_message(arrayResult.error())));
        }

        simdjson::ondemand::array triggerArray = arrayResult.value();

        for (simdjson::ondemand::value triggerValue : triggerArray)
        {
            if (triggersOut.size() >= WorkflowParserLimits::kMaxTriggers)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                    "'triggers' exceeds max count (" +
                        std::to_string(WorkflowParserLimits::kMaxTriggers) + ")"));
            }
            WorkflowTrigger trigger;

            auto objectResult = triggerValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                    std::string("trigger entry must be an object: ") +
                        simdjson::error_message(objectResult.error())));
            }

            simdjson::ondemand::object triggerObject = objectResult.value();

            if (auto r = ParseTrigger(triggerObject, trigger); !r)
            {
                return std::unexpected(std::move(r.error()));
            }

            triggersOut.push_back(trigger);
        }

        return {};
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

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseControlNodes(simdjson::ondemand::value& jsonValue,
                                              std::vector<ControlNodeDef>& controlNodesOut) const
    {
        controlNodesOut.clear();

        if (auto r = RequireArray(jsonValue, "control_nodes"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'control_nodes' array: ") + simdjson::error_message(arrayResult.error())));
        }

        simdjson::ondemand::array nodeArray = arrayResult.value();
        for (simdjson::ondemand::value entryValue : nodeArray)
        {
            if (controlNodesOut.size() >= WorkflowParserLimits::kMaxControlNodes)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                    "'control_nodes' exceeds max count (" +
                        std::to_string(WorkflowParserLimits::kMaxControlNodes) + ")"));
            }
            auto objectResult = entryValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                    std::string("control_nodes entry must be an object: ") +
                        simdjson::error_message(objectResult.error())));
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
                    return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                        std::string("failed to read control_nodes field key: ") +
                            simdjson::error_message(keyResult.error())));
                }

                std::string_view keyView = keyResult.value();
                std::string key(keyView.begin(), keyView.end());
                simdjson::ondemand::value value = field.value();

                if (key == "id")
                {
                    if (!ElementToString(value, node.m_Id))
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "control_nodes field 'id' must be string"));
                    }
                    hasId = true;
                }
                else if (key == "type")
                {
                    std::string typeString;
                    if (!ElementToString(value, typeString))
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "control_nodes field 'type' must be string"));
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
                return std::unexpected(ParserError::Make(
                    ParserErrorCode::MissingField, "control_nodes entry missing required field: id"));
            }

            if (!hasType)
            {
                return std::unexpected(ParserError::Make(
                    ParserErrorCode::MissingField, "control_nodes entry missing required field: type"));
            }

            controlNodesOut.push_back(std::move(node));
        }

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseControlflow(simdjson::ondemand::value& jsonValue,
                                             std::vector<ControlflowEdgeDef>& controlflowOut) const
    {
        controlflowOut.clear();

        if (auto r = RequireArray(jsonValue, "controlflow"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'controlflow' array: ") + simdjson::error_message(arrayResult.error())));
        }

        simdjson::ondemand::array edgeArray = arrayResult.value();
        for (simdjson::ondemand::value entryValue : edgeArray)
        {
            if (controlflowOut.size() >= WorkflowParserLimits::kMaxControlflowEdges)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                    "'controlflow' exceeds max count (" +
                        std::to_string(WorkflowParserLimits::kMaxControlflowEdges) + ")"));
            }
            auto objectResult = entryValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                    std::string("controlflow entry must be an object: ") +
                        simdjson::error_message(objectResult.error())));
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
                    return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                        std::string("failed to read controlflow field key: ") +
                            simdjson::error_message(keyResult.error())));
                }

                std::string_view keyView = keyResult.value();
                std::string key(keyView.begin(), keyView.end());
                simdjson::ondemand::value value = field.value();

                if (key == "from")
                {
                    if (!ElementToString(value, edge.m_From))
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "controlflow field 'from' must be string"));
                    }
                    hasFrom = true;
                }
                else if (key == "to")
                {
                    if (!ElementToString(value, edge.m_To))
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "controlflow field 'to' must be string"));
                    }
                    hasTo = true;
                }
                else if (key == "kind")
                {
                    std::string kindString;
                    if (!ElementToString(value, kindString))
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "controlflow field 'kind' must be string"));
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
                return std::unexpected(ParserError::Make(ParserErrorCode::MissingField,
                    "controlflow entry missing required fields (from, to, kind)"));
            }

            controlflowOut.push_back(std::move(edge));
        }

        return {};
    }

    // ---------------------------------------------------------------------
    // Tasks
    // ---------------------------------------------------------------------

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseTasks(simdjson::ondemand::value& jsonValue,
                                       std::unordered_map<std::string, TaskDef>& tasksOut) const
    {
        if (auto r = RequireObject(jsonValue, "tasks"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto objectResult = jsonValue.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'tasks' object: ") + simdjson::error_message(objectResult.error())));
        }

        simdjson::ondemand::object tasksObject = objectResult.value();

        for (auto field : tasksObject)
        {
            if (tasksOut.size() >= WorkflowParserLimits::kMaxTasks)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                    "'tasks' exceeds max count (" + std::to_string(WorkflowParserLimits::kMaxTasks) + ")"));
            }
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read task key: ") + simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string taskKey(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            auto taskObjectResult = value.get_object();
            if (taskObjectResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                    std::string("task entry must be an object: ") +
                        simdjson::error_message(taskObjectResult.error())));
            }

            simdjson::ondemand::object taskObject = taskObjectResult.value();

            TaskDef task;
            if (auto r = ParseTask(taskObject, task); !r)
            {
                return std::unexpected(std::move(r.error()));
            }

            if (task.m_Id.empty())
            {
                // If the task does not have an explicit "id", use the key from the map.
                task.m_Id = taskKey;
            }

            tasksOut[taskKey] = task;
        }

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseTaskInputs(simdjson::ondemand::value& jsonValue, TaskIOMap& inputsOut) const
    {
        if (auto r = RequireObject(jsonValue, "task.inputs"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto objectResult = jsonValue.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'inputs' object: ") + simdjson::error_message(objectResult.error())));
        }

        simdjson::ondemand::object inputsObject = objectResult.value();

        for (auto field : inputsObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read input key: ") + simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (auto r = RequireObject(value, "task.inputs entry"); !r)
            {
                return std::unexpected(std::move(r.error()));
            }

            auto subObjectResult = value.get_object();
            if (subObjectResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read task input definition object: ") +
                        simdjson::error_message(subObjectResult.error())));
            }

            simdjson::ondemand::object subObject = subObjectResult.value();

            TaskIOField ioField;

            for (auto subField : subObject)
            {
                auto subKeyResult = subField.unescaped_key();
                if (subKeyResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                        std::string("failed to read task input field key: ") +
                            simdjson::error_message(subKeyResult.error())));
                }

                std::string_view subKeyView = subKeyResult.value();
                std::string subKey(subKeyView.begin(), subKeyView.end());

                simdjson::ondemand::value subValue = subField.value();

                if (subKey == "type")
                {
                    if (!ElementToString(subValue, ioField.m_Type))
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task input field 'type' must be string"));
                    }
                }
                else if (subKey == "required")
                {
                    auto boolResult = subValue.get_bool();
                    if (boolResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task input field 'required' must be bool"));
                    }

                    ioField.m_IsRequired = boolResult.value();
                }
                else if (subKey == "default")
                {
                    if (!ElementToString(subValue, ioField.m_Default))
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task input field 'default' must be string"));
                    }
                }
                else
                {
                    LOG_CORE_WARN("Unknown field in workflow task input '{}': {}", key, subKey);
                }
            }

            inputsOut[key] = ioField;
        }

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseTaskOutputs(simdjson::ondemand::value& jsonValue, TaskIOMap& outputsOut) const
    {
        if (auto r = RequireObject(jsonValue, "task.outputs"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto objectResult = jsonValue.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'outputs' object: ") + simdjson::error_message(objectResult.error())));
        }

        simdjson::ondemand::object outputsObject = objectResult.value();

        for (auto field : outputsObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read output key: ") + simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (auto r = RequireObject(value, "task.outputs entry"); !r)
            {
                return std::unexpected(std::move(r.error()));
            }

            auto subObjectResult = value.get_object();
            if (subObjectResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read task output definition object: ") +
                        simdjson::error_message(subObjectResult.error())));
            }

            simdjson::ondemand::object subObject = subObjectResult.value();

            TaskIOField ioField;

            for (auto subField : subObject)
            {
                auto subKeyResult = subField.unescaped_key();
                if (subKeyResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                        std::string("failed to read task output field key: ") +
                            simdjson::error_message(subKeyResult.error())));
                }

                std::string_view subKeyView = subKeyResult.value();
                std::string subKey(subKeyView.begin(), subKeyView.end());

                simdjson::ondemand::value subValue = subField.value();

                if (subKey == "type")
                {
                    if (!ElementToString(subValue, ioField.m_Type))
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task output field 'type' must be string"));
                    }
                }
                else if (subKey == "required")
                {
                    auto boolResult = subValue.get_bool();
                    if (boolResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task output field 'required' must be bool"));
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

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseTaskEnvironment(simdjson::ondemand::value& jsonValue,
                                                 TaskEnvironment& environmentOut) const
    {
        if (auto r = RequireObject(jsonValue, "task.environment"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto objectResult = jsonValue.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'environment' object: ") + simdjson::error_message(objectResult.error())));
        }

        simdjson::ondemand::object envObject = objectResult.value();

        for (auto field : envObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read environment field key: ") +
                        simdjson::error_message(keyResult.error())));
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
                if (auto r = RequireObject(value, "task.environment.variables"); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }

                auto varsObjectResult = value.get_object();
                if (varsObjectResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                        std::string("failed to read 'variables' object: ") +
                            simdjson::error_message(varsObjectResult.error())));
                }

                simdjson::ondemand::object varsObject = varsObjectResult.value();

                for (auto variableField : varsObject)
                {
                    auto varKeyResult = variableField.unescaped_key();
                    if (varKeyResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                            std::string("failed to read environment variable key: ") +
                                simdjson::error_message(varKeyResult.error())));
                    }

                    std::string_view varKeyView = varKeyResult.value();
                    std::string variableKey(varKeyView.begin(), varKeyView.end());

                    simdjson::ondemand::value variableValue = variableField.value();

                    auto jsonResult = simdjson::to_json_string(variableValue);
                    if (jsonResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                            "failed to serialize environment variable value"));
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

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseTaskQueueBinding(simdjson::ondemand::value& jsonValue, QueueBinding& bindingOut) const
    {
        return ::AIAssistant::ParseTaskQueueBinding(jsonValue, bindingOut);
    }

    // ---------------------------------------------------------------------
    // Dataflow
    // ---------------------------------------------------------------------

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseDataflow(simdjson::ondemand::value& jsonValue,
                                          std::vector<DataflowDef>& dataflowsOut) const
    {
        if (auto r = RequireArray(jsonValue, "dataflow"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'dataflow' array: ") + simdjson::error_message(arrayResult.error())));
        }

        simdjson::ondemand::array dataflowArray = arrayResult.value();

        for (simdjson::ondemand::value entryValue : dataflowArray)
        {
            if (dataflowsOut.size() >= WorkflowParserLimits::kMaxDataflows)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                    "'dataflow' exceeds max count (" +
                        std::to_string(WorkflowParserLimits::kMaxDataflows) + ")"));
            }
            DataflowDef dataflowDefinition;

            auto objectResult = entryValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                    std::string("dataflow entry must be an object: ") +
                        simdjson::error_message(objectResult.error())));
            }

            simdjson::ondemand::object entryObject = objectResult.value();

            if (auto r = ParseSingleDataflow(entryObject, dataflowDefinition); !r)
            {
                return std::unexpected(std::move(r.error()));
            }

            dataflowsOut.push_back(dataflowDefinition);
        }

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseSingleDataflow(simdjson::ondemand::object& jsonObject, DataflowDef& dataflowOut) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read dataflow field key: ") +
                        simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "from_task")
            {
                if (!ElementToString(value, dataflowOut.m_FromTask))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "dataflow field 'from_task' must be string"));
                }
            }
            else if (key == "from_output")
            {
                if (!ElementToString(value, dataflowOut.m_FromOutput))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "dataflow field 'from_output' must be string"));
                }
            }
            else if (key == "to_task")
            {
                if (!ElementToString(value, dataflowOut.m_ToTask))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "dataflow field 'to_task' must be string"));
                }
            }
            else if (key == "to_input")
            {
                if (!ElementToString(value, dataflowOut.m_ToInput))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "dataflow field 'to_input' must be string"));
                }
            }
            else if (key == "mapping")
            {
                if (auto r = RequireObject(value, "dataflow.mapping"); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }

                auto mappingObjectResult = value.get_object();
                if (mappingObjectResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                        std::string("failed to read 'mapping' object: ") +
                            simdjson::error_message(mappingObjectResult.error())));
                }

                simdjson::ondemand::object mappingObject = mappingObjectResult.value();

                for (auto mappingField : mappingObject)
                {
                    auto mappingKeyResult = mappingField.unescaped_key();
                    if (mappingKeyResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                            std::string("failed to read dataflow mapping key: ") +
                                simdjson::error_message(mappingKeyResult.error())));
                    }

                    std::string_view mappingKeyView = mappingKeyResult.value();
                    std::string mappingKey(mappingKeyView.begin(), mappingKeyView.end());

                    simdjson::ondemand::value mappingValue = mappingField.value();

                    auto jsonResult = simdjson::to_json_string(mappingValue);
                    if (jsonResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::SimdjsonError, "failed to serialize dataflow mapping value"));
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
            return std::unexpected(ParserError::Make(ParserErrorCode::MissingField,
                "dataflow entry missing required fields (from_task, from_output, to_task, to_input)"));
        }

        return {};
    }

    // ---------------------------------------------------------------------
    // Retries
    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // Filters (v1.1)
    // ---------------------------------------------------------------------

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseFilters(simdjson::ondemand::value& jsonValue,
                                         std::vector<FilterDef>& filtersOut) const
    {
        if (auto r = RequireArray(jsonValue, "filters"); !r)
        {
            return std::unexpected(std::move(r.error()));
        }

        auto arrayResult = jsonValue.get_array();
        if (arrayResult.error() != simdjson::SUCCESS)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("failed to read 'filters' array: ") + simdjson::error_message(arrayResult.error())));
        }

        simdjson::ondemand::array filterArray = arrayResult.value();

        for (simdjson::ondemand::value filterValue : filterArray)
        {
            if (filtersOut.size() >= WorkflowParserLimits::kMaxFilters)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                    "'filters' exceeds max count (" +
                        std::to_string(WorkflowParserLimits::kMaxFilters) + ")"));
            }
            FilterDef filter;

            auto objectResult = filterValue.get_object();
            if (objectResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                    std::string("filter entry must be an object: ") +
                        simdjson::error_message(objectResult.error())));
            }

            simdjson::ondemand::object filterObject = objectResult.value();

            if (auto r = ParseFilter(filterObject, filter); !r)
            {
                return std::unexpected(std::move(r.error()));
            }

            filtersOut.push_back(std::move(filter));
        }

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseFilter(simdjson::ondemand::object& jsonObject, FilterDef& filterOut) const
    {
        bool hasId = false;
        bool hasSource = false;
        bool hasBinding = false;

        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read filter field key: ") +
                        simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "id")
            {
                if (!ElementToString(value, filterOut.m_Id))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter field 'id' must be string"));
                }

                hasId = true;
            }
            else if (key == "source")
            {
                if (auto r = RequireObject(value, "filter.source"); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }

                auto sourceObjectResult = value.get_object();
                if (sourceObjectResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                        std::string("failed to read filter 'source' object: ") +
                            simdjson::error_message(sourceObjectResult.error())));
                }

                simdjson::ondemand::object sourceObject = sourceObjectResult.value();

                if (auto r = ParseFilterSource(sourceObject, filterOut.m_Source); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }

                hasSource = true;
            }
            else if (key == "binding")
            {
                if (!ElementToString(value, filterOut.m_Binding))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter field 'binding' must be string"));
                }

                hasBinding = true;
            }
            else if (key == "max_items")
            {
                auto maxItemsResult = value.get_int64();
                if (maxItemsResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter field 'max_items' must be integer"));
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
            return std::unexpected(ParserError::Make(
                ParserErrorCode::MissingField, "filter missing required field: id"));
        }

        if (!hasSource)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::MissingField,
                "filter '" + filterOut.m_Id + "' missing required field: source"));
        }

        if (!hasBinding)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::MissingField,
                "filter '" + filterOut.m_Id + "' missing required field: binding"));
        }

        if (filterOut.m_Source.m_Kind.empty())
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::MissingField,
                "filter '" + filterOut.m_Id + "' source missing required field: kind"));
        }

        LOG_APP_INFO("[filter] parsed filter id={} kind={} binding={} maxItems={}", filterOut.m_Id,
                     filterOut.m_Source.m_Kind, filterOut.m_Binding, filterOut.m_MaxItems);

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseFilterSource(simdjson::ondemand::object& jsonObject, FilterSource& sourceOut) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read filter source field key: ") +
                        simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "kind")
            {
                if (!ElementToString(value, sourceOut.m_Kind))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'kind' must be string"));
                }
            }
            else if (key == "path")
            {
                if (!ElementToString(value, sourceOut.m_Path))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'path' must be string"));
                }
            }
            else if (key == "delimiter")
            {
                if (!ElementToString(value, sourceOut.m_Delimiter))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'delimiter' must be string"));
                }
            }
            else if (key == "has_header")
            {
                auto boolResult = value.get_bool();
                if (boolResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'has_header' must be bool"));
                }

                sourceOut.m_HasHeader = boolResult.value();
            }
            else if (key == "range")
            {
                if (!ElementToString(value, sourceOut.m_Range))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'range' must be string"));
                }
            }
            else if (key == "skip_empty")
            {
                auto boolResult = value.get_bool();
                if (boolResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'skip_empty' must be bool"));
                }

                sourceOut.m_SkipEmpty = boolResult.value();
            }
            else if (key == "index_path")
            {
                if (!ElementToString(value, sourceOut.m_IndexPath))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'index_path' must be string"));
                }
            }
            else if (key == "query")
            {
                if (!ElementToString(value, sourceOut.m_Query))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'query' must be string"));
                }
            }
            else if (key == "fields")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'fields' must be array of strings"));
                }

                simdjson::ondemand::array fieldsArray = arrayResult.value();
                for (simdjson::ondemand::value fieldValue : fieldsArray)
                {
                    auto stringResult = fieldValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "filter source field 'fields' must be array of strings"));
                    }

                    std::string_view fieldView = stringResult.value();
                    sourceOut.m_Fields.emplace_back(fieldView.begin(), fieldView.end());
                }
            }
            else if (key == "connection")
            {
                if (!ElementToString(value, sourceOut.m_Connection))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'connection' must be string"));
                }
            }
            else if (key == "base_url")
            {
                if (!ElementToString(value, sourceOut.m_BaseUrl))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'base_url' must be string"));
                }
            }
            else if (key == "project_id")
            {
                if (!ElementToString(value, sourceOut.m_ProjectId))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'project_id' must be string"));
                }
            }
            else if (key == "key_name")
            {
                if (!ElementToString(value, sourceOut.m_KeyName))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'key_name' must be string"));
                }
            }
            else if (key == "page_size")
            {
                auto pageSizeResult = value.get_int64();
                if (pageSizeResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "filter source field 'page_size' must be integer"));
                }

                sourceOut.m_PageSize = static_cast<uint32_t>(pageSizeResult.value());
            }
            else
            {
                LOG_CORE_WARN("Unknown field in filter source: {}", key);
            }
        }

        return {};
    }

} // namespace AIAssistant
