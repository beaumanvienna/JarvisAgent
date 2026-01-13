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

#include <fstream>
#include <algorithm>
#include <sstream>
#include <optional>
#include <filesystem>
#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "web/webServer.h"
#include "web/chatMessages.h"

#include "workflow/workflowRegistry.h"
#include "workflow/workflowJsonParser.h"

#include "workflow/workflowRuntimeManager.h"
#include "workflow/workflowOrchestrator.h"
#include "workflow/workflowTypes.h"

#include "event/events.h"

namespace fs = std::filesystem;
namespace AIAssistant

{
    namespace
    {
        crow::response MakeWorkflowJsonError(int const httpStatus,
                                            std::string const& errorCode,
                                            std::string const& message,
                                            std::string const& endpoint,
                                            std::optional<std::string> const& workflowId = std::nullopt)
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = errorCode;
            responseJson["message"] = message;
            responseJson["endpoint"] = endpoint;
            if (workflowId.has_value())
            {
                responseJson["workflowId"] = workflowId.value();
            }

            return crow::response(httpStatus, responseJson.dump());
        }

        bool IsValidWorkflowId(std::string const& workflowId)
        {
            if (workflowId.empty())
            {
                return false;
            }

            for (char const character : workflowId)
            {
                bool const isAlphaNumeric = ((character >= 'a' && character <= 'z') ||
                                             (character >= 'A' && character <= 'Z') ||
                                             (character >= '0' && character <= '9'));
                bool const isAllowedSymbol = (character == '_' || character == '-');
                if (!isAlphaNumeric && !isAllowedSymbol)
                {
                    return false;
                }
            }

            return true;
        }

        std::filesystem::path GetWorkflowsDirectoryAbsolute(std::string& errorMessage)
        {
            errorMessage.clear();

            if (Core::g_Core == nullptr)
            {
                errorMessage = "Core::g_Core is null";
                return {};
            }

            auto const& config = Core::g_Core->GetConfig();

            std::filesystem::path workflowsPathFromConfig = std::filesystem::path(config.m_WorkflowsFolderFilepath);
            if (workflowsPathFromConfig.empty())
            {
                errorMessage = "Config m_WorkflowsFolderFilepath is empty";
                return {};
            }

            std::filesystem::path const& launchCwdAbsolute = Core::g_Core->GetLaunchCWDAbsolute();
            std::filesystem::path workflowsDirectoryAbsolute =
                workflowsPathFromConfig.is_absolute()
                    ? workflowsPathFromConfig
                    : (launchCwdAbsolute / workflowsPathFromConfig);

            return std::filesystem::absolute(workflowsDirectoryAbsolute).lexically_normal();
        }

        bool ReadTextFile(std::filesystem::path const& filePath, std::string& outContent)
        {
            std::ifstream fileStream(filePath, std::ios::in | std::ios::binary);
            if (!fileStream)
            {
                return false;
            }

            std::ostringstream stringStream;
            stringStream << fileStream.rdbuf();
            outContent = stringStream.str();
            return true;
        }

        bool WriteTextFileAtomic(std::filesystem::path const& filePath, std::string const& content, std::string& errorMessage)
        {
            errorMessage.clear();
            std::filesystem::path const parentDirectory = filePath.parent_path();
            std::error_code errorCode;
            std::filesystem::create_directories(parentDirectory, errorCode);
            if (errorCode)
            {
                errorMessage = "Failed to create directories: " + parentDirectory.string() + " error=" + errorCode.message();
                return false;
            }

            std::filesystem::path const tempPath = filePath.string() + ".tmp";
            {
                std::ofstream fileStream(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!fileStream)
                {
                    errorMessage = "Failed to open temp file for writing: " + tempPath.string();
                    return false;
                }

                fileStream.write(content.data(), static_cast<std::streamsize>(content.size()));
                if (!fileStream.good())
                {
                    errorMessage = "Failed while writing temp file: " + tempPath.string();
                    return false;
                }
            }

            std::filesystem::rename(tempPath, filePath, errorCode);
            if (errorCode)
            {
                // Try to cleanup temp file best-effort.
                std::filesystem::remove(tempPath, errorCode);
                errorMessage = "Failed to rename temp file to target: " + filePath.string() + " error=" + errorCode.message();
                return false;
            }

            return true;
        }


char const* ToStringWorkflowRunState(WorkflowRunState const state)
{
    switch (state)
    {
    case WorkflowRunState::Pending:
        return "pending";
    case WorkflowRunState::Running:
        return "running";
    case WorkflowRunState::Succeeded:
        return "succeeded";
    case WorkflowRunState::Failed:
        return "failed";
    case WorkflowRunState::Cancelled:
        return "cancelled";
    default:
        return "unknown";
    }
}

char const* ToStringTaskInstanceStateKind(TaskInstanceStateKind const state)
{
    switch (state)
    {
    case TaskInstanceStateKind::Pending:
        return "pending";
    case TaskInstanceStateKind::Ready:
        return "ready";
    case TaskInstanceStateKind::Running:
        return "running";
    case TaskInstanceStateKind::Skipped:
        return "skipped";
    case TaskInstanceStateKind::Succeeded:
        return "succeeded";
    case TaskInstanceStateKind::Failed:
        return "failed";
    case TaskInstanceStateKind::WaitingExternal:
        return "waiting_external";
    default:
        return "unknown";
    }
}

    } // namespace

    WebServer::WebServer()
    {
        m_Server.loglevel(crow::LogLevel::Warning);
        RegisterRoutes();
        RegisterWebSocket();
    }

    WebServer::~WebServer() { Stop(); }


void WebServer::SetWorkflowRegistry(WorkflowRegistry const* workflowRegistry)
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);
    m_WorkflowRegistry = workflowRegistry;
}

void WebServer::SetWorkflowRuntimeManager(WorkflowRuntimeManager* workflowRuntimeManager)
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);
    m_WorkflowRuntimeManager = workflowRuntimeManager;
}

void WebServer::BroadcastWorkflowRunsSnapshot()
{
    WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        workflowRuntimeManager = m_WorkflowRuntimeManager;
    }

    if (workflowRuntimeManager == nullptr)
    {
        return;
    }

    crow::json::wvalue msg;
    msg["type"] = "workflow-runs-snapshot";

    auto activeRuns = workflowRuntimeManager->GetActiveRunsSnapshot();
    crow::json::wvalue activeRunsJson = crow::json::wvalue::list();
    for (auto const& run : activeRuns)
    {
        crow::json::wvalue runJson;
        runJson["runId"] = run.m_RunId;
        runJson["workflowId"] = run.m_WorkflowId;
        runJson["state"] = ToStringWorkflowRunState(run.m_State);
        runJson["startedAt"] = run.m_StartedAtIso8601;
        runJson["completedAt"] = run.m_CompletedAtIso8601;
        activeRunsJson.emplace_back(std::move(runJson));
    }
    msg["activeRuns"] = std::move(activeRunsJson);

    BroadcastJSON(msg.dump());
}


    void WebServer::RegisterRoutes()
    {
        // ---- Serve static index page ----
        CROW_ROUTE(m_Server, "/")(
            []()
            {
                std::ifstream file("web/index.html");
                if (!file)
                {
                    return crow::response(404, "index.html not found");
                }

                std::stringstream buffer;
                buffer << file.rdbuf();
                return crow::response(200, buffer.str());
            });

        // ---- POST /api/chat ----
        CROW_ROUTE(m_Server, "/api/chat")
            .methods("POST"_method)([this](const crow::request& req) { return HandleChatPost(req); });

        // ---- GET /api/status ----
        CROW_ROUTE(m_Server, "/api/status")([this]() { return HandleStatusGet(); });

        // ---- Workflow Editor: CRUD (stubs moved to real implementation) ----
        CROW_ROUTE(m_Server, "/api/workflows")
            .methods("GET"_method)([this]() { return HandleWorkflowsListGet(); });

        CROW_ROUTE(m_Server, "/api/workflows")
            .methods("POST"_method)([this](crow::request const& req) { return HandleWorkflowsCreatePost(req); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("GET"_method)([this](std::string const& workflowId) { return HandleWorkflowGet(workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("PUT"_method)([this](crow::request const& req, std::string const& workflowId)
                                   { return HandleWorkflowUpdatePut(req, workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("DELETE"_method)([this](std::string const& workflowId) { return HandleWorkflowDelete(workflowId); });


// ---- Workflow Editor: validation ----
CROW_ROUTE(m_Server, "/api/workflows/validate")
    .methods("POST"_method)([this](crow::request const& req) { return HandleWorkflowValidatePost(req); });

CROW_ROUTE(m_Server, "/api/workflows/<string>/validate")
    .methods("GET"_method)([this](std::string const& workflowId) { return HandleWorkflowValidateGet(workflowId); });

// ---- Workflow Editor: run control + monitoring ----
CROW_ROUTE(m_Server, "/api/workflows/<string>/run")
    .methods("POST"_method)([this](std::string const& workflowId) { return HandleWorkflowRunPost(workflowId); });

CROW_ROUTE(m_Server, "/api/workflow-runs/active")
    .methods("GET"_method)([this]() { return HandleWorkflowRunsActiveGet(); });

CROW_ROUTE(m_Server, "/api/workflow-runs/last")
    .methods("GET"_method)([this]() { return HandleWorkflowRunsLastGet(); });

CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/cancel")
    .methods("POST"_method)([this](std::string const& runId) { return HandleWorkflowRunCancelPost(runId); });

    }

    crow::response WebServer::HandleChatPost(const crow::request& req)
    {
        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string json(req.body);
            auto doc = parser.iterate(json);

            std::string subsystem = std::string(doc["subsystem"].get_string().value());
            std::string message = std::string(doc["message"].get_string().value());

            fs::path queuePath = fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath) / subsystem;

            fs::create_directories(queuePath);

            uint64_t id = App::g_App->GetChatMessagePool()->AddMessage(subsystem, message);
            fs::path filename = queuePath / ("ISSUE_" + std::to_string(id) + ".txt");

            std::ofstream out(filename);
            out << message;
            out.close();

            crow::json::wvalue response;
            response["status"] = "queued";
            response["id"] = id;
            response["file"] = filename.string();
            return crow::response(200, response.dump());
        }
        catch (const std::exception& e)
        {
            crow::json::wvalue error;
            error["error"] = e.what();
            return crow::response(400, error.dump());
        }
    }

    crow::response WebServer::HandleStatusGet()
    {
        crow::json::wvalue status;

        status["type"] = "status";
        status["name"] = "../queue/ICE";
        status["state"] = "SendingQueries";
        status["outputs"] = 4;
        status["inflight"] = 1;
        status["completed"] = 7;
        return crow::response(200, status.dump());
    }

    

    crow::response WebServer::HandleWorkflowsListGet()
    {
        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "GET /api/workflows");
        }

        WorkflowRegistry workflowRegistry;
        if (!workflowRegistry.LoadDirectory(workflowsDirectoryAbsolute))
        {
            return MakeWorkflowJsonError(500, "workflow_registry_load_failed",
                                         "Failed to load workflows directory: " + workflowsDirectoryAbsolute.string(),
                                         "GET /api/workflows");
        }

        std::vector<std::string> workflowIds = workflowRegistry.GetWorkflowIds();
        std::sort(workflowIds.begin(), workflowIds.end());

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;

        crow::json::wvalue::list workflowsList;
        workflowsList.reserve(workflowIds.size());

        for (std::string const& workflowId : workflowIds)
        {
            crow::json::wvalue workflowEntry;
            workflowEntry["id"] = workflowId;

            std::optional<WorkflowDefinition> workflowDefinition = workflowRegistry.GetWorkflow(workflowId);
            if (workflowDefinition.has_value())
            {
                if (!workflowDefinition->m_Label.empty())
                {
                    workflowEntry["label"] = workflowDefinition->m_Label;
                }

                if (!workflowDefinition->m_WorkflowFilePath.empty())
                {
                    workflowEntry["path"] = workflowDefinition->m_WorkflowFilePath;
                }
            }

            workflowsList.emplace_back(std::move(workflowEntry));
        }

        responseJson["workflows"] = std::move(workflowsList);
        return crow::response(200, responseJson.dump());
    }

    crow::response WebServer::HandleWorkflowGet(std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id",
                                         "Workflow id contains invalid characters",
                                         "GET /api/workflows/{id}", workflowId);
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "GET /api/workflows/{id}", workflowId);
        }

        WorkflowRegistry workflowRegistry;
        if (!workflowRegistry.LoadDirectory(workflowsDirectoryAbsolute))
        {
            return MakeWorkflowJsonError(500, "workflow_registry_load_failed",
                                         "Failed to load workflows directory: " + workflowsDirectoryAbsolute.string(),
                                         "GET /api/workflows/{id}", workflowId);
        }

        std::optional<WorkflowDefinition> workflowDefinition = workflowRegistry.GetWorkflow(workflowId);
        if (!workflowDefinition.has_value())
        {
            return MakeWorkflowJsonError(404, "workflow_not_found",
                                         "Workflow not found",
                                         "GET /api/workflows/{id}", workflowId);
        }

        fs::path workflowFilePath = fs::path(workflowDefinition->m_WorkflowFilePath);
        if (workflowFilePath.empty())
        {
            return MakeWorkflowJsonError(500, "workflow_path_missing",
                                         "Workflow definition is missing workflow file path",
                                         "GET /api/workflows/{id}", workflowId);
        }

        if (workflowFilePath.is_relative())
        {
            workflowFilePath = (workflowsDirectoryAbsolute / workflowFilePath).lexically_normal();
        }

        std::string workflowJsonContent;
        if (!ReadTextFile(workflowFilePath, workflowJsonContent))
        {
            return MakeWorkflowJsonError(500, "workflow_read_failed",
                                         "Failed to read workflow file: " + workflowFilePath.string(),
                                         "GET /api/workflows/{id}", workflowId);
        }

        // Return the raw JCWF JSON as the response body (canonical).
        return crow::response(200, workflowJsonContent);
    }

    crow::response WebServer::HandleWorkflowsCreatePost(crow::request const& req)
    {
        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "POST /api/workflows");
        }

        WorkflowJsonParser workflowJsonParser;
        WorkflowDefinition parsedWorkflow;
        std::string parseErrorMessage;
        if (!workflowJsonParser.ParseWorkflowJson(req.body, parsedWorkflow, parseErrorMessage))
        {
            return MakeWorkflowJsonError(400, "invalid_jcwf", parseErrorMessage, "POST /api/workflows");
        }

        if (!IsValidWorkflowId(parsedWorkflow.m_Id))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id",
                                         "Parsed JCWF id contains invalid characters",
                                         "POST /api/workflows");
        }

        fs::path const targetPath = (workflowsDirectoryAbsolute / (parsedWorkflow.m_Id + ".jcwf")).lexically_normal();
        if (fs::exists(targetPath))
        {
            return MakeWorkflowJsonError(409, "workflow_already_exists",
                                         "Workflow file already exists: " + targetPath.string(),
                                         "POST /api/workflows", parsedWorkflow.m_Id);
        }

        std::string writeErrorMessage;
        if (!WriteTextFileAtomic(targetPath, req.body, writeErrorMessage))
        {
            return MakeWorkflowJsonError(500, "workflow_write_failed", writeErrorMessage, "POST /api/workflows",
                                         parsedWorkflow.m_Id);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["id"] = parsedWorkflow.m_Id;
        responseJson["savedPath"] = targetPath.string();
        return crow::response(201, responseJson.dump());
    }

    crow::response WebServer::HandleWorkflowUpdatePut(crow::request const& req, std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id",
                                         "Workflow id contains invalid characters",
                                         "PUT /api/workflows/{id}", workflowId);
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "PUT /api/workflows/{id}", workflowId);
        }

        WorkflowJsonParser workflowJsonParser;
        WorkflowDefinition parsedWorkflow;
        std::string parseErrorMessage;
        if (!workflowJsonParser.ParseWorkflowJson(req.body, parsedWorkflow, parseErrorMessage))
        {
            return MakeWorkflowJsonError(400, "invalid_jcwf", parseErrorMessage, "PUT /api/workflows/{id}", workflowId);
        }

        if (parsedWorkflow.m_Id != workflowId)
        {
            return MakeWorkflowJsonError(400, "workflow_id_mismatch",
                                         "URL workflow id does not match parsed JCWF id",
                                         "PUT /api/workflows/{id}", workflowId);
        }

        fs::path const targetPath = (workflowsDirectoryAbsolute / (workflowId + ".jcwf")).lexically_normal();
        if (!fs::exists(targetPath))
        {
            return MakeWorkflowJsonError(404, "workflow_not_found",
                                         "Workflow file does not exist: " + targetPath.string(),
                                         "PUT /api/workflows/{id}", workflowId);
        }

        std::string writeErrorMessage;
        if (!WriteTextFileAtomic(targetPath, req.body, writeErrorMessage))
        {
            return MakeWorkflowJsonError(500, "workflow_write_failed", writeErrorMessage, "PUT /api/workflows/{id}",
                                         workflowId);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["id"] = workflowId;
        responseJson["savedPath"] = targetPath.string();
        return crow::response(200, responseJson.dump());
    }

    crow::response WebServer::HandleWorkflowDelete(std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id",
                                         "Workflow id contains invalid characters",
                                         "DELETE /api/workflows/{id}", workflowId);
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "DELETE /api/workflows/{id}", workflowId);
        }

        WorkflowRegistry workflowRegistry;
        if (!workflowRegistry.LoadDirectory(workflowsDirectoryAbsolute))
        {
            return MakeWorkflowJsonError(500, "workflow_registry_load_failed",
                                         "Failed to load workflows directory: " + workflowsDirectoryAbsolute.string(),
                                         "DELETE /api/workflows/{id}", workflowId);
        }

        std::optional<WorkflowDefinition> workflowDefinition = workflowRegistry.GetWorkflow(workflowId);
        if (!workflowDefinition.has_value())
        {
            return MakeWorkflowJsonError(404, "workflow_not_found",
                                         "Workflow not found",
                                         "DELETE /api/workflows/{id}", workflowId);
        }

        fs::path workflowFilePath = fs::path(workflowDefinition->m_WorkflowFilePath);
        if (workflowFilePath.is_relative())
        {
            workflowFilePath = (workflowsDirectoryAbsolute / workflowFilePath).lexically_normal();
        }

        std::error_code errorCode;
        bool const removed = fs::remove(workflowFilePath, errorCode);
        if (errorCode)
        {
            return MakeWorkflowJsonError(500, "workflow_delete_failed",
                                         "Failed to delete workflow file: " + workflowFilePath.string() + " error=" +
                                             errorCode.message(),
                                         "DELETE /api/workflows/{id}", workflowId);
        }

        if (!removed)
        {
            return MakeWorkflowJsonError(404, "workflow_not_found",
                                         "Workflow file did not exist: " + workflowFilePath.string(),
                                         "DELETE /api/workflows/{id}", workflowId);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["id"] = workflowId;
        return crow::response(200, responseJson.dump());
    }



crow::response WebServer::HandleWorkflowValidatePost(crow::request const& req)
{
    WorkflowJsonParser workflowJsonParser;
    WorkflowDefinition parsedWorkflow;
    std::string parseErrorMessage;
    if (!workflowJsonParser.ParseWorkflowJson(req.body, parsedWorkflow, parseErrorMessage))
    {
        return MakeWorkflowJsonError(400, "invalid_jcwf", parseErrorMessage, "POST /api/workflows/validate");
    }

    crow::json::wvalue responseJson;
    responseJson["ok"] = true;
    responseJson["id"] = parsedWorkflow.m_Id;
    return crow::response(200, responseJson.dump());
}

crow::response WebServer::HandleWorkflowValidateGet(std::string const& workflowId)
{
    if (!IsValidWorkflowId(workflowId))
    {
        return MakeWorkflowJsonError(400, "invalid_workflow_id",
                                     "Workflow id contains invalid characters",
                                     "GET /api/workflows/{id}/validate", workflowId);
    }

    std::string errorMessage;
    fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
    if (workflowsDirectoryAbsolute.empty())
    {
        return MakeWorkflowJsonError(500, "config_error", errorMessage, "GET /api/workflows/{id}/validate",
                                     workflowId);
    }

    WorkflowRegistry workflowRegistry;
    if (!workflowRegistry.LoadDirectory(workflowsDirectoryAbsolute))
    {
        return MakeWorkflowJsonError(500, "workflow_registry_load_failed",
                                     "Failed to load workflows directory: " + workflowsDirectoryAbsolute.string(),
                                     "GET /api/workflows/{id}/validate", workflowId);
    }

    auto workflowDefinition = workflowRegistry.GetWorkflow(workflowId);
    if (!workflowDefinition.has_value())
    {
        return MakeWorkflowJsonError(404, "workflow_not_found",
                                     "Workflow not found: " + workflowId,
                                     "GET /api/workflows/{id}/validate", workflowId);
    }

    fs::path workflowFilePath = fs::path(workflowDefinition->m_WorkflowFilePath);
    if (workflowFilePath.is_relative())
    {
        workflowFilePath = (workflowsDirectoryAbsolute / workflowFilePath).lexically_normal();
    }

    std::string workflowJsonContent;
    if (!ReadTextFile(workflowFilePath, workflowJsonContent))
    {
        return MakeWorkflowJsonError(500, "workflow_read_failed",
                                     "Failed to read workflow file: " + workflowFilePath.string(),
                                     "GET /api/workflows/{id}/validate", workflowId);
    }

    WorkflowJsonParser workflowJsonParser;
    WorkflowDefinition parsedWorkflow;
    std::string parseErrorMessage;
    if (!workflowJsonParser.ParseWorkflowJson(workflowJsonContent, parsedWorkflow, parseErrorMessage))
    {
        return MakeWorkflowJsonError(400, "invalid_jcwf", parseErrorMessage, "GET /api/workflows/{id}/validate",
                                     workflowId);
    }

    crow::json::wvalue responseJson;
    responseJson["ok"] = true;
    responseJson["id"] = parsedWorkflow.m_Id;
    return crow::response(200, responseJson.dump());
}

crow::response WebServer::HandleWorkflowRunPost(std::string const& workflowId)
{
    if (!IsValidWorkflowId(workflowId))
    {
        return MakeWorkflowJsonError(400, "invalid_workflow_id",
                                     "Workflow id contains invalid characters",
                                     "POST /api/workflows/{id}/run", workflowId);
    }

    WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        workflowRuntimeManager = m_WorkflowRuntimeManager;
    }

    if (workflowRuntimeManager != nullptr)
    {
        workflowRuntimeManager->EnqueueWorkflowRun(workflowId);

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["enqueued"] = true;
        responseJson["id"] = workflowId;
        return crow::response(202, responseJson.dump());
    }

    std::string const runId = WorkflowOrchestrator::Get().StartWorkflowRun(workflowId);

    crow::json::wvalue responseJson;
    responseJson["ok"] = true;
    responseJson["enqueued"] = false;
    responseJson["id"] = workflowId;
    responseJson["runId"] = runId;
    return crow::response(202, responseJson.dump());
}

crow::response WebServer::HandleWorkflowRunsActiveGet()
{
    WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        workflowRuntimeManager = m_WorkflowRuntimeManager;
    }

    if (workflowRuntimeManager == nullptr)
    {
        return MakeWorkflowJsonError(501, "not_configured",
                                     "Workflow runtime manager not configured on web server",
                                     "GET /api/workflow-runs/active");
    }

    auto activeRuns = workflowRuntimeManager->GetActiveRunsSnapshot();

    crow::json::wvalue responseJson;
    responseJson["ok"] = true;
    crow::json::wvalue runsJson = crow::json::wvalue::list();
    for (auto const& run : activeRuns)
    {
        crow::json::wvalue runJson;
        runJson["runId"] = run.m_RunId;
        runJson["workflowId"] = run.m_WorkflowId;
        runJson["state"] = ToStringWorkflowRunState(run.m_State);
        runJson["startedAt"] = run.m_StartedAtIso8601;
        runJson["completedAt"] = run.m_CompletedAtIso8601;
        runJson["taskCount"] = static_cast<int64_t>(run.m_TaskStates.size());
        runsJson.emplace_back(std::move(runJson));
    }
    responseJson["runs"] = std::move(runsJson);

    return crow::response(200, responseJson.dump());
}

crow::response WebServer::HandleWorkflowRunsLastGet()
{
    WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        workflowRuntimeManager = m_WorkflowRuntimeManager;
    }

    if (workflowRuntimeManager == nullptr)
    {
        return MakeWorkflowJsonError(501, "not_configured",
                                     "Workflow runtime manager not configured on web server",
                                     "GET /api/workflow-runs/last");
    }

    auto lastRuns = workflowRuntimeManager->GetLastRunsSnapshot();

    crow::json::wvalue responseJson;
    responseJson["ok"] = true;

    crow::json::wvalue runsJson = crow::json::wvalue::list();
    for (auto const& [workflowId, run] : lastRuns)
    {
        crow::json::wvalue runJson;
        runJson["runId"] = run.m_RunId;
        runJson["workflowId"] = workflowId;
        runJson["state"] = ToStringWorkflowRunState(run.m_State);
        runJson["startedAt"] = run.m_StartedAtIso8601;
        runJson["completedAt"] = run.m_CompletedAtIso8601;
        runJson["taskCount"] = static_cast<int64_t>(run.m_TaskStates.size());
        runsJson.emplace_back(std::move(runJson));
    }
    responseJson["runs"] = std::move(runsJson);

    return crow::response(200, responseJson.dump());
}

crow::response WebServer::HandleWorkflowRunCancelPost(std::string const& runId)
{
    (void)runId;
    return MakeWorkflowJsonError(501, "not_implemented",
                                 "Run cancel is not implemented yet",
                                 "POST /api/workflow-runs/{runId}/cancel");
}

void WebServer::RegisterWebSocket()
    {
        CROW_WEBSOCKET_ROUTE(m_Server, "/ws")
            .onopen(
                [this](crow::websocket::connection& conn)
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Clients.insert(&conn);
                    LOG_APP_INFO("WebSocket client connected");
                })
            .onclose(
                [this](crow::websocket::connection& conn, const std::string& reason, uint16_t code)
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Clients.erase(&conn);
                    LOG_APP_INFO("WebSocket client disconnected ({}, code {})", reason, code);
                })
            .onmessage(
                [this](crow::websocket::connection& conn, const std::string& data, bool /*is_binary*/)
                {
                    try
                    {
                        simdjson::ondemand::parser parser;
                        simdjson::padded_string json(data);
                        auto doc = parser.iterate(json);

                        std::string type = std::string(doc["type"].get_string().value());

                        if (type == "chat")
                        {
                            std::string subsystem = std::string(doc["subsystem"].get_string().value());
                            std::string text = std::string(doc["message"].get_string().value());

                            // add to chat message pool
                            uint64_t id = App::g_App->GetChatMessagePool()->AddMessage(subsystem, text);
                            auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();

                            // determine queue directory
                            fs::path queuePath = fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath) / subsystem;
                            fs::create_directories(queuePath);

                            // write the PROB_<id>_<timestamp>.txt file
                            fs::path filename =
                                queuePath / ("PROB_" + std::to_string(id) + "_" + std::to_string(timestamp) + ".txt");

                            std::ofstream out(filename);
                            out << text;

                            // respond to browser
                            crow::json::wvalue response;
                            response["type"] = "queued";
                            response["id"] = id; // <-- RETURN UNIQUE ID
                            response["file"] = filename.string();
                            conn.send_text(response.dump());
                        }
                        
else if (type == "workflow-runs-request")
{
    WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        workflowRuntimeManager = m_WorkflowRuntimeManager;
    }

    crow::json::wvalue msg;
    msg["type"] = "workflow-runs-snapshot";

    if (workflowRuntimeManager != nullptr)
    {
        auto activeRuns = workflowRuntimeManager->GetActiveRunsSnapshot();
        crow::json::wvalue activeRunsJson = crow::json::wvalue::list();
        for (auto const& run : activeRuns)
        {
            crow::json::wvalue runJson;
            runJson["runId"] = run.m_RunId;
            runJson["workflowId"] = run.m_WorkflowId;
            runJson["state"] = ToStringWorkflowRunState(run.m_State);
            runJson["startedAt"] = run.m_StartedAtIso8601;
            runJson["completedAt"] = run.m_CompletedAtIso8601;
            activeRunsJson.emplace_back(std::move(runJson));
        }
        msg["activeRuns"] = std::move(activeRunsJson);
    }
    else
    {
        msg["activeRuns"] = crow::json::wvalue::list();
        msg["warning"] = "workflow runtime manager not configured";
    }

    conn.send_text(msg.dump());
}
else if (type == "quit")
                        {
                            auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                            Core::g_Core->PushEvent(event);

                            crow::json::wvalue response;
                            response["type"] = "quit-ack";
                            response["message"] = "Shutdown initiated.";
                            conn.send_text(response.dump());

                            return;
                        }

                        else
                        {
                            conn.send_text(R"({"error":"unknown type"})");
                        }
                    }
                    catch (const std::exception& e)
                    {
                        crow::json::wvalue error;
                        error["error"] = e.what();
                        conn.send_text(error.dump());
                    }
                });
    }

    void WebServer::Start()
    {
        if (m_Running)
        {
            return;
        }

        m_Running = true;
        m_ServerTask = Core::g_Core->GetThreadPool().SubmitTask(
            [this]()
            {
                LOG_APP_INFO("Crow web server started at http://localhost:8080");
                m_Server.port(8080).multithreaded().signal_clear().run();
            });
    }

    void WebServer::Stop()
    {
        if (!m_Running)
        {
            return;
        }

        m_Running = false;
        m_Server.stop();
        if (m_ServerTask.valid())
        {
            m_ServerTask.wait();
            LOG_APP_INFO("Crow web server stopped");
        }
    }

    void WebServer::Broadcast(const std::string& jsonMessage)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto* client : m_Clients)
        {
            client->send_text(jsonMessage);
        }
    }

    void WebServer::BroadcastJSON(std::string const& jsonString)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto* client : m_Clients)
        {
            client->send_text(jsonString);
        }
    }

    void WebServer::BroadcastPythonStatus(bool pythonRunning)
    {
        crow::json::wvalue msg;
        msg["type"] = "python-status";
        msg["running"] = pythonRunning;

        BroadcastJSON(msg.dump());
    }

} // namespace AIAssistant