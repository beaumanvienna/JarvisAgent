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
#include <limits>
#include <string_view>
#include <unordered_set>

#include "engine.h"
#include "workflow/workflowJsonParserDetails.h"
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
        using namespace WorkflowParserLimits;
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
            if (outFileRefs.size() >= kMaxQueueFilesPerSection)
            {
                outErrorMessage = context + " exceeds max queue-file count (" +
                                  std::to_string(kMaxQueueFilesPerSection) + ")";
                return false;
            }
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

                std::string const pathStr(pathText);
                if (!IsAcceptedRelativePath(pathStr))
                {
                    outErrorMessage = context + " path rejected (absolute, empty, "
                                      "or overlength): '" + pathStr + "'";
                    return false;
                }
                QueueFileRef fileRef{};
                fileRef.m_Path = pathStr;
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

                std::string const pathStr(pathText);
                if (!IsAcceptedRelativePath(pathStr))
                {
                    outErrorMessage = context + " path rejected (absolute, empty, "
                                      "or overlength): '" + pathStr + "'";
                    return false;
                }
                if (contentText.size() > kMaxInlineContentBytes)
                {
                    outErrorMessage = context + " inline 'content' size " +
                                      std::to_string(contentText.size()) +
                                      " exceeds cap " + std::to_string(kMaxInlineContentBytes);
                    return false;
                }
                QueueFileRef fileRef{};
                fileRef.m_Path = pathStr;
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
    std::expected<void, ParserError> ParseTaskQueueBinding(simdjson::ondemand::value& value, QueueBinding& binding)
    {
        binding = {};

        simdjson::ondemand::object obj;
        auto const objError = value.get_object().get(obj);
        if (objError)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch, "queue_binding must be an object"));
        }

        std::string innerErr;
        auto readArray = [&](char const* fieldName, std::vector<QueueFileRef>& destination) -> bool
        {
            simdjson::ondemand::value fieldValue;
            auto const fieldError = obj[fieldName].get(fieldValue);
            if (fieldError)
            {
                return true; // optional
            }

            return ReadQueueFileRefArray(fieldValue, destination, std::string("queue_binding.") + fieldName, innerErr);
        };

        auto bridge = [&]() -> std::expected<void, ParserError>
        {
            // ReadQueueFileRefArray rejections cover three categories: type
            // mismatch (array shape), out-of-range (queue-file count cap +
            // inline content cap), and missing-field (object missing path /
            // content).  The Details string distinguishes them — caller-side
            // grep ("must be an array" / "exceeds" / "is missing") if needed.
            // Pick the dominant category at this granularity.
            return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch, std::move(innerErr)));
        };

        if (!readArray("stng_files", binding.m_StngFiles))
        {
            return bridge();
        }

        if (!readArray("task_files", binding.m_TaskFiles))
        {
            return bridge();
        }

        if (!readArray("cntx_files", binding.m_CntxFiles))
        {
            return bridge();
        }

        if (!readArray("prob_files", binding.m_ProbFiles))
        {
            return bridge();
        }

        return {};
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

        if (rawType == "snowflake_query")
        {
            return TaskType::SnowflakeQuery;
        }

        if (rawType == "slack_message")
        {
            return TaskType::SlackMessage;
        }

        if (rawType == "slack_read")
        {
            return TaskType::SlackRead;
        }

        if (rawType == "email_send")
        {
            return TaskType::EmailSend;
        }

        if (rawType == "email_read")
        {
            return TaskType::EmailRead;
        }

        if (rawType == "github_issue")
        {
            return TaskType::GitHubIssue;
        }

        if (rawType == "jira_issue")
        {
            return TaskType::JiraIssue;
        }

        if (rawType == "redmine_issue")
        {
            return TaskType::RedmineIssue;
        }

        if (rawType == "sheets_read")
        {
            return TaskType::SheetsRead;
        }

        if (rawType == "sheets_write")
        {
            return TaskType::SheetsWrite;
        }

        if (rawType == "azure_blob_upload" || rawType == "azure_blob_download")
        {
            return TaskType::AzureBlob;
        }

        if (rawType == "gcs_upload" || rawType == "gcs_download")
        {
            return TaskType::Gcs;
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

        if (typeString == "email_watch")
        {
            return WorkflowTriggerType::EmailWatch;
        }

        if (typeString == "azure_blob_watch")
        {
            return WorkflowTriggerType::AzureBlobWatch;
        }

        if (typeString == "gcs_watch")
        {
            return WorkflowTriggerType::GcsWatch;
        }

        LOG_CORE_WARN("Unknown trigger type '{}', defaulting to Unknown", typeString);
        return WorkflowTriggerType::Unknown;
    }
    std::expected<void, ParserError>
        WorkflowJsonParser::ParseTrigger(simdjson::ondemand::object& jsonObject, WorkflowTrigger& triggerOut) const
    {
        bool hasType = false;
        bool hasId = false;

        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read trigger field key: ") + simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "type")
            {
                std::string typeString;
                if (!ElementToString(value, typeString))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "trigger field 'type' must be string"));
                }

                triggerOut.m_Type = StringToTriggerType(typeString);
                hasType = true;
            }
            else if (key == "id")
            {
                if (!ElementToString(value, triggerOut.m_Id))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "trigger field 'id' must be string"));
                }

                hasId = true;
            }
            else if (key == "enabled")
            {
                auto boolResult = value.get_bool();
                if (boolResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "trigger field 'enabled' must be bool"));
                }

                triggerOut.m_IsEnabled = boolResult.value();
            }
            else if (key == "params")
            {
                if (!ExtractRawJson(value, triggerOut.m_ParamsJson))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::SimdjsonError, "failed to read trigger 'params' JSON"));
                }
            }
            else
            {
                LOG_CORE_WARN("Unknown field in trigger '{}': {}", triggerOut.m_Id, key);
            }
        }

        if (!hasType)
        {
            return std::unexpected(ParserError::Make(
                ParserErrorCode::MissingField, "trigger missing required field: type"));
        }

        if (!hasId)
        {
            return std::unexpected(ParserError::Make(
                ParserErrorCode::MissingField, "trigger missing required field: id"));
        }

        return {};
    }
    std::expected<void, ParserError>
        WorkflowJsonParser::ParseTask(simdjson::ondemand::object& jsonObject, TaskDef& taskOut) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read task field key: ") + simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "id")
            {
                if (!ElementToString(value, taskOut.m_Id))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'id' must be string"));
                }
            }
            else if (key == "type")
            {
                std::string typeString;
                if (!ElementToString(value, typeString))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'type' must be string"));
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
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'working_directory' must be a string"));
                }
                // Empty string is the documented "use queue folder" default;
                // anything non-empty must pass the parse-time syntactic gate.
                if (!taskOut.m_WorkingDirectory.empty() &&
                    !IsAcceptedRelativePath(taskOut.m_WorkingDirectory))
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "task field 'working_directory' rejected (absolute, '..' "
                        "segment, empty, or overlength): '" + taskOut.m_WorkingDirectory + "'"));
                }
            }
            else if (key == "mode")
            {
                std::string modeString;
                if (!ElementToString(value, modeString))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'mode' must be string"));
                }

                taskOut.m_Mode = StringToTaskMode(modeString);
            }
            else if (key == "depends_on")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'depends_on' must be array of strings"));
                }

                simdjson::ondemand::array dependsArray = arrayResult.value();
                for (simdjson::ondemand::value dependencyValue : dependsArray)
                {
                    if (taskOut.m_DependsOn.size() >= WorkflowParserLimits::kMaxDependsOnPerTask)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                            "task field 'depends_on' exceeds max count (" +
                                std::to_string(WorkflowParserLimits::kMaxDependsOnPerTask) + ")"));
                    }
                    auto stringResult = dependencyValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task field 'depends_on' must be array of strings"));
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
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'expose_error_signal' must be boolean"));
                }

                taskOut.m_ExposeErrorSignal = boolResult.value();
            }
            else if (key == "workflow_file")
            {
                if (!ElementToString(value, taskOut.m_WorkflowFile))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'workflow_file' must be a string"));
                }
            }
            else if (key == "output_schema")
            {
                if (!ExtractRawJson(value, taskOut.m_OutputSchemaJson))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::SimdjsonError, "task field 'output_schema' must be a JSON value"));
                }
            }
            else if (key == "output_retries")
            {
                uint64_t retriesValue = 0;
                auto const retriesResult = value.get_uint64().get(retriesValue);
                if (retriesResult != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                        "task field 'output_retries' must be a non-negative integer"));
                }
                if (retriesValue > std::numeric_limits<std::uint32_t>::max())
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "task field 'output_retries' exceeds uint32 range: " + std::to_string(retriesValue)));
                }
                taskOut.m_OutputSchemaMaxAttempts = static_cast<uint32_t>(retriesValue);
            }
            else if (key == "file_inputs")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'file_inputs' must be array of strings"));
                }

                simdjson::ondemand::array inputsArray = arrayResult.value();
                for (simdjson::ondemand::value inputValue : inputsArray)
                {
                    if (taskOut.m_FileInputs.size() >= WorkflowParserLimits::kMaxFileInputsPerTask)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                            "task field 'file_inputs' exceeds max count (" +
                                std::to_string(WorkflowParserLimits::kMaxFileInputsPerTask) + ")"));
                    }
                    auto stringResult = inputValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task field 'file_inputs' must be array of strings"));
                    }

                    std::string_view inputView = stringResult.value();
                    std::string const inputPath(inputView);
                    if (!IsAcceptedRelativePath(inputPath))
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                            "task 'file_inputs' path rejected (absolute, "
                            "empty, or overlength): '" + inputPath + "'"));
                    }
                    taskOut.m_FileInputs.push_back(inputPath);
                }
            }
            else if (key == "file_outputs")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'file_outputs' must be array of strings"));
                }

                simdjson::ondemand::array outputsArray = arrayResult.value();
                for (simdjson::ondemand::value outputValue : outputsArray)
                {
                    if (taskOut.m_FileOutputs.size() >= WorkflowParserLimits::kMaxFileOutputsPerTask)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                            "task field 'file_outputs' exceeds max count (" +
                                std::to_string(WorkflowParserLimits::kMaxFileOutputsPerTask) + ")"));
                    }
                    auto stringResult = outputValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task field 'file_outputs' must be array of strings"));
                    }

                    std::string_view outputView = stringResult.value();
                    std::string const outputPath(outputView);
                    if (!IsAcceptedRelativePath(outputPath))
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                            "task 'file_outputs' path rejected (absolute, "
                            "empty, or overlength): '" + outputPath + "'"));
                    }
                    taskOut.m_FileOutputs.push_back(outputPath);
                }
            }
            else if (key == "materialize")
            {
                auto objectResult = value.get_object();
                if (objectResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'materialize' must be an object"));
                }

                simdjson::ondemand::object materializeObj = objectResult.value();
                for (auto materializeField : materializeObj)
                {
                    auto fieldKeyResult = materializeField.unescaped_key(false);
                    if (fieldKeyResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::SimdjsonError, "task field 'materialize' has invalid key"));
                    }
                    std::string_view fieldKey = fieldKeyResult.value();

                    auto fieldValueResult = materializeField.value().get_string(false);
                    if (fieldValueResult.error() != simdjson::SUCCESS)
                    {
                        return std::unexpected(ParserError::Make(
                            ParserErrorCode::TypeMismatch, "task field 'materialize' values must be strings"));
                    }
                    std::string_view fieldValue = fieldValueResult.value();

                    taskOut.m_Materialize.emplace_back(std::string(fieldKey.begin(), fieldKey.end()),
                                                       std::string(fieldValue.begin(), fieldValue.end()));
                }
            }
            else if (key == "environment")
            {
                if (auto r = ParseTaskEnvironment(value, taskOut.m_Environment); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "queue_binding")
            {
                if (auto r = ParseTaskQueueBinding(value, taskOut.m_QueueBinding); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "inputs")
            {
                if (auto r = ParseTaskInputs(value, taskOut.m_Inputs); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "outputs")
            {
                if (auto r = ParseTaskOutputs(value, taskOut.m_Outputs); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "timeout_ms")
            {
                auto timeoutResult = value.get_int64();
                if (timeoutResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'timeout_ms' must be integer"));
                }
                int64_t const timeoutSigned = timeoutResult.value();
                if (timeoutSigned < 0)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "task field 'timeout_ms' must be non-negative, got " + std::to_string(timeoutSigned)));
                }
                if (static_cast<uint64_t>(timeoutSigned) > WorkflowParserLimits::kMaxTimeoutMs)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "task field 'timeout_ms' exceeds 7-day cap: " + std::to_string(timeoutSigned)));
                }
                taskOut.m_TimeoutMs = static_cast<uint64_t>(timeoutSigned);
            }
            else if (key == "retries")
            {
                auto objectResult = value.get_object();
                if (objectResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'retries' must be object"));
                }

                simdjson::ondemand::object retriesObject = objectResult.value();
                if (auto r = ParseRetries(retriesObject, taskOut.m_RetryPolicy); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "params")
            {
                if (!ExtractRawJson(value, taskOut.m_ParamsJson))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::SimdjsonError, "failed to read task 'params' JSON"));
                }
            }
            else if (key == "filter")
            {
                if (!ElementToString(value, taskOut.m_Filter))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "task field 'filter' must be string"));
                }
            }
            else
            {
                LOG_CORE_WARN("Unknown field in task '{}': {}", taskOut.m_Id, key);
            }
        }

        // Note: m_Id MAY be empty here — ParseTasks (the only caller) falls
        // back to the JSON map key when no explicit "id" is provided.  The
        // empty-id-equals-map-key contract lives in ParseTasks; do not add
        // a strict check here without updating that fallback in lockstep.
        if (taskOut.m_Type == TaskType::Unknown)
        {
            return std::unexpected(ParserError::Make(
                ParserErrorCode::MissingField, "task missing required field: type"));
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

        return {};
    }
    std::expected<void, ParserError>
        WorkflowJsonParser::ParseRetries(simdjson::ondemand::object& jsonObject, RetryPolicy& retryPolicyOut) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read retries key: ") + simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "max_attempts")
            {
                auto maxAttemptsResult = value.get_int64();
                if (maxAttemptsResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "retries field 'max_attempts' must be integer"));
                }
                int64_t const maxAttempts = maxAttemptsResult.value();
                if (maxAttempts < 0)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "retries field 'max_attempts' must be non-negative, got " + std::to_string(maxAttempts)));
                }
                if (static_cast<uint64_t>(maxAttempts) > WorkflowParserLimits::kMaxRetryAttempts)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "retries field 'max_attempts' exceeds cap " +
                            std::to_string(WorkflowParserLimits::kMaxRetryAttempts) + ": " +
                            std::to_string(maxAttempts)));
                }
                retryPolicyOut.m_MaxAttempts = static_cast<uint32_t>(maxAttempts);
            }
            else if (key == "backoff_ms")
            {
                auto backoffResult = value.get_int64();
                if (backoffResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "retries field 'backoff_ms' must be integer"));
                }
                int64_t const backoff = backoffResult.value();
                if (backoff < 0)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "retries field 'backoff_ms' must be non-negative, got " + std::to_string(backoff)));
                }
                if (static_cast<uint64_t>(backoff) > WorkflowParserLimits::kMaxBackoffMs)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "retries field 'backoff_ms' exceeds cap " +
                            std::to_string(WorkflowParserLimits::kMaxBackoffMs) + ": " + std::to_string(backoff)));
                }
                retryPolicyOut.m_BackoffMs = static_cast<uint32_t>(backoff);
            }
            else
            {
                LOG_CORE_WARN("Unknown field in retries: {}", key);
            }
        }

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseDefaults(simdjson::ondemand::object& jsonObject, WorkflowDefaults& defaultsOut) const
    {
        for (auto field : jsonObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read defaults key: ") + simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "timeout_ms")
            {
                auto timeoutResult = value.get_int64();
                if (timeoutResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "defaults field 'timeout_ms' must be integer"));
                }
                int64_t const timeoutSigned = timeoutResult.value();
                if (timeoutSigned < 0)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "defaults field 'timeout_ms' must be non-negative, got " + std::to_string(timeoutSigned)));
                }
                if (static_cast<uint64_t>(timeoutSigned) > WorkflowParserLimits::kMaxTimeoutMs)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "defaults field 'timeout_ms' exceeds 7-day cap: " + std::to_string(timeoutSigned)));
                }
                defaultsOut.m_TimeoutMs = static_cast<uint64_t>(timeoutSigned);
            }
            else if (key == "retries")
            {
                auto objectResult = value.get_object();
                if (objectResult.error() != simdjson::SUCCESS)
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "defaults field 'retries' must be object"));
                }

                simdjson::ondemand::object retriesObject = objectResult.value();
                if (auto r = ParseRetries(retriesObject, defaultsOut.m_RetryPolicy); !r)
                {
                    return std::unexpected(std::move(r.error()));
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

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseWorkflowJson(std::string const& jsonContent,
                                              WorkflowDefinition& outputDefinition) const
    {
        if (jsonContent.empty())
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::MissingField, "Workflow JSON content is empty"));
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(jsonContent);
        simdjson::ondemand::document document;

        simdjson::error_code errorCode = parser.iterate(paddedJson).get(document);

        if (errorCode)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("Failed to parse workflow JSON: ") + simdjson::error_message(errorCode)));
        }

        simdjson::ondemand::object rootObject;
        errorCode = document.get_object().get(rootObject);
        if (errorCode)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                std::string("Workflow JSON root must be an object, got: ") + simdjson::error_message(errorCode)));
        }
        return ParseRootObject(rootObject, outputDefinition);
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseRootObject(simdjson::ondemand::object& root,
                                            WorkflowDefinition& outputDefinition) const
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
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("failed to read root key: ") + simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());

            simdjson::ondemand::value value = field.value();

            if (key == "version")
            {
                std::string version;
                if (!ElementToString(value, version))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "field 'version' must be string"));
                }

                // Parse "major.minor" and enforce version gate
                {
                    constexpr int KNOWN_MAJOR = 1;
                    constexpr int KNOWN_MINOR = 1;

                    auto const dotPos = version.find('.');
                    if (dotPos == std::string::npos || dotPos == 0 || dotPos == version.size() - 1)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                            "malformed JCWF version (expected 'major.minor'): " + version));
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
                        return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                            "malformed JCWF version (non-numeric): " + version));
                    }

                    if (major != KNOWN_MAJOR)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                            "unsupported JCWF major version " + std::to_string(major) +
                                " (supported: " + std::to_string(KNOWN_MAJOR) + "): " + version));
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
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "field 'id' must be string"));
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
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::SimdjsonError, "failed to read 'doc' JSON"));
                }
            }
            else if (key == "base_directory")
            {
                if (!ElementToString(value, outputDefinition.m_WorkflowBaseDirectory))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "field 'base_directory' must be a string"));
                }
                if (!outputDefinition.m_WorkflowBaseDirectory.empty() &&
                    !IsAcceptedRelativePath(outputDefinition.m_WorkflowBaseDirectory))
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "field 'base_directory' rejected (absolute, "
                        "empty, or overlength): '" + outputDefinition.m_WorkflowBaseDirectory + "'"));
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
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "field 'manual_start' must be a boolean"));
                }
            }
            else if (key == "triggers")
            {
                if (auto r = ParseTriggers(value, outputDefinition.m_Triggers); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }

                hasTriggersField = true;
            }
            else if (key == "tasks")
            {
                if (auto r = ParseTasks(value, outputDefinition.m_Tasks); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }

                hasTasks = true;
            }
            else if (key == "dataflow")
            {
                if (auto r = ParseDataflow(value, outputDefinition.m_Dataflows); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "filters")
            {
                if (auto r = ParseFilters(value, outputDefinition.m_Filters); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "control_nodes")
            {
                if (auto r = ParseControlNodes(value, outputDefinition.m_ControlNodes); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "controlflow")
            {
                if (auto r = ParseControlflow(value, outputDefinition.m_ControlflowEdges); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "defaults")
            {
                if (!ExtractRawJson(value, outputDefinition.m_DefaultsJson))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::SimdjsonError, "failed to read 'defaults' JSON"));
                }

                // Re-parse the raw JSON into the typed struct (simdjson is single-pass).
                {
                    simdjson::ondemand::parser defaultsParser;
                    simdjson::padded_string paddedDefaults(outputDefinition.m_DefaultsJson);
                    simdjson::ondemand::document defaultsDoc;
                    if (auto ec = defaultsParser.iterate(paddedDefaults).get(defaultsDoc); ec)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                            std::string("failed to re-parse 'defaults' JSON: ") + simdjson::error_message(ec)));
                    }

                    simdjson::ondemand::object defaultsObj;
                    if (auto ec = defaultsDoc.get_object().get(defaultsObj); ec)
                    {
                        return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                            std::string("'defaults' value must be an object: ") + simdjson::error_message(ec)));
                    }
                    if (auto r = ParseDefaults(defaultsObj, outputDefinition.m_Defaults); !r)
                    {
                        return std::unexpected(std::move(r.error()));
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
            return std::unexpected(ParserError::Make(
                ParserErrorCode::MissingField, "workflow missing required field: version"));
        }

        if (!hasId)
        {
            return std::unexpected(ParserError::Make(
                ParserErrorCode::MissingField, "workflow missing required field: id"));
        }

        if (!hasTasks)
        {
            return std::unexpected(ParserError::Make(
                ParserErrorCode::MissingField, "workflow missing required field: tasks"));
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
                    return std::unexpected(ParserError::Make(ParserErrorCode::MissingField,
                        "task '" + taskId + "' references unknown filter '" + task.m_Filter + "'"));
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

        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseGlobalJson(std::string const& jsonContent, WorkflowDefinition& workflowOut) const
    {
        if (jsonContent.empty())
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::MissingField, "global.json content is empty"));
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(jsonContent);
        simdjson::ondemand::document document;

        simdjson::error_code ec = parser.iterate(paddedJson).get(document);
        if (ec)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("Failed to parse global.json: ") + simdjson::error_message(ec)));
        }

        simdjson::ondemand::object root;
        ec = document.get_object().get(root);
        if (ec)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                std::string("global.json root must be an object, got: ") + simdjson::error_message(ec)));
        }

        for (auto field : root)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("global.json: failed to read field key: ") +
                        simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());
            simdjson::ondemand::value value = field.value();

            if (key == "version")
            {
                if (!ElementToString(value, workflowOut.m_Version))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "global.json field 'version' must be string"));
                }
            }
            else if (key == "id")
            {
                if (!ElementToString(value, workflowOut.m_Id))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "global.json field 'id' must be string"));
                }
            }
            else if (key == "label")
            {
                if (!ElementToString(value, workflowOut.m_Label))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "global.json field 'label' must be string"));
                }
            }
            else if (key == "doc")
            {
                if (!ExtractRawJson(value, workflowOut.m_Doc))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::SimdjsonError, "global.json field 'doc' must be a JSON value"));
                }
            }
            else if (key == "base_directory")
            {
                if (!ElementToString(value, workflowOut.m_WorkflowBaseDirectory))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "global.json field 'base_directory' must be string"));
                }
                if (!workflowOut.m_WorkflowBaseDirectory.empty() &&
                    !IsAcceptedRelativePath(workflowOut.m_WorkflowBaseDirectory))
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "global.json field 'base_directory' rejected (absolute, '..' "
                        "segment, empty, or overlength): '" + workflowOut.m_WorkflowBaseDirectory + "'"));
                }
            }
            else if (key == "manual_start")
            {
                if (value.type() == simdjson::ondemand::json_type::boolean)
                {
                    workflowOut.m_ManualStart = value.get_bool().value();
                }
                else
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::TypeMismatch, "global.json field 'manual_start' must be boolean"));
                }
            }
            else if (key == "concurrency")
            {
                std::string concurrencyStr;
                if (!ElementToString(value, concurrencyStr))
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                        "global.json field 'concurrency' must be a string "
                        "('serialize', 'parallel', or 'reject')"));
                }
                if (concurrencyStr == "serialize")
                {
                    workflowOut.m_ConcurrencyPolicy = WorkflowConcurrencyPolicy::Serialize;
                }
                else if (concurrencyStr == "parallel")
                {
                    workflowOut.m_ConcurrencyPolicy = WorkflowConcurrencyPolicy::Parallel;
                }
                else if (concurrencyStr == "reject")
                {
                    workflowOut.m_ConcurrencyPolicy = WorkflowConcurrencyPolicy::Reject;
                }
                else
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::ValueOutOfRange,
                        "global.json field 'concurrency' has unknown value '" + concurrencyStr +
                            "' (expected 'serialize', 'parallel', or 'reject')"));
                }
            }
            else if (key == "triggers")
            {
                if (auto r = ParseTriggers(value, workflowOut.m_Triggers); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "defaults")
            {
                if (!ExtractRawJson(value, workflowOut.m_DefaultsJson))
                {
                    return std::unexpected(ParserError::Make(
                        ParserErrorCode::SimdjsonError, "global.json field 'defaults' must be a JSON value"));
                }
                simdjson::ondemand::parser defaultsParser;
                simdjson::padded_string paddedDefaults(workflowOut.m_DefaultsJson);
                simdjson::ondemand::document defaultsDoc;
                if (auto defaultsParseEc = defaultsParser.iterate(paddedDefaults).get(defaultsDoc); defaultsParseEc)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                        std::string("global.json: failed to re-parse 'defaults' JSON: ") +
                            simdjson::error_message(defaultsParseEc)));
                }
                simdjson::ondemand::object defaultsObj;
                if (auto defaultsObjEc = defaultsDoc.get_object().get(defaultsObj); defaultsObjEc)
                {
                    return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                        std::string("global.json: 'defaults' must be an object: ") +
                            simdjson::error_message(defaultsObjEc)));
                }
                if (auto r = ParseDefaults(defaultsObj, workflowOut.m_Defaults); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            // Silently ignore tasks, dataflow, etc. — those belong in canvas JSONs.
        }

        LOG_APP_INFO("[JcwfContainer] parsed global.json: id='{}' version='{}' label='{}'", workflowOut.m_Id,
                     workflowOut.m_Version, workflowOut.m_Label);
        return {};
    }

    std::expected<void, ParserError>
        WorkflowJsonParser::ParseCanvasJson(std::string const& jsonContent, WorkflowDefinition& workflowOut) const
    {
        if (jsonContent.empty())
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::MissingField, "Canvas JSON content is empty"));
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(jsonContent);
        simdjson::ondemand::document document;

        simdjson::error_code ec = parser.iterate(paddedJson).get(document);
        if (ec)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                std::string("Failed to parse canvas JSON: ") + simdjson::error_message(ec)));
        }

        simdjson::ondemand::object root;
        ec = document.get_object().get(root);
        if (ec)
        {
            return std::unexpected(ParserError::Make(ParserErrorCode::TypeMismatch,
                std::string("Canvas JSON root must be an object, got: ") + simdjson::error_message(ec)));
        }

        bool hasTasks = false;

        for (auto field : root)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                return std::unexpected(ParserError::Make(ParserErrorCode::SimdjsonError,
                    std::string("canvas JSON: failed to read field key: ") +
                        simdjson::error_message(keyResult.error())));
            }

            std::string_view keyView = keyResult.value();
            std::string key(keyView.begin(), keyView.end());
            simdjson::ondemand::value value = field.value();

            if (key == "tasks")
            {
                if (auto r = ParseTasks(value, workflowOut.m_Tasks); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
                hasTasks = true;
            }
            else if (key == "dataflow")
            {
                if (auto r = ParseDataflow(value, workflowOut.m_Dataflows); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "filters")
            {
                if (auto r = ParseFilters(value, workflowOut.m_Filters); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "control_nodes")
            {
                if (auto r = ParseControlNodes(value, workflowOut.m_ControlNodes); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            else if (key == "controlflow")
            {
                if (auto r = ParseControlflow(value, workflowOut.m_ControlflowEdges); !r)
                {
                    return std::unexpected(std::move(r.error()));
                }
            }
            // Silently ignore metadata fields (version, id, label, triggers, etc.)
            // — those come from global.json for container workflows.
        }

        if (!hasTasks)
        {
            return std::unexpected(ParserError::Make(
                ParserErrorCode::MissingField, "canvas JSON missing required field: tasks"));
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
                    return std::unexpected(ParserError::Make(ParserErrorCode::MissingField,
                        "task '" + taskId + "' references unknown filter '" + task.m_Filter + "'"));
                }
            }
        }

        return {};
    }

} // namespace AIAssistant
