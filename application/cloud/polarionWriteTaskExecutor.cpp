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

#include <filesystem>
#include <fstream>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/polarionWriteTaskExecutor.h"
#include "workflow/filter/polarionClient.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;

    bool PolarionWriteTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
                                                 WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                                                 TaskInstanceState& taskState, CloudConnection const& connection,
                                                 CloudCredentials const& credentials,
                                                 TaskCancellationToken const& cancellationToken)
    {
        // Parse task params
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            taskState.m_LastErrorMessage = "Failed to parse polarion_write params JSON";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string operation;
        {
            std::string_view sv;
            if (doc["operation"].get_string().get(sv) != simdjson::SUCCESS || sv.empty())
            {
                taskState.m_LastErrorMessage = "Missing required 'operation' in polarion_write params";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            operation = std::string(sv);
        }

        std::string const& baseUrl = connection.m_Endpoint;
        auto projectIt = connection.m_Params.find("project_id");
        if (projectIt == connection.m_Params.end() || projectIt->second.empty())
        {
            taskState.m_LastErrorMessage = "Connection '" + connection.m_Name + "' missing project_id param";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }
        std::string const& projectId = projectIt->second;

        PolarionClient client;
        std::string responseBody;
        std::string errorMessage;

        auto getStringParam = [&doc](std::string const& key) -> std::string
        {
            std::string_view sv;
            if (doc[key].get_string().get(sv) == simdjson::SUCCESS)
            {
                return std::string(sv);
            }
            return {};
        };

        if (operation == "update")
        {
            std::string workItemId = getStringParam("work_item_id");
            std::string body = getStringParam("body");

            if (workItemId.empty())
            {
                taskState.m_LastErrorMessage = "polarion_write 'update' requires 'work_item_id'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (body.empty())
            {
                taskState.m_LastErrorMessage = "polarion_write 'update' requires 'body' (JSON:API patch body)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (!client.UpdateWorkItem(baseUrl, projectId, workItemId, credentials.m_Token, body, responseBody,
                                       errorMessage))
            {
                taskState.m_LastErrorMessage = "Polarion update failed: " + errorMessage;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[polarion_write] updated work item {} in project {}", workItemId, projectId);
        }
        else if (operation == "create")
        {
            std::string body = getStringParam("body");

            if (body.empty())
            {
                taskState.m_LastErrorMessage = "polarion_write 'create' requires 'body' (JSON:API post body)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (!client.CreateWorkItem(baseUrl, projectId, credentials.m_Token, body, responseBody, errorMessage))
            {
                taskState.m_LastErrorMessage = "Polarion create failed: " + errorMessage;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[polarion_write] created work item in project {}", projectId);
        }
        else if (operation == "upload_attachment")
        {
            std::string workItemId = getStringParam("work_item_id");
            std::string filePath = getStringParam("file_path");
            std::string fileName = getStringParam("file_name");

            if (workItemId.empty())
            {
                taskState.m_LastErrorMessage = "polarion_write 'upload_attachment' requires 'work_item_id'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (filePath.empty())
            {
                taskState.m_LastErrorMessage = "polarion_write 'upload_attachment' requires 'file_path'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (fileName.empty())
            {
                fileName = std::filesystem::path(filePath).filename().string();
            }

            if (!client.UploadAttachment(baseUrl, projectId, workItemId, credentials.m_Token, filePath, fileName,
                                         responseBody, errorMessage))
            {
                taskState.m_LastErrorMessage = "Polarion upload failed: " + errorMessage;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[polarion_write] uploaded attachment '{}' to {}/{}", fileName, projectId, workItemId);
        }
        else if (operation == "download_attachment")
        {
            std::string workItemId = getStringParam("work_item_id");
            std::string attachmentId = getStringParam("attachment_id");
            std::string filePath = getStringParam("file_path");

            if (workItemId.empty() || attachmentId.empty() || filePath.empty())
            {
                taskState.m_LastErrorMessage =
                    "polarion_write 'download_attachment' requires 'work_item_id', 'attachment_id', 'file_path'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (!client.DownloadAttachment(baseUrl, projectId, workItemId, attachmentId, credentials.m_Token, filePath,
                                           errorMessage))
            {
                taskState.m_LastErrorMessage = "Polarion download failed: " + errorMessage;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody = "{\"ok\":true,\"file_path\":\"" + filePath + "\"}";
            LOG_APP_INFO("[polarion_write] downloaded attachment {} to {}", attachmentId, filePath);
        }
        else if (operation == "linked_items")
        {
            std::string workItemId = getStringParam("work_item_id");

            if (workItemId.empty())
            {
                taskState.m_LastErrorMessage = "polarion_write 'linked_items' requires 'work_item_id'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (!client.FetchLinkedWorkItems(baseUrl, projectId, workItemId, credentials.m_Token, responseBody,
                                             errorMessage))
            {
                taskState.m_LastErrorMessage = "Polarion linked items fetch failed: " + errorMessage;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[polarion_write] fetched linked items for {}/{}", projectId, workItemId);
        }
        else
        {
            taskState.m_LastErrorMessage =
                "Unknown polarion_write operation '" + operation +
                "'. Valid: update, create, upload_attachment, download_attachment, linked_items";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Write response to stdout capture and to file
        taskState.m_CapturedStdout = responseBody.substr(0, std::min(responseBody.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write full response to the task working directory
        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        if (!workflowBaseDir.empty())
        {
            std::filesystem::path workDir =
                TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

            std::error_code ec;
            std::filesystem::create_directories(workDir, ec);

            std::filesystem::path responsePath = workDir / "response.json";
            std::ofstream responseFile(responsePath, std::ios::trunc);
            if (responseFile.is_open())
            {
                responseFile << responseBody;
            }
        }

        return true;
    }
} // namespace AIAssistant
