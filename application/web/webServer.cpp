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
#include <string_view>
#include <optional>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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

        
        struct WorkflowValidationFinding
        {
            std::string m_Code;
            std::string m_Message;
        };

        crow::json::wvalue MakeWorkflowValidationResponse(bool const ok,
                                                         std::string const& workflowId,
                                                         std::vector<WorkflowValidationFinding> const& errors,
                                                         std::vector<WorkflowValidationFinding> const& warnings)
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = ok;
            responseJson["id"] = workflowId;

            crow::json::wvalue::list errorsList;
for (auto const& error : errors)
{
    crow::json::wvalue item;
    item["code"] = error.m_Code;
    item["message"] = error.m_Message;
    errorsList.push_back(std::move(item));
}
responseJson["errors"] = std::move(errorsList);

crow::json::wvalue::list warningsList;
for (auto const& warning : warnings)
{
    crow::json::wvalue item;
    item["code"] = warning.m_Code;
    item["message"] = warning.m_Message;
    warningsList.push_back(std::move(item));
}
responseJson["warnings"] = std::move(warningsList);


            return responseJson;
        }

        enum class DfsState
        {
            NotVisited,
            Visiting,
            Visited
        };

        void DetectCyclesDfs(std::string const& nodeId,
                             std::unordered_map<std::string, std::vector<std::string>> const& adjacency,
                             std::unordered_map<std::string, DfsState>& states,
                             std::unordered_set<std::string>& cycleNodes)
        {
            auto const stateIt = states.find(nodeId);
            if (stateIt != states.end() && stateIt->second == DfsState::Visiting)
            {
                cycleNodes.insert(nodeId);
                return;
            }

            if (stateIt != states.end() && stateIt->second == DfsState::Visited)
            {
                return;
            }

            states[nodeId] = DfsState::Visiting;

            auto const adjacencyIt = adjacency.find(nodeId);
            if (adjacencyIt != adjacency.end())
            {
                for (auto const& nextId : adjacencyIt->second)
                {
                    DetectCyclesDfs(nextId, adjacency, states, cycleNodes);
                }
            }

            states[nodeId] = DfsState::Visited;
        }

        void ValidateJcwfJson(std::string const& workflowJsonText,
                              std::vector<WorkflowValidationFinding>& errors,
                              std::vector<WorkflowValidationFinding>& warnings,
                              std::string& workflowIdOut)
        {
            simdjson::ondemand::parser parser;

            simdjson::padded_string json(workflowJsonText);
            auto doc = parser.iterate(json);

            // version
            std::string version;
            {
                auto versionField = doc["version"];
                if (versionField.error() != simdjson::SUCCESS)
                {
                    errors.push_back({"missing_version", "Missing required field: version"});
                }
                else
                {
                    auto versionStr = versionField.get_string();
                    if (versionStr.error() != simdjson::SUCCESS)
                    {
                        errors.push_back({"invalid_version", "Field 'version' must be a string"});
                    }
                    else
                    {
                        version = std::string(versionStr.value());
                        if (version != "1.0")
                        {
                            warnings.push_back({"unexpected_version", "Expected version '1.0' (got '" + version + "')"});
                        }
                    }
                }
            }

            // id
            {
                auto idField = doc["id"];
                if (idField.error() != simdjson::SUCCESS)
                {
                    errors.push_back({"missing_id", "Missing required field: id"});
                    workflowIdOut = "";
                }
                else
                {
                    auto idStr = idField.get_string();
                    if (idStr.error() != simdjson::SUCCESS)
                    {
                        errors.push_back({"invalid_id", "Field 'id' must be a string"});
                        workflowIdOut = "";
                    }
                    else
                    {
                        workflowIdOut = std::string(idStr.value());
                    }
                }
            }

            // tasks
            std::unordered_set<std::string> taskIds;
            std::unordered_map<std::string, std::vector<std::string>> adjacency;
            {
                auto tasksField = doc["tasks"];
                if (tasksField.error() != simdjson::SUCCESS)
                {
                    errors.push_back({"missing_tasks", "Missing required field: tasks"});
                    return;
                }

                auto tasksObject = tasksField.get_object();
                if (tasksObject.error() != simdjson::SUCCESS)
                {
                    errors.push_back({"invalid_tasks", "Field 'tasks' must be an object (dictionary)"});
                    return;
                }

                // Collect IDs first
                for (auto taskEntry : tasksObject.value())
                {
                                        std::string_view taskKeyView;
                    {
                        simdjson::simdjson_result<std::string_view> keyResult = taskEntry.unescaped_key();
                        if (keyResult.error() != simdjson::SUCCESS)
                        {
                            errors.push_back({"invalid_task_key", "Invalid task key (object field name)"});
                            continue;
                        }
                        taskKeyView = keyResult.value();
                    }
                    std::string const taskKey(taskKeyView);
                    taskIds.insert(taskKey);
                    adjacency[taskKey] = {};
                }

                // Validate each task + build adjacency
                for (auto taskEntry : tasksObject.value())
                {
                                        std::string_view taskKeyView;
                    {
                        simdjson::simdjson_result<std::string_view> keyResult = taskEntry.unescaped_key();
                        if (keyResult.error() != simdjson::SUCCESS)
                        {
                            errors.push_back({"invalid_task_key", "Invalid task key (object field name)"});
                            continue;
                        }
                        taskKeyView = keyResult.value();
                    }
                    std::string const taskKey(taskKeyView);
                    auto taskObj = taskEntry.value().get_object();
                    if (taskObj.error() != simdjson::SUCCESS)
                    {
                        errors.push_back({"invalid_task", "Task '" + taskKey + "' must be an object"});
                        continue;
                    }

                    // task.type (required)
                    bool hasType = false;
                    {
                        auto typeField = taskEntry.value()["type"];
                        if (typeField.error() != simdjson::SUCCESS)
                        {
                            errors.push_back({"missing_task_type", "Task '" + taskKey + "': missing required field 'type'"});
                        }
                        else
                        {
                            auto typeStr = typeField.get_string();
                            if (typeStr.error() != simdjson::SUCCESS)
                            {
                                errors.push_back({"invalid_task_type", "Task '" + taskKey + "': field 'type' must be a string"});
                            }
                            else
                            {
                                hasType = true;
                            }
                        }
                    }

                    (void)hasType;

                    // task.working_directory (optional but recommended)
                    {
                        auto wdField = taskEntry.value()["working_directory"];
                        if (wdField.error() == simdjson::SUCCESS)
                        {
                            auto wdStr = wdField.get_string();
                            if (wdStr.error() != simdjson::SUCCESS)
                            {
                                errors.push_back({"invalid_working_directory", "Task '" + taskKey + "': 'working_directory' must be a string"});
                            }
                        }
                        else
                        {
                            warnings.push_back({"missing_working_directory", "Task '" + taskKey + "': missing 'working_directory' (recommended)"});
                        }
                    }

                    // task.
                    // task.label (optional string)
                    {
                        auto labelField = taskEntry.value()["label"];
                        if (labelField.error() == simdjson::SUCCESS)
                        {
                            auto labelStr = labelField.get_string();
                            if (labelStr.error() != simdjson::SUCCESS)
                            {
                                errors.push_back({"invalid_task_label", "Task '" + taskKey + "': field 'label' must be a string"});
                            }
                        }
                    }

                    // task.doc (optional string)
                    {
                        auto docField = taskEntry.value()["doc"];
                        if (docField.error() == simdjson::SUCCESS)
                        {
                            auto docStr = docField.get_string();
                            if (docStr.error() != simdjson::SUCCESS)
                            {
                                errors.push_back({"invalid_task_doc", "Task '" + taskKey + "': field 'doc' must be a string"});
                            }
                        }
                    }

                    // task.working_directory (optional string)
                    {
                        auto workingDirectoryField = taskEntry.value()["working_directory"];
                        if (workingDirectoryField.error() == simdjson::SUCCESS)
                        {
                            auto workingDirectoryStr = workingDirectoryField.get_string();
                            if (workingDirectoryStr.error() != simdjson::SUCCESS)
                            {
                                errors.push_back({"invalid_task_working_directory",
                                                  "Task '" + taskKey + "': field 'working_directory' must be a string"});
                            }
                        }
                    }

                    // task.params (optional object)
                    {
                        auto paramsField = taskEntry.value()["params"];
                        if (paramsField.error() == simdjson::SUCCESS)
                        {
                            auto paramsObj = paramsField.get_object();
                            if (paramsObj.error() != simdjson::SUCCESS)
                            {
                                errors.push_back({"invalid_task_params", "Task '" + taskKey + "': field 'params' must be an object"});
                            }
                        }
                    }

                    // task.depends_on (optional array of strings)
                    {
                        auto dependsField = taskEntry.value()["depends_on"];
                        if (dependsField.error() == simdjson::SUCCESS)
                        {
                            auto dependsArray = dependsField.get_array();
                            if (dependsArray.error() != simdjson::SUCCESS)
                            {
                                errors.push_back({"invalid_depends_on", "Task '" + taskKey + "': 'depends_on' must be an array of strings"});
                            }
                            else
                            {
                                std::unordered_set<std::string> dependencyIds;

                                for (auto depValue : dependsArray.value())
                                {
                                    auto depStr = depValue.get_string();
                                    if (depStr.error() != simdjson::SUCCESS)
                                    {
                                        errors.push_back({"invalid_depends_on", "Task '" + taskKey + "': 'depends_on' must contain strings only"});
                                        continue;
                                    }

                                    std::string const depId = std::string(depStr.value());
                                    if (depId.empty())
                                    {
                                        errors.push_back({"invalid_dependency", "Task '" + taskKey + "': depends_on contains an empty task id"});
                                        continue;
                                    }

                                    if (depId == taskKey)
                                    {
                                        errors.push_back({"self_dependency", "Task '" + taskKey + "': depends_on must not include itself"});
                                        continue;
                                    }

                                    if (dependencyIds.find(depId) != dependencyIds.end())
                                    {
                                        warnings.push_back({"duplicate_dependency",
                                                            "Task '" + taskKey + "': depends_on contains duplicate entry '" + depId + "'"});
                                        continue;
                                    }
                                    dependencyIds.insert(depId);

                                    if (taskIds.find(depId) == taskIds.end())
                                    {
                                        errors.push_back({"unknown_dependency", "Task '" + taskKey + "': depends_on references unknown task '" + depId + "'"});
                                    }
                                    else
                                    {
                                        // depId -> taskKey
                                        adjacency[depId].push_back(taskKey);
                                    }
                                }
                            }
                        }
                    }

                    // task.id mismatch (warning)
                    {
                        auto idField = taskEntry.value()["id"];
                        if (idField.error() == simdjson::SUCCESS)
                        {
                            auto idStr = idField.get_string();
                            if (idStr.error() == simdjson::SUCCESS)
                            {
                                std::string const embeddedId = std::string(idStr.value());
                                if (embeddedId != taskKey)
                                {
                                    warnings.push_back({"task_id_mismatch",
                                                        "Task key '" + taskKey + "' does not match task.id '" + embeddedId + "'"});
                                }
                            }
                        }
                    }
                }
            }

            // Cycle detection (directed adjacency)
            {
                std::unordered_map<std::string, DfsState> states;
                std::unordered_set<std::string> cycleNodes;
                for (auto const& taskId : taskIds)
                {
                    DetectCyclesDfs(taskId, adjacency, states, cycleNodes);
                }

                for (auto const& nodeId : cycleNodes)
                {
                    errors.push_back({"cycle_detected", "Cycle detected involving task '" + nodeId + "'"});
                }
            }
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
    crow::json::wvalue::list activeRunsJson;
    for (auto const& run : activeRuns)
    {
        crow::json::wvalue runJson;
        runJson["runId"] = run.m_RunId;
        runJson["workflowId"] = run.m_WorkflowId;
        runJson["state"] = ToStringWorkflowRunState(run.m_State);
        runJson["startedAt"] = run.m_StartedAtIso8601;
        runJson["completedAt"] = run.m_CompletedAtIso8601;
        activeRunsJson.push_back(std::move(runJson));
    }
    msg["activeRuns"] = std::move(activeRunsJson);

    BroadcastJSON(msg.dump());
}


    
namespace
{
    std::string GetMimeType(std::filesystem::path const& path)
    {
        std::string const ext = path.extension().string();
        if (ext == ".html")
        {
            return "text/html; charset=utf-8";
        }
        if (ext == ".js")
        {
            return "application/javascript; charset=utf-8";
        }
        if (ext == ".css")
        {
            return "text/css; charset=utf-8";
        }
        if (ext == ".json")
        {
            return "application/json; charset=utf-8";
        }
        if (ext == ".svg")
        {
            return "image/svg+xml";
        }
        if (ext == ".png")
        {
            return "image/png";
        }
        if (ext == ".jpg" || ext == ".jpeg")
        {
            return "image/jpeg";
        }
        if (ext == ".webp")
        {
            return "image/webp";
        }
        if (ext == ".woff2")
        {
            return "font/woff2";
        }
        if (ext == ".woff")
        {
            return "font/woff";
        }
        if (ext == ".ttf")
        {
            return "font/ttf";
        }
        return "application/octet-stream";
    }

    bool TryReadBinaryFile(std::filesystem::path const& filePath, std::string& outContent)
    {
        std::ifstream file(filePath, std::ios::in | std::ios::binary);
        if (!file)
        {
            return false;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        outContent = buffer.str();
        return true;
    }
} // namespace

crow::response WebServer::ServeStaticFile(std::filesystem::path const& filePath) const
{
    std::string content;
    if (!TryReadBinaryFile(filePath, content))
    {
        return crow::response(404, "File not found");
    }

    crow::response response(200);
    response.set_header("Content-Type", GetMimeType(filePath));
    response.body = std::move(content);
    return response;
}

crow::response WebServer::ServeWorkflowEditorIndex() const
{
    std::filesystem::path const distIndex = std::filesystem::path("workflow-editor") / "ui" / "dist" / "index.html";
    if (!std::filesystem::exists(distIndex))
    {
        return crow::response(
            500,
            "Workflow Editor UI build not found. Please run: cd workflow-editor/ui && npm run build");
    }

    return ServeStaticFile(distIndex);
}

crow::response WebServer::ServeWorkflowEditorStatic(std::string const& requestPath) const
{
    std::filesystem::path const distRoot = std::filesystem::path("workflow-editor") / "ui" / "dist";

    if (requestPath == "/editor" || requestPath == "/editor/")
    {
        return ServeWorkflowEditorIndex();
    }

    // Serve assets from dist under two possible URL layouts:
    //  - "/assets/..." (Vite default when base is "/")
    //  - "/editor/assets/..." (if base is later set to "/editor/")
    if (requestPath.rfind("/assets/", 0) == 0)
    {
        std::string const relative = requestPath.substr(std::string("/assets/").size());
        return ServeStaticFile(distRoot / "assets" / relative);
    }
    if (requestPath.rfind("/editor/assets/", 0) == 0)
    {
        std::string const relative = requestPath.substr(std::string("/editor/assets/").size());
        return ServeStaticFile(distRoot / "assets" / relative);
    }

    // SPA fallback: any /editor/* route should serve index.html
    if (requestPath.rfind("/editor/", 0) == 0)
    {
        return ServeWorkflowEditorIndex();
    }

    return crow::response(404, "Not found");
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

        

// ---- Workflow Editor UI (React) ----
// Serves the production build from: workflow-editor/ui/dist
CROW_ROUTE(m_Server, "/editor")([this]() { return ServeWorkflowEditorIndex(); });

// Vite default asset paths are rooted at "/assets/...".
CROW_ROUTE(m_Server, "/assets/<path>")([this](std::string const& path)
    {
        return ServeWorkflowEditorStatic(std::string("/assets/") + path);
    });

// SPA fallback for any sub-route under /editor (e.g. /editor/workflows/...)
CROW_ROUTE(m_Server, "/editor/<path>")([this](std::string const& path)
    {
        return ServeWorkflowEditorStatic(std::string("/editor/") + path);
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

CROW_ROUTE(m_Server, "/api/workflow-runs/<string>")
    .methods("GET"_method)([this](std::string const& runId) { return HandleWorkflowRunGet(runId); });

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
    std::vector<WorkflowValidationFinding> errors;
    std::vector<WorkflowValidationFinding> warnings;
    std::string workflowId;

    try
    {
        ValidateJcwfJson(req.body, errors, warnings, workflowId);
    }
    catch (simdjson::simdjson_error const& error)
    {
        return MakeWorkflowJsonError(400, "invalid_jcwf",
                                     std::string("Invalid JSON: ") + error.what(),
                                     "POST /api/workflows/validate");
    }
    catch (std::exception const& error)
    {
        return MakeWorkflowJsonError(400, "invalid_jcwf",
                                     std::string("Invalid JSON: ") + error.what(),
                                     "POST /api/workflows/validate");
    }

    bool const ok = errors.empty();
    crow::json::wvalue responseJson = MakeWorkflowValidationResponse(ok, workflowId, errors, warnings);
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

    WorkflowRegistry const* workflowRegistryPtr = nullptr;
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        workflowRegistryPtr = m_WorkflowRegistry;
    }

    std::optional<WorkflowDefinition> workflowDefinition;

    if (workflowRegistryPtr != nullptr)
    {
        workflowDefinition = workflowRegistryPtr->GetWorkflow(workflowId);
    }
    else
    {
        WorkflowRegistry workflowRegistry;
        if (!workflowRegistry.LoadDirectory(workflowsDirectoryAbsolute))
        {
            return MakeWorkflowJsonError(500, "workflow_registry_load_failed",
                                         "Failed to load workflows directory: " + workflowsDirectoryAbsolute.string(),
                                         "GET /api/workflows/{id}/validate", workflowId);
        }

        workflowDefinition = workflowRegistry.GetWorkflow(workflowId);
    }
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

    std::vector<WorkflowValidationFinding> errors;
    std::vector<WorkflowValidationFinding> warnings;
    std::string parsedWorkflowId;

    try
    {
        ValidateJcwfJson(workflowJsonContent, errors, warnings, parsedWorkflowId);
    }
    catch (simdjson::simdjson_error const& error)
    {
        return MakeWorkflowJsonError(400, "invalid_jcwf",
                                     std::string("Invalid JSON: ") + error.what(),
                                     "GET /api/workflows/{id}/validate", workflowId);
    }
    catch (std::exception const& error)
    {
        return MakeWorkflowJsonError(400, "invalid_jcwf",
                                     std::string("Invalid JSON: ") + error.what(),
                                     "GET /api/workflows/{id}/validate", workflowId);
    }

    if (!parsedWorkflowId.empty() && parsedWorkflowId != workflowId)
    {
        warnings.push_back({"workflow_id_mismatch",
                            "Workflow id in file ('" + parsedWorkflowId + "') does not match requested id ('" + workflowId + "')"});
    }

    bool const ok = errors.empty();
    crow::json::wvalue responseJson = MakeWorkflowValidationResponse(ok,
                                                                    parsedWorkflowId.empty() ? workflowId : parsedWorkflowId,
                                                                    errors,
                                                                    warnings);
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
    crow::json::wvalue::list runsJson;
    for (auto const& run : activeRuns)
    {
        crow::json::wvalue runJson;
        runJson["runId"] = run.m_RunId;
        runJson["workflowId"] = run.m_WorkflowId;
        runJson["state"] = ToStringWorkflowRunState(run.m_State);
        runJson["startedAt"] = run.m_StartedAtIso8601;
        runJson["completedAt"] = run.m_CompletedAtIso8601;
        runJson["taskCount"] = static_cast<int64_t>(run.m_TaskStates.size());
        runsJson.push_back(std::move(runJson));
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

    crow::json::wvalue::list runsJson;
    for (auto const& [workflowId, run] : lastRuns)
    {
        crow::json::wvalue runJson;
        runJson["runId"] = run.m_RunId;
        runJson["workflowId"] = workflowId;
        runJson["state"] = ToStringWorkflowRunState(run.m_State);
        runJson["startedAt"] = run.m_StartedAtIso8601;
        runJson["completedAt"] = run.m_CompletedAtIso8601;
        runJson["taskCount"] = static_cast<int64_t>(run.m_TaskStates.size());
        runsJson.push_back(std::move(runJson));
    }
    responseJson["runs"] = std::move(runsJson);

    return crow::response(200, responseJson.dump());
}

crow::response WebServer::HandleWorkflowRunGet(std::string const& runId)
{
    if (runId.empty())
    {
        return MakeWorkflowJsonError(400, "invalid_run_id",
                                     "Run id is empty",
                                     "GET /api/workflow-runs/{runId}");
    }

    WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        workflowRuntimeManager = m_WorkflowRuntimeManager;
    }

    if (workflowRuntimeManager == nullptr)
    {
        return MakeWorkflowJsonError(501, "not_configured",
                                     "Workflow runtime manager not configured on web server",
                                     "GET /api/workflow-runs/{runId}");
    }

    WorkflowRun run;
    if (!workflowRuntimeManager->TryGetRunById(runId, run))
    {
        return MakeWorkflowJsonError(404, "run_not_found",
                                     "Run not found: " + runId,
                                     "GET /api/workflow-runs/{runId}", runId);
    }

    crow::json::wvalue responseJson;
    responseJson["ok"] = true;

    crow::json::wvalue runJson;
    runJson["runId"] = run.m_RunId;
    runJson["workflowId"] = run.m_WorkflowId;
    runJson["state"] = ToStringWorkflowRunState(run.m_State);
    runJson["startedAt"] = run.m_StartedAtIso8601;
    runJson["completedAt"] = run.m_CompletedAtIso8601;

    crow::json::wvalue::list tasksJson;
    tasksJson.reserve(run.m_TaskStates.size());

    for (auto const& taskPair : run.m_TaskStates)
    {
        std::string const& taskId = taskPair.first;
        TaskInstanceState const& taskState = taskPair.second;

        crow::json::wvalue taskJson;
        taskJson["taskId"] = taskId;
        taskJson["state"] = ToStringTaskInstanceStateKind(taskState.m_State);
        taskJson["attemptCount"] = static_cast<int64_t>(taskState.m_AttemptCount);
        taskJson["startedAt"] = taskState.m_StartedAtIso8601;
        taskJson["completedAt"] = taskState.m_CompletedAtIso8601;

        if (!taskState.m_LastErrorMessage.empty())
        {
            taskJson["error"] = taskState.m_LastErrorMessage;
        }

        tasksJson.push_back(std::move(taskJson));
    }

    runJson["tasks"] = std::move(tasksJson);
    responseJson["run"] = std::move(runJson);

    return crow::response(200, responseJson.dump());
}

crow::response WebServer::HandleWorkflowRunCancelPost(std::string const& runId)
{
if (runId.empty())
{
    return MakeWorkflowJsonError(400, "invalid_run_id",
                                 "Run id is empty",
                                 "POST /api/workflow-runs/{runId}/cancel");
}

WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);
    workflowRuntimeManager = m_WorkflowRuntimeManager;
}

if (workflowRuntimeManager == nullptr)
{
    return MakeWorkflowJsonError(501, "not_configured",
                                 "Workflow runtime manager not configured on web server",
                                 "POST /api/workflow-runs/{runId}/cancel", runId);
}

bool const cancelRequested = workflowRuntimeManager->RequestCancelRun(runId);
if (!cancelRequested)
{
    return MakeWorkflowJsonError(404, "run_not_found",
                                 "Run not found or not active: " + runId,
                                 "POST /api/workflow-runs/{runId}/cancel", runId);
}

// Best-effort: push an updated snapshot to any connected editor clients.
BroadcastWorkflowRunsSnapshot();

crow::json::wvalue responseJson;
responseJson["ok"] = true;
responseJson["cancelRequested"] = true;
responseJson["runId"] = runId;

return crow::response(202, responseJson.dump());

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
        crow::json::wvalue::list activeRunsJson;
        for (auto const& run : activeRuns)
        {
            crow::json::wvalue runJson;
            runJson["runId"] = run.m_RunId;
            runJson["workflowId"] = run.m_WorkflowId;
            runJson["state"] = ToStringWorkflowRunState(run.m_State);
            runJson["startedAt"] = run.m_StartedAtIso8601;
            runJson["completedAt"] = run.m_CompletedAtIso8601;
            activeRunsJson.push_back(std::move(runJson));
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