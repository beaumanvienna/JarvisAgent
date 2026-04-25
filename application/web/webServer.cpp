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

#include <array>
#include <cstring>
#include <ctime>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <iomanip>
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
#include <openssl/sha.h>

#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "python/pythonEnginePool.h"
#include "web/webServer.h"
#include "file/scriptRegistry.h"
#include "workflow/taskPathResolver.h"

#include "workflow/aiRequestPool.h"
#include "workflow/jcwfContainer.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowJsonParser.h"

#include "workflow/workflowValidator.h"

#include "workflow/workflowRuntimeManager.h"
#include "workflow/workflowTypes.h"

#include "event/events.h"
#include "keys/keyEncryption.h"
#include "cloud/cloudConnector.h"
#include "cloud/cloudConnectorRegistry.h"
#include "cloud/cloudConnectionManager.h"
#include "cloud/cloudCircuitBreaker.h"
#include "cloud/oneDriveConnector.h"
#include "keys/oauthTokenManager.h"
#include "curlWrapper/curlWrapper.h"
#include <curl/curl.h>
#include "workflow/triggerEngine.h"
#include "workflow/workflowTriggerBinder.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

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

        void SetSecurityHeaders(crow::response& response)
        {
            response.set_header("X-Frame-Options", "DENY");
            response.set_header("X-Content-Type-Options", "nosniff");
            response.set_header("Referrer-Policy", "strict-origin-when-cross-origin");
            response.set_header("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
            response.set_header("Content-Security-Policy",
                                "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                                "connect-src 'self' ws: wss:; img-src 'self' data:");
        }

        void SetJsonHeaders(crow::response& response)
        {
            response.add_header("Content-Type", "application/json");
            response.add_header("Cache-Control", "no-store");
            SetSecurityHeaders(response);
        }

        bool IsBodyTooLarge(crow::request const& req, size_t maxMB)
        {
            return maxMB > 0 && req.body.size() > maxMB * 1024 * 1024;
        }

        crow::response MakePayloadTooLargeResponse(size_t maxMB)
        {
            crow::json::wvalue body;
            body["ok"] = false;
            body["error"] = "payload_too_large";
            body["message"] = "Request body exceeds " + std::to_string(maxMB) + " MB limit";
            crow::response resp(413, body.dump());
            SetJsonHeaders(resp);
            return resp;
        }

        crow::response MakeJsonResponse(int const httpStatus, crow::json::wvalue const& json)
        {
            crow::response response(httpStatus, json.dump());
            SetJsonHeaders(response);
            return response;
        }

        crow::response MakeAuthErrorResponse(std::string const& error)
        {
            crow::json::wvalue body;
            body["ok"] = false;
            if (error == "locked_out")
            {
                body["error"] = "locked_out";
                body["message"] = "Too many failed authentication attempts. Try again in 15 minutes.";
                crow::response resp(403, body.dump());
                SetJsonHeaders(resp);
                resp.add_header("Retry-After", "900");
                return resp;
            }
            else if (error == "rate_limited")
            {
                body["error"] = "rate_limited";
                body["message"] = "Too many requests. Try again later.";
                crow::response resp(429, body.dump());
                SetJsonHeaders(resp);
                resp.add_header("Retry-After", "5");
                return resp;
            }
            else if (error == "missing" || error == "malformed")
            {
                body["error"] = "unauthorized";
                body["message"] = "Authorization header required. Use: Authorization: Bearer <token>";
                crow::response resp(401, body.dump());
                SetJsonHeaders(resp);
                resp.add_header("WWW-Authenticate", "Bearer");
                return resp;
            }
            else if (error == "token_expired")
            {
                body["error"] = "token_expired";
                body["message"] = "API token has expired. A new token has been generated — check server logs.";
                crow::response resp(403, body.dump());
                SetJsonHeaders(resp);
                return resp;
            }
            else if (error == "insufficient_role")
            {
                body["error"] = "insufficient_role";
                body["message"] = "Your role does not have permission for this endpoint.";
                crow::response resp(403, body.dump());
                SetJsonHeaders(resp);
                return resp;
            }
            else
            {
                body["error"] = "forbidden";
                body["message"] = "Invalid API token.";
                crow::response resp(403, body.dump());
                SetJsonHeaders(resp);
                return resp;
            }
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

#ifdef J9T_STUDIO
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
#endif // J9T_STUDIO

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

    namespace
    {
        // Forwards Crow's logger to our spdlog. Crow's default CerrLogHandler writes
        // directly to std::cerr, which bypasses ncurses and corrupts the TUI status
        // window (raw writes overpaint the bottom rows). Routing through spdlog sends
        // the output to log.txt and the ncurses LOG window, same as our own logs.
        class CrowSpdlogHandler : public crow::ILogHandler
        {
          public:
            void log(std::string const& message, crow::LogLevel level) override
            {
                // Benign; every untrusted-cert client (browser prefetch, stale curl) triggers it.
                if (message.find("Could not start adaptor") != std::string::npos)
                {
                    return;
                }

                switch (level)
                {
                    case crow::LogLevel::Debug:    LOG_CORE_INFO("[crow] {}", message);      break;
                    case crow::LogLevel::Info:     LOG_CORE_INFO("[crow] {}", message);      break;
                    case crow::LogLevel::Warning:  LOG_CORE_WARN("[crow] {}", message);      break;
                    case crow::LogLevel::Error:    LOG_CORE_ERROR("[crow] {}", message);     break;
                    case crow::LogLevel::Critical: LOG_CORE_CRITICAL("[crow] {}", message);  break;
                }
            }
        };

        CrowSpdlogHandler& GetCrowLogHandler()
        {
            static CrowSpdlogHandler instance;
            return instance;
        }
    } // namespace

    WebServer::WebServer()
    {
        crow::logger::setHandler(&GetCrowLogHandler());
        m_Server.loglevel(crow::LogLevel::Warning);
        RegisterRoutes();
        RegisterWebSocket();
#ifdef J9T_STUDIO
        RegisterAssistantWebSocket();

        m_AiJcwfService.SetBroadcastFn(
            [this](std::string const& jsonString)
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_PendingBroadcasts.push_back(jsonString);
            });
#endif
    }

    WebServer::~WebServer() { Stop(); }

    void WebServer::SetWorkflowRegistry(WorkflowRegistry* workflowRegistry)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_WorkflowRegistry = workflowRegistry;
#ifdef J9T_STUDIO
        m_AssistantController.SetWorkflowRegistry(workflowRegistry);
#endif
        // Now that the registry is available, build the adhoc manager and attach it
        // to the base `_adhoc/` folder under the launch cwd. The reaper thread is
        // started so TTL-based cleanup kicks in every 60 s.
        if (workflowRegistry != nullptr && !m_AdhocManager)
        {
            m_AdhocManager = std::make_unique<AdhocWorkflowManager>(m_McpKeyManager, *workflowRegistry);
            auto const adhocBase = Core::g_Core->GetLaunchCWDAbsolute() / "_adhoc";
            m_AdhocManager->Init(adhocBase);
            m_AdhocManager->StartReaperThread();
            LOG_APP_INFO("[adhoc] Manager ready — base='{}'", adhocBase.string());
        }

        // Script catalog — scan scripts/ so MCP agents can discover what's
        // available before composing an adhoc JCWF. Cheap; idempotent.
        {
            auto const scriptsBase = Core::g_Core->GetLaunchCWDAbsolute() / "scripts";
            m_ScriptCatalog.Refresh(scriptsBase);
        }
    }

    void WebServer::SetWorkflowRuntimeManager(WorkflowRuntimeManager* workflowRuntimeManager)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_WorkflowRuntimeManager = workflowRuntimeManager;
#ifdef J9T_STUDIO
        m_AssistantController.SetWorkflowRuntimeManager(workflowRuntimeManager);
#endif
        // Plumb terminal-state notifications through to the adhoc manager so
        // on_completion runs are cleaned up the moment they finish.
        if (workflowRuntimeManager && m_AdhocManager)
        {
            AdhocWorkflowManager* adhoc = m_AdhocManager.get();
            workflowRuntimeManager->SetRunTerminalObserver(
                [adhoc](std::string const& runId, WorkflowRunState /*state*/)
                {
                    if (runId.rfind("adhoc_", 0) == 0)
                    {
                        adhoc->OnRunCompleted(runId);
                    }
                });
        }
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

                // Cap captured output at 512 KB per stream to prevent multi-MB WebSocket frames.
                static constexpr size_t kMaxCapturedBytes = 512 * 1024;
                if (!taskState.m_CapturedStdout.empty())
                {
                    if (taskState.m_CapturedStdout.size() <= kMaxCapturedBytes)
                    {
                        task["capturedStdout"] = SanitizeUtf8(taskState.m_CapturedStdout);
                    }
                    else
                    {
                        std::string truncated = taskState.m_CapturedStdout.substr(0, kMaxCapturedBytes);
                        truncated += "\n\n--- truncated (";
                        truncated += std::to_string(taskState.m_CapturedStdout.size());
                        truncated += " bytes total) ---";
                        task["capturedStdout"] = SanitizeUtf8(truncated);
                    }
                }
                if (!taskState.m_CapturedStderr.empty())
                {
                    if (taskState.m_CapturedStderr.size() <= kMaxCapturedBytes)
                    {
                        task["capturedStderr"] = SanitizeUtf8(taskState.m_CapturedStderr);
                    }
                    else
                    {
                        std::string truncated = taskState.m_CapturedStderr.substr(0, kMaxCapturedBytes);
                        truncated += "\n\n--- truncated (";
                        truncated += std::to_string(taskState.m_CapturedStderr.size());
                        truncated += " bytes total) ---";
                        task["capturedStderr"] = SanitizeUtf8(truncated);
                    }
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
        SetSecurityHeaders(response);

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

        if (m_TlsEnabled)
        {
            response.set_header("Strict-Transport-Security", "max-age=63072000; includeSubDomains");
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

#ifdef J9T_STUDIO
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
#endif // J9T_STUDIO

    // =========================================================================
    // Admin authentication (Engine edition only)
    // =========================================================================

    // Failed auth lockout constants.
    static constexpr size_t kMaxAuthFailures = 10;
    static constexpr auto kAuthFailureWindow = std::chrono::minutes(5);
    [[maybe_unused]] static constexpr auto kLockoutDuration = std::chrono::minutes(15);

    // Role hierarchy: admin > operator > viewer.
    static int RoleLevel(std::string_view role)
    {
        if (role == "admin") return 3;
        if (role == "operator") return 2;
        if (role == "viewer") return 1;
        return 0; // unknown → no access
    }

    bool WebServer::HasRole(AuthResult const& auth, std::string_view requiredRole)
    {
        return RoleLevel(auth.m_Role) >= RoleLevel(requiredRole);
    }

    std::string WebServer::ExtractBearerToken(crow::request const& req)
    {
        std::string const& authHeader = req.get_header_value("Authorization");
        static constexpr std::string_view kBearerPrefix = "Bearer ";
        if (authHeader.size() <= kBearerPrefix.size()) return {};
        if (authHeader.compare(0, kBearerPrefix.size(), kBearerPrefix) != 0) return {};
        return authHeader.substr(kBearerPrefix.size());
    }

    std::string WebServer::ExtractSessionCookie(crow::request const& req)
    {
        std::string const& cookieHeader = req.get_header_value("Cookie");
        if (cookieHeader.empty()) return {};
        // Cookie header is a ';'-separated list of "name=value" pairs.
        size_t pos = 0;
        while (pos < cookieHeader.size())
        {
            size_t end = cookieHeader.find(';', pos);
            if (end == std::string::npos) end = cookieHeader.size();
            size_t start = pos;
            while (start < end && (cookieHeader[start] == ' ' || cookieHeader[start] == '\t')) ++start;
            size_t eq = cookieHeader.find('=', start);
            if (eq != std::string::npos && eq < end)
            {
                std::string_view name(cookieHeader.data() + start, eq - start);
                if (name == "session")
                {
                    return std::string(cookieHeader.data() + eq + 1, end - eq - 1);
                }
            }
            pos = end + 1;
        }
        return {};
    }

    std::optional<WebServer::AuthResult> WebServer::TryMcpAuth(crow::request const& req) const
    {
        std::string token = ExtractBearerToken(req);
        if (token.rfind("mcp_", 0) != 0)
        {
            return std::nullopt; // not an MCP token — caller should continue with other paths
        }

        std::string const& ip = req.remote_ip_address;
        auto result = m_McpKeyManager.Authenticate(token);
        if (!result)
        {
            LOG_SECURITY_WARN("[security] mcp_auth_failure reason=invalid_key ip={}", ip);
            const_cast<WebServer*>(this)->RecordAuthFailure(ip);
            return AuthResult{"invalid_token", "", ""};
        }
        if (!result->m_Record.m_Enabled)
        {
            LOG_SECURITY_WARN("[security] mcp_auth_failure reason=key_disabled ip={} user={}",
                              ip, result->m_Record.m_User);
            return AuthResult{"key_disabled", "", ""};
        }
        if (result->m_DaysUntilExpiry < 0)
        {
            LOG_SECURITY_WARN("[security] mcp_auth_failure reason=token_expired ip={} user={}",
                              ip, result->m_Record.m_User);
            return AuthResult{"token_expired", "", ""};
        }

        LOG_SECURITY_INFO("[security] mcp_auth_success ip={} user={} role={} endpoint={}",
                          ip, result->m_Record.m_User, result->m_Record.m_Role, req.url);
        AuthResult out{"", result->m_Record.m_User, result->m_Record.m_Role};
        out.m_DaysUntilExpiry = result->m_DaysUntilExpiry;
        return out;
    }

    void WebServer::AttachMcpExpiryHeader(crow::response& resp, crow::request const& req) const
    {
        // Re-run the bearer-token extraction; the auth cache is not exposed by Crow per-response,
        // so we look up once more. McpKeyManager::Authenticate is a cheap hash + map lookup.
        std::string const token = ExtractBearerToken(req);
        if (token.rfind("mcp_", 0) != 0) return;
        auto result = m_McpKeyManager.Authenticate(token);
        if (!result) return;
        int const days = result->m_DaysUntilExpiry;
        if (days < 0 || days > 30) return;
        resp.add_header("X-Key-Expires-In", std::to_string(days) + "d");
        resp.add_header("X-Key-Self-Renew", "POST /api/auth/mcp-keys/self-renew");
    }

    std::optional<WebServer::AuthResult> WebServer::TrySessionAuth(crow::request const& req) const
    {
        std::string sessionId = ExtractSessionCookie(req);
        if (sessionId.empty()) return std::nullopt;
        auto session = m_WebSessionManager.Validate(sessionId);
        if (!session) return std::nullopt; // stale/unknown cookie — fall through to other paths
        return AuthResult{"", session->m_User, session->m_Role};
    }

    WebServer::AuthResult WebServer::Authenticate(crow::request const& req) const
    {
        // ---- MCP key (both editions) — takes precedence over everything else ----
        if (auto mcp = TryMcpAuth(req); mcp.has_value())
        {
            return *mcp;
        }

        // ---- Dashboard session cookie (both editions; Engine issues, Studio ignores) ----
        if (auto session = TrySessionAuth(req); session.has_value())
        {
            return *session;
        }

#ifdef J9T_STUDIO
        // Studio edition: no auth required for non-MCP, non-session requests (browser UI, localhost).
        (void)req;
        return {"", "studio", "admin"};
#else
        std::string const& ip = req.remote_ip_address;
        std::string const& endpoint = req.url;

        // ---- Lockout check (before rate limiting — locked IPs should not consume tokens) ----
        {
            auto* self = const_cast<WebServer*>(this);
            std::lock_guard<std::mutex> lock(self->m_RateLimitMutex);
            auto it = self->m_AuthFailures.find(ip);
            if (it != self->m_AuthFailures.end())
            {
                auto const elapsed = std::chrono::steady_clock::now() - it->second.m_FirstFailure;
                if (it->second.m_Count >= kMaxAuthFailures && elapsed < kLockoutDuration)
                {
                    LOG_SECURITY_WARN("[security] locked_out ip={} endpoint={}", ip, endpoint);
                    return {"locked_out", "", ""};
                }
            }
        }

        // Rate limit check (cast away const — rate limit state is mutable).
        if (const_cast<WebServer*>(this)->IsRateLimited(req))
        {
            LOG_SECURITY_WARN("[security] rate_limited ip={} endpoint={}", ip, endpoint);
            return {"rate_limited", "", ""};
        }

        // ---- Gateway-trusted identity headers (opt-in via TrustedProxyHeader config) ----
        auto const& config = Core::g_Core->GetConfig();
        if (!config.m_TrustedProxyHeader.empty())
        {
            std::string const& userHeader = req.get_header_value(config.m_TrustedProxyHeader);
            if (!userHeader.empty())
            {
                // Gateway authenticated this request. Extract role from role header.
                std::string role = "viewer"; // default: least privilege
                if (!config.m_TrustedRoleHeader.empty())
                {
                    std::string const& roleHeader = req.get_header_value(config.m_TrustedRoleHeader);
                    if (roleHeader == "admin" || roleHeader == "operator" || roleHeader == "viewer")
                    {
                        role = roleHeader;
                    }
                }

                LOG_SECURITY_INFO("[security] auth_success ip={} user={} role={} method=gateway endpoint={}", ip,
                                  userHeader, role, endpoint);
                return {"", userHeader, role};
            }
            // Gateway header not present — fall through to bearer token check.
        }

        // No auth mechanism matched. MCP key, session cookie, and gateway header
        // are the only supported paths — anything else (including an Authorization
        // header that did not begin with "mcp_") is rejected.
        std::string const& authHeader = req.get_header_value("Authorization");
        if (authHeader.empty())
        {
            LOG_SECURITY_WARN("[security] auth_failure reason=missing_token ip={} endpoint={}", ip, endpoint);
            return {"missing", "", ""};
        }
        LOG_SECURITY_WARN("[security] auth_failure reason=unrecognised_token ip={} endpoint={}", ip, endpoint);
        return {"forbidden", "", ""};
#endif
    }

    // Legacy wrapper — used by existing route lambdas that require admin.
    // Returns empty string if the request is authenticated with admin-equivalent privileges,
    // or an error code ("forbidden", "missing", ...) otherwise.
    std::string WebServer::CheckAdminAuth(crow::request const& req) const
    {
        return CheckAuth(req, "admin");
    }

    std::string WebServer::CheckAuth(crow::request const& req, std::string_view minRole) const
    {
        auto auth = Authenticate(req);
        if (!auth.m_Error.empty()) return auth.m_Error;
        if (!HasRole(auth, minRole))
        {
            // Authenticated but lacks the required role.
            LOG_SECURITY_WARN("[security] forbidden reason=insufficient_role ip={} user={} role={} "
                              "required={} endpoint={}",
                              req.remote_ip_address, auth.m_User, auth.m_Role, minRole, req.url);
            return "forbidden";
        }
        return "";
    }

    void WebServer::RecordAuthFailure(std::string const& ip)
    {
        auto const now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_RateLimitMutex);

        auto& record = m_AuthFailures[ip];
        if (record.m_Count == 0 || (now - record.m_FirstFailure) > kAuthFailureWindow)
        {
            // First failure or window expired — reset.
            record.m_Count = 1;
            record.m_FirstFailure = now;
        }
        else
        {
            record.m_Count++;
        }

        if (record.m_Count == kMaxAuthFailures)
        {
            LOG_SECURITY_WARN("[security] lockout_triggered ip={} failures={}", ip, record.m_Count);
        }
    }

    bool WebServer::IsRateLimited(crow::request const& req)
    {
#ifdef J9T_STUDIO
        (void)req;
        return false;
#else

        std::string const ip = req.remote_ip_address;
        auto const now = std::chrono::steady_clock::now();

        static constexpr double kMaxTokens = 20.0;       // burst capacity
        static constexpr double kRefillRate = 100.0 / 60.0; // tokens per second (100/min)

        std::lock_guard<std::mutex> lock(m_RateLimitMutex);

        // Periodic cleanup: evict buckets older than 10 minutes.
        auto const sinceCleanup = std::chrono::duration_cast<std::chrono::minutes>(now - m_LastRateLimitCleanup);
        if (sinceCleanup.count() >= 5)
        {
            for (auto it = m_RateLimitBuckets.begin(); it != m_RateLimitBuckets.end();)
            {
                auto const age = std::chrono::duration_cast<std::chrono::minutes>(now - it->second.m_LastRefill);
                if (age.count() >= 10)
                {
                    it = m_RateLimitBuckets.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            // Also evict expired lockout entries (older than 15 minutes).
            for (auto it = m_AuthFailures.begin(); it != m_AuthFailures.end();)
            {
                auto const age = std::chrono::duration_cast<std::chrono::minutes>(now - it->second.m_FirstFailure);
                if (age > kLockoutDuration)
                {
                    it = m_AuthFailures.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            m_LastRateLimitCleanup = now;
        }

        auto& bucket = m_RateLimitBuckets[ip];
        double const elapsed = std::chrono::duration<double>(now - bucket.m_LastRefill).count();
        bucket.m_Tokens = std::min(kMaxTokens, bucket.m_Tokens + elapsed * kRefillRate);
        bucket.m_LastRefill = now;

        if (bucket.m_Tokens >= 1.0)
        {
            bucket.m_Tokens -= 1.0;
            return false;
        }

        return true;
#endif
    }

    void WebServer::RegisterRoutes()
    {
        RegisterCommonRoutes();
        RegisterEngineRoutes();
#ifdef J9T_STUDIO
        RegisterStudioRoutes();
#endif
    }

    void WebServer::RegisterCommonRoutes()
    {
        // ---- Public: Dashboard UI (React) — no auth ----
        CROW_ROUTE(m_Server, "/")([this]() { return ServeDashboardIndex(); });
        CROW_ROUTE(m_Server, "/dash-assets/<path>")
        ([this](std::string const& path) { return ServeDashboardStatic(std::string("/dash-assets/") + path); });

        // ---- Public: GET /api/status — no auth (health checks, load balancers) ----
        CROW_ROUTE(m_Server, "/api/status")([this]() { return HandleStatusGet(); });

        // ---- Public: POST /api/mcp/heartbeat — MCP sidecar liveness ----
        CROW_ROUTE(m_Server, "/api/mcp/heartbeat")
            .methods("POST"_method)([this](crow::request const& req) { return HandleMcpHeartbeatPost(); });

        // ---- MCP API keys + dashboard auth (both editions) ----
        // Activation is public: the enrollment token IS the auth.
        CROW_ROUTE(m_Server, "/api/auth/mcp-keys/activate")
            .methods("POST"_method)(
                [this](crow::request const& req) { return HandleMcpKeysActivatePost(req); });

        // Login is public: the MCP key (or gateway header) IS the auth.
        CROW_ROUTE(m_Server, "/api/auth/login")
            .methods("POST"_method)([this](crow::request const& req) { return HandleLoginPost(req); });

        // Logout only needs a session cookie — validation happens inside the handler.
        CROW_ROUTE(m_Server, "/api/auth/logout")
            .methods("POST"_method)([this](crow::request const& req) { return HandleLogoutPost(req); });

        // Key store lifecycle — reachable in both editions (Engine also needs
        // to unlock mcp_keys.json.enc on startup). Public: the submitted master
        // password is itself the credential, so no prior auth is required.
        CROW_ROUTE(m_Server, "/api/settings/keys/status")
            .methods("GET"_method)([this]() { return HandleKeysStatusGet(); });
        CROW_ROUTE(m_Server, "/api/settings/keys/unlock")
            .methods("POST"_method)([this](crow::request const& req) { return HandleKeysUnlockPost(req); });

        // whoami returns identity for the current auth (any successful auth path).
        CROW_ROUTE(m_Server, "/api/auth/whoami")
            ([this](crow::request const& req) { return HandleWhoamiGet(req); });

        // Self-renew requires a still-valid MCP key (not session).
        CROW_ROUTE(m_Server, "/api/auth/mcp-keys/self-renew")
            .methods("POST"_method)(
                [this](crow::request const& req) { return HandleMcpKeysSelfRenewPost(req); });

        // Adhoc workflow submission — MCP key with adhoc_enabled, role ≥ operator.
        CROW_ROUTE(m_Server, "/api/workflows/run-adhoc")
            .methods("POST"_method)(
                [this](crow::request const& req) { return HandleAdhocRunPost(req); });

        // Run artifact discovery — list files produced by a workflow run.
        // Adhoc-only today; registered-workflow attribution arrives with Phase 6+.
        // Authorisation enforced inside the handler (own-run or admin).
        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/files")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    return HandleRunFilesListGet(req, runId);
                });

        // Script catalog — discovery endpoint for MCP agents composing adhoc JCWFs.
        // Viewer+ is enough; there's no sensitive metadata surfaced.
        CROW_ROUTE(m_Server, "/api/scripts")
            .methods("GET"_method)(
                [this](crow::request const& req) { return HandleScriptsListGet(req); });

        // Run artifact download — stream a single file's bytes.
        // <path> captures the entire remaining URL so folder-nested paths work
        // verbatim from the download_url field returned by the list endpoint.
        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/files/<path>")
            .methods("GET"_method)(
                [this](crow::request const& req,
                       std::string const& runId,
                       std::string const& relPath)
                {
                    return HandleRunFileGet(req, runId, relPath);
                });

#ifdef DEBUG
        // Debug introspection endpoint — registered only in debug builds.
        // Release builds have the whole #ifdef block compiled out; the route simply
        // doesn't exist and any request to it returns 404.
        CROW_ROUTE(m_Server, "/api/debug/signals")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleDebugSignalsGet();
                });
#endif

        // Admin CRUD on MCP keys.
        CROW_ROUTE(m_Server, "/api/auth/mcp-keys")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleMcpKeysListGet();
                });
        CROW_ROUTE(m_Server, "/api/auth/mcp-keys/enroll")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleMcpKeysEnrollPost(req);
                });
        CROW_ROUTE(m_Server, "/api/auth/mcp-keys/<string>")
            .methods("PUT"_method)(
                [this](crow::request const& req, std::string const& keyId)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleMcpKeysUpdatePut(req, keyId);
                });
        CROW_ROUTE(m_Server, "/api/auth/mcp-keys/<string>")
            .methods("DELETE"_method)(
                [this](crow::request const& req, std::string const& keyId)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleMcpKeysDelete(keyId);
                });

        // ---- Viewer+: Workflow list + detail (read-only) ----
        CROW_ROUTE(m_Server, "/api/workflows")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty())
                        return MakeAuthErrorResponse(err);
                    return HandleWorkflowsListGet();
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty())
                        return MakeAuthErrorResponse(err);
                    return HandleWorkflowGet(workflowId);
                });

        // ---- Viewer+: Run monitoring (read-only). Operator+ for run-control below. ----
        CROW_ROUTE(m_Server, "/api/workflow-runs/active")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty())
                        return MakeAuthErrorResponse(err);
                    return HandleWorkflowRunsActiveGet();
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/last")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty())
                        return MakeAuthErrorResponse(err);
                    return HandleWorkflowRunsLastGet();
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty())
                        return MakeAuthErrorResponse(err);
                    return HandleWorkflowRunGet(runId);
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/cancel")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                        return MakeAuthErrorResponse(auth.m_Error);
                    if (!HasRole(auth, "operator"))
                        return MakeAuthErrorResponse("insufficient_role");
                    LOG_SECURITY_INFO("[security] run_cancel ip={} user={} runId={}", req.remote_ip_address, auth.m_User,
                                      runId);
                    return HandleWorkflowRunCancelPost(runId);
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/pause")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                        return MakeAuthErrorResponse(auth.m_Error);
                    if (!HasRole(auth, "operator"))
                        return MakeAuthErrorResponse("insufficient_role");
                    LOG_SECURITY_INFO("[security] run_pause ip={} user={} runId={}", req.remote_ip_address, auth.m_User,
                                      runId);
                    return HandleWorkflowRunPausePost(runId);
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/resume")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                        return MakeAuthErrorResponse(auth.m_Error);
                    if (!HasRole(auth, "operator"))
                        return MakeAuthErrorResponse("insufficient_role");
                    LOG_SECURITY_INFO("[security] run_resume ip={} user={} runId={}", req.remote_ip_address, auth.m_User,
                                      runId);
                    return HandleWorkflowRunResumePost(runId);
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/stop")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                        return MakeAuthErrorResponse(auth.m_Error);
                    if (!HasRole(auth, "operator"))
                        return MakeAuthErrorResponse("insufficient_role");
                    LOG_SECURITY_INFO("[security] run_stop ip={} user={} runId={}", req.remote_ip_address, auth.m_User,
                                      runId);
                    return HandleWorkflowRunStopPost(runId);
                });

        // ---- Admin: Log viewer ----
        CROW_ROUTE(m_Server, "/api/log")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                        return MakeAuthErrorResponse(auth.m_Error);
                    if (!HasRole(auth, "operator"))
                        return MakeAuthErrorResponse("insufficient_role");
                    return HandleLogGet(req);
                });

        // ---- Admin: Security log (admin only) ----
        CROW_ROUTE(m_Server, "/api/log/security")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                        return MakeAuthErrorResponse(auth.m_Error);
                    if (!HasRole(auth, "admin"))
                        return MakeAuthErrorResponse("insufficient_role");
                    return HandleSecurityLogGet(req);
                });

        // ---- Admin: POST /api/task/<taskId>/heartbeat ----
        CROW_ROUTE(m_Server, "/api/task/<string>/heartbeat")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& taskId)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty())
                        return MakeAuthErrorResponse(err);

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

        // ---- Admin: POST /api/shutdown (admin only) ----
        CROW_ROUTE(m_Server, "/api/shutdown")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                        return MakeAuthErrorResponse(auth.m_Error);
                    if (!HasRole(auth, "admin"))
                        return MakeAuthErrorResponse("insufficient_role");

                    LOG_SECURITY_INFO("[security] shutdown_requested ip={} user={}", req.remote_ip_address, auth.m_User);
                    Core::g_Core->RequestQuit();
                    auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                    Core::g_Core->PushEvent(event);

                    crow::json::wvalue response;
                    response["message"] = "Shutdown initiated.";
                    return crow::response(200, response);
                });
    }

    void WebServer::RegisterEngineRoutes()
    {
        // ---- Webhook: has its own HMAC auth ----
        CROW_ROUTE(m_Server, "/api/webhook/<string>")
            .methods("POST"_method)([this](crow::request const& req, std::string const& workflowId)
                                    { return HandleWebhookPost(req, workflowId); });

        // ---- Admin: n8n integration ----
        CROW_ROUTE(m_Server, "/api/integrations/n8n/start")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty())
                        return MakeAuthErrorResponse(err);
                    return HandleN8nStartPost(req);
                });
    }

#ifdef J9T_STUDIO
    void WebServer::RegisterStudioRoutes()
    {
        // ---- Workflow Editor UI (React SPA) ----
        CROW_ROUTE(m_Server, "/editor")([this]() { return ServeWorkflowEditorIndex(); });
        CROW_ROUTE(m_Server, "/assets/<path>")
        ([this](std::string const& path) { return ServeWorkflowEditorStatic("/assets/" + path); });
        CROW_ROUTE(m_Server, "/editor/<path>")
        ([this](std::string const& path) { return ServeWorkflowEditorStatic("/editor/" + path); });

        // ---- Workflow CRUD ----
        CROW_ROUTE(m_Server, "/api/workflows")
            .methods("POST"_method)([this](crow::request const& req) { return HandleWorkflowsCreatePost(req); });

        CROW_ROUTE(m_Server, "/api/workflows/reload")
            .methods("POST"_method)([this]() { return HandleWorkflowsReloadPost(); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("PUT"_method)([this](crow::request const& req, std::string const& workflowId)
                                   { return HandleWorkflowUpdatePut(req, workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>")
            .methods("DELETE"_method)(
                [this](std::string const& workflowId) { return HandleWorkflowDelete(workflowId); });

        // ---- Workflow versioning ----
        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions")
            .methods("GET"_method)(
                [this](std::string const& workflowId) { return HandleWorkflowVersionsListGet(workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions/<string>")
            .methods("GET"_method)([this](std::string const& workflowId, std::string const& timestamp)
                                   { return HandleWorkflowVersionGetGet(workflowId, timestamp); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions/<string>/restore")
            .methods("POST"_method)([this](std::string const& workflowId, std::string const& timestamp)
                                    { return HandleWorkflowVersionRestorePost(workflowId, timestamp); });

        // ---- Sub-workflow dependency graph ----
        CROW_ROUTE(m_Server, "/api/workflows/dependency-graph")
            .methods("GET"_method)([this]()
            {
                WorkflowRegistry const* registry = nullptr;
                {
                    std::scoped_lock<std::mutex> const lock(m_Mutex);
                    registry = m_WorkflowRegistry;
                }

                if (registry == nullptr)
                {
                    return crow::response(503, "application/json", R"({"ok":false,"error":"registry_unavailable"})");
                }

                auto const graph = registry->GetSubWorkflowDependencyGraph();

                crow::json::wvalue edgesArray(crow::json::wvalue::list{});
                size_t idx = 0;
                for (auto const& [parentId, children] : graph)
                {
                    for (auto const& childId : children)
                    {
                        crow::json::wvalue edge;
                        edge["parent"] = parentId;
                        edge["child"] = childId;
                        edgesArray[idx++] = std::move(edge);
                    }
                }

                crow::json::wvalue body;
                body["ok"] = true;
                body["edges"] = std::move(edgesArray);
                return crow::response(200, "application/json", body.dump());
            });

        // ---- Sub-workflow tree structure ----
        CROW_ROUTE(m_Server, "/api/workflows/<string>/tree")
            .methods("GET"_method)([this](std::string const& workflowId)
            {
                WorkflowRegistry const* registry = nullptr;
                {
                    std::scoped_lock<std::mutex> const lock(m_Mutex);
                    registry = m_WorkflowRegistry;
                }

                if (registry == nullptr)
                {
                    return crow::response(503, "application/json", R"({"ok":false,"error":"registry_unavailable"})");
                }

                auto const workflowOpt = registry->GetWorkflow(workflowId);
                if (!workflowOpt.has_value())
                {
                    return crow::response(404, "application/json", R"({"ok":false,"error":"not_found"})");
                }

                // Build a tree of sub-workflows by scanning the registry for children.
                auto const allIds = registry->GetWorkflowIds();
                std::string const prefix = workflowId + "__";

                crow::json::wvalue childrenArray(crow::json::wvalue::list{});
                size_t idx = 0;

                for (auto const& id : allIds)
                {
                    auto const childOpt = registry->GetWorkflow(id);
                    if (!childOpt.has_value() || !childOpt->m_IsSubWorkflow)
                    {
                        continue;
                    }
                    if (childOpt->m_ParentWorkflowId != workflowId &&
                        id.rfind(prefix, 0) != 0)
                    {
                        continue;
                    }

                    crow::json::wvalue child;
                    child["id"] = id;
                    child["label"] = childOpt->m_Label;
                    child["folderPath"] = childOpt->m_ContainerFolderPath;
                    child["parentId"] = childOpt->m_ParentWorkflowId;
                    childrenArray[idx++] = std::move(child);
                }

                crow::json::wvalue body;
                body["ok"] = true;
                body["workflowId"] = workflowId;
                body["label"] = workflowOpt->m_Label;
                body["isContainer"] = !workflowOpt->m_ContainerPath.empty();
                body["children"] = std::move(childrenArray);
                return crow::response(200, "application/json", body.dump());
            });

        // ---- Workflow validation + run trigger ----
        CROW_ROUTE(m_Server, "/api/workflows/validate")
            .methods("POST"_method)([this](crow::request const& req) { return HandleWorkflowValidatePost(req); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/validate")
            .methods("GET"_method)(
                [this](std::string const& workflowId) { return HandleWorkflowValidateGet(workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/run")
            .methods("POST"_method)([this](crow::request const& req, std::string const& workflowId)
                                    { return HandleWorkflowRunPost(req, workflowId); });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/clean")
            .methods("DELETE"_method)(
                [this](std::string const& workflowId) { return HandleWorkflowCleanDelete(workflowId); });

        // ---- Script / file check ----
        CROW_ROUTE(m_Server, "/api/scripts/check")
            .methods("GET"_method)([this](crow::request const& req) { return HandleScriptCheckGet(req); });

        CROW_ROUTE(m_Server, "/api/scripts/registry")
            .methods("GET"_method)([this]() { return HandleScriptRegistryGet(); });

        CROW_ROUTE(m_Server, "/api/files/check")
            .methods("GET"_method)([this](crow::request const& req) { return HandleFileCheckGet(req); });

        // ---- Log analysis (requires AI) ----
        CROW_ROUTE(m_Server, "/api/log/analyze-last-run")
            .methods("GET"_method)([this](crow::request const& req) { return HandleLogAnalyzeLastRunGet(req); });

        // ---- AI interfaces API ----
        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces")
            .methods("GET"_method)([this]() { return HandleAiInterfacesListGet(); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces")
            .methods("POST"_method)([this](crow::request const& req) { return HandleAiInterfaceCreatePost(req); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/<string>")
            .methods("PUT"_method)(
                [this](crow::request const& req, std::string const& name) { return HandleAiInterfaceUpdatePut(req, name); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/<string>")
            .methods("DELETE"_method)(
                [this](std::string const& name) { return HandleAiInterfaceDeleteDelete(name); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/save")
            .methods("POST"_method)([this]() { return HandleAiInterfacesSavePost(); });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/test")
            .methods("POST"_method)([this](crow::request const& req) { return HandleAiInterfaceTestPost(req); });

        // ---- Config settings API ----
        CROW_ROUTE(m_Server, "/api/settings/config")
            .methods("GET"_method)([this]() { return HandleConfigSettingsGet(); });

        CROW_ROUTE(m_Server, "/api/settings/config")
            .methods("PUT"_method)([this](crow::request const& req) { return HandleConfigSettingsPut(req); });

        CROW_ROUTE(m_Server, "/api/settings/config/reload")
            .methods("POST"_method)([this]() { return HandleConfigReloadPost(); });

        // ---- Provider settings API ----
        CROW_ROUTE(m_Server, "/api/settings/providers")
            .methods("GET"_method)([this]() { return HandleProvidersListGet(); });

        CROW_ROUTE(m_Server, "/api/settings/providers")
            .methods("POST"_method)([this](crow::request const& req) { return HandleProviderCreatePost(req); });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>")
            .methods("PUT"_method)([this](crow::request const& req, std::string const& providerName)
                                   { return HandleProviderUpdatePut(req, providerName); });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>")
            .methods("DELETE"_method)(
                [this](std::string const& providerName) { return HandleProviderDelete(providerName); });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>/default")
            .methods("POST"_method)(
                [this](std::string const& providerName) { return HandleProviderSetDefaultPost(providerName); });

        CROW_ROUTE(m_Server, "/api/settings/providers/save")
            .methods("POST"_method)([this](crow::request const& req) { return HandleProvidersSavePost(req); });

        // ---- Cloud connections API ----
        CROW_ROUTE(m_Server, "/api/connections")
            .methods("GET"_method)([this]() { return HandleConnectionsListGet(); });

        CROW_ROUTE(m_Server, "/api/connections")
            .methods("POST"_method)([this](crow::request const& req) { return HandleConnectionCreatePost(req); });

        CROW_ROUTE(m_Server, "/api/connections/<string>")
            .methods("PUT"_method)([this](crow::request const& req, std::string const& connectionName)
                                   { return HandleConnectionUpdatePut(req, connectionName); });

        CROW_ROUTE(m_Server, "/api/connections/<string>")
            .methods("DELETE"_method)(
                [this](std::string const& connectionName) { return HandleConnectionDelete(connectionName); });

        CROW_ROUTE(m_Server, "/api/connections/<string>/test")
            .methods("POST"_method)(
                [this](std::string const& connectionName) { return HandleConnectionTestPost(connectionName); });

        CROW_ROUTE(m_Server, "/api/connections/save")
            .methods("POST"_method)([this]() { return HandleConnectionsSavePost(); });

        // OAuth consent flow
        CROW_ROUTE(m_Server, "/api/connections/<string>/oauth/authorize")
            .methods("GET"_method)(
                [this](std::string const& connectionName) { return HandleOAuthAuthorizeGet(connectionName); });

        CROW_ROUTE(m_Server, "/api/connections/<string>/oauth/callback")
            .methods("GET"_method)([this](crow::request const& req, std::string const& connectionName)
                                   { return HandleOAuthCallbackGet(req, connectionName); });
    }
#endif // J9T_STUDIO

    crow::response WebServer::HandleMcpHeartbeatPost()
    {
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_McpLastHeartbeat = std::chrono::steady_clock::now();
        }

        crow::json::wvalue response;
        response["ok"] = true;
        return MakeJsonResponse(200, response);
    }

    crow::response WebServer::HandleStatusGet()
    {
        crow::json::wvalue status;
        status["ok"] = true;

        // Edition + capabilities
#ifdef J9T_STUDIO
        status["edition"] = "studio";
        status["capabilities"]["workflow_crud"] = true;
        status["capabilities"]["workflow_run_endpoint"] = true;
        status["capabilities"]["ai_assistant"] = true;
        status["capabilities"]["ai_jcwf"] = true;
        status["capabilities"]["settings_api"] = true;
#else
        status["edition"] = "engine";
        status["capabilities"]["workflow_crud"] = false;
        status["capabilities"]["workflow_run_endpoint"] = false;
        status["capabilities"]["ai_assistant"] = false;
        status["capabilities"]["ai_jcwf"] = false;
        status["capabilities"]["settings_api"] = false;
#endif

        status["tls"] = m_TlsEnabled;

        // Key unlock state — both the provider keys and the MCP key store.
        {
            auto const& keyManager = Core::g_Core->GetKeyManager();
            status["keys_unlocked"] = (keyManager.GetKeyLoadStatus() == KeyManager::KeyLoadStatus::Ok);
            status["mcp_keys_loaded"] = m_McpKeysLoaded.load();
        }

        // Adhoc workflow submission stats (plan §6).
        if (m_AdhocManager)
        {
            status["adhoc_runs_active"] = static_cast<int64_t>(m_AdhocManager->GetActiveRunCount());
            status["adhoc_disk_usage_bytes"] =
                static_cast<int64_t>(m_AdhocManager->GetTotalDiskUsageBytes());
        }
        else
        {
            status["adhoc_runs_active"] = 0;
            status["adhoc_disk_usage_bytes"] = 0;
        }

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

        // AI dispatch
        JarvisAgent* app = App::g_App;
        {
            AiRequestPool const* pool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;
            status["ai_calls_inflight"] = static_cast<int64_t>(pool != nullptr ? pool->GetDirectDispatchInflight() : 0);
        }

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

        // Python engine pool
        if (app)
        {
            PythonEnginePool* pyPool = app->GetPythonEnginePool();
            if (pyPool)
            {
                status["python_engines"] = static_cast<int64_t>(pyPool->GetEngineCount());
                for (size_t i = 0; i < pyPool->GetEngineCount(); ++i)
                {
                    status["python_engine_tasks_completed"][i] =
                        static_cast<int64_t>(pyPool->GetTasksCompleted(i));
                }
            }
        }

        // MCP sidecar status
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            auto const elapsed = std::chrono::steady_clock::now() - m_McpLastHeartbeat;
            bool const mcpConnected =
                m_McpLastHeartbeat.time_since_epoch().count() > 0 &&
                elapsed < std::chrono::seconds(35);
            status["mcp_connected"] = mcpConnected;
            if (mcpConnected)
            {
                auto const secs =
                    std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                status["mcp_last_heartbeat_secs_ago"] = static_cast<int64_t>(secs);
            }
        }

        // Connection health.  The dashboard LED wants a view of ALL configured
        // connections (otherwise you see "Cloud: no connections" until the
        // circuit breaker has been exercised).  Build the list from the
        // configured connections and overlay any breaker state we have.
        {
            auto const& connectionManager = Core::g_Core->GetCloudConnectionManager();
            auto const& circuitBreaker = Core::g_Core->GetCloudCircuitBreaker();
            auto configuredNames = connectionManager.GetConnectionNames();
            if (!configuredNames.empty())
            {
                std::sort(configuredNames.begin(), configuredNames.end());
                crow::json::wvalue::list connections;
                for (auto const& name : configuredNames)
                {
                    crow::json::wvalue entry;
                    entry["name"] = name;
                    entry["circuit_state"] =
                        CloudCircuitBreaker::StateToString(circuitBreaker.GetState(name));
                    entry["consecutive_failures"] = 0;
                    // confirmed_healthy: true once a successful Test click or JCWF
                    // cloud task has proved this connection.  Drives the LED so
                    // merely-configured connections stay "unknown" instead of
                    // showing as healthy until we have actual evidence.
                    entry["confirmed_healthy"] = circuitBreaker.HasEverSucceeded(name);
                    connections.push_back(std::move(entry));
                }
                // Overlay the breaker's own summary so `consecutive_failures` is
                // filled in for connections that have actually been exercised.
                auto health = circuitBreaker.GetHealthSummary();
                for (auto const& ch : health)
                {
                    // linear scan is fine — N ~= 20 connections in realistic deployments
                    for (size_t i = 0; i < configuredNames.size(); ++i)
                    {
                        if (configuredNames[i] != ch.m_Name) continue;
                        connections[i]["consecutive_failures"] = ch.m_ConsecutiveFailures;
                        break;
                    }
                }
                status["connection_health"] = std::move(connections);
            }
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
                workflowEntry["is_sub_workflow"] = workflowDefinition->m_IsSubWorkflow;

                if (!workflowDefinition->m_ContainerPath.empty())
                {
                    workflowEntry["container_path"] = workflowDefinition->m_ContainerPath;
                }

                if (!workflowDefinition->m_ContainerFolderPath.empty())
                {
                    workflowEntry["container_folder"] = workflowDefinition->m_ContainerFolderPath;
                }

                if (!workflowDefinition->m_ParentWorkflowId.empty())
                {
                    workflowEntry["parent_workflow_id"] = workflowDefinition->m_ParentWorkflowId;
                }
            }

            workflowsList.emplace_back(std::move(workflowEntry));
        }

        responseJson["workflows"] = std::move(workflowsList);

        // Include broken .jcwf files so the editor can show them with error badges.
        auto const& broken = workflowRegistryPtr->GetBrokenWorkflows();
        if (!broken.empty())
        {
            crow::json::wvalue::list brokenList;
            brokenList.reserve(broken.size());
            for (auto const& b : broken)
            {
                crow::json::wvalue entry;
                entry["id"] = b.m_Stem;
                entry["path"] = b.m_ContainerPath;
                entry["error"] = b.m_Error;
                brokenList.emplace_back(std::move(entry));
            }
            responseJson["broken"] = std::move(brokenList);
        }

        return MakeJsonResponse(200, responseJson);
    }

#ifdef J9T_STUDIO
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
#endif // J9T_STUDIO

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

#ifdef J9T_STUDIO
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

        // Write the restored version via registry (handles zip container).
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                std::string upsertErrorMessage;
                if (!m_WorkflowRegistry->SaveOrUpdateWorkflowFromJson(versionContent, targetPath, upsertErrorMessage))
                {
                    return MakeWorkflowJsonError(500, "restore_failed", upsertErrorMessage,
                                                 "POST /api/workflows/{id}/versions/{ts}/restore", workflowId);
                }
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
#endif // J9T_STUDIO

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
        // Body size check.
        auto const maxBodyMB = Core::g_Core->GetConfig().m_MaxRequestBodyMB;
        if (IsBodyTooLarge(req, maxBodyMB))
        {
            LOG_SECURITY_WARN("[security] payload_too_large ip={} endpoint=POST /api/integrations/n8n/start size={}",
                              req.remote_ip_address, req.body.size());
            return MakePayloadTooLargeResponse(maxBodyMB);
        }

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

        // Body size check.
        auto const maxBodyMB = Core::g_Core->GetConfig().m_MaxRequestBodyMB;
        if (IsBodyTooLarge(req, maxBodyMB))
        {
            LOG_SECURITY_WARN("[security] payload_too_large ip={} endpoint={} size={}", req.remote_ip_address, kEndpoint,
                              req.body.size());
            return MakePayloadTooLargeResponse(maxBodyMB);
        }

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
#ifndef J9T_STUDIO
        // Engine mode: webhook secret is mandatory.
        if (webhookTrigger->m_Secret.empty())
        {
            LOG_SECURITY_WARN("[security] webhook_rejected reason=secret_not_configured ip={} workflowId={}",
                              req.remote_ip_address, workflowId);
            LOG_APP_WARN("WebServer::HandleWebhookPost: webhook secret not configured for workflow '{}' "
                         "(required in Engine mode)",
                         workflowId);
            return MakeWorkflowJsonError(403, "secret_required",
                                         "Webhook secret is required in Engine mode. "
                                         "Configure a secret in the workflow trigger.",
                                         kEndpoint, workflowId);
        }
#endif
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
                LOG_SECURITY_WARN("[security] webhook_rejected reason=missing_signature ip={} workflowId={}",
                                  req.remote_ip_address, workflowId);
                LOG_APP_WARN("WebServer::HandleWebhookPost: missing X-Webhook-Signature header for workflow '{}'",
                             workflowId);
                return MakeWorkflowJsonError(401, "missing_signature",
                                             "X-Webhook-Signature header is required for this webhook", kEndpoint,
                                             workflowId);
            }

            if (!VerifyHmacSignature(webhookTrigger->m_Secret, req.body, signatureHeader))
            {
                LOG_SECURITY_WARN("[security] webhook_rejected reason=hmac_mismatch ip={} workflowId={}",
                                  req.remote_ip_address, workflowId);
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

        LOG_SECURITY_INFO("[security] webhook_accepted ip={} workflowId={} runId={}", req.remote_ip_address, workflowId,
                          enqueuedRunId);
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

#ifdef J9T_STUDIO
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
#endif // J9T_STUDIO

    crow::response WebServer::ReadLogFile(crow::request const& req, std::string const& logPath)
    {
        // GET ...?tail=N        — return last N lines (initial load)
        // GET ...?offset=N      — return lines appended since byte offset N (delta polling)
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

        // Count total lines in the file so the frontend can compute absolute line numbers.
        // Re-scan from the beginning up to readPos (the part we didn't read for the tail).
        int64_t skippedLines = 0;
        if (readPos > 0)
        {
            // readPos is where our tail buffer starts; count newlines before that.
            file.clear();
            file.seekg(0);
            static constexpr int64_t kCountChunk = 65536;
            int64_t remaining = readPos;
            std::string buf(static_cast<size_t>(std::min(remaining, kCountChunk)), '\0');
            while (remaining > 0)
            {
                int64_t const toRead = std::min(remaining, kCountChunk);
                buf.resize(static_cast<size_t>(toRead));
                file.read(buf.data(), toRead);
                for (char c : buf)
                {
                    if (c == '\n')
                        ++skippedLines;
                }
                remaining -= toRead;
            }
        }
        int64_t const totalLines = skippedLines + static_cast<int64_t>(allLines.size());

        crow::json::wvalue resp;
        resp["ok"] = true;
        resp["lines"] = std::move(linesJson);
        resp["byteOffset"] = fileSize;
        resp["totalSize"] = fileSize;
        resp["totalLines"] = totalLines;
        return MakeJsonResponse(200, resp);
    }

    crow::response WebServer::HandleLogGet(crow::request const& req)
    {
        return ReadLogFile(req, "log/log.txt");
    }

    crow::response WebServer::HandleSecurityLogGet(crow::request const& req)
    {
        return ReadLogFile(req, "log/security.txt");
    }

#ifdef J9T_STUDIO
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

        // Collect issue lines between start and end (inclusive of the terminal
        // [workflow] run 'X' failed/completed line at endLineIdx — that line is
        // often the most informative ERROR for a failed run).
        //
        // Lines must mention this run's runId or workflowId; concurrent runs
        // interleave in the log so unscoped errors would attribute to the wrong run.
        // Every fail-path log in the backend MUST carry one of those identifiers,
        // otherwise it is invisible to per-run analysis (see feedback_log_failures).
        //
        // Match by log-level tags: [error], [critical], [warning], [warn].
        // Also match [workflow] lines containing "failed" or "skipping" (task-level events).
        crow::json::wvalue::list issuesJson;
        int issueCount = 0;

        int const inclusiveEnd = endLineIdx >= 0 ? endLineIdx + 1 : searchEnd;
        for (int i = startLineIdx; i < inclusiveEnd; ++i)
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
#endif // J9T_STUDIO

    void WebServer::RegisterWebSocket()
    {
        CROW_WEBSOCKET_ROUTE(m_Server, "/ws")
#ifndef J9T_STUDIO
            // Engine: validate the dashboard session cookie at upgrade time.
            // Studio has no auth; Crow's .onaccept is omitted there to allow all local upgrades.
            .onaccept(
                [this](crow::request const& req, void**)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                    {
                        LOG_SECURITY_WARN("[security] ws_upgrade_rejected ip={} reason={}",
                                          req.remote_ip_address, auth.m_Error);
                        return false;
                    }
                    return true;
                })
#endif
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

                        // Auth happens at the upgrade handshake (.onaccept) in Engine; by the
                        // time we are in onmessage, the connection is already trusted.

                        // Heartbeat from dashboard — no response needed, but must
                        // NOT return early so DrainPendingBroadcasts() at the end
                        // still runs.
                        if (type == "ping")
                        {
                            // fall through to drain
                        }

                        if (type == "workflow-runs-request")
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
#ifdef J9T_STUDIO
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
#endif // J9T_STUDIO

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

#ifdef J9T_STUDIO
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
#endif // J9T_STUDIO

    bool WebServer::Start()
    {
        if (m_Running)
        {
            return true;
        }

        // ---- Determine TLS mode from config ----
        auto const& config = Core::g_Core->GetConfig();
        bool const hasCert = !config.m_TlsCert.empty();
        bool const hasKey = !config.m_TlsKey.empty();

        if (hasCert != hasKey)
        {
            LOG_APP_CRITICAL("[web] TLS misconfigured: both TlsCert and TlsKey must be set (got cert={}, key={})",
                             hasCert ? "yes" : "no", hasKey ? "yes" : "no");
            return false;
        }

        m_TlsEnabled = hasCert && hasKey;
        uint16_t const defaultPort = m_TlsEnabled ? 8443 : 8080;
        uint16_t const port = (config.m_Port != 0) ? config.m_Port : defaultPort;

        if (m_TlsEnabled)
        {
            if (!std::filesystem::exists(config.m_TlsCert))
            {
                LOG_APP_CRITICAL("[web] TLS certificate file not found: {}", config.m_TlsCert);
                return false;
            }
            if (!std::filesystem::exists(config.m_TlsKey))
            {
                LOG_APP_CRITICAL("[web] TLS key file not found: {}", config.m_TlsKey);
                return false;
            }
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
            addr.sin_port = htons(port);

            bool const portAvailable = (::bind(testSocket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
#if defined(_WIN32)
            ::closesocket(testSocket);
#else
            ::close(testSocket);
#endif

            if (!portAvailable)
            {
                LOG_APP_CRITICAL("[web] Port {} is already in use — is another JarvisAgent running? Exiting.", port);
                return false;
            }
        }

        if (m_TlsEnabled)
        {
            m_Server.ssl_file(config.m_TlsCert, config.m_TlsKey);
        }

        // Try to initialise the MCP key store if the master password is already cached
        // (from engine-level keys.json.enc load at startup). If the password isn't
        // cached yet, the store is lazily initialised on HandleKeysUnlockPost().
        {
            auto& keyManager = Core::g_Core->GetKeyManager();
            std::string const cachedPwd = keyManager.GetCachedMasterPassword();
            if (!cachedPwd.empty())
            {
                InitMcpKeyStore(cachedPwd);
            }
            else
            {
                LOG_CORE_INFO("MCP key store deferred — awaiting master password via /api/settings/keys/unlock");
            }
        }

        m_Running = true;
        m_ServerThread = std::thread(
            [this, port]()
            {
                if (m_TlsEnabled)
                {
                    LOG_APP_INFO("Crow web server started at https://localhost:{}", port);
                }
                else
                {
                    LOG_APP_INFO("Crow web server started at http://localhost:{}", port);
                }
                m_Server.port(port).multithreaded().signal_clear().run();
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

#ifdef J9T_STUDIO
        // Shut down the AI JCWF service so background threads are joined.
        m_AiJcwfService.Shutdown();

        // Assistant controller is shut down early via ShutdownAssistantController().
        // The call here is a no-op safety net (Shutdown() is idempotent).
        m_AssistantController.Shutdown();
#endif

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

        if (safeBatch.size() != batch.size())
        {
            LOG_APP_WARN("[ws] SanitizeUtf8 changed batch: {}B -> {}B", batch.size(), safeBatch.size());
        }
        for (auto* client : clients)
        {
            // Re-check that the client is still alive under the lock.
            // Between the snapshot and here, onclose may have fired on another ASIO
            // thread, destroying the connection and leaving a dangling pointer.
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                if (m_Clients.find(client) == m_Clients.end())
                    continue;
            }
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

    bool WebServer::IsMcpConnected()
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        if (m_McpLastHeartbeat.time_since_epoch().count() == 0) return false;
        auto const elapsed = std::chrono::steady_clock::now() - m_McpLastHeartbeat;
        return elapsed < std::chrono::seconds(35);
    }

    void WebServer::BroadcastAiCallStarted(std::string const& probName, std::string const& interfaceName)
    {
        crow::json::wvalue msg;
        msg["type"] = "ai-call-started";
        msg["prob"] = probName;
        msg["interface"] = interfaceName;
        BroadcastJSON(msg.dump());
    }

    void WebServer::BroadcastAiCallCompleted(std::string const& probName, int32_t inputTokens,
                                             int32_t outputTokens, int32_t totalTokens,
                                             std::string const& finishReason)
    {
        crow::json::wvalue msg;
        msg["type"] = "ai-call-completed";
        msg["prob"] = probName;
        msg["input_tokens"] = static_cast<int64_t>(inputTokens);
        msg["output_tokens"] = static_cast<int64_t>(outputTokens);
        msg["total_tokens"] = static_cast<int64_t>(totalTokens);
        msg["finish_reason"] = finishReason;
        BroadcastJSON(msg.dump());
    }

    void WebServer::BroadcastAiCallFailed(std::string const& probName, int errorKind,
                                          int httpStatus, std::string const& errorMessage)
    {
        crow::json::wvalue msg;
        msg["type"] = "ai-call-failed";
        msg["prob"] = probName;
        msg["error_kind"] = static_cast<int64_t>(errorKind);
        msg["http_status"] = static_cast<int64_t>(httpStatus);
        msg["error_message"] = errorMessage;
        BroadcastJSON(msg.dump());
    }

#ifdef J9T_STUDIO
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
            switch (iface.m_InterfaceType)
            {
                case ConfigParser::EngineConfig::InterfaceType::API2: item["api_type"] = "API2"; break;
                case ConfigParser::EngineConfig::InterfaceType::API3: item["api_type"] = "API3"; break;
                case ConfigParser::EngineConfig::InterfaceType::API4: item["api_type"] = "API4"; break;
                case ConfigParser::EngineConfig::InterfaceType::Test: item["api_type"] = "Test"; break;
                case ConfigParser::EngineConfig::InterfaceType::API1Azure: item["api_type"] = "API1Azure"; break;
                case ConfigParser::EngineConfig::InterfaceType::API5: item["api_type"] = "API5"; break;
                default: item["api_type"] = "API1"; break;
            }
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
        uint64_t maxContextTokensOverride = 0; // 0 = fall back to model-name resolution

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
                {
                    auto d2 = parser.iterate(json);
                    int64_t ctxVal = 0;
                    if (d2["max_context_tokens"].get_int64().get(ctxVal) == simdjson::SUCCESS && ctxVal > 0)
                    {
                        maxContextTokensOverride = static_cast<uint64_t>(ctxVal);
                    }
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
        newIface.m_MaxContextTokens =
            maxContextTokensOverride > 0
                ? maxContextTokensOverride
                : ConfigParser::EngineConfig::ResolveMaxContextTokensFromModel(model);

        if (apiTypeStr == "API4")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API4;
        else if (apiTypeStr == "API3")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API3;
        else if (apiTypeStr == "API2")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API2;
        else if (apiTypeStr == "Test")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::Test;
        else if (apiTypeStr == "API1Azure")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API1Azure;
        else if (apiTypeStr == "API5")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API5;
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
                        if (apiTypeStr == "API4")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API4;
                        else if (apiTypeStr == "API3")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API3;
                        else if (apiTypeStr == "API2")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API2;
                        else if (apiTypeStr == "Test")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::Test;
                        else if (apiTypeStr == "API1Azure")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API1Azure;
                        else if (apiTypeStr == "API5")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API5;
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
            std::string apiStr;
            switch (iface.m_InterfaceType)
            {
                case ConfigParser::EngineConfig::InterfaceType::API2: apiStr = "API2"; break;
                case ConfigParser::EngineConfig::InterfaceType::API3: apiStr = "API3"; break;
                case ConfigParser::EngineConfig::InterfaceType::API4: apiStr = "API4"; break;
                case ConfigParser::EngineConfig::InterfaceType::Test: apiStr = "Test"; break;
                case ConfigParser::EngineConfig::InterfaceType::API1Azure: apiStr = "API1Azure"; break;
                case ConfigParser::EngineConfig::InterfaceType::API5: apiStr = "API5"; break;
                default: apiStr = "API1"; break;
            }

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

#endif // J9T_STUDIO

    // =========================================================================
    // Key management API handlers (both editions — Engine also needs unlock)
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

        auto const& keysPath = keyManager.GetKeysFilePath();
        bool const keysFileExists = !keysPath.empty() && std::filesystem::exists(keysPath);

        bool bootstrapped = false;
        if (keysFileExists)
        {
            // Existing install — try to decrypt with the submitted password.
            if (!keyManager.Unlock(masterPassword))
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["status"] = "wrong_password";
                err["message"] = "Incorrect master password. Please try again.";
                return MakeJsonResponse(401, err);
            }
        }
        else
        {
            // First-run bootstrap — no encrypted keys file exists yet. Treat the
            // submitted password as the *new* master password for this install:
            // write an empty encrypted keys file so later provider additions go
            // straight into the same encrypted store.
            if (keysPath.empty())
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["status"] = "internal_error";
                err["message"] = "Keys file path not configured.";
                return MakeJsonResponse(500, err);
            }
            if (!keyManager.Save(keysPath, masterPassword))
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["status"] = "bootstrap_failed";
                err["message"] = "Failed to create encrypted keys file.";
                return MakeJsonResponse(500, err);
            }
            keyManager.SetKeyLoadStatus(KeyManager::KeyLoadStatus::Ok);
            LOG_SECURITY_INFO(
                "[security] bootstrap: master password set, encrypted keys store created at '{}'",
                keysPath.string());
            bootstrapped = true;
        }

        // Same master password unlocks (or creates) the MCP key store.
        InitMcpKeyStore(masterPassword);

        // Re-hydrate OAuth tokens now that providers are readable. The initial
        // HydrateFromKeyManager call in Core::Initialize ran before unlock, so
        // it saw an empty provider map; without this call, persisted OAuth
        // refresh_tokens (Google Sheets, OneDrive) would never be restored
        // across restarts and every provider would need re-authorisation.
        Core::g_Core->GetOAuthTokenManager().HydrateFromKeyManager();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["status"] = "ok";
        responseJson["message"] = bootstrapped
                                      ? "Master password set. Encrypted key stores created."
                                      : "Keys unlocked successfully.";
        responseJson["bootstrapped"] = bootstrapped;
        responseJson["mcp_keys_loaded"] = m_McpKeysLoaded.load();

        // If the MCP key store is empty at this point — either because this is
        // a truly fresh install (bootstrap) or because mcp_keys.json.enc was
        // deleted without keys.json.enc — hand the admin an MCP key in the
        // response. CreateBootstrapAdminKey is a no-op when the store already
        // has keys, so the call is idempotent and safe outside the `bootstrapped`
        // branch.
        if (m_McpKeysLoaded.load())
        {
            auto admin = m_McpKeyManager.CreateBootstrapAdminKey();
            if (admin)
            {
                SaveMcpKeyStore();
                LOG_SECURITY_INFO(
                    "[security] bootstrap: first admin MCP key issued (key_id={}, user={})",
                    admin->m_KeyId, admin->m_Record.m_User);

                crow::json::wvalue admWval;
                admWval["key_id"] = admin->m_KeyId;
                admWval["api_key"] = admin->m_RawKey;
                admWval["user"] = admin->m_Record.m_User;
                admWval["role"] = admin->m_Record.m_Role;
                admWval["expires_at"] = admin->m_Record.m_ExpiresAt;
                responseJson["admin_key"] = std::move(admWval);
            }
        }

        return MakeJsonResponse(200, responseJson);
    }

#ifdef J9T_STUDIO
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
            entry["credential_type"] = provider->m_CredentialType;

            // Type-specific metadata (secrets NOT returned)
            if (provider->m_CredentialType == "oauth")
            {
                entry["has_refresh_token"] = !provider->m_RefreshToken.empty();
                entry["expires_at"] = provider->m_ExpiresAt;
                entry["scopes"] = provider->m_Scopes;
            }
            else if (provider->m_CredentialType == "credentials")
            {
                entry["username"] = provider->m_Username;
            }

            // Return non-secret params; strip known-sensitive keys. AWS secret_access_key /
            // session_token must never leave the server. Extend the blocklist as new
            // sensitive param keys are introduced.
            if (!provider->m_Params.empty())
            {
                static std::array<char const*, 2> const kSensitiveParamKeys = {"secret_access_key", "session_token"};
                crow::json::wvalue paramsJson;
                for (auto const& [k, v] : provider->m_Params)
                {
                    bool sensitive = false;
                    for (auto const* skey : kSensitiveParamKeys)
                    {
                        if (k == skey) { sensitive = true; break; }
                    }
                    if (sensitive)
                    {
                        // Surface a "set / not set" boolean so the UI can render an indicator
                        // without the value crossing the network.
                        entry[std::string("has_") + k] = !v.empty();
                    }
                    else
                    {
                        paramsJson[k] = v;
                    }
                }
                entry["params"] = std::move(paramsJson);
            }

            // API key and private_key_pem are intentionally NOT returned for security.
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
            if (doc["credential_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_CredentialType = std::string(sv);
            }
            if (doc["scopes"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Scopes = std::string(sv);
            }
            if (doc["private_key_pem"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_PrivateKeyPem = std::string(sv);
            }
            if (doc["username"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Username = std::string(sv);
            }
            if (doc["password"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Password = std::string(sv);
            }

            // Optional per-provider params (Azure resource/deployment/api_version, AWS region/secrets, ...).
            {
                simdjson::ondemand::object paramsObj;
                if (doc["params"].get_object().get(paramsObj) == simdjson::SUCCESS)
                {
                    for (auto paramField : paramsObj)
                    {
                        std::string_view paramKey = paramField.unescaped_key();
                        std::string_view paramVal;
                        if (paramField.value().get_string().get(paramVal) == simdjson::SUCCESS)
                        {
                            config.m_Params[std::string(paramKey)] = std::string(paramVal);
                        }
                    }
                }
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
            if (doc["credential_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_CredentialType = std::string(sv);
            }
            if (doc["scopes"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Scopes = std::string(sv);
            }
            if (doc["private_key_pem"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_PrivateKeyPem = std::string(sv);
            }
            if (doc["username"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Username = std::string(sv);
            }
            if (doc["password"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Password = std::string(sv);
            }

            // Per-provider params: replace wholesale if the field is present (UI sends the
            // full edited map); leave existing values untouched if absent.
            {
                simdjson::ondemand::object paramsObj;
                if (doc["params"].get_object().get(paramsObj) == simdjson::SUCCESS)
                {
                    config.m_Params.clear();
                    for (auto paramField : paramsObj)
                    {
                        std::string_view paramKey = paramField.unescaped_key();
                        std::string_view paramVal;
                        if (paramField.value().get_string().get(paramVal) == simdjson::SUCCESS)
                        {
                            config.m_Params[std::string(paramKey)] = std::string(paramVal);
                        }
                    }
                }
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

        // Master password: request body or already-cached password from a prior unlock.
        // There is no env-var fallback — see doc/cyber security.md §"Master password after restart".
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
                // Body might be empty or not JSON — fall through to cached password.
            }
        }

        if (masterPassword.empty())
        {
            masterPassword = keyManager.GetCachedMasterPassword();
        }

        if (masterPassword.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "no_password";
            err["message"] = "Master password required. Include it in the request body, or unlock the key "
                              "store first via POST /api/settings/keys/unlock.";
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

    // ================================================================
    // Cloud connections API
    // ================================================================

    crow::response WebServer::HandleConnectionsListGet()
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["dirty"] = connectionManager.IsDirty();

        auto connections = connectionManager.GetAllConnections();
        std::vector<crow::json::wvalue> connList;
        connList.reserve(connections.size());

        for (auto const& conn : connections)
        {
            crow::json::wvalue c;
            c["name"] = conn.m_Name;
            c["type"] = conn.m_Type;
            c["endpoint"] = conn.m_Endpoint;
            c["key_name"] = conn.m_KeyName;
            c["auth_type"] = AuthTypeToString(conn.m_AuthType);

            crow::json::wvalue params;
            for (auto const& [key, val] : conn.m_Params)
            {
                params[key] = val;
            }
            c["params"] = std::move(params);

            connList.push_back(std::move(c));
        }
        responseJson["connections"] = std::move(connList);

        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleConnectionCreatePost(crow::request const& req)
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string json(req.body);
            auto doc = parser.iterate(json);

            CloudConnection conn;
            std::string_view sv;

            if (doc["name"].get_string().get(sv) != simdjson::SUCCESS || sv.empty())
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "missing_name";
                err["message"] = "Connection name is required";
                return MakeJsonResponse(400, err);
            }
            conn.m_Name = std::string(sv);

            if (doc["type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_Type = std::string(sv);
            }
            if (doc["endpoint"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_Endpoint = std::string(sv);
            }
            if (doc["key_name"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_KeyName = std::string(sv);
            }
            if (doc["auth_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_AuthType = StringToAuthType(sv);
            }

            // Parse type-specific params
            simdjson::ondemand::object params;
            if (doc["params"].get_object().get(params) == simdjson::SUCCESS)
            {
                for (auto field : params)
                {
                    std::string_view key = field.unescaped_key();
                    std::string_view val;
                    if (field.value().get_string().get(val) == simdjson::SUCCESS)
                    {
                        conn.m_Params[std::string(key)] = std::string(val);
                    }
                }
            }

            if (!connectionManager.AddConnection(std::move(conn)))
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "already_exists";
                err["message"] = "Connection with this name already exists";
                return MakeJsonResponse(409, err);
            }

            crow::json::wvalue responseJson;
            responseJson["ok"] = true;
            responseJson["name"] = std::string(conn.m_Name);
            return MakeJsonResponse(201, responseJson);
        }
        catch (simdjson::simdjson_error const& e)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_json";
            err["message"] = std::string("JSON parse error: ") + e.what();
            return MakeJsonResponse(400, err);
        }
    }

    crow::response WebServer::HandleConnectionUpdatePut(crow::request const& req, std::string const& connectionName)
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

        CloudConnection const* existing = connectionManager.GetConnection(connectionName);
        if (!existing)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Connection '" + connectionName + "' not found";
            return MakeJsonResponse(404, err);
        }

        // Start with existing config and overlay provided fields
        CloudConnection updated = *existing;

        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string json(req.body);
            auto doc = parser.iterate(json);

            std::string_view sv;
            if (doc["type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                updated.m_Type = std::string(sv);
            }
            if (doc["endpoint"].get_string().get(sv) == simdjson::SUCCESS)
            {
                updated.m_Endpoint = std::string(sv);
            }
            if (doc["key_name"].get_string().get(sv) == simdjson::SUCCESS)
            {
                updated.m_KeyName = std::string(sv);
            }
            if (doc["auth_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                updated.m_AuthType = StringToAuthType(sv);
            }

            simdjson::ondemand::object params;
            if (doc["params"].get_object().get(params) == simdjson::SUCCESS)
            {
                updated.m_Params.clear();
                for (auto field : params)
                {
                    std::string_view key = field.unescaped_key();
                    std::string_view val;
                    if (field.value().get_string().get(val) == simdjson::SUCCESS)
                    {
                        updated.m_Params[std::string(key)] = std::string(val);
                    }
                }
            }
        }
        catch (simdjson::simdjson_error const& e)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_json";
            err["message"] = std::string("JSON parse error: ") + e.what();
            return MakeJsonResponse(400, err);
        }

        if (!connectionManager.UpdateConnection(connectionName, std::move(updated)))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "update_failed";
            err["message"] = "Failed to update connection";
            return MakeJsonResponse(500, err);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["name"] = connectionName;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleConnectionDelete(std::string const& connectionName)
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

        if (!connectionManager.RemoveConnection(connectionName))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Connection '" + connectionName + "' not found";
            return MakeJsonResponse(404, err);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleConnectionTestPost(std::string const& connectionName)
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();
        auto& connectorRegistry = Core::g_Core->GetCloudConnectorRegistry();

        CloudConnection const* connection = connectionManager.GetConnection(connectionName);
        if (!connection)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Connection '" + connectionName + "' not found";
            return MakeJsonResponse(404, err);
        }

        ICloudConnector* connector = connectorRegistry.GetConnector(connection->m_Type);
        if (!connector)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "no_connector";
            err["message"] = "No connector registered for type '" + connection->m_Type + "'";
            return MakeJsonResponse(400, err);
        }

        std::string errorMessage;
        bool success = connector->TestConnection(*connection, errorMessage);

        // Record the outcome on the circuit breaker so the dashboard Cloud LED
        // lights up as soon as a Test button (not just a JCWF cloud task) has
        // proved a connection works end-to-end.
        auto& circuitBreaker = Core::g_Core->GetCloudCircuitBreaker();
        if (success)
        {
            circuitBreaker.RecordSuccess(connectionName);
        }
        else
        {
            circuitBreaker.RecordFailure(connectionName);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = success;
        if (!success)
        {
            responseJson["error"] = "test_failed";
            responseJson["message"] = errorMessage;
        }
        return MakeJsonResponse(success ? 200 : 400, responseJson);
    }

    crow::response WebServer::HandleConnectionsSavePost()
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

        std::string json = connectionManager.SerializeToJson();

        // Save to connections.json in the launch directory
        std::filesystem::path const connectionsFilePath =
            Core::g_Core->GetLaunchCWDAbsolute() / "connections.json";

        std::ofstream file(connectionsFilePath);
        if (!file)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "save_failed";
            err["message"] = "Failed to open '" + connectionsFilePath.string() + "' for writing";
            return MakeJsonResponse(500, err);
        }

        file << json;
        file.close();
        connectionManager.ClearDirty();

        LOG_SECURITY_INFO("[security] cloud_connections saved to {}", connectionsFilePath.string());

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["path"] = connectionsFilePath.string();
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleOAuthAuthorizeGet(std::string const& connectionName)
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

        CloudConnection const* connection = connectionManager.GetConnection(connectionName);
        if (!connection)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Connection '" + connectionName + "' not found";
            return MakeJsonResponse(404, err);
        }

        if (connection->m_AuthType != CloudAuthType::OAuth2)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_auth_type";
            err["message"] = "Connection '" + connectionName + "' does not use OAuth2 authentication";
            return MakeJsonResponse(400, err);
        }

        auto clientIdIt = connection->m_Params.find("client_id");
        if (clientIdIt == connection->m_Params.end() || clientIdIt->second.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "missing_client_id";
            err["message"] = "Connection '" + connectionName + "' requires 'client_id' parameter";
            return MakeJsonResponse(400, err);
        }

        std::string clientId = clientIdIt->second;

        // Look up the connector for this connection's type so we can get provider-specific
        // OAuth2 endpoints and parameters (Google vs Microsoft vs future providers).
        auto& connectorRegistry = Core::g_Core->GetCloudConnectorRegistry();
        ICloudConnector* connector = connectorRegistry.GetConnector(connection->m_Type);
        if (!connector)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "no_connector";
            err["message"] = "No connector registered for type '" + connection->m_Type + "'";
            return MakeJsonResponse(400, err);
        }

        OAuth2ProviderInfo providerInfo;
        if (!connector->GetOAuth2ProviderInfo(*connection, providerInfo))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "oauth2_not_supported";
            err["message"] = "Connector '" + connection->m_Type + "' does not support OAuth2";
            return MakeJsonResponse(400, err);
        }

        auto scopesIt = connection->m_Params.find("scopes");
        std::string scopes = (scopesIt != connection->m_Params.end() && !scopesIt->second.empty())
                                 ? scopesIt->second
                                 : providerInfo.m_DefaultScopes;

        // Proper base64url encoder (no padding) — RFC 4648 §5, used by PKCE per RFC 7636.
        auto base64UrlEncode = [](unsigned char const* data, size_t len) -> std::string
        {
            static char const alphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            std::string out;
            out.reserve(((len + 2) / 3) * 4);
            size_t i = 0;
            while (i + 3 <= len)
            {
                uint32_t const v =
                    (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
                out += alphabet[(v >> 18) & 0x3F];
                out += alphabet[(v >> 12) & 0x3F];
                out += alphabet[(v >> 6) & 0x3F];
                out += alphabet[v & 0x3F];
                i += 3;
            }
            if (i < len)
            {
                uint32_t v = uint32_t(data[i]) << 16;
                if (i + 1 < len)
                {
                    v |= uint32_t(data[i + 1]) << 8;
                }
                out += alphabet[(v >> 18) & 0x3F];
                out += alphabet[(v >> 12) & 0x3F];
                if (i + 1 < len)
                {
                    out += alphabet[(v >> 6) & 0x3F];
                }
            }
            return out;
        };

        // Build the authorization URL with PKCE.
        // PKCE code_verifier: base64url-encoded 32 random bytes → 43 chars (no padding),
        // within the RFC 7636 range of 43–128 chars.
        unsigned char randomBytes[32];
        RAND_bytes(randomBytes, sizeof(randomBytes));
        std::string codeVerifier = base64UrlEncode(randomBytes, sizeof(randomBytes));

        // code_challenge = BASE64URL(SHA256(code_verifier))
        unsigned char hash[32];
        EVP_Digest(codeVerifier.data(), codeVerifier.size(), hash, nullptr, EVP_sha256(), nullptr);
        std::string codeChallenge = base64UrlEncode(hash, sizeof(hash));

        // CSRF state token: 16 random bytes, base64url-encoded.
        unsigned char stateBytes[16];
        RAND_bytes(stateBytes, sizeof(stateBytes));
        std::string stateToken = base64UrlEncode(stateBytes, sizeof(stateBytes));

        // Store code_verifier and state token for the callback (keyed by connection name)
        // Using a simple in-memory map — acceptable since OAuth flows are short-lived
        {
            std::lock_guard lock(m_OAuthStateMutex);
            m_OAuthCodeVerifiers[connectionName] = codeVerifier;
            m_OAuthStateTokens[connectionName] = stateToken;
        }

        // Build redirect URI — the callback endpoint on this server.
        // Use https:// when TLS is configured so the redirect URI the provider sends the
        // browser to actually matches the scheme the server listens on.
        auto const& cfg = Core::g_Core->GetConfig();
        uint16_t port = (cfg.m_Port != 0) ? cfg.m_Port : static_cast<uint16_t>(8080);
        std::string const scheme = (!cfg.m_TlsCert.empty() && !cfg.m_TlsKey.empty()) ? "https" : "http";
        std::string redirectUri = scheme + "://localhost:" + std::to_string(port) +
                                  "/api/connections/" + connectionName + "/oauth/callback";

        // Percent-encode a string for safe inclusion in URL query parameters.
        auto percentEncode = [](std::string const& input)
        {
            static char const kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(input.size() * 3);
            for (unsigned char c : input)
            {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.' || c == '~')
                {
                    out += static_cast<char>(c);
                }
                else
                {
                    out += '%';
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                }
            }
            return out;
        };

        std::string fullUrl = providerInfo.m_AuthorizeUrl + "?client_id=" + percentEncode(clientId) +
                              "&response_type=code" + "&redirect_uri=" + percentEncode(redirectUri) +
                              "&scope=" + percentEncode(scopes) + "&code_challenge=" + codeChallenge +
                              "&code_challenge_method=S256" + "&state=" + stateToken;
        for (auto const& [k, v] : providerInfo.m_ExtraAuthorizeParams)
        {
            fullUrl += "&" + percentEncode(k) + "=" + percentEncode(v);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["authorize_url"] = fullUrl;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleOAuthCallbackGet(crow::request const& req, std::string const& connectionName)
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

        CloudConnection const* connection = connectionManager.GetConnection(connectionName);
        if (!connection)
        {
            return crow::response(400, "Connection not found: " + connectionName);
        }

        // Extract authorization code from query params
        auto codeParam = req.url_params.get("code");
        if (!codeParam)
        {
            auto errorParam = req.url_params.get("error");
            auto errorDescParam = req.url_params.get("error_description");
            std::string errorMsg = "OAuth authorization failed";
            if (errorParam)
            {
                errorMsg += ": " + std::string(errorParam);
            }
            if (errorDescParam)
            {
                errorMsg += " — " + std::string(errorDescParam);
            }
            LOG_CORE_ERROR("{}", errorMsg);
            return crow::response(400, errorMsg);
        }

        std::string authCode = codeParam;

        // Validate CSRF state parameter
        auto stateParam = req.url_params.get("state");
        {
            std::lock_guard lock(m_OAuthStateMutex);

            // Verify state token
            auto stateIt = m_OAuthStateTokens.find(connectionName);
            if (stateIt == m_OAuthStateTokens.end())
            {
                LOG_CORE_ERROR("OAuth callback for '{}': no pending state token (possible CSRF)", connectionName);
                return crow::response(400, "No pending OAuth state for connection '" + connectionName + "'");
            }
            if (!stateParam || stateIt->second != std::string(stateParam))
            {
                m_OAuthStateTokens.erase(stateIt);
                LOG_CORE_ERROR("OAuth callback for '{}': state mismatch (possible CSRF attack)", connectionName);
                return crow::response(400, "OAuth state mismatch — possible CSRF attack");
            }
            m_OAuthStateTokens.erase(stateIt);
        }

        // Retrieve the code_verifier for PKCE
        std::string codeVerifier;
        {
            std::lock_guard lock(m_OAuthStateMutex);
            auto it = m_OAuthCodeVerifiers.find(connectionName);
            if (it == m_OAuthCodeVerifiers.end())
            {
                return crow::response(400, "No pending OAuth flow for connection '" + connectionName + "'");
            }
            codeVerifier = it->second;
            m_OAuthCodeVerifiers.erase(it);
        }

        auto clientIdIt = connection->m_Params.find("client_id");
        std::string clientId = (clientIdIt != connection->m_Params.end()) ? clientIdIt->second : "";

        // Look up provider info via the registered connector (Google vs Microsoft vs ...).
        auto& connectorRegistryCb = Core::g_Core->GetCloudConnectorRegistry();
        ICloudConnector* connectorCb = connectorRegistryCb.GetConnector(connection->m_Type);
        if (!connectorCb)
        {
            return crow::response(400, "No connector registered for type '" + connection->m_Type + "'");
        }
        OAuth2ProviderInfo providerInfo;
        if (!connectorCb->GetOAuth2ProviderInfo(*connection, providerInfo))
        {
            return crow::response(400, "Connector '" + connection->m_Type + "' does not support OAuth2");
        }
        std::string tokenUrl = providerInfo.m_TokenUrl;

        auto const& cfg = Core::g_Core->GetConfig();
        uint16_t port = (cfg.m_Port != 0) ? cfg.m_Port : static_cast<uint16_t>(8080);
        std::string const scheme = (!cfg.m_TlsCert.empty() && !cfg.m_TlsKey.empty()) ? "https" : "http";
        std::string redirectUri = scheme + "://localhost:" + std::to_string(port) +
                                  "/api/connections/" + connectionName + "/oauth/callback";

        // Percent-encode a string for safe inclusion in x-www-form-urlencoded bodies.
        auto percentEncodeCb = [](std::string const& input)
        {
            static char const kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(input.size() * 3);
            for (unsigned char c : input)
            {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.' || c == '~')
                {
                    out += static_cast<char>(c);
                }
                else
                {
                    out += '%';
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                }
            }
            return out;
        };

        // Exchange authorization code for tokens.
        std::string postBody = "grant_type=authorization_code"
                               "&code=" + percentEncodeCb(authCode) +
                               "&redirect_uri=" + percentEncodeCb(redirectUri) +
                               "&client_id=" + percentEncodeCb(clientId) +
                               "&code_verifier=" + codeVerifier;

        // Google (and other confidential clients) require client_secret in the token
        // exchange in addition to PKCE.  Microsoft PKCE public clients do not.
        if (providerInfo.m_RequiresClientSecret)
        {
            auto clientSecretIt = connection->m_Params.find("client_secret");
            if (clientSecretIt == connection->m_Params.end() || clientSecretIt->second.empty())
            {
                LOG_CORE_ERROR("OAuth callback for '{}': provider requires client_secret but connection has none",
                               connectionName);
                return crow::response(400, "Connection '" + connectionName +
                                               "' requires 'client_secret' parameter for this provider");
            }
            postBody += "&client_secret=" + percentEncodeCb(clientSecretIt->second);
        }

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            return crow::response(500, "curl_easy_init() failed");
        }

        std::string responseBody;
        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            buf->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, tokenUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            std::string errMsg = std::string("Token exchange failed: ") + curl_easy_strerror(res);
            LOG_CORE_ERROR("OAuth callback for '{}': {}", connectionName, errMsg);
            return crow::response(500, errMsg);
        }

        if (httpCode != 200)
        {
            std::string errMsg = "Token exchange returned HTTP " + std::to_string(httpCode);
            if (!responseBody.empty())
            {
                // Always include the provider's error body — Microsoft/Google
                // put the actionable message (AADSTS code, invalid_client, etc.)
                // there.  Truncate to keep logs readable.
                constexpr size_t kMaxBodyInLog = 1500;
                if (responseBody.size() <= kMaxBodyInLog)
                {
                    errMsg += ": " + responseBody;
                }
                else
                {
                    errMsg += ": " + responseBody.substr(0, kMaxBodyInLog) + "…(truncated)";
                }
            }
            LOG_CORE_ERROR("OAuth callback for '{}': {}", connectionName, errMsg);
            return crow::response(500, errMsg);
        }

        // Parse the token response
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(responseBody);
        simdjson::ondemand::document doc;
        auto parseError = parser.iterate(paddedJson).get(doc);
        if (parseError)
        {
            LOG_CORE_ERROR("OAuth callback for '{}': failed to parse token response", connectionName);
            return crow::response(500, "Failed to parse token response");
        }

        std::string_view accessToken;
        if (doc["access_token"].get_string().get(accessToken))
        {
            LOG_CORE_ERROR("OAuth callback for '{}': token response missing access_token", connectionName);
            return crow::response(500, "Token response missing access_token");
        }

        std::string_view refreshToken;
        auto refreshErr = doc["refresh_token"].get_string().get(refreshToken);
        (void)refreshErr; // Optional field — ignore if absent

        int64_t expiresIn = 3600;
        int64_t parsedExpiry;
        if (!doc["expires_in"].get_int64().get(parsedExpiry))
        {
            expiresIn = parsedExpiry;
        }

        // Store tokens in OAuthTokenManager.  Pass client_secret through for confidential
        // clients so the background refresh loop can use it.
        std::string clientSecretForRefresh;
        if (providerInfo.m_RequiresClientSecret)
        {
            auto clientSecretIt = connection->m_Params.find("client_secret");
            if (clientSecretIt != connection->m_Params.end())
            {
                clientSecretForRefresh = clientSecretIt->second;
            }
        }
        auto& oauthManager = Core::g_Core->GetOAuthTokenManager();
        oauthManager.StoreTokens(connection->m_KeyName, std::string(accessToken), std::string(refreshToken),
                                 expiresIn, tokenUrl, clientId, clientSecretForRefresh);

        // Persist refresh_token + OAuth app config to the encrypted keys file so tokens
        // survive a restart.  The access_token itself is short-lived and is NOT persisted;
        // on startup the OAuthTokenManager hydrates from the refresh_token and fetches a
        // fresh access_token.
        {
            auto& keyManager = Core::g_Core->GetKeyManager();
            auto const* existing = keyManager.GetProvider(connection->m_KeyName);

            // Auto-create a placeholder provider if one doesn't already exist for this
            // connection's key_name. Without this, the OAuth callback would have to bail
            // out and tokens would remain in OAuthTokenManager memory only — lost on the
            // next j9t restart. Matches the natural UX where a user creates an OAuth
            // connection in the editor and immediately clicks "Authorize" without having
            // to manually create a same-named entry in the providers/keys view first.
            KeyManager::ProviderConfig updated = existing ? *existing : KeyManager::ProviderConfig{};
            if (!existing)
            {
                updated.m_DisplayName = connection->m_KeyName;
                LOG_CORE_INFO("OAuth callback: auto-creating KeyManager provider '{}' for connection '{}'",
                              connection->m_KeyName, connectionName);
            }

            updated.m_CredentialType = "oauth";
            updated.m_RefreshToken = std::string(refreshToken);
            updated.m_ExpiresAt = static_cast<int64_t>(std::time(nullptr)) + expiresIn;
            updated.m_Scopes = connection->m_Params.count("scopes")
                                   ? connection->m_Params.at("scopes")
                                   : providerInfo.m_DefaultScopes;
            updated.m_TokenEndpoint = tokenUrl;
            updated.m_ClientId = clientId;
            updated.m_ClientSecret = clientSecretForRefresh;

            if (existing)
            {
                keyManager.UpdateProvider(connection->m_KeyName, std::move(updated));
            }
            else
            {
                keyManager.AddProvider(connection->m_KeyName, std::move(updated));
            }

            // Persist OAuth tokens if the key store has been unlocked this session.
            // If the admin never unlocked (/api/settings/keys/unlock), the tokens stay
            // in memory only and the user gets a warning below. No env-var fallback.
            std::string cachedPassword = keyManager.GetCachedMasterPassword();

            auto const& keysPath = keyManager.GetKeysFilePath();
            if (!cachedPassword.empty() && !keysPath.empty())
            {
                if (keyManager.Save(keysPath, cachedPassword))
                {
                    LOG_SECURITY_INFO("[security] OAuth tokens persisted to encrypted keys file for '{}'",
                                      connection->m_KeyName);
                }
                else
                {
                    LOG_CORE_WARN("OAuth callback: failed to persist tokens for '{}' — refresh_token "
                                  "will be lost on restart",
                                  connection->m_KeyName);
                }
            }
            else
            {
                LOG_CORE_WARN("OAuth callback: no cached master password or keys file path — refresh_token "
                              "for '{}' held in memory only and will be lost on restart",
                              connection->m_KeyName);
            }
        }

        LOG_SECURITY_INFO("[security] OAuth tokens acquired for connection '{}' (key: '{}')", connectionName,
                          connection->m_KeyName);

        // Return a simple HTML page that closes itself (browser was redirected here)
        std::string html = "<!DOCTYPE html><html><body>"
                           "<h2>Authorization successful</h2>"
                           "<p>You can close this window and return to j9t Studio.</p>"
                           "<script>window.close();</script>"
                           "</body></html>";
        auto response = crow::response(200, html);
        response.set_header("Content-Type", "text/html");
        return response;
    }
#endif // J9T_STUDIO

    // ========================================================================
    // MCP key store lifecycle + auth endpoints (both editions)
    // ========================================================================

    bool WebServer::InitMcpKeyStore(std::string_view masterPassword)
    {
        if (m_McpKeysFilePath.empty())
        {
            auto const& config = Core::g_Core->GetConfig();
            m_McpKeysFilePath = Core::g_Core->GetLaunchCWDAbsolute() / config.m_McpKeysFilePath;
            m_WebSessionManager.SetTimeoutHours(config.m_SessionTimeoutHours);
        }

        bool loaded = false;
        if (std::filesystem::exists(m_McpKeysFilePath))
        {
            loaded = m_McpKeyManager.Load(m_McpKeysFilePath, masterPassword);
            if (!loaded)
            {
                LOG_CORE_ERROR("MCP key store present at '{}' but decryption failed — master password mismatch?",
                               m_McpKeysFilePath.string());
                m_McpKeysLoaded.store(false);
                return false;
            }
        }
        else
        {
            // No file yet — treat as empty-but-ready. Save() will create it on first change.
            loaded = m_McpKeyManager.Save(m_McpKeysFilePath, masterPassword);
            if (!loaded)
            {
                LOG_CORE_ERROR("Failed to create initial empty MCP key store at '{}'",
                               m_McpKeysFilePath.string());
                m_McpKeysLoaded.store(false);
                return false;
            }
            LOG_CORE_INFO("Created empty MCP key store at '{}'", m_McpKeysFilePath.string());
        }

        m_McpKeysLoaded.store(true);
        // The dashboard bootstrap flow (HandleKeysUnlockPost) handles first-run
        // admin provisioning by calling CreateBootstrapAdminKey on its own path —
        // we deliberately do not emit a log-banner enrollment token here because
        // the UI now surfaces the admin key directly in the response.
        return true;
    }

    bool WebServer::SaveMcpKeyStore()
    {
        if (!m_McpKeysLoaded.load())
        {
            LOG_CORE_WARN("SaveMcpKeyStore called before init — ignoring");
            return false;
        }
        auto& keyManager = Core::g_Core->GetKeyManager();
        std::string const pwd = keyManager.GetCachedMasterPassword();
        if (pwd.empty())
        {
            LOG_CORE_WARN("SaveMcpKeyStore: master password not cached — cannot persist");
            return false;
        }
        return m_McpKeyManager.Save(m_McpKeysFilePath, pwd);
    }


    // ---- Route handlers ---------------------------------------------------------

    crow::response WebServer::HandleMcpKeysListGet()
    {
        if (!m_McpKeysLoaded.load())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "mcp_keys_not_loaded";
            err["message"] = "MCP key store not unlocked. POST the master password to /api/settings/keys/unlock.";
            return MakeJsonResponse(503, err);
        }
        auto const records = m_McpKeyManager.ListKeys();
        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        crow::json::wvalue::list list;
        list.reserve(records.size());
        for (auto const& r : records)
        {
            crow::json::wvalue entry;
            entry["key_id"] = r.m_KeyId;
            entry["user"] = r.m_User;
            entry["role"] = r.m_Role;
            entry["adhoc_enabled"] = r.m_AdhocEnabled;
            entry["disk_quota_mb"] = r.m_DiskQuotaMb;
            entry["default_cleanup_policy"] = r.m_DefaultCleanupPolicy;
            entry["created_at"] = r.m_CreatedAt;
            entry["expires_at"] = r.m_ExpiresAt;
            entry["last_used_at"] = r.m_LastUsedAt;
            entry["enabled"] = r.m_Enabled;
            entry["description"] = r.m_Description;
            list.push_back(std::move(entry));
        }
        responseJson["keys"] = std::move(list);
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleMcpKeysEnrollPost(crow::request const& req)
    {
        if (!m_McpKeysLoaded.load())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "mcp_keys_not_loaded";
            err["message"] = "MCP key store not unlocked.";
            return MakeJsonResponse(503, err);
        }

        McpKeyManager::EnrollmentRequest enrollReq;
        enrollReq.m_Role = "operator";
        enrollReq.m_AdhocEnabled = false;
        enrollReq.m_DiskQuotaMb = 1024;
        enrollReq.m_DefaultCleanupPolicy = "ttl_72h";
        enrollReq.m_KeyExpiryDays = 90;
        enrollReq.m_EnrollmentTtlMinutes = 30;

        try
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(req.body);
            auto doc = parser.iterate(padded);
            std::string_view sv;
            if (doc["user"].get_string().get(sv) == simdjson::SUCCESS) enrollReq.m_User = std::string(sv);
            if (doc["role"].get_string().get(sv) == simdjson::SUCCESS) enrollReq.m_Role = std::string(sv);
            bool b = false;
            if (doc["adhoc_enabled"].get_bool().get(b) == simdjson::SUCCESS) enrollReq.m_AdhocEnabled = b;
            int64_t n = 0;
            if (doc["disk_quota_mb"].get_int64().get(n) == simdjson::SUCCESS) enrollReq.m_DiskQuotaMb = static_cast<int>(n);
            if (doc["default_cleanup_policy"].get_string().get(sv) == simdjson::SUCCESS)
                enrollReq.m_DefaultCleanupPolicy = std::string(sv);
            if (doc["description"].get_string().get(sv) == simdjson::SUCCESS) enrollReq.m_Description = std::string(sv);
            if (doc["key_expiry_days"].get_int64().get(n) == simdjson::SUCCESS)
                enrollReq.m_KeyExpiryDays = static_cast<int>(n);
            if (doc["enrollment_ttl_minutes"].get_int64().get(n) == simdjson::SUCCESS)
                enrollReq.m_EnrollmentTtlMinutes = static_cast<int>(n);
        }
        catch (...)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "malformed_body";
            return MakeJsonResponse(400, err);
        }

        if (enrollReq.m_User.empty() ||
            (enrollReq.m_Role != "admin" && enrollReq.m_Role != "operator" && enrollReq.m_Role != "viewer"))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_request";
            err["message"] = "Fields 'user' and 'role' (admin|operator|viewer) are required.";
            return MakeJsonResponse(400, err);
        }

        // Attribute the creator based on the authenticated caller.
        auto auth = Authenticate(req);
        enrollReq.m_CreatedBy = auth.m_User.empty() ? std::string("unknown") : auth.m_User;

        std::string const rawToken = m_McpKeyManager.CreateEnrollment(enrollReq);
        if (rawToken.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "enrollment_failed";
            return MakeJsonResponse(500, err);
        }
        SaveMcpKeyStore();

        LOG_SECURITY_INFO("[security] enrollment_created user={} role={} by={} adhoc={}",
                          enrollReq.m_User, enrollReq.m_Role, enrollReq.m_CreatedBy, enrollReq.m_AdhocEnabled);

        crow::json::wvalue body;
        body["ok"] = true;
        body["enrollment_token"] = rawToken;
        body["expires_in_minutes"] = enrollReq.m_EnrollmentTtlMinutes;
        body["user"] = enrollReq.m_User;
        body["role"] = enrollReq.m_Role;
        body["message"] = "Share this token with the user. You will not see their final API key.";
        return MakeJsonResponse(201, body);
    }

    crow::response WebServer::HandleMcpKeysActivatePost(crow::request const& req)
    {
        if (!m_McpKeysLoaded.load())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "mcp_keys_not_loaded";
            return MakeJsonResponse(503, err);
        }

        std::string rawToken;
        try
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(req.body);
            auto doc = parser.iterate(padded);
            std::string_view sv;
            if (doc["enrollment_token"].get_string().get(sv) == simdjson::SUCCESS)
                rawToken = std::string(sv);
        }
        catch (...) { /* malformed */ }

        if (rawToken.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "missing_token";
            err["message"] = "Request body must contain 'enrollment_token'.";
            return MakeJsonResponse(400, err);
        }

        auto result = m_McpKeyManager.ActivateEnrollment(rawToken);
        if (!result)
        {
            LOG_SECURITY_WARN("[security] activation_failed ip={}", req.remote_ip_address);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_or_expired";
            err["message"] = "Enrollment token is invalid or expired.";
            return MakeJsonResponse(401, err);
        }
        SaveMcpKeyStore();

        LOG_SECURITY_INFO("[security] activation_success user={} role={} key_id={}",
                          result->m_Record.m_User, result->m_Record.m_Role, result->m_KeyId);

        crow::json::wvalue body;
        body["ok"] = true;
        body["key_id"] = result->m_KeyId;
        body["api_key"] = result->m_RawKey;
        body["user"] = result->m_Record.m_User;
        body["role"] = result->m_Record.m_Role;
        body["expires_at"] = result->m_Record.m_ExpiresAt;
        body["message"] = "Save this key — it will not be shown again.";
        return MakeJsonResponse(200, body);
    }

    crow::response WebServer::HandleMcpKeysSelfRenewPost(crow::request const& req)
    {
        if (!m_McpKeysLoaded.load())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "mcp_keys_not_loaded";
            return MakeJsonResponse(503, err);
        }
        std::string const token = ExtractBearerToken(req);
        if (token.rfind("mcp_", 0) != 0)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "unauthorized";
            err["message"] = "Self-renew requires a valid MCP key in the Authorization header.";
            return MakeJsonResponse(401, err);
        }
        auto renew = m_McpKeyManager.SelfRenew(token);
        if (!renew)
        {
            LOG_SECURITY_WARN("[security] self_renew_failed ip={}", req.remote_ip_address);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_or_expired";
            err["message"] = "Your key is invalid or expired. Ask your admin for a new enrollment token.";
            return MakeJsonResponse(401, err);
        }
        SaveMcpKeyStore();

        LOG_SECURITY_INFO("[security] self_renew_success new_key_id={}", renew->m_KeyId);

        crow::json::wvalue body;
        body["ok"] = true;
        body["key_id"] = renew->m_KeyId;
        body["api_key"] = renew->m_RawKey;
        body["expires_at"] = renew->m_ExpiresAt;
        body["message"] = "New key activated. Old key remains valid for 24 hours. Update your config now.";
        // No expiry header on self-renew response — the old key is now irrelevant and the
        // new key is fresh (90 days); the client should switch to the new api_key immediately.
        return MakeJsonResponse(200, body);
    }

    crow::response WebServer::HandleMcpKeysUpdatePut(crow::request const& req, std::string const& keyId)
    {
        if (!m_McpKeysLoaded.load())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "mcp_keys_not_loaded";
            return MakeJsonResponse(503, err);
        }

        McpKeyManager::UpdateFields fields;
        try
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(req.body);
            auto doc = parser.iterate(padded);
            std::string_view sv;
            if (doc["role"].get_string().get(sv) == simdjson::SUCCESS) fields.m_Role = std::string(sv);
            bool b = false;
            if (doc["adhoc_enabled"].get_bool().get(b) == simdjson::SUCCESS) fields.m_AdhocEnabled = b;
            int64_t n = 0;
            if (doc["disk_quota_mb"].get_int64().get(n) == simdjson::SUCCESS)
                fields.m_DiskQuotaMb = static_cast<int>(n);
            if (doc["default_cleanup_policy"].get_string().get(sv) == simdjson::SUCCESS)
                fields.m_DefaultCleanupPolicy = std::string(sv);
            if (doc["enabled"].get_bool().get(b) == simdjson::SUCCESS) fields.m_Enabled = b;
            if (doc["description"].get_string().get(sv) == simdjson::SUCCESS)
                fields.m_Description = std::string(sv);
            if (doc["expires_at"].get_string().get(sv) == simdjson::SUCCESS)
                fields.m_ExpiresAt = std::string(sv);
        }
        catch (...)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "malformed_body";
            return MakeJsonResponse(400, err);
        }

        if (!m_McpKeyManager.UpdateKey(keyId, fields))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            return MakeJsonResponse(404, err);
        }
        SaveMcpKeyStore();
        LOG_SECURITY_INFO("[security] mcp_key_updated key_id={}", keyId);

        crow::json::wvalue body;
        body["ok"] = true;
        return MakeJsonResponse(200, body);
    }

    crow::response WebServer::HandleMcpKeysDelete(std::string const& keyId)
    {
        if (!m_McpKeysLoaded.load())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "mcp_keys_not_loaded";
            return MakeJsonResponse(503, err);
        }
        if (!m_McpKeyManager.RevokeKey(keyId))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            return MakeJsonResponse(404, err);
        }
        SaveMcpKeyStore();
        LOG_SECURITY_INFO("[security] mcp_key_revoked key_id={}", keyId);

        crow::json::wvalue body;
        body["ok"] = true;
        return MakeJsonResponse(200, body);
    }

    crow::response WebServer::HandleWhoamiGet(crow::request const& req)
    {
        auto auth = Authenticate(req);
        crow::json::wvalue body;
        body["ok"] = auth.m_Error.empty();
        body["user"] = auth.m_User;
        body["role"] = auth.m_Role;
        if (auth.m_DaysUntilExpiry >= 0)
        {
            body["days_until_expiry"] = auth.m_DaysUntilExpiry;
        }
        if (!auth.m_Error.empty())
        {
            body["error"] = auth.m_Error;
            return MakeJsonResponse(401, body);
        }
        auto resp = MakeJsonResponse(200, body);
        AttachMcpExpiryHeader(resp, req);
        return resp;
    }

    crow::response WebServer::HandleLoginPost(crow::request const& req)
    {
        // Login path: either gateway-injected identity, or MCP API key in body.
        auto const& config = Core::g_Core->GetConfig();
        if (!config.m_TrustedProxyHeader.empty())
        {
            std::string const& userHeader = req.get_header_value(config.m_TrustedProxyHeader);
            if (!userHeader.empty())
            {
                std::string role = "viewer";
                if (!config.m_TrustedRoleHeader.empty())
                {
                    std::string const& roleHeader = req.get_header_value(config.m_TrustedRoleHeader);
                    if (roleHeader == "admin" || roleHeader == "operator" || roleHeader == "viewer")
                        role = roleHeader;
                }
                auto session = m_WebSessionManager.Create(userHeader, role);
                crow::json::wvalue body;
                body["ok"] = true;
                body["user"] = userHeader;
                body["role"] = role;
                auto resp = MakeJsonResponse(200, body);
                std::string cookie = "session=" + session.m_SessionId + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" +
                                      std::to_string(config.m_SessionTimeoutHours * 3600);
                if (m_TlsEnabled) cookie += "; Secure";
                resp.add_header("Set-Cookie", cookie);
                LOG_SECURITY_INFO("[security] login_success user={} role={} method=gateway ip={}",
                                  userHeader, role, req.remote_ip_address);
                return resp;
            }
        }

        std::string apiKey;
        try
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(req.body);
            auto doc = parser.iterate(padded);
            std::string_view sv;
            if (doc["api_key"].get_string().get(sv) == simdjson::SUCCESS) apiKey = std::string(sv);
        }
        catch (...) { /* malformed */ }

        if (apiKey.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "missing_api_key";
            err["message"] = "Request body must contain 'api_key'.";
            return MakeJsonResponse(400, err);
        }

        auto result = m_McpKeyManager.Authenticate(apiKey);
        if (!result || !result->m_Record.m_Enabled || result->m_DaysUntilExpiry < 0)
        {
            LOG_SECURITY_WARN("[security] login_failed ip={}", req.remote_ip_address);
            RecordAuthFailure(req.remote_ip_address);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_key";
            err["message"] = "Invalid, disabled, or expired MCP API key.";
            return MakeJsonResponse(401, err);
        }

        auto session = m_WebSessionManager.Create(result->m_Record.m_User, result->m_Record.m_Role);
        LOG_SECURITY_INFO("[security] login_success user={} role={} method=mcp_key ip={}",
                          result->m_Record.m_User, result->m_Record.m_Role, req.remote_ip_address);

        crow::json::wvalue body;
        body["ok"] = true;
        body["user"] = result->m_Record.m_User;
        body["role"] = result->m_Record.m_Role;
        auto resp = MakeJsonResponse(200, body);
        std::string cookie = "session=" + session.m_SessionId + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" +
                              std::to_string(config.m_SessionTimeoutHours * 3600);
        if (m_TlsEnabled) cookie += "; Secure";
        resp.add_header("Set-Cookie", cookie);
        return resp;
    }

    std::optional<McpKeyManager::Record> WebServer::TryGetMcpRecord(crow::request const& req) const
    {
        std::string const token = ExtractBearerToken(req);
        if (token.rfind("mcp_", 0) != 0) return std::nullopt;
        auto result = m_McpKeyManager.Authenticate(token);
        if (!result) return std::nullopt;
        return result->m_Record;
    }

    crow::response WebServer::HandleAdhocRunPost(crow::request const& req)
    {
        if (!m_AdhocManager)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "adhoc_unavailable";
            err["message"] = "Adhoc manager not initialised (workflow registry not attached).";
            return MakeJsonResponse(503, err);
        }

        auto auth = Authenticate(req);
        if (!auth.Ok()) return MakeAuthErrorResponse(auth.m_Error);
        if (!HasRole(auth, "operator"))
        {
            LOG_SECURITY_WARN("[security] adhoc_denied reason=insufficient_role ip={} user={} role={}",
                              req.remote_ip_address, auth.m_User, auth.m_Role);
            return MakeAuthErrorResponse("insufficient_role");
        }

        auto mcpRecord = TryGetMcpRecord(req);
        if (!mcpRecord)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "mcp_key_required";
            err["message"] = "Adhoc submission requires an MCP API key (Bearer mcp_...).";
            return MakeJsonResponse(403, err);
        }
        if (!mcpRecord->m_AdhocEnabled)
        {
            LOG_SECURITY_WARN("[security] adhoc_denied reason=adhoc_not_enabled user={} key_id={}",
                              mcpRecord->m_User, mcpRecord->m_KeyId);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "adhoc_not_enabled";
            err["message"] = "This MCP key is not authorised for adhoc submission. Ask your admin to enable it.";
            return MakeJsonResponse(403, err);
        }

        // Parse body: { jcwf: {...}, context: {k:v}, cleanup_policy: "..." }.
        std::string jcwfJson;
        std::string cleanupPolicy;
        std::map<std::string, std::string> context;

        try
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(req.body);
            auto doc = parser.iterate(padded);

            // jcwf — serialise the object back to a string for staging.
            simdjson::ondemand::object jcwfObj;
            if (doc["jcwf"].get_object().get(jcwfObj) != simdjson::SUCCESS)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "missing_jcwf";
                err["message"] = "Request body must contain a 'jcwf' object (canvas JSON).";
                return MakeJsonResponse(400, err);
            }
            auto rawJson = jcwfObj.raw_json();
            if (rawJson.error() == simdjson::SUCCESS)
            {
                jcwfJson = std::string(rawJson.value());
            }

            // cleanup_policy — optional; default to the MCP key's configured policy.
            std::string_view sv;
            if (doc["cleanup_policy"].get_string().get(sv) == simdjson::SUCCESS)
            {
                cleanupPolicy = std::string(sv);
            }
            else
            {
                cleanupPolicy = mcpRecord->m_DefaultCleanupPolicy;
            }

            // context — optional map of string→string.
            simdjson::ondemand::object ctxObj;
            if (doc["context"].get_object().get(ctxObj) == simdjson::SUCCESS)
            {
                for (auto field : ctxObj)
                {
                    std::string_view key = field.unescaped_key();
                    std::string_view val;
                    if (field.value().get_string().get(val) == simdjson::SUCCESS)
                    {
                        context[std::string(key)] = std::string(val);
                    }
                }
            }
        }
        catch (...)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "malformed_body";
            return MakeJsonResponse(400, err);
        }

        if (jcwfJson.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "empty_jcwf";
            return MakeJsonResponse(400, err);
        }

        // Script-existence pre-check: external callers cannot submit scripts, so every
        // shell `params.command` and python `params.module` referenced by the JCWF must
        // already exist on disk under scripts/. This is the hard security boundary from
        // "Adhoc Workflow Submission and MCP plan.md" §2. Runs through the top-level
        // tasks object only — sub-workflow canvases inside an adhoc submission are not
        // supported today.
        std::vector<std::string> missingScripts;
        {
            fs::path const launchCWD = Core::g_Core ? Core::g_Core->GetLaunchCWDAbsolute() : fs::path{};
            try
            {
                simdjson::ondemand::parser scanParser;
                simdjson::padded_string const scanPadded(jcwfJson);
                auto scanDoc = scanParser.iterate(scanPadded);

                simdjson::ondemand::object tasksObj;
                if (scanDoc["tasks"].get_object().get(tasksObj) == simdjson::SUCCESS)
                {
                    for (auto field : tasksObj)
                    {
                        simdjson::ondemand::object task;
                        if (field.value().get_object().get(task) != simdjson::SUCCESS) continue;

                        std::string_view taskType;
                        if (task["type"].get_string().get(taskType) != simdjson::SUCCESS) continue;

                        simdjson::ondemand::object params;
                        if (task["params"].get_object().get(params) != simdjson::SUCCESS) continue;

                        if (taskType == "shell")
                        {
                            std::string_view cmd;
                            if (params["command"].get_string().get(cmd) != simdjson::SUCCESS) continue;
                            std::string cmdStr(cmd);
                            if (cmdStr.rfind("scripts/", 0) != 0) continue;
                            fs::path const normalized = fs::path(cmdStr).lexically_normal();
                            if (normalized.string().rfind("scripts/", 0) != 0)
                            {
                                missingScripts.emplace_back(cmdStr + " (escapes scripts/)");
                                continue;
                            }
                            fs::path const abs = (launchCWD / normalized).lexically_normal();
                            if (!fs::exists(abs))
                            {
                                missingScripts.emplace_back(cmdStr);
                            }
                        }
                        else if (taskType == "python")
                        {
                            std::string_view mod;
                            if (params["module"].get_string().get(mod) != simdjson::SUCCESS) continue;
                            std::string modStr(mod);
                            std::string modPath = modStr;
                            if (modPath.rfind("scripts.", 0) == 0) modPath = modPath.substr(std::string("scripts.").size());
                            std::replace(modPath.begin(), modPath.end(), '.', '/');
                            fs::path const base = launchCWD / "scripts" / modPath;
                            fs::path const asFile = fs::path(base.string() + ".py");
                            fs::path const asPackage = base / "__init__.py";
                            if (!fs::exists(asFile) && !fs::exists(asPackage))
                            {
                                missingScripts.emplace_back(modStr + " (expected scripts/" + modPath + ".py)");
                            }
                        }
                    }
                }
            }
            catch (...)
            {
                // Parsing failure here is not fatal — the JCWF parser downstream will
                // raise a more descriptive error when Stage() calls SaveOrUpdateWorkflowFromJson.
            }
        }
        if (!missingScripts.empty())
        {
            LOG_SECURITY_WARN("[security] adhoc_missing_scripts user={} count={}",
                              mcpRecord->m_User, missingScripts.size());
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "missing_scripts";
            err["message"] = "One or more scripts referenced by the JCWF do not exist under scripts/. "
                             "Adhoc submissions cannot ship scripts — they must be pre-deployed by an admin.";
            crow::json::wvalue::list missingList;
            for (auto const& entry : missingScripts) missingList.emplace_back(entry);
            err["missing"] = std::move(missingList);
            return MakeJsonResponse(400, err);
        }

        // Retention policies from shortest-lived to longest-lived. The order matters:
        // a submission may pick any policy at or below the user's configured ceiling.
        static constexpr std::array<std::string_view, 6> kPoliciesShortToLong = {
            "on_completion", "ttl_1h", "ttl_24h", "ttl_48h", "ttl_72h", "retain"};
        auto policyRank = [&](std::string const& p) -> int
        {
            for (size_t i = 0; i < kPoliciesShortToLong.size(); ++i)
            {
                if (p == kPoliciesShortToLong[i]) return static_cast<int>(i);
            }
            return -1;
        };
        int const submittedRank = policyRank(cleanupPolicy);
        if (submittedRank < 0)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_cleanup_policy";
            err["message"] = "cleanup_policy must be one of: on_completion, ttl_1h, ttl_24h, ttl_48h, ttl_72h, retain";
            return MakeJsonResponse(400, err);
        }
        int const ceilingRank = policyRank(mcpRecord->m_DefaultCleanupPolicy);
        if (ceilingRank >= 0 && submittedRank > ceilingRank)
        {
            LOG_SECURITY_WARN("[security] adhoc_policy_rejected user={} submitted={} ceiling={}",
                              mcpRecord->m_User, cleanupPolicy, mcpRecord->m_DefaultCleanupPolicy);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "policy_exceeds_ceiling";
            err["message"] = "Requested cleanup_policy exceeds this key's configured maximum (" +
                              mcpRecord->m_DefaultCleanupPolicy + "). Pick a shorter TTL or the ceiling itself.";
            err["ceiling"] = mcpRecord->m_DefaultCleanupPolicy;
            return MakeJsonResponse(403, err);
        }

        AdhocWorkflowManager::StageRequest stageReq;
        stageReq.m_JcwfJson = jcwfJson;
        stageReq.m_User = mcpRecord->m_User;
        stageReq.m_Role = mcpRecord->m_Role;
        stageReq.m_CleanupPolicy = cleanupPolicy;
        stageReq.m_DiskQuotaMb = mcpRecord->m_DiskQuotaMb;

        auto stageOut = m_AdhocManager->Stage(stageReq);
        if (std::holds_alternative<std::string>(stageOut))
        {
            std::string const errMsg = std::get<std::string>(stageOut);
            LOG_SECURITY_WARN("[security] adhoc_stage_failed user={} error={}", mcpRecord->m_User, errMsg);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = errMsg == "quota_exceeded" ? "quota_exceeded" : "stage_failed";
            err["message"] = errMsg;
            return MakeJsonResponse(errMsg == "quota_exceeded" ? 413 : 400, err);
        }

        auto result = std::get<AdhocWorkflowManager::StageResult>(stageOut);

        WorkflowRuntimeManager* runtime = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            runtime = m_WorkflowRuntimeManager;
        }
        if (runtime == nullptr)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "runtime_unavailable";
            return MakeJsonResponse(503, err);
        }

        // Convert parsed context to the runtime's ContextMap (string → ContextValue).
        ContextMap runtimeContext;
        for (auto const& [k, v] : context)
        {
            ContextValue cv;
            cv.m_Value = v;
            runtimeContext[k] = cv;
        }
        runtime->EnqueueWorkflowRunWithContextAndGetRunId(result.m_WorkflowId, result.m_RunId, runtimeContext);

        LOG_SECURITY_INFO("[security] adhoc_submitted user={} key_id={} runId={} workflowId={} policy={}",
                          mcpRecord->m_User, mcpRecord->m_KeyId, result.m_RunId, result.m_WorkflowId, cleanupPolicy);

        crow::json::wvalue body;
        body["ok"] = true;
        body["runId"] = result.m_RunId;
        body["workflowId"] = result.m_WorkflowId;
        body["cleanup_policy"] = cleanupPolicy;
        body["folder_path"] = result.m_FolderPath.string();
        auto resp = MakeJsonResponse(202, body);
        AttachMcpExpiryHeader(resp, req);
        return resp;
    }

    crow::response WebServer::HandleRunFilesListGet(crow::request const& req, std::string const& runId)
    {
        auto auth = Authenticate(req);
        if (!auth.Ok()) return MakeAuthErrorResponse(auth.m_Error);
        // Viewer is not permitted — artifact retrieval follows the same role floor
        // as the run-adhoc endpoint that produced the data. Operators can read their
        // own runs; admins can read any run.
        if (!HasRole(auth, "operator"))
        {
            LOG_SECURITY_WARN("[security] run_files_denied reason=insufficient_role ip={} user={} role={}",
                              req.remote_ip_address, auth.m_User, auth.m_Role);
            return MakeAuthErrorResponse("insufficient_role");
        }

        if (m_AdhocManager == nullptr)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "adhoc_unavailable";
            return MakeJsonResponse(503, err);
        }

        auto info = m_AdhocManager->GetRunInfo(runId);
        if (!info)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "run_not_found";
            err["message"] = "No run with that id is currently tracked. "
                             "Either it never existed or its folder has been reaped.";
            return MakeJsonResponse(404, err);
        }

        bool const isAdmin = auth.m_Role == "admin";
        std::string const callerSlug = AdhocWorkflowManager::SanitizeUserSlug(auth.m_User);
        if (!isAdmin && callerSlug != info->m_OwnerSlug)
        {
            LOG_SECURITY_WARN("[security] run_files_denied reason=not_owner caller={} owner={} runId={}",
                              auth.m_User, info->m_User, runId);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_owner";
            err["message"] = "This run belongs to another user.";
            return MakeJsonResponse(403, err);
        }
        if (isAdmin && callerSlug != info->m_OwnerSlug)
        {
            // Cross-user admin read — durable audit trail for compliance.
            LOG_SECURITY_INFO("[security] admin_cross_user_read kind=list caller={} owner={} runId={}",
                              auth.m_User, info->m_User, runId);
        }

        // Terminal? We don't hold a direct pointer to the runtime manager for this
        // check — the existing status endpoint already does, but for Phase 5 we
        // derive terminality from folder state. The presence of manifest.json is
        // the signal: OnRunCompleted writes it for non-`on_completion` runs. While
        // the run is active, the manifest isn't there yet.
        std::filesystem::path const manifestPath = info->m_FolderPath / "manifest.json";
        bool const terminal = std::filesystem::exists(manifestPath);

        // Retention — parse delete-at from the folder name; surface seconds_remaining.
        std::string deleteAtStr;
        int64_t secondsRemaining = -1;
        {
            std::string const folderName = info->m_FolderPath.filename().string();
            auto pos = folderName.rfind("_del-");
            if (pos != std::string::npos)
            {
                std::string const tail = folderName.substr(pos + std::string("_del-").size());
                if (tail == "retain")
                {
                    deleteAtStr = "retain";
                }
                else if (tail == "on_completion")
                {
                    deleteAtStr = "on_completion";
                    secondsRemaining = 0;
                }
                else
                {
                    // YYYYMMDDTHHMMSS → ISO pretty + seconds-remaining delta.
                    std::tm tm{};
                    std::istringstream iss(tail);
                    iss >> std::get_time(&tm, "%Y%m%dT%H%M%S");
                    if (!iss.fail())
                    {
#ifdef _WIN32
                        std::time_t t = _mkgmtime(&tm);
#else
                        std::time_t t = timegm(&tm);
#endif
                        if (t != static_cast<std::time_t>(-1))
                        {
                            auto deleteAt = std::chrono::system_clock::from_time_t(t);
                            auto now = std::chrono::system_clock::now();
                            auto delta = std::chrono::duration_cast<std::chrono::seconds>(deleteAt - now).count();
                            secondsRemaining = delta < 0 ? 0 : delta;
                            std::tm utc{};
#ifdef _WIN32
                            gmtime_s(&utc, &t);
#else
                            gmtime_r(&t, &utc);
#endif
                            std::ostringstream iso;
                            iso << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
                            deleteAtStr = iso.str();
                        }
                    }
                }
            }
        }

        // Optional prefix filter — lexically normalised to avoid client tricks.
        std::string prefix;
        if (auto const* p = req.url_params.get("prefix"); p != nullptr)
        {
            prefix = fs::path(std::string(p)).lexically_normal().generic_string();
            if (prefix.find("..") != std::string::npos)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "invalid_prefix";
                err["message"] = "prefix may not contain '..'";
                return MakeJsonResponse(400, err);
            }
        }

        // Extension → content-type (conservative defaults; anything unrecognised
        // falls through to application/octet-stream so clients never guess).
        auto const contentTypeFor = [](std::string const& ext) -> std::string {
            if (ext == ".json") return "application/json";
            if (ext == ".txt" || ext == ".log") return "text/plain; charset=utf-8";
            if (ext == ".csv") return "text/csv; charset=utf-8";
            if (ext == ".md") return "text/markdown; charset=utf-8";
            if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
            if (ext == ".xml") return "application/xml";
            if (ext == ".yaml" || ext == ".yml") return "application/yaml";
            if (ext == ".pdf") return "application/pdf";
            if (ext == ".png") return "image/png";
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".svg") return "image/svg+xml";
            if (ext == ".zip") return "application/zip";
            return "application/octet-stream";
        };

        auto const taskIdFor = [](std::string const& relPath) -> std::optional<std::string> {
            // Paths live under queue/<workflowId>/<taskId>/<file...> for task outputs.
            // Anything else (workflows/..., manifest.json, meta.json) has no task.
            constexpr std::string_view queuePrefix = "queue/";
            if (relPath.rfind(queuePrefix, 0) != 0) return std::nullopt;
            auto firstSlash = relPath.find('/', queuePrefix.size());
            if (firstSlash == std::string::npos) return std::nullopt;
            auto secondSlash = relPath.find('/', firstSlash + 1);
            if (secondSlash == std::string::npos) return std::nullopt;
            return relPath.substr(firstSlash + 1, secondSlash - firstSlash - 1);
        };

        // Walk the run folder live. Manifest-backed listing is a Phase 5 optimisation
        // that can come later; the live walk is always correct and fast enough for
        // typical adhoc folder sizes (low-hundreds of files).
        crow::json::wvalue::list filesJson;
        std::error_code ec;
        for (auto const& e : fs::recursive_directory_iterator(info->m_FolderPath, ec))
        {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            std::string const name = e.path().filename().string();
            // Skip bookkeeping files — meta.json and manifest.json aren't task outputs.
            if (name == "meta.json" || name == "manifest.json") continue;

            auto rel = fs::relative(e.path(), info->m_FolderPath, ec);
            if (ec) continue;
            std::string const relPath = rel.generic_string();

            if (!prefix.empty() && relPath.rfind(prefix, 0) != 0) continue;

            crow::json::wvalue entry;
            entry["path"] = relPath;
            auto size = e.file_size(ec);
            entry["size_bytes"] = ec ? 0 : static_cast<int64_t>(size);

            // mtime → ISO8601 UTC.
            auto ftime = fs::last_write_time(e.path(), ec);
            if (!ec)
            {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                std::time_t t = std::chrono::system_clock::to_time_t(sctp);
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

            auto taskId = taskIdFor(relPath);
            if (taskId) entry["task_id"] = *taskId;

            entry["content_type"] = contentTypeFor(e.path().extension().string());
            entry["local_path"] = e.path().string();
            entry["download_url"] = std::string("/api/workflow-runs/") + runId + "/files/" + relPath;

            filesJson.push_back(std::move(entry));
        }

        crow::json::wvalue body;
        body["ok"] = true;
        body["runId"] = runId;
        body["owner"] = info->m_User;
        body["owner_slug"] = info->m_OwnerSlug;
        body["terminal"] = terminal;

        crow::json::wvalue retention;
        retention["policy"] = info->m_CleanupPolicy;
        if (!deleteAtStr.empty()) retention["delete_at"] = deleteAtStr;
        if (secondsRemaining >= 0) retention["seconds_remaining"] = static_cast<int64_t>(secondsRemaining);
        body["retention"] = std::move(retention);

        body["files"] = std::move(filesJson);

        auto resp = MakeJsonResponse(200, body);
        AttachMcpExpiryHeader(resp, req);
        return resp;
    }

    crow::response WebServer::HandleRunFileGet(crow::request const& req,
                                               std::string const& runId,
                                               std::string const& relPath)
    {
        // Shared max — agents fetching terabyte files through one HTTP response
        // is an anti-pattern. Range requests remain available for larger files.
        constexpr uint64_t kMaxSingleResponseBytes = 10ull * 1024 * 1024;

        auto auth = Authenticate(req);
        if (!auth.Ok()) return MakeAuthErrorResponse(auth.m_Error);
        if (!HasRole(auth, "operator"))
        {
            LOG_SECURITY_WARN("[security] run_file_denied reason=insufficient_role ip={} user={} role={}",
                              req.remote_ip_address, auth.m_User, auth.m_Role);
            return MakeAuthErrorResponse("insufficient_role");
        }

        if (m_AdhocManager == nullptr)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "adhoc_unavailable";
            return MakeJsonResponse(503, err);
        }

        auto info = m_AdhocManager->GetRunInfo(runId);
        if (!info)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "run_not_found";
            err["message"] = "No run with that id is currently tracked.";
            return MakeJsonResponse(404, err);
        }

        bool const isAdmin = auth.m_Role == "admin";
        std::string const callerSlug = AdhocWorkflowManager::SanitizeUserSlug(auth.m_User);
        if (!isAdmin && callerSlug != info->m_OwnerSlug)
        {
            LOG_SECURITY_WARN("[security] run_file_denied reason=not_owner caller={} owner={} runId={} path={}",
                              auth.m_User, info->m_User, runId, relPath);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_owner";
            err["message"] = "This run belongs to another user.";
            return MakeJsonResponse(403, err);
        }
        if (isAdmin && callerSlug != info->m_OwnerSlug)
        {
            LOG_SECURITY_INFO("[security] admin_cross_user_read kind=file caller={} owner={} runId={} path={}",
                              auth.m_User, info->m_User, runId, relPath);
        }

        // --- Path safety ---
        // Reject absolute paths, '..' segments, and null bytes up front — cheaper
        // to bail before touching the filesystem. The lexical normalisation pass
        // catches URL-encoded traversal (%2E%2E) since Crow URL-decodes before
        // handing us the string.
        if (relPath.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "missing_path";
            return MakeJsonResponse(400, err);
        }
        if (relPath.find('\0') != std::string::npos)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid_path";
            return MakeJsonResponse(400, err);
        }
        fs::path const requested(relPath);
        if (requested.is_absolute())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "absolute_path_rejected";
            err["message"] = "path must be relative to the run folder";
            return MakeJsonResponse(400, err);
        }
        fs::path const normalized = requested.lexically_normal();
        {
            std::string const normStr = normalized.generic_string();
            if (normStr == ".." ||
                normStr.rfind("../", 0) == 0 ||
                normStr.find("/../") != std::string::npos ||
                (normStr.size() >= 3 && normStr.substr(normStr.size() - 3) == "/.."))
            {
                LOG_SECURITY_WARN("[security] run_file_path_escape user={} runId={} path={}",
                                  auth.m_User, runId, relPath);
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "path_escape";
                err["message"] = "Resolved path escapes the run folder.";
                return MakeJsonResponse(400, err);
            }
        }

        fs::path const absPath = (info->m_FolderPath / normalized).lexically_normal();

        // Belt-and-braces: confirm the absolute path starts with the run folder.
        {
            std::string const base = info->m_FolderPath.lexically_normal().generic_string();
            std::string const target = absPath.generic_string();
            if (target.rfind(base, 0) != 0)
            {
                LOG_SECURITY_WARN("[security] run_file_path_escape (prefix) user={} runId={} path={}",
                                  auth.m_User, runId, relPath);
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "path_escape";
                return MakeJsonResponse(400, err);
            }
        }

        // meta.json / manifest.json are bookkeeping — not task outputs.
        // Refusing to serve them matches the listing endpoint's filtering and
        // keeps the file endpoint from leaking internal attribution metadata.
        std::string const filename = absPath.filename().string();
        if (filename == "meta.json" || filename == "manifest.json")
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "reserved_file";
            err["message"] = "meta.json and manifest.json are internal bookkeeping files.";
            return MakeJsonResponse(403, err);
        }

        // Symlink check WITHOUT following — closes a TOCTOU class where a
        // malicious task could swap a regular file for a symlink pointing
        // outside the run folder between listing and download.
        std::error_code ec;
        auto symStatus = fs::symlink_status(absPath, ec);
        if (ec || !fs::exists(symStatus))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "file_not_found";
            err["message"] = "No file at that path in the run folder.";
            return MakeJsonResponse(404, err);
        }
        if (fs::is_symlink(symStatus))
        {
            LOG_SECURITY_WARN("[security] run_file_symlink_rejected user={} runId={} path={}",
                              auth.m_User, runId, relPath);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "symlink_rejected";
            err["message"] = "Symlinks are not served.";
            return MakeJsonResponse(400, err);
        }
        if (fs::is_directory(symStatus))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "is_directory";
            err["message"] = "Use GET /api/workflow-runs/<id>/files to list directory contents.";
            return MakeJsonResponse(400, err);
        }
        if (!fs::is_regular_file(symStatus))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_regular_file";
            return MakeJsonResponse(400, err);
        }

        uint64_t const fileSize = static_cast<uint64_t>(fs::file_size(absPath, ec));
        if (ec)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "stat_failed";
            return MakeJsonResponse(500, err);
        }

        // --- Content-type lookup (same table the listing endpoint uses) ---
        auto const contentTypeFor = [](std::string const& ext) -> std::string {
            if (ext == ".json") return "application/json";
            if (ext == ".txt" || ext == ".log") return "text/plain; charset=utf-8";
            if (ext == ".csv") return "text/csv; charset=utf-8";
            if (ext == ".md") return "text/markdown; charset=utf-8";
            if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
            if (ext == ".xml") return "application/xml";
            if (ext == ".yaml" || ext == ".yml") return "application/yaml";
            if (ext == ".pdf") return "application/pdf";
            if (ext == ".png") return "image/png";
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".svg") return "image/svg+xml";
            if (ext == ".zip") return "application/zip";
            return "application/octet-stream";
        };
        std::string const contentType = contentTypeFor(absPath.extension().string());

        // --- Range request parsing (if present) ---
        // Only `bytes=start-end` is supported (single range). Multipart byte-ranges
        // are not needed for the agent use case.
        bool isRange = false;
        uint64_t rangeStart = 0;
        uint64_t rangeEnd = 0; // inclusive
        {
            auto const* rangeHeader = req.get_header_value("Range").data();
            std::string rangeValue = rangeHeader ? std::string(rangeHeader) : std::string();
            if (!rangeValue.empty())
            {
                constexpr std::string_view kPrefix = "bytes=";
                if (rangeValue.rfind(kPrefix, 0) == 0)
                {
                    std::string const spec = rangeValue.substr(kPrefix.size());
                    auto dash = spec.find('-');
                    if (dash != std::string::npos)
                    {
                        std::string const startStr = spec.substr(0, dash);
                        std::string const endStr = spec.substr(dash + 1);
                        bool ok = true;
                        try
                        {
                            if (startStr.empty())
                            {
                                // "bytes=-N" → last N bytes.
                                uint64_t const suffix = std::stoull(endStr);
                                if (suffix == 0 || fileSize == 0) { ok = false; }
                                else
                                {
                                    rangeStart = suffix >= fileSize ? 0 : fileSize - suffix;
                                    rangeEnd = fileSize - 1;
                                }
                            }
                            else if (endStr.empty())
                            {
                                rangeStart = std::stoull(startStr);
                                rangeEnd = fileSize == 0 ? 0 : fileSize - 1;
                            }
                            else
                            {
                                rangeStart = std::stoull(startStr);
                                rangeEnd = std::stoull(endStr);
                            }
                        }
                        catch (...) { ok = false; }

                        if (!ok || rangeStart >= fileSize || rangeEnd < rangeStart)
                        {
                            crow::response resp(416);
                            resp.set_header("Content-Range",
                                            std::string("bytes */") + std::to_string(fileSize));
                            SetSecurityHeaders(resp);
                            return resp;
                        }
                        if (rangeEnd >= fileSize) rangeEnd = fileSize - 1;
                        isRange = true;
                    }
                }
            }
        }

        // --- Size cap enforcement ---
        // Full-file request above the cap → 413 with a suggested Range so the
        // agent can slice the download without guessing the correct header.
        if (!isRange && fileSize > kMaxSingleResponseBytes)
        {
            crow::response resp(413);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "file_too_large";
            err["message"] = "Use a Range request to fetch this file in slices.";
            err["size_bytes"] = static_cast<int64_t>(fileSize);
            err["max_single_response_bytes"] = static_cast<int64_t>(kMaxSingleResponseBytes);
            resp.body = crow::json::wvalue(err).dump();
            resp.set_header("Content-Type", "application/json; charset=utf-8");
            resp.set_header("X-Suggested-Range",
                            std::string("bytes=0-") +
                                std::to_string(kMaxSingleResponseBytes - 1));
            SetSecurityHeaders(resp);
            return resp;
        }

        uint64_t const toRead = isRange ? (rangeEnd - rangeStart + 1) : fileSize;
        if (toRead > kMaxSingleResponseBytes)
        {
            crow::response resp(413);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "range_too_large";
            err["message"] = "Requested range exceeds the single-response cap.";
            err["size_bytes"] = static_cast<int64_t>(fileSize);
            err["max_single_response_bytes"] = static_cast<int64_t>(kMaxSingleResponseBytes);
            resp.body = crow::json::wvalue(err).dump();
            resp.set_header("Content-Type", "application/json; charset=utf-8");
            SetSecurityHeaders(resp);
            return resp;
        }

        // --- Read bytes ---
        std::ifstream ifs(absPath, std::ios::binary);
        if (!ifs)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "read_failed";
            return MakeJsonResponse(500, err);
        }
        if (isRange && rangeStart > 0)
        {
            ifs.seekg(static_cast<std::streamoff>(rangeStart), std::ios::beg);
            if (!ifs)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "seek_failed";
                return MakeJsonResponse(500, err);
            }
        }
        std::string body;
        body.resize(static_cast<size_t>(toRead));
        if (toRead > 0)
        {
            ifs.read(body.data(), static_cast<std::streamsize>(toRead));
            auto const got = static_cast<uint64_t>(ifs.gcount());
            if (got != toRead)
            {
                body.resize(static_cast<size_t>(got));
            }
        }

        // --- SHA-256 (full file only — the hash covers the whole artifact, so
        //     partial responses omit it and rely on the listing endpoint for the
        //     canonical digest).
        std::string sha256Hex;
        if (!isRange && fileSize > 0)
        {
            unsigned char digest[SHA256_DIGEST_LENGTH];
            ::SHA256(reinterpret_cast<unsigned char const*>(body.data()), body.size(), digest);
            static constexpr char const* kHex = "0123456789abcdef";
            sha256Hex.reserve(SHA256_DIGEST_LENGTH * 2);
            for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i)
            {
                sha256Hex.push_back(kHex[(digest[i] >> 4) & 0xF]);
                sha256Hex.push_back(kHex[digest[i] & 0xF]);
            }
        }
        else if (!isRange && fileSize == 0)
        {
            // Canonical hash of the empty string.
            sha256Hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        }

        crow::response resp(isRange ? 206 : 200);
        resp.set_header("Content-Type", contentType);
        resp.set_header("Content-Length", std::to_string(body.size()));
        resp.set_header("Accept-Ranges", "bytes");
        resp.set_header("X-Run-Id", runId);
        resp.set_header("X-Run-Owner", info->m_User);
        if (!sha256Hex.empty())
        {
            resp.set_header("X-Content-SHA256", sha256Hex);
        }
        if (isRange)
        {
            resp.set_header("Content-Range",
                            std::string("bytes ") + std::to_string(rangeStart) + "-" +
                                std::to_string(rangeEnd) + "/" + std::to_string(fileSize));
        }

        // Retention echo — tells streaming clients how long their fetch URL will
        // stay valid without a second round-trip to the listing endpoint.
        {
            std::string const folderName = info->m_FolderPath.filename().string();
            auto pos = folderName.rfind("_del-");
            if (pos != std::string::npos)
            {
                std::string const tail = folderName.substr(pos + std::string("_del-").size());
                if (!tail.empty()) resp.set_header("X-Retention-Delete-At", tail);
            }
        }

        // Inline by default; ?download=1 forces attachment-style browsers.
        std::string const dispositionFilename = absPath.filename().string();
        bool const forceDownload = req.url_params.get("download") != nullptr;
        resp.set_header("Content-Disposition",
                        std::string(forceDownload ? "attachment" : "inline") +
                            "; filename=\"" + dispositionFilename + "\"");

        SetSecurityHeaders(resp);
        resp.body = std::move(body);
        AttachMcpExpiryHeader(resp, req);

        LOG_SECURITY_INFO("[security] run_file_read user={} runId={} path={} bytes={}{}",
                          auth.m_User, runId, relPath, resp.body.size(),
                          isRange ? " (range)" : "");
        return resp;
    }

    crow::response WebServer::HandleScriptsListGet(crow::request const& req)
    {
        auto auth = Authenticate(req);
        if (!auth.Ok()) return MakeAuthErrorResponse(auth.m_Error);
        // viewer is the floor — any authenticated caller can see what's available.

        // `?type=shell` or `?type=python` to narrow; `?refresh=1` to re-scan
        // (useful after an admin drops new scripts onto the host without a restart).
        std::string typeFilter;
        if (auto const* t = req.url_params.get("type"); t != nullptr)
        {
            std::string v(t);
            if (v == "shell" || v == "python") typeFilter = std::move(v);
        }
        if (req.url_params.get("refresh") != nullptr)
        {
            m_ScriptCatalog.Refresh(Core::g_Core->GetLaunchCWDAbsolute() / "scripts");
        }

        auto const entries = m_ScriptCatalog.List(typeFilter);

        crow::json::wvalue body;
        body["ok"] = true;
        body["count"] = static_cast<int64_t>(entries.size());

        crow::json::wvalue::list arr;
        arr.reserve(entries.size());
        for (auto const& e : entries)
        {
            crow::json::wvalue j;
            j["path"] = e.m_Path;
            j["type"] = e.m_Type;
            if (!e.m_Module.empty()) j["module"] = e.m_Module;
            j["short"] = e.m_Short;
            if (!e.m_Description.empty()) j["description"] = e.m_Description;
            if (!e.m_Outputs.empty()) j["outputs"] = e.m_Outputs;
            j["has_shebang"] = e.m_HasShebang;
            j["has_jarvis_marker"] = e.m_HasJarvisMarker;
            j["executable"] = e.m_Executable;

            crow::json::wvalue::list params;
            for (auto const& p : e.m_Params) params.emplace_back(p);
            j["params"] = std::move(params);

            arr.push_back(std::move(j));
        }
        body["scripts"] = std::move(arr);

        auto resp = MakeJsonResponse(200, body);
        AttachMcpExpiryHeader(resp, req);
        return resp;
    }

#ifdef DEBUG
    crow::response WebServer::HandleDebugSignalsGet()
    {
        // Live engine introspection. Only compiled in debug builds. Admin-gated at
        // the route level (see RegisterCommonRoutes). Extend freely with whatever
        // counter JC is investigating — keep it cheap, don't hold mutexes across
        // expensive work. See memory/reference_debug_signals.md for the convention.
        crow::json::wvalue body;
        body["ok"] = true;

        crow::json::wvalue signals;

        // ---- Uptime ----
        auto const uptime = std::chrono::steady_clock::now() - m_ProcessStart;
        signals["uptime_seconds"] =
            static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(uptime).count());

        // ---- WebSocket / broadcast state ----
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            signals["websocket_clients"] = static_cast<int64_t>(m_Clients.size());
            signals["websocket_total_connects"] = static_cast<int64_t>(m_WsTotalConnects);
            signals["websocket_total_disconnects"] = static_cast<int64_t>(m_WsTotalDisconnects);
            signals["websocket_peak_clients"] = static_cast<int64_t>(m_WsPeakClients);
            signals["websocket_peak_pending_broadcasts"] =
                static_cast<int64_t>(m_WsPeakPendingBroadcasts);
            signals["websocket_pending_broadcasts"] = static_cast<int64_t>(m_PendingBroadcasts.size());
        }

        // ---- Key store state ----
        {
            auto const& keyManager = Core::g_Core->GetKeyManager();
            signals["keys_unlocked"] = (keyManager.GetKeyLoadStatus() == KeyManager::KeyLoadStatus::Ok);
            signals["mcp_keys_loaded"] = m_McpKeysLoaded.load();
            signals["mcp_keys_count"] = static_cast<int64_t>(m_McpKeyManager.ListKeys().size());
        }

        // ---- Rate-limit + auth-failure buckets ----
        {
            std::lock_guard<std::mutex> lock(m_RateLimitMutex);
            signals["rate_limit_buckets_tracked"] = static_cast<int64_t>(m_RateLimitBuckets.size());
            signals["auth_failure_records"] = static_cast<int64_t>(m_AuthFailures.size());
        }

        // ---- Workflow runs ----
        size_t activeRuns = 0;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRuntimeManager != nullptr)
            {
                auto snapshot = m_WorkflowRuntimeManager->GetActiveRunsSnapshot();
                activeRuns = snapshot.size();
                size_t paused = 0;
                for (auto const& run : snapshot)
                {
                    if (run.m_State == WorkflowRunState::Paused) ++paused;
                }
                signals["workflow_runs_paused"] = static_cast<int64_t>(paused);

                uint64_t completed = 0;
                uint64_t failed = 0;
                m_WorkflowRuntimeManager->GetRunCounters(completed, failed);
                signals["workflow_runs_total_completed"] = static_cast<int64_t>(completed);
                signals["workflow_runs_total_failed"] = static_cast<int64_t>(failed);
            }
        }
        signals["workflow_runs_active"] = static_cast<int64_t>(activeRuns);

        // ---- AI dispatch state ----
        JarvisAgent* app = App::g_App;
        if (app != nullptr)
        {
            AiRequestPool const* pool = app->GetAiRequestPool();
            if (pool != nullptr)
            {
                signals["ai_calls_inflight"] = static_cast<int64_t>(pool->GetDirectDispatchInflight());
                // Lifetime-monotonic counters — confirm structured submissions,
                // schema retries/failures, chunking fan-out, and fence-strip are
                // firing in live runs without parsing transcripts or logs.
                signals["ai_structured_submissions"] =
                    static_cast<int64_t>(pool->GetStructuredSubmissions());
                signals["ai_schema_validation_retries"] =
                    static_cast<int64_t>(pool->GetSchemaValidationRetries());
                signals["ai_schema_validation_failures"] =
                    static_cast<int64_t>(pool->GetSchemaValidationFailures());
                signals["ai_chunked_dispatches"] = static_cast<int64_t>(pool->GetChunkedDispatches());
                signals["ai_fence_strips"] = static_cast<int64_t>(pool->GetFenceStrips());
            }
            else
            {
                signals["ai_calls_inflight"] = 0;
            }
        }

        // ---- Python engine pool ----
        if (app != nullptr)
        {
            PythonEnginePool* pyPool = app->GetPythonEnginePool();
            if (pyPool != nullptr)
            {
                size_t const engineCount = pyPool->GetEngineCount();
                signals["python_engines_total"] = static_cast<int64_t>(engineCount);
                crow::json::wvalue::list perEngineCompleted;
                perEngineCompleted.reserve(engineCount);
                for (size_t i = 0; i < engineCount; ++i)
                {
                    perEngineCompleted.push_back(
                        crow::json::wvalue(static_cast<int64_t>(pyPool->GetTasksCompleted(i))));
                }
                signals["python_tasks_completed"] = std::move(perEngineCompleted);
                // Queue depth per engine needs a per-engine accessor we don't expose yet;
                // extend PythonEnginePool with a GetQueueDepth(idx) helper when that
                // becomes the investigation target.
            }
        }

        // ---- Dashboard session store ----
        signals["dashboard_sessions_timeout_hours"] =
            static_cast<int64_t>(m_WebSessionManager.GetTimeoutHours());

        // ---- Adhoc manager ----
        if (m_AdhocManager)
        {
            signals["adhoc_runs_active"] = static_cast<int64_t>(m_AdhocManager->GetActiveRunCount());
            signals["adhoc_disk_usage_bytes"] =
                static_cast<int64_t>(m_AdhocManager->GetTotalDiskUsageBytes());
        }

        body["signals"] = std::move(signals);
        return MakeJsonResponse(200, body);
    }
#endif  // DEBUG

    crow::response WebServer::HandleLogoutPost(crow::request const& req)
    {
        std::string sessionId = ExtractSessionCookie(req);
        if (!sessionId.empty())
        {
            m_WebSessionManager.Destroy(sessionId);
        }
        LOG_SECURITY_INFO("[security] logout ip={}", req.remote_ip_address);
        crow::json::wvalue body;
        body["ok"] = true;
        auto resp = MakeJsonResponse(200, body);
        resp.add_header("Set-Cookie", "session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        return resp;
    }

} // namespace AIAssistant
