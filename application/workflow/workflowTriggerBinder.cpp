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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#include "engine.h"

#include "workflowTriggerBinder.h"
#include "workflowRegistry.h"
#include "workflowTypes.h"
#include "triggerEngine.h"

#include <cctype>
#include <limits>
#include <string_view>
#include <vector>

#include "simdjson/simdjson.h"

namespace
{
    using namespace simdjson;

    std::string ToLowerAscii(std::string_view input)
    {
        std::string result;
        result.reserve(input.size());

        for (char character : input)
        {
            unsigned char const asUnsigned = static_cast<unsigned char>(character);
            result.push_back(static_cast<char>(std::tolower(asUnsigned)));
        }

        return result;
    }

    bool ParseCronParams(std::string const& paramsJson, std::string& outExpression)
    {
        outExpression.clear();

        if (paramsJson.empty())
        {
            LOG_APP_ERROR("ParseCronParams: params JSON is empty");
            return false;
        }

        ondemand::parser parser;
        padded_string json = padded_string(paramsJson.data(), paramsJson.size());

        ondemand::document document;
        simdjson::error_code errorCode = parser.iterate(json).get(document);
        if (errorCode != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("ParseCronParams: failed to parse params JSON: {}", simdjson::error_message(errorCode));
            return false;
        }

        auto objectResult = document.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("ParseCronParams: root of params JSON must be an object: {}",
                          simdjson::error_message(objectResult.error()));
            return false;
        }

        ondemand::object rootObject = objectResult.value();
        bool hasExpression = false;

        for (auto field : rootObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                LOG_APP_ERROR("ParseCronParams: failed to read field key: {}", simdjson::error_message(keyResult.error()));
                return false;
            }

            std::string_view keyView = keyResult.value();

            if (keyView == "expression")
            {
                ondemand::value value = field.value();

                auto stringResult = value.get_string(false);
                if (stringResult.error() != simdjson::SUCCESS)
                {
                    LOG_APP_ERROR("ParseCronParams: 'expression' must be a string: {}",
                                  simdjson::error_message(stringResult.error()));
                    return false;
                }

                std::string_view expressionView = stringResult.value();
                outExpression.assign(expressionView.begin(), expressionView.end());
                hasExpression = true;
                break;
            }
        }

        if (!hasExpression)
        {
            LOG_APP_ERROR("ParseCronParams: missing 'expression' field in params JSON");
            return false;
        }

        return true;
    }

    bool ParseFileWatchParams(std::string const& paramsJson, std::string& outPath,
                              std::vector<AIAssistant::TriggerEngine::FileEventType>& outEvents,
                              uint32_t& outDebounceMilliseconds)
    {
        using FileEventType = AIAssistant::TriggerEngine::FileEventType;

        outPath.clear();
        outEvents.clear();
        outDebounceMilliseconds = 0;

        if (paramsJson.empty())
        {
            LOG_APP_ERROR("ParseFileWatchParams: params JSON is empty");
            return false;
        }

        ondemand::parser parser;
        padded_string json = padded_string(paramsJson.data(), paramsJson.size());

        ondemand::document document;
        simdjson::error_code errorCode = parser.iterate(json).get(document);
        if (errorCode != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("ParseFileWatchParams: failed to parse params JSON: {}", simdjson::error_message(errorCode));
            return false;
        }

        auto objectResult = document.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("ParseFileWatchParams: root of params JSON must be an object: {}",
                          simdjson::error_message(objectResult.error()));
            return false;
        }

        ondemand::object rootObject = objectResult.value();

        bool hasPath = false;
        bool hasEvents = false;

        for (auto field : rootObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                LOG_APP_ERROR("ParseFileWatchParams: failed to read field key: {}",
                              simdjson::error_message(keyResult.error()));
                return false;
            }

            std::string_view keyView = keyResult.value();
            ondemand::value value = field.value();

            if (keyView == "path")
            {
                auto stringResult = value.get_string(false);
                if (stringResult.error() != simdjson::SUCCESS)
                {
                    LOG_APP_ERROR("ParseFileWatchParams: 'path' must be a string: {}",
                                  simdjson::error_message(stringResult.error()));
                    return false;
                }

                std::string_view pathView = stringResult.value();
                outPath.assign(pathView.begin(), pathView.end());
                hasPath = true;
            }
            else if (keyView == "events")
            {
                auto arrayResult = value.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    LOG_APP_ERROR("ParseFileWatchParams: 'events' must be an array: {}",
                                  simdjson::error_message(arrayResult.error()));
                    return false;
                }

                ondemand::array eventsArray = arrayResult.value();

                for (ondemand::value eventValue : eventsArray)
                {
                    auto stringResult = eventValue.get_string(false);
                    if (stringResult.error() != simdjson::SUCCESS)
                    {
                        LOG_APP_WARN("ParseFileWatchParams: skipping non-string event entry");
                        continue;
                    }

                    std::string_view eventView = stringResult.value();
                    std::string eventLower = ToLowerAscii(eventView);

                    if (eventLower == "created")
                    {
                        outEvents.push_back(FileEventType::Created);
                    }
                    else if (eventLower == "modified")
                    {
                        outEvents.push_back(FileEventType::Modified);
                    }
                    else if (eventLower == "deleted")
                    {
                        outEvents.push_back(FileEventType::Deleted);
                    }
                    else
                    {
                        LOG_APP_WARN("ParseFileWatchParams: unknown event '{}', ignoring", eventLower);
                    }
                }

                if (!outEvents.empty())
                {
                    hasEvents = true;
                }
            }
            else if (keyView == "debounce_ms")
            {
                auto intResult = value.get_int64();
                if (intResult.error() != simdjson::SUCCESS)
                {
                    LOG_APP_WARN("ParseFileWatchParams: 'debounce_ms' is not an integer, defaulting to 0");
                }
                else
                {
                    int64_t rawValue = intResult.value();

                    if (rawValue < 0)
                    {
                        rawValue = 0;
                    }

                    if (rawValue > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                    {
                        rawValue = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
                    }

                    outDebounceMilliseconds = static_cast<uint32_t>(rawValue);
                }
            }
        }

        if (!hasPath)
        {
            LOG_APP_ERROR("ParseFileWatchParams: missing 'path' field in params JSON");
            return false;
        }

        if (!hasEvents)
        {
            LOG_APP_ERROR("ParseFileWatchParams: no valid events in 'events' array");
            return false;
        }

        return true;
    }

} // anonymous namespace

namespace AIAssistant
{
    void WorkflowTriggerBinder::RegisterAll(WorkflowRegistry const& workflowRegistry, TriggerEngine& triggerEngine) const
    {
        std::vector<std::string> workflowIds = workflowRegistry.GetWorkflowIds();

        for (std::string const& workflowId : workflowIds)
        {
            std::optional<WorkflowDefinition> optionalWorkflowDefinition = workflowRegistry.GetWorkflow(workflowId);
            if (!optionalWorkflowDefinition.has_value())
            {
                LOG_APP_WARN("WorkflowTriggerBinder::RegisterAll: workflow '{}' disappeared during registration",
                             workflowId);
                continue;
            }

            WorkflowDefinition const& workflowDefinition = optionalWorkflowDefinition.value();

            if (workflowDefinition.m_Triggers.empty())
            {
                triggerEngine.AddAutoTrigger(workflowDefinition.m_Id, "auto", true);
                continue;
            }

            for (WorkflowTrigger const& workflowTrigger : workflowDefinition.m_Triggers)
            {
                switch (workflowTrigger.m_Type)
                {
                    case WorkflowTriggerType::Auto:
                    {
                        triggerEngine.AddAutoTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id,
                                                     workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::Cron:
                    {
                        std::string cronExpression;
                        if (!ParseCronParams(workflowTrigger.m_ParamsJson, cronExpression))
                        {
                            LOG_APP_ERROR(
                                "WorkflowTriggerBinder::RegisterAll: failed to parse cron params for trigger '{}' in "
                                "workflow '{}'",
                                workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddCronTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id, cronExpression,
                                                     workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::FileWatch:
                    {
                        std::string watchedPath;
                        std::vector<TriggerEngine::FileEventType> fileEvents;
                        uint32_t debounceMilliseconds = 0;

                        if (!ParseFileWatchParams(workflowTrigger.m_ParamsJson, watchedPath, fileEvents,
                                                  debounceMilliseconds))
                        {
                            LOG_APP_ERROR(
                                "WorkflowTriggerBinder::RegisterAll: failed to parse file_watch params for trigger '{}' "
                                "in workflow '{}'",
                                workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddFileWatchTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id, watchedPath,
                                                          fileEvents, debounceMilliseconds, workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::Manual:
                    {
                        triggerEngine.AddManualTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id,
                                                       workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::Structure:
                    {
                        LOG_APP_INFO(
                            "WorkflowTriggerBinder::RegisterAll: structure trigger '{}' in workflow '{}' is used for "
                            "per-item expansion and does not register a runtime trigger",
                            workflowTrigger.m_Id, workflowDefinition.m_Id);
                        break;
                    }

                    case WorkflowTriggerType::Unknown:
                    default:
                    {
                        LOG_APP_WARN("WorkflowTriggerBinder::RegisterAll: trigger '{}' of workflow '{}' has unsupported or "
                                     "unknown type",
                                     workflowTrigger.m_Id, workflowDefinition.m_Id);
                        break;
                    }
                }
            }
        }
    }

} // namespace AIAssistant
