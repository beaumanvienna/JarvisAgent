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

#include "workflow/workflowJsonParserDetails.h"
#include "workflow/workflowJsonParser.h"

#include <optional>
#include <string_view>

#include "engine.h"

namespace AIAssistant
{
    static bool ReadQueueFileRefArray(simdjson::ondemand::value& value, std::vector<QueueFileRef>& outFileRefs,
                                      std::string const& context, std::string& outErrorMessage)
    {
        outFileRefs.clear();

        simdjson::ondemand::array array;
        auto const arrayError = value.get_array().get(array);
        if (arrayError)
        {
            outErrorMessage = context + " must be an array";
            return false;
        }

        for (simdjson::ondemand::value item : array)
        {
            // Either "path string" OR {"path":"...", "content":"..."}.
            if (item.type() == simdjson::ondemand::json_type::string)
            {
                std::string_view pathText;
                auto const stringError = item.get_string().get(pathText);
                if (stringError)
                {
                    outErrorMessage = context + " contains an invalid string";
                    return false;
                }

                QueueFileRef fileRef{};
                fileRef.m_Path = std::string(pathText);
                fileRef.m_HasInlineContent = false;
                outFileRefs.push_back(std::move(fileRef));
                continue;
            }

            if (item.type() == simdjson::ondemand::json_type::object)
            {
                simdjson::ondemand::object object;
                auto const objectError = item.get_object().get(object);
                if (objectError)
                {
                    outErrorMessage = context + " contains an invalid object";
                    return false;
                }

                std::string_view pathText;
                std::string_view contentText;

                auto const pathError = object["path"].get_string().get(pathText);
                if (pathError)
                {
                    outErrorMessage = context + " object is missing 'path' (string)";
                    return false;
                }

                auto const contentError = object["content"].get_string().get(contentText);
                if (contentError)
                {
                    outErrorMessage = context + " object is missing 'content' (string)";
                    return false;
                }

                QueueFileRef fileRef{};
                fileRef.m_Path = std::string(pathText);
                fileRef.m_Content = std::string(contentText);
                fileRef.m_HasInlineContent = true;
                outFileRefs.push_back(std::move(fileRef));
                continue;
            }

            outErrorMessage = context + " contains an unsupported item type (expected string or object)";
            return false;
        }

        return true;
    }

    bool ParseTaskQueueBinding(simdjson::ondemand::value& value, QueueBinding& binding, std::string& errorMessage)
    {
        binding = {};

        simdjson::ondemand::object obj;
        auto const objError = value.get_object().get(obj);
        if (objError)
        {
            errorMessage = "queue_binding must be an object";
            return false;
        }

        auto readArray = [&](char const* fieldName, std::vector<QueueFileRef>& destination) -> bool
        {
            simdjson::ondemand::value fieldValue;
            auto const fieldError = obj[fieldName].get(fieldValue);
            if (fieldError)
            {
                return true; // optional
            }

            return ReadQueueFileRefArray(fieldValue, destination, std::string("queue_binding.") + fieldName, errorMessage);
        };

        if (!readArray("stng_files", binding.m_StngFiles))
        {
            return false;
        }

        if (!readArray("task_files", binding.m_TaskFiles))
        {
            return false;
        }

        if (!readArray("cntx_files", binding.m_CntxFiles))
        {
            return false;
        }

        if (!readArray("prob_files", binding.m_ProbFiles))
        {
            return false;
        }

        return true;
    }

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

    bool WorkflowJsonParser::ExtractRawJson(simdjson::ondemand::value& element, std::string& rawJsonOut) const
    {
        auto jsonResult = simdjson::to_json_string(element);
        if (jsonResult.error() != simdjson::SUCCESS)
        {
            rawJsonOut.clear();
            return false;
        }

        std::string_view jsonView = jsonResult.value();
        rawJsonOut.assign(jsonView.begin(), jsonView.end());
        return true;
    }

    bool WorkflowJsonParser::ElementToString(simdjson::ondemand::value& element, std::string& output) const
    {
        auto typeResult = element.type();
        if (typeResult.error() != simdjson::SUCCESS)
        {
            return false;
        }

        simdjson::ondemand::json_type type = typeResult.value();

        if (type == simdjson::ondemand::json_type::string)
        {
            auto stringResult = element.get_string(false);
            if (stringResult.error() != simdjson::SUCCESS)
            {
                return false;
            }

            std::string_view stringView = stringResult.value();
            output.assign(stringView.begin(), stringView.end());
            return true;
        }
        else if (type == simdjson::ondemand::json_type::number || type == simdjson::ondemand::json_type::boolean)
        {
            auto jsonResult = simdjson::to_json_string(element);
            if (jsonResult.error() != simdjson::SUCCESS)
            {
                return false;
            }

            std::string_view jsonView = jsonResult.value();
            output.assign(jsonView.begin(), jsonView.end());
            return true;
        }

        return false;
    }

    TaskMode WorkflowJsonParser::StringToTaskMode(std::string const& rawMode) const
    {
        if (rawMode == "single")
        {
            return TaskMode::Single;
        }

        if (rawMode == "per_item")
        {
            return TaskMode::PerItem;
        }

        LOG_CORE_WARN("Unknown task mode '{}', defaulting to Single", rawMode);
        return TaskMode::Single;
    }

    TaskType WorkflowJsonParser::StringToTaskType(std::string const& rawType) const
    {
        if (rawType == "python")
        {
            return TaskType::Python;
        }

        if (rawType == "shell")
        {
            return TaskType::Shell;
        }

        if (rawType == "ai_call")
        {
            return TaskType::AiCall;
        }

        if (rawType == "internal")
        {
            return TaskType::Internal;
        }

        LOG_CORE_WARN("Unknown task type '{}', defaulting to Internal", rawType);
        return TaskType::Internal;
    }

    WorkflowTriggerType WorkflowJsonParser::StringToTriggerType(std::string const& typeString) const
    {
        if (typeString == "auto")
        {
            return WorkflowTriggerType::Auto;
        }

        if (typeString == "cron")
        {
            return WorkflowTriggerType::Cron;
        }

        if (typeString == "file_watch")
        {
            return WorkflowTriggerType::FileWatch;
        }

        if (typeString == "structure")
        {
            return WorkflowTriggerType::Structure;
        }

        if (typeString == "manual")
        {
            return WorkflowTriggerType::Manual;
        }

        LOG_CORE_WARN("Unknown trigger type '{}', defaulting to Unknown", typeString);
        return WorkflowTriggerType::Unknown;
    }

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

    bool WorkflowJsonParser::ParseTrigger(simdjson::ondemand::object& jsonObject, WorkflowTrigger& triggerOut,
                                          std::string& errorMessage) const
    {
        bool hasType = false;
        bool hasId = false;

        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read trigger field key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "type")
            {
                std::string typeString;
                if (!ElementToString(value, typeString))
                {
                    errorMessage = "trigger field 'type' must be string";
                    return false;
                }

                triggerOut.m_Type = StringToTriggerType(typeString);
                hasType = true;
            }
            else if (key == "id")
            {
                if (!ElementToString(value, triggerOut.m_Id))
                {
                    errorMessage = "trigger field 'id' must be string";
                    return false;
                }

                hasId = true;
            }
            else if (key == "enabled")
            {
                auto boolResult = value.get_bool();
                if (boolResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "trigger field 'enabled' must be bool";
                    return false;
                }

                triggerOut.m_IsEnabled = boolResult.value();
            }
            else if (key == "params")
            {
                if (!ExtractRawJson(value, triggerOut.m_ParamsJson))
                {
                    errorMessage = "failed to read trigger 'params' JSON";
                    return false;
                }
            }
            else
            {
                LOG_CORE_WARN("Unknown field in trigger '{}': {}", triggerOut.m_Id, key);
            }
        }

        if (!hasType)
        {
            errorMessage = "trigger missing required field: type";
            return false;
        }

        if (!hasId)
        {
            errorMessage = "trigger missing required field: id";
            return false;
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

    bool WorkflowJsonParser::ParseTask(simdjson::ondemand::object& jsonObject, TaskDef& taskOut,
                                       std::string& errorMessage) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read task field key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "id")
            {
                if (!ElementToString(value, taskOut.m_Id))
                {
                    errorMessage = "task field 'id' must be string";
                    return false;
                }
            }
            else if (key == "type")
            {
                std::string typeString;
                if (!ElementToString(value, typeString))
                {
                    errorMessage = "task field 'type' must be string";
                    return false;
                }

                taskOut.m_Type = StringToTaskType(typeString);
            }
            else if (key == "label")
            {
                ElementToString(value, taskOut.m_Label);
            }
            else if (key == "doc")
            {
                ElementToString(value, taskOut.m_Doc);
            }
            else if (key == "mode")
            {
                std::string modeString;
                if (!ElementToString(value, modeString))
                {
                    errorMessage = "task field 'mode' must be string";
                    return false;
                }

                taskOut.m_Mode = StringToTaskMode(modeString);
            }
            else if (key == "depends_on")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "task field 'depends_on' must be array of strings";
                    return false;
                }

                simdjson::ondemand::array dependsArray = arrayResult.value();
                for (simdjson::ondemand::value dependencyValue : dependsArray)
                {
                    auto stringResult = dependencyValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "task field 'depends_on' must be array of strings";
                        return false;
                    }

                    std::string_view dependencyView = stringResult.value();
                    taskOut.m_DependsOn.emplace_back(dependencyView.begin(), dependencyView.end());
                }
            }
            else if (key == "file_inputs")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "task field 'file_inputs' must be array of strings";
                    return false;
                }

                simdjson::ondemand::array inputsArray = arrayResult.value();
                for (simdjson::ondemand::value inputValue : inputsArray)
                {
                    auto stringResult = inputValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "task field 'file_inputs' must be array of strings";
                        return false;
                    }

                    std::string_view inputView = stringResult.value();
                    taskOut.m_FileInputs.emplace_back(inputView.begin(), inputView.end());
                }
            }
            else if (key == "file_outputs")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "task field 'file_outputs' must be array of strings";
                    return false;
                }

                simdjson::ondemand::array outputsArray = arrayResult.value();
                for (simdjson::ondemand::value outputValue : outputsArray)
                {
                    auto stringResult = outputValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "task field 'file_outputs' must be array of strings";
                        return false;
                    }

                    std::string_view outputView = stringResult.value();
                    taskOut.m_FileOutputs.emplace_back(outputView.begin(), outputView.end());
                }
            }
            else if (key == "environment")
            {
                if (!ParseTaskEnvironment(value, taskOut.m_Environment, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "queue_binding")
            {
                if (!ParseTaskQueueBinding(value, taskOut.m_QueueBinding, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "inputs")
            {
                if (!ParseTaskInputs(value, taskOut.m_Inputs, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "outputs")
            {
                if (!ParseTaskOutputs(value, taskOut.m_Outputs, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "timeout_ms")
            {
                auto timeoutResult = value.get_int64();
                if (timeoutResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "task field 'timeout_ms' must be integer";
                    return false;
                }

                taskOut.m_TimeoutMs = static_cast<uint64_t>(timeoutResult.value());
            }
            else if (key == "retries")
            {
                auto objectResult = value.get_object();
                if (objectResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "task field 'retries' must be object";
                    return false;
                }

                simdjson::ondemand::object retriesObject = objectResult.value();
                if (!ParseRetries(retriesObject, taskOut.m_RetryPolicy, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "params")
            {
                if (!ExtractRawJson(value, taskOut.m_ParamsJson))
                {
                    errorMessage = "failed to read task 'params' JSON";
                    return false;
                }
            }
            else
            {
                LOG_CORE_WARN("Unknown field in task '{}': {}", taskOut.m_Id, key);
            }
        }

        if (taskOut.m_Type == TaskType::Unknown)
        {
            errorMessage = "task missing required field: type";
            return false;
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

    bool WorkflowJsonParser::ParseRetries(simdjson::ondemand::object& jsonObject, RetryPolicy& retryPolicyOut,
                                          std::string& errorMessage) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read retries key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "max_attempts")
            {
                auto maxAttemptsResult = value.get_int64();
                if (maxAttemptsResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "retries field 'max_attempts' must be integer";
                    return false;
                }

                retryPolicyOut.m_MaxAttempts = static_cast<uint32_t>(maxAttemptsResult.value());
            }
            else if (key == "backoff_ms")
            {
                auto backoffResult = value.get_int64();
                if (backoffResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "retries field 'backoff_ms' must be integer";
                    return false;
                }

                retryPolicyOut.m_BackoffMs = static_cast<uint32_t>(backoffResult.value());
            }
            else
            {
                LOG_CORE_WARN("Unknown field in retries: {}", key);
            }
        }

        return true;
    }

} // namespace AIAssistant
