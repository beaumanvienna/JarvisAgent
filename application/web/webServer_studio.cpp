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

// Studio-edition WebServer methods.  Compile-excluded from the Engine binary
// by `removefiles` in premake5.lua; an additional `#ifdef J9T_STUDIO` wraps
// the whole file as a defence-in-depth backstop in case the premake gating
// is ever bypassed.

#ifdef J9T_STUDIO

#include <chrono>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core.h"
#include "engine.h"
#include "web/webServer.h"
#include "web/webServer_helpers.h"
#include "workflow/jcwfContainer.h"
#include "workflow/workflowJsonParser.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowTypes.h"
#include "workflow/workflowValidator.h"
#include "file/scriptRegistry.h"
#include "workflow/taskPathResolver.h"
#include "jarvisAgent.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    using namespace WebServerHelpers;

    crow::response WebServer::ServeWorkflowEditorIndex() const
    {
        std::filesystem::path const distIndex = std::filesystem::path("workflow-editor") / "ui" / "dist" / "index.html";
        if (!std::filesystem::exists(distIndex))
        {
            return crow::response(
                500,
                "Workflow Editor UI build not found. Please run: cd workflow-editor/ui && npm install && npm run build");
        }

        return ServeStaticFile(distIndex);
    }

    crow::response WebServer::ServeWorkflowEditorStatic(std::string const& requestPath) const
    {
        std::filesystem::path const distRoot = std::filesystem::path("workflow-editor") / "ui" / "dist";
        std::filesystem::path const assetsRoot = distRoot / "assets";

        if (requestPath == "/editor" || requestPath == "/editor/")
        {
            return ServeWorkflowEditorIndex();
        }

        // Serve assets from dist under two possible URL layouts:
        //  - "/assets/..." (Vite default when base is "/")
        //  - "/editor/assets/..." (if base is later set to "/editor/")
        // ConfinePathUnder rejects `..` traversal and absolute paths, so a
        // request like `/assets/../../etc/passwd` cannot escape assetsRoot.
        std::string relative;
        if (requestPath.rfind("/assets/", 0) == 0)
        {
            relative = requestPath.substr(std::string("/assets/").size());
        }
        else if (requestPath.rfind("/editor/assets/", 0) == 0)
        {
            relative = requestPath.substr(std::string("/editor/assets/").size());
        }

        if (!relative.empty())
        {
            std::filesystem::path const resolved = ConfinePathUnder(assetsRoot, relative);
            if (resolved.empty())
            {
                LOG_SECURITY_WARN("[security] editor_static_path_escape len={}", relative.size());
                return crow::response(400, "Bad Request");
            }
            return ServeStaticFile(resolved);
        }

        // SPA fallback: any /editor/* route should serve index.html
        if (requestPath.rfind("/editor/", 0) == 0)
        {
            return ServeWorkflowEditorIndex();
        }

        return crow::response(404, "Not found");
    }

    void WebServer::RegisterStudioRoutes()
    {
        // ---- Workflow Editor UI (React SPA) — anonymous shell, page-level auth via dashboard ----
        CROW_ROUTE(m_Server, "/editor")([this]() { return ServeWorkflowEditorIndex(); });
        CROW_ROUTE(m_Server, "/assets/<path>")
        ([this](std::string const& path) { return ServeWorkflowEditorStatic("/assets/" + path); });
        CROW_ROUTE(m_Server, "/editor/<path>")
        ([this](std::string const& path) { return ServeWorkflowEditorStatic("/editor/" + path); });

        // ---- Admin: workflow CRUD (editor-only mutations) ----
        CROW_ROUTE(m_Server, "/api/workflows")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowsCreatePost(req);
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("PUT"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowUpdatePut(req, workflowId);
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("DELETE"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowDelete(workflowId);
                });

        // ---- Admin: workflow validation (editor-only) ----
        CROW_ROUTE(m_Server, "/api/workflows/validate")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowValidatePost(req);
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/validate")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowValidateGet(workflowId);
                });

        // ---- Viewer+: editor support endpoints ----
        CROW_ROUTE(m_Server, "/api/scripts/check")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleScriptCheckGet(req);
                });

        CROW_ROUTE(m_Server, "/api/scripts/registry")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleScriptRegistryGet();
                });

        CROW_ROUTE(m_Server, "/api/files/check")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleFileCheckGet(req);
                });
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
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Parsed JCWF id contains invalid characters",
                                         "POST /api/workflows");
        }

        fs::path const targetPath = (workflowsDirectoryAbsolute / (parsedWorkflow.m_Id + ".jcwf")).lexically_normal();
        if (fs::exists(targetPath))
        {
            return MakeWorkflowJsonError(409, "workflow_already_exists",
                                         "Workflow file already exists: " + targetPath.string(), "POST /api/workflows",
                                         parsedWorkflow.m_Id);
        }

        // Create the .jcwf zip container via the registry (handles extracted dir + pack).
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                std::string upsertErrorMessage;
                if (!m_WorkflowRegistry->SaveOrUpdateWorkflowFromJson(req.body, targetPath, upsertErrorMessage))
                {
                    return MakeWorkflowJsonError(500, "workflow_write_failed", upsertErrorMessage, "POST /api/workflows",
                                                 parsedWorkflow.m_Id);
                }
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["id"] = parsedWorkflow.m_Id;
        responseJson["savedPath"] = targetPath.string();
        return MakeJsonResponse(201, responseJson);
    }

    crow::response WebServer::HandleWorkflowUpdatePut(crow::request const& req, std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
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
            return MakeWorkflowJsonError(400, "workflow_id_mismatch", "URL workflow id does not match parsed JCWF id",
                                         "PUT /api/workflows/{id}", workflowId);
        }

        fs::path const targetPath = (workflowsDirectoryAbsolute / (workflowId + ".jcwf")).lexically_normal();
        if (!fs::exists(targetPath))
        {
            return MakeWorkflowJsonError(404, "workflow_not_found", "Workflow file does not exist: " + targetPath.string(),
                                         "PUT /api/workflows/{id}", workflowId);
        }

        // Version history: backup the existing file before overwriting
        {
            fs::path const historyDir = workflowsDirectoryAbsolute / ".history" / workflowId;
            std::error_code ec;
            fs::create_directories(historyDir, ec);
            if (!ec)
            {
                auto const now = std::chrono::system_clock::now();
                auto const timeT = std::chrono::system_clock::to_time_t(now);
                std::tm gmTime{};
#ifdef _WIN32
                gmtime_s(&gmTime, &timeT);
#else
                gmtime_r(&timeT, &gmTime);
#endif
                char timestampBuf[32];
                std::strftime(timestampBuf, sizeof(timestampBuf), "%Y%m%dT%H%M%S", &gmTime);

                fs::path const backupPath = historyDir / (std::string(timestampBuf) + ".jcwf");
                fs::copy_file(targetPath, backupPath, fs::copy_options::overwrite_existing, ec);
            }
        }

        // Update the .jcwf zip container via the registry (handles extracted dir + repack).
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                std::string upsertErrorMessage;
                if (!m_WorkflowRegistry->SaveOrUpdateWorkflowFromJson(req.body, targetPath, upsertErrorMessage))
                {
                    return MakeWorkflowJsonError(500, "workflow_write_failed", upsertErrorMessage,
                                                 "PUT /api/workflows/{id}", workflowId);
                }
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["id"] = workflowId;
        responseJson["savedPath"] = targetPath.string();
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleWorkflowDelete(std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
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
            return MakeWorkflowJsonError(404, "workflow_not_found", "Workflow not found", "DELETE /api/workflows/{id}",
                                         workflowId);
        }

        fs::path workflowFilePath = fs::path(workflowDefinition->m_WorkflowFilePath);
        if (workflowFilePath.is_relative())
        {
            workflowFilePath = (workflowsDirectoryAbsolute / workflowFilePath).lexically_normal();
        }

        std::error_code errorCode;

        // Delete the .jcwf zip container.
        fs::path const containerPath = (workflowsDirectoryAbsolute / (workflowId + ".jcwf")).lexically_normal();
        bool removed = false;
        if (fs::exists(containerPath, errorCode))
        {
            removed = fs::remove(containerPath, errorCode);
            if (errorCode)
            {
                return MakeWorkflowJsonError(500, "workflow_delete_failed",
                                             "Failed to delete container: " + containerPath.string() +
                                                 " error=" + errorCode.message(),
                                             "DELETE /api/workflows/{id}", workflowId);
            }
        }

        // Delete the extracted directory.
        fs::path const extractedDir = (workflowsDirectoryAbsolute / workflowId).lexically_normal();
        if (fs::is_directory(extractedDir, errorCode))
        {
            fs::remove_all(extractedDir, errorCode);
        }

        // Also try the old plain-file path (for any leftover files).
        if (!removed && fs::exists(workflowFilePath, errorCode))
        {
            removed = fs::remove(workflowFilePath, errorCode);
        }

        if (!removed)
        {
            return MakeWorkflowJsonError(404, "workflow_not_found",
                                         "Workflow files did not exist for: " + workflowId,
                                         "DELETE /api/workflows/{id}", workflowId);
        }

        // Remove from the main registry.
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                std::string removeError;
                m_WorkflowRegistry->RemoveWorkflow(workflowId, false, removeError);
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["id"] = workflowId;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleWorkflowValidatePost(crow::request const& req)
    {
        std::vector<WorkflowValidationFinding> errors;
        std::vector<WorkflowValidationFinding> warnings;
        std::vector<WorkflowValidationFinding> infos;
        std::string workflowId;
        std::string parseErrorMessage;

        ValidateJcwfJsonText(req.body, errors, warnings, infos, workflowId, parseErrorMessage);
        if (!parseErrorMessage.empty())
        {
            return MakeWorkflowJsonError(400, "invalid_jcwf", parseErrorMessage, "POST /api/workflows/validate");
        }

        bool const ok = errors.empty();
        crow::json::wvalue responseJson = MakeWorkflowValidationResponse(ok, workflowId, errors, warnings, infos);
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleWorkflowValidateGet(std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "GET /api/workflows/{id}/validate", workflowId);
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "GET /api/workflows/{id}/validate", workflowId);
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
            return MakeWorkflowJsonError(404, "workflow_not_found", "Workflow not found: " + workflowId,
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
        std::vector<WorkflowValidationFinding> infos;
        std::string parsedWorkflowId;
        std::string parseErrorMessage;

        ValidateJcwfJsonText(workflowJsonContent, errors, warnings, infos, parsedWorkflowId, parseErrorMessage);
        if (!parseErrorMessage.empty())
        {
            return MakeWorkflowJsonError(400, "invalid_jcwf", parseErrorMessage, "GET /api/workflows/{id}/validate",
                                         workflowId);
        }

        if (!parsedWorkflowId.empty() && parsedWorkflowId != workflowId)
        {
            WorkflowValidationFinding mismatch;
            mismatch.m_Code = "workflow_id_mismatch";
            mismatch.m_Message =
                "Workflow id in file ('" + parsedWorkflowId + "') does not match requested id ('" + workflowId + "')";
            mismatch.m_Path = "$.id";
            mismatch.m_TaskId = std::string();
            mismatch.m_Tier = "C";
            warnings.push_back(std::move(mismatch));
        }

        bool const ok = errors.empty();
        crow::json::wvalue responseJson = MakeWorkflowValidationResponse(
            ok, parsedWorkflowId.empty() ? workflowId : parsedWorkflowId, errors, warnings, infos);
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleScriptCheckGet(crow::request const& req)
    {
        // GET /api/scripts/check?path=scripts/runMake.sh
        // Returns: { ok, path, exists, executable, error? }

        std::string scriptPath;
        auto pathParam = req.url_params.get("path");
        if (pathParam != nullptr)
        {
            scriptPath = std::string(pathParam);
        }

        if (scriptPath.empty())
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "missing_path";
            responseJson["message"] = "Query parameter 'path' is required.";
            return MakeJsonResponse(400, responseJson);
        }

        // Security: enforce "scripts/" prefix (same rule as ShellTaskExecutor::ValidateScriptPath)
        if (scriptPath.rfind("scripts/", 0) != 0)
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "invalid_path";
            responseJson["message"] = "Script path must be inside the 'scripts/' directory.";
            responseJson["path"] = scriptPath;
            return MakeJsonResponse(400, responseJson);
        }

        // Allow ".." in raw path (e.g. scripts/helpers/../run.sh) but the
        // lexically-normalized result must still start with "scripts/" (JCWF spec §3.1.2).
        std::string const normalizedScriptPath = fs::path(scriptPath).lexically_normal().string();
        if (normalizedScriptPath.rfind("scripts/", 0) != 0)
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "invalid_path";
            responseJson["message"] = "Resolved script path escapes the 'scripts/' directory.";
            responseJson["path"] = scriptPath;
            return MakeJsonResponse(400, responseJson);
        }

        // Resolve relative to JarvisAgent launch CWD (same as ShellTaskExecutor)
        fs::path const launchCWD = Core::g_Core ? Core::g_Core->GetLaunchCWDAbsolute() : fs::path{};
        if (launchCWD.empty())
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "server_error";
            responseJson["message"] = "Cannot determine JarvisAgent working directory.";
            return MakeJsonResponse(500, responseJson);
        }

        fs::path const absolutePath = (launchCWD / fs::path(scriptPath)).lexically_normal();

        bool const exists = fs::exists(absolutePath);
        bool executable = false;

        if (exists)
        {
            // Check if regular file and has execute permission
            std::error_code ec;
            auto const status = fs::status(absolutePath, ec);
            if (!ec && fs::is_regular_file(status))
            {
                auto const perms = status.permissions();
                executable =
                    (perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["path"] = scriptPath;
        responseJson["exists"] = exists;
        responseJson["executable"] = executable;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleFileCheckGet(crow::request const& req)
    {
        // GET /api/files/check?path=<fileInput>&workflowId=<id>&wd=<working_directory>
        // Uses TaskPathResolver (same as runtime) to resolve the file path.
        // Returns: { ok, path, exists, resolved }

        std::string filePath;
        std::string workflowId;
        std::string workingDirectory;

        auto pathParam = req.url_params.get("path");
        if (pathParam != nullptr)
        {
            filePath = std::string(pathParam);
        }
        auto wfParam = req.url_params.get("workflowId");
        if (wfParam != nullptr)
        {
            workflowId = std::string(wfParam);
        }
        auto wdParam = req.url_params.get("wd");
        if (wdParam != nullptr)
        {
            workingDirectory = std::string(wdParam);
        }

        if (filePath.empty())
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "missing_path";
            responseJson["message"] = "Query parameter 'path' is required.";
            return MakeJsonResponse(400, responseJson);
        }

        // Security: reject absolute paths
        if (fs::path(filePath).is_absolute())
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "invalid_path";
            responseJson["message"] = "Absolute paths are not allowed.";
            responseJson["path"] = filePath;
            return MakeJsonResponse(400, responseJson);
        }

        fs::path const launchCWD = Core::g_Core ? Core::g_Core->GetLaunchCWDAbsolute() : fs::path{};
        if (launchCWD.empty())
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "server_error";
            responseJson["message"] = "Cannot determine JarvisAgent working directory.";
            return MakeJsonResponse(500, responseJson);
        }

        // Resolve using TaskPathResolver — same code path as the runtime.
        fs::path absolutePath;

        if (!workflowId.empty())
        {
            WorkflowRegistry const* registry = nullptr;
            {
                std::scoped_lock<std::mutex> const lock(m_Mutex);
                registry = m_WorkflowRegistry;
            }

            if (registry != nullptr)
            {
                auto const workflowOpt = registry->GetWorkflow(workflowId);
                if (workflowOpt.has_value())
                {
                    fs::path const baseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowOpt.value());
                    fs::path const taskDir = TaskPathResolver::ResolveTaskWorkingDirectoryPath(baseDir, workingDirectory);
                    absolutePath = TaskPathResolver::ResolvePath(taskDir, fs::path(filePath));
                }
            }
        }

        // Fallback: resolve relative to launchCWD (backward compatibility).
        if (absolutePath.empty())
        {
            absolutePath = (launchCWD / fs::path(filePath)).lexically_normal();
        }

        // Security: verify resolved path stays within CWD
        std::string const absStr = absolutePath.string();
        std::string const cwdStr = launchCWD.string();
        if (absStr.rfind(cwdStr, 0) != 0)
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "invalid_path";
            responseJson["message"] = "Resolved path escapes the working directory.";
            responseJson["path"] = filePath;
            return MakeJsonResponse(400, responseJson);
        }

        bool const exists = fs::exists(absolutePath);

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["path"] = filePath;
        responseJson["exists"] = exists;
        responseJson["resolved"] = absolutePath.string();
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleScriptRegistryGet()
    {
        // GET /api/scripts/registry
        // Returns: { "scripts": [ { "path", "short", "params", "description", "outputs" }, ... ] }

        auto* registry = App::g_App ? App::g_App->GetScriptRegistry() : nullptr;
        if (registry == nullptr)
        {
            crow::json::wvalue responseJson;
            responseJson["scripts"] = crow::json::wvalue::list();
            return MakeJsonResponse(200, responseJson);
        }

        auto entries = registry->GetEntries();

        crow::json::wvalue responseJson;
        std::vector<crow::json::wvalue> scriptsArray;
        scriptsArray.reserve(entries.size());

        for (auto const& entry : entries)
        {
            crow::json::wvalue scriptJson;
            scriptJson["path"] = entry.m_FilePath;
            scriptJson["short"] = entry.m_Short;
            scriptJson["description"] = entry.m_Description;

            std::vector<crow::json::wvalue> paramsArray;
            for (auto const& p : entry.m_Params)
            {
                paramsArray.push_back(crow::json::wvalue(p));
            }
            scriptJson["params"] = std::move(paramsArray);

            std::vector<crow::json::wvalue> outputsArray;
            for (auto const& o : entry.m_Outputs)
            {
                outputsArray.push_back(crow::json::wvalue(o));
            }
            scriptJson["outputs"] = std::move(outputsArray);

            scriptsArray.push_back(std::move(scriptJson));
        }

        responseJson["scripts"] = std::move(scriptsArray);
        return MakeJsonResponse(200, responseJson);
    }

    void WebServer::ShutdownAssistantController() { m_AssistantController.Shutdown(); }

    void WebServer::RegisterAssistantWebSocket()
    {
        CROW_WEBSOCKET_ROUTE(m_Server, "/ws/assistant")
            // The assistant channel reads workflow files, runs scripts, and
            // streams AI replies — anonymous access is unacceptable.  Same
            // handshake gate as /ws.
            .onaccept(
                [this](crow::request const& req, void**)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                    {
                        LOG_SECURITY_WARN("[security] ws_assistant_upgrade_rejected ip={} reason={}",
                                          req.remote_ip_address, auth.m_Error);
                        return false;
                    }
                    return true;
                })
            .onopen([this](crow::websocket::connection& conn) { m_AssistantController.OnOpen(conn); })
            .onclose([this](crow::websocket::connection& conn, const std::string& /*reason*/, uint16_t /*code*/)
                     { m_AssistantController.OnClose(conn); })
            .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool /*is_binary*/)
                       { m_AssistantController.OnMessage(conn, data); });
    }


} // namespace AIAssistant

#endif // J9T_STUDIO
