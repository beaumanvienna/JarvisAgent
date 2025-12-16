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

#include "engine.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
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

        simdjson::ondemand::object rootObject = document.get_object();
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

                if (version != "1.0")
                {
                    errorMessage = "unsupported JCWF version: ";
                    errorMessage += version;
                    return false;
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
            else if (key == "defaults")
            {
                if (!ExtractRawJson(value, outputDefinition.m_DefaultsJson))
                {
                    errorMessage = "failed to read 'defaults' JSON";
                    return false;
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

        return true;
    }

} // namespace AIAssistant
