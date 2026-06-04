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

#include "taskPathResolver.h"
#include "workflowTriggerBinder.h"
#include "workflowRegistry.h"
#include "workflowTypes.h"
#include "triggerEngine.h"

#include <cctype>
#include <filesystem>
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

    bool ParseCronParams(std::string const& paramsJson, std::string& outExpression, std::string& outTimezone)
    {
        outExpression.clear();
        outTimezone.clear();

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
            }
            else if (keyView == "timezone")
            {
                ondemand::value value = field.value();

                auto stringResult = value.get_string(false);
                if (stringResult.error() != simdjson::SUCCESS)
                {
                    LOG_APP_WARN("ParseCronParams: 'timezone' must be a string, ignoring");
                }
                else
                {
                    std::string_view timezoneView = stringResult.value();
                    outTimezone.assign(timezoneView.begin(), timezoneView.end());
                }
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

    bool ParseWebhookParams(std::string const& paramsJson, std::string& outSecret)
    {
        outSecret.clear();

        // Webhook secret is **mandatory** — pre-fix, missing/empty/invalid
        // params silently produced an open webhook (any caller could trigger
        // the workflow without HMAC).  Post-fix: every failure mode returns
        // false with ERROR log; only an explicitly-supplied non-empty
        // `secret` field leads to a registered webhook trigger.  The
        // TriggerEngine validator additionally refuses empty-secret webhooks
        // at registration; this gate is defense in depth at the parse layer.
        if (paramsJson.empty())
        {
            LOG_APP_ERROR("ParseWebhookParams: webhook params JSON is empty — secret is required");
            return false;
        }

        ondemand::parser parser;
        padded_string json = padded_string(paramsJson.data(), paramsJson.size());

        ondemand::document document;
        simdjson::error_code errorCode = parser.iterate(json).get(document);
        if (errorCode != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("ParseWebhookParams: failed to parse params JSON: {}",
                          simdjson::error_message(errorCode));
            return false;
        }

        auto objectResult = document.get_object();
        if (objectResult.error() != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("ParseWebhookParams: webhook params root is not a JSON object");
            return false;
        }

        ondemand::object rootObject = objectResult.value();

        for (auto field : rootObject)
        {
            auto keyResult = field.unescaped_key();
            if (keyResult.error() != simdjson::SUCCESS)
            {
                continue;
            }

            std::string_view keyView = keyResult.value();

            if (keyView == "secret")
            {
                ondemand::value value = field.value();
                auto stringResult = value.get_string(false);
                if (stringResult.error() == simdjson::SUCCESS)
                {
                    std::string_view secretView = stringResult.value();
                    outSecret.assign(secretView.begin(), secretView.end());
                }
            }
        }

        if (outSecret.empty())
        {
            LOG_APP_ERROR("ParseWebhookParams: 'secret' field missing or empty in webhook params");
            return false;
        }
        return true;
    }

} // anonymous namespace

namespace AIAssistant
{
    void WorkflowTriggerBinder::RegisterAll(WorkflowRegistry const& workflowRegistry, TriggerEngine& triggerEngine,
                                            bool fireAutoTriggers) const
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

            // Sub-workflows are invoked by their parent — never auto-trigger them.
            if (workflowDefinition.m_IsSubWorkflow)
            {
                continue;
            }

            if (workflowDefinition.m_Triggers.empty())
            {
                triggerEngine.AddAutoTrigger(workflowDefinition.m_Id, "auto", true, fireAutoTriggers);
                continue;
            }

            for (WorkflowTrigger const& workflowTrigger : workflowDefinition.m_Triggers)
            {
                switch (workflowTrigger.m_Type)
                {
                    case WorkflowTriggerType::Auto:
                    {
                        triggerEngine.AddAutoTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id,
                                                     workflowTrigger.m_IsEnabled, fireAutoTriggers);
                        break;
                    }

                    case WorkflowTriggerType::Cron:
                    {
                        std::string cronExpression;
                        std::string cronTimezone;
                        if (!ParseCronParams(workflowTrigger.m_ParamsJson, cronExpression, cronTimezone))
                        {
                            LOG_APP_ERROR(
                                "WorkflowTriggerBinder::RegisterAll: failed to parse cron params for trigger '{}' in "
                                "workflow '{}'",
                                workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddCronTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id, cronExpression,
                                                     cronTimezone, workflowTrigger.m_IsEnabled);
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

                        LOG_APP_INFO(
                            "[paths debug] debug reason=bindTrigger workflowId={} triggerId={} watchedPathRelative={} "
                            "workflowBaseDirectoryRelative={} workflowBaseDirectoryAbsolute={}",
                            workflowDefinition.m_Id, workflowTrigger.m_Id, watchedPath,
                            workflowDefinition.m_WorkflowBaseDirectory, workflowDefinition.m_WorkflowBaseDirectoryAbsolute);

                        std::string watchedPathAbsolute = watchedPath;

                        if (!watchedPath.empty())
                        {
                            std::filesystem::path const workflowBaseDirectoryPathAbsolute =
                                TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);

                            // Spec: workflow-level relative trigger paths resolve relative to Workflow Base Directory.
                            if (!watchedPath.empty() && watchedPath.front() != '/')
                            {
                                watchedPathAbsolute =
                                    (workflowBaseDirectoryPathAbsolute / std::filesystem::path(watchedPath))
                                        .lexically_normal()
                                        .generic_string();
                            }
                            else
                            {
                                watchedPathAbsolute = std::filesystem::path(watchedPath).lexically_normal().generic_string();
                            }
                        }

                        LOG_APP_INFO("[paths debug] debug reason=bindTriggerResolved workflowId={} triggerId={} "
                                     "watchedPathRelative={} watchedPathAbsolute={} workflowBaseDirectoryRelative={} "
                                     "workflowBaseDirectoryAbsolute={}",
                                     workflowDefinition.m_Id, workflowTrigger.m_Id, watchedPath, watchedPathAbsolute,
                                     workflowDefinition.m_WorkflowBaseDirectory,
                                     workflowDefinition.m_WorkflowBaseDirectoryAbsolute);

                        triggerEngine.AddFileWatchTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id, watchedPathAbsolute,
                                                          fileEvents, debounceMilliseconds, workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::Manual:
                    {
                        triggerEngine.AddManualTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id,
                                                       workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::Webhook:
                    {
                        std::string secret;
                        if (!ParseWebhookParams(workflowTrigger.m_ParamsJson, secret))
                        {
                            // Webhook secret is mandatory.  Skip registration on
                            // any parse failure or missing/empty secret — the
                            // ERROR log inside ParseWebhookParams details which.
                            LOG_APP_ERROR("WorkflowTriggerBinder: skipping webhook trigger '{}' for workflow '{}' "
                                          "— secret invalid or missing",
                                          workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddWebhookTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id, secret,
                                                        workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::S3Watch:
                    {
                        std::string connectionName;
                        std::string bucket;
                        std::string prefix;
                        uint32_t pollIntervalSeconds = 300;

                        // Parse s3_watch params from JSON
                        if (!workflowTrigger.m_ParamsJson.empty())
                        {
                            simdjson::ondemand::parser s3Parser;
                            simdjson::padded_string s3Padded(workflowTrigger.m_ParamsJson);
                            simdjson::ondemand::document s3Doc;

                            if (s3Parser.iterate(s3Padded).get(s3Doc) == simdjson::SUCCESS)
                            {
                                std::string_view sv;
                                if (s3Doc["connection"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    connectionName = std::string(sv);
                                }
                                if (s3Doc["bucket"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    bucket = std::string(sv);
                                }
                                if (s3Doc["prefix"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    prefix = std::string(sv);
                                }
                                int64_t interval = 0;
                                if (s3Doc["poll_interval_seconds"].get_int64().get(interval) == simdjson::SUCCESS)
                                {
                                    pollIntervalSeconds = static_cast<uint32_t>(interval);
                                }
                            }
                        }

                        if (connectionName.empty())
                        {
                            LOG_APP_ERROR(
                                "WorkflowTriggerBinder::RegisterAll: s3_watch trigger '{}' in workflow '{}' missing "
                                "'connection' param",
                                workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddS3WatchTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id, connectionName,
                                                        bucket, prefix, pollIntervalSeconds, workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::OneDriveWatch:
                    {
                        std::string connectionName;
                        std::string folder;
                        uint32_t pollIntervalSeconds = 300;

                        if (!workflowTrigger.m_ParamsJson.empty())
                        {
                            simdjson::ondemand::parser odParser;
                            simdjson::padded_string odPadded(workflowTrigger.m_ParamsJson);
                            simdjson::ondemand::document odDoc;

                            if (odParser.iterate(odPadded).get(odDoc) == simdjson::SUCCESS)
                            {
                                std::string_view sv;
                                if (odDoc["connection"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    connectionName = std::string(sv);
                                }
                                if (odDoc["folder"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    folder = std::string(sv);
                                }
                                uint64_t interval;
                                if (odDoc["poll_interval_seconds"].get_uint64().get(interval) == simdjson::SUCCESS)
                                {
                                    pollIntervalSeconds = static_cast<uint32_t>(interval);
                                }
                            }
                        }

                        if (connectionName.empty())
                        {
                            LOG_APP_ERROR(
                                "WorkflowTriggerBinder::RegisterAll: onedrive_watch trigger '{}' in workflow '{}' "
                                "missing 'connection' param",
                                workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddOneDriveWatchTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id,
                                                             connectionName, folder, pollIntervalSeconds,
                                                             workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::EmailWatch:
                    {
                        std::string connectionName;
                        std::string folder;
                        std::string subjectFilter;
                        uint32_t pollIntervalSeconds = 300;

                        if (!workflowTrigger.m_ParamsJson.empty())
                        {
                            simdjson::ondemand::parser emailParser;
                            simdjson::padded_string emailPadded(workflowTrigger.m_ParamsJson);
                            simdjson::ondemand::document emailDoc;

                            if (emailParser.iterate(emailPadded).get(emailDoc) == simdjson::SUCCESS)
                            {
                                std::string_view sv;
                                if (emailDoc["connection"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    connectionName = std::string(sv);
                                }
                                if (emailDoc["folder"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    folder = std::string(sv);
                                }
                                if (emailDoc["subject_filter"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    subjectFilter = std::string(sv);
                                }
                                uint64_t interval;
                                if (emailDoc["poll_interval_seconds"].get_uint64().get(interval) == simdjson::SUCCESS)
                                {
                                    pollIntervalSeconds = static_cast<uint32_t>(interval);
                                }
                            }
                        }

                        if (connectionName.empty())
                        {
                            LOG_APP_ERROR(
                                "WorkflowTriggerBinder::RegisterAll: email_watch trigger '{}' in workflow '{}' "
                                "missing 'connection' param",
                                workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddEmailWatchTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id,
                                                          connectionName, folder, subjectFilter,
                                                          pollIntervalSeconds, workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::AzureBlobWatch:
                    {
                        std::string connectionName;
                        std::string container;
                        std::string prefix;
                        uint32_t pollIntervalSeconds = 300;

                        if (!workflowTrigger.m_ParamsJson.empty())
                        {
                            simdjson::ondemand::parser azureParser;
                            simdjson::padded_string azurePadded(workflowTrigger.m_ParamsJson);
                            simdjson::ondemand::document azureDoc;

                            if (azureParser.iterate(azurePadded).get(azureDoc) == simdjson::SUCCESS)
                            {
                                std::string_view sv;
                                if (azureDoc["connection"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    connectionName = std::string(sv);
                                }
                                if (azureDoc["container"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    container = std::string(sv);
                                }
                                if (azureDoc["prefix"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    prefix = std::string(sv);
                                }
                                int64_t interval = 0;
                                if (azureDoc["poll_interval_seconds"].get_int64().get(interval) == simdjson::SUCCESS)
                                {
                                    pollIntervalSeconds = static_cast<uint32_t>(interval);
                                }
                            }
                        }

                        if (connectionName.empty())
                        {
                            LOG_APP_ERROR(
                                "WorkflowTriggerBinder::RegisterAll: azure_blob_watch trigger '{}' in workflow '{}' "
                                "missing 'connection' param",
                                workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddAzureBlobWatchTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id,
                                                               connectionName, container, prefix,
                                                               pollIntervalSeconds, workflowTrigger.m_IsEnabled);
                        break;
                    }

                    case WorkflowTriggerType::GcsWatch:
                    {
                        std::string connectionName;
                        std::string bucket;
                        std::string prefix;
                        uint32_t pollIntervalSeconds = 300;

                        if (!workflowTrigger.m_ParamsJson.empty())
                        {
                            simdjson::ondemand::parser gcsParser;
                            simdjson::padded_string gcsPadded(workflowTrigger.m_ParamsJson);
                            simdjson::ondemand::document gcsDoc;

                            if (gcsParser.iterate(gcsPadded).get(gcsDoc) == simdjson::SUCCESS)
                            {
                                std::string_view sv;
                                if (gcsDoc["connection"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    connectionName = std::string(sv);
                                }
                                if (gcsDoc["bucket"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    bucket = std::string(sv);
                                }
                                if (gcsDoc["prefix"].get_string().get(sv) == simdjson::SUCCESS)
                                {
                                    prefix = std::string(sv);
                                }
                                int64_t interval = 0;
                                if (gcsDoc["poll_interval_seconds"].get_int64().get(interval) == simdjson::SUCCESS)
                                {
                                    pollIntervalSeconds = static_cast<uint32_t>(interval);
                                }
                            }
                        }

                        if (connectionName.empty())
                        {
                            LOG_APP_ERROR(
                                "WorkflowTriggerBinder::RegisterAll: gcs_watch trigger '{}' in workflow '{}' "
                                "missing 'connection' param",
                                workflowTrigger.m_Id, workflowDefinition.m_Id);
                            break;
                        }

                        triggerEngine.AddGcsWatchTrigger(workflowDefinition.m_Id, workflowTrigger.m_Id, connectionName,
                                                         bucket, prefix, pollIntervalSeconds,
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
