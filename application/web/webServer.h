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

#pragma once
#include "crow.h"
#include "auxiliary/threadPool.h"
#ifdef J9T_STUDIO
#include "web/aiJcwfService.h"
#include "assistant/assistantController.h"
#endif
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace AIAssistant
{
    class WorkflowRegistry;
    class WorkflowRuntimeManager;
    class TriggerEngine;
    class WebServer
    {
    public:
        WebServer();
        ~WebServer();

        bool Start();
        void Stop();
        void SignalStop();
        void WaitStop();

        // Optional pointers for Workflow Editor API (set by JarvisAgent during startup).
        // If not set, editor run-monitoring endpoints will return "not configured".
        void SetWorkflowRegistry(WorkflowRegistry* workflowRegistry);
        void SetWorkflowRuntimeManager(WorkflowRuntimeManager* workflowRuntimeManager);
        void SetTriggerEngine(TriggerEngine* triggerEngine);

        // Workflow Editor: optional server-side push of run snapshots (call periodically from main thread).
        void BroadcastWorkflowRunsSnapshot();
        void BroadcastWorkflowRunsLastSnapshot();

        void Broadcast(std::string const& jsonMessage);
        void BroadcastJSON(const std::string& jsonString);
        void BroadcastPythonStatus(bool pythonRunning);

        // Log streaming: buffer lines for WebSocket broadcast (called from TerminalLogStreamBuf).
        void EnqueueLogLine(std::string const& line);

#ifdef J9T_STUDIO
        // Shut down the assistant controller early (before WRM/AiRequestPool are reset).
        void ShutdownAssistantController();
#endif

        // Drain queued broadcasts to connected WS clients.
        // Must be called periodically from the main thread (JarvisAgent::OnUpdate).
        void DrainPendingBroadcasts();

    private:
        void RegisterRoutes();
        void RegisterCommonRoutes();
        void RegisterEngineRoutes();
        void RegisterWebSocket();
#ifdef J9T_STUDIO
        void RegisterStudioRoutes();
        void RegisterAssistantWebSocket();
#endif

        // ---- Admin auth (Engine edition only) ----
        // Authentication result — returned by Authenticate().
        struct AuthResult
        {
            std::string m_Error; // empty on success
            std::string m_User;  // identity from gateway header or "token" for bearer auth
            std::string m_Role;  // "admin", "operator", or "viewer"

            bool Ok() const { return m_Error.empty(); }
        };

        // Authenticate the request. Returns AuthResult with error/user/role.
        AuthResult Authenticate(crow::request const& req) const;
        // Check if the auth result's role meets the minimum required level.
        static bool HasRole(AuthResult const& auth, std::string_view requiredRole);

        // Legacy wrapper — returns empty string on success, error code on failure.
        std::string CheckAdminAuth(crow::request const& req) const;
        // Generate a cryptographically random hex token and persist it to config.json.
        void GenerateAndPersistApiToken();
        // Record a failed auth attempt for lockout tracking.
        void RecordAuthFailure(std::string const& ip);
        // Returns true if the request should be rate-limited (429).
        bool IsRateLimited(crow::request const& req);
        // Cached token for constant-time comparison (loaded from config at startup).
        std::string m_AdminToken;
        // When the current token was issued (for expiry checks).
        std::chrono::system_clock::time_point m_TokenIssuedAt{};

        // ---- Rate limiting (Engine edition only) ----
        struct TokenBucket
        {
            double m_Tokens{20.0};
            std::chrono::steady_clock::time_point m_LastRefill{std::chrono::steady_clock::now()};
        };
        std::mutex m_RateLimitMutex;
        std::unordered_map<std::string, TokenBucket> m_RateLimitBuckets;
        std::chrono::steady_clock::time_point m_LastRateLimitCleanup{std::chrono::steady_clock::now()};

        // ---- Failed auth lockout (Engine edition only) ----
        struct AuthFailureRecord
        {
            size_t m_Count{0};
            std::chrono::steady_clock::time_point m_FirstFailure{std::chrono::steady_clock::now()};
        };
        std::unordered_map<std::string, AuthFailureRecord> m_AuthFailures; // guarded by m_RateLimitMutex

        // Static file serving — Dashboard (both editions)
        crow::response ServeStaticFile(std::filesystem::path const& filePath) const;
        crow::response ServeDashboardIndex() const;
        crow::response ServeDashboardStatic(std::string const& requestPath) const;

        // ---- MCP heartbeat ----
        crow::response HandleMcpHeartbeatPost();
        std::chrono::steady_clock::time_point m_McpLastHeartbeat{}; // guarded by m_Mutex
        std::string m_McpVersion;                                   // guarded by m_Mutex

        // ---- Engine handlers (both editions) ----
        crow::response HandleStatusGet();
        crow::response HandleWorkflowsListGet();
        crow::response HandleWorkflowGet(std::string const& workflowId);
        crow::response HandleWorkflowRunsActiveGet();
        crow::response HandleWorkflowRunsLastGet();
        crow::response HandleWorkflowRunGet(std::string const& runId);
        crow::response HandleWorkflowRunCancelPost(std::string const& runId);
        crow::response HandleWorkflowRunPausePost(std::string const& runId);
        crow::response HandleWorkflowRunResumePost(std::string const& runId);
        crow::response HandleWorkflowRunStopPost(std::string const& runId);
        crow::response HandleN8nStartPost(crow::request const& req);
        crow::response HandleWebhookPost(crow::request const& req, std::string const& workflowId);
        crow::response ReadLogFile(crow::request const& req, std::string const& logPath);
        crow::response HandleLogGet(crow::request const& req);
        crow::response HandleSecurityLogGet(crow::request const& req);

#ifdef J9T_STUDIO
        // ---- Studio handlers (Studio edition only) ----

        // Workflow Editor UI
        crow::response ServeWorkflowEditorIndex() const;
        crow::response ServeWorkflowEditorStatic(std::string const& requestPath) const;

        // Chat
        crow::response HandleChatPost(crow::request const& req);

        // Workflow CRUD
        crow::response HandleWorkflowsReloadPost();
        crow::response HandleWorkflowsCreatePost(crow::request const& req);
        crow::response HandleWorkflowUpdatePut(crow::request const& req, std::string const& workflowId);
        crow::response HandleWorkflowDelete(std::string const& workflowId);

        // Workflow versioning
        crow::response HandleWorkflowVersionsListGet(std::string const& workflowId);
        crow::response HandleWorkflowVersionGetGet(std::string const& workflowId, std::string const& timestamp);
        crow::response HandleWorkflowVersionRestorePost(std::string const& workflowId, std::string const& timestamp);

        // Workflow validation + run trigger
        crow::response HandleWorkflowValidatePost(crow::request const& req);
        crow::response HandleWorkflowValidateGet(std::string const& workflowId);
        crow::response HandleWorkflowRunPost(crow::request const& req, std::string const& workflowId);
        crow::response HandleWorkflowCleanDelete(std::string const& workflowId);

        // Script / file check
        crow::response HandleScriptCheckGet(crow::request const& req);
        crow::response HandleScriptRegistryGet();
        crow::response HandleFileCheckGet(crow::request const& req);

        // Log analysis (requires AI)
        crow::response HandleLogAnalyzeLastRunGet(crow::request const& req);

        // AI interfaces API
        crow::response HandleAiInterfacesListGet();
        crow::response HandleAiInterfaceCreatePost(crow::request const& req);
        crow::response HandleAiInterfaceUpdatePut(crow::request const& req, std::string const& name);
        crow::response HandleAiInterfaceDeleteDelete(std::string const& name);
        crow::response HandleAiInterfacesSavePost();
        crow::response HandleAiInterfaceTestPost(crow::request const& req);

        // Config settings API
        crow::response HandleConfigReloadPost();
        crow::response HandleConfigSettingsGet();
        crow::response HandleConfigSettingsPut(crow::request const& req);

        // Key management API
        crow::response HandleKeysStatusGet();
        crow::response HandleKeysUnlockPost(crow::request const& req);

        // Provider settings API
        crow::response HandleProvidersListGet();
        crow::response HandleProviderCreatePost(crow::request const& req);
        crow::response HandleProviderUpdatePut(crow::request const& req, std::string const& providerName);
        crow::response HandleProviderDelete(std::string const& providerName);
        crow::response HandleProviderSetDefaultPost(std::string const& providerName);
        crow::response HandleProvidersSavePost(crow::request const& req);

        // Cloud connections API
        crow::response HandleConnectionsListGet();
        crow::response HandleConnectionCreatePost(crow::request const& req);
        crow::response HandleConnectionUpdatePut(crow::request const& req, std::string const& connectionName);
        crow::response HandleConnectionDelete(std::string const& connectionName);
        crow::response HandleConnectionTestPost(std::string const& connectionName);
        crow::response HandleConnectionsSavePost();

        // OAuth consent flow
        crow::response HandleOAuthAuthorizeGet(std::string const& connectionName);
        crow::response HandleOAuthCallbackGet(crow::request const& req, std::string const& connectionName);
#endif // J9T_STUDIO

    private:
        crow::SimpleApp m_Server;
        std::atomic<bool> m_Running{false};
        bool m_TlsEnabled{false};
        std::thread m_ServerThread;
        std::mutex m_Mutex;

        std::unordered_set<crow::websocket::connection*> m_Clients;
        std::unordered_set<crow::websocket::connection*> m_AuthenticatedClients; // Engine: WS clients that sent valid auth
        std::atomic<size_t> m_ClientCount{0}; // lock-free mirror of m_Clients.size() for EnqueueLogLine

        // WebSocket accumulation statistics (all guarded by m_Mutex)
        size_t m_WsTotalConnects{0};
        size_t m_WsTotalDisconnects{0};
        size_t m_WsPeakClients{0};
        size_t m_WsPeakPendingBroadcasts{0};

        std::vector<std::string> m_PendingBroadcasts;

        std::mutex m_LogMutex; // separate from m_Mutex to avoid deadlock when logging inside m_Mutex scope
        std::vector<std::string> m_PendingLogLines;
        static constexpr size_t kMaxPendingLogLines = 500; // defense-in-depth cap

        WorkflowRegistry* m_WorkflowRegistry = nullptr;
        WorkflowRuntimeManager* m_WorkflowRuntimeManager = nullptr;
        TriggerEngine* m_TriggerEngine = nullptr;

#ifdef J9T_STUDIO
        AiJcwfService m_AiJcwfService;
        AssistantController m_AssistantController;

        // OAuth PKCE state: code_verifier per connection (short-lived, in-memory only)
        std::mutex m_OAuthStateMutex;
        std::unordered_map<std::string, std::string> m_OAuthCodeVerifiers;
#endif
    };
} // namespace AIAssistant
