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
#include "web/mcpKeyManager.h"
#include "web/webSessionManager.h"
#include "workflow/adhocWorkflowManager.h"
#include "workflow/scriptCatalog.h"
#include <memory>
#ifdef J9T_STUDIO
#include "web/aiJcwfService.h"
#include "assistant/assistantController.h"
#endif
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
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

        // AI dispatch lifecycle — forwarded to the dashboard so per-task live state
        // is visible beyond the aggregate "queries in flight" LED.
        void BroadcastAiCallStarted(std::string const& probName, std::string const& interfaceName);
        void BroadcastAiCallCompleted(std::string const& probName, int32_t inputTokens,
                                      int32_t outputTokens, int32_t totalTokens,
                                      std::string const& finishReason);
        void BroadcastAiCallFailed(std::string const& probName, int errorKind,
                                   int httpStatus, std::string const& errorMessage);

        // Log streaming: buffer lines for WebSocket broadcast (called from TerminalLogStreamBuf).
        void EnqueueLogLine(std::string const& line);

        // MCP sidecar heartbeat: true when the sidecar sent a heartbeat within the last 35 s.
        // Same threshold the dashboard's `mcp_connected` LED uses.
        bool IsMcpConnected();

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
        void RegisterWebSocket();
#ifdef J9T_STUDIO
        void RegisterStudioRoutes();
        void RegisterAssistantWebSocket();
#endif

        // ---- Admin auth (Engine edition only) ----
        // Authentication result — returned by Authenticate().
        struct AuthResult
        {
            std::string m_Error;        // empty on success
            std::string m_User;         // identity from gateway header, session, or MCP key
            std::string m_Role;         // "admin", "operator", or "viewer"
            int m_DaysUntilExpiry{-1};  // MCP key only — negative means "not applicable"

            bool Ok() const { return m_Error.empty(); }
        };

        // Attach `X-Key-Expires-In` and `X-Key-Self-Renew` headers when the request
        // was authenticated via an MCP key whose remaining lifetime is <= 30 days.
        // No-op when the request was not MCP-authenticated or the key is healthy.
        void AttachMcpExpiryHeader(crow::response& resp, crow::request const& req) const;

        // Authenticate the request. Returns AuthResult with error/user/role.
        AuthResult Authenticate(crow::request const& req) const;
        // Check if the auth result's role meets the minimum required level.
        static bool HasRole(AuthResult const& auth, std::string_view requiredRole);

        // Wrapper for routes that require admin role — returns empty string on success,
        // or an error code ("missing", "forbidden", "locked_out", ...) for MakeAuthErrorResponse.
        std::string CheckAdminAuth(crow::request const& req) const;
        // Role-parametrized auth check. Used by viewer/operator routes that would
        // otherwise over-restrict themselves to admin via CheckAdminAuth.
        // Returns "" on success, error code on failure.
        std::string CheckAuth(crow::request const& req, std::string_view minRole) const;
        // Same as above but populates `outAuth` on success — for handlers that
        // need the user/role on the success path (e.g. for downstream audit
        // logging or quota lookups).  Both overloads emit the
        // `forbidden reason=insufficient_role …` security log line on a role
        // denial; never roll your own role check inline.
        std::string CheckAuth(crow::request const& req, std::string_view minRole,
                              AuthResult& outAuth) const;

        // MCP key store lifecycle (shared with the existing KeyManager master password).
        // Returns true if the store is now initialised (loaded from disk, or empty-ready).
        bool InitMcpKeyStore(std::string_view masterPassword);
        // Persist pending changes using the cached master password.
        bool SaveMcpKeyStore();

        // Lookup an MCP auth result from a raw bearer token; returns nullopt if not MCP.
        std::optional<AuthResult> TryMcpAuth(crow::request const& req) const;
        // Lookup a session result from the request cookie; returns nullopt if no cookie.
        std::optional<AuthResult> TrySessionAuth(crow::request const& req) const;
        // Extract the "session=" value from the Cookie header, or empty if not present.
        static std::string ExtractSessionCookie(crow::request const& req);
        // Extract the token after "Bearer " from the Authorization header, or empty.
        static std::string ExtractBearerToken(crow::request const& req);
        // Record a failed auth attempt for lockout tracking.
        void RecordAuthFailure(std::string const& ip);

        // Two-tier rate limiting.  Pre-auth tier (per-IP) is tight and protects
        // unauthenticated traffic from credential-stuffing/floods; authenticated
        // tier (per-user) is loose and trusts the validated credential.  See
        // doc/cyber security.md §"Per-tier rate limiting" for the rationale.
        enum class RateLimitTier { PreAuth, Authenticated };
        bool IsRateLimited(RateLimitTier tier, std::string const& key);

        // ---- MCP keys + dashboard sessions ----
        mutable McpKeyManager m_McpKeyManager;
        mutable WebSessionManager m_WebSessionManager;
        std::filesystem::path m_McpKeysFilePath;
        std::atomic<bool> m_McpKeysLoaded{false};

        // ---- Adhoc workflow submission ----
        // Held by unique_ptr because construction depends on WorkflowRegistry,
        // which is set via SetWorkflowRegistry() after WebServer is constructed.
        std::unique_ptr<AdhocWorkflowManager> m_AdhocManager;

        // ---- Script catalog ----
        // Scans scripts/ at startup + on-demand refreshes; served via
        // GET /api/scripts so MCP agents can pick pre-deployed scripts when
        // composing adhoc workflows (the same scripts/ contents enforced by
        // the security boundary in HandleAdhocRunPost).
        ScriptCatalog m_ScriptCatalog;

        // ---- Uptime for debug_signals ----
        std::chrono::steady_clock::time_point m_ProcessStart{std::chrono::steady_clock::now()};

        // ---- Rate limiting (both editions, two-tier) ----
        // Buckets are seeded to full burst on first encounter.  Tokens
        // refill at the tier's rate up to the burst ceiling.
        struct TokenBucket
        {
            double m_Tokens{0.0};
            std::chrono::steady_clock::time_point m_LastRefill{};
        };
        std::mutex m_RateLimitMutex;
        std::unordered_map<std::string, TokenBucket> m_PreAuthBuckets;       // key: client IP
        std::unordered_map<std::string, TokenBucket> m_AuthenticatedBuckets; // key: validated user
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

        // ---- MCP key management + session handlers (both editions) ----
        crow::response HandleMcpKeysListGet();
        crow::response HandleMcpKeysEnrollPost(crow::request const& req);
        crow::response HandleMcpKeysActivatePost(crow::request const& req);
        crow::response HandleMcpKeysSelfRenewPost(crow::request const& req);
        crow::response HandleMcpKeysUpdatePut(crow::request const& req, std::string const& keyId);
        crow::response HandleMcpKeysDelete(std::string const& keyId);
        crow::response HandleWhoamiGet(crow::request const& req);
        crow::response HandleLoginPost(crow::request const& req);
        crow::response HandleLogoutPost(crow::request const& req);

        // ---- Adhoc workflow submission (both editions, MCP key required) ----
        crow::response HandleAdhocRunPost(crow::request const& req);

        // ---- Run artifact discovery (both editions; only adhoc runs are resolvable
        //      today — registered runs return 404 until we extend attribution). ----
        crow::response HandleRunFilesListGet(crow::request const& req, std::string const& runId);
        crow::response HandleRunFileGet(crow::request const& req,
                                        std::string const& runId,
                                        std::string const& relPath);

        // ---- Script catalog (both editions, viewer+; any authenticated user). ----
        crow::response HandleScriptsListGet(crow::request const& req);

        // ---- Key store unlock (both editions — Engine also needs this to
        //      unlock mcp_keys.json.enc). Public: the submitted master password
        //      itself is the credential, so no prior auth is required. ----
        crow::response HandleKeysStatusGet();
        crow::response HandleKeysUnlockPost(crow::request const& req);

#ifdef DEBUG
        // ---- Debug introspection (debug builds only, admin role required) ----
        //
        // Live dump of in-memory counters used to investigate runtime behavior.
        // Extend this handler with new counters when debugging — see
        // `reference_debug_signals.md` in the memory/ folder for the convention.
        // The release build has this entire path stripped at compile time.
        crow::response HandleDebugSignalsGet();
#endif

        // Look up the MCP record backing the request, or nullopt if not MCP-auth.
        // Used by handlers that need adhoc_enabled / quota / identity metadata.
        std::optional<McpKeyManager::Record> TryGetMcpRecord(crow::request const& req) const;

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

        // ---- Common admin handlers (both editions; route-level RBAC) ----

        // Workflow registry management (admin)
        crow::response HandleWorkflowsReloadPost();
        crow::response HandleWorkflowVersionsListGet(std::string const& workflowId);
        crow::response HandleWorkflowVersionGetGet(std::string const& workflowId, std::string const& timestamp);
        crow::response HandleWorkflowVersionRestorePost(std::string const& workflowId, std::string const& timestamp);

        // Run control (operator+)
        crow::response HandleWorkflowRunPost(crow::request const& req, std::string const& workflowId);
        crow::response HandleWorkflowCleanDelete(std::string const& workflowId);

        // Log analysis (operator+)
        crow::response HandleLogAnalyzeLastRunGet(crow::request const& req);

        // AI interfaces API (admin)
        crow::response HandleAiInterfacesListGet();
        crow::response HandleAiInterfaceCreatePost(crow::request const& req);
        crow::response HandleAiInterfaceUpdatePut(crow::request const& req, std::string const& name);
        crow::response HandleAiInterfaceDeleteDelete(std::string const& name);
        crow::response HandleAiInterfacesSavePost();
        crow::response HandleAiInterfaceTestPost(crow::request const& req);

        // Config settings API (admin)
        crow::response HandleConfigReloadPost();
        crow::response HandleConfigSettingsGet();
        crow::response HandleConfigSettingsPut(crow::request const& req);

        // Provider settings API (admin)
        crow::response HandleProvidersListGet();
        crow::response HandleProviderCreatePost(crow::request const& req);
        crow::response HandleProviderUpdatePut(crow::request const& req, std::string const& providerName);
        crow::response HandleProviderDelete(std::string const& providerName);
        crow::response HandleProviderSetDefaultPost(std::string const& providerName);
        crow::response HandleProvidersSavePost(crow::request const& req);

        // Cloud connections API (admin)
        crow::response HandleConnectionsListGet();
        crow::response HandleConnectionCreatePost(crow::request const& req);
        crow::response HandleConnectionUpdatePut(crow::request const& req, std::string const& connectionName);
        crow::response HandleConnectionDelete(std::string const& connectionName);
        crow::response HandleConnectionTestPost(std::string const& connectionName);
        crow::response HandleConnectionsSavePost();

        // OAuth consent flow (admin)
        crow::response HandleOAuthAuthorizeGet(std::string const& connectionName);
        crow::response HandleOAuthCallbackGet(crow::request const& req, std::string const& connectionName);

#ifdef J9T_STUDIO
        // ---- Studio-only handlers (compile-time excluded from Engine) ----

        // Workflow Editor UI
        crow::response ServeWorkflowEditorIndex() const;
        crow::response ServeWorkflowEditorStatic(std::string const& requestPath) const;

        // Workflow CRUD (mutating, editor-only)
        crow::response HandleWorkflowsCreatePost(crow::request const& req);
        crow::response HandleWorkflowUpdatePut(crow::request const& req, std::string const& workflowId);
        crow::response HandleWorkflowDelete(std::string const& workflowId);

        // Workflow validation (editor-only)
        crow::response HandleWorkflowValidatePost(crow::request const& req);
        crow::response HandleWorkflowValidateGet(std::string const& workflowId);

        // Script / file check (editor support)
        crow::response HandleScriptCheckGet(crow::request const& req);
        crow::response HandleScriptRegistryGet();
        crow::response HandleFileCheckGet(crow::request const& req);
#endif // J9T_STUDIO

    private:
        crow::SimpleApp m_Server;
        std::atomic<bool> m_Running{false};
        bool m_TlsEnabled{false};
        std::thread m_ServerThread;
        std::mutex m_Mutex;

        std::unordered_set<crow::websocket::connection*> m_Clients;
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
#endif

        // OAuth PKCE state: code_verifier per connection (short-lived, in-memory only).
        // Both editions — connections + OAuth admin routes are common (admin role-gated).
        std::mutex m_OAuthStateMutex;
        std::unordered_map<std::string, std::string> m_OAuthCodeVerifiers;
        std::unordered_map<std::string, std::string> m_OAuthStateTokens; // CSRF state param per connection
    };
} // namespace AIAssistant
