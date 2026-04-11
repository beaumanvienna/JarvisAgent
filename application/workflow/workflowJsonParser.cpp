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

/*
Expected JCWF JSON structure:

{
  "version": "1.0",
  "id": "daily-report",
  "label": "Daily Reporting Workflow",
  "doc": "Generates a daily report from XLS and sends it to an AI assistant.",
  "triggers": [
    {
      "type": "auto | cron | file_watch | structure | manual",
      "id": "trigger-id",
      "enabled": true,
      "params": { ... }
    }
  ],
  "tasks": {
    "taskId": {
      "id": "taskId",
      "type": "python | shell | ai_call | internal",
      "label": "Summarize report with AI",
      "doc": "Task documentation...",
      "mode": "single | per_item",
      "depends_on": ["otherTaskId"],
      "file_inputs": ["input1.ext"],
      "file_outputs": ["output1.ext"],
      "environment": {
        "name": "assistant_env",
        "assistant_id": "assistant-123",
        "variables": {
          "PROJECT": "DailyReports"
        }
      },
      "queue_binding": {
        "stng_files": ["STNG_daily.txt"],
        "task_files": ["TASK_summarize.txt"],
        "cntx_files": ["CNTX_daily.txt"]
      },
      "inputs": {
        "source_path": { "type": "string", "required": true }
      },
      "outputs": {
        "markdown_path": { "type": "string" }
      },
      "timeout_ms": 600000,
      "retries": {
        "max_attempts": 3,
        "backoff_ms": 1000
      },
      "params": {
        "provider": "openai",
        "model": "gpt-4.1-mini",
        "mode": "one_shot | assistant",
        "prompt_template": "..."
      }
    }
  },
  "dataflow": [
    {
      "from_task": "load_xls",
      "from_output": "rows",
      "to_task": "summarize_section",
      "to_input": "section_text",
      "mapping": {
        "use_field": "A"
      }
    }
  ],
  "defaults": {
    "timeout_ms": 600000,
    "retries": {
      "max_attempts": 2,
      "backoff_ms": 1000
    },
    "ai": {
      "provider": "openai",
      "model": "gpt-4.1-mini"
    }
  }
}
*/

#include "workflow/workflowJsonParser.h"
#include <filesystem>
#include <string_view>
#include <unordered_set>

#include "engine.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{

    static bool IsRelativePathString(std::string const& pathText)
    {
        if (pathText.empty())
        {
            return false;
        }

        return std::filesystem::path(pathText).is_relative();
    }

    // ---------------------------------------------------------
    // Implementation moved from workflowJsonParserDetails.cpp
    // (keeps TUs smaller / more balanced)
    // ---------------------------------------------------------

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

        if (rawType == "sub_workflow")
        {
            return TaskType::SubWorkflow;
        }

        if (rawType == "polarion_write")
        {
            return TaskType::PolarionWrite;
        }

        if (rawType == "s3")
        {
            return TaskType::S3;
        }

        if (rawType == "db_query")
        {
            return TaskType::DbQuery;
        }

        if (rawType == "onedrive_upload" || rawType == "onedrive_download")
        {
            return TaskType::OneDrive;
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

        if (typeString == "webhook")
        {
            return WorkflowTriggerType::Webhook;
        }

        if (typeString == "s3_watch")
        {
            return WorkflowTriggerType::S3Watch;
        }

        if (typeString == "onedrive_watch")
        {
            return WorkflowTriggerType::OneDriveWatch;
        }

        LOG_CORE_WARN("Unknown trigger type '{}', defaulting to Unknown", typeString);
        return WorkflowTriggerType::Unknown;
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
            else if (key == "working_directory")
            {
                if (!ElementToString(value, taskOut.m_WorkingDirectory))
                {
                    errorMessage = "task field 'working_directory' must be a string";
                    return false;
                }
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
            else if (key == "expose_error_signal")
            {
                auto boolResult = value.get_bool();
                if (boolResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "task field 'expose_error_signal' must be boolean";
                    return false;
                }

                taskOut.m_ExposeErrorSignal = boolResult.value();
            }
            else if (key == "workflow_file")
            {
                if (!ElementToString(value, taskOut.m_WorkflowFile))
                {
                    errorMessage = "task field 'workflow_file' must be a string";
                    return false;
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
            else if (key == "materialize")
            {
                auto objectResult = value.get_object();
                if (objectResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "task field 'materialize' must be an object";
                    return false;
                }

                simdjson::ondemand::object materializeObj = objectResult.value();
                for (auto materializeField : materializeObj)
                {
                    auto fieldKeyResult = materializeField.unescaped_key(false);
                    if (fieldKeyResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "task field 'materialize' has invalid key";
                        return false;
                    }
                    std::string_view fieldKey = fieldKeyResult.value();

                    auto fieldValueResult = materializeField.value().get_string(false);
                    if (fieldValueResult.error() != simdjson::SUCCESS)
                    {
                        errorMessage = "task field 'materialize' values must be strings";
                        return false;
                    }
                    std::string_view fieldValue = fieldValueResult.value();

                    taskOut.m_Materialize.emplace_back(std::string(fieldKey.begin(), fieldKey.end()),
                                                       std::string(fieldValue.begin(), fieldValue.end()));
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
            else if (key == "filter")
            {
                if (!ElementToString(value, taskOut.m_Filter))
                {
                    errorMessage = "task field 'filter' must be string";
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

        LOG_APP_INFO("[paths debug] debug reason=parseTaskPaths taskId={} taskType={} taskWorkingDirectoryRelative={} "
                     "taskWorkingDirectoryIsRelative={} fileInputsCount={} fileOutputsCount={} stngFilesCount={} "
                     "taskFilesCount={} cntxFilesCount={} probFilesCount={}",
                     taskOut.m_Id, static_cast<int>(taskOut.m_Type), taskOut.m_WorkingDirectory,
                     IsRelativePathString(taskOut.m_WorkingDirectory), taskOut.m_FileInputs.size(),
                     taskOut.m_FileOutputs.size(), taskOut.m_QueueBinding.m_StngFiles.size(),
                     taskOut.m_QueueBinding.m_TaskFiles.size(), taskOut.m_QueueBinding.m_CntxFiles.size(),
                     taskOut.m_QueueBinding.m_ProbFiles.size());

        for (std::string const& inputPath : taskOut.m_FileInputs)
        {
            LOG_APP_INFO(
                "[paths debug] debug reason=parseTaskFileInput taskId={} inputPathRelative={} inputPathIsRelative={}",
                taskOut.m_Id, inputPath, IsRelativePathString(inputPath));
        }

        for (std::string const& outputPath : taskOut.m_FileOutputs)
        {
            LOG_APP_INFO(
                "[paths debug] debug reason=parseTaskFileOutput taskId={} outputPathRelative={} outputPathIsRelative={}",
                taskOut.m_Id, outputPath, IsRelativePathString(outputPath));
        }

        auto logQueueFileRef = [&](char const* reason, QueueFileRef const& fileRef)
        {
            LOG_APP_INFO("[paths debug] debug reason={} taskId={} queueFilePathRelative={} queueFilePathIsRelative={} "
                         "queueFileHasInlineContent={}",
                         reason, taskOut.m_Id, fileRef.m_Path, IsRelativePathString(fileRef.m_Path),
                         fileRef.m_HasInlineContent);
        };

        for (QueueFileRef const& fileRef : taskOut.m_QueueBinding.m_StngFiles)
        {
            logQueueFileRef("parseTaskQueueBindingStngFile", fileRef);
        }

        for (QueueFileRef const& fileRef : taskOut.m_QueueBinding.m_TaskFiles)
        {
            logQueueFileRef("parseTaskQueueBindingTaskFile", fileRef);
        }

        for (QueueFileRef const& fileRef : taskOut.m_QueueBinding.m_CntxFiles)
        {
            logQueueFileRef("parseTaskQueueBindingCntxFile", fileRef);
        }

        for (QueueFileRef const& fileRef : taskOut.m_QueueBinding.m_ProbFiles)
        {
            logQueueFileRef("parseTaskQueueBindingProbFile", fileRef);
        }

        return true;
    }
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

    bool WorkflowJsonParser::ParseDefaults(simdjson::ondemand::object& jsonObject, WorkflowDefaults& defaultsOut,
                                           std::string& errorMessage) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read defaults key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "timeout_ms")
            {
                auto timeoutResult = value.get_int64();
                if (timeoutResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "defaults field 'timeout_ms' must be integer";
                    return false;
                }

                defaultsOut.m_TimeoutMs = static_cast<uint64_t>(timeoutResult.value());
            }
            else if (key == "retries")
            {
                auto objectResult = value.get_object();
                if (objectResult.error() != simdjson::SUCCESS)
                {
                    errorMessage = "defaults field 'retries' must be object";
                    return false;
                }

                simdjson::ondemand::object retriesObject = objectResult.value();
                if (!ParseRetries(retriesObject, defaultsOut.m_RetryPolicy, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "ai")
            {
                // AI defaults are resolved at dispatch time (see aiCallTaskExecutor); skip here.
            }
            else
            {
                LOG_CORE_WARN("Unknown field in defaults: {}", key);
            }
        }

        return true;
    }

    bool WorkflowJsonParser::ParseWorkflowJson(std::string const& jsonContent, WorkflowDefinition& outputDefinition,
                                               std::string& errorMessage) const
    {
        if (jsonContent.empty())
        {
            errorMessage = "Workflow JSON content is empty";
            return false;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(jsonContent);
        simdjson::ondemand::document document;

        simdjson::error_code errorCode = parser.iterate(paddedJson).get(document);

        if (errorCode)
        {
            errorMessage = "Failed to parse workflow JSON: ";
            errorMessage += simdjson::error_message(errorCode);
            return false;
        }

        simdjson::ondemand::object rootObject;
        errorCode = document.get_object().get(rootObject);
        if (errorCode)
        {
            errorMessage = "Workflow JSON root must be an object, got: ";
            errorMessage += simdjson::error_message(errorCode);
            return false;
        }
        return ParseRootObject(rootObject, outputDefinition, errorMessage);
    }

    bool WorkflowJsonParser::ParseRootObject(simdjson::ondemand::object root, WorkflowDefinition& outputDefinition,
                                             std::string& errorMessage) const
    {
        bool hasVersion = false;
        bool hasId = false;
        bool hasTasks = false;
        bool hasTriggersField = false;

        for (auto field : root)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                errorMessage = "failed to read root key: ";
                errorMessage += simdjson::error_message(keyResult.error());
                return false;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "version")
            {
                std::string version;
                if (!ElementToString(value, version))
                {
                    errorMessage = "field 'version' must be string";
                    return false;
                }

                // Parse "major.minor" and enforce version gate
                {
                    constexpr int KNOWN_MAJOR = 1;
                    constexpr int KNOWN_MINOR = 1;

                    auto const dotPos = version.find('.');
                    if (dotPos == std::string::npos || dotPos == 0 || dotPos == version.size() - 1)
                    {
                        errorMessage = "malformed JCWF version (expected 'major.minor'): " + version;
                        return false;
                    }

                    int major = 0;
                    int minor = 0;
                    try
                    {
                        major = std::stoi(version.substr(0, dotPos));
                        minor = std::stoi(version.substr(dotPos + 1));
                    }
                    catch (...)
                    {
                        errorMessage = "malformed JCWF version (non-numeric): " + version;
                        return false;
                    }

                    if (major != KNOWN_MAJOR)
                    {
                        errorMessage = "unsupported JCWF major version " + std::to_string(major) +
                                       " (supported: " + std::to_string(KNOWN_MAJOR) + "): " + version;
                        return false;
                    }

                    if (minor > KNOWN_MINOR)
                    {
                        LOG_CORE_WARN("JCWF version {} has minor version newer than known ({}). "
                                      "Some features may be unsupported.",
                                      version, std::to_string(KNOWN_MAJOR) + "." + std::to_string(KNOWN_MINOR));
                    }
                }

                outputDefinition.m_Version = version;
                hasVersion = true;
            }
            else if (key == "id")
            {
                if (!ElementToString(value, outputDefinition.m_Id))
                {
                    errorMessage = "field 'id' must be string";
                    return false;
                }

                hasId = true;
            }
            else if (key == "label")
            {
                ElementToString(value, outputDefinition.m_Label);
            }
            else if (key == "doc")
            {
                if (!ExtractRawJson(value, outputDefinition.m_Doc))
                {
                    errorMessage = "failed to read 'doc' JSON";
                    return false;
                }
            }
            else if (key == "base_directory")
            {
                if (!ElementToString(value, outputDefinition.m_WorkflowBaseDirectory))
                {
                    errorMessage = "field 'base_directory' must be a string";
                    return false;
                }
            }
            else if (key == "manual_start")
            {
                if (value.type() == simdjson::ondemand::json_type::boolean)
                {
                    outputDefinition.m_ManualStart = value.get_bool().value();
                }
                else
                {
                    errorMessage = "field 'manual_start' must be a boolean";
                    return false;
                }
            }
            else if (key == "triggers")
            {
                if (!ParseTriggers(value, outputDefinition.m_Triggers, errorMessage))
                {
                    return false;
                }

                hasTriggersField = true;
            }
            else if (key == "tasks")
            {
                if (!ParseTasks(value, outputDefinition.m_Tasks, errorMessage))
                {
                    return false;
                }

                hasTasks = true;
            }
            else if (key == "dataflow")
            {
                if (!ParseDataflow(value, outputDefinition.m_Dataflows, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "filters")
            {
                if (!ParseFilters(value, outputDefinition.m_Filters, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "control_nodes")
            {
                if (!ParseControlNodes(value, outputDefinition.m_ControlNodes, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "controlflow")
            {
                if (!ParseControlflow(value, outputDefinition.m_ControlflowEdges, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "defaults")
            {
                if (!ExtractRawJson(value, outputDefinition.m_DefaultsJson))
                {
                    errorMessage = "failed to read 'defaults' JSON";
                    return false;
                }

                // Re-parse the raw JSON into the typed struct (simdjson is single-pass).
                {
                    simdjson::ondemand::parser defaultsParser;
                    simdjson::padded_string paddedDefaults(outputDefinition.m_DefaultsJson);
                    simdjson::ondemand::document defaultsDoc;
                    simdjson::error_code ec = defaultsParser.iterate(paddedDefaults).get(defaultsDoc);
                    if (ec)
                    {
                        errorMessage = "failed to re-parse 'defaults' JSON: ";
                        errorMessage += simdjson::error_message(ec);
                        return false;
                    }

                    simdjson::ondemand::object defaultsObj = defaultsDoc.get_object();
                    if (!ParseDefaults(defaultsObj, outputDefinition.m_Defaults, errorMessage))
                    {
                        return false;
                    }
                }
            }
            else
            {
                LOG_CORE_WARN("Unknown field in root JCWF object: {}", key);
            }
        }

        if (!hasVersion)
        {
            errorMessage = "workflow missing required field: version";
            return false;
        }

        if (!hasId)
        {
            errorMessage = "workflow missing required field: id";
            return false;
        }

        if (!hasTasks)
        {
            errorMessage = "workflow missing required field: tasks";
            return false;
        }

        // If no trigger is provided (missing field or empty array), 'auto' is assumed as the default trigger.
        if (!hasTriggersField || outputDefinition.m_Triggers.empty())
        {
            WorkflowTrigger const autoTrigger{
                .m_Type = WorkflowTriggerType::Auto, //
                .m_Id = "auto",                      //
                .m_IsEnabled = true,                 //
                .m_ParamsJson = "{}"                 //
            };

            outputDefinition.m_Triggers.push_back(autoTrigger);
        }

        // Apply workflow-level defaults to tasks (task-level values take precedence).
        {
            WorkflowDefaults const& defaults = outputDefinition.m_Defaults;

            for (auto& [taskId, task] : outputDefinition.m_Tasks)
            {
                if (task.m_TimeoutMs == 0 && defaults.m_TimeoutMs != 0)
                {
                    task.m_TimeoutMs = defaults.m_TimeoutMs;
                }

                if (task.m_RetryPolicy.m_MaxAttempts == 0 && defaults.m_RetryPolicy.m_MaxAttempts != 0)
                {
                    task.m_RetryPolicy.m_MaxAttempts = defaults.m_RetryPolicy.m_MaxAttempts;
                }

                if (task.m_RetryPolicy.m_BackoffMs == 0 && defaults.m_RetryPolicy.m_BackoffMs != 0)
                {
                    task.m_RetryPolicy.m_BackoffMs = defaults.m_RetryPolicy.m_BackoffMs;
                }
            }
        }

        // Validate that every task's "filter" reference points to an existing filter ID.
        {
            std::unordered_set<std::string> filterIds;
            for (auto const& filter : outputDefinition.m_Filters)
            {
                filterIds.insert(filter.m_Id);
            }

            for (auto const& [taskId, task] : outputDefinition.m_Tasks)
            {
                if (!task.m_Filter.empty() && filterIds.find(task.m_Filter) == filterIds.end())
                {
                    errorMessage = "task '" + taskId + "' references unknown filter '" + task.m_Filter + "'";
                    return false;
                }
            }
        }

        LOG_APP_INFO("[paths debug] debug reason=parseWorkflowRoot workflowId={} workflowBaseDirectoryRelative={} "
                     "workflowBaseDirectoryIsRelative={} triggersCount={} tasksCount={} dataflowsCount={} "
                     "filtersCount={} controlNodesCount={} controlflowCount={}",
                     outputDefinition.m_Id, outputDefinition.m_WorkflowBaseDirectory,
                     IsRelativePathString(outputDefinition.m_WorkflowBaseDirectory), outputDefinition.m_Triggers.size(),
                     outputDefinition.m_Tasks.size(), outputDefinition.m_Dataflows.size(), outputDefinition.m_Filters.size(),
                     outputDefinition.m_ControlNodes.size(), outputDefinition.m_ControlflowEdges.size());

        return true;
    }

    bool WorkflowJsonParser::ParseGlobalJson(std::string const& jsonContent, WorkflowDefinition& workflowOut,
                                             std::string& errorMessage) const
    {
        if (jsonContent.empty())
        {
            errorMessage = "global.json content is empty";
            return false;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(jsonContent);
        simdjson::ondemand::document document;

        simdjson::error_code ec = parser.iterate(paddedJson).get(document);
        if (ec)
        {
            errorMessage = "Failed to parse global.json: ";
            errorMessage += simdjson::error_message(ec);
            return false;
        }

        simdjson::ondemand::object root;
        ec = document.get_object().get(root);
        if (ec)
        {
            errorMessage = "global.json root must be an object, got: ";
            errorMessage += simdjson::error_message(ec);
            return false;
        }

        for (auto field : root)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                continue;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());
            simdjson::ondemand::value value = field.value();

            if (key == "version")
            {
                ElementToString(value, workflowOut.m_Version);
            }
            else if (key == "id")
            {
                ElementToString(value, workflowOut.m_Id);
            }
            else if (key == "label")
            {
                ElementToString(value, workflowOut.m_Label);
            }
            else if (key == "doc")
            {
                ExtractRawJson(value, workflowOut.m_Doc);
            }
            else if (key == "base_directory")
            {
                ElementToString(value, workflowOut.m_WorkflowBaseDirectory);
            }
            else if (key == "manual_start")
            {
                if (value.type() == simdjson::ondemand::json_type::boolean)
                {
                    workflowOut.m_ManualStart = value.get_bool().value();
                }
            }
            else if (key == "triggers")
            {
                ParseTriggers(value, workflowOut.m_Triggers, errorMessage);
            }
            else if (key == "defaults")
            {
                ExtractRawJson(value, workflowOut.m_DefaultsJson);
                simdjson::ondemand::parser defaultsParser;
                simdjson::padded_string paddedDefaults(workflowOut.m_DefaultsJson);
                simdjson::ondemand::document defaultsDoc;
                simdjson::error_code defaultsEc = defaultsParser.iterate(paddedDefaults).get(defaultsDoc);
                if (!defaultsEc)
                {
                    simdjson::ondemand::object defaultsObj = defaultsDoc.get_object();
                    ParseDefaults(defaultsObj, workflowOut.m_Defaults, errorMessage);
                }
            }
            // Silently ignore tasks, dataflow, etc. — those belong in canvas JSONs.
        }

        LOG_APP_INFO("[JcwfContainer] parsed global.json: id='{}' version='{}' label='{}'", workflowOut.m_Id,
                     workflowOut.m_Version, workflowOut.m_Label);
        return true;
    }

    bool WorkflowJsonParser::ParseCanvasJson(std::string const& jsonContent, WorkflowDefinition& workflowOut,
                                             std::string& errorMessage) const
    {
        if (jsonContent.empty())
        {
            errorMessage = "Canvas JSON content is empty";
            return false;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(jsonContent);
        simdjson::ondemand::document document;

        simdjson::error_code ec = parser.iterate(paddedJson).get(document);
        if (ec)
        {
            errorMessage = "Failed to parse canvas JSON: ";
            errorMessage += simdjson::error_message(ec);
            return false;
        }

        simdjson::ondemand::object root;
        ec = document.get_object().get(root);
        if (ec)
        {
            errorMessage = "Canvas JSON root must be an object, got: ";
            errorMessage += simdjson::error_message(ec);
            return false;
        }

        bool hasTasks = false;

        for (auto field : root)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                continue;
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());
            simdjson::ondemand::value value = field.value();

            if (key == "tasks")
            {
                if (!ParseTasks(value, workflowOut.m_Tasks, errorMessage))
                {
                    return false;
                }
                hasTasks = true;
            }
            else if (key == "dataflow")
            {
                if (!ParseDataflow(value, workflowOut.m_Dataflows, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "filters")
            {
                if (!ParseFilters(value, workflowOut.m_Filters, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "control_nodes")
            {
                if (!ParseControlNodes(value, workflowOut.m_ControlNodes, errorMessage))
                {
                    return false;
                }
            }
            else if (key == "controlflow")
            {
                if (!ParseControlflow(value, workflowOut.m_ControlflowEdges, errorMessage))
                {
                    return false;
                }
            }
            // Silently ignore metadata fields (version, id, label, triggers, etc.)
            // — those come from global.json for container workflows.
        }

        if (!hasTasks)
        {
            errorMessage = "canvas JSON missing required field: tasks";
            return false;
        }

        // Apply defaults to tasks.
        {
            WorkflowDefaults const& defaults = workflowOut.m_Defaults;
            for (auto& [taskId, task] : workflowOut.m_Tasks)
            {
                if (task.m_TimeoutMs == 0 && defaults.m_TimeoutMs != 0)
                {
                    task.m_TimeoutMs = defaults.m_TimeoutMs;
                }
                if (task.m_RetryPolicy.m_MaxAttempts == 0 && defaults.m_RetryPolicy.m_MaxAttempts != 0)
                {
                    task.m_RetryPolicy.m_MaxAttempts = defaults.m_RetryPolicy.m_MaxAttempts;
                }
                if (task.m_RetryPolicy.m_BackoffMs == 0 && defaults.m_RetryPolicy.m_BackoffMs != 0)
                {
                    task.m_RetryPolicy.m_BackoffMs = defaults.m_RetryPolicy.m_BackoffMs;
                }
            }
        }

        // Validate filter references.
        {
            std::unordered_set<std::string> filterIds;
            for (auto const& filter : workflowOut.m_Filters)
            {
                filterIds.insert(filter.m_Id);
            }
            for (auto const& [taskId, task] : workflowOut.m_Tasks)
            {
                if (!task.m_Filter.empty() && filterIds.find(task.m_Filter) == filterIds.end())
                {
                    errorMessage = "task '" + taskId + "' references unknown filter '" + task.m_Filter + "'";
                    return false;
                }
            }
        }

        return true;
    }

} // namespace AIAssistant
