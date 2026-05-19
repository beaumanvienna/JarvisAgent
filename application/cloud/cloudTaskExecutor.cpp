/* Copyright (c) 2026 JC Technolabs
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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <fstream>

#include "simdjson/simdjson.h"

#include "auxiliary/file.h"
#include "core.h"
#include "engine.h"
#include "cloud/cloudTaskExecutor.h"
#include "cloud/cloudConnectorRegistry.h"
#include "cloud/cloudConnectionManager.h"
#include "cloud/cloudCircuitBreaker.h"
#include "workflow/templateEngine.h"

namespace AIAssistant
{
    ICloudTaskExecutor::ICloudTaskExecutor(CloudConnectorRegistry& connectorRegistry,
                                           CloudConnectionManager& connectionManager)
        : m_ConnectorRegistry(connectorRegistry), m_ConnectionManager(connectionManager)
    {
    }

    bool ICloudTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                     TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        // Extract connection name from task params JSON
        std::string connectionName;
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
            simdjson::ondemand::document doc;

            auto error = parser.iterate(paddedJson).get(doc);
            if (error)
            {
                taskState.m_LastErrorMessage = "Failed to parse task params JSON";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("ICloudTaskExecutor: JSON parse error for task '{}': {}", taskDefinition.m_Id,
                              simdjson::error_message(error));
                return false;
            }

            std::string_view sv;
            if (doc["connection"].get_string().get(sv) != simdjson::SUCCESS || sv.empty())
            {
                taskState.m_LastErrorMessage = "Missing required 'connection' param in cloud task";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("ICloudTaskExecutor: task '{}' has no 'connection' param", taskDefinition.m_Id);
                return false;
            }
            connectionName = std::string(sv);
        }

        // Look up the connection config.  GetConnection returns std::optional<CloudConnection>
        // by value; the optional owns the bytes for the rest of this function so the
        // CloudConnection reference passed to ResolveCredentials / ExecuteCloud below
        // (which may run for seconds during cloud I/O) is stable regardless of any
        // concurrent connection-manager mutation.
        auto connection = m_ConnectionManager.GetConnection(connectionName);
        if (!connection)
        {
            taskState.m_LastErrorMessage = "Cloud connection '" + connectionName + "' not found";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("ICloudTaskExecutor: connection '{}' not found for task '{}'", connectionName,
                          taskDefinition.m_Id);
            return false;
        }

        // Get the connector for this connection type
        ICloudConnector* connector = m_ConnectorRegistry.GetConnector(connection->m_Type);
        if (!connector)
        {
            taskState.m_LastErrorMessage = "No cloud connector registered for type '" + connection->m_Type + "'";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("ICloudTaskExecutor: no connector for type '{}' (task '{}')", connection->m_Type,
                          taskDefinition.m_Id);
            return false;
        }

        // Resolve credentials
        CloudCredentials credentials;
        std::string credError;
        if (!connector->ResolveCredentials(*connection, credentials, credError))
        {
            taskState.m_LastErrorMessage = "Credential resolution failed: " + credError;
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("ICloudTaskExecutor: credential resolution failed for task '{}': {}", taskDefinition.m_Id,
                          credError);
            return false;
        }

        // Audit log: cloud task execution
        LOG_SECURITY_INFO("[security] cloud_task_execute: task='{}' connection='{}' type='{}' run='{}'",
                          taskDefinition.m_Id, connectionName, connection->m_Type, workflowRun.m_RunId);

        // Check circuit breaker before proceeding
        auto& circuitBreaker = Core::g_Core->GetCloudCircuitBreaker();
        if (!circuitBreaker.AllowRequest(connectionName))
        {
            taskState.m_LastErrorMessage = "Cloud connection '" + connectionName +
                                           "' circuit breaker is open — request short-circuited";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_WARN("ICloudTaskExecutor: circuit breaker open for '{}', task '{}' short-circuited",
                         connectionName, taskDefinition.m_Id);
            return false;
        }

        // Use the run's cancellation token (wired into RequestCancelRun in Phase 9).
        // Falls back to a local no-op token if the run has no token (shouldn't happen).
        TaskCancellationToken localToken;
        TaskCancellationToken const& cancellationToken =
            workflowRun.m_CancellationToken ? *workflowRun.m_CancellationToken : localToken;

        // Expand template variables in params JSON (e.g. {{item.id}}, {{taskId.output_file}}).
        // Per-item filter bindings and upstream per_item outputs are in taskState.m_InputValues.
        // Values are JSON-escaped before substitution so that embedded quotes, backslashes,
        // and newlines don't break the JSON structure of m_ParamsJson.
        TaskDef expandedTaskDef = taskDefinition;
        if (!taskState.m_InputValues.empty() && !taskDefinition.m_ParamsJson.empty())
        {
            std::unordered_map<std::string, std::string> jsonEscapedValues;
            jsonEscapedValues.reserve(taskState.m_InputValues.size());
            for (auto const& [key, value] : taskState.m_InputValues)
            {
                std::string escaped;
                escaped.reserve(value.size() + 16);
                for (char c : value)
                {
                    switch (c)
                    {
                        case '"':  escaped += "\\\""; break;
                        case '\\': escaped += "\\\\"; break;
                        case '\n': escaped += "\\n"; break;
                        case '\r': escaped += "\\r"; break;
                        case '\t': escaped += "\\t"; break;
                        default:   escaped += c; break;
                    }
                }
                jsonEscapedValues[key] = std::move(escaped);
            }

            TemplateContext templateContext;
            templateContext.m_InputValues = &jsonEscapedValues;

            std::string expandedParams;
            std::string templateError;
            if (ExpandTemplate(taskDefinition.m_ParamsJson, templateContext, TemplateMode::Lenient,
                               expandedParams, templateError))
            {
                expandedTaskDef.m_ParamsJson = std::move(expandedParams);
            }
            else
            {
                LOG_APP_WARN("ICloudTaskExecutor: template expansion warning for task '{}': {}",
                             taskDefinition.m_Id, templateError);
            }
        }

        bool success = ExecuteCloud(workflowDefinition, workflowRun, expandedTaskDef, taskState, *connection,
                                    credentials, cancellationToken);

        // Record result in circuit breaker
        if (success)
        {
            circuitBreaker.RecordSuccess(connectionName);
        }
        else
        {
            circuitBreaker.RecordFailure(connectionName);
        }

        return success;
    }
    bool ICloudTaskExecutor::ValidateLocalPath(std::string const& localPath,
                                               std::filesystem::path const& baseDir, std::string const& taskId)
    {
        if (localPath.empty())
        {
            return true;
        }

        // Per JCWF spec §3.2.1: task-scoped relative file paths resolve relative
        // to the task's working_directory; the spec also explicitly allows `..`
        // segments in working_directory and (by extension) file paths, resolving
        // via lexical normalization.  And the upstream-output template
        // `{{A.output_file}}` produces an ABSOLUTE path — that's the canonical
        // way a downstream cloud task consumes an upstream ai_call's output.
        //
        // The security boundary therefore lives at the project tree (the
        // JarvisAgent launch CWD), NOT the task working_directory.  That's wide
        // enough for both literal relative paths under any task's workDir AND
        // absolute template values pointing into queue/, while still rejecting
        // paths that escape the project (e.g. `/etc/passwd`).
        std::filesystem::path const inputPath(localPath);
        std::filesystem::path const resolved =
            (inputPath.is_absolute() ? inputPath : baseDir / inputPath).lexically_normal();
        std::filesystem::path const launchCwd =
            std::filesystem::path(Core::g_Core->GetLaunchCWDAbsolute()).lexically_normal();

        std::string const resolvedStr = resolved.string();
        std::string const launchCwdStr = launchCwd.string();

        if (resolvedStr.find(launchCwdStr) != 0)
        {
            LOG_SECURITY_INFO("[security] path_traversal_blocked: task='{}' resolved='{}' escapes launch_cwd='{}'",
                              taskId, resolvedStr, launchCwdStr);
            return false;
        }

        return true;
    }

    std::string ICloudTaskExecutor::ResponseJsonFilename(TaskInstanceState const& taskState)
    {
        // Per-item child instance ids have the form "parentId#N".  Regular tasks have
        // no '#' — use the plain filename for them to preserve existing behaviour.
        std::string const& instanceId = taskState.m_TaskInstanceId;
        auto const hashPos = instanceId.rfind('#');
        if (hashPos == std::string::npos)
        {
            return "response.json";
        }
        std::string const suffix = instanceId.substr(hashPos + 1);
        if (suffix.empty())
        {
            return "response.json";
        }
        return "response_" + suffix + ".json";
    }

    void ICloudTaskExecutor::WriteResponseJson(std::filesystem::path const& workDir,
                                               TaskInstanceState const& taskState,
                                               std::string const& responseBody)
    {
        if (workDir.empty())
        {
            return;
        }

        // response.json is the downstream-task input for cloud tasks — a
        // truncated partial would be parsed as malformed.  Atomic write
        // through the shared helper guarantees readers see either the
        // previous version or the new one.
        std::filesystem::path const path = workDir / ResponseJsonFilename(taskState);
        std::string writeError;
        if (!EngineCore::AtomicWriteFile(path, responseBody, writeError))
        {
            LOG_APP_ERROR("CloudTaskExecutor::WriteResponseJson: {} taskInstance='{}' path='{}'", writeError,
                          taskState.m_TaskInstanceId, path.string());
        }
    }

    bool ICloudTaskExecutor::ContainsCrlf(std::string const& s)
    {
        return s.find('\r') != std::string::npos || s.find('\n') != std::string::npos;
    }
} // namespace AIAssistant
