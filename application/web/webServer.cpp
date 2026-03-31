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

#include <cstring>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>
#include <optional>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "web/webServer.h"
#include "web/chatMessages.h"
#include "file/scriptRegistry.h"

#include "session/sessionManager.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowJsonParser.h"

#include "workflow/workflowValidator.h"

#include "workflow/workflowRuntimeManager.h"
#include "workflow/workflowTypes.h"

#include "event/events.h"
#include "keys/keyEncryption.h"
#include "workflow/triggerEngine.h"
#include "workflow/workflowTriggerBinder.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>

namespace fs = std::filesystem;
namespace AIAssistant

{
    namespace
    {
        std::string GenerateIntegrationRunId(std::string const& workflowId)
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            return workflowId + "_" + std::to_string(millis);
        }

        std::string ComputeHmacSha256Hex(std::string const& secret, std::string const& data)
        {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int digestLength = 0;

            HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                 reinterpret_cast<unsigned char const*>(data.data()), data.size(), digest, &digestLength);

            std::string hex;
            hex.reserve(digestLength * 2);
            static constexpr char hexDigits[] = "0123456789abcdef";
            for (unsigned int i = 0; i < digestLength; ++i)
            {
                hex.push_back(hexDigits[(digest[i] >> 4) & 0x0F]);
                hex.push_back(hexDigits[digest[i] & 0x0F]);
            }
            return hex;
        }

        bool VerifyHmacSignature(std::string const& secret, std::string const& body, std::string const& headerValue)
        {
            // Expected header format: "sha256=<hex>"
            static constexpr std::string_view kPrefix = "sha256=";
            if (headerValue.size() <= kPrefix.size() || headerValue.compare(0, kPrefix.size(), kPrefix) != 0)
            {
                return false;
            }

            std::string const providedHex = headerValue.substr(kPrefix.size());
            std::string const expectedHex = ComputeHmacSha256Hex(secret, body);

            // Constant-time comparison to prevent timing attacks.
            if (providedHex.size() != expectedHex.size())
            {
                return false;
            }
            unsigned char result = 0;
            for (size_t i = 0; i < providedHex.size(); ++i)
            {
                result |= static_cast<unsigned char>(providedHex[i]) ^ static_cast<unsigned char>(expectedHex[i]);
            }
            return result == 0;
        }

        void SetJsonHeaders(crow::response& response)
        {
            response.add_header("Content-Type", "application/json");
            response.add_header("Cache-Control", "no-store");
        }

        crow::response MakeJsonResponse(int const httpStatus, crow::json::wvalue const& json)
        {
            crow::response response(httpStatus, json.dump());
            SetJsonHeaders(response);
            return response;
        }

        crow::response MakeJsonTextResponse(int const httpStatus, std::string const& jsonText)
        {
            crow::response response(httpStatus, jsonText);
            SetJsonHeaders(response);
            return response;
        }

        crow::response MakeWorkflowJsonError(int const httpStatus, std::string const& errorCode, std::string const& message,
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

            return MakeJsonResponse(httpStatus, responseJson);
        }

        struct WorkflowValidationFinding
        {
            std::string m_Code;
            std::string m_Message;

            std::string m_Path;
            std::string m_TaskId;

            std::string m_Tier;
        };

        std::string ToTierString(WorkflowValidationTier const tier)
        {
            switch (tier)
            {
                case WorkflowValidationTier::A:
                    return "A";
                case WorkflowValidationTier::B:
                    return "B";
                case WorkflowValidationTier::C:
                    return "C";
                case WorkflowValidationTier::D:
                    return "D";
                default:
                    return "B";
            }
        }

        void AddFindingToList(crow::json::wvalue::list& list, WorkflowValidationFinding const& finding)
        {
            crow::json::wvalue item;
            item["code"] = finding.m_Code;
            item["message"] = finding.m_Message;
            if (!finding.m_Tier.empty())
            {
                item["tier"] = finding.m_Tier;
            }
            if (!finding.m_Path.empty())
            {
                item["path"] = finding.m_Path;
            }
            if (!finding.m_TaskId.empty())
            {
                item["taskId"] = finding.m_TaskId;
            }
            list.push_back(std::move(item));
        }

        crow::json::wvalue MakeWorkflowValidationResponse(bool const ok, std::string const& workflowId,
                                                          std::vector<WorkflowValidationFinding> const& errors,
                                                          std::vector<WorkflowValidationFinding> const& warnings,
                                                          std::vector<WorkflowValidationFinding> const& infos)
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = ok;
            responseJson["id"] = workflowId;

            crow::json::wvalue::list errorsList;
            for (auto const& error : errors)
            {
                AddFindingToList(errorsList, error);
            }
            responseJson["errors"] = std::move(errorsList);

            crow::json::wvalue::list warningsList;
            for (auto const& warning : warnings)
            {
                AddFindingToList(warningsList, warning);
            }
            responseJson["warnings"] = std::move(warningsList);

            crow::json::wvalue::list infosList;
            for (auto const& info : infos)
            {
                AddFindingToList(infosList, info);
            }
            responseJson["infos"] = std::move(infosList);

            return responseJson;
        }

        void ValidateJcwfParsedWorkflow(WorkflowDefinition const& workflow, std::vector<WorkflowValidationFinding>& errors,
                                        std::vector<WorkflowValidationFinding>& warnings,
                                        std::vector<WorkflowValidationFinding>& infos)
        {
            std::vector<WorkflowValidationIssue> issues;
            WorkflowValidator::Validate(workflow, issues);

            for (WorkflowValidationIssue const& issue : issues)
            {
                WorkflowValidationFinding finding;
                finding.m_Code = issue.m_Code;
                finding.m_Message = issue.m_Message;
                finding.m_Path = issue.m_Path;
                finding.m_TaskId = issue.m_TaskId;
                finding.m_Tier = ToTierString(issue.m_Tier);

                if (issue.m_Severity == WorkflowValidationSeverity::Error)
                {
                    errors.push_back(std::move(finding));
                }
                else if (issue.m_Severity == WorkflowValidationSeverity::Info)
                {
                    infos.push_back(std::move(finding));
                }
                else
                {
                    warnings.push_back(std::move(finding));
                }
            }
        }

        void ValidateJcwfJsonText(std::string const& workflowJsonText, std::vector<WorkflowValidationFinding>& errors,
                                  std::vector<WorkflowValidationFinding>& warnings,
                                  std::vector<WorkflowValidationFinding>& infos, std::string& workflowIdOut,
                                  std::string& parseErrorMessageOut)
        {
            errors.clear();
            warnings.clear();
            infos.clear();
            workflowIdOut.clear();
            parseErrorMessageOut.clear();

            WorkflowJsonParser workflowJsonParser;
            WorkflowDefinition parsedWorkflow;
            if (!workflowJsonParser.ParseWorkflowJson(workflowJsonText, parsedWorkflow, parseErrorMessageOut))
            {
                return;
            }

            workflowIdOut = parsedWorkflow.m_Id;
            ValidateJcwfParsedWorkflow(parsedWorkflow, errors, warnings, infos);
        }

        bool IsValidWorkflowId(std::string const& workflowId)
        {
            if (workflowId.empty())
            {
                return false;
            }

            for (char const character : workflowId)
            {
                bool const isAlphaNumeric =
                    ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                     (character >= '0' && character <= '9'));
                bool const isAllowedSymbol = (character == '_' || character == '-');
                if (!isAlphaNumeric && !isAllowedSymbol)
                {
                    return false;
                }
            }

            return true;
        }

        bool IsValidTaskName(std::string const& taskName)
        {
            // Same restrictions as workflow ids: simple path-segment with no slashes.
            return IsValidWorkflowId(taskName);
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
            std::filesystem::path workflowsDirectoryAbsolute = workflowsPathFromConfig.is_absolute()
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

        bool WriteTextFileAtomic(std::filesystem::path const& filePath, std::string const& content,
                                 std::string& errorMessage)
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
                errorMessage =
                    "Failed to rename temp file to target: " + filePath.string() + " error=" + errorCode.message();
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
                case WorkflowRunState::Paused:
                    return "paused";
                case WorkflowRunState::Stopping:
                    return "stopping";
                case WorkflowRunState::Succeeded:
                    return "succeeded";
                case WorkflowRunState::Failed:
                    return "failed";
                case WorkflowRunState::Cancelled:
                    return "cancelled";
                case WorkflowRunState::Stopped:
                    return "stopped";
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
        RegisterAssistantWebSocket();

        m_AiJcwfService.SetBroadcastFn(
            [this](std::string const& jsonString)
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_PendingBroadcasts.push_back(jsonString);
            });
    }

    WebServer::~WebServer() { Stop(); }

    void WebServer::SetWorkflowRegistry(WorkflowRegistry* workflowRegistry)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_WorkflowRegistry = workflowRegistry;
        m_AssistantController.SetWorkflowRegistry(workflowRegistry);
    }

    void WebServer::SetWorkflowRuntimeManager(WorkflowRuntimeManager* workflowRuntimeManager)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_WorkflowRuntimeManager = workflowRuntimeManager;
        m_AssistantController.SetWorkflowRuntimeManager(workflowRuntimeManager);
    }

    void WebServer::SetTriggerEngine(TriggerEngine* triggerEngine)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_TriggerEngine = triggerEngine;
    }

    namespace
    {
        // Replace invalid UTF-8 bytes with the Unicode replacement character so
        // WebSocket text frames stay valid (RFC 6455 requires valid UTF-8).
        std::string SanitizeUtf8(std::string const& input)
        {
            std::string out;
            out.reserve(input.size());
            size_t i = 0;
            while (i < input.size())
            {
                unsigned char c = static_cast<unsigned char>(input[i]);
                size_t seqLen = 0;
                if (c <= 0x7F)
                {
                    seqLen = 1;
                }
                else if ((c & 0xE0) == 0xC0)
                {
                    seqLen = 2;
                }
                else if ((c & 0xF0) == 0xE0)
                {
                    seqLen = 3;
                }
                else if ((c & 0xF8) == 0xF0)
                {
                    seqLen = 4;
                }

                if (seqLen == 0 || i + seqLen > input.size())
                {
                    out += "\xEF\xBF\xBD"; // U+FFFD
                    ++i;
                    continue;
                }

                bool valid = true;
                for (size_t j = 1; j < seqLen; ++j)
                {
                    if ((static_cast<unsigned char>(input[i + j]) & 0xC0) != 0x80)
                    {
                        valid = false;
                        break;
                    }
                }

                if (valid)
                {
                    out.append(input, i, seqLen);
                    i += seqLen;
                }
                else
                {
                    out += "\xEF\xBF\xBD"; // U+FFFD
                    ++i;
                }
            }
            return out;
        }
    } // anonymous namespace

    void WebServer::BroadcastWorkflowRunsSnapshot()
    {
        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (!workflowRuntimeManager)
        {
            return;
        }

        std::vector<WorkflowRun> const activeRuns = workflowRuntimeManager->GetActiveRunsSnapshot();

        auto ToRunStateString = [](WorkflowRun const& run) -> char const*
        {
            if (run.m_IsCompleted)
            {
                return run.m_HasFailed ? "failed" : "completed";
            }

            // If any task is running, call the run running.
            for (auto const& pair : run.m_TaskStates)
            {
                if (pair.second.m_State == TaskInstanceStateKind::Running)
                {
                    return "running";
                }
            }

            return "queued";
        };

        crow::json::wvalue json;
        json["type"] = "workflowRunsSnapshot";

        std::vector<crow::json::wvalue> runs;
        runs.reserve(activeRuns.size());

        for (WorkflowRun const& runSnapshot : activeRuns)
        {
            crow::json::wvalue run;
            run["runId"] = runSnapshot.m_RunId;
            run["workflowId"] = runSnapshot.m_WorkflowId;
            run["state"] = ToRunStateString(runSnapshot);

            std::vector<std::string> taskIds;
            taskIds.reserve(runSnapshot.m_TaskStates.size());
            for (auto const& it : runSnapshot.m_TaskStates)
            {
                taskIds.push_back(it.first);
            }
            std::sort(taskIds.begin(), taskIds.end());

            std::vector<crow::json::wvalue> tasks;
            tasks.reserve(taskIds.size());

            for (std::string const& taskId : taskIds)
            {
                auto const taskIt = runSnapshot.m_TaskStates.find(taskId);
                if (taskIt == runSnapshot.m_TaskStates.end())
                {
                    continue;
                }

                TaskInstanceState const& taskState = taskIt->second;

                crow::json::wvalue task;
                task["taskId"] = taskId;
                task["state"] = ToStringTaskInstanceStateKind(taskState.m_State);
                task["attemptCount"] = taskState.m_AttemptCount;
                task["lastErrorMessage"] = SanitizeUtf8(taskState.m_LastErrorMessage);
                if (!taskState.m_CapturedStdout.empty())
                {
                    task["capturedStdout"] = SanitizeUtf8(taskState.m_CapturedStdout);
                }
                if (!taskState.m_CapturedStderr.empty())
                {
                    task["capturedStderr"] = SanitizeUtf8(taskState.m_CapturedStderr);
                }

                tasks.push_back(std::move(task));
            }

            run["tasks"] = std::move(tasks);
            runs.push_back(std::move(run));
        }

        json["runs"] = std::move(runs);

        BroadcastJSON(json.dump());
    }

    void WebServer::BroadcastWorkflowRunsLastSnapshot()
    {
        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (!workflowRuntimeManager)
        {
            return;
        }

        auto lastRuns = workflowRuntimeManager->GetLastRunsSnapshot();

        uint64_t completedCount = 0;
        uint64_t failedCount = 0;
        workflowRuntimeManager->GetRunCounters(completedCount, failedCount);

        crow::json::wvalue json;
        json["type"] = "workflowRunsLastSnapshot";
        json["totalCompleted"] = static_cast<int64_t>(completedCount);
        json["totalFailed"] = static_cast<int64_t>(failedCount);

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
        json["runs"] = std::move(runsJson);

        BroadcastJSON(json.dump());
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
        response.set_header("X-Content-Type-Options", "nosniff");

        std::string const fileName = filePath.filename().string();
        bool const isIndexHtml = fileName == "index.html";
        if (isIndexHtml)
        {
            response.set_header("Cache-Control", "no-cache");
        }
        else
        {
            response.set_header("Cache-Control", "public, max-age=31536000, immutable");
        }

        response.body = std::move(content);
        return response;
    }

    crow::response WebServer::ServeDashboardIndex() const
    {
        std::filesystem::path const distIndex = std::filesystem::path("dashboard") / "ui" / "dist" / "index.html";
        if (!std::filesystem::exists(distIndex))
        {
            return crow::response(
                500, "Dashboard UI build not found. Please run: cd dashboard/ui && npm install && npm run build");
        }

        return ServeStaticFile(distIndex);
    }

    crow::response WebServer::ServeDashboardStatic(std::string const& requestPath) const
    {
        std::filesystem::path const distRoot = std::filesystem::path("dashboard") / "ui" / "dist";

        // Dashboard assets live under /dash-assets/...
        if (requestPath.rfind("/dash-assets/", 0) == 0)
        {
            std::string const relative = requestPath.substr(std::string("/dash-assets/").size());
            return ServeStaticFile(distRoot / relative);
        }

        // Fallback to dashboard index (SPA)
        return ServeDashboardIndex();
    }

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
        // ---- Dashboard UI (React) ----
        // Serves the production build from: dashboard/ui/dist
        CROW_ROUTE(m_Server, "/")([this]() { return ServeDashboardIndex(); });

        // Dashboard assets: /dash-assets/...
        CROW_ROUTE(m_Server, "/dash-assets/<path>")
        ([this](std::string const& path) { return ServeDashboardStatic(std::string("/dash-assets/") + path); });

        // ---- Workflow Editor UI (React) ----
        // Serves the production build from: workflow-editor/ui/dist
        CROW_ROUTE(m_Server, "/editor")([this]() { return ServeWorkflowEditorIndex(); });

        // Vite default asset paths are rooted at "/assets/...".
        CROW_ROUTE(m_Server, "/assets/<path>")
        ([this](std::string const& path) { return ServeWorkflowEditorStatic(std::string("/assets/") + path); });

        // SPA fallback for any sub-route under /editor (e.g. /editor/workflows/...)
        CROW_ROUTE(m_Server, "/editor/<path>")
        ([this](std::string const& path) { return ServeWorkflowEditorStatic(std::string("/editor/") + path); });
        // ---- POST /api/chat ----
        CROW_ROUTE(m_Server, "/api/chat")
            .methods("POST"_method)([this](const crow::request& req) { return HandleChatPost(req); });

        // ---- GET /api/status ----
        CROW_ROUTE(m_Server, "/api/status")([this]() { return HandleStatusGet(); });

        // ---- Workflow Editor: CRUD (stubs moved to real implementation) ----
        CROW_ROUTE(m_Server, "/api/workflows").methods("GET"_method)([this]() { return HandleWorkflowsListGet(); });

        CROW_ROUTE(m_Server, "/api/workflows/reload")
            .methods("POST"_method)([this]() { return HandleWorkflowsReloadPost(); });

        CROW_ROUTE(m_Server, "/api/workflows")
            .methods("POST"_method)([this](crow::request const& req) { return HandleWorkflowsCreatePost(req); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("GET"_method)([this](std::string const& workflowId) { return HandleWorkflowGet(workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("PUT"_method)([this](crow::request const& req, std::string const& workflowId)
                                   { return HandleWorkflowUpdatePut(req, workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("DELETE"_method)([this](std::string const& workflowId) { return HandleWorkflowDelete(workflowId); });

        // ---- Workflow Editor: versioning ----
        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions")
            .methods("GET"_method)([this](std::string const& workflowId)
                                   { return HandleWorkflowVersionsListGet(workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions/<string>")
            .methods("GET"_method)([this](std::string const& workflowId, std::string const& timestamp)
                                   { return HandleWorkflowVersionGetGet(workflowId, timestamp); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions/<string>/restore")
            .methods("POST"_method)([this](std::string const& workflowId, std::string const& timestamp)
                                    { return HandleWorkflowVersionRestorePost(workflowId, timestamp); });

        // ---- Workflow Editor: validation ----
        CROW_ROUTE(m_Server, "/api/workflows/validate")
            .methods("POST"_method)([this](crow::request const& req) { return HandleWorkflowValidatePost(req); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/validate")
            .methods("GET"_method)([this](std::string const& workflowId) { return HandleWorkflowValidateGet(workflowId); });

        // ---- Workflow Editor: run control + monitoring ----
        CROW_ROUTE(m_Server, "/api/workflows/<string>/run")
            .methods("POST"_method)([this](crow::request const& req, std::string const& workflowId)
                                    { return HandleWorkflowRunPost(req, workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/clean")
            .methods("DELETE"_method)([this](std::string const& workflowId)
                                      { return HandleWorkflowCleanDelete(workflowId); });

        CROW_ROUTE(m_Server, "/api/workflow-runs/active")
            .methods("GET"_method)([this]() { return HandleWorkflowRunsActiveGet(); });

        CROW_ROUTE(m_Server, "/api/workflow-runs/last")
            .methods("GET"_method)([this]() { return HandleWorkflowRunsLastGet(); });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>")
            .methods("GET"_method)([this](std::string const& runId) { return HandleWorkflowRunGet(runId); });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/cancel")
            .methods("POST"_method)([this](std::string const& runId) { return HandleWorkflowRunCancelPost(runId); });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/pause")
            .methods("POST"_method)([this](std::string const& runId) { return HandleWorkflowRunPausePost(runId); });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/resume")
            .methods("POST"_method)([this](std::string const& runId) { return HandleWorkflowRunResumePost(runId); });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/stop")
            .methods("POST"_method)([this](std::string const& runId) { return HandleWorkflowRunStopPost(runId); });

        // ---- Integrations: n8n ----
        CROW_ROUTE(m_Server, "/api/integrations/n8n/start")
            .methods("POST"_method)([this](crow::request const& req) { return HandleN8nStartPost(req); });

        // ---- Webhook trigger ----
        CROW_ROUTE(m_Server, "/api/webhook/<string>")
            .methods("POST"_method)([this](crow::request const& req, std::string const& workflowId)
                                    { return HandleWebhookPost(req, workflowId); });

        // ---- AI interfaces API (config.json) ----
        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces")
            .methods("GET"_method)([this]() { return HandleAiInterfacesListGet(); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces")
            .methods("POST"_method)([this](crow::request const& req) { return HandleAiInterfaceCreatePost(req); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/save")
            .methods("POST"_method)([this]() { return HandleAiInterfacesSavePost(); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/test")
            .methods("POST"_method)([this](crow::request const& req) { return HandleAiInterfaceTestPost(req); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/<string>")
            .methods("PUT"_method)([this](crow::request const& req, std::string const& name)
                                   { return HandleAiInterfaceUpdatePut(req, name); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/<string>")
            .methods("DELETE"_method)([this](std::string const& name) { return HandleAiInterfaceDeleteDelete(name); });

        CROW_ROUTE(m_Server, "/api/settings/config/reload")
            .methods("POST"_method)([this]() { return HandleConfigReloadPost(); });

        CROW_ROUTE(m_Server, "/api/settings/config").methods("GET"_method)([this]() { return HandleConfigSettingsGet(); });

        CROW_ROUTE(m_Server, "/api/settings/config")
            .methods("PUT"_method)([this](crow::request const& req) { return HandleConfigSettingsPut(req); });

        // ---- Key management API ----
        CROW_ROUTE(m_Server, "/api/settings/keys/status").methods("GET"_method)([this]() { return HandleKeysStatusGet(); });

        CROW_ROUTE(m_Server, "/api/settings/keys/unlock")
            .methods("POST"_method)([this](crow::request const& req) { return HandleKeysUnlockPost(req); });

        // ---- Provider settings API ----
        CROW_ROUTE(m_Server, "/api/settings/providers").methods("GET"_method)([this]() { return HandleProvidersListGet(); });

        CROW_ROUTE(m_Server, "/api/settings/providers")
            .methods("POST"_method)([this](crow::request const& req) { return HandleProviderCreatePost(req); });

        CROW_ROUTE(m_Server, "/api/settings/providers/save")
            .methods("POST"_method)([this](crow::request const& req) { return HandleProvidersSavePost(req); });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>")
            .methods("PUT"_method)([this](crow::request const& req, std::string const& name)
                                   { return HandleProviderUpdatePut(req, name); });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>")
            .methods("DELETE"_method)([this](std::string const& name) { return HandleProviderDelete(name); });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>/default")
            .methods("POST"_method)([this](std::string const& name) { return HandleProviderSetDefaultPost(name); });

        // ---- Script check API (Workflow Editor) ----
        CROW_ROUTE(m_Server, "/api/scripts/check")
            .methods("GET"_method)([this](crow::request const& req) { return HandleScriptCheckGet(req); });

        CROW_ROUTE(m_Server, "/api/scripts/registry").methods("GET"_method)([this]() { return HandleScriptRegistryGet(); });

        // ---- File existence check API (Workflow Editor) ----
        CROW_ROUTE(m_Server, "/api/files/check")
            .methods("GET"_method)([this](crow::request const& req) { return HandleFileCheckGet(req); });

        // ---- Log viewer API ----
        CROW_ROUTE(m_Server, "/api/log")
            .methods("GET"_method)([this](crow::request const& req) { return HandleLogGet(req); });

        CROW_ROUTE(m_Server, "/api/log/analyze-last-run")
            .methods("GET"_method)([this](crow::request const& req) { return HandleLogAnalyzeLastRunGet(req); });

        // ---- POST /api/task/<taskId>/heartbeat ----
        CROW_ROUTE(m_Server, "/api/task/<string>/heartbeat")
            .methods("POST"_method)(
                [](std::string const& taskId)
                {
                    JarvisAgent* app = App::g_App;
                    if (app == nullptr || app->GetWorkflowRuntimeManager() == nullptr)
                    {
                        crow::json::wvalue response;
                        response["error"] = "Runtime not available.";
                        return crow::response(503, response);
                    }

                    bool const found = app->GetWorkflowRuntimeManager()->Heartbeat(taskId);
                    crow::json::wvalue response;
                    if (found)
                    {
                        LOG_APP_INFO("[watchdog] Heartbeat received for task '{}'", taskId);
                        response["message"] = "Heartbeat received.";
                        return crow::response(200, response);
                    }
                    else
                    {
                        response["error"] = "Task not found or no active watchdog.";
                        return crow::response(404, response);
                    }
                });

        // ---- POST /api/shutdown ----
        CROW_ROUTE(m_Server, "/api/shutdown")
            .methods("POST"_method)(
                []()
                {
                    Core::g_Core->RequestQuit();
                    auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                    Core::g_Core->PushEvent(event);

                    crow::json::wvalue response;
                    response["message"] = "Shutdown initiated.";
                    return crow::response(200, response);
                });
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
            return MakeJsonResponse(200, response);
        }
        catch (const std::exception& e)
        {
            crow::json::wvalue error;
            error["error"] = e.what();
            return MakeJsonResponse(400, error);
        }
    }

    crow::response WebServer::HandleStatusGet()
    {
        crow::json::wvalue status;
        status["ok"] = true;

        // Workflows
        size_t registeredWorkflows = 0;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                registeredWorkflows = m_WorkflowRegistry->GetWorkflowIds().size();
            }
        }
        status["workflows_registered"] = static_cast<int64_t>(registeredWorkflows);

        size_t activeWorkflowRuns = 0;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRuntimeManager != nullptr)
            {
                activeWorkflowRuns = m_WorkflowRuntimeManager->GetActiveRunsSnapshot().size();
            }
        }
        status["workflow_runs_active"] = static_cast<int64_t>(activeWorkflowRuns);

        // Session managers
        JarvisAgent* app = App::g_App;
        status["session_managers_total"] = static_cast<int64_t>(app ? app->GetSessionManagerCount() : 0);
        status["session_managers_with_inflight"] = static_cast<int64_t>(app ? app->GetSessionManagersWithInflight() : 0);
        status["session_managers_inflight_total"] = static_cast<int64_t>(app ? app->GetSessionManagerInflightTotal() : 0);

        // WebSocket clients and accumulation stats
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            status["websocket_clients"] = static_cast<int64_t>(m_Clients.size());
            status["websocket_total_connects"] = static_cast<int64_t>(m_WsTotalConnects);
            status["websocket_total_disconnects"] = static_cast<int64_t>(m_WsTotalDisconnects);
            status["websocket_peak_clients"] = static_cast<int64_t>(m_WsPeakClients);
            status["websocket_peak_pending_broadcasts"] = static_cast<int64_t>(m_WsPeakPendingBroadcasts);
            status["websocket_pending_broadcasts"] = static_cast<int64_t>(m_PendingBroadcasts.size());
        }

        return MakeJsonResponse(200, status);
    }

    crow::response WebServer::HandleWorkflowsListGet()
    {
        WorkflowRegistry const* workflowRegistryPtr = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistryPtr = m_WorkflowRegistry;
        }

        if (workflowRegistryPtr == nullptr)
        {
            return MakeWorkflowJsonError(500, "registry_not_available", "Workflow registry is not available",
                                         "GET /api/workflows");
        }

        std::vector<std::string> workflowIds = workflowRegistryPtr->GetWorkflowIds();
        std::sort(workflowIds.begin(), workflowIds.end());

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;

        crow::json::wvalue::list workflowsList;
        workflowsList.reserve(workflowIds.size());

        for (std::string const& workflowId : workflowIds)
        {
            crow::json::wvalue workflowEntry;
            workflowEntry["id"] = workflowId;

            std::optional<WorkflowDefinition> workflowDefinition = workflowRegistryPtr->GetWorkflow(workflowId);
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

                workflowEntry["manual_start"] = workflowDefinition->m_ManualStart;
                workflowEntry["has_ai_call"] = workflowDefinition->m_HasAiCallTasks;
            }

            workflowsList.emplace_back(std::move(workflowEntry));
        }

        responseJson["workflows"] = std::move(workflowsList);
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleWorkflowsReloadPost()
    {
        WorkflowRegistry* workflowRegistryPtr = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistryPtr = m_WorkflowRegistry;
        }

        if (workflowRegistryPtr == nullptr)
        {
            return MakeWorkflowJsonError(500, "registry_not_available", "Workflow registry is not available",
                                         "POST /api/workflows/reload");
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "POST /api/workflows/reload");
        }

        workflowRegistryPtr->Clear();
        if (!workflowRegistryPtr->LoadDirectory(workflowsDirectoryAbsolute))
        {
            return MakeWorkflowJsonError(500, "workflow_registry_load_failed",
                                         "Failed to reload workflows directory: " + workflowsDirectoryAbsolute.string(),
                                         "POST /api/workflows/reload");
        }

        std::vector<std::string> workflowIds = workflowRegistryPtr->GetWorkflowIds();

        // Re-bind triggers so new/changed webhook/cron/file_watch triggers take effect.
        {
            TriggerEngine* triggerEngine = nullptr;
            {
                std::scoped_lock<std::mutex> const lock(m_Mutex);
                triggerEngine = m_TriggerEngine;
            }
            if (triggerEngine != nullptr)
            {
                triggerEngine->ClearAll();
                WorkflowTriggerBinder workflowTriggerBinder;
                workflowTriggerBinder.RegisterAll(*workflowRegistryPtr, *triggerEngine, /*fireAutoTriggers=*/false);
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["reloaded"] = true;
        responseJson["workflowCount"] = static_cast<int>(workflowIds.size());

        LOG_APP_INFO("Workflow registry reloaded from disk: {} workflows loaded (triggers re-bound)", workflowIds.size());

        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleWorkflowGet(std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "GET /api/workflows/{id}", workflowId);
        }

        WorkflowRegistry const* workflowRegistryPtr = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistryPtr = m_WorkflowRegistry;
        }

        if (workflowRegistryPtr == nullptr)
        {
            return MakeWorkflowJsonError(500, "registry_not_available", "Workflow registry is not available",
                                         "GET /api/workflows/{id}", workflowId);
        }

        std::optional<WorkflowDefinition> workflowDefinition = workflowRegistryPtr->GetWorkflow(workflowId);
        if (!workflowDefinition.has_value())
        {
            return MakeWorkflowJsonError(404, "workflow_not_found", "Workflow not found", "GET /api/workflows/{id}",
                                         workflowId);
        }

        fs::path workflowFilePath = fs::path(workflowDefinition->m_WorkflowFilePath);
        if (workflowFilePath.empty())
        {
            return MakeWorkflowJsonError(500, "workflow_path_missing", "Workflow definition is missing workflow file path",
                                         "GET /api/workflows/{id}", workflowId);
        }

        // Workflow file path should already be absolute from the registry
        workflowFilePath = fs::absolute(workflowFilePath).lexically_normal();

        std::string workflowJsonContent;
        if (!ReadTextFile(workflowFilePath, workflowJsonContent))
        {
            return MakeWorkflowJsonError(500, "workflow_read_failed",
                                         "Failed to read workflow file: " + workflowFilePath.string(),
                                         "GET /api/workflows/{id}", workflowId);
        }

        // Return the raw JCWF JSON as the response body (canonical).
        return MakeJsonTextResponse(200, workflowJsonContent);
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

        std::string writeErrorMessage;
        if (!WriteTextFileAtomic(targetPath, req.body, writeErrorMessage))
        {
            return MakeWorkflowJsonError(500, "workflow_write_failed", writeErrorMessage, "POST /api/workflows",
                                         parsedWorkflow.m_Id);
        }

        // Update the main registry so the orchestrator can run this workflow
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                std::string upsertErrorMessage;
                m_WorkflowRegistry->SaveOrUpdateWorkflowFromJson(req.body, targetPath, upsertErrorMessage);
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

        std::string writeErrorMessage;
        if (!WriteTextFileAtomic(targetPath, req.body, writeErrorMessage))
        {
            return MakeWorkflowJsonError(500, "workflow_write_failed", writeErrorMessage, "PUT /api/workflows/{id}",
                                         workflowId);
        }

        // Update the main registry so the orchestrator can run this workflow
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                std::string upsertErrorMessage;
                m_WorkflowRegistry->SaveOrUpdateWorkflowFromJson(req.body, targetPath, upsertErrorMessage);
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
        bool const removed = fs::remove(workflowFilePath, errorCode);
        if (errorCode)
        {
            return MakeWorkflowJsonError(500, "workflow_delete_failed",
                                         "Failed to delete workflow file: " + workflowFilePath.string() +
                                             " error=" + errorCode.message(),
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
        return MakeJsonResponse(200, responseJson);
    }

    // ---- Workflow versioning ----

    crow::response WebServer::HandleWorkflowVersionsListGet(std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "GET /api/workflows/{id}/versions", workflowId);
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "GET /api/workflows/{id}/versions", workflowId);
        }

        fs::path const historyDir = workflowsDirectoryAbsolute / ".history" / workflowId;
        std::vector<std::string> timestamps;

        if (fs::exists(historyDir) && fs::is_directory(historyDir))
        {
            for (auto const& entry : fs::directory_iterator(historyDir))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".jcwf")
                {
                    timestamps.push_back(entry.path().stem().string());
                }
            }
        }

        // Sort descending (newest first)
        std::sort(timestamps.begin(), timestamps.end(), std::greater<>());

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["workflowId"] = workflowId;

        std::vector<crow::json::wvalue> versionList;
        for (auto const& ts : timestamps)
        {
            crow::json::wvalue versionEntry;
            versionEntry["timestamp"] = ts;

            // Compute file size
            fs::path const versionPath = historyDir / (ts + ".jcwf");
            std::error_code ec;
            auto const fileSize = fs::file_size(versionPath, ec);
            if (!ec)
            {
                versionEntry["sizeBytes"] = static_cast<int64_t>(fileSize);
            }
            versionList.push_back(std::move(versionEntry));
        }
        responseJson["versions"] = std::move(versionList);
        responseJson["count"] = static_cast<int>(timestamps.size());
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleWorkflowVersionGetGet(std::string const& workflowId, std::string const& timestamp)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "GET /api/workflows/{id}/versions/{ts}", workflowId);
        }

        // Sanitize timestamp: allow only alphanumeric and 'T'
        for (char const c : timestamp)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != 'T')
            {
                return MakeWorkflowJsonError(400, "invalid_timestamp", "Timestamp contains invalid characters",
                                             "GET /api/workflows/{id}/versions/{ts}", workflowId);
            }
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "GET /api/workflows/{id}/versions/{ts}",
                                         workflowId);
        }

        fs::path const versionPath = workflowsDirectoryAbsolute / ".history" / workflowId / (timestamp + ".jcwf");
        if (!fs::exists(versionPath))
        {
            return MakeWorkflowJsonError(404, "version_not_found", "Version not found: " + timestamp,
                                         "GET /api/workflows/{id}/versions/{ts}", workflowId);
        }

        std::ifstream ifs(versionPath);
        if (!ifs.is_open())
        {
            return MakeWorkflowJsonError(500, "read_failed", "Failed to read version file",
                                         "GET /api/workflows/{id}/versions/{ts}", workflowId);
        }

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        crow::response resp(200, content);
        resp.set_header("Content-Type", "application/json");
        return resp;
    }

    crow::response WebServer::HandleWorkflowVersionRestorePost(std::string const& workflowId, std::string const& timestamp)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "POST /api/workflows/{id}/versions/{ts}/restore", workflowId);
        }

        // Sanitize timestamp
        for (char const c : timestamp)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != 'T')
            {
                return MakeWorkflowJsonError(400, "invalid_timestamp", "Timestamp contains invalid characters",
                                             "POST /api/workflows/{id}/versions/{ts}/restore", workflowId);
            }
        }

        std::string errorMessage;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(errorMessage);
        if (workflowsDirectoryAbsolute.empty())
        {
            return MakeWorkflowJsonError(500, "config_error", errorMessage, "POST /api/workflows/{id}/versions/{ts}/restore",
                                         workflowId);
        }

        fs::path const versionPath = workflowsDirectoryAbsolute / ".history" / workflowId / (timestamp + ".jcwf");
        if (!fs::exists(versionPath))
        {
            return MakeWorkflowJsonError(404, "version_not_found", "Version not found: " + timestamp,
                                         "POST /api/workflows/{id}/versions/{ts}/restore", workflowId);
        }

        fs::path const targetPath = (workflowsDirectoryAbsolute / (workflowId + ".jcwf")).lexically_normal();

        // Read the version content
        std::ifstream ifs(versionPath);
        if (!ifs.is_open())
        {
            return MakeWorkflowJsonError(500, "read_failed", "Failed to read version file",
                                         "POST /api/workflows/{id}/versions/{ts}/restore", workflowId);
        }
        std::string versionContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();

        // Backup current before restoring
        if (fs::exists(targetPath))
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

        // Write the restored version
        std::string writeErrorMessage;
        if (!WriteTextFileAtomic(targetPath, versionContent, writeErrorMessage))
        {
            return MakeWorkflowJsonError(500, "restore_failed", writeErrorMessage,
                                         "POST /api/workflows/{id}/versions/{ts}/restore", workflowId);
        }

        // Update registry
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                std::string upsertErrorMessage;
                m_WorkflowRegistry->SaveOrUpdateWorkflowFromJson(versionContent, targetPath, upsertErrorMessage);
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["workflowId"] = workflowId;
        responseJson["restoredVersion"] = timestamp;
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

    crow::response WebServer::HandleWorkflowRunPost(crow::request const& req, std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "POST /api/workflows/{id}/run", workflowId);
        }

        WorkflowRegistry* workflowRegistry = nullptr;
        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        // Validate that the workflow exists and enforce manual_start flag.
        if (workflowRegistry != nullptr)
        {
            std::optional<WorkflowDefinition> definition = workflowRegistry->GetWorkflow(workflowId);
            if (!definition.has_value())
            {
                return MakeWorkflowJsonError(404, "workflow_not_found",
                                             "No workflow with id '" + workflowId + "' is registered",
                                             "POST /api/workflows/{id}/run", workflowId);
            }
            if (!definition->m_ManualStart)
            {
                return MakeWorkflowJsonError(403, "manual_start_disabled",
                                             "This workflow has manual_start set to false and cannot be started manually",
                                             "POST /api/workflows/{id}/run", workflowId);
            }
        }

        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(500, "runtime_not_available", "Workflow runtime manager is not available",
                                         "POST /api/workflows/{id}/run", workflowId);
        }

        // Parse optional JSON body: { "context": { "key": "value", ... } }
        ContextMap context;
        if (!req.body.empty())
        {
            try
            {
                simdjson::ondemand::parser parser;
                simdjson::padded_string json = simdjson::padded_string(req.body.data(), req.body.size());
                simdjson::ondemand::document document = parser.iterate(json);

                auto contextResult = document["context"].get_object();
                if (contextResult.error() == simdjson::SUCCESS)
                {
                    for (auto field : contextResult.value())
                    {
                        auto keyResult = field.unescaped_key();
                        if (keyResult.error() != simdjson::SUCCESS)
                        {
                            continue;
                        }

                        std::string_view keyView = keyResult.value();
                        std::string key(keyView.begin(), keyView.end());

                        simdjson::ondemand::value value = field.value();
                        auto stringResult = value.get_string();
                        if (stringResult.error() == simdjson::SUCCESS)
                        {
                            std::string_view valueView = stringResult.value();
                            context[key] = ContextValue{std::string(valueView.begin(), valueView.end())};
                        }
                    }
                }
            }
            catch (...)
            {
                // Malformed body is not fatal — run without context.
                LOG_APP_WARN("HandleWorkflowRunPost: failed to parse request body for context (workflow '{}')", workflowId);
            }
        }

        std::string runId;
        if (context.empty())
        {
            runId = workflowRuntimeManager->EnqueueWorkflowRunAndGetRunId(workflowId);
        }
        else
        {
            runId = workflowRuntimeManager->EnqueueWorkflowRunWithContextAndGetRunId(workflowId, std::string(), context);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["enqueued"] = true;
        responseJson["id"] = workflowId;
        responseJson["runId"] = runId;

        BroadcastWorkflowRunsSnapshot();
        BroadcastWorkflowRunsLastSnapshot();
        return MakeJsonResponse(202, responseJson);
    }

    crow::response WebServer::HandleWorkflowCleanDelete(std::string const& workflowId)
    {
        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "Workflow id contains invalid characters",
                                         "DELETE /api/workflows/{id}/clean", workflowId);
        }

        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(500, "runtime_not_available", "Workflow runtime manager is not available",
                                         "DELETE /api/workflows/{id}/clean", workflowId);
        }

        std::string errorMessage;
        bool const ok = workflowRuntimeManager->CleanWorkflow(workflowId, errorMessage);

        crow::json::wvalue responseJson;
        responseJson["ok"] = ok;
        responseJson["id"] = workflowId;

        if (!ok)
        {
            responseJson["error"] = errorMessage;
            return MakeJsonResponse(ok ? 200 : 409, responseJson);
        }

        return MakeJsonResponse(200, responseJson);
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
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured on web server",
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

        return MakeJsonResponse(200, responseJson);
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
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured on web server",
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

        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleWorkflowRunGet(std::string const& runId)
    {
        if (runId.empty())
        {
            return MakeWorkflowJsonError(400, "invalid_run_id", "Run id is empty", "GET /api/workflow-runs/{runId}");
        }

        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured on web server",
                                         "GET /api/workflow-runs/{runId}");
        }

        WorkflowRun run;
        if (!workflowRuntimeManager->TryGetRunById(runId, run))
        {
            return MakeWorkflowJsonError(404, "run_not_found", "Run not found: " + runId, "GET /api/workflow-runs/{runId}",
                                         runId);
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

            if (!taskState.m_CapturedStdout.empty())
            {
                taskJson["capturedStdout"] = taskState.m_CapturedStdout;
            }
            if (!taskState.m_CapturedStderr.empty())
            {
                taskJson["capturedStderr"] = taskState.m_CapturedStderr;
            }

            tasksJson.push_back(std::move(taskJson));
        }

        runJson["tasks"] = std::move(tasksJson);
        responseJson["run"] = std::move(runJson);

        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleWorkflowRunCancelPost(std::string const& runId)
    {
        if (runId.empty())
        {
            return MakeWorkflowJsonError(400, "invalid_run_id", "Run id is empty", "POST /api/workflow-runs/{runId}/cancel");
        }

        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured on web server",
                                         "POST /api/workflow-runs/{runId}/cancel", runId);
        }

        bool const cancelRequested = workflowRuntimeManager->RequestCancelRun(runId);
        if (!cancelRequested)
        {
            return MakeWorkflowJsonError(404, "run_not_found", "Run not found or not active: " + runId,
                                         "POST /api/workflow-runs/{runId}/cancel", runId);
        }

        // Best-effort: push an updated snapshot to any connected editor/dashboard clients.
        BroadcastWorkflowRunsSnapshot();
        BroadcastWorkflowRunsLastSnapshot();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["cancelRequested"] = true;
        responseJson["runId"] = runId;

        return MakeJsonResponse(202, responseJson);
    }

    crow::response WebServer::HandleWorkflowRunPausePost(std::string const& runId)
    {
        if (runId.empty())
        {
            return MakeWorkflowJsonError(400, "invalid_run_id", "Run id is empty", "POST /api/workflow-runs/{runId}/pause");
        }

        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured on web server",
                                         "POST /api/workflow-runs/{runId}/pause", runId);
        }

        bool const ok = workflowRuntimeManager->RequestPauseRun(runId);
        if (!ok)
        {
            return MakeWorkflowJsonError(404, "run_not_found",
                                         "Run not found, not active, or already cancelled/stopped: " + runId,
                                         "POST /api/workflow-runs/{runId}/pause", runId);
        }

        BroadcastWorkflowRunsSnapshot();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["paused"] = true;
        responseJson["runId"] = runId;

        return MakeJsonResponse(202, responseJson);
    }

    crow::response WebServer::HandleWorkflowRunResumePost(std::string const& runId)
    {
        if (runId.empty())
        {
            return MakeWorkflowJsonError(400, "invalid_run_id", "Run id is empty", "POST /api/workflow-runs/{runId}/resume");
        }

        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured on web server",
                                         "POST /api/workflow-runs/{runId}/resume", runId);
        }

        bool const ok = workflowRuntimeManager->RequestResumeRun(runId);
        if (!ok)
        {
            return MakeWorkflowJsonError(404, "run_not_found", "Run not found, not active, or not paused: " + runId,
                                         "POST /api/workflow-runs/{runId}/resume", runId);
        }

        BroadcastWorkflowRunsSnapshot();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["resumed"] = true;
        responseJson["runId"] = runId;

        return MakeJsonResponse(202, responseJson);
    }

    crow::response WebServer::HandleWorkflowRunStopPost(std::string const& runId)
    {
        if (runId.empty())
        {
            return MakeWorkflowJsonError(400, "invalid_run_id", "Run id is empty", "POST /api/workflow-runs/{runId}/stop");
        }

        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured on web server",
                                         "POST /api/workflow-runs/{runId}/stop", runId);
        }

        bool const ok = workflowRuntimeManager->RequestStopRun(runId);
        if (!ok)
        {
            return MakeWorkflowJsonError(404, "run_not_found", "Run not found, not active, or already cancelled: " + runId,
                                         "POST /api/workflow-runs/{runId}/stop", runId);
        }

        BroadcastWorkflowRunsSnapshot();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["stopRequested"] = true;
        responseJson["runId"] = runId;

        return MakeJsonResponse(202, responseJson);
    }

    crow::response WebServer::HandleN8nStartPost(crow::request const& req)
    {
        // Expected body:
        // {
        //   "workflowId": "...",
        //   "runId": "..." (optional),
        //   "callbackUrl": "..." (optional),
        //   "context": { "k": "v", ... } (optional)
        // }

        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string json(req.body);
            auto doc = parser.iterate(json);

            auto workflowIdField = doc["workflowId"].get_string();
            if (workflowIdField.error() != simdjson::SUCCESS)
            {
                return MakeWorkflowJsonError(400, "missing_workflow_id", "Missing required field: workflowId",
                                             "POST /api/integrations/n8n/start");
            }

            std::string const workflowId = std::string(workflowIdField.value());
            if (!IsValidWorkflowId(workflowId))
            {
                return MakeWorkflowJsonError(400, "invalid_workflow_id", "workflowId contains invalid characters",
                                             "POST /api/integrations/n8n/start", workflowId);
            }

            std::string runId;
            {
                auto runIdField = doc["runId"].get_string();
                if (runIdField.error() == simdjson::SUCCESS)
                {
                    runId = std::string(runIdField.value());
                }
            }

            std::string taskName;
            {
                auto taskNameField = doc["taskName"].get_string();
                if (taskNameField.error() == simdjson::SUCCESS)
                {
                    taskName = std::string(taskNameField.value());
                }
            }

            if (taskName.empty())
            {
                taskName = "n8n";
            }
            else if (!IsValidTaskName(taskName))
            {
                return MakeWorkflowJsonError(400, "invalid_task_name", "taskName contains invalid characters",
                                             "POST /api/integrations/n8n/start", workflowId);
            }

            if (runId.empty())
            {
                runId = GenerateIntegrationRunId(workflowId);
            }

            // Persist the raw request body to disk for traceability.
            std::string workflowsDirError;
            fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(workflowsDirError);
            if (workflowsDirectoryAbsolute.empty())
            {
                return MakeWorkflowJsonError(500, "config_error", workflowsDirError, "POST /api/integrations/n8n/start");
            }

            fs::path const runsRoot = workflowsDirectoryAbsolute / workflowId / taskName / "n8n" / runId;
            fs::path const requestJsonPath = runsRoot / "request.json";

            std::string writeError;
            if (!WriteTextFileAtomic(requestJsonPath, req.body, writeError))
            {
                return MakeWorkflowJsonError(500, "write_failed", writeError, "POST /api/integrations/n8n/start",
                                             workflowId);
            }

            // Build run context (string -> string) for task executors.
            ContextMap context;
            context["n8n_request_path"].m_Value = requestJsonPath.string();
            context["n8n_task"].m_Value = taskName;

            {
                auto callbackUrlField = doc["callbackUrl"].get_string();
                if (callbackUrlField.error() == simdjson::SUCCESS)
                {
                    context["callbackUrl"].m_Value = std::string(callbackUrlField.value());
                }
            }

            // Merge any provided context fields.
            auto contextField = doc["context"];
            if (contextField.error() == simdjson::SUCCESS)
            {
                simdjson::ondemand::object ctxObj;
                if (contextField.get_object().get(ctxObj) == simdjson::SUCCESS)
                {
                    for (auto field : ctxObj)
                    {
                        simdjson::simdjson_result<std::string_view> keyResult = field.unescaped_key();
                        if (keyResult.error() != simdjson::SUCCESS)
                        {
                            continue;
                        }

                        std::string const key = std::string(keyResult.value());

                        if (field.value().is_string())
                        {
                            auto strValue = field.value().get_string();
                            if (strValue.error() == simdjson::SUCCESS)
                            {
                                context[key].m_Value = std::string(strValue.value());
                            }
                        }
                        else
                        {
                            auto rawJson = field.value().get_raw_json_string();
                            if (rawJson.error() == simdjson::SUCCESS)
                            {
                                context[key].m_Value = std::string(rawJson.value().raw());
                            }
                        }
                    }
                }
            }

            WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
            {
                std::scoped_lock<std::mutex> const lock(m_Mutex);
                workflowRuntimeManager = m_WorkflowRuntimeManager;
            }

            if (workflowRuntimeManager == nullptr)
            {
                return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured on web server",
                                             "POST /api/integrations/n8n/start", workflowId);
            }

            std::string const enqueuedRunId =
                workflowRuntimeManager->EnqueueWorkflowRunWithContextAndGetRunId(workflowId, runId, context);

            crow::json::wvalue responseJson;
            responseJson["ok"] = true;
            responseJson["workflowId"] = workflowId;
            responseJson["runId"] = enqueuedRunId;
            responseJson["requestPath"] = requestJsonPath.string();

            BroadcastWorkflowRunsSnapshot();
            BroadcastWorkflowRunsLastSnapshot();
            return MakeJsonResponse(202, responseJson);
        }
        catch (std::exception const& e)
        {
            return MakeWorkflowJsonError(400, "invalid_json", e.what(), "POST /api/integrations/n8n/start");
        }
    }

    crow::response WebServer::HandleWebhookPost(crow::request const& req, std::string const& workflowId)
    {
        static constexpr char const* kEndpoint = "POST /api/webhook/{id}";

        if (!IsValidWorkflowId(workflowId))
        {
            return MakeWorkflowJsonError(400, "invalid_workflow_id", "workflowId contains invalid characters", kEndpoint,
                                         workflowId);
        }

        // ---- Look up the webhook trigger for this workflow ----
        TriggerEngine* triggerEngine = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            triggerEngine = m_TriggerEngine;
        }

        if (triggerEngine == nullptr)
        {
            return MakeWorkflowJsonError(501, "not_configured", "Trigger engine not configured", kEndpoint, workflowId);
        }

        TriggerEngine::WebhookTriggerInstance const* webhookTrigger = triggerEngine->GetWebhookTrigger(workflowId);
        if (webhookTrigger == nullptr)
        {
            return MakeWorkflowJsonError(404, "no_webhook_trigger",
                                         "No webhook trigger registered for workflow '" + workflowId + "'", kEndpoint,
                                         workflowId);
        }

        if (!webhookTrigger->m_IsEnabled)
        {
            return MakeWorkflowJsonError(403, "trigger_disabled",
                                         "Webhook trigger for workflow '" + workflowId + "' is disabled", kEndpoint,
                                         workflowId);
        }

        // ---- HMAC-SHA256 signature verification ----
        if (!webhookTrigger->m_Secret.empty())
        {
            std::string signatureHeader;
            auto const it = req.headers.find("X-Webhook-Signature");
            if (it != req.headers.end())
            {
                signatureHeader = it->second;
            }

            if (signatureHeader.empty())
            {
                LOG_APP_WARN("WebServer::HandleWebhookPost: missing X-Webhook-Signature header for workflow '{}'",
                             workflowId);
                return MakeWorkflowJsonError(401, "missing_signature",
                                             "X-Webhook-Signature header is required for this webhook", kEndpoint,
                                             workflowId);
            }

            if (!VerifyHmacSignature(webhookTrigger->m_Secret, req.body, signatureHeader))
            {
                LOG_APP_WARN("WebServer::HandleWebhookPost: HMAC signature mismatch for workflow '{}'", workflowId);
                return MakeWorkflowJsonError(401, "invalid_signature", "HMAC signature verification failed", kEndpoint,
                                             workflowId);
            }
        }

        // ---- Parse optional context from request body ----
        ContextMap context;
        std::string runId;
        std::string callbackUrl;

        if (!req.body.empty())
        {
            try
            {
                simdjson::ondemand::parser parser;
                simdjson::padded_string json(req.body);
                auto doc = parser.iterate(json);

                // Optional runId
                {
                    auto runIdField = doc["runId"].get_string();
                    if (runIdField.error() == simdjson::SUCCESS)
                    {
                        runId = std::string(runIdField.value());
                    }
                }

                // Optional callbackUrl
                {
                    auto callbackUrlField = doc["callbackUrl"].get_string();
                    if (callbackUrlField.error() == simdjson::SUCCESS)
                    {
                        callbackUrl = std::string(callbackUrlField.value());
                        context["callbackUrl"].m_Value = callbackUrl;
                    }
                }

                // Optional context object
                auto contextField = doc["context"];
                if (contextField.error() == simdjson::SUCCESS)
                {
                    simdjson::ondemand::object ctxObj;
                    if (contextField.get_object().get(ctxObj) == simdjson::SUCCESS)
                    {
                        for (auto field : ctxObj)
                        {
                            simdjson::simdjson_result<std::string_view> keyResult = field.unescaped_key();
                            if (keyResult.error() != simdjson::SUCCESS)
                            {
                                continue;
                            }

                            std::string const key = std::string(keyResult.value());

                            if (field.value().is_string())
                            {
                                auto strValue = field.value().get_string();
                                if (strValue.error() == simdjson::SUCCESS)
                                {
                                    context[key].m_Value = std::string(strValue.value());
                                }
                            }
                            else
                            {
                                auto rawJson = field.value().get_raw_json_string();
                                if (rawJson.error() == simdjson::SUCCESS)
                                {
                                    context[key].m_Value = std::string(rawJson.value().raw());
                                }
                            }
                        }
                    }
                }
            }
            catch (std::exception const& e)
            {
                return MakeWorkflowJsonError(400, "invalid_json", e.what(), kEndpoint, workflowId);
            }
        }

        // ---- Persist request for traceability ----
        std::string workflowsDirError;
        fs::path const workflowsDirectoryAbsolute = GetWorkflowsDirectoryAbsolute(workflowsDirError);
        if (!workflowsDirectoryAbsolute.empty() && !req.body.empty())
        {
            if (runId.empty())
            {
                runId = GenerateIntegrationRunId(workflowId);
            }

            fs::path const runsRoot = workflowsDirectoryAbsolute / workflowId / "webhook" / runId;
            fs::path const requestJsonPath = runsRoot / "request.json";

            std::string writeError;
            if (WriteTextFileAtomic(requestJsonPath, req.body, writeError))
            {
                context["webhook_request_path"].m_Value = requestJsonPath.string();
            }
            else
            {
                LOG_APP_WARN("WebServer::HandleWebhookPost: failed to persist request.json: {}", writeError);
            }
        }

        if (runId.empty())
        {
            runId = GenerateIntegrationRunId(workflowId);
        }

        context["webhook_trigger_id"].m_Value = webhookTrigger->m_TriggerId;

        // ---- Enqueue the workflow run ----
        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }

        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured", kEndpoint,
                                         workflowId);
        }

        std::string const enqueuedRunId =
            workflowRuntimeManager->EnqueueWorkflowRunWithContextAndGetRunId(workflowId, runId, context);

        LOG_APP_INFO("WebServer::HandleWebhookPost: enqueued run '{}' for workflow '{}' (trigger '{}')", enqueuedRunId,
                     workflowId, webhookTrigger->m_TriggerId);

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["workflowId"] = workflowId;
        responseJson["runId"] = enqueuedRunId;
        responseJson["triggerId"] = webhookTrigger->m_TriggerId;

        BroadcastWorkflowRunsSnapshot();
        BroadcastWorkflowRunsLastSnapshot();
        return MakeJsonResponse(202, responseJson);
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
        // GET /api/files/check?path=OpenSSH_2k.log
        // Returns: { ok, path, exists }

        std::string filePath;
        auto pathParam = req.url_params.get("path");
        if (pathParam != nullptr)
        {
            filePath = std::string(pathParam);
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

        // Resolve and verify it stays within CWD
        fs::path const launchCWD = Core::g_Core ? Core::g_Core->GetLaunchCWDAbsolute() : fs::path{};
        if (launchCWD.empty())
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = "server_error";
            responseJson["message"] = "Cannot determine JarvisAgent working directory.";
            return MakeJsonResponse(500, responseJson);
        }

        fs::path const absolutePath = (launchCWD / fs::path(filePath)).lexically_normal();
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

    crow::response WebServer::HandleLogGet(crow::request const& req)
    {
        // GET /api/log?tail=N        — return last N lines (initial load)
        // GET /api/log?offset=N      — return lines appended since byte offset N (delta polling)
        // Returns: { ok, lines[], byteOffset, totalSize }

        int tailLines = 5000;
        auto const tailParam = req.url_params.get("tail");
        if (tailParam != nullptr)
        {
            tailLines = std::clamp(std::atoi(tailParam), 1, 200000);
        }

        int64_t fromOffset = -1;
        auto const offsetParam = req.url_params.get("offset");
        if (offsetParam != nullptr)
        {
            fromOffset = std::atoll(offsetParam);
        }

        std::string const logPath = "log/log.txt";
        std::ifstream file(logPath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            crow::json::wvalue resp;
            resp["ok"] = false;
            resp["error"] = "Log file not found";
            return MakeJsonResponse(404, resp);
        }

        int64_t const fileSize = static_cast<int64_t>(file.tellg());

        // --- Delta mode: read from offset to end ---
        if (fromOffset >= 0)
        {
            if (fromOffset >= fileSize)
            {
                // No new data (or file was truncated/rotated).
                crow::json::wvalue resp;
                resp["ok"] = true;
                resp["lines"] = crow::json::wvalue::list();
                resp["byteOffset"] = fileSize;
                resp["totalSize"] = fileSize;
                return MakeJsonResponse(200, resp);
            }

            int64_t const deltaSize = fileSize - fromOffset;
            file.seekg(fromOffset);
            std::string content(static_cast<size_t>(deltaSize), '\0');
            file.read(content.data(), deltaSize);

            crow::json::wvalue::list linesJson;
            size_t start = 0;
            for (size_t i = 0; i < content.size(); ++i)
            {
                if (content[i] == '\n')
                {
                    size_t end = (i > 0 && content[i - 1] == '\r') ? i - 1 : i;
                    linesJson.push_back(content.substr(start, end - start));
                    start = i + 1;
                }
            }
            // Trailing partial line (no final newline yet)
            if (start < content.size())
            {
                linesJson.push_back(content.substr(start));
            }

            crow::json::wvalue resp;
            resp["ok"] = true;
            resp["lines"] = std::move(linesJson);
            resp["byteOffset"] = fileSize;
            resp["totalSize"] = fileSize;
            return MakeJsonResponse(200, resp);
        }

        // --- Tail mode: read last N lines ---
        static constexpr int64_t kChunkSize = 65536;
        std::string accumulated;
        int64_t readPos = fileSize;
        int newlineCount = 0;

        while (readPos > 0 && newlineCount <= tailLines)
        {
            int64_t const chunkStart = std::max(int64_t(0), readPos - kChunkSize);
            int64_t const chunkLen = readPos - chunkStart;

            file.seekg(chunkStart);
            std::string chunk(static_cast<size_t>(chunkLen), '\0');
            file.read(chunk.data(), chunkLen);

            for (char c : chunk)
            {
                if (c == '\n')
                    ++newlineCount;
            }

            accumulated = chunk + accumulated;
            readPos = chunkStart;
        }

        // Split into lines, take last tailLines
        std::vector<std::string> allLines;
        {
            size_t start = 0;
            for (size_t i = 0; i < accumulated.size(); ++i)
            {
                if (accumulated[i] == '\n')
                {
                    size_t end = (i > 0 && accumulated[i - 1] == '\r') ? i - 1 : i;
                    allLines.push_back(accumulated.substr(start, end - start));
                    start = i + 1;
                }
            }
            if (start < accumulated.size())
            {
                allLines.push_back(accumulated.substr(start));
            }
        }

        size_t const startIdx =
            allLines.size() > static_cast<size_t>(tailLines) ? allLines.size() - static_cast<size_t>(tailLines) : 0;

        crow::json::wvalue::list linesJson;
        for (size_t i = startIdx; i < allLines.size(); ++i)
        {
            linesJson.push_back(std::move(allLines[i]));
        }

        crow::json::wvalue resp;
        resp["ok"] = true;
        resp["lines"] = std::move(linesJson);
        resp["byteOffset"] = fileSize;
        resp["totalSize"] = fileSize;
        return MakeJsonResponse(200, resp);
    }

    crow::response WebServer::HandleLogAnalyzeLastRunGet(crow::request const& req)
    {
        // GET /api/log/analyze-last-run?index=N
        // Log-based analysis.  index=0 (default) is the most recent run,
        // index=1 is the second-to-last, etc.  Returns runIndex + totalRuns
        // so the frontend can cycle through all runs.

        int requestedIndex = 0;
        {
            auto const* idxParam = req.url_params.get("index");
            if (idxParam != nullptr)
            {
                try
                {
                    requestedIndex = std::stoi(idxParam);
                }
                catch (...)
                {
                    requestedIndex = 0;
                }
                if (requestedIndex < 0)
                    requestedIndex = 0;
            }
        }

        std::ifstream logFile("log/log.txt", std::ios::binary);
        if (!logFile.is_open())
        {
            crow::json::wvalue resp;
            resp["ok"] = false;
            resp["error"] = "Log file not found";
            return MakeJsonResponse(404, resp);
        }

        std::vector<std::string> lines;
        {
            std::string line;
            while (std::getline(logFile, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(std::move(line));
            }
        }

        // Collect ALL "[workflow] run '...' started (workflow '...')" markers
        // so we know totalRuns and can index into them.
        static std::string const kRunMarker = "[workflow] run '";
        static std::string const kStartedToken = "' started (workflow '";

        struct RunStartMarker
        {
            int lineIdx;
            std::string runId;
            std::string workflowId;
        };
        std::vector<RunStartMarker> runStarts;

        for (int i = 0; i < static_cast<int>(lines.size()); ++i)
        {
            auto const& line = lines[static_cast<size_t>(i)];
            auto const markerPos = line.find(kRunMarker);
            if (markerPos == std::string::npos)
                continue;

            auto const runIdStart = markerPos + kRunMarker.size();
            auto const startedPos = line.find(kStartedToken, runIdStart);
            if (startedPos == std::string::npos)
                continue;

            RunStartMarker marker;
            marker.lineIdx = i;
            marker.runId = line.substr(runIdStart, startedPos - runIdStart);

            auto const wfIdStart = startedPos + kStartedToken.size();
            auto const wfIdEnd = line.find("')", wfIdStart);
            if (wfIdEnd != std::string::npos)
            {
                marker.workflowId = line.substr(wfIdStart, wfIdEnd - wfIdStart);
            }
            runStarts.push_back(std::move(marker));
        }

        int const totalRuns = static_cast<int>(runStarts.size());

        if (totalRuns == 0)
        {
            crow::json::wvalue resp;
            resp["ok"] = true;
            resp["found"] = false;
            resp["totalRuns"] = 0;
            resp["message"] = "No workflow run start found in log.";
            return MakeJsonResponse(200, resp);
        }

        // Wrap index around so cycling is seamless.
        int const runIndex = requestedIndex % totalRuns;

        // runStarts is ordered oldest-first; we want index 0 = newest.
        auto const& selected = runStarts[static_cast<size_t>(totalRuns - 1 - runIndex)];
        int const startLineIdx = selected.lineIdx;
        std::string const& runId = selected.runId;
        std::string const& workflowId = selected.workflowId;

        // Find the matching completion line (same run ID) after the start line.
        std::string const completedMarker = kRunMarker + runId + "' completed";
        std::string const failedMarker = kRunMarker + runId + "' failed";
        std::string const cancelledMarker = kRunMarker + runId + "' cancelled";
        std::string const stoppedMarker = kRunMarker + runId + "' stopped";

        int endLineIdx = -1;
        std::string state = "running";

        for (size_t i = static_cast<size_t>(startLineIdx) + 1; i < lines.size(); ++i)
        {
            auto const& line = lines[i];
            if (line.find(completedMarker) != std::string::npos)
            {
                endLineIdx = static_cast<int>(i);
                state = "completed";
                break;
            }
            if (line.find(failedMarker) != std::string::npos)
            {
                endLineIdx = static_cast<int>(i);
                state = "failed";
                break;
            }
            if (line.find(cancelledMarker) != std::string::npos)
            {
                endLineIdx = static_cast<int>(i);
                state = "cancelled";
                break;
            }
            if (line.find(stoppedMarker) != std::string::npos)
            {
                endLineIdx = static_cast<int>(i);
                state = "stopped";
                break;
            }
        }

        int const searchEnd = endLineIdx >= 0 ? endLineIdx : static_cast<int>(lines.size());

        // Extract timestamp from a log line:  "[YYYY-MM-DD HH:MM:SS.mmm] ..."
        auto extractTimestamp = [](std::string const& line) -> std::string
        {
            if (line.size() > 25 && line[0] == '[')
            {
                auto const endBracket = line.find(']');
                if (endBracket != std::string::npos)
                    return line.substr(1, endBracket - 1);
            }
            return "";
        };

        std::string const startedAt = extractTimestamp(lines[static_cast<size_t>(startLineIdx)]);
        std::string const completedAt = endLineIdx >= 0 ? extractTimestamp(lines[static_cast<size_t>(endLineIdx)]) : "";

        // Collect issue lines between start and end.
        // Match by log-level tags: [error], [critical], [warning], [warn]
        // Also match [workflow] lines containing "failed" or "skipping" (task-level events).
        // Only include lines that mention this run's runId or workflowId to avoid
        // false positives from concurrent workflow runs.
        crow::json::wvalue::list issuesJson;
        int issueCount = 0;

        for (int i = startLineIdx; i < searchEnd; ++i)
        {
            auto const& line = lines[static_cast<size_t>(i)];

            // Skip lines that don't belong to this run (concurrent runs are interleaved).
            if (line.find(runId) == std::string::npos && line.find(workflowId) == std::string::npos)
                continue;

            std::string severity;

            if (line.find("] [error]") != std::string::npos || line.find("] [critical]") != std::string::npos)
            {
                severity = "error";
            }
            else if (line.find("] [warning]") != std::string::npos || line.find("] [warn]") != std::string::npos)
            {
                severity = "warning";
            }
            else if (line.find("[workflow]") != std::string::npos &&
                     (line.find("failed") != std::string::npos || line.find("skipping") != std::string::npos))
            {
                severity = "error";
            }
            else
            {
                continue;
            }

            crow::json::wvalue issueJson;
            issueJson["line"] = i + 1; // 1-indexed for display
            issueJson["severity"] = severity;
            issueJson["text"] = SanitizeUtf8(line);
            issuesJson.push_back(std::move(issueJson));
            ++issueCount;
        }

        crow::json::wvalue resp;
        resp["ok"] = true;
        resp["found"] = true;
        resp["runIndex"] = runIndex;
        resp["totalRuns"] = totalRuns;
        resp["runId"] = runId;
        resp["workflowId"] = workflowId;
        resp["state"] = state;
        resp["startedAt"] = startedAt;
        resp["completedAt"] = completedAt;
        resp["startLine"] = startLineIdx + 1; // 1-indexed
        resp["endLine"] = endLineIdx >= 0 ? endLineIdx + 1 : -1;
        resp["issues"] = std::move(issuesJson);
        resp["issueCount"] = issueCount;

        return MakeJsonResponse(200, resp);
    }

    void WebServer::RegisterWebSocket()
    {
        CROW_WEBSOCKET_ROUTE(m_Server, "/ws")
            .onopen(
                [this](crow::websocket::connection& conn)
                {
                    {
                        std::lock_guard<std::mutex> lock(m_Mutex);
                        m_Clients.insert(&conn);
                        m_ClientCount.store(m_Clients.size(), std::memory_order_relaxed);
                        ++m_WsTotalConnects;
                        if (m_Clients.size() > m_WsPeakClients)
                        {
                            m_WsPeakClients = m_Clients.size();
                        }

                        // Queue current session manager states for the new client.
                        // Cannot call conn.send_text() from onopen (CROW_ENFORCE_WS_SPEC).
                        // Broadcasting to all clients is harmless — existing clients just get a refresh.
                        JarvisAgent* app = App::g_App;
                        if (app)
                        {
                            app->ForEachSessionManager(
                                [this](SessionManager& sm)
                                {
                                    crow::json::wvalue msg;
                                    msg["type"] = "status";
                                    msg["name"] = sm.GetName();
                                    msg["state"] = std::string(sm.GetStateName());
                                    msg["outputs"] = sm.GetOutputsCount();
                                    msg["inflight"] = sm.GetInflightCount();
                                    msg["completed"] = sm.GetCompletedCount();
                                    m_PendingBroadcasts.push_back(msg.dump());
                                });
                        }
                    }
                    LOG_APP_INFO("WebSocket client connected (total: {}, lifetime: {}, peak: {})", m_Clients.size(),
                                 m_WsTotalConnects, m_WsPeakClients);

                    // Queue current workflow run snapshots.
                    BroadcastWorkflowRunsSnapshot();
                    BroadcastWorkflowRunsLastSnapshot();
                })
            .onclose(
                [this](crow::websocket::connection& conn, const std::string& reason, uint16_t code)
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Clients.erase(&conn);
                    m_ClientCount.store(m_Clients.size(), std::memory_order_relaxed);
                    ++m_WsTotalDisconnects;
                    LOG_APP_INFO("WebSocket client disconnected ({}, code {}) (remaining: {}, lifetime disconnects: {})",
                                 reason, code, m_Clients.size(), m_WsTotalDisconnects);
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

                        // Heartbeat from dashboard — no response needed, but must
                        // NOT return early so DrainPendingBroadcasts() at the end
                        // still runs.
                        if (type == "ping")
                        {
                            // fall through to drain
                        }

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

                                    crow::json::wvalue::list tasksJson;
                                    for (auto const& [taskId, taskState] : run.m_TaskStates)
                                    {
                                        crow::json::wvalue taskJson;
                                        taskJson["taskId"] = taskId;
                                        taskJson["state"] = ToStringTaskInstanceStateKind(taskState.m_State);
                                        taskJson["attemptCount"] = static_cast<int64_t>(taskState.m_AttemptCount);
                                        taskJson["lastErrorMessage"] = SanitizeUtf8(taskState.m_LastErrorMessage);
                                        if (!taskState.m_CapturedStdout.empty())
                                        {
                                            taskJson["capturedStdout"] = SanitizeUtf8(taskState.m_CapturedStdout);
                                        }
                                        if (!taskState.m_CapturedStderr.empty())
                                        {
                                            taskJson["capturedStderr"] = SanitizeUtf8(taskState.m_CapturedStderr);
                                        }
                                        tasksJson.push_back(std::move(taskJson));
                                    }
                                    runJson["tasks"] = std::move(tasksJson);

                                    activeRunsJson.push_back(std::move(runJson));
                                }
                                msg["activeRuns"] = std::move(activeRunsJson);
                            }
                            else
                            {
                                msg["activeRuns"] = crow::json::wvalue::list();
                                msg["warning"] = "workflow runtime manager not configured";
                            }

                            {
                                std::lock_guard<std::mutex> lock(m_Mutex);
                                m_PendingBroadcasts.push_back(msg.dump());
                            }
                        }
                        else if (type == "ai-explain-jcwf")
                        {
                            std::string jcwfJson = std::string(doc["jcwf"].get_string().value());
                            m_AiJcwfService.ExplainAsync(jcwfJson);
                        }

                        else if (type == "ai-generate-jcwf")
                        {
                            std::string prompt = std::string(doc["prompt"].get_string().value());
                            std::string currentJcwf;
                            auto currentResult = doc["currentJcwf"].get_string();
                            if (currentResult.error() == simdjson::SUCCESS)
                            {
                                currentJcwf = std::string(currentResult.value());
                            }
                            m_AiJcwfService.GenerateAsync(prompt, currentJcwf);
                        }

                        else if (type == "ai-write-scripts")
                        {
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

                                    // Security: must start with "scripts/" and not escape
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

                                    // Create parent directories if needed
                                    std::error_code ec;
                                    fs::path parentDir = normalized.parent_path();
                                    if (!parentDir.empty())
                                    {
                                        fs::create_directories(parentDir, ec);
                                    }

                                    // Write file
                                    std::ofstream ofs(normalized, std::ios::out | std::ios::binary);
                                    if (!ofs)
                                    {
                                        crow::json::wvalue err;
                                        err["path"] = scriptPath;
                                        err["error"] = "Failed to open file for writing";
                                        errorsList.push_back(std::move(err));
                                        continue;
                                    }
                                    ofs << content;
                                    ofs.close();

                                    // Set executable permission for shell scripts
                                    if (executable || scriptPath.ends_with(".sh"))
                                    {
                                        fs::permissions(normalized,
                                                        fs::perms::owner_exec | fs::perms::group_exec |
                                                            fs::perms::others_exec,
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
                        }

                        else if (type == "ai-fix-failed-script")
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
                        }

                        else
                        {
                            std::lock_guard<std::mutex> lock(m_Mutex);
                            m_PendingBroadcasts.push_back(R"({"error":"unknown type"})");
                        }
                    }
                    catch (const std::exception& e)
                    {
                        crow::json::wvalue error;
                        error["error"] = e.what();
                        try
                        {
                            std::lock_guard<std::mutex> lock(m_Mutex);
                            m_PendingBroadcasts.push_back(error.dump());
                        }
                        catch (...)
                        {
                        }
                    }

                    // Drain ALL queued broadcasts (including the response above) in a
                    // single batched send_text call.  Draining at the END of onmessage
                    // avoids the double-send_text that caused Crow's async-write overlap
                    // and WebSocket disconnects.
                    DrainPendingBroadcasts();
                });
    }

    void WebServer::ShutdownAssistantController() { m_AssistantController.Shutdown(); }

    void WebServer::RegisterAssistantWebSocket()
    {
        CROW_WEBSOCKET_ROUTE(m_Server, "/ws/assistant")
            .onopen([this](crow::websocket::connection& conn) { m_AssistantController.OnOpen(conn); })
            .onclose([this](crow::websocket::connection& conn, const std::string& /*reason*/, uint16_t /*code*/)
                     { m_AssistantController.OnClose(conn); })
            .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool /*is_binary*/)
                       { m_AssistantController.OnMessage(conn, data); });
    }

    bool WebServer::Start()
    {
        if (m_Running)
        {
            return true;
        }

        // Pre-test port availability to detect a second JA instance.
        {
#if defined(_WIN32)
            SOCKET const testSocket = ::socket(AF_INET, SOCK_STREAM, 0);
            if (testSocket == INVALID_SOCKET)
#else
            int const testSocket = ::socket(AF_INET, SOCK_STREAM, 0);
            if (testSocket < 0)
#endif
            {
                LOG_APP_CRITICAL("[web] Failed to create test socket — cannot verify port availability");
                return false;
            }

            int opt = 1;
#if defined(_WIN32)
            ::setsockopt(testSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&opt), sizeof(opt));
#else
            ::setsockopt(testSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

            struct sockaddr_in addr
            {
            };
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(8080);

            bool const portAvailable = (::bind(testSocket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
#if defined(_WIN32)
            ::closesocket(testSocket);
#else
            ::close(testSocket);
#endif

            if (!portAvailable)
            {
                LOG_APP_CRITICAL("[web] Port 8080 is already in use — is another JarvisAgent running? Exiting.");
                return false;
            }
        }

        m_Running = true;
        m_ServerThread = std::thread(
            [this]()
            {
                LOG_APP_INFO("Crow web server started at http://localhost:8080");
                m_Server.port(8080).multithreaded().signal_clear().run();
            });

        return true;
    }

    void WebServer::Stop()
    {
        SignalStop();
        WaitStop();
    }

    void WebServer::SignalStop()
    {
        if (!m_Running)
        {
            return;
        }

        m_Running = false;

        // Shut down the AI JCWF service so background threads are joined.
        m_AiJcwfService.Shutdown();

        // Assistant controller is shut down early via ShutdownAssistantController().
        // The call here is a no-op safety net (Shutdown() is idempotent).
        m_AssistantController.Shutdown();

        // Force-close all WebSocket connections before stopping.
        // Crow's I/O loop won't exit while connections are open.
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            LOG_APP_INFO("[shutdown] WebSocket stats: totalConnects={}, totalDisconnects={}, "
                         "peakClients={}, peakPendingBroadcasts={}, currentClients={}, "
                         "pendingBroadcasts={}",
                         m_WsTotalConnects, m_WsTotalDisconnects, m_WsPeakClients, m_WsPeakPendingBroadcasts,
                         m_Clients.size(), m_PendingBroadcasts.size());
            LOG_APP_INFO("[shutdown] WebSocket: force-closing {} client(s)...", m_Clients.size());
            size_t closed = 0;
            for (auto* client : m_Clients)
            {
                try
                {
                    client->close("server shutting down");
                    ++closed;
                }
                catch (...)
                {
                    LOG_APP_WARN("[shutdown] WebSocket: exception closing client");
                }
            }
            LOG_APP_INFO("[shutdown] WebSocket: sent close to {} client(s)", closed);
        }

        LOG_APP_INFO("[shutdown] WebSocket: calling m_Server.stop()...");
        m_Server.stop();
        LOG_APP_INFO("[shutdown] WebSocket: m_Server.stop() returned");
    }

    void WebServer::WaitStop()
    {
        if (m_ServerThread.joinable())
        {
            LOG_APP_INFO("[shutdown] WebSocket: joining server thread...");
            m_ServerThread.join();
            LOG_APP_INFO("Crow web server stopped");
        }
    }

    void WebServer::Broadcast(const std::string& jsonMessage)
    {
        if (m_ClientCount.load(std::memory_order_relaxed) == 0)
            return;

        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PendingBroadcasts.push_back(jsonMessage);
        if (m_PendingBroadcasts.size() > m_WsPeakPendingBroadcasts)
        {
            m_WsPeakPendingBroadcasts = m_PendingBroadcasts.size();
        }
    }

    void WebServer::BroadcastJSON(std::string const& jsonString)
    {
        if (m_ClientCount.load(std::memory_order_relaxed) == 0)
            return;

        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PendingBroadcasts.push_back(jsonString);
        if (m_PendingBroadcasts.size() > m_WsPeakPendingBroadcasts)
        {
            m_WsPeakPendingBroadcasts = m_PendingBroadcasts.size();
        }
    }

    void WebServer::EnqueueLogLine(std::string const& line)
    {
        // Skip buffering when no WebSocket client is connected — avoids
        // unbounded memory growth when JarvisAgent runs without a browser.
        if (m_ClientCount.load(std::memory_order_relaxed) == 0)
            return;

        std::lock_guard<std::mutex> lock(m_LogMutex);
        if (m_PendingLogLines.size() >= kMaxPendingLogLines)
            m_PendingLogLines.erase(m_PendingLogLines.begin());
        m_PendingLogLines.push_back(line);
    }

    void WebServer::DrainPendingBroadcasts()
    {
        std::vector<std::string> pending;
        std::vector<std::string> logLines;
        std::unordered_set<crow::websocket::connection*> clients;

        // Drain log lines under m_LogMutex first (separate lock to avoid deadlock
        // when logging happens inside a m_Mutex scope).
        {
            std::lock_guard<std::mutex> lock(m_LogMutex);
            if (!m_PendingLogLines.empty())
                logLines.swap(m_PendingLogLines);
        }

        {
            std::lock_guard<std::mutex> lock(m_Mutex);

            // Flush buffered log lines into a single broadcast message
            if (!logLines.empty())
            {
                // Build {"type":"log","lines":[...]} JSON manually for speed
                std::string logMsg = R"({"type":"log","lines":[)";
                for (size_t i = 0; i < logLines.size(); ++i)
                {
                    if (i > 0)
                        logMsg += ',';
                    // JSON-escape the line
                    logMsg += '"';
                    for (char c : logLines[i])
                    {
                        switch (c)
                        {
                            case '"':
                                logMsg += "\\\"";
                                break;
                            case '\\':
                                logMsg += "\\\\";
                                break;
                            case '\n':
                                logMsg += "\\n";
                                break;
                            case '\r':
                                logMsg += "\\r";
                                break;
                            case '\t':
                                logMsg += "\\t";
                                break;
                            default:
                                if (static_cast<unsigned char>(c) < 0x20)
                                {
                                    char buf[8];
                                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                                    logMsg += buf;
                                }
                                else
                                {
                                    logMsg += c;
                                }
                                break;
                        }
                    }
                    logMsg += '"';
                }
                logMsg += "]}";
                m_PendingBroadcasts.push_back(std::move(logMsg));
            }

            if (m_PendingBroadcasts.empty())
                return;
            pending.swap(m_PendingBroadcasts);
            clients = m_Clients; // snapshot
        }

        // Build a single JSON batch envelope to avoid multiple rapid send_text calls
        // (Crow's dispatch-based send overlaps async writes when called in a loop).
        std::string batch = R"({"type":"batch","messages":[)";
        for (size_t i = 0; i < pending.size(); ++i)
        {
            if (i > 0)
                batch += ',';

            // Diagnostic: log the first message that contains invalid UTF-8.
            {
                bool valid = true;
                size_t remaining = 0;
                for (unsigned char ch : pending[i])
                {
                    if (remaining > 0)
                    {
                        if ((ch & 0xC0) != 0x80)
                        {
                            valid = false;
                            break;
                        }
                        --remaining;
                    }
                    else if (ch < 0x80)
                    { /* ASCII */
                    }
                    else if ((ch & 0xE0) == 0xC0)
                    {
                        remaining = 1;
                    }
                    else if ((ch & 0xF0) == 0xE0)
                    {
                        remaining = 2;
                    }
                    else if ((ch & 0xF8) == 0xF0)
                    {
                        remaining = 3;
                    }
                    else
                    {
                        valid = false;
                        break;
                    }
                }
                if (remaining != 0)
                    valid = false;
                if (!valid)
                {
                    std::string preview = pending[i].substr(0, 300);
                    LOG_APP_WARN("[ws] Invalid UTF-8 in pending broadcast #{} (len={}): {}…", i, pending[i].size(), preview);
                }
            }

            batch += pending[i];
        }
        batch += "]}";

        // RFC 6455 requires valid UTF-8 in text frames.  Sanitize the entire
        // batch to prevent "Invalid UTF-8 in text frame" disconnects (code 1002).
        std::string const safeBatch = SanitizeUtf8(batch);

        for (auto* client : clients)
        {
            try
            {
                client->send_text(safeBatch);
            }
            catch (...)
            {
            }
        }
    }

    void WebServer::BroadcastPythonStatus(bool pythonRunning)
    {
        crow::json::wvalue msg;
        msg["type"] = "python-status";
        msg["running"] = pythonRunning;

        BroadcastJSON(msg.dump());
    }

    // =========================================================================
    // AI interfaces API handlers (config.json "API interfaces")
    // =========================================================================

    static std::string UrlDecode(std::string const& encoded)
    {
        std::string decoded;
        decoded.reserve(encoded.size());
        for (size_t i = 0; i < encoded.size(); ++i)
        {
            if (encoded[i] == '%' && i + 2 < encoded.size())
            {
                int hi = 0, lo = 0;
                auto fromHex = [](char c) -> int
                {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                    if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                    return -1;
                };
                hi = fromHex(encoded[i + 1]);
                lo = fromHex(encoded[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    decoded += static_cast<char>((hi << 4) | lo);
                    i += 2;
                    continue;
                }
            }
            else if (encoded[i] == '+')
            {
                decoded += ' ';
                continue;
            }
            decoded += encoded[i];
        }
        return decoded;
    }

    crow::response WebServer::HandleAiInterfacesListGet()
    {
        auto const& config = Core::g_Core->GetConfig();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["api_index"] = config.m_ApiIndex;
        responseJson["dirty"] = config.m_InterfacesDirty;

        std::vector<crow::json::wvalue> items;
        for (auto const& iface : config.m_ApiInterfaces)
        {
            crow::json::wvalue item;
            item["name"] = iface.m_Name;
            item["description"] = iface.m_Description;
            item["url"] = iface.m_Url;
            item["model"] = iface.m_Model;
            item["api_type"] = (iface.m_InterfaceType == ConfigParser::EngineConfig::InterfaceType::API3)   ? "API3"
                               : (iface.m_InterfaceType == ConfigParser::EngineConfig::InterfaceType::API2) ? "API2"
                                                                                                            : "API1";
            item["key_name"] = iface.m_KeyName;
            items.push_back(std::move(item));
        }
        responseJson["interfaces"] = std::move(items);
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleAiInterfaceCreatePost(crow::request const& req)
    {
        auto& config = Core::g_Core->GetMutableConfig();

        std::string name, description, url, model, apiTypeStr, keyName;

        {
            simdjson::ondemand::parser parser;
            try
            {
                simdjson::padded_string json(req.body);
                auto doc = parser.iterate(json);

                std::string_view sv;
                if (doc["url"].get_string().get(sv) == simdjson::SUCCESS)
                    url = std::string(sv);
                {
                    auto d2 = parser.iterate(json);
                    if (d2["model"].get_string().get(sv) == simdjson::SUCCESS)
                        model = std::string(sv);
                }
                {
                    auto d2 = parser.iterate(json);
                    if (d2["api_type"].get_string().get(sv) == simdjson::SUCCESS)
                        apiTypeStr = std::string(sv);
                }
                {
                    auto d2 = parser.iterate(json);
                    if (d2["name"].get_string().get(sv) == simdjson::SUCCESS)
                        name = std::string(sv);
                }
                {
                    auto d2 = parser.iterate(json);
                    if (d2["description"].get_string().get(sv) == simdjson::SUCCESS)
                        description = std::string(sv);
                }
                {
                    auto d2 = parser.iterate(json);
                    if (d2["key_name"].get_string().get(sv) == simdjson::SUCCESS)
                        keyName = std::string(sv);
                }
            }
            catch (...)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "invalid_json";
                err["message"] = "Failed to parse request body.";
                return MakeJsonResponse(400, err);
            }
        }

        if (url.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "missing_url";
            err["message"] = "Field 'url' is required.";
            return MakeJsonResponse(400, err);
        }

        ConfigParser::EngineConfig::ApiInterface newIface;
        newIface.m_Url = url;
        newIface.m_Model = model;
        newIface.m_Description = description;
        newIface.m_KeyName = keyName;
        newIface.m_Name =
            name.empty()
                ? ConfigParser::EngineConfig::GenerateInterfaceName(url, model, apiTypeStr.empty() ? "API1" : apiTypeStr)
                : name;

        if (apiTypeStr == "API3")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API3;
        else if (apiTypeStr == "API2")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API2;
        else
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API1;

        // Check for duplicate name
        for (auto const& existing : config.m_ApiInterfaces)
        {
            if (existing.m_Name == newIface.m_Name)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "duplicate_name";
                err["message"] = "An AI interface with name '" + newIface.m_Name + "' already exists.";
                return MakeJsonResponse(409, err);
            }
        }

        config.m_ApiInterfaces.push_back(std::move(newIface));
        config.m_InterfacesDirty = true;

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["name"] = config.m_ApiInterfaces.back().m_Name;
        return MakeJsonResponse(201, responseJson);
    }

    crow::response WebServer::HandleAiInterfaceUpdatePut(crow::request const& req, std::string const& name)
    {
        auto& config = Core::g_Core->GetMutableConfig();
        std::string const decodedName = UrlDecode(name);

        // Find the interface by name
        ConfigParser::EngineConfig::ApiInterface* target = nullptr;
        for (auto& iface : config.m_ApiInterfaces)
        {
            if (iface.m_Name == decodedName)
            {
                target = &iface;
                break;
            }
        }

        if (!target)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "AI interface '" + decodedName + "' not found.";
            return MakeJsonResponse(404, err);
        }

        {
            simdjson::ondemand::parser parser;
            try
            {
                simdjson::padded_string json(req.body);
                std::string_view sv;

                {
                    auto d = parser.iterate(json);
                    if (d["url"].get_string().get(sv) == simdjson::SUCCESS)
                        target->m_Url = std::string(sv);
                }
                {
                    auto d = parser.iterate(json);
                    if (d["model"].get_string().get(sv) == simdjson::SUCCESS)
                        target->m_Model = std::string(sv);
                }
                {
                    auto d = parser.iterate(json);
                    if (d["description"].get_string().get(sv) == simdjson::SUCCESS)
                        target->m_Description = std::string(sv);
                }
                {
                    auto d = parser.iterate(json);
                    if (d["name"].get_string().get(sv) == simdjson::SUCCESS)
                        target->m_Name = std::string(sv);
                }
                {
                    auto d = parser.iterate(json);
                    if (d["api_type"].get_string().get(sv) == simdjson::SUCCESS)
                    {
                        std::string apiTypeStr(sv);
                        if (apiTypeStr == "API3")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API3;
                        else if (apiTypeStr == "API2")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API2;
                        else
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API1;
                    }
                }
                {
                    auto d = parser.iterate(json);
                    if (d["key_name"].get_string().get(sv) == simdjson::SUCCESS)
                        target->m_KeyName = std::string(sv);
                }
            }
            catch (...)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "invalid_json";
                err["message"] = "Failed to parse request body.";
                return MakeJsonResponse(400, err);
            }
        }

        config.m_InterfacesDirty = true;

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["name"] = target->m_Name;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleAiInterfaceDeleteDelete(std::string const& name)
    {
        auto& config = Core::g_Core->GetMutableConfig();
        std::string const decodedName = UrlDecode(name);

        auto it = std::find_if(config.m_ApiInterfaces.begin(), config.m_ApiInterfaces.end(),
                               [&decodedName](auto const& iface) { return iface.m_Name == decodedName; });

        if (it == config.m_ApiInterfaces.end())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "AI interface '" + decodedName + "' not found.";
            return MakeJsonResponse(404, err);
        }

        config.m_ApiInterfaces.erase(it);
        config.m_InterfacesDirty = true;

        // Fix API index if it now exceeds bounds
        if (!config.m_ApiInterfaces.empty() && config.m_ApiIndex >= config.m_ApiInterfaces.size())
        {
            config.m_ApiIndex = config.m_ApiInterfaces.size() - 1;
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleAiInterfacesSavePost()
    {
        auto const& config = Core::g_Core->GetConfig();
        auto const& configPath = Core::g_Core->GetConfigFilePath();

        if (configPath.empty() || !std::filesystem::exists(configPath))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "no_config";
            err["message"] = "Config file path not set or file does not exist.";
            return MakeJsonResponse(500, err);
        }

        // Read the existing config.json
        std::string fileContent;
        {
            std::ifstream ifs(configPath, std::ios::binary);
            if (!ifs)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "read_failed";
                err["message"] = "Failed to read config file.";
                return MakeJsonResponse(500, err);
            }
            std::ostringstream oss;
            oss << ifs.rdbuf();
            fileContent = oss.str();
        }

        // Build the new "API interfaces" JSON array
        std::string newArray = "[\n";
        for (size_t i = 0; i < config.m_ApiInterfaces.size(); ++i)
        {
            auto const& iface = config.m_ApiInterfaces[i];
            std::string apiStr = (iface.m_InterfaceType == ConfigParser::EngineConfig::InterfaceType::API3)   ? "API3"
                                 : (iface.m_InterfaceType == ConfigParser::EngineConfig::InterfaceType::API2) ? "API2"
                                                                                                              : "API1";

            newArray += "        {\n";
            newArray += "            \"name\": \"" + iface.m_Name + "\",\n";
            if (!iface.m_Description.empty())
            {
                newArray += "            \"description\": \"" + iface.m_Description + "\",\n";
            }
            newArray += "            \"url\": \"" + iface.m_Url + "\",\n";
            newArray += "            \"model\": \"" + iface.m_Model + "\",\n";
            newArray += "            \"API\": \"" + apiStr + "\"";
            if (!iface.m_KeyName.empty())
            {
                newArray += ",\n";
                newArray += "            \"key_name\": \"" + iface.m_KeyName + "\"";
            }
            newArray += "\n";
            newArray += "        }";
            if (i + 1 < config.m_ApiInterfaces.size())
            {
                newArray += ",";
            }
            newArray += "\n";
        }
        newArray += "    ]";

        // Find and replace the "API interfaces" array in the file content
        auto keyPos = fileContent.find("\"API interfaces\"");
        if (keyPos == std::string::npos)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "parse_failed";
            err["message"] = "Could not find 'API interfaces' key in config.json.";
            return MakeJsonResponse(500, err);
        }

        // Find the opening [ after "API interfaces"
        auto arrayStart = fileContent.find('[', keyPos);
        if (arrayStart == std::string::npos)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "parse_failed";
            err["message"] = "Could not find array start for 'API interfaces'.";
            return MakeJsonResponse(500, err);
        }

        // Find matching ] (skip brackets inside strings)
        int depth = 0;
        size_t arrayEnd = std::string::npos;
        for (size_t i = arrayStart; i < fileContent.size(); ++i)
        {
            char c = fileContent[i];
            if (c == '"')
            {
                ++i;
                while (i < fileContent.size() && fileContent[i] != '"')
                {
                    if (fileContent[i] == '\\')
                        ++i;
                    ++i;
                }
            }
            else if (c == '[')
            {
                ++depth;
            }
            else if (c == ']')
            {
                --depth;
                if (depth == 0)
                {
                    arrayEnd = i;
                    break;
                }
            }
        }

        if (arrayEnd == std::string::npos)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "parse_failed";
            err["message"] = "Could not find matching ']' for 'API interfaces' array.";
            return MakeJsonResponse(500, err);
        }

        fileContent.replace(arrayStart, arrayEnd - arrayStart + 1, newArray);

        // Write back
        {
            std::ofstream ofs(configPath, std::ios::binary | std::ios::trunc);
            if (!ofs)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "write_failed";
                err["message"] = "Failed to write config file.";
                return MakeJsonResponse(500, err);
            }
            ofs << fileContent;
        }

        LOG_CORE_INFO("WebServer: saved {} AI interfaces to '{}'", config.m_ApiInterfaces.size(), configPath.string());

        Core::g_Core->GetMutableConfig().m_InterfacesDirty = false;

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["path"] = configPath.string();
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleAiInterfaceTestPost(crow::request const& req)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string json(req.body);
        auto doc = parser.iterate(json);

        int64_t index = -1;
        auto indexResult = doc["index"].get_int64();
        if (indexResult.error() != simdjson::SUCCESS)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "bad_request";
            err["message"] = "Missing required field: 'index' (integer).";
            return MakeJsonResponse(400, err);
        }
        index = indexResult.value();

        if (index < 0)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "bad_request";
            err["message"] = "Index must be >= 0.";
            return MakeJsonResponse(400, err);
        }

        std::string responsePreview;
        std::string error;
        int64_t latencyMs = 0;

        bool const ok = m_AiJcwfService.TestAiInterface(static_cast<size_t>(index), responsePreview, error, latencyMs);

        auto const& config = Core::g_Core->GetConfig();
        std::string interfaceName;
        std::string model;
        if (static_cast<size_t>(index) < config.m_ApiInterfaces.size())
        {
            interfaceName = config.m_ApiInterfaces[static_cast<size_t>(index)].m_Name;
            model = config.m_ApiInterfaces[static_cast<size_t>(index)].m_Model;
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = ok;
        responseJson["index"] = index;
        responseJson["name"] = interfaceName;
        responseJson["model"] = model;
        responseJson["latency_ms"] = latencyMs;

        if (ok)
        {
            responseJson["response_preview"] = responsePreview;
        }
        else
        {
            responseJson["error"] = error;
        }

        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleConfigReloadPost()
    {
        auto const& configPath = Core::g_Core->GetConfigFilePath();
        if (configPath.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "no_config";
            err["message"] = "Config file path not set.";
            return MakeJsonResponse(500, err);
        }

        std::string const configPathStr = configPath.lexically_normal().string();
        ConfigParser configParser(configPathStr);
        ConfigParser::EngineConfig newConfig{};
        configParser.Parse(newConfig);

        if (!configParser.ConfigParsed())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "parse_failed";
            err["message"] = "Failed to parse config.json.";
            return MakeJsonResponse(500, err);
        }

        // Update the in-memory config (preserve m_ConfigValid from checker)
        auto& config = Core::g_Core->GetMutableConfig();
        config.m_ApiInterfaces = std::move(newConfig.m_ApiInterfaces);
        config.m_ApiIndex = newConfig.m_ApiIndex;
        config.m_InterfacesDirty = false;

        LOG_CORE_INFO("WebServer: reloaded config.json — {} AI interfaces", config.m_ApiInterfaces.size());

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["interface_count"] = config.m_ApiInterfaces.size();
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleConfigSettingsGet()
    {
        auto const& config = Core::g_Core->GetConfig();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["api_index"] = config.m_ApiIndex;
        responseJson["max_threads"] = config.m_MaxThreads;
        responseJson["verbose"] = config.m_Verbose;
        responseJson["max_file_size_kb"] = config.m_MaxFileSizekB;
        responseJson["jcwf_batch_size"] = config.m_JcwfBatchSize;
        responseJson["jcwf_ai_interface"] = config.m_JcwfAiInterfaceIndex;
        responseJson["queue_folder"] = config.m_QueueFolderFilepath;
        responseJson["workflows_folder"] = config.m_WorkflowsFolderFilepath;
        responseJson["interface_count"] = config.m_ApiInterfaces.size();
        responseJson["use_bash"] = config.m_UseBashOnWindows;
#if defined(_WIN32)
        responseJson["platform"] = "windows";
#elif defined(__APPLE__)
        responseJson["platform"] = "macos";
#else
        responseJson["platform"] = "linux";
#endif

        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleConfigSettingsPut(crow::request const& req)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string json(req.body);
        auto doc = parser.iterate(json);

        auto& config = Core::g_Core->GetMutableConfig();
        bool anyChanged = false;

        // api_index
        {
            auto result = doc["api_index"].get_int64();
            if (result.error() == simdjson::SUCCESS)
            {
                int64_t val = result.value();
                if (val >= 0 && static_cast<size_t>(val) < config.m_ApiInterfaces.size())
                {
                    config.m_ApiIndex = static_cast<size_t>(val);
                    anyChanged = true;
                }
            }
        }

        // max_threads
        {
            auto result = doc["max_threads"].get_int64();
            if (result.error() == simdjson::SUCCESS)
            {
                int64_t val = result.value();
                if (val > 0 && val <= 256)
                {
                    config.m_MaxThreads = static_cast<size_t>(val);
                    anyChanged = true;
                }
            }
        }

        // verbose
        {
            auto result = doc["verbose"].get_bool();
            if (result.error() == simdjson::SUCCESS)
            {
                config.m_Verbose = result.value();
                anyChanged = true;
            }
        }

        // max_file_size_kb
        {
            auto result = doc["max_file_size_kb"].get_int64();
            if (result.error() == simdjson::SUCCESS)
            {
                int64_t val = result.value();
                if (val > 0 && val <= 10240)
                {
                    config.m_MaxFileSizekB = static_cast<size_t>(val);
                    anyChanged = true;
                }
            }
        }

        // jcwf_batch_size
        {
            auto result = doc["jcwf_batch_size"].get_int64();
            if (result.error() == simdjson::SUCCESS)
            {
                int64_t val = result.value();
                if (val >= 1 && val <= 100)
                {
                    config.m_JcwfBatchSize = static_cast<size_t>(val);
                    anyChanged = true;
                }
            }
        }

        // jcwf_ai_interface (-1 = use global default, >= 0 = specific interface index)
        {
            auto result = doc["jcwf_ai_interface"].get_int64();
            if (result.error() == simdjson::SUCCESS)
            {
                int64_t val = result.value();
                if (val >= -1 && (val < 0 || static_cast<size_t>(val) < config.m_ApiInterfaces.size()))
                {
                    config.m_JcwfAiInterfaceIndex = static_cast<int>(val);
                    anyChanged = true;
                }
            }
        }

        // use_bash (Windows-only meaning; accepted on all platforms)
        {
            auto result = doc["use_bash"].get_bool();
            if (result.error() == simdjson::SUCCESS)
            {
                config.m_UseBashOnWindows = result.value();
                anyChanged = true;
            }
        }

        if (!anyChanged)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "no_changes";
            err["message"] = "No valid fields provided or values unchanged.";
            return MakeJsonResponse(400, err);
        }

        // Persist to config.json: read, patch the scalar fields, write back.
        auto const& configPath = Core::g_Core->GetConfigFilePath();
        if (!configPath.empty() && std::filesystem::exists(configPath))
        {
            std::string fileContent;
            {
                std::ifstream ifs(configPath, std::ios::binary);
                if (ifs)
                {
                    std::ostringstream oss;
                    oss << ifs.rdbuf();
                    fileContent = oss.str();
                }
            }

            if (!fileContent.empty())
            {
                // Helper: replace a JSON scalar field value in-place.
                auto replaceField = [&](std::string const& key, std::string const& newValue)
                {
                    std::string const searchKey = "\"" + key + "\"";
                    auto pos = fileContent.find(searchKey);
                    if (pos == std::string::npos)
                        return;
                    auto colonPos = fileContent.find(':', pos + searchKey.size());
                    if (colonPos == std::string::npos)
                        return;
                    // Find start of value (skip whitespace)
                    size_t valStart = colonPos + 1;
                    while (valStart < fileContent.size() && (fileContent[valStart] == ' ' || fileContent[valStart] == '\t'))
                        ++valStart;
                    // Find end of value (next comma, newline, or closing brace)
                    size_t valEnd = valStart;
                    if (fileContent[valEnd] == '"')
                    {
                        // String value
                        ++valEnd;
                        while (valEnd < fileContent.size() && fileContent[valEnd] != '"')
                        {
                            if (fileContent[valEnd] == '\\')
                                ++valEnd;
                            ++valEnd;
                        }
                        if (valEnd < fileContent.size())
                            ++valEnd; // past closing quote
                    }
                    else
                    {
                        // Numeric/bool value
                        while (valEnd < fileContent.size() && fileContent[valEnd] != ',' && fileContent[valEnd] != '\n' &&
                               fileContent[valEnd] != '\r' && fileContent[valEnd] != '}')
                            ++valEnd;
                        // Trim trailing whitespace from the value
                        while (valEnd > valStart && (fileContent[valEnd - 1] == ' ' || fileContent[valEnd - 1] == '\t'))
                            --valEnd;
                    }
                    fileContent.replace(valStart, valEnd - valStart, newValue);
                };

                replaceField("API index", std::to_string(config.m_ApiIndex));
                replaceField("max threads", std::to_string(config.m_MaxThreads));
                replaceField("verbose", config.m_Verbose ? "true" : "false");
                replaceField("max file size in kB", std::to_string(config.m_MaxFileSizekB));
                replaceField("jcwf batch size", std::to_string(config.m_JcwfBatchSize));
                replaceField("jcwf AI interface", std::to_string(config.m_JcwfAiInterfaceIndex));
                replaceField("use_bash", config.m_UseBashOnWindows ? "true" : "false");

                std::ofstream ofs(configPath, std::ios::binary | std::ios::trunc);
                if (ofs)
                {
                    ofs << fileContent;
                    LOG_CORE_INFO("WebServer: saved config settings to '{}'", configPath.string());
                }
            }
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["api_index"] = config.m_ApiIndex;
        responseJson["max_threads"] = config.m_MaxThreads;
        responseJson["verbose"] = config.m_Verbose;
        responseJson["max_file_size_kb"] = config.m_MaxFileSizekB;
        responseJson["jcwf_batch_size"] = config.m_JcwfBatchSize;
        responseJson["jcwf_ai_interface"] = config.m_JcwfAiInterfaceIndex;
        responseJson["use_bash"] = config.m_UseBashOnWindows;
        return MakeJsonResponse(200, responseJson);
    }

    // =========================================================================
    // Key management API handlers
    // =========================================================================

    crow::response WebServer::HandleKeysStatusGet()
    {
        auto const& keyManager = Core::g_Core->GetKeyManager();
        auto status = keyManager.GetKeyLoadStatus();

        crow::json::wvalue responseJson;

        std::string statusStr;
        std::string message;
        switch (status)
        {
            case KeyManager::KeyLoadStatus::Ok:
                statusStr = "ok";
                message = "Keys loaded successfully.";
                break;
            case KeyManager::KeyLoadStatus::NoPassword:
                statusStr = "no_password";
                message = "No master password provided. Please enter your master password.";
                break;
            case KeyManager::KeyLoadStatus::WrongPassword:
                statusStr = "wrong_password";
                message = "Incorrect master password provided. Please enter the correct password.";
                break;
            case KeyManager::KeyLoadStatus::NoKeysFile:
                statusStr = "no_keys_file";
                message = "No encrypted keys file found.";
                break;
        }

        responseJson["ok"] = true;
        responseJson["status"] = statusStr;
        responseJson["message"] = message;
        responseJson["has_providers"] = keyManager.HasProviders();
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleKeysUnlockPost(crow::request const& req)
    {
        auto& keyManager = Core::g_Core->GetKeyManager();

        // Parse master_password from request body
        std::string masterPassword;
        {
            simdjson::ondemand::parser parser;
            try
            {
                simdjson::padded_string json(req.body);
                auto doc = parser.iterate(json);

                std::string_view sv;
                if (doc["master_password"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    masterPassword = std::string(sv);
                }
            }
            catch (...)
            {
                // malformed body
            }
        }

        if (masterPassword.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "missing_password";
            err["message"] = "Request body must contain 'master_password'.";
            return MakeJsonResponse(400, err);
        }

        if (keyManager.Unlock(masterPassword))
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = true;
            responseJson["status"] = "ok";
            responseJson["message"] = "Keys unlocked successfully.";
            return MakeJsonResponse(200, responseJson);
        }

        crow::json::wvalue err;
        err["ok"] = false;
        err["status"] = "wrong_password";
        err["message"] = "Incorrect master password. Please try again.";
        return MakeJsonResponse(401, err);
    }

    // =========================================================================
    // Provider settings API handlers
    // =========================================================================

    crow::response WebServer::HandleProvidersListGet()
    {
        auto& keyManager = Core::g_Core->GetKeyManager();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["dirty"] = keyManager.IsDirty();
        responseJson["default_provider"] = keyManager.GetDefaultProviderName();

        std::vector<std::string> names = keyManager.GetProviderNames();

        crow::json::wvalue::list providersList;
        providersList.reserve(names.size());

        for (std::string const& name : names)
        {
            auto const* provider = keyManager.GetProvider(name);
            if (!provider)
            {
                continue;
            }

            crow::json::wvalue entry;
            entry["name"] = name;
            entry["display_name"] = provider->m_DisplayName;
            entry["endpoint"] = provider->m_Endpoint;
            entry["default_model"] = provider->m_DefaultModel;
            entry["api_type"] = provider->m_ApiType;
            entry["has_key"] = !provider->m_ApiKey.empty();
            // API key is intentionally NOT returned for security.
            providersList.push_back(std::move(entry));
        }

        responseJson["providers"] = std::move(providersList);
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleProviderCreatePost(crow::request const& req)
    {
        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string json(req.body);
            auto doc = parser.iterate(json);

            std::string_view name;
            if (doc["name"].get_string().get(name) != simdjson::SUCCESS || name.empty())
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "missing_name";
                err["message"] = "'name' is required and must be a non-empty string";
                return MakeJsonResponse(400, err);
            }

            KeyManager::ProviderConfig config;

            std::string_view sv;
            if (doc["display_name"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_DisplayName = std::string(sv);
            }
            if (doc["endpoint"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Endpoint = std::string(sv);
            }
            if (doc["api_key"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_ApiKey = std::string(sv);
            }
            if (doc["default_model"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_DefaultModel = std::string(sv);
            }
            if (doc["api_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_ApiType = std::string(sv);
            }

            auto& keyManager = Core::g_Core->GetKeyManager();
            if (!keyManager.AddProvider(std::string(name), std::move(config)))
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "already_exists";
                err["message"] = "Provider '" + std::string(name) + "' already exists";
                return MakeJsonResponse(409, err);
            }

            crow::json::wvalue responseJson;
            responseJson["ok"] = true;
            responseJson["name"] = std::string(name);
            return MakeJsonResponse(201, responseJson);
        }
        catch (std::exception const& e)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_json";
            err["message"] = std::string("JSON parse error: ") + e.what();
            return MakeJsonResponse(400, err);
        }
    }

    crow::response WebServer::HandleProviderUpdatePut(crow::request const& req, std::string const& providerName)
    {
        auto& keyManager = Core::g_Core->GetKeyManager();

        auto const* existing = keyManager.GetProvider(providerName);
        if (!existing)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Provider '" + providerName + "' not found";
            return MakeJsonResponse(404, err);
        }

        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string json(req.body);
            auto doc = parser.iterate(json);

            // Start from existing config, overlay provided fields
            KeyManager::ProviderConfig config = *existing;

            std::string_view sv;
            if (doc["display_name"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_DisplayName = std::string(sv);
            }
            if (doc["endpoint"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Endpoint = std::string(sv);
            }
            if (doc["api_key"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_ApiKey = std::string(sv);
            }
            if (doc["default_model"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_DefaultModel = std::string(sv);
            }
            if (doc["api_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_ApiType = std::string(sv);
            }

            keyManager.UpdateProvider(providerName, std::move(config));

            crow::json::wvalue responseJson;
            responseJson["ok"] = true;
            responseJson["name"] = providerName;
            return MakeJsonResponse(200, responseJson);
        }
        catch (std::exception const& e)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_json";
            err["message"] = std::string("JSON parse error: ") + e.what();
            return MakeJsonResponse(400, err);
        }
    }

    crow::response WebServer::HandleProviderDelete(std::string const& providerName)
    {
        auto& keyManager = Core::g_Core->GetKeyManager();

        if (!keyManager.RemoveProvider(providerName))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Provider '" + providerName + "' not found";
            return MakeJsonResponse(404, err);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleProviderSetDefaultPost(std::string const& providerName)
    {
        auto& keyManager = Core::g_Core->GetKeyManager();

        if (!keyManager.GetProvider(providerName))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Provider '" + providerName + "' not found";
            return MakeJsonResponse(404, err);
        }

        keyManager.SetDefaultProvider(providerName);

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["default_provider"] = providerName;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleProvidersSavePost(crow::request const& req)
    {
        auto& keyManager = Core::g_Core->GetKeyManager();

        // Master password: try request body first, then env var
        std::string masterPassword;
        {
            simdjson::ondemand::parser parser;
            try
            {
                simdjson::padded_string json(req.body);
                auto doc = parser.iterate(json);

                std::string_view sv;
                if (doc["master_password"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    masterPassword = std::string(sv);
                }
            }
            catch (...)
            {
                // Body might be empty or not JSON — fall through to env var
            }
        }

        if (masterPassword.empty())
        {
            char const* envPassword = std::getenv("JARVIS_MASTER_PASSWORD");
            if (envPassword && std::strlen(envPassword) > 0)
            {
                masterPassword = std::string(envPassword);
            }
        }

        if (masterPassword.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "no_password";
            err["message"] = "Master password required. Provide in request body or set JARVIS_MASTER_PASSWORD env var.";
            return MakeJsonResponse(400, err);
        }

        std::filesystem::path const keysFilePath =
            Core::g_Core->GetLaunchCWDAbsolute() / Core::g_Core->GetConfig().m_KeysFilePath;

        // If an encrypted file already exists, verify the password matches before overwriting
        if (std::filesystem::exists(keysFilePath))
        {
            std::ifstream verifyFile(keysFilePath, std::ios::binary);
            if (verifyFile)
            {
                std::vector<uint8_t> existingBlob((std::istreambuf_iterator<char>(verifyFile)),
                                                  std::istreambuf_iterator<char>());
                verifyFile.close();

                if (!existingBlob.empty())
                {
                    std::string decrypted = KeyEncryption::Decrypt(existingBlob, masterPassword);
                    if (decrypted.empty())
                    {
                        crow::json::wvalue err;
                        err["ok"] = false;
                        err["error"] = "wrong_password";
                        err["message"] =
                            "Incorrect master password. The password must match the one used to create the keys file.";
                        return MakeJsonResponse(403, err);
                    }
                }
            }
        }

        if (!keyManager.Save(keysFilePath, masterPassword))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "save_failed";
            err["message"] = "Failed to save encrypted keys file to '" + keysFilePath.string() + "'";
            return MakeJsonResponse(500, err);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["path"] = keysFilePath.string();
        return MakeJsonResponse(200, responseJson);
    }

} // namespace AIAssistant