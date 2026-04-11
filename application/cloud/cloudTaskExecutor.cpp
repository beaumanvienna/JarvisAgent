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

#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "cloud/cloudTaskExecutor.h"
#include "cloud/cloudConnectorRegistry.h"
#include "cloud/cloudConnectionManager.h"
#include "cloud/cloudCircuitBreaker.h"

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

        // Look up the connection config
        CloudConnection const* connection = m_ConnectionManager.GetConnection(connectionName);
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

        bool success = ExecuteCloud(workflowDefinition, workflowRun, taskDefinition, taskState, *connection,
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

        // Reject obvious traversal patterns
        if (localPath.find("..") != std::string::npos)
        {
            LOG_SECURITY_INFO("[security] path_traversal_blocked: task='{}' local_path='{}' contains '..'", taskId,
                              localPath);
            return false;
        }

        // Resolve and verify the path stays within the base directory
        std::filesystem::path resolved = (baseDir / localPath).lexically_normal();
        std::string resolvedStr = resolved.string();
        std::string baseDirStr = baseDir.lexically_normal().string();

        if (resolvedStr.find(baseDirStr) != 0)
        {
            LOG_SECURITY_INFO("[security] path_traversal_blocked: task='{}' resolved='{}' escapes base='{}'", taskId,
                              resolvedStr, baseDirStr);
            return false;
        }

        return true;
    }
} // namespace AIAssistant
