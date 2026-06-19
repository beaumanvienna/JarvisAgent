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
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "auxiliary/file.h"
#include "core.h"
#include "engine.h"
#include "file/pathConfinement.h"
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
        std::filesystem::path const distIndex = ResolveUiDistRoot("workflow-editor") / "index.html";
        if (!std::filesystem::exists(distIndex))
        {
            return crow::response(
                500,
                "Workflow Editor UI build not found. Please run: cd code/frontend/workflow-editor/ui && npm install && "
                "npm run build");
        }

        return ServeStaticFile(distIndex);
    }

    crow::response WebServer::ServeWorkflowEditorStatic(std::string const& requestPath) const
    {
        std::filesystem::path const distRoot = ResolveUiDistRoot("workflow-editor");
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

        // ---- Workflow folder files: list (operator+) + upload (admin) ----
        CROW_ROUTE(m_Server, "/api/workflows/<string>/files")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAuth(req, "operator");
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowFilesListGet(workflowId);
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/files")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowFileUploadPost(req, workflowId);
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/files/<path>")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& workflowId, std::string const& relPath)
                {
                    auto err = CheckAuth(req, "operator");
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowFileGet(workflowId, relPath);
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
        if (auto r = workflowJsonParser.ParseWorkflowJson(req.body, parsedWorkflow); !r)
        {
            return MakeWorkflowJsonError(400, "invalid_jcwf", r.error().m_Details, "POST /api/workflows");
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
        if (auto r = workflowJsonParser.ParseWorkflowJson(req.body, parsedWorkflow); !r)
        {
            return MakeWorkflowJsonError(400, "invalid_jcwf", r.error().m_Details, "PUT /api/workflows/{id}", workflowId);
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
                if (auto removeResult = m_WorkflowRegistry->RemoveWorkflow(workflowId, false); !removeResult)
                {
                    LOG_APP_WARN("HandleWorkflowDelete: registry RemoveWorkflow failed workflow='{}' code={}: {}",
                                 workflowId, Describe(removeResult.error().m_Code), removeResult.error().m_Details);
                }
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["id"] = workflowId;
        return MakeJsonResponse(200, responseJson);
    }

    // GET /api/workflows/{id}/files — list the data files in a workflow's folder (workflows/<id>/),
    // so the editor can show a "+ file" picker and reconstruct artifact-file nodes. Reads the
    // extracted folder directly (the runtime source of truth); the internal version-history backups
    // (.history/) are skipped. operator+; Studio-only.
    crow::response WebServer::HandleWorkflowFilesListGet(std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "GET /api/workflows/{id}/files", workflowId);
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "GET /api/workflows/{id}/files", workflowId);
        }

        fs::path const jcwfPath = (workflowsDirectoryAbsolute / (workflowId + ".jcwf")).lexically_normal();
        if (!fs::exists(jcwfPath))
        {
            return MakeWorkflowJsonError(404, "workflow_not_found", "Workflow file does not exist: " + jcwfPath.string(),
                                         "GET /api/workflows/{id}/files", workflowId);
        }

        fs::path const extractedDir = ConfineUnderProjectRoot(
            (workflowsDirectoryAbsolute / workflowId).lexically_normal());
        if (extractedDir.empty())
        {
            return MakeWorkflowJsonError(500, "path_rejected", "Workflow folder does not resolve under project root",
                                         "GET /api/workflows/{id}/files", workflowId);
        }

        crow::json::wvalue::list filesJson;
        std::error_code ec;
        if (fs::is_directory(extractedDir, ec))
        {
            for (auto const& e : fs::recursive_directory_iterator(extractedDir, ec))
            {
                if (!e.is_regular_file()) continue;

                auto rel = fs::relative(e.path(), extractedDir, ec);
                if (ec) continue;
                std::string const relPath = rel.generic_string();
                // Skip internal version-history backups — not workflow content.
                if (relPath == ".history" || relPath.rfind(".history/", 0) == 0) continue;

                crow::json::wvalue entry;
                entry["path"] = relPath;
                entry["is_dir"] = false;
                auto const size = e.file_size(ec);
                entry["size_bytes"] = ec ? 0 : static_cast<int64_t>(size);

                auto const ftime = fs::last_write_time(e.path(), ec);
                if (!ec)
                {
                    auto const sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                    std::time_t const t = std::chrono::system_clock::to_time_t(sctp);
                    std::tm utc{};
#ifdef _WIN32
                    gmtime_s(&utc, &t);
#else
                    gmtime_r(&t, &utc);
#endif
                    std::ostringstream iso;
                    iso << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
                    entry["modified_at"] = iso.str();
                }

                filesJson.push_back(std::move(entry));
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["workflowId"] = workflowId;
        responseJson["files"] = std::move(filesJson);
        return MakeJsonResponse(200, responseJson);
    }

    // POST /api/workflows/{id}/files — upload one file (multipart/form-data, field "file") into a
    // workflow's folder, then repack the .jcwf. The destination is the file's basename under
    // workflows/<id>/ (no sub-paths, no traversal); confined under the project root and verified to
    // land inside the workflow folder before any write. admin; Studio-only.
    crow::response WebServer::HandleWorkflowFileUploadPost(crow::request const& req, std::string const& workflowId)
    {
        constexpr uint64_t kMaxUploadBytes = 25ull * 1024 * 1024; // 25 MB

        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "POST /api/workflows/{id}/files", workflowId);
        }

        std::string const contentType = req.get_header_value("Content-Type");
        if (contentType.find("multipart/form-data") == std::string::npos)
        {
            return MakeWorkflowJsonError(400, "invalid_content_type",
                                         "Expected multipart/form-data with a 'file' part",
                                         "POST /api/workflows/{id}/files", workflowId);
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "POST /api/workflows/{id}/files", workflowId);
        }

        fs::path const jcwfPath = (workflowsDirectoryAbsolute / (workflowId + ".jcwf")).lexically_normal();
        if (!fs::exists(jcwfPath))
        {
            return MakeWorkflowJsonError(404, "workflow_not_found", "Workflow file does not exist: " + jcwfPath.string(),
                                         "POST /api/workflows/{id}/files", workflowId);
        }

        fs::path const extractedDir = ConfineUnderProjectRoot(
            (workflowsDirectoryAbsolute / workflowId).lexically_normal());
        std::error_code ec;
        if (extractedDir.empty() || !fs::is_directory(extractedDir, ec))
        {
            return MakeWorkflowJsonError(500, "workflow_not_extracted",
                                         "Workflow folder is missing or does not resolve under project root",
                                         "POST /api/workflows/{id}/files", workflowId);
        }

        // Parse the multipart body and locate the file part (the first part carrying a filename).
        std::string rawFilename;
        std::string fileBody;
        bool haveFile = false;
        try
        {
            crow::multipart::message const msg(req);
            for (auto const& part : msg.parts)
            {
                auto const& disposition = part.get_header_object("Content-Disposition");
                auto const it = disposition.params.find("filename");
                if (it != disposition.params.end() && !it->second.empty())
                {
                    rawFilename = it->second;
                    fileBody = part.body;
                    haveFile = true;
                    break;
                }
            }
        }
        catch (std::exception const& e)
        {
            return MakeWorkflowJsonError(400, "malformed_multipart", std::string("Malformed multipart body: ") + e.what(),
                                         "POST /api/workflows/{id}/files", workflowId);
        }

        if (!haveFile)
        {
            return MakeWorkflowJsonError(400, "no_file", "No file part found in multipart body",
                                         "POST /api/workflows/{id}/files", workflowId);
        }

        // Force the destination to the basename — strip any directory the client included — then
        // reject anything that isn't a plain, traversal-free filename.
        std::string const filename = fs::path(rawFilename).filename().string();
        if (filename.empty() || filename == "." || filename == ".."
            || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos)
        {
            return MakeWorkflowJsonError(400, "invalid_filename", "Upload filename is empty or contains a path",
                                         "POST /api/workflows/{id}/files", workflowId);
        }

        if (fileBody.size() > kMaxUploadBytes)
        {
            return MakeWorkflowJsonError(413, "file_too_large",
                                         "Uploaded file exceeds the 25 MB limit", "POST /api/workflows/{id}/files",
                                         workflowId);
        }

        // Confine the target under the project root, then verify it lands inside THIS workflow's
        // folder (project-root confinement alone would still allow a sibling workflow). The basename
        // validation above already guarantees a direct child; this fs::relative check is portable
        // belt-and-suspenders against any surprise from canonicalisation.
        fs::path const targetConfined = ConfineUnderProjectRoot((extractedDir / filename).lexically_normal());
        fs::path const relToWorkflow = targetConfined.empty() ? fs::path{} : fs::relative(targetConfined, extractedDir, ec);
        std::string const relStr = relToWorkflow.generic_string();
        if (targetConfined.empty() || ec || relStr.empty() || relStr == ".." || relStr.rfind("../", 0) == 0)
        {
            return MakeWorkflowJsonError(400, "path_rejected", "Upload path does not resolve inside the workflow folder",
                                         "POST /api/workflows/{id}/files", workflowId);
        }

        if (!EngineCore::AtomicWriteFile(targetConfined, fileBody, errorMessage))
        {
            LOG_APP_ERROR("HandleWorkflowFileUploadPost: write failed workflow='{}' file='{}': {}", workflowId, filename,
                          errorMessage);
            return MakeWorkflowJsonError(500, "write_failed", errorMessage, "POST /api/workflows/{id}/files", workflowId);
        }

        // Repack the .jcwf so the container reflects the new file. Serialised against other
        // registry mutations on m_Mutex.
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            std::string packError;
            if (!JcwfContainer::Pack(extractedDir, jcwfPath, packError))
            {
                LOG_APP_ERROR("HandleWorkflowFileUploadPost: repack failed workflow='{}' file='{}': {}", workflowId,
                              filename, packError);
                return MakeWorkflowJsonError(500, "repack_failed", packError, "POST /api/workflows/{id}/files", workflowId);
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["workflowId"] = workflowId;
        responseJson["path"] = filename;
        responseJson["size_bytes"] = static_cast<int64_t>(fileBody.size());
        return MakeJsonResponse(201, responseJson);
    }

    // GET /api/workflows/{id}/files/{path} — read one file from a workflow's folder (e.g. a filter's
    // source CSV so the editor's fan-out builder can parse its header). Confined under workflows/<id>/,
    // 10 MB cap, returned as text/plain. operator+; Studio-only.
    crow::response WebServer::HandleWorkflowFileGet(std::string const& workflowId, std::string const& relPath)
    {
        constexpr uint64_t kMaxFileBytes = 10ull * 1024 * 1024; // 10 MB

        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "GET /api/workflows/{id}/files/{path}", workflowId);
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "GET /api/workflows/{id}/files/{path}",
                                         workflowId);
        }

        fs::path const extractedDir = ConfineUnderProjectRoot(
            (workflowsDirectoryAbsolute / workflowId).lexically_normal());
        std::error_code ec;
        if (extractedDir.empty() || !fs::is_directory(extractedDir, ec))
        {
            return MakeWorkflowJsonError(404, "workflow_not_found", "Workflow folder does not exist",
                                         "GET /api/workflows/{id}/files/{path}", workflowId);
        }

        // Confine the requested path under the workflow folder (rejects traversal / symlink escape).
        fs::path const targetConfined = ConfineUnderProjectRoot((extractedDir / relPath).lexically_normal());
        fs::path const relToWorkflow = targetConfined.empty() ? fs::path{} : fs::relative(targetConfined, extractedDir, ec);
        std::string const relStr = relToWorkflow.generic_string();
        if (targetConfined.empty() || ec || relStr.empty() || relStr == ".." || relStr.rfind("../", 0) == 0)
        {
            return MakeWorkflowJsonError(400, "path_rejected", "Requested path does not resolve inside the workflow folder",
                                         "GET /api/workflows/{id}/files/{path}", workflowId);
        }

        if (!fs::is_regular_file(targetConfined, ec))
        {
            return MakeWorkflowJsonError(404, "file_not_found", "File does not exist: " + relStr,
                                         "GET /api/workflows/{id}/files/{path}", workflowId);
        }

        uint64_t const size = fs::file_size(targetConfined, ec);
        if (ec)
        {
            return MakeWorkflowJsonError(500, "read_failed", "Could not stat file", "GET /api/workflows/{id}/files/{path}",
                                         workflowId);
        }
        if (size > kMaxFileBytes)
        {
            return MakeWorkflowJsonError(413, "file_too_large", "File exceeds the 10 MB read limit",
                                         "GET /api/workflows/{id}/files/{path}", workflowId);
        }

        std::ifstream in(targetConfined, std::ios::binary);
        if (!in)
        {
            return MakeWorkflowJsonError(500, "read_failed", "Could not open file", "GET /api/workflows/{id}/files/{path}",
                                         workflowId);
        }
        std::string const content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        crow::response response(200, content);
        response.set_header("Content-Type", "text/plain; charset=utf-8");
        return response;
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

        JarvisAgent* const app = App::g_App.load(std::memory_order_acquire);
        auto* registry = (app != nullptr) ? app->GetScriptRegistry() : nullptr;
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

    void WebServer::InitEditionSpecific()
    {
        RegisterAssistantWebSocket();

        m_AiJcwfService.SetBroadcastFn(
            [this](std::string const& jsonString)
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_PendingBroadcasts.push_back(jsonString);
            });
    }

    bool WebServer::HandleAssistantWebSocketMessage(crow::websocket::connection& conn,
                                                    simdjson::ondemand::document& doc,
                                                    std::string_view type)
    {
        if (type == "ai-explain-jcwf")
        {
            std::string jcwfJson = std::string(doc["jcwf"].get_string().value());
            m_AiJcwfService.ExplainAsync(jcwfJson);
            return true;
        }

        if (type == "ai-generate-jcwf")
        {
            std::string prompt = std::string(doc["prompt"].get_string().value());
            std::string currentJcwf;
            auto currentResult = doc["currentJcwf"].get_string();
            if (currentResult.error() == simdjson::SUCCESS)
            {
                currentJcwf = std::string(currentResult.value());
            }
            m_AiJcwfService.GenerateAsync(prompt, currentJcwf);
            return true;
        }

        if (type == "ai-write-scripts")
        {
            // ai-write-scripts mutates disk state (writes files under scripts/,
            // sets +x on .sh) so it requires admin role.  Without this gate, any
            // operator/viewer who held a valid /ws upgrade could plant scripts
            // that would then run on the next workflow trigger.
            std::string role;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                auto it = m_WsClientRoles.find(&conn);
                if (it != m_WsClientRoles.end())
                {
                    role = it->second;
                }
            }
            if (role != "admin")
            {
                LOG_SECURITY_WARN("[security] ai_write_scripts_role_denied role='{}' ip={}", role,
                                  conn.get_remote_ip());
                crow::json::wvalue err;
                err["type"] = "ai-write-scripts-result";
                err["ok"] = false;
                err["error"] = "forbidden";
                err["message"] = "ai-write-scripts requires admin role";
                conn.send_text(err.dump());
                return true;
            }

            crow::json::wvalue result;
            result["type"] = "ai-write-scripts-result";
            crow::json::wvalue::list writtenList;
            crow::json::wvalue::list errorsList;

            auto scriptsArr = doc["scripts"].get_array();
            if (scriptsArr.error() == simdjson::SUCCESS)
            {
                for (auto scriptEl : scriptsArr.value())
                {
                    simdjson::ondemand::object scriptObj;
                    if (scriptEl.get_object().get(scriptObj) != simdjson::SUCCESS)
                    {
                        continue;
                    }

                    std::string_view pathView;
                    if (scriptObj["path"].get_string().get(pathView) != simdjson::SUCCESS)
                    {
                        continue;
                    }
                    std::string scriptPath(pathView);

                    std::string_view contentView;
                    if (scriptObj["content"].get_string().get(contentView) != simdjson::SUCCESS)
                    {
                        continue;
                    }
                    std::string content(contentView);

                    bool executable = false;
                    [[maybe_unused]] auto execErr = scriptObj["executable"].get_bool().get(executable);

                    if (scriptPath.rfind("scripts/", 0) != 0)
                    {
                        crow::json::wvalue err;
                        err["path"] = scriptPath;
                        err["error"] = "Path must start with 'scripts/'";
                        errorsList.push_back(std::move(err));
                        continue;
                    }

                    fs::path normalized = fs::path(scriptPath).lexically_normal();
                    if (normalized.string().rfind("scripts/", 0) != 0)
                    {
                        crow::json::wvalue err;
                        err["path"] = scriptPath;
                        err["error"] = "Path escapes scripts/ directory";
                        errorsList.push_back(std::move(err));
                        continue;
                    }

                    std::string scriptWriteError;
                    if (!EngineCore::AtomicWriteFile(normalized, content, scriptWriteError))
                    {
                        crow::json::wvalue err;
                        err["path"] = scriptPath;
                        err["error"] = scriptWriteError;
                        errorsList.push_back(std::move(err));
                        continue;
                    }

                    std::error_code ec;
                    if (executable || scriptPath.ends_with(".sh"))
                    {
                        fs::permissions(normalized,
                                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                                        fs::perm_options::add, ec);
                    }

                    writtenList.push_back(scriptPath);
                    LOG_APP_INFO("[ai-write-scripts] Wrote script: {}", normalized.string());
                }
            }

            result["ok"] = errorsList.empty();
            result["written"] = std::move(writtenList);
            result["errors"] = std::move(errorsList);

            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_PendingBroadcasts.push_back(result.dump());
            }
            return true;
        }

        if (type == "ai-fix-failed-script")
        {
            std::string scriptPath;
            std::string stderrContent;
            std::string taskType;

            {
                std::string_view sv;
                if (doc["scriptPath"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    scriptPath = std::string(sv);
                }
                if (doc["stderr"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    stderrContent = std::string(sv);
                }
                if (doc["taskType"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    taskType = std::string(sv);
                }
            }

            if (scriptPath.empty())
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_PendingBroadcasts.push_back(
                    R"({"type":"ai-fix-script-result","ok":false,"error":"Missing scriptPath"})");
            }
            else
            {
                m_AiJcwfService.FixFailedScriptAsync(scriptPath, stderrContent, taskType);
            }
            return true;
        }

        return false;
    }


} // namespace AIAssistant

#endif // J9T_STUDIO
