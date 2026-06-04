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
#include <thread>
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

#include "auxiliary/file.h"
#include "core.h"
#include "json/jsonHelper.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "python/pythonEnginePool.h"
#include "web/webServer.h"
#include "web/webServer_helpers.h"
#include "file/pathConfinement.h"
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
#include "keys/credential.h"
#include "keys/keyEncryption.h"
#include "cloud/cloudConnector.h"
#include "cloud/cloudConnectorRegistry.h"
#include "cloud/cloudConnectionManager.h"
#include "cloud/cloudCircuitBreaker.h"
#include "cloud/connectorHttp.h"
#include "cloud/oneDriveConnector.h"
#include "network/urlPolicy.h"
#include "keys/oauthTokenManager.h"
#include "curlWrapper/curlWrapper.h"
#include "curlWrapper/curlMultiDispatcher.h"
#include "curlWrapper/mockTransport.h"
#include "curlWrapper/rateLimitController.h"
#include "curlWrapper/rateLimitObservation.h"
#include "curlWrapper/rateLimitStrategy.h"
#include <curl/curl.h>
#include "workflow/triggerEngine.h"
#include "workflow/workflowTriggerBinder.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace fs = std::filesystem;
namespace AIAssistant

{
    using namespace WebServerHelpers;

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
        InitEditionSpecific();
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

        // If we're swapping to a different WRM, detach the old observer first.
        // The old observer captures `m_AdhocManager.get()` as a raw pointer;
        // leaving it installed on a WRM whose lifetime we don't control would
        // dangle as soon as `m_AdhocManager` is reset.
        if (m_WorkflowRuntimeManager && m_WorkflowRuntimeManager != workflowRuntimeManager)
        {
            m_WorkflowRuntimeManager->SetRunTerminalObserver({});
            LOG_APP_INFO("WebServer::SetWorkflowRuntimeManager: detached run-terminal observer "
                         "from previous WRM before swap");
        }

        m_WorkflowRuntimeManager = workflowRuntimeManager;
#ifdef J9T_STUDIO
        m_AssistantController.SetWorkflowRuntimeManager(workflowRuntimeManager);
#endif
        // Plumb terminal-state notifications through to the adhoc manager so
        // on_completion runs are cleaned up the moment they finish.  The lambda
        // captures `m_AdhocManager.get()` by raw pointer; the observer MUST be
        // cleared from this WRM before `m_AdhocManager` is destroyed.  Two
        // checkpoints guarantee that: (a) the swap-detach above when a new WRM
        // arrives, and (b) the explicit clear in SignalStop() before WebServer
        // teardown unwinds `m_AdhocManager`.
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

    // SanitizeUtf8 lives in workflow/workflowTypes.h (AIAssistant namespace).
    // Previously this file had its own anonymous-namespace copy; consolidated
    // so sanitization applies project-wide at the boundaries where external
    // bytes enter, not just at the WS layer.

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

        std::string const payload = json.dump();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_WsTotalRunsSnapshotsEnqueued;
        }
        BroadcastJSON(payload);
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

        std::string const payload = json.dump();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_WsTotalLastRunsSnapshotsEnqueued;
        }
        BroadcastJSON(payload);
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
        std::filesystem::path const distIndex = ResolveUiDistRoot("dashboard") / "index.html";
        // Try the read directly — fs::exists() followed by ServeStaticFile is
        // a TOCTOU window where the file can disappear between the two calls.
        // ServeStaticFile returns 404 on missing/unreadable; we substitute the
        // build-instruction message to keep the developer-facing UX.
        crow::response resp = ServeStaticFile(distIndex);
        if (resp.code == 404)
        {
            return crow::response(
                500,
                "Dashboard UI build not found. Please run: cd code/frontend/dashboard/ui && npm install && npm run build");
        }
        return resp;
    }

    crow::response WebServer::ServeDashboardStatic(std::string const& requestPath) const
    {
        std::filesystem::path const distRoot = ResolveUiDistRoot("dashboard");

        // Dashboard assets live under /dash-assets/...
        if (requestPath.rfind("/dash-assets/", 0) == 0)
        {
            std::string const relative = requestPath.substr(std::string("/dash-assets/").size());
            std::filesystem::path const resolved = ConfinePathUnder(distRoot, relative);
            if (resolved.empty())
            {
                LOG_SECURITY_WARN("[security] dashboard_static_path_escape len={}", relative.size());
                return crow::response(400, "Bad Request");
            }
            return ServeStaticFile(resolved);
        }

        // Fallback to dashboard index (SPA)
        return ServeDashboardIndex();
    }


    // =========================================================================
    // Admin authentication (Engine edition only)
    // =========================================================================

    // Failed auth lockout constants.
    static constexpr size_t kMaxAuthFailures = 10;
    static constexpr auto kAuthFailureWindow = std::chrono::minutes(5);
    [[maybe_unused]] static constexpr auto kLockoutDuration = std::chrono::minutes(15);

    // Two-tier rate-limit constants.
    //   * Pre-auth (per-IP): tight quota for unauthenticated/invalid traffic.
    //     Defends against credential-stuffing and anonymous flooding.
    //     Backed by the failed-auth lockout above.
    //   * Authenticated (per-user): generous quota once a credential validates.
    //     Sized to absorb dashboard polling, MCP heartbeats, and contract test
    //     bursts.  Audit logging makes runaway authenticated traffic
    //     investigable rather than blanket-blocked at the auth layer.
    static constexpr double kPreAuthBurst      = 20.0;
    static constexpr double kPreAuthRefillRate = 100.0 / 60.0;       // 100 req/min
    static constexpr double kAuthBurst         = 200.0;
    static constexpr double kAuthRefillRate    = 1200.0 / 60.0;      // 1200 req/min

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

    std::optional<WebServer::AuthResult> WebServer::TryMcpAuth(crow::request const& req)
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
            RecordAuthFailure(ip);
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

    void WebServer::AttachMcpExpiryHeader(crow::response& resp, crow::request const& req)
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

    std::optional<WebServer::AuthResult> WebServer::TrySessionAuth(crow::request const& req)
    {
        std::string sessionId = ExtractSessionCookie(req);
        if (sessionId.empty()) return std::nullopt;
        auto session = m_WebSessionManager.Validate(sessionId);
        if (!session) return std::nullopt; // stale/unknown cookie — fall through to other paths
        return AuthResult{"", session->m_User, session->m_Role};
    }

    WebServer::AuthResult WebServer::Authenticate(crow::request const& req)
    {
        // Unified auth funnel — identical across Studio and Engine.  Edition
        // controls *which routes are registered*, never *how requests are
        // authenticated*.  See doc/engine-studio-capability-review.md.
        //
        // Order:
        //   1. Lockout check                        (cheap, IP-level)
        //   2. Credential extraction + validation   (MCP token | session cookie)
        //      2a. Pre-auth rate limit (per-IP) on the failure paths
        //   3. Authenticated rate limit (per-user)
        //   4. Gateway cross-check                  (when TrustedProxyHeader is configured AND header present)
        //   5. Return AuthResult
        //
        // Splitting the rate limit either side of step 2 keeps legitimate
        // users on a generous per-user quota while still capping unauthenticated
        // probing on a tight per-IP quota.  Token validation itself is cheap
        // (HMAC compare on a 256-bit key) so doing it before throttling is
        // affordable; the lockout (step 1) handles persistent attackers.

        std::string const& ip = req.remote_ip_address;
        std::string const& endpoint = req.url;

        // ---- 1. Lockout check (locked IPs short-circuit before any rate-limit work) ----
        {
            std::lock_guard<std::mutex> lock(m_RateLimitMutex);
            auto it = m_AuthFailures.find(ip);
            if (it != m_AuthFailures.end())
            {
                auto const elapsed = std::chrono::steady_clock::now() - it->second.m_FirstFailure;
                if (it->second.m_Count >= kMaxAuthFailures && elapsed < kLockoutDuration)
                {
                    LOG_SECURITY_WARN("[security] locked_out ip={} endpoint={}", ip, endpoint);
                    return {"locked_out", "", ""};
                }
            }
        }

        // ---- 2. Credential extraction + validation ----
        // Exactly one of MCP token / session cookie must be present and valid.
        // No anonymous path.  No "gateway header alone" path — gateway is a
        // cross-check on top of a primary credential (step 4 below).
        AuthResult auth;
        if (auto mcp = TryMcpAuth(req); mcp.has_value())
        {
            // TryMcpAuth populates an error code (invalid_token / key_disabled /
            // token_expired) when the bearer header started with "mcp_" but did
            // not validate.  Surface that immediately, but rate-limit the
            // pre-auth path so a flood of invalid keys is cheap to reject.
            if (!mcp->m_Error.empty())
            {
                if (IsRateLimited(RateLimitTier::PreAuth, ip))
                {
                    LOG_SECURITY_WARN("[security] rate_limited_preauth ip={} endpoint={}", ip, endpoint);
                    return {"rate_limited", "", ""};
                }
                return *mcp;
            }
            auth = *mcp;
        }
        else if (auto session = TrySessionAuth(req); session.has_value())
        {
            auth = *session;
        }
        else
        {
            // No valid credential — pre-auth rate limit applies before logging
            // the rejection so a flood of empty/garbage requests is throttled.
            if (IsRateLimited(RateLimitTier::PreAuth, ip))
            {
                LOG_SECURITY_WARN("[security] rate_limited_preauth ip={} endpoint={}", ip, endpoint);
                return {"rate_limited", "", ""};
            }
            std::string const& authHeader = req.get_header_value("Authorization");
            if (authHeader.empty())
            {
                LOG_SECURITY_WARN("[security] auth_failure reason=missing_credential ip={} endpoint={}",
                                  ip, endpoint);
                return {"missing", "", ""};
            }
            LOG_SECURITY_WARN("[security] auth_failure reason=unrecognised_credential ip={} endpoint={}",
                              ip, endpoint);
            return {"forbidden", "", ""};
        }

        // ---- 3. Authenticated rate limit (per-user) ----
        if (IsRateLimited(RateLimitTier::Authenticated, auth.m_User))
        {
            LOG_SECURITY_WARN("[security] rate_limited_authenticated user={} ip={} endpoint={}",
                              auth.m_User, ip, endpoint);
            return {"rate_limited", "", ""};
        }

        // ---- 4. Gateway cross-check (opt-in via TrustedProxyHeader) ----
        // When configured AND the gateway has injected its identity header,
        // verify the gateway-asserted user matches the credential's user, and
        // cap the role downward if the gateway asserts a lower role.
        // Gateway can downgrade, never escalate.
        auto const& config = Core::g_Core->GetConfig();
        if (!config.m_TrustedProxyHeader.empty())
        {
            std::string const& gatewayUser = req.get_header_value(config.m_TrustedProxyHeader);
            if (!gatewayUser.empty())
            {
                if (gatewayUser != auth.m_User)
                {
                    LOG_SECURITY_WARN("[security] forbidden reason=identity_mismatch ip={} "
                                      "credential_user={} gateway_user={} endpoint={}",
                                      ip, auth.m_User, gatewayUser, endpoint);
                    return {"identity_mismatch", "", ""};
                }
                if (!config.m_TrustedRoleHeader.empty())
                {
                    std::string const& gatewayRole = req.get_header_value(config.m_TrustedRoleHeader);
                    if (gatewayRole == "admin" || gatewayRole == "operator" || gatewayRole == "viewer")
                    {
                        if (RoleLevel(gatewayRole) < RoleLevel(auth.m_Role))
                        {
                            LOG_SECURITY_INFO("[security] role_downgrade user={} from={} to={} endpoint={}",
                                              auth.m_User, auth.m_Role, gatewayRole, endpoint);
                            auth.m_Role = gatewayRole;
                        }
                    }
                }
            }
        }

        return auth;
    }

    // Legacy wrapper — used by existing route lambdas that require admin.
    // Returns empty string if the request is authenticated with admin-equivalent privileges,
    // or an error code ("forbidden", "missing", ...) otherwise.
    std::string WebServer::CheckAdminAuth(crow::request const& req)
    {
        return CheckAuth(req, "admin");
    }

    std::string WebServer::CheckAuth(crow::request const& req, std::string_view minRole)
    {
        AuthResult unused;
        return CheckAuth(req, minRole, unused);
    }

    std::string WebServer::CheckAuth(crow::request const& req, std::string_view minRole,
                                     AuthResult& outAuth)
    {
        outAuth = Authenticate(req);
        if (!outAuth.m_Error.empty()) return outAuth.m_Error;
        if (!HasRole(outAuth, minRole))
        {
            // Authenticated but lacks the required role.  Return the typed
            // "insufficient_role" code so MakeAuthErrorResponse routes to the
            // matching response-body branch ("Your role does not have
            // permission for this endpoint.") instead of the generic
            // fall-through "Invalid API token." which is misleading when the
            // token is in fact valid.
            LOG_SECURITY_WARN("[security] forbidden reason=insufficient_role ip={} user={} role={} "
                              "required={} endpoint={}",
                              req.remote_ip_address, outAuth.m_User, outAuth.m_Role, minRole, req.url);
            return "insufficient_role";
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

    bool WebServer::IsRateLimited(RateLimitTier tier, std::string const& key)
    {
        auto const now = std::chrono::steady_clock::now();

        double const burst       = (tier == RateLimitTier::PreAuth) ? kPreAuthBurst       : kAuthBurst;
        double const refillRate  = (tier == RateLimitTier::PreAuth) ? kPreAuthRefillRate  : kAuthRefillRate;
        auto& buckets            = (tier == RateLimitTier::PreAuth) ? m_PreAuthBuckets    : m_AuthenticatedBuckets;

        std::lock_guard<std::mutex> lock(m_RateLimitMutex);

        // Periodic cleanup: evict idle buckets in both tiers, plus expired
        // lockout records.  Cheap to do here since we already hold the mutex.
        auto const sinceCleanup = std::chrono::duration_cast<std::chrono::minutes>(now - m_LastRateLimitCleanup);
        if (sinceCleanup.count() >= 5)
        {
            auto evictStale = [&now](auto& m)
            {
                for (auto it = m.begin(); it != m.end();)
                {
                    auto const age = std::chrono::duration_cast<std::chrono::minutes>(now - it->second.m_LastRefill);
                    if (age.count() >= 10) it = m.erase(it);
                    else ++it;
                }
            };
            evictStale(m_PreAuthBuckets);
            evictStale(m_AuthenticatedBuckets);
            for (auto it = m_AuthFailures.begin(); it != m_AuthFailures.end();)
            {
                auto const age = std::chrono::duration_cast<std::chrono::minutes>(now - it->second.m_FirstFailure);
                if (age > kLockoutDuration) it = m_AuthFailures.erase(it);
                else ++it;
            }
            m_LastRateLimitCleanup = now;
        }

        auto& bucket = buckets[key];
        // First-touch buckets are seeded at full burst — clients pay no
        // warm-up tax for arriving on a steady-clock zero default.
        if (bucket.m_LastRefill.time_since_epoch().count() == 0)
        {
            bucket.m_Tokens = burst;
            bucket.m_LastRefill = now;
        }
        else
        {
            double const elapsed = std::chrono::duration<double>(now - bucket.m_LastRefill).count();
            bucket.m_Tokens = std::min(burst, bucket.m_Tokens + elapsed * refillRate);
            bucket.m_LastRefill = now;
        }

        if (bucket.m_Tokens >= 1.0)
        {
            bucket.m_Tokens -= 1.0;
            return false;
        }

        return true;
    }

    void WebServer::RegisterRoutes()
    {
        RegisterCommonRoutes();
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

        // ---- POST /api/mcp/heartbeat — MCP sidecar liveness (admin-key auth) ----
        // Previously unauthenticated; an attacker on the network could spoof
        // MCP-connected status indefinitely.  Now requires a valid MCP key
        // (TryMcpAuth) and applies the pre-auth rate limiter as defense in depth.
        CROW_ROUTE(m_Server, "/api/mcp/heartbeat")
            .methods("POST"_method)([this](crow::request const& req) { return HandleMcpHeartbeatPost(req); });

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

        // Hermetic-test entry point for the rate-limit strategy parsers.  Lets
        // a Python test feed canned header buffers per provider through
        // IRateLimitStrategy::Parse() without a live HTTP round-trip.  See
        // test/dispatch/test_rate_limit_observation_parse.py for the canonical
        // caller.
        CROW_ROUTE(m_Server, "/api/debug/parse-rate-limit-headers")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleParseRateLimitHeadersPost(req);
                });

        // Completion-callback payload renderer — exposes the body that
        // FireCompletionCallback would POST to a configured callbackUrl,
        // without actually firing the HTTP request.  Lets
        // test_negative_paths.py exercise the 64 KiB per-output cap +
        // UTF-8-safe truncation without standing up an HTTPS receiver
        // (the SSRF gate rejects loopback, so end-to-end testing the
        // payload-rendering path otherwise requires a tunneled public
        // HTTPS receiver).  Query params: runId (required), include_outputs
        // (optional, default true; accepts false/0/no/False/FALSE).
        CROW_ROUTE(m_Server, "/api/debug/build-callback-payload")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleDebugBuildCallbackPayloadGet(req);
                });

        // Size-aware-budget readback — surfaces the dispatcher's bounded
        // ring of recent submissions (QueryData::m_TimeoutMs etc.) so
        // test_size_aware_budget.py can assert the timeout-budget formula
        // without scraping logs.
        CROW_ROUTE(m_Server, "/api/debug/recent-submissions")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleDebugRecentSubmissionsGet();
                });

        // Observe-idempotent contract test entry.  Test feeds a sequence of
        // observations through an ephemeral RateLimitController and reads
        // back the merged state, asserting that headers-only + body-only
        // observations produce the same state as a single combined
        // observation (idempotence-by-replacement).
        CROW_ROUTE(m_Server, "/api/debug/test-observe-idempotent")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleDebugTestObserveIdempotentPost(req);
                });

        // Mock-AI-response endpoint for hermetic dispatcher tests.  Auth-free
        // on purpose — the dispatcher hits this with provider auth
        // (Bearer/x-api-key/etc.), not an MCP key.  Compiled out of release
        // builds with the rest of the #ifdef DEBUG block.
        CROW_ROUTE(m_Server, "/api/debug/mock-ai-response")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    return HandleDebugMockAiResponsePost(req);
                });

        // Hermetic-dispatcher-test isolation reset.  Phase B tests call this
        // at setup so each run starts from a clean dispatcher state.
        CROW_ROUTE(m_Server, "/api/debug/reset-dispatcher-state")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleDebugResetDispatcherStatePost();
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
                    AuthResult auth;
                    if (auto err = CheckAuth(req, "operator", auth); !err.empty())
                        return MakeAuthErrorResponse(err);
                    LOG_SECURITY_INFO("[security] run_cancel ip={} user={} runId={}", req.remote_ip_address, auth.m_User,
                                      runId);
                    return HandleWorkflowRunCancelPost(runId);
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/pause")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    AuthResult auth;
                    if (auto err = CheckAuth(req, "operator", auth); !err.empty())
                        return MakeAuthErrorResponse(err);
                    LOG_SECURITY_INFO("[security] run_pause ip={} user={} runId={}", req.remote_ip_address, auth.m_User,
                                      runId);
                    return HandleWorkflowRunPausePost(runId);
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/resume")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    AuthResult auth;
                    if (auto err = CheckAuth(req, "operator", auth); !err.empty())
                        return MakeAuthErrorResponse(err);
                    LOG_SECURITY_INFO("[security] run_resume ip={} user={} runId={}", req.remote_ip_address, auth.m_User,
                                      runId);
                    return HandleWorkflowRunResumePost(runId);
                });

        CROW_ROUTE(m_Server, "/api/workflow-runs/<string>/stop")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& runId)
                {
                    AuthResult auth;
                    if (auto err = CheckAuth(req, "operator", auth); !err.empty())
                        return MakeAuthErrorResponse(err);
                    LOG_SECURITY_INFO("[security] run_stop ip={} user={} runId={}", req.remote_ip_address, auth.m_User,
                                      runId);
                    return HandleWorkflowRunStopPost(runId);
                });

        // ---- Admin: Log viewer ----
        CROW_ROUTE(m_Server, "/api/log")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    if (auto err = CheckAuth(req, "operator"); !err.empty())
                        return MakeAuthErrorResponse(err);
                    return HandleLogGet(req);
                });

        // ---- Admin: Security log (admin only) ----
        CROW_ROUTE(m_Server, "/api/log/security")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    if (auto err = CheckAuth(req, "admin"); !err.empty())
                        return MakeAuthErrorResponse(err);
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

                    JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
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

        // ---- Webhook (HMAC auth handled inside the handler — both editions) ----
        CROW_ROUTE(m_Server, "/api/webhook/<string>")
            .methods("POST"_method)([this](crow::request const& req, std::string const& workflowId)
                                    { return HandleWebhookPost(req, workflowId); });

        // ---- Admin: n8n integration ----
        CROW_ROUTE(m_Server, "/api/integrations/n8n/start")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleN8nStartPost(req);
                });

        // ---- Viewer+: sub-workflow tree + dependency graph (read-only registry queries) ----
        CROW_ROUTE(m_Server, "/api/workflows/dependency-graph")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty()) return MakeAuthErrorResponse(err);

                    WorkflowRegistry const* registry = nullptr;
                    {
                        std::scoped_lock<std::mutex> const lock(m_Mutex);
                        registry = m_WorkflowRegistry;
                    }
                    if (registry == nullptr)
                    {
                        return crow::response(503, "application/json",
                                              R"({"ok":false,"error":"registry_unavailable"})");
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

        CROW_ROUTE(m_Server, "/api/workflows/<string>/tree")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAuth(req, "viewer");
                    if (!err.empty()) return MakeAuthErrorResponse(err);

                    WorkflowRegistry const* registry = nullptr;
                    {
                        std::scoped_lock<std::mutex> const lock(m_Mutex);
                        registry = m_WorkflowRegistry;
                    }
                    if (registry == nullptr)
                    {
                        return crow::response(503, "application/json",
                                              R"({"ok":false,"error":"registry_unavailable"})");
                    }

                    auto const workflowOpt = registry->GetWorkflow(workflowId);
                    if (!workflowOpt.has_value())
                    {
                        return crow::response(404, "application/json", R"({"ok":false,"error":"not_found"})");
                    }

                    auto const allIds = registry->GetWorkflowIds();
                    std::string const prefix = workflowId + "__";
                    crow::json::wvalue childrenArray(crow::json::wvalue::list{});
                    size_t idx = 0;
                    for (auto const& id : allIds)
                    {
                        auto const childOpt = registry->GetWorkflow(id);
                        if (!childOpt.has_value() || !childOpt->m_IsSubWorkflow) continue;
                        if (childOpt->m_ParentWorkflowId != workflowId && id.rfind(prefix, 0) != 0) continue;

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

        // ---- Operator+: pre-registered workflow run + clean ----
        CROW_ROUTE(m_Server, "/api/workflows/<string>/run")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAuth(req, "operator");
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowRunPost(req, workflowId);
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/clean")
            .methods("DELETE"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAuth(req, "operator");
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowCleanDelete(workflowId);
                });

        // ---- Operator+: log analyze-last-run ----
        CROW_ROUTE(m_Server, "/api/log/analyze-last-run")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAuth(req, "operator");
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleLogAnalyzeLastRunGet(req);
                });

        // ---- Admin: workflow registry refresh + versioning ----
        CROW_ROUTE(m_Server, "/api/workflows/reload")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowsReloadPost();
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& workflowId)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowVersionsListGet(workflowId);
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions/<string>")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& workflowId, std::string const& timestamp)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowVersionGetGet(workflowId, timestamp);
                });

        CROW_ROUTE(m_Server, "/api/workflows/<string>/versions/<string>/restore")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& workflowId, std::string const& timestamp)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleWorkflowVersionRestorePost(workflowId, timestamp);
                });

        // ---- Admin: AI interface CRUD ----
        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleAiInterfacesListGet();
                });

        // Sitting-8 Workstream D: per-interface health snapshot for the AI
        // Health LED.  Read-only join of config interfaces × AiRequestPool's
        // last-error tracking × dispatcher AIMD cap state.  Available in all
        // 4 build targets (no debug-only gating).  Dashboard fetches on mount
        // + on WS reconnect to hydrate the LED + popover before the first
        // `cap-changed` broadcast arrives.
        CROW_ROUTE(m_Server, "/api/providers/health")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleProvidersHealthGet();
                });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleAiInterfaceCreatePost(req);
                });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/<string>")
            .methods("PUT"_method)(
                [this](crow::request const& req, std::string const& name)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleAiInterfaceUpdatePut(req, name);
                });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/<string>")
            .methods("DELETE"_method)(
                [this](crow::request const& req, std::string const& name)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleAiInterfaceDeleteDelete(name);
                });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/save")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleAiInterfacesSavePost();
                });

        CROW_ROUTE(m_Server, "/api/settings/ai-interfaces/test")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleAiInterfaceTestPost(req);
                });

        // ---- Admin: config edit ----
        CROW_ROUTE(m_Server, "/api/settings/config")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConfigSettingsGet();
                });

        CROW_ROUTE(m_Server, "/api/settings/config")
            .methods("PUT"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConfigSettingsPut(req);
                });

        CROW_ROUTE(m_Server, "/api/settings/config/reload")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConfigReloadPost();
                });

        // ---- Admin: AI provider CRUD ----
        CROW_ROUTE(m_Server, "/api/settings/providers")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleProvidersListGet();
                });

        CROW_ROUTE(m_Server, "/api/settings/providers")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleProviderCreatePost(req);
                });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>")
            .methods("PUT"_method)(
                [this](crow::request const& req, std::string const& providerName)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleProviderUpdatePut(req, providerName);
                });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>")
            .methods("DELETE"_method)(
                [this](crow::request const& req, std::string const& providerName)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleProviderDelete(providerName);
                });

        CROW_ROUTE(m_Server, "/api/settings/providers/<string>/default")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& providerName)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleProviderSetDefaultPost(providerName);
                });

        CROW_ROUTE(m_Server, "/api/settings/providers/save")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleProvidersSavePost(req);
                });

        // ---- Admin: cloud connections ----
        CROW_ROUTE(m_Server, "/api/connections")
            .methods("GET"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConnectionsListGet();
                });

        CROW_ROUTE(m_Server, "/api/connections")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConnectionCreatePost(req);
                });

        CROW_ROUTE(m_Server, "/api/connections/<string>")
            .methods("PUT"_method)(
                [this](crow::request const& req, std::string const& connectionName)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConnectionUpdatePut(req, connectionName);
                });

        CROW_ROUTE(m_Server, "/api/connections/<string>")
            .methods("DELETE"_method)(
                [this](crow::request const& req, std::string const& connectionName)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConnectionDelete(connectionName);
                });

        CROW_ROUTE(m_Server, "/api/connections/<string>/test")
            .methods("POST"_method)(
                [this](crow::request const& req, std::string const& connectionName)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConnectionTestPost(connectionName);
                });

        CROW_ROUTE(m_Server, "/api/connections/save")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleConnectionsSavePost();
                });

        CROW_ROUTE(m_Server, "/api/connections/<string>/oauth/authorize")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& connectionName)
                {
                    auto err = CheckAdminAuth(req);
                    if (!err.empty()) return MakeAuthErrorResponse(err);
                    return HandleOAuthAuthorizeGet(connectionName);
                });

        // OAuth2 redirect endpoint.  Intentionally unauthenticated — the user-agent
        // redirect from Google / Microsoft cannot carry the j9t admin Bearer.  The
        // CSRF gate is the `state` query parameter, which is a single-use random
        // nonce stored server-side at `/oauth/authorize` time and verified inside
        // HandleOAuthCallbackGet (see line ~6685).  Per RFC 6749 §10.12, `state`
        // IS the security mechanism for an OAuth callback — adding Bearer auth on
        // top breaks the legitimate flow without adding security.
        CROW_ROUTE(m_Server, "/api/connections/<string>/oauth/callback")
            .methods("GET"_method)(
                [this](crow::request const& req, std::string const& connectionName)
                {
                    return HandleOAuthCallbackGet(req, connectionName);
                });

        // ---- Admin: POST /api/shutdown (admin only) ----
        CROW_ROUTE(m_Server, "/api/shutdown")
            .methods("POST"_method)(
                [this](crow::request const& req)
                {
                    AuthResult auth;
                    if (auto err = CheckAuth(req, "admin", auth); !err.empty())
                        return MakeAuthErrorResponse(err);

                    LOG_SECURITY_INFO("[security] shutdown_requested ip={} user={}", req.remote_ip_address, auth.m_User);
                    Core::g_Core->RequestQuit();
                    auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                    Core::g_Core->PushEvent(event, ProducerId::WebServer);

                    crow::json::wvalue response;
                    response["message"] = "Shutdown initiated.";
                    return crow::response(200, response);
                });
    }


    crow::response WebServer::HandleMcpHeartbeatPost(crow::request const& req)
    {
        // Pre-auth rate limit on the source IP — first line of defense against
        // unauthenticated floods that try to keep the heartbeat fresh.
        if (IsRateLimited(RateLimitTier::PreAuth, req.remote_ip_address))
        {
            LOG_SECURITY_WARN("[security] rate_limited_preauth ip={} endpoint=mcp_heartbeat",
                              req.remote_ip_address);
            return MakeAuthErrorResponse("rate_limited");
        }

        // Body cap before any allocation-heavy work.  A heartbeat carries no
        // body content; 1 KB is generous slack for future fields.
        if (IsBodyTooLarge(req, 1))
        {
            return MakePayloadTooLargeResponse(1);
        }

        // Short-circuit the locked-keystore case BEFORE attempting MCP auth.  When
        // the keystore hasn't been unlocked yet (normal post-restart state), the
        // MCP-key cache is empty by construction, so every heartbeat attempt would
        // fail and increment the per-IP lockout counter — locking the trusted
        // bridge out within ~150 s of startup for 15 minutes and blocking REST
        // shutdown along with it.  Returning 503 Service Unavailable here is the
        // right shape: tells the bridge "system is starting, retry later" without
        // consuming adversarial-flood budget.  (423 Locked would be semantically
        // closer but Crow's response constructor does not recognise it and
        // rewrites it to 500.)  No RecordAuthFailure call — the heartbeat never
        // actually attempted auth, so it's not an auth failure.
        if (!m_McpKeysLoaded.load())
        {
            crow::json::wvalue body;
            body["ok"] = false;
            body["error"] = "keystore_locked";
            body["message"] = "keystore not yet unlocked";
            return MakeJsonResponse(503, body);
        }

        // Require a valid MCP key.  Without this gate, any unauthenticated
        // caller could pin IsMcpConnected() to true and suppress staleness alerts.
        std::optional<AuthResult> const auth = TryMcpAuth(req);
        if (!auth.has_value())
        {
            LOG_SECURITY_WARN("[security] mcp_heartbeat_unauthorized ip={}", req.remote_ip_address);
            RecordAuthFailure(req.remote_ip_address);
            return MakeAuthErrorResponse("missing or invalid MCP credential");
        }

        // A populated AuthResult with a non-empty m_Error is an auth FAILURE, not a
        // success — TryMcpAuth returns AuthResult{"invalid_token", ...} on a bogus
        // mcp_* token (and has already logged mcp_auth_failure + called
        // RecordAuthFailure for the lockout counter).  Without this check the
        // handler would treat any non-empty mcp_* Bearer as a valid heartbeat,
        // updating m_McpLastHeartbeat and pinning IsMcpConnected() to true — exactly
        // the staleness-alert suppression the gate above warns about.  Don't
        // double-count the failure here; TryMcpAuth owns the counter.
        if (!auth->m_Error.empty())
        {
            LOG_SECURITY_WARN("[security] mcp_heartbeat_unauthorized ip={} error={}", req.remote_ip_address,
                              auth->m_Error);
            return MakeAuthErrorResponse("missing or invalid MCP credential");
        }

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

        // Edition + capabilities. Capabilities reflect actual route registration —
        // routes that are role-gated but exist in both editions are reported `true`
        // here regardless of edition; the dashboard does its own role check via
        // /api/auth/whoami before exposing admin/operator UI.
#ifdef J9T_STUDIO
        status["edition"] = "studio";
        status["capabilities"]["workflow_crud"] = true;       // Studio-only: POST/PUT/DELETE /api/workflows
        status["capabilities"]["ai_assistant"] = true;        // Studio-only: /ws/assistant
        status["capabilities"]["ai_jcwf"] = true;             // Studio-only: AI JCWF generation
#else
        status["edition"] = "engine";
        status["capabilities"]["workflow_crud"] = false;
        status["capabilities"]["ai_assistant"] = false;
        status["capabilities"]["ai_jcwf"] = false;
#endif
        // Common routes registered in both editions, role-gated at the handler.
        status["capabilities"]["workflow_run_endpoint"] = true;  // POST /api/workflows/<id>/run (operator+)
        status["capabilities"]["settings_api"] = true;           // /api/settings/* + /api/connections/* (admin)
        status["capabilities"]["log_analyze"] = true;            // GET /api/log/analyze-last-run (operator+)
        status["capabilities"]["workflow_versions"] = true;      // GET /api/workflows/<id>/versions* (admin)
        status["capabilities"]["workflow_reload"] = true;        // POST /api/workflows/reload (admin)

        status["tls"] = m_TlsEnabled;

        // Key unlock state — both the provider keys and the MCP key store.
        {
            auto const& keyManager = Core::g_Core->GetKeyManager();
            status["keys_unlocked"] = (keyManager.GetKeyLoadStatus() == KeyManager::KeyLoadStatus::Ok);
            status["mcp_keys_loaded"] = m_McpKeysLoaded.load();
        }

        // Adhoc workflow submission stats.
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
        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
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
                        if (ch.m_LastFailureCode.has_value())
                        {
                            connections[i]["last_failure_code"] = std::string(Describe(*ch.m_LastFailureCode));
                        }
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

                // Sitting-7 Workstream C: the dashboard's hazard glyph paints workflow
                // rows red when ANY of their ai_call tasks use a degraded provider.
                // Pre-resolved at workflow-load time (`m_RequiredAiProviders`).  Empty
                // string in the vector means "system default provider".
                if (!workflowDefinition->m_RequiredAiProviders.empty())
                {
                    crow::json::wvalue::list interfaceNamesJson;
                    interfaceNamesJson.reserve(workflowDefinition->m_RequiredAiProviders.size());
                    for (std::string const& providerName : workflowDefinition->m_RequiredAiProviders)
                    {
                        interfaceNamesJson.push_back(providerName);
                    }
                    workflowEntry["interface_names"] = std::move(interfaceNamesJson);
                }

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
        // Drop the fs::exists() precheck — it forms a TOCTOU window where the
        // version file can be deleted between the existence check and the open.
        // The is_open() check below covers both "file gone" and "permission
        // denied"; for this read-only endpoint, conflating both into 404 is
        // fine (the caller can't act on the distinction).  Binary mode is
        // mandatory — the snapshot is a zip blob, not text; text-mode read
        // would apply CRLF translation on Windows and silently corrupt it.
        std::ifstream ifs(versionPath, std::ios::binary);
        if (!ifs.is_open())
        {
            return MakeWorkflowJsonError(404, "version_not_found", "Version not found: " + timestamp,
                                         "GET /api/workflows/{id}/versions/{ts}", workflowId);
        }

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        crow::response resp(200, content);
        resp.set_header("Content-Type", "application/octet-stream");
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
        fs::path const targetPath = (workflowsDirectoryAbsolute / (workflowId + ".jcwf")).lexically_normal();

        // Read the version content directly.  Dropping the fs::exists() precheck
        // closes a TOCTOU window where the version file could be deleted between
        // the check and the open; the is_open() failure below covers both
        // "file gone" and "permission denied".  Binary mode is mandatory — the
        // .history snapshot is a zip blob, not text; the default text-mode read
        // would apply CRLF translation on Windows and silently corrupt the zip.
        std::ifstream ifs(versionPath, std::ios::binary);
        if (!ifs.is_open())
        {
            return MakeWorkflowJsonError(404, "version_not_found", "Version not found: " + timestamp,
                                         "POST /api/workflows/{id}/versions/{ts}/restore", workflowId);
        }
        std::string versionContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();

        // Backup current before restoring (best-effort).  Drop the
        // fs::exists(targetPath) precheck and let fs::copy_file's std::error_code
        // signal "source missing" — the copy is best-effort either way; a missing
        // source just means there was nothing to back up.
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
                // ec absorbed: best-effort backup, restore proceeds regardless.
            }
        }

        // Install the restored zip blob via the container-aware registry path.
        // `.jcwf` is always a zip (per `feedback_no_legacy_jcwf`); the previous
        // call to SaveOrUpdateWorkflowFromJson here treated the bytes as JSON
        // and failed-closed with UNCLOSED_STRING on every restore attempt.
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRegistry != nullptr)
            {
                std::string upsertErrorMessage;
                if (!m_WorkflowRegistry->UpsertJcwfFromZipBytes(versionContent, targetPath, upsertErrorMessage))
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

        EnqueueRunResult enqueueResult;
        if (context.empty())
        {
            enqueueResult = workflowRuntimeManager->EnqueueWorkflowRunAndGetRunId(workflowId);
        }
        else
        {
            enqueueResult =
                workflowRuntimeManager->EnqueueWorkflowRunWithContextAndGetRunId(workflowId, std::string(), context);
        }

        if (auto errorResponse = MaybeEnqueueErrorResponse(enqueueResult, "POST /api/workflows/{id}/run", workflowId))
        {
            return std::move(*errorResponse);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["enqueued"] = true;
        responseJson["id"] = workflowId;
        responseJson["runId"] = enqueueResult.m_RunId;

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
        auto pendingRuns = workflowRuntimeManager->GetPendingRunsSnapshot();

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
        // Serialize-policy deferred runs: rendered with state="pending", empty timestamps,
        // taskCount=0.  The dashboard's existing run-row renderer handles these uniformly
        // — pending state was already in the WorkflowRunState enum and the stringifier
        // emits "pending" for it.  Without this, a user clicking Run while another run is
        // already active would see no response in /active and think the click was lost.
        for (auto const& pending : pendingRuns)
        {
            crow::json::wvalue runJson;
            runJson["runId"] = pending.m_RunId;
            runJson["workflowId"] = pending.m_WorkflowId;
            runJson["state"] = ToStringWorkflowRunState(WorkflowRunState::Pending);
            runJson["startedAt"] = std::string();
            runJson["completedAt"] = std::string();
            runJson["taskCount"] = static_cast<int64_t>(0);
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
            // runId becomes a path segment (workflowsDir / workflowId / taskName / n8n / runId).
            // Without this validation a caller-supplied "../../foo" escapes the run dir
            // and writes request.json wherever the process can write.  The same allowlist
            // (alnum + `_`/`-`) used for workflowId / taskName applies here.
            else if (!IsValidWorkflowId(runId))
            {
                return MakeWorkflowJsonError(400, "invalid_run_id", "runId contains invalid characters",
                                             "POST /api/integrations/n8n/start", workflowId);
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

            EnqueueRunResult const enqueueResult =
                workflowRuntimeManager->EnqueueWorkflowRunWithContextAndGetRunId(workflowId, runId, context);

            if (auto errorResponse =
                    MaybeEnqueueErrorResponse(enqueueResult, "POST /api/integrations/n8n/start", workflowId))
            {
                return std::move(*errorResponse);
            }

            crow::json::wvalue responseJson;
            responseJson["ok"] = true;
            responseJson["workflowId"] = workflowId;
            responseJson["runId"] = enqueueResult.m_RunId;
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
        // Webhook secret is mandatory in both editions; the validator rejects JCWFs
        // missing one, so reaching this point with an empty secret means the trigger
        // was registered before the validator change or via direct file edit.
        if (webhookTrigger->m_Secret.empty())
        {
            LOG_SECURITY_WARN("[security] webhook_rejected reason=secret_not_configured ip={} workflowId={}",
                              req.remote_ip_address, workflowId);
            LOG_APP_ERROR("WebServer::HandleWebhookPost: webhook secret not configured for workflow '{}' "
                          "(secrets are mandatory)",
                          workflowId);
            return MakeWorkflowJsonError(403, "secret_required",
                                         "Webhook secret is required. "
                                         "Configure a secret in the workflow's webhook trigger.",
                                         kEndpoint, workflowId);
        }
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

                // Optional runId.  When provided by the caller, the value
                // becomes a path segment under the webhook run dir
                // (workflowsDir / workflowId / "webhook" / runId / request.json).
                // Without this allowlist, a runId like "../../foo" escapes that
                // dir and writes request.json wherever the process can write.
                {
                    auto runIdField = doc["runId"].get_string();
                    if (runIdField.error() == simdjson::SUCCESS)
                    {
                        runId = std::string(runIdField.value());
                        if (!runId.empty() && !IsValidWorkflowId(runId))
                        {
                            return MakeWorkflowJsonError(400, "invalid_run_id",
                                                         "runId contains invalid characters",
                                                         kEndpoint, workflowId);
                        }
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

        EnqueueRunResult const enqueueResult =
            workflowRuntimeManager->EnqueueWorkflowRunWithContextAndGetRunId(workflowId, runId, context);

        if (auto errorResponse = MaybeEnqueueErrorResponse(enqueueResult, kEndpoint, workflowId))
        {
            return std::move(*errorResponse);
        }

        LOG_SECURITY_INFO("[security] webhook_accepted ip={} workflowId={} runId={}", req.remote_ip_address, workflowId,
                          enqueueResult.m_RunId);
        LOG_APP_INFO("WebServer::HandleWebhookPost: enqueued run '{}' for workflow '{}' (trigger '{}')",
                     enqueueResult.m_RunId, workflowId, webhookTrigger->m_TriggerId);

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["workflowId"] = workflowId;
        responseJson["runId"] = enqueueResult.m_RunId;
        responseJson["triggerId"] = webhookTrigger->m_TriggerId;

        BroadcastWorkflowRunsSnapshot();
        BroadcastWorkflowRunsLastSnapshot();
        return MakeJsonResponse(202, responseJson);
    }


    crow::response WebServer::ReadLogFile(crow::request const& req, std::string const& logPath)
    {
        // GET ...?tail=N        — return last N lines (initial load)
        // GET ...?offset=N      — return lines appended since byte offset N (delta polling)
        // Returns: { ok, lines[], byteOffset, totalSize }

        // Path confinement: resolve `logPath` under the launch cwd and reject
        // anything that doesn't land inside the launch cwd's `log/` directory.
        // Current call sites pass hardcoded "log/log.txt" / "log/security.txt"
        // — the gate exists so a future refactor that lets the caller influence
        // logPath cannot read arbitrary files.
        std::filesystem::path const launchCwd =
            (Core::g_Core != nullptr) ? Core::g_Core->GetLaunchCWDAbsolute() : std::filesystem::path{};
        std::filesystem::path const logsRoot = launchCwd / "log";
        std::filesystem::path const resolvedPath = WebServerHelpers::ConfinePathUnder(launchCwd, logPath);
        std::error_code ec;
        std::filesystem::path const canonicalLogsRoot = std::filesystem::weakly_canonical(logsRoot, ec);
        bool const insideLogsRoot = !resolvedPath.empty() && !ec && !canonicalLogsRoot.empty() &&
                                    [&]()
                                    {
                                        std::filesystem::path const rel =
                                            resolvedPath.lexically_relative(canonicalLogsRoot);
                                        std::string const relGeneric = rel.generic_string();
                                        return !rel.empty() && relGeneric != ".." &&
                                               relGeneric.rfind("../", 0) != 0;
                                    }();
        if (!insideLogsRoot)
        {
            LOG_SECURITY_WARN("[security] readlog_path_escape len={}", logPath.size());
            crow::json::wvalue resp;
            resp["ok"] = false;
            resp["error"] = "Invalid log path";
            return MakeJsonResponse(400, resp);
        }

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

        std::ifstream file(resolvedPath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            crow::json::wvalue resp;
            resp["ok"] = false;
            resp["error"] = "Log file not found";
            return MakeJsonResponse(404, resp);
        }

        int64_t const fileSize = static_cast<int64_t>(file.tellg());

        // Defense-in-depth: clamp fromOffset to [0, fileSize] before any
        // arithmetic uses it.  The two `if` guards below already prevent the
        // overflow path, but explicit clamping documents the invariant and
        // bounds any future refactor that drops one of the guards.
        if (fromOffset > fileSize)
            fromOffset = fileSize;

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
        // otherwise it is invisible to per-run analysis.
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

    void WebServer::RegisterWebSocket()
    {
        CROW_WEBSOCKET_ROUTE(m_Server, "/ws")
            // Validate the credential at upgrade time in both editions.
            // The auth funnel is identical across Studio and Engine — anonymous
            // WS clients would otherwise receive workflow-run snapshots and
            // log lines on connect, which is a real data leak.
            .onaccept(
                [this](crow::request const& req, void** userdata)
                {
                    auto auth = Authenticate(req);
                    if (!auth.Ok())
                    {
                        LOG_SECURITY_WARN("[security] ws_upgrade_rejected ip={} reason={}",
                                          req.remote_ip_address, auth.m_Error);
                        return false;
                    }
                    // Pin the role to this specific connection.  ai-write-scripts
                    // (and any future admin-only message type) re-checks the role
                    // before mutating disk state, so a session/MCP key with role
                    // "operator" or "viewer" cannot escalate to admin via the
                    // shared /ws upgrade.  The string is freed in .onopen.
                    if (userdata != nullptr)
                    {
                        *userdata = new std::string(auth.m_Role);
                    }
                    return true;
                })
            .onopen(
                [this](crow::websocket::connection& conn)
                {
                    std::string role;
                    if (auto* rolePtr = static_cast<std::string*>(conn.userdata()))
                    {
                        role = std::move(*rolePtr);
                        delete rolePtr;
                        conn.userdata(nullptr);
                    }

                    size_t clients = 0;
                    size_t totalConnects = 0;
                    size_t peak = 0;
                    {
                        std::lock_guard<std::mutex> lock(m_Mutex);
                        m_Clients.insert(&conn);
                        m_WsClientRoles[&conn] = std::move(role);
                        m_ClientCount.store(m_Clients.size(), std::memory_order_relaxed);
                        ++m_WsTotalConnects;
                        if (m_Clients.size() > m_WsPeakClients)
                        {
                            m_WsPeakClients = m_Clients.size();
                        }
                        clients = m_Clients.size();
                        totalConnects = m_WsTotalConnects;
                        peak = m_WsPeakClients;
                    }
                    LOG_APP_INFO("WebSocket client connected (total: {}, lifetime: {}, peak: {})", clients,
                                 totalConnects, peak);

                    // Queue current workflow run snapshots.
                    BroadcastWorkflowRunsSnapshot();
                    BroadcastWorkflowRunsLastSnapshot();
                })
            .onclose(
                [this](crow::websocket::connection& conn, const std::string& reason, uint16_t code)
                {
                    size_t clients = 0;
                    size_t totalDisconnects = 0;
                    {
                        std::lock_guard<std::mutex> lock(m_Mutex);
                        m_Clients.erase(&conn);
                        m_WsClientRoles.erase(&conn);
                        m_ClientCount.store(m_Clients.size(), std::memory_order_relaxed);
                        ++m_WsTotalDisconnects;
                        clients = m_Clients.size();
                        totalDisconnects = m_WsTotalDisconnects;
                    }
                    LOG_APP_INFO("WebSocket client disconnected ({}, code {}) (remaining: {}, lifetime disconnects: {})",
                                 reason, code, clients, totalDisconnects);
                })
            .onmessage(
                [this](crow::websocket::connection& conn, const std::string& data, bool /*is_binary*/)
                {
                    try
                    {
                        simdjson::ondemand::parser parser;
                        simdjson::padded_string json(data);
                        simdjson::ondemand::document doc = parser.iterate(json);

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
                        else if (HandleAssistantWebSocketMessage(conn, doc, type))
                        {
                            // Dispatched by the edition-specific assistant handler
                            // (Studio: ai-explain-jcwf / ai-generate-jcwf /
                            // ai-write-scripts / ai-fix-failed-script).  Engine
                            // always returns false here.
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

        // Try to initialise the MCP key store if the master password is already
        // held in mlock memory (from engine-level keys.json.enc load at startup).
        // If not yet held, the store is lazily initialised on HandleKeysUnlockPost().
        {
            auto& keyManager = Core::g_Core->GetKeyManager();
            bool const initialised = keyManager.WithMasterPassword(
                [this](std::string_view masterPassword) { InitMcpKeyStore(masterPassword); });
            if (!initialised)
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

        // Detach our run-terminal observer from the WRM before any teardown can
        // start unwinding `m_AdhocManager`.  The observer's lambda captures
        // `m_AdhocManager.get()` as a raw pointer; if WRM outlives WebServer
        // (e.g. teardown order at process exit) and a run terminates after
        // `m_AdhocManager` is destroyed, the lambda's dispatch into
        // `adhoc->OnRunCompleted()` would be a use-after-free.  Clearing here
        // is defensive — the swap-detach in SetWorkflowRuntimeManager covers
        // re-init flows; this covers final shutdown.
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            if (m_WorkflowRuntimeManager)
            {
                m_WorkflowRuntimeManager->SetRunTerminalObserver({});
                LOG_APP_INFO("[shutdown] WebServer::SignalStop: detached run-terminal observer from WRM");
            }
        }

        // Stop the reaper inside the watchdog window.  Without this, the reaper
        // is only torn down via ~AdhocWorkflowManager during unique_ptr<Application>
        // destruction at end of main — AFTER engine.cpp's 3-s shutdown watchdog is
        // diffused.  A future CV-wake regression in StopReaperThread would then
        // silently stall exit by up to 60 s with no watchdog line and no force-exit.
        // Lifting the stop here puts it inside the watchdog window so any stall
        // becomes a loud _exit(EXIT_FAILURE) instead of a silent slow exit.
        // StopReaperThread is idempotent — the destructor still calls it as a no-op
        // safety net.
        if (m_AdhocManager)
        {
            LOG_APP_INFO("[shutdown] stopping AdhocManager reaper thread...");
            m_AdhocManager->StopReaperThread();
            LOG_APP_INFO("[shutdown] AdhocManager reaper thread stopped");
        }

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
        ++m_WsTotalBroadcastsEnqueued;
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
        ++m_WsTotalBroadcastsEnqueued;
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
        auto const drainStart = std::chrono::steady_clock::now();
        std::vector<std::string> pending;
        std::vector<std::string> logLines;

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
                ++m_WsTotalBroadcastsEnqueued;
                ++m_WsTotalLogBatchesEnqueued;
            }

            if (m_PendingBroadcasts.empty())
                return;
            pending.swap(m_PendingBroadcasts);
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

        // Hold m_Mutex for the entire send loop.  The previous pattern (snapshot
        // m_Clients under lock, drop lock, build the JSON batch outside lock,
        // then per-client lock-find-unlock-send) had a UAF window: between the
        // per-client re-validation and the send_text call, onclose on another
        // ASIO thread could erase the connection and Crow could destroy it,
        // leaving a dangling pointer.  Holding m_Mutex throughout the send loop
        // keeps onclose blocked on the lock — which guarantees the connections
        // we're sending to are alive for the duration of the loop.  Crow's
        // send_text is asio::post-based (verified in code/vendor/crow/include/crow/
        // crow/websocket.h::send_data — the actual write happens on the
        // connection's io-context strand), so the send_text call itself is
        // microseconds; the lock window stays small.
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            for (auto* client : m_Clients)
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

        // Update drain diagnostics. Duration captures the *whole* drain including
        // build + send_text on every client; on TLS sockets the send is the
        // dominant cost when batches grow large, which is exactly what we are
        // trying to measure here.
        auto const drainEnd = std::chrono::steady_clock::now();
        uint64_t const durationUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(drainEnd - drainStart).count());
        size_t const batchBytes = safeBatch.size();
        size_t const messageCount = pending.size();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_WsTotalDrains;
            m_WsLastDrainBytes = batchBytes;
            m_WsLastDrainMessages = messageCount;
            m_WsLastDrainDurationUs = durationUs;
            if (batchBytes > m_WsPeakDrainBytes)
                m_WsPeakDrainBytes = batchBytes;
            if (durationUs > m_WsPeakDrainDurationUs)
                m_WsPeakDrainDurationUs = durationUs;
        }
    }

    void WebServer::BroadcastPythonStatus(bool pythonRunning)
    {
        crow::json::wvalue msg;
        msg["type"] = "python-status";
        msg["running"] = pythonRunning;

        std::string const payload = msg.dump();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_WsTotalPythonStatusEnqueued;
        }
        BroadcastJSON(payload);
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
        std::string const payload = msg.dump();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_WsTotalAiCallEventsEnqueued;
        }
        BroadcastJSON(payload);
    }

    void WebServer::BroadcastAiCallCompleted(std::string const& probName, std::string const& interfaceName,
                                             int32_t inputTokens, int32_t outputTokens, int32_t totalTokens,
                                             std::string const& finishReason)
    {
        crow::json::wvalue msg;
        msg["type"] = "ai-call-completed";
        msg["prob"] = probName;
        msg["interface_name"] = interfaceName;
        msg["input_tokens"] = static_cast<int64_t>(inputTokens);
        msg["output_tokens"] = static_cast<int64_t>(outputTokens);
        msg["total_tokens"] = static_cast<int64_t>(totalTokens);
        msg["finish_reason"] = finishReason;
        std::string const payload = msg.dump();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_WsTotalAiCallEventsEnqueued;
        }
        BroadcastJSON(payload);
    }

    void WebServer::BroadcastCapChanged()
    {
        // Sitting-8 Workstream D close-out: payload-free wake signal.  Dashboard
        // hears this, refetches /api/providers/health for the new cap state.
        // Bounded broadcast frequency: dispatcher only fires on actual cap
        // mutation, not on every observation — natural rate-limiting from AIMD's
        // halve-on-429 / streak-grow dynamics.
        crow::json::wvalue msg;
        msg["type"] = "cap-changed";
        std::string const payload = msg.dump();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_WsTotalAiCallEventsEnqueued;
        }
        BroadcastJSON(payload);
    }

    void WebServer::BroadcastAiCallFailed(std::string const& probName, int errorKind,
                                          int httpStatus, std::string const& errorMessage,
                                          std::string const& providerErrorCode,
                                          std::string const& providerErrorType,
                                          std::string_view category,
                                          std::optional<int> retryAfterSeconds,
                                          std::string const& interfaceName)
    {
        crow::json::wvalue msg;
        msg["type"] = "ai-call-failed";
        msg["prob"] = probName;
        msg["error_kind"] = static_cast<int64_t>(errorKind);
        msg["http_status"] = static_cast<int64_t>(httpStatus);
        msg["error_message"] = errorMessage;
        msg["provider_error_code"] = providerErrorCode;
        msg["provider_error_type"] = providerErrorType;
        msg["category"] = std::string(category);
        msg["interface_name"] = interfaceName;
        if (retryAfterSeconds.has_value())
        {
            msg["retry_after_seconds"] = static_cast<int64_t>(*retryAfterSeconds);
        }
        std::string const payload = msg.dump();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_WsTotalAiCallEventsEnqueued;
        }
        BroadcastJSON(payload);
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

    // ------------------------------------------------------------------
    // Parse the optional `rate_limit` and `default_output_tokens` blocks
    // from a REST POST/PUT body and apply them to the target structs.
    // Missing fields preserve current values (so partial updates work for
    // PUT, and the create path keeps its struct defaults when the client
    // omits rate_limit).  Mirrors the on-disk parsing in
    // ConfigParser::Load — same field names, same semantics.
    // ------------------------------------------------------------------
    static void ApplyAiInterfaceRateLimitFromJson(std::string const& json,
                                                   ConfigParser::EngineConfig::RateLimit& rateLimit,
                                                   int32_t& defaultOutputTokens)
    {
        // simdjson::ondemand requires SIMDJSON_PADDING bytes after the buffer;
        // padded_string copies + adds the padding.  Iterating a raw std::string
        // silently no-ops (no exception, no SUCCESS) — this exact bug used to
        // make POST/PUT bodies' rate_limit and default_output_tokens overrides
        // get dropped, leaving every new interface with C++ struct defaults.
        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string padded(json);
            auto doc = parser.iterate(padded);
            int64_t outVal = 0;
            if (doc["default_output_tokens"].get_int64().get(outVal) == simdjson::SUCCESS && outVal > 0)
            {
                defaultOutputTokens = static_cast<int32_t>(outVal);
            }
        }
        catch (...)
        {
        }

        try
        {
            simdjson::padded_string padded(json);
            auto doc = parser.iterate(padded);
            simdjson::ondemand::object rateLimitObject;
            if (doc["rate_limit"].get_object().get(rateLimitObject) != simdjson::SUCCESS)
            {
                return;
            }
            auto& budget = rateLimit.m_RequestBudget;
            for (auto rlField : rateLimitObject)
            {
                std::string_view rlKey;
                if (rlField.unescaped_key().get(rlKey) != simdjson::SUCCESS)
                    continue;

                if (rlKey == "initial_concurrency_probe")
                {
                    int64_t v = 0;
                    if (rlField.value().get_int64().get(v) == simdjson::SUCCESS)
                        rateLimit.m_InitialConcurrencyProbe = static_cast<int>(v);
                }
                else if (rlKey == "max_concurrency")
                {
                    int64_t v = 0;
                    if (rlField.value().get_int64().get(v) == simdjson::SUCCESS && v > 0)
                        rateLimit.m_MaxConcurrency = static_cast<int>(v);
                }
                else if (rlKey == "max_retries_429")
                {
                    int64_t v = 0;
                    if (rlField.value().get_int64().get(v) == simdjson::SUCCESS && v >= 0)
                        rateLimit.m_MaxRetries429 = static_cast<int>(v);
                }
                else if (rlKey == "max_retries_transient")
                {
                    int64_t v = 0;
                    if (rlField.value().get_int64().get(v) == simdjson::SUCCESS && v >= 0)
                        rateLimit.m_MaxRetriesTransient = static_cast<int>(v);
                }
                else if (rlKey == "base_retry_ms")
                {
                    int64_t v = 0;
                    if (rlField.value().get_int64().get(v) == simdjson::SUCCESS && v > 0)
                        rateLimit.m_BaseRetryMs = static_cast<int>(v);
                }
                else if (rlKey == "request_budget")
                {
                    simdjson::ondemand::object budgetObject;
                    if (rlField.value().get_object().get(budgetObject) != simdjson::SUCCESS)
                        continue;
                    for (auto bField : budgetObject)
                    {
                        std::string_view bKey;
                        if (bField.unescaped_key().get(bKey) != simdjson::SUCCESS)
                            continue;
                        double v = 0.0;
                        if (bField.value().get_double().get(v) != simdjson::SUCCESS)
                            continue;
                        if (bKey == "per_1k_input_token_seconds")
                            budget.m_Per1kInputTokenSeconds = v;
                        else if (bKey == "per_1k_output_token_seconds")
                            budget.m_Per1kOutputTokenSeconds = v;
                        else if (bKey == "fixed_overhead_seconds")
                            budget.m_FixedOverheadSeconds = v;
                        else if (bKey == "safety_margin_factor")
                            budget.m_SafetyMarginFactor = v;
                        else if (bKey == "min_seconds")
                            budget.m_MinSeconds = v;
                        else if (bKey == "max_seconds")
                            budget.m_MaxSeconds = v;
                    }
                }
            }
        }
        catch (...)
        {
        }
    }

    crow::response WebServer::HandleProvidersHealthGet()
    {
        // Sitting-8 Workstream D: per-interface health snapshot.  Joins config
        // identity + pool last-error + dispatcher AIMD cap into one array.
        // Returns empty array (200) when no AiRequestPool is wired — keeps the
        // dashboard's mount fetch happy in test/dev configurations without a
        // full j9t process behind it.
        crow::json::wvalue responseJson;
        responseJson["ok"] = true;

        JarvisAgent* app = dynamic_cast<JarvisAgent*>(App::g_App.load(std::memory_order_acquire));
        AiRequestPool const* pool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;
        if (pool == nullptr)
        {
            responseJson["interfaces"] = crow::json::wvalue::list{};
            return crow::response(200, responseJson);
        }

        auto const snapshots = pool->SnapshotProviderHealth();
        std::vector<crow::json::wvalue> items;
        items.reserve(snapshots.size());
        for (auto const& snap : snapshots)
        {
            crow::json::wvalue item;
            item["interface_name"]      = snap.m_InterfaceName;
            item["interface_type_name"] = snap.m_InterfaceTypeName;
            item["quota_key"]           = snap.m_QuotaKey;
            item["is_mock"]             = snap.m_IsMock;
            item["current_cap"]         = static_cast<int64_t>(snap.m_CurrentCap);
            item["max_cap"]             = static_cast<int64_t>(snap.m_MaxCap);
            item["floor_cap"]           = static_cast<int64_t>(snap.m_FloorCap);
            // Timestamps as Unix milliseconds (matches the rest of the WS
            // schema — easier for the dashboard's JS Date constructor than
            // ISO 8601 strings).  Zero = epoch = "never errored".
            auto const lastErrorMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                snap.m_LastErrorAt.time_since_epoch()).count();
            item["last_error_at_ms"]    = static_cast<int64_t>(lastErrorMs);
            item["last_error_code"]     = snap.m_LastErrorCode;
            item["last_error_type"]     = snap.m_LastErrorType;
            item["last_error_message"]  = snap.m_LastErrorMessage;
            item["last_error_category"] = std::string(CategoryToString(snap.m_LastErrorCategory));
            item["last_http_status"]    = static_cast<int64_t>(snap.m_LastHttpStatus);
            if (snap.m_RetryAfterSeconds.has_value())
            {
                item["retry_after_seconds"] = static_cast<int64_t>(*snap.m_RetryAfterSeconds);
            }
            item["consecutive_errors"]            = static_cast<int64_t>(snap.m_ConsecutiveErrors);
            item["success_streak_since_last_error"] =
                static_cast<int64_t>(snap.m_SuccessStreakSinceLastError);
            auto const pinnedSinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                snap.m_CapPinnedAtFloorSince.time_since_epoch()).count();
            item["cap_pinned_at_floor_since_ms"] = static_cast<int64_t>(pinnedSinceMs);
            items.push_back(std::move(item));
        }
        responseJson["interfaces"] = std::move(items);
        return crow::response(200, responseJson);
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
                case ConfigParser::EngineConfig::InterfaceType::API5: item["api_type"] = "API5"; break;
                case ConfigParser::EngineConfig::InterfaceType::API6: item["api_type"] = "API6"; break;
                default: item["api_type"] = "API1"; break;
            }
            item["key_name"] = iface.m_KeyName;
            item["max_context_tokens"] = static_cast<int64_t>(iface.m_MaxContextTokens);
            item["default_output_tokens"] = static_cast<int64_t>(iface.m_DefaultOutputTokens);
            item["is_mock"] = iface.m_IsMock;
            item["fixture_path"] = iface.m_FixturePath;

            // Rate-limit + size-aware request budget — always emit so the UI
            // can render current effective values (defaults included) without
            // needing a second source-of-truth.
            crow::json::wvalue rl;
            rl["initial_concurrency_probe"] = iface.m_RateLimit.m_InitialConcurrencyProbe;
            rl["max_concurrency"]           = iface.m_RateLimit.m_MaxConcurrency;
            rl["max_retries_429"]           = iface.m_RateLimit.m_MaxRetries429;
            rl["max_retries_transient"]     = iface.m_RateLimit.m_MaxRetriesTransient;
            rl["base_retry_ms"]             = iface.m_RateLimit.m_BaseRetryMs;
            crow::json::wvalue budget;
            budget["per_1k_input_token_seconds"]  = iface.m_RateLimit.m_RequestBudget.m_Per1kInputTokenSeconds;
            budget["per_1k_output_token_seconds"] = iface.m_RateLimit.m_RequestBudget.m_Per1kOutputTokenSeconds;
            budget["fixed_overhead_seconds"]      = iface.m_RateLimit.m_RequestBudget.m_FixedOverheadSeconds;
            budget["safety_margin_factor"]        = iface.m_RateLimit.m_RequestBudget.m_SafetyMarginFactor;
            budget["min_seconds"]                 = iface.m_RateLimit.m_RequestBudget.m_MinSeconds;
            budget["max_seconds"]                 = iface.m_RateLimit.m_RequestBudget.m_MaxSeconds;
            rl["request_budget"] = std::move(budget);
            item["rate_limit"] = std::move(rl);

            items.push_back(std::move(item));
        }
        responseJson["interfaces"] = std::move(items);
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleAiInterfaceCreatePost(crow::request const& req)
    {
        auto& config = Core::g_Core->GetMutableConfig();

        std::string name, description, url, model, apiTypeStr, keyName, fixturePath;
        uint64_t maxContextTokensOverride = 0; // 0 = fall back to model-name resolution
        bool isMock = false;

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
                {
                    auto d2 = parser.iterate(json);
                    bool isMockVal = false;
                    if (d2["is_mock"].get_bool().get(isMockVal) == simdjson::SUCCESS)
                    {
                        isMock = isMockVal;
                    }
                }
                {
                    auto d2 = parser.iterate(json);
                    if (d2["fixture_path"].get_string().get(sv) == simdjson::SUCCESS)
                    {
                        fixturePath = std::string(sv);
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

        // Legacy api_type "Test" rejection with migration guidance.
        if (apiTypeStr == "Test")
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "api_type_test_removed";
            err["message"] = "api_type 'Test' has been removed.  Use api_type: '<API1..API6>' + "
                             "is_mock: true + fixture_path: '<path>' instead.  See doc/jarvisagent.md.";
            return MakeJsonResponse(400, err);
        }

        // is_mock + fixture_path coupling — same fail-closed rule as
        // ConfigParser: is_mock=true requires a fixture_path that resolves
        // under the project root.
        if (isMock)
        {
            if (fixturePath.empty())
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "is_mock_requires_fixture_path";
                err["message"] = "is_mock=true requires a non-empty fixture_path.";
                return MakeJsonResponse(400, err);
            }
            std::filesystem::path const confined = ConfineUnderProjectRoot(fixturePath);
            if (confined.empty())
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "fixture_path_rejected";
                err["message"] = "fixture_path rejected by ConfineUnderProjectRoot "
                                 "(outside project root, symlink escape, or unresolvable): '" + fixturePath + "'";
                return MakeJsonResponse(400, err);
            }
        }

        // Plain-HTTP policy: http:// is loopback-only AND never with a
        // key_name.  Mirrors the ConfigParser gate so REST-driven creation
        // can't slip a misconfigured interface past the config-load check.
        if (auto const result = UrlPolicy::ValidateAiInterfaceUrl(url, keyName);
            !result.has_value())
        {
            bool const isCredentialed =
                result.error().m_Code == UrlPolicy::UrlPolicyErrorCode::CredentialedPlaintextHttp;
            if (isCredentialed)
            {
                UrlPolicy::RecordCredentialedPlaintextHttpRejection();
            }
            else
            {
                UrlPolicy::RecordUrlPolicyRejection();
            }
            LOG_SECURITY_WARN("[security] ai_interface_url_rejected origin=rest_create url='{}' key_name='{}' "
                              "code={} details='{}'",
                              url, keyName, UrlPolicy::Describe(result.error().m_Code),
                              result.error().m_Details);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = std::string(UrlPolicy::Describe(result.error().m_Code));
            err["message"] = result.error().m_Details;
            return MakeJsonResponse(400, err);
        }

        ConfigParser::EngineConfig::ApiInterface newIface;
        newIface.m_Url = url;
        newIface.m_Model = model;
        newIface.m_Description = description;
        newIface.m_KeyName = keyName;
        newIface.m_IsMock = isMock;
        newIface.m_FixturePath = fixturePath;
        newIface.m_Name =
            name.empty()
                ? ConfigParser::EngineConfig::GenerateInterfaceName(url, model, apiTypeStr.empty() ? "API1" : apiTypeStr)
                : name;
        newIface.m_MaxContextTokens =
            maxContextTokensOverride > 0
                ? maxContextTokensOverride
                : ConfigParser::EngineConfig::ResolveMaxContextTokensFromModel(model);

        // rate_limit + default_output_tokens — if absent in body, struct
        // defaults stay; partial overrides land cleanly via the helper.
        ApplyAiInterfaceRateLimitFromJson(req.body, newIface.m_RateLimit, newIface.m_DefaultOutputTokens);

        if (apiTypeStr == "API4")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API4;
        else if (apiTypeStr == "API3")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API3;
        else if (apiTypeStr == "API2")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API2;
        else if (apiTypeStr == "API5")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API5;
        else if (apiTypeStr == "API6")
            newIface.m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API6;
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
                        if (apiTypeStr == "Test")
                        {
                            crow::json::wvalue err;
                            err["ok"] = false;
                            err["error"] = "api_type_test_removed";
                            err["message"] = "api_type 'Test' has been removed.  Use api_type: '<API1..API6>' + "
                                             "is_mock: true + fixture_path: '<path>' instead.";
                            return MakeJsonResponse(400, err);
                        }
                        if (apiTypeStr == "API4")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API4;
                        else if (apiTypeStr == "API3")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API3;
                        else if (apiTypeStr == "API2")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API2;
                        else if (apiTypeStr == "API5")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API5;
                        else if (apiTypeStr == "API6")
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API6;
                        else
                            target->m_InterfaceType = ConfigParser::EngineConfig::InterfaceType::API1;
                    }
                }
                {
                    auto d = parser.iterate(json);
                    if (d["key_name"].get_string().get(sv) == simdjson::SUCCESS)
                        target->m_KeyName = std::string(sv);
                }
                {
                    auto d = parser.iterate(json);
                    int64_t v = 0;
                    if (d["max_context_tokens"].get_int64().get(v) == simdjson::SUCCESS && v > 0)
                        target->m_MaxContextTokens = static_cast<uint64_t>(v);
                }
                {
                    auto d = parser.iterate(json);
                    bool isMockVal = false;
                    if (d["is_mock"].get_bool().get(isMockVal) == simdjson::SUCCESS)
                        target->m_IsMock = isMockVal;
                }
                {
                    auto d = parser.iterate(json);
                    if (d["fixture_path"].get_string().get(sv) == simdjson::SUCCESS)
                        target->m_FixturePath = std::string(sv);
                }

                // After applying updates, validate the is_mock + fixture_path
                // coupling.  If either field is in an invalid state, reject
                // the whole update — partial-update semantics are still kept
                // because the prior values stay if the body didn't override
                // them, but the resulting coupling must satisfy the rule.
                if (target->m_IsMock)
                {
                    if (target->m_FixturePath.empty())
                    {
                        crow::json::wvalue err;
                        err["ok"] = false;
                        err["error"] = "is_mock_requires_fixture_path";
                        err["message"] = "is_mock=true requires a non-empty fixture_path.";
                        return MakeJsonResponse(400, err);
                    }
                    std::filesystem::path const confined = ConfineUnderProjectRoot(target->m_FixturePath);
                    if (confined.empty())
                    {
                        crow::json::wvalue err;
                        err["ok"] = false;
                        err["error"] = "fixture_path_rejected";
                        err["message"] = "fixture_path rejected by ConfineUnderProjectRoot: '" +
                                         target->m_FixturePath + "'";
                        return MakeJsonResponse(400, err);
                    }
                }

                // Plain-HTTP policy: re-validate after applying updates so a
                // partial PUT that swaps the URL or adds a key_name can't
                // sneak past the create-time gate.  Checks the resulting
                // state of m_Url + m_KeyName, not the request fields, so the
                // partial-update semantics carry through.
                if (auto const result =
                        UrlPolicy::ValidateAiInterfaceUrl(target->m_Url, target->m_KeyName);
                    !result.has_value())
                {
                    bool const isCredentialed =
                        result.error().m_Code == UrlPolicy::UrlPolicyErrorCode::CredentialedPlaintextHttp;
                    if (isCredentialed)
                    {
                        UrlPolicy::RecordCredentialedPlaintextHttpRejection();
                    }
                    else
                    {
                        UrlPolicy::RecordUrlPolicyRejection();
                    }
                    LOG_SECURITY_WARN("[security] ai_interface_url_rejected origin=rest_update name='{}' "
                                      "url='{}' key_name='{}' code={} details='{}'",
                                      target->m_Name, target->m_Url, target->m_KeyName,
                                      UrlPolicy::Describe(result.error().m_Code),
                                      result.error().m_Details);
                    crow::json::wvalue err;
                    err["ok"] = false;
                    err["error"] = std::string(UrlPolicy::Describe(result.error().m_Code));
                    err["message"] = result.error().m_Details;
                    return MakeJsonResponse(400, err);
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

            // Apply rate_limit + default_output_tokens overrides if present.
            // Outside the broad try/catch above so a malformed rate_limit
            // block silently keeps existing values rather than 400'ing the
            // whole update — partial updates were already accepted.
            ApplyAiInterfaceRateLimitFromJson(req.body, target->m_RateLimit, target->m_DefaultOutputTokens);
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
                case ConfigParser::EngineConfig::InterfaceType::API5: apiStr = "API5"; break;
                case ConfigParser::EngineConfig::InterfaceType::API6: apiStr = "API6"; break;
                default: apiStr = "API1"; break;
            }

            // Detect whether the rate_limit struct deviates from the
            // baked-in defaults (RateLimit{}, RequestBudget{}). Only emit
            // the block when it does — keeps the saved config.json minimal
            // and stable for users who never touched the knobs.
            ConfigParser::EngineConfig::RateLimit const defaultRateLimit{};
            ConfigParser::EngineConfig::RequestBudget const defaultBudget{};
            auto const& rl = iface.m_RateLimit;
            auto const& budget = rl.m_RequestBudget;
            bool const rateLimitDeviates =
                rl.m_InitialConcurrencyProbe != defaultRateLimit.m_InitialConcurrencyProbe ||
                rl.m_MaxConcurrency           != defaultRateLimit.m_MaxConcurrency ||
                rl.m_MaxRetries429            != defaultRateLimit.m_MaxRetries429 ||
                rl.m_MaxRetriesTransient      != defaultRateLimit.m_MaxRetriesTransient ||
                rl.m_BaseRetryMs              != defaultRateLimit.m_BaseRetryMs ||
                budget.m_Per1kInputTokenSeconds  != defaultBudget.m_Per1kInputTokenSeconds ||
                budget.m_Per1kOutputTokenSeconds != defaultBudget.m_Per1kOutputTokenSeconds ||
                budget.m_FixedOverheadSeconds    != defaultBudget.m_FixedOverheadSeconds ||
                budget.m_SafetyMarginFactor      != defaultBudget.m_SafetyMarginFactor ||
                budget.m_MinSeconds              != defaultBudget.m_MinSeconds ||
                budget.m_MaxSeconds              != defaultBudget.m_MaxSeconds;

            // RFC 8259 escape every caller-supplied string field — without this an admin
            // submitting a name / description / url / model / key_name with `"`, `\`,
            // newline, or any control byte would corrupt the resulting config.json
            // (naive string replacement without JSON escaping breaks the file).  apiStr
            // comes from a closed enum and needs no escaping.
            newArray += "        {\n";
            newArray += "            \"name\": \"" + JsonHelper::EscapeJsonString(iface.m_Name) + "\",\n";
            if (!iface.m_Description.empty())
            {
                newArray += "            \"description\": \"" + JsonHelper::EscapeJsonString(iface.m_Description) + "\",\n";
            }
            newArray += "            \"url\": \"" + JsonHelper::EscapeJsonString(iface.m_Url) + "\",\n";
            newArray += "            \"model\": \"" + JsonHelper::EscapeJsonString(iface.m_Model) + "\",\n";
            newArray += "            \"API\": \"" + apiStr + "\"";
            if (!iface.m_KeyName.empty())
            {
                newArray += ",\n";
                newArray += "            \"key_name\": \"" + JsonHelper::EscapeJsonString(iface.m_KeyName) + "\"";
            }
            if (iface.m_IsMock)
            {
                newArray += ",\n";
                newArray += "            \"is_mock\": true";
            }
            if (!iface.m_FixturePath.empty())
            {
                newArray += ",\n";
                newArray += "            \"fixture_path\": \"" + JsonHelper::EscapeJsonString(iface.m_FixturePath) + "\"";
            }
            if (iface.m_DefaultOutputTokens != 4096)
            {
                newArray += ",\n";
                newArray += "            \"default_output_tokens\": " + std::to_string(iface.m_DefaultOutputTokens);
            }
            if (rateLimitDeviates)
            {
                auto const num = [](double v) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%g", v);
                    return std::string(buf);
                };
                newArray += ",\n";
                newArray += "            \"rate_limit\": {\n";
                newArray += "                \"initial_concurrency_probe\": " + std::to_string(rl.m_InitialConcurrencyProbe) + ",\n";
                newArray += "                \"max_concurrency\": "           + std::to_string(rl.m_MaxConcurrency) + ",\n";
                newArray += "                \"max_retries_429\": "           + std::to_string(rl.m_MaxRetries429) + ",\n";
                newArray += "                \"max_retries_transient\": "     + std::to_string(rl.m_MaxRetriesTransient) + ",\n";
                newArray += "                \"base_retry_ms\": "             + std::to_string(rl.m_BaseRetryMs) + ",\n";
                newArray += "                \"request_budget\": {\n";
                newArray += "                    \"per_1k_input_token_seconds\": "  + num(budget.m_Per1kInputTokenSeconds) + ",\n";
                newArray += "                    \"per_1k_output_token_seconds\": " + num(budget.m_Per1kOutputTokenSeconds) + ",\n";
                newArray += "                    \"fixed_overhead_seconds\": "      + num(budget.m_FixedOverheadSeconds) + ",\n";
                newArray += "                    \"safety_margin_factor\": "        + num(budget.m_SafetyMarginFactor) + ",\n";
                newArray += "                    \"min_seconds\": "                 + num(budget.m_MinSeconds) + ",\n";
                newArray += "                    \"max_seconds\": "                 + num(budget.m_MaxSeconds) + "\n";
                newArray += "                }\n";
                newArray += "            }";
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

        // Tripwire: parse the post-replacement text with simdjson and confirm the
        // "API interfaces" array round-trips with the expected element count.  This
        // catches any future bug in the bracket-counting find-replace (e.g. a
        // mismatched `]` inside a string that the scanner misclassifies) before
        // the corrupt content lands on disk.
        {
            simdjson::ondemand::parser validateParser;
            simdjson::padded_string padded(fileContent);
            auto validateDoc = validateParser.iterate(padded);
            auto interfacesField = validateDoc["API interfaces"].get_array();
            if (interfacesField.error() != simdjson::SUCCESS)
            {
                LOG_APP_ERROR("WebServer::HandleAiInterfacesSavePost: post-replacement validation failed "
                              "path='{}' reason=interfaces_field_unparsable",
                              configPath.string());
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "validation_failed";
                err["message"] = "Generated config.json did not re-parse cleanly; aborting write.";
                return MakeJsonResponse(500, err);
            }
            size_t reparsedCount = 0;
            for (auto element : interfacesField.value())
            {
                (void)element;
                ++reparsedCount;
            }
            if (reparsedCount != config.m_ApiInterfaces.size())
            {
                LOG_APP_ERROR("WebServer::HandleAiInterfacesSavePost: post-replacement count mismatch "
                              "path='{}' expected={} got={}",
                              configPath.string(), config.m_ApiInterfaces.size(), reparsedCount);
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "validation_failed";
                err["message"] = "Generated config.json had wrong interface count; aborting write.";
                return MakeJsonResponse(500, err);
            }
        }

        // Atomic write — tmp-file + rename.  A failed / partial write previously
        // truncated config.json (non-atomic write would corrupt the file on disk-
        // full / SIGKILL mid-write).  WriteTextFileAtomic returns false without
        // touching the target file on any failure.
        std::string writeError;
        if (!WriteTextFileAtomic(configPath, fileContent, writeError))
        {
            LOG_APP_ERROR("WebServer::HandleAiInterfacesSavePost: atomic write failed path='{}' reason='{}'",
                          configPath.string(), writeError);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "write_failed";
            err["message"] = "Failed to write '" + configPath.string() + "': " + writeError;
            return MakeJsonResponse(500, err);
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

        // Test path lives on AiRequestPool — available in both editions.  Engine
        // admins use it via the dashboard's Test button for operational
        // verification of provider config; Studio uses it via the same route
        // plus the assistant-side generation pipeline.
        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
        AiRequestPool* pool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;
        bool const ok = (pool != nullptr) &&
                        pool->TestInterface(static_cast<size_t>(index), responsePreview, error, latencyMs);
        if (pool == nullptr)
        {
            error = "ai_request_pool_unavailable";
        }

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
                // Replace a top-level scalar field value in-place.  Object-depth-aware
                // so a key that also appears inside a nested object (e.g. inside the
                // "API interfaces" array elements) does not collide with the same
                // top-level key — a naive find() across the whole document would
                // collide with the same key name nested inside array elements.  Only
                // matches at object depth 1 (immediately inside the root `{`).
                auto replaceField = [&](std::string const& key, std::string const& newValue)
                {
                    std::string const searchKey = "\"" + key + "\"";
                    int objectDepth = 0;
                    bool insideRootObject = false;
                    size_t i = 0;
                    while (i < fileContent.size())
                    {
                        char const c = fileContent[i];
                        if (c == '"')
                        {
                            // Walk to the closing quote, honouring escapes.
                            size_t const stringStart = i;
                            ++i;
                            while (i < fileContent.size() && fileContent[i] != '"')
                            {
                                if (fileContent[i] == '\\' && i + 1 < fileContent.size())
                                    ++i;
                                ++i;
                            }
                            if (i >= fileContent.size())
                                return;
                            size_t const stringLen = i - stringStart + 1;
                            if (insideRootObject && objectDepth == 1 && stringLen == searchKey.size() &&
                                fileContent.compare(stringStart, stringLen, searchKey) == 0)
                            {
                                // Found a top-level key match.  Scan past whitespace to the colon.
                                size_t colonPos = i + 1;
                                while (colonPos < fileContent.size() &&
                                       (fileContent[colonPos] == ' ' || fileContent[colonPos] == '\t'))
                                    ++colonPos;
                                if (colonPos >= fileContent.size() || fileContent[colonPos] != ':')
                                {
                                    ++i;
                                    continue;
                                }
                                size_t valStart = colonPos + 1;
                                while (valStart < fileContent.size() &&
                                       (fileContent[valStart] == ' ' || fileContent[valStart] == '\t'))
                                    ++valStart;
                                size_t valEnd = valStart;
                                if (valEnd < fileContent.size() && fileContent[valEnd] == '"')
                                {
                                    ++valEnd;
                                    while (valEnd < fileContent.size() && fileContent[valEnd] != '"')
                                    {
                                        if (fileContent[valEnd] == '\\' && valEnd + 1 < fileContent.size())
                                            ++valEnd;
                                        ++valEnd;
                                    }
                                    if (valEnd < fileContent.size())
                                        ++valEnd;
                                }
                                else
                                {
                                    while (valEnd < fileContent.size() && fileContent[valEnd] != ',' &&
                                           fileContent[valEnd] != '\n' && fileContent[valEnd] != '\r' &&
                                           fileContent[valEnd] != '}')
                                        ++valEnd;
                                    while (valEnd > valStart &&
                                           (fileContent[valEnd - 1] == ' ' || fileContent[valEnd - 1] == '\t'))
                                        --valEnd;
                                }
                                fileContent.replace(valStart, valEnd - valStart, newValue);
                                return;
                            }
                            ++i;
                            continue;
                        }
                        if (c == '{')
                        {
                            ++objectDepth;
                            if (objectDepth == 1)
                                insideRootObject = true;
                        }
                        else if (c == '}')
                        {
                            --objectDepth;
                        }
                        ++i;
                    }
                };

                replaceField("API index", std::to_string(config.m_ApiIndex));
                replaceField("max threads", std::to_string(config.m_MaxThreads));
                replaceField("verbose", config.m_Verbose ? "true" : "false");
                replaceField("max file size in kB", std::to_string(config.m_MaxFileSizekB));
                replaceField("jcwf batch size", std::to_string(config.m_JcwfBatchSize));
                replaceField("jcwf AI interface", std::to_string(config.m_JcwfAiInterfaceIndex));
                replaceField("use_bash", config.m_UseBashOnWindows ? "true" : "false");

                // Tripwire: confirm the patched text re-parses as valid JSON before
                // it lands on disk.  Catches any future replaceField bug that would
                // otherwise corrupt config.json silently.
                {
                    simdjson::ondemand::parser validateParser;
                    simdjson::padded_string padded(fileContent);
                    auto validateDoc = validateParser.iterate(padded);
                    if (validateDoc.error() != simdjson::SUCCESS)
                    {
                        LOG_APP_ERROR("WebServer::HandleConfigSettingsPut: post-replacement parse failed "
                                      "path='{}' reason=document_unparsable",
                                      configPath.string());
                        crow::json::wvalue err;
                        err["ok"] = false;
                        err["error"] = "validation_failed";
                        err["message"] = "Generated config.json did not re-parse cleanly; aborting write.";
                        return MakeJsonResponse(500, err);
                    }
                    // Accessing one field forces real parse work; any structural break
                    // (e.g. a clobbered closing brace) surfaces here rather than silently
                    // producing a half-broken file.
                    auto rootCheck = validateDoc.get_object();
                    if (rootCheck.error() != simdjson::SUCCESS)
                    {
                        LOG_APP_ERROR("WebServer::HandleConfigSettingsPut: post-replacement root-not-object "
                                      "path='{}'",
                                      configPath.string());
                        crow::json::wvalue err;
                        err["ok"] = false;
                        err["error"] = "validation_failed";
                        err["message"] = "Generated config.json root is not an object; aborting write.";
                        return MakeJsonResponse(500, err);
                    }
                }

                // Atomic write — tmp-file + rename.  Disk-full / SIGKILL during a
                // non-atomic write would leave a truncated config.json on disk.
                std::string writeError;
                if (!WriteTextFileAtomic(configPath, fileContent, writeError))
                {
                    LOG_APP_ERROR("WebServer::HandleConfigSettingsPut: atomic write failed path='{}' reason='{}'",
                                  configPath.string(), writeError);
                    crow::json::wvalue err;
                    err["ok"] = false;
                    err["error"] = "write_failed";
                    err["message"] = "Failed to write '" + configPath.string() + "': " + writeError;
                    return MakeJsonResponse(500, err);
                }
                LOG_CORE_INFO("WebServer: saved config settings to '{}'", configPath.string());
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
        // Pre-auth rate limit on the source IP — this endpoint is intentionally
        // unauthenticated (the master password IS the credential), so without a
        // throttle an attacker could brute-force the password against the live
        // API.  The pre-auth bucket is sized for legitimate operator traffic
        // (a handful of attempts, then login).
        if (IsRateLimited(RateLimitTier::PreAuth, req.remote_ip_address))
        {
            LOG_SECURITY_WARN("[security] rate_limited_preauth ip={} endpoint=keys_unlock",
                              req.remote_ip_address);
            return MakeAuthErrorResponse("rate_limited");
        }

        // Body cap before any parsing.  The body carries only a master password
        // string; 16 KB is generous slack and bounds malformed-body memory use.
        if (IsBodyTooLarge(req, 1))
        {
            return MakePayloadTooLargeResponse(1);
        }

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
                // Record the failure so repeated wrong passwords trigger the
                // standard auth-failure lockout (kMaxAuthFailures within
                // kAuthFailureWindow), and log at SECURITY level so the
                // dashboard's run analyser surfaces brute-force attempts.
                RecordAuthFailure(req.remote_ip_address);
                LOG_SECURITY_WARN("[security] keys_unlock_wrong_password ip={}",
                                  req.remote_ip_address);
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
            crow::json::wvalue entry;
            bool const found = keyManager.WithCredential(name,
                [&](ICredential const& cred)
                {
                    entry["name"] = name;
                    entry["display_name"]    = cred.m_DisplayName;
                    entry["endpoint"]        = cred.m_Endpoint;
                    entry["default_model"]   = cred.m_DefaultModel;
                    entry["api_type"]        = cred.m_ApiType;
                    entry["credential_type"] = std::string(cred.GetType());

                    // Per-type secret-presence indicators (the secret values themselves NEVER cross
                    // the network — only "set / not set" booleans).  Each branch handles one subtype
                    // exhaustively; an unhandled subtype emits no `has_key` indicator (UI will show
                    // "no key").  Adding a new subtype requires a new branch — visible omission.
                    if (auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred))
                    {
                        entry["has_key"] = !api->m_ApiKey.IsEmpty();
                    }
                    else if (auto const* oauth = dynamic_cast<OAuthCredential const*>(&cred))
                    {
                        entry["has_key"]            = !oauth->m_AccessToken.IsEmpty();
                        entry["has_refresh_token"]  = !oauth->m_RefreshToken.IsEmpty();
                        entry["expires_at"]         = oauth->m_ExpiresAt;
                        entry["scopes"]             = oauth->m_Scopes;
                    }
                    else if (auto const* kp = dynamic_cast<KeyPairCredential const*>(&cred))
                    {
                        entry["has_key"] = !kp->m_PrivateKeyPem.IsEmpty();
                    }
                    else if (auto const* basic = dynamic_cast<BasicAuthCredential const*>(&cred))
                    {
                        entry["has_key"]  = !basic->m_Password.IsEmpty();
                        entry["username"] = basic->m_Username;
                    }
                    else if (auto const* aws = dynamic_cast<AwsCredential const*>(&cred))
                    {
                        entry["has_key"]               = !aws->m_AccessKeyId.empty();
                        entry["has_secret_access_key"] = !aws->m_SecretAccessKey.IsEmpty();
                        entry["has_session_token"]    = !aws->m_SessionToken.IsEmpty();
                        if (!aws->m_Region.empty())
                        {
                            entry["region"] = aws->m_Region;
                        }
                    }

                    // Non-secret params — defense-in-depth strip for any future subtype that puts
                    // secrets in m_Params (the SecureString-typed subtypes don't, but the strip
                    // keeps the contract stable).  AWS secret_access_key / session_token must never
                    // leave the server.
                    if (!cred.m_Params.empty())
                    {
                        static std::array<char const*, 2> const kSensitiveParamKeys = {
                            "secret_access_key", "session_token"};
                        crow::json::wvalue paramsJson;
                        for (auto const& [k, v] : cred.m_Params)
                        {
                            bool sensitive = false;
                            for (auto const* skey : kSensitiveParamKeys)
                            {
                                if (k == skey) { sensitive = true; break; }
                            }
                            if (sensitive)
                            {
                                entry[std::string("has_") + k] = !v.empty();
                            }
                            else
                            {
                                paramsJson[k] = v;
                            }
                        }
                        entry["params"] = std::move(paramsJson);
                    }
                });
            if (!found)
            {
                continue;
            }

            // Secret values (api_key, refresh_token, client_secret, private_key_pem,
            // password, secret_access_key, session_token) are intentionally NOT returned.
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

            // Extract the top-level object FIRST — simdjson's ondemand iterator is
            // forward-only and stateful, so reading any `doc["key"]` before this
            // would advance the iterator past the opening `{` and break get_object().
            simdjson::ondemand::object docObj;
            if (doc.get_object().get(docObj) != simdjson::SUCCESS)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "invalid_json";
                err["message"] = "Request body must be a JSON object";
                return MakeJsonResponse(400, err);
            }

            std::string_view name;
            if (docObj["name"].get_string().get(name) != simdjson::SUCCESS || name.empty())
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "missing_name";
                err["message"] = "'name' is required and must be a non-empty string";
                return MakeJsonResponse(400, err);
            }

            // CredentialFactory::CreateFromJson dispatches on `credential_type` (defaults
            // to "api_key") and reads only the fields appropriate for the chosen subtype.
            // Same code path that loads keys.json.enc — the request body shape is the
            // same as the per-provider object in the encrypted store.
            std::unique_ptr<ICredential> cred = CredentialFactory::CreateFromJson(docObj);
            if (!cred)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "invalid_credential_type";
                err["message"] = "Unknown or unsupported credential_type";
                return MakeJsonResponse(400, err);
            }

            auto& keyManager = Core::g_Core->GetKeyManager();
            if (!keyManager.AddCredential(std::string(name), std::move(cred)))
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

        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string json(req.body);
            auto doc = parser.iterate(json);

            simdjson::ondemand::object patchObj;
            if (doc.get_object().get(patchObj) != simdjson::SUCCESS)
            {
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "invalid_json";
                err["message"] = "Request body must be a JSON object";
                return MakeJsonResponse(400, err);
            }

            // Atomic read-modify-write under unique_lock — no window between read
            // and write where a concurrent RemoveProvider could leave `existing`
            // dangling.  Mutator builds the patched credential; ModifyCredential
            // returns false if the provider doesn't exist or the mutator returns
            // nullptr (clone failed).
            bool cloneFailed = false;
            bool const ok = keyManager.ModifyCredential(providerName,
                [&](ICredential const& existing) -> std::unique_ptr<ICredential>
                {
                    // CloneAndPatch builds a new same-subtype credential, copying all of
                    // `existing`'s fields, then overlays the optional patch keys.  Note:
                    // `credential_type` in the patch is intentionally ignored — to change
                    // a credential's type, DELETE + CREATE.
                    auto updated = CredentialFactory::CloneAndPatch(existing, patchObj);
                    if (!updated)
                    {
                        cloneFailed = true;
                    }
                    return updated;
                });

            if (!ok)
            {
                if (cloneFailed)
                {
                    crow::json::wvalue err;
                    err["ok"] = false;
                    err["error"] = "internal_error";
                    err["message"] = "Failed to clone existing credential for update";
                    return MakeJsonResponse(500, err);
                }
                crow::json::wvalue err;
                err["ok"] = false;
                err["error"] = "not_found";
                err["message"] = "Provider '" + providerName + "' not found";
                return MakeJsonResponse(404, err);
            }

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

        // If the deleted provider had OAuth tokens cached in OAuthTokenManager, drop
        // them so the in-memory state stays in sync with the on-disk state we just
        // mutated.  RemoveTokens is idempotent — no-op for non-OAuth providers.
        Core::g_Core->GetOAuthTokenManager().RemoveTokens(providerName);

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleProviderSetDefaultPost(std::string const& providerName)
    {
        auto& keyManager = Core::g_Core->GetKeyManager();

        if (!keyManager.HasCredential(providerName))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Provider '" + providerName + "' not found";
            return MakeJsonResponse(404, err);
        }

        if (!keyManager.SetDefaultProvider(providerName))
        {
            // HasCredential confirmed above, so a false return here means
            // the credential was removed in the TOCTOU window — race rather
            // than caller error.  Surface as 409 so the client can retry.
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "race_lost";
            err["message"] = "Provider was removed before default could be set; retry";
            return MakeJsonResponse(409, err);
        }

        crow::json::wvalue responseJson;
        responseJson["ok"] = true;
        responseJson["default_provider"] = providerName;
        return MakeJsonResponse(200, responseJson);
    }

    crow::response WebServer::HandleProvidersSavePost(crow::request const& req)
    {
        auto& keyManager = Core::g_Core->GetKeyManager();

        // Master password: request body or the password already held in mlock
        // memory from a prior unlock.  There is no env-var fallback — see
        // doc/cyber security.md §"Master password after restart".
        std::string bodyPassword;
        {
            simdjson::ondemand::parser parser;
            try
            {
                simdjson::padded_string json(req.body);
                auto doc = parser.iterate(json);

                std::string_view sv;
                if (doc["master_password"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    bodyPassword = std::string(sv);
                }
            }
            catch (...)
            {
                // Body might be empty or not JSON — fall through to the held master password.
            }
        }

        std::filesystem::path const keysFilePath =
            Core::g_Core->GetLaunchCWDAbsolute() / Core::g_Core->GetConfig().m_KeysFilePath;

        auto doSave = [&](std::string_view password) -> crow::response
        {
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
                        std::string decrypted = KeyEncryption::Decrypt(existingBlob, password);
                        if (decrypted.empty())
                        {
                            crow::json::wvalue err;
                            err["ok"] = false;
                            err["error"] = "wrong_password";
                            err["message"] = "Incorrect master password. The password must match the one used to "
                                              "create the keys file.";
                            return MakeJsonResponse(403, err);
                        }
                    }
                }
            }

            if (!keyManager.Save(keysFilePath, password))
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
        };

        if (!bodyPassword.empty())
        {
            return doSave(bodyPassword);
        }

        crow::response heldResp;
        bool const hadHeld = keyManager.WithMasterPassword(
            [&](std::string_view masterPassword) { heldResp = doSave(masterPassword); });
        if (hadHeld)
        {
            return heldResp;
        }

        crow::json::wvalue err;
        err["ok"] = false;
        err["error"] = "no_password";
        err["message"] = "Master password required. Include it in the request body, or unlock the key "
                          "store first via POST /api/settings/keys/unlock.";
        return MakeJsonResponse(400, err);
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

        auto existing = connectionManager.GetConnection(connectionName);
        if (!existing)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_found";
            err["message"] = "Connection '" + connectionName + "' not found";
            return MakeJsonResponse(404, err);
        }

        // Start with existing config and overlay provided fields.  *existing returns the
        // CloudConnection by reference (the optional owns the bytes); the copy below
        // makes a private working copy that the request handler can mutate freely.
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

        auto connection = connectionManager.GetConnection(connectionName);
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

        auto result = connector->TestConnection(*connection);

        // Record the outcome on the circuit breaker so the dashboard Cloud LED
        // lights up as soon as a Test button (not just a JCWF cloud task) has
        // proved a connection works end-to-end.  The typed `ConnectorErrorCode`
        // on failure is stored on the breaker and surfaced via
        // `GetHealthSummary().m_LastFailureCode` for the dashboard.
        auto& circuitBreaker = Core::g_Core->GetCloudCircuitBreaker();
        if (result)
        {
            circuitBreaker.RecordSuccess(connectionName);
            crow::json::wvalue responseJson;
            responseJson["ok"] = true;
            return MakeJsonResponse(200, responseJson);
        }

        circuitBreaker.RecordFailure(connectionName, result.error().m_Code);

        crow::json::wvalue responseJson;
        responseJson["ok"] = false;
        responseJson["error"] = "test_failed";
        responseJson["code"] = std::string(Describe(result.error().m_Code));
        responseJson["message"] = result.error().m_Details;
        return MakeJsonResponse(400, responseJson);
    }

    crow::response WebServer::HandleConnectionsSavePost()
    {
        auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

        std::string json = connectionManager.SerializeToJson();

        // Save to connections.json in the launch directory.  Atomic write
        // (tmp-file + rename) so a partial / failed write never leaves the
        // existing connections.json truncated or empty.
        std::filesystem::path const connectionsFilePath =
            Core::g_Core->GetLaunchCWDAbsolute() / "connections.json";

        std::string writeError;
        if (!WriteTextFileAtomic(connectionsFilePath, json, writeError))
        {
            LOG_APP_ERROR("WebServer::HandleConnectionsSavePost: atomic write failed path='{}' reason='{}'",
                          connectionsFilePath.string(), writeError);
            LOG_SECURITY_WARN("[security] connections_save_failed path={} reason={}",
                              connectionsFilePath.string(), writeError);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "save_failed";
            err["message"] = "Failed to write '" + connectionsFilePath.string() + "': " + writeError;
            return MakeJsonResponse(500, err);
        }

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

        auto connection = connectionManager.GetConnection(connectionName);
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

        auto connection = connectionManager.GetConnection(connectionName);
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

        // Verify the OAuth provider's TLS certificate.  Set explicitly rather
        // than relying on libcurl's defaults, so a future change to the build
        // (or a libcurl rebuild with --without-ssl-verifypeer) cannot silently
        // open a MitM window.  CURLOPT_SSL_VERIFYPEER=1 enables CA validation;
        // CURLOPT_SSL_VERIFYHOST=2 enforces hostname match against the cert.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }
        else
        {
            // No project-managed CA bundle — libcurl falls back to the system
            // trust store, which is the desired behaviour on Linux distros and
            // macOS.  Logged once so an operator with a misconfigured build
            // (no system CA either) sees the early-warning signal.
            LOG_APP_INFO(
                "OAuth callback for '{}': using system CA bundle (CurlWrapper::GetCaBundlePath() empty)",
                connectionName);
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
        // clients so the background refresh loop can use it.  The three secret-bearing
        // inputs are threaded through SecureString locals so the bytes stay in mlock'd /
        // zero-on-destruct memory — no std::string heap allocation between the consent
        // response and OAuthTokenManager's in-memory TokenEntry.
        SecureString accessTokenSecure;
        SecureString refreshTokenSecure;
        SecureString clientSecretSecure;
        accessTokenSecure.Set(accessToken);
        if (!refreshToken.empty()) refreshTokenSecure.Set(refreshToken);
        if (providerInfo.m_RequiresClientSecret)
        {
            auto clientSecretIt = connection->m_Params.find("client_secret");
            if (clientSecretIt != connection->m_Params.end())
            {
                clientSecretSecure.Set(clientSecretIt->second);
            }
        }
        auto& oauthManager = Core::g_Core->GetOAuthTokenManager();
        oauthManager.StoreTokens(connection->m_KeyName, accessTokenSecure, refreshTokenSecure,
                                 expiresIn, tokenUrl, clientId, clientSecretSecure);

        // Persist refresh_token + OAuth app config to the encrypted keys file so tokens
        // survive a restart.  The access_token itself is short-lived and is NOT persisted;
        // on startup the OAuthTokenManager hydrates from the refresh_token and fetches a
        // fresh access_token.
        {
            auto& keyManager = Core::g_Core->GetKeyManager();

            // Atomic add-or-update under one unique_lock — no window between the read
            // (preserve existing metadata) and the write (store new OAuth fields)
            // where a concurrent RemoveProvider could leave `existing` dangling, and
            // no window where AddCredential could fail because another thread inserted
            // the same name first.
            //
            // Builder receives `existing` (possibly null).  When non-null we preserve
            // non-OAuth metadata (display_name, endpoint, default_model, api_type,
            // params) — useful when the user edited those before authorising.  The
            // OAuth-specific fields are always rewritten to the freshly-issued values.
            //
            // Auto-create on null `existing`: without this, the OAuth callback would
            // bail and tokens would remain in OAuthTokenManager memory only — lost on
            // the next j9t restart.  Matches the UX where a user creates an OAuth
            // connection in the editor and immediately clicks "Authorize" without
            // having to pre-create a same-named entry in the providers view.
            keyManager.UpsertCredential(connection->m_KeyName,
                [&](ICredential const* existing) -> std::unique_ptr<ICredential>
                {
                    auto cred = std::make_unique<OAuthCredential>();
                    if (existing)
                    {
                        cred->m_DisplayName  = existing->m_DisplayName;
                        cred->m_Endpoint     = existing->m_Endpoint;
                        cred->m_DefaultModel = existing->m_DefaultModel;
                        cred->m_ApiType      = existing->m_ApiType;
                        cred->m_Params       = existing->m_Params;
                    }
                    else
                    {
                        cred->m_DisplayName = connection->m_KeyName;
                        LOG_CORE_INFO("OAuth callback: auto-creating KeyManager provider '{}' "
                                      "for connection '{}'",
                                      connection->m_KeyName, connectionName);
                    }
                    // Access token is short-lived — OAuthTokenManager hydrates a fresh
                    // one on the next request from the persisted refresh token, so we
                    // don't write it here.
                    cred->m_RefreshToken.Set(refreshToken);
                    if (!clientSecretSecure.IsEmpty())
                    {
                        cred->m_ClientSecret.Set(clientSecretSecure.Get());
                    }
                    cred->m_ExpiresAt = static_cast<int64_t>(std::time(nullptr)) + expiresIn;
                    cred->m_Scopes = connection->m_Params.count("scopes")
                                         ? connection->m_Params.at("scopes")
                                         : providerInfo.m_DefaultScopes;
                    cred->m_TokenEndpoint = tokenUrl;
                    cred->m_ClientId      = clientId;
                    return cred;
                });

            // Persist OAuth tokens if the key store has been unlocked this session.
            // If the admin never unlocked (/api/settings/keys/unlock), the tokens stay
            // in memory only and the user gets a warning below. No env-var fallback.
            auto const& keysPath = keyManager.GetKeysFilePath();
            bool const persisted = !keysPath.empty() && keyManager.WithMasterPassword(
                [&](std::string_view masterPassword)
                {
                    if (keyManager.Save(keysPath, masterPassword))
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
                });
            if (!persisted)
            {
                LOG_CORE_WARN("OAuth callback: master password not held in mlock memory or keys file path "
                              "not set — refresh_token for '{}' held in process memory only and will be "
                              "lost on restart",
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
        bool saved = false;
        bool const hadPassword = keyManager.WithMasterPassword(
            [this, &saved](std::string_view pwd) { saved = m_McpKeyManager.Save(m_McpKeysFilePath, pwd); });
        if (!hadPassword)
        {
            LOG_CORE_WARN("SaveMcpKeyStore: master password not held in mlock memory — cannot persist");
            return false;
        }
        return saved;
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
        // already exist on disk under scripts/.  This is the hard security boundary on
        // adhoc submission (see `doc/cyber security.md` "Adhoc workflow submission").
        // Runs through the top-level tasks object only — sub-workflow canvases inside
        // an adhoc submission are not supported today.
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
        EnqueueRunResult const adhocEnqueueResult =
            runtime->EnqueueWorkflowRunWithContextAndGetRunId(result.m_WorkflowId, result.m_RunId, runtimeContext);
        if (auto errorResponse = MaybeEnqueueErrorResponse(adhocEnqueueResult, "POST /api/adhoc/submit",
                                                           result.m_WorkflowId))
        {
            return std::move(*errorResponse);
        }

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
        // Authorize on the actual user identity (m_User), not on the derived
        // slug.  The slug is a filesystem-naming primitive — distinct users
        // are guaranteed to land in distinct slug dirs (via the SHA-256
        // suffix in SanitizeUserSlug), but the comparison primitive for
        // "does this caller own this run" is the user string itself.  Legacy
        // runs written before the hash suffix existed remain reachable
        // because m_User is the same string the caller authenticated as.
        if (!isAdmin && auth.m_User != info->m_User)
        {
            LOG_SECURITY_WARN("[security] run_files_denied reason=not_owner caller={} owner={} runId={}",
                              auth.m_User, info->m_User, runId);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_owner";
            err["message"] = "This run belongs to another user.";
            return MakeJsonResponse(403, err);
        }
        if (isAdmin && auth.m_User != info->m_User)
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
        // Authz on m_User (not slug) — see HandleRunFilesListGet for rationale.
        if (!isAdmin && auth.m_User != info->m_User)
        {
            LOG_SECURITY_WARN("[security] run_file_denied reason=not_owner caller={} owner={} runId={} path={}",
                              auth.m_User, info->m_User, runId, relPath);
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "not_owner";
            err["message"] = "This run belongs to another user.";
            return MakeJsonResponse(403, err);
        }
        if (isAdmin && auth.m_User != info->m_User)
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

            // Diagnostic counters for the dashboard live-update queue path —
            // a flatlining snapshot counter while completions arrive means a
            // producer-side bug; the per-type counters narrow it down.
            signals["websocket_total_broadcasts_enqueued"] =
                static_cast<int64_t>(m_WsTotalBroadcastsEnqueued);
            signals["websocket_total_runs_snapshots_enqueued"] =
                static_cast<int64_t>(m_WsTotalRunsSnapshotsEnqueued);
            signals["websocket_total_last_runs_snapshots_enqueued"] =
                static_cast<int64_t>(m_WsTotalLastRunsSnapshotsEnqueued);
            signals["websocket_total_ai_call_events_enqueued"] =
                static_cast<int64_t>(m_WsTotalAiCallEventsEnqueued);
            signals["websocket_total_python_status_enqueued"] =
                static_cast<int64_t>(m_WsTotalPythonStatusEnqueued);
            signals["websocket_total_log_batches_enqueued"] =
                static_cast<int64_t>(m_WsTotalLogBatchesEnqueued);
            signals["websocket_total_drains"] = static_cast<int64_t>(m_WsTotalDrains);
            signals["websocket_last_drain_bytes"] = static_cast<int64_t>(m_WsLastDrainBytes);
            signals["websocket_last_drain_messages"] = static_cast<int64_t>(m_WsLastDrainMessages);
            signals["websocket_last_drain_duration_us"] =
                static_cast<int64_t>(m_WsLastDrainDurationUs);
            signals["websocket_peak_drain_bytes"] = static_cast<int64_t>(m_WsPeakDrainBytes);
            signals["websocket_peak_drain_duration_us"] =
                static_cast<int64_t>(m_WsPeakDrainDurationUs);
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
            signals["rate_limit_buckets_preauth"] = static_cast<int64_t>(m_PreAuthBuckets.size());
            signals["rate_limit_buckets_authenticated"] = static_cast<int64_t>(m_AuthenticatedBuckets.size());
            signals["auth_failure_records"] = static_cast<int64_t>(m_AuthFailures.size());
        }

        // ---- Cloud surface security counters ----
        // Lifetime totals from each cloud-surface security gate.  Per-instance
        // forensic detail (timestamps, task/run/connection identifiers,
        // rejected values) is in the security log; these counters answer the
        // "is this gate firing at all?" question without grepping the log.
        // See connectorHttp.h docstring on each Get*RejectionCount accessor
        // for the gate's responsibility.
        signals["cloud_dns_resolved_ip_rejections"] =
            static_cast<int64_t>(ConnectorHttp::GetDnsResolvedIpRejectionCount());
        signals["cloud_endpoint_ssrf_rejections"] =
            static_cast<int64_t>(ConnectorHttp::GetEndpointSsrfRejectionCount());
        signals["cloud_credential_crlf_rejections"] =
            static_cast<int64_t>(ConnectorHttp::GetCredentialCrlfRejectionCount());
        signals["cloud_input_validation_rejections"] =
            static_cast<int64_t>(ConnectorHttp::GetInputValidationRejectionCount());
        signals["cloud_postgres_invalid_sslmode_rejections"] =
            static_cast<int64_t>(ConnectorHttp::GetPostgresInvalidSslmodeRejectionCount());
        signals["cloud_postgres_forbidden_param_rejections"] =
            static_cast<int64_t>(ConnectorHttp::GetPostgresForbiddenParamRejectionCount());

        // ---- AI-interface plain-HTTP policy ----
        // url_policy_rejections fires for any non-loopback http:// or
        // disallowed-scheme URL at config-load OR REST POST/PUT.
        // credentialed_plaintext_http_rejections fires only for the http:// +
        // key_name combo — separate counter because that pattern indicates an
        // operator configured a credential they intended to use, which is the
        // higher-priority operational signal.
        signals["url_policy_rejections"] =
            static_cast<int64_t>(UrlPolicy::GetUrlPolicyRejectionCount());
        signals["credentialed_plaintext_http_rejections"] =
            static_cast<int64_t>(UrlPolicy::GetCredentialedPlaintextHttpRejectionCount());

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
        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
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

            // ---- HTTP dispatcher (throttle gate + retry queue) ----
            CurlMultiDispatcher* dispatcher = app->GetCurlMultiDispatcher();
            if (dispatcher != nullptr)
            {
                auto snap = dispatcher->GetDebugSnapshot();
                signals["dispatcher_total_dispatched"]        = static_cast<int64_t>(snap.m_TotalDispatched);
                signals["dispatcher_total_throttled"]         = static_cast<int64_t>(snap.m_TotalThrottled);
                signals["dispatcher_total_429s"]              = static_cast<int64_t>(snap.m_Total429s);
                signals["dispatcher_total_retries_exhausted"] = static_cast<int64_t>(snap.m_TotalRetriesExhausted);
                signals["dispatcher_total_completed"]         = static_cast<int64_t>(snap.m_TotalCompleted);
                signals["dispatcher_total_cancelled"]         = static_cast<int64_t>(snap.m_TotalCancelled);
                signals["dispatcher_inbox_size"]              = static_cast<int64_t>(snap.m_InboxSize);
                signals["dispatcher_active_count"]            = static_cast<int64_t>(snap.m_ActiveCount);
                signals["dispatcher_retry_queue_size"]        = static_cast<int64_t>(snap.m_RetryQueueSize);
                // CURLOPT_TCP_KEEPALIVE is set unconditionally on every easy
                // handle in CurlMultiDispatcher::SetupEasyHandle.  Surfaced
                // as a flag so test_tcp_keepalive_set.py can assert the
                // policy without poking at libcurl internals.
                signals["dispatcher_keepalive_enabled"]       = true;

                crow::json::wvalue::list hosts;
                hosts.reserve(snap.m_Hosts.size());
                for (auto const& h : snap.m_Hosts)
                {
                    crow::json::wvalue host;
                    host["host"]               = h.m_Host;
                    host["remaining_requests"] = static_cast<int64_t>(h.m_RemainingRequests);
                    host["remaining_tokens"]   = static_cast<int64_t>(h.m_RemainingTokens);
                    host["req_reset_in_sec"]   = static_cast<int64_t>(h.m_ReqResetInSec);
                    host["tok_reset_in_sec"]   = static_cast<int64_t>(h.m_TokResetInSec);
                    host["active_count"]       = static_cast<int64_t>(h.m_ActiveCount);
                    hosts.push_back(std::move(host));
                }
                signals["dispatcher_hosts"] = std::move(hosts);

                crow::json::wvalue::list controllers;
                controllers.reserve(snap.m_Controllers.size());
                for (auto const& c : snap.m_Controllers)
                {
                    crow::json::wvalue controller;
                    controller["quota_key"]                     = c.m_QuotaKey;
                    controller["current_concurrency_cap"]       = c.m_CurrentConcurrencyCap;
                    controller["streak_since_last_429"]         = c.m_StreakSinceLast429;
                    controller["remaining_requests"]            = static_cast<int64_t>(c.m_RemainingRequests);
                    controller["remaining_tokens"]              = static_cast<int64_t>(c.m_RemainingTokens);
                    controller["req_reset_in_sec"]              = static_cast<int64_t>(c.m_ReqResetInSec);
                    controller["tok_reset_in_sec"]              = static_cast<int64_t>(c.m_TokResetInSec);
                    controller["last_consumed_input_tokens"]    = static_cast<int64_t>(c.m_LastConsumedInputTokens);
                    controller["last_consumed_output_tokens"]   = static_cast<int64_t>(c.m_LastConsumedOutputTokens);
                    controllers.push_back(std::move(controller));
                }
                signals["dispatcher_controllers"] = std::move(controllers);
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

        // ---- MockTransport signing captures ----
        // Most-recent signing outputs from mock dispatches.  Surfaces the
        // Authorization (and any sibling auth headers) the signer emitted for
        // each request routed through MockTransport — consumed by hermetic
        // signature KAT tests like test/dispatch/test_bedrock_sigv4.py.
        // Snapshot semantics: oldest first, capped at
        // MockTransport::kMaxCapturedSignatures (FIFO evicted past that).
        {
            crow::json::wvalue::list captures;
            for (auto const& cap : MockTransport::GetRecentCapturedSignatures())
            {
                crow::json::wvalue entry;
                entry["cancel_key"] = cap.m_CancelKey;
                entry["quota_key"]  = cap.m_QuotaKey;
                crow::json::wvalue::list headers;
                headers.reserve(cap.m_Headers.size());
                for (auto const& h : cap.m_Headers)
                {
                    headers.push_back(crow::json::wvalue(h));
                }
                entry["headers"] = std::move(headers);
                captures.push_back(std::move(entry));
            }
            signals["last_mock_signatures"] = std::move(captures);
        }

        body["signals"] = std::move(signals);
        return MakeJsonResponse(200, body);
    }

    crow::response WebServer::HandleParseRateLimitHeadersPost(crow::request const& req)
    {
        // Parse JSON body. Mandatory: interface_type. Optional everything else.
        std::string interfaceTypeStr;
        std::string model;
        std::string headerBuffer;
        std::string responseBody;
        int httpStatus = 200;
        try
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(req.body);
            auto doc = parser.iterate(padded);

            std::string_view sv;
            if (doc["interface_type"].get_string().get(sv) == simdjson::SUCCESS)
                interfaceTypeStr = std::string(sv);
            if (doc["model"].get_string().get(sv) == simdjson::SUCCESS)
                model = std::string(sv);
            if (doc["header_buffer"].get_string().get(sv) == simdjson::SUCCESS)
                headerBuffer = std::string(sv);
            if (doc["body"].get_string().get(sv) == simdjson::SUCCESS)
                responseBody = std::string(sv);
            int64_t status = 0;
            if (doc["http_status"].get_int64().get(status) == simdjson::SUCCESS)
                httpStatus = static_cast<int>(status);
        }
        catch (std::exception const& e)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = std::string("invalid JSON body: ") + e.what();
            return MakeJsonResponse(400, err);
        }

        if (interfaceTypeStr.empty())
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "interface_type is required";
            return MakeJsonResponse(400, err);
        }

        // Map interface_type string → enum, mirroring the configParser branch.
        ConfigParser::EngineConfig::InterfaceType interfaceType =
            ConfigParser::EngineConfig::InterfaceType::InvalidAPI;
        if (interfaceTypeStr == "API1")       interfaceType = ConfigParser::EngineConfig::InterfaceType::API1;
        else if (interfaceTypeStr == "API2")  interfaceType = ConfigParser::EngineConfig::InterfaceType::API2;
        else if (interfaceTypeStr == "API3")  interfaceType = ConfigParser::EngineConfig::InterfaceType::API3;
        else if (interfaceTypeStr == "API4")  interfaceType = ConfigParser::EngineConfig::InterfaceType::API4;
        else if (interfaceTypeStr == "API5")  interfaceType = ConfigParser::EngineConfig::InterfaceType::API5;
        else if (interfaceTypeStr == "API6")  interfaceType = ConfigParser::EngineConfig::InterfaceType::API6;
        else
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = std::string("unknown interface_type '") + interfaceTypeStr + "'";
            return MakeJsonResponse(400, err);
        }

        // Drive the strategy through its pure interface — no I/O, no controller
        // state mutation. The same call path the dispatcher takes, just exposed
        // for hermetic testing.
        IRateLimitStrategy const& strategy = IRateLimitStrategy::Get(interfaceType);
        RateLimitObservation observation = strategy.Parse(headerBuffer, responseBody, httpStatus);

        // Convert steady_clock::time_point resets to "seconds-from-now" — same
        // shape the dispatcher_controllers debug rollup uses. Negative or
        // already-elapsed values stay observable; -1 means "not present".
        auto const now = std::chrono::steady_clock::now();
        auto resetSecsFromNow = [&now](std::optional<std::chrono::steady_clock::time_point> const& tp) -> int64_t
        {
            if (!tp.has_value()) return -1;
            return std::chrono::duration_cast<std::chrono::seconds>(*tp - now).count();
        };

        crow::json::wvalue body;
        body["ok"] = true;
        body["quota_key"] = strategy.DeriveQuotaKey(model);
        body["initial_concurrency_probe"] = static_cast<int64_t>(strategy.InitialConcurrencyProbe());

        crow::json::wvalue obs;
        obs["is_empty"] = observation.IsEmpty();
        obs["remaining_requests"] = static_cast<int64_t>(observation.m_RemainingRequests);
        obs["remaining_input_tokens"] = static_cast<int64_t>(observation.m_RemainingInputTokens);
        obs["remaining_output_tokens"] = static_cast<int64_t>(observation.m_RemainingOutputTokens);
        obs["remaining_combined_tokens"] = static_cast<int64_t>(observation.m_RemainingCombinedTokens);
        obs["requests_reset_in_sec"] = resetSecsFromNow(observation.m_RequestsResetAt);
        obs["tokens_reset_in_sec"] = resetSecsFromNow(observation.m_TokensResetAt);
        obs["retry_after_ms"] = observation.m_RetryAfter.has_value()
            ? static_cast<int64_t>(observation.m_RetryAfter->count())
            : -1;
        obs["consumed_input_tokens"] = static_cast<int64_t>(observation.m_ConsumedInputTokens);
        obs["consumed_output_tokens"] = static_cast<int64_t>(observation.m_ConsumedOutputTokens);
        body["observation"] = std::move(obs);

        return MakeJsonResponse(200, body);
    }

    crow::response WebServer::HandleDebugBuildCallbackPayloadGet(crow::request const& req)
    {
        auto const* runIdParam = req.url_params.get("runId");
        if (runIdParam == nullptr || std::string(runIdParam).empty())
        {
            return MakeWorkflowJsonError(400, "missing_runid", "runId query parameter is required",
                                         "GET /api/debug/build-callback-payload");
        }
        std::string runId(runIdParam);

        bool includeOutputs = true;
        if (auto const* p = req.url_params.get("include_outputs"); p != nullptr)
        {
            std::string const v(p);
            if (v == "false" || v == "0" || v == "no" || v == "False" || v == "FALSE")
            {
                includeOutputs = false;
            }
        }

        WorkflowRuntimeManager* workflowRuntimeManager = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRuntimeManager = m_WorkflowRuntimeManager;
        }
        if (workflowRuntimeManager == nullptr)
        {
            return MakeWorkflowJsonError(501, "not_configured", "Workflow runtime manager not configured",
                                         "GET /api/debug/build-callback-payload");
        }

        WorkflowRun run;
        if (!workflowRuntimeManager->TryGetRunById(runId, run))
        {
            return MakeWorkflowJsonError(404, "run_not_found", "Run not found: " + runId,
                                         "GET /api/debug/build-callback-payload", runId);
        }

        std::string const payload = BuildCallbackPayload(run, includeOutputs);
        crow::response resp(200, payload);
        resp.set_header("Content-Type", "application/json");
        return resp;
    }

    crow::response WebServer::HandleDebugRecentSubmissionsGet()
    {
        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
        CurlMultiDispatcher* dispatcher = (app != nullptr) ? app->GetCurlMultiDispatcher() : nullptr;
        crow::json::wvalue body;
        body["ok"] = true;
        if (dispatcher == nullptr)
        {
            body["submissions"] = crow::json::wvalue::list{};
            return MakeJsonResponse(200, body);
        }

        auto const recent = dispatcher->GetRecentSubmissions(64);
        auto const now = std::chrono::steady_clock::now();
        crow::json::wvalue::list out;
        out.reserve(recent.size());
        for (auto const& s : recent)
        {
            crow::json::wvalue entry;
            entry["quota_key"]               = s.m_QuotaKey;
            entry["url"]                     = s.m_Url;
            entry["timeout_ms"]              = static_cast<int64_t>(s.m_TimeoutMs);
            entry["estimated_input_tokens"]  = static_cast<int64_t>(s.m_EstimatedInputTokens);
            entry["interface_type"]          = s.m_InterfaceType;
            entry["age_seconds"] = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now - s.m_SubmittedAt).count());
            out.push_back(std::move(entry));
        }
        body["submissions"] = std::move(out);
        return MakeJsonResponse(200, body);
    }

    crow::response WebServer::HandleDebugTestObserveIdempotentPost(crow::request const& req)
    {
        // Body shape:
        //   {
        //     "initial_concurrency_probe": int,
        //     "hard_cap": int,
        //     "observations": [
        //       {
        //         "remaining_requests": int (-1 = unknown),
        //         "remaining_input_tokens": int,
        //         "remaining_output_tokens": int,
        //         "remaining_combined_tokens": int,
        //         "requests_reset_in_sec": int (-1 = unknown),
        //         "tokens_reset_in_sec": int,
        //         "retry_after_ms": int,
        //         "consumed_input_tokens": int,
        //         "consumed_output_tokens": int,
        //         "was_429": bool
        //       },
        //       ...
        //     ]
        //   }
        int initialConcurrencyProbe = 1;
        int hardCap = 48;
        std::vector<std::pair<RateLimitObservation, bool>> observations;
        try
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(req.body);
            auto doc = parser.iterate(padded);

            int64_t i64 = 0;
            if (doc["initial_concurrency_probe"].get_int64().get(i64) == simdjson::SUCCESS)
                initialConcurrencyProbe = static_cast<int>(i64);
            if (doc["hard_cap"].get_int64().get(i64) == simdjson::SUCCESS)
                hardCap = static_cast<int>(i64);

            auto const now = std::chrono::steady_clock::now();
            auto obsArray = doc["observations"].get_array();
            if (obsArray.error() == simdjson::SUCCESS)
            {
                for (auto element : obsArray.value())
                {
                    RateLimitObservation observation;
                    bool was429 = false;
                    auto reader = element.get_object();
                    if (reader.error() != simdjson::SUCCESS) continue;
                    auto obj = reader.value();

                    auto readInt = [&](char const* key, int64_t& out) {
                        int64_t v = 0;
                        if (obj[key].get_int64().get(v) == simdjson::SUCCESS) { out = v; return true; }
                        return false;
                    };
                    int64_t v = 0;
                    if (readInt("remaining_requests", v))         observation.m_RemainingRequests = v;
                    if (readInt("remaining_input_tokens", v))     observation.m_RemainingInputTokens = v;
                    if (readInt("remaining_output_tokens", v))    observation.m_RemainingOutputTokens = v;
                    if (readInt("remaining_combined_tokens", v))  observation.m_RemainingCombinedTokens = v;
                    if (readInt("consumed_input_tokens", v))      observation.m_ConsumedInputTokens = v;
                    if (readInt("consumed_output_tokens", v))     observation.m_ConsumedOutputTokens = v;
                    if (readInt("requests_reset_in_sec", v) && v >= 0)
                        observation.m_RequestsResetAt = now + std::chrono::seconds(v);
                    if (readInt("tokens_reset_in_sec", v) && v >= 0)
                        observation.m_TokensResetAt = now + std::chrono::seconds(v);
                    if (readInt("retry_after_ms", v) && v >= 0)
                        observation.m_RetryAfter = std::chrono::milliseconds(v);
                    bool b = false;
                    if (obj["was_429"].get_bool().get(b) == simdjson::SUCCESS) was429 = b;

                    observations.emplace_back(std::move(observation), was429);
                }
            }
        }
        catch (std::exception const& e)
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = std::string("invalid JSON body: ") + e.what();
            return MakeJsonResponse(400, err);
        }

        // Apply observations in order to an ephemeral controller.
        RateLimitController controller(initialConcurrencyProbe, hardCap);
        for (auto const& [obs, was429] : observations)
        {
            controller.Observe(obs, was429);
        }

        // Read back state.  The test compares the post-state across two
        // different observation sequences that should be equivalent.
        auto const& last = controller.LastObservation();
        auto const now2 = std::chrono::steady_clock::now();
        auto resetSecsFromNow = [&now2](std::optional<std::chrono::steady_clock::time_point> const& tp) -> int64_t
        {
            if (!tp.has_value()) return -1;
            return std::chrono::duration_cast<std::chrono::seconds>(*tp - now2).count();
        };

        crow::json::wvalue body;
        body["ok"] = true;
        body["current_concurrency_cap"] = controller.CurrentConcurrencyCap();
        body["streak_since_last_429"]   = controller.StreakSinceLast429();

        crow::json::wvalue lastObs;
        lastObs["is_empty"]                  = last.IsEmpty();
        lastObs["remaining_requests"]        = static_cast<int64_t>(last.m_RemainingRequests);
        lastObs["remaining_input_tokens"]    = static_cast<int64_t>(last.m_RemainingInputTokens);
        lastObs["remaining_output_tokens"]   = static_cast<int64_t>(last.m_RemainingOutputTokens);
        lastObs["remaining_combined_tokens"] = static_cast<int64_t>(last.m_RemainingCombinedTokens);
        lastObs["requests_reset_in_sec"]     = resetSecsFromNow(last.m_RequestsResetAt);
        lastObs["tokens_reset_in_sec"]       = resetSecsFromNow(last.m_TokensResetAt);
        lastObs["retry_after_ms"]            = last.m_RetryAfter.has_value()
            ? static_cast<int64_t>(last.m_RetryAfter->count()) : -1;
        lastObs["consumed_input_tokens"]     = static_cast<int64_t>(last.m_ConsumedInputTokens);
        lastObs["consumed_output_tokens"]    = static_cast<int64_t>(last.m_ConsumedOutputTokens);
        body["last_observation"] = std::move(lastObs);
        return MakeJsonResponse(200, body);
    }

    crow::response WebServer::HandleDebugMockAiResponsePost(crow::request const& req)
    {
        // Query params drive the response shape.  Defaults: 200 OK, no delay,
        // 60s reset, no fixtures (returns "{}").
        int status = 200;
        int delay_ms = 0;
        int reset_in_sec = 60;
        std::string headerFixture;
        std::string bodyFixture;

        if (auto const* v = req.url_params.get("status")) status = std::atoi(v);
        if (auto const* v = req.url_params.get("delay_ms")) delay_ms = std::atoi(v);
        if (auto const* v = req.url_params.get("reset_in_sec")) reset_in_sec = std::atoi(v);
        if (auto const* v = req.url_params.get("header_fixture")) headerFixture = v;
        if (auto const* v = req.url_params.get("body_fixture")) bodyFixture = v;

        // Path-confinement: fixture names are basenames, not paths.  No
        // separators, no `..`, no leading dot.  Defends the file-load step
        // against URL-injected traversal even though this endpoint is debug-only.
        auto isSafeName = [](std::string const& n)
        {
            if (n.empty()) return false;
            if (n.find("..") != std::string::npos) return false;
            if (n.find('/')  != std::string::npos) return false;
            if (n.find('\\') != std::string::npos) return false;
            if (n.front() == '.') return false;
            return true;
        };

        if (!headerFixture.empty() && !isSafeName(headerFixture))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid header_fixture name";
            return MakeJsonResponse(400, err);
        }
        if (!bodyFixture.empty() && !isSafeName(bodyFixture))
        {
            crow::json::wvalue err;
            err["ok"] = false;
            err["error"] = "invalid body_fixture name";
            return MakeJsonResponse(400, err);
        }

        // Sleep first so timeout tests see the wire-time blow past
        // CURLOPT_TIMEOUT_MS before the response is even built.
        if (delay_ms > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }

        auto const fixturesRoot = Core::g_Core->GetLaunchCWDAbsolute() / "test" / "dispatch" / "fixtures";

        std::string responseBody = "{}";
        if (!bodyFixture.empty())
        {
            auto path = fixturesRoot / "responses" / (bodyFixture + ".json");
            std::ifstream f(path);
            if (f)
            {
                std::stringstream ss;
                ss << f.rdbuf();
                responseBody = ss.str();
            }
            else
            {
                LOG_APP_WARN("[mock-ai-response] body_fixture not found: {}", path.string());
            }
        }

        crow::response resp(status, responseBody);

        if (!headerFixture.empty())
        {
            auto path = fixturesRoot / "headers" / (headerFixture + ".txt");
            std::ifstream f(path);
            if (!f)
            {
                LOG_APP_WARN("[mock-ai-response] header_fixture not found: {}", path.string());
                resp.add_header("Content-Type", "application/json");
                return resp;
            }
            std::stringstream ss;
            ss << f.rdbuf();
            std::string headerText = ss.str();

            // Substitute {{RESET_AT_ISO}} with an ISO 8601 timestamp
            // reset_in_sec into the future.  Anthropic fixture uses this
            // pattern; OpenAI fixtures use literal "Ns" duration syntax
            // and don't need substitution.
            {
                std::time_t resetT = std::time(nullptr) + reset_in_sec;
                std::tm gmt{};
#ifdef _WIN32
                gmtime_s(&gmt, &resetT);
#else
                gmtime_r(&resetT, &gmt);
#endif
                char iso[32]{};
                std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &gmt);
                std::string const placeholder = "{{RESET_AT_ISO}}";
                std::string const isoStr(iso);
                size_t pos = 0;
                while ((pos = headerText.find(placeholder, pos)) != std::string::npos)
                {
                    headerText.replace(pos, placeholder.size(), isoStr);
                    pos += isoStr.size();
                }
            }

            // Parse line-by-line.  First line is the HTTP status line ("HTTP/1.1
            // 200 OK"); skip it — Crow sets the status from `resp.code`.  Each
            // subsequent "Key: Value" line becomes a response header.
            std::istringstream lines(headerText);
            std::string line;
            bool firstLine = true;
            bool contentTypeSet = false;
            while (std::getline(lines, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (firstLine) { firstLine = false; continue; }
                auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                if (!val.empty() && val.front() == ' ') val.erase(0, 1);
                resp.add_header(key, val);
                // Case-insensitive "content-type" check without locale tables.
                if (key.size() == 12)
                {
                    bool match = true;
                    static char const* const kCT = "content-type";
                    for (size_t i = 0; i < 12; ++i)
                    {
                        char const a = static_cast<char>(std::tolower(static_cast<unsigned char>(key[i])));
                        if (a != kCT[i]) { match = false; break; }
                    }
                    if (match) contentTypeSet = true;
                }
            }
            if (!contentTypeSet)
                resp.add_header("Content-Type", "application/json");
        }
        else
        {
            resp.add_header("Content-Type", "application/json");
        }

        return resp;
    }

    crow::response WebServer::HandleDebugResetDispatcherStatePost()
    {
        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
        CurlMultiDispatcher* dispatcher = (app != nullptr) ? app->GetCurlMultiDispatcher() : nullptr;
        crow::json::wvalue body;
        body["ok"] = true;
        if (dispatcher == nullptr)
        {
            body["reset"] = false;
            body["reason"] = "dispatcher unavailable";
            return MakeJsonResponse(200, body);
        }
        dispatcher->ResetTestState();
        body["reset"] = true;
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
