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

} // namespace AIAssistant
