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
#include "simdjson/simdjson.h"
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
#include <string_view>
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
        void BroadcastAiCallCompleted(std::string const& probName, std::string const& interfaceName,
                                      int32_t inputTokens, int32_t outputTokens, int32_t totalTokens,
                                      std::string const& finishReason);
        // Extended payload (Workstream B): provider-side discriminators + UI-facing
        // semantic category + Retry-After hint + the user-configured interface label.
        // The dashboard branches banner/LED state on `category` (string-serialized
        // ProviderErrorCategory) rather than the raw provider strings, so the
        // wire schema is stable across providers.  retryAfterSeconds omitted from
        // the JSON when nullopt.
        void BroadcastAiCallFailed(std::string const& probName, int errorKind,
                                   int httpStatus, std::string const& errorMessage,
                                   std::string const& providerErrorCode,
                                   std::string const& providerErrorType,
                                   std::string_view category,
                                   std::optional<int> retryAfterSeconds,
                                   std::string const& interfaceName);

        // Sitting-8 Workstream D close-out: cap-changed wake signal.  Payload-free
        // — receivers refetch /api/providers/health for authoritative state.
        // Fired from JarvisAgent's AiCapChangedEvent dispatcher when the AIMD
        // controller's m_CurrentConcurrencyCap mutates on any per-interface
        // observation.
        void BroadcastCapChanged();

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

        // Edition-specific WebServer init.  Studio impl
        // (`webServer_studio.cpp`) wires up the assistant WS route and the
        // AiJcwfService broadcast callback; Engine impl (`webServer_engine.cpp`)
        // is a no-op.  Resolves at link time — no `#ifdef J9T_STUDIO` needed at
        // the call site in the shared constructor.
        void InitEditionSpecific();

        // Edition-specific WS message dispatch.  Returns true if `type`
        // matched an assistant message (ai-explain-jcwf / ai-generate-jcwf /
        // ai-write-scripts / ai-fix-failed-script) and was dispatched; false
        // otherwise so the caller falls through to "unknown type".  Studio
        // impl lives in `webServer_studio.cpp`; Engine impl in
        // `webServer_engine.cpp` always returns false (Engine has no assistant
        // surface).
        [[nodiscard]] bool HandleAssistantWebSocketMessage(crow::websocket::connection& conn,
                                                           simdjson::ondemand::document& doc,
                                                           std::string_view type);

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
        // Non-const: the funnel mutates m_AuthFailures / rate-limit buckets via
        // RecordAuthFailure / IsRateLimited; marking these methods const and
        // const_cast-ing inside hides the mutation from the type system and
        // creates a foot-gun for future readers who assume const = thread-safe.
        void AttachMcpExpiryHeader(crow::response& resp, crow::request const& req);

        // Authenticate the request. Returns AuthResult with error/user/role.
        AuthResult Authenticate(crow::request const& req);
        // Check if the auth result's role meets the minimum required level.
        static bool HasRole(AuthResult const& auth, std::string_view requiredRole);

        // Wrapper for routes that require admin role — returns empty string on success,
        // or an error code ("missing", "forbidden", "locked_out", ...) for MakeAuthErrorResponse.
        std::string CheckAdminAuth(crow::request const& req);
        // Role-parametrized auth check. Used by viewer/operator routes that would
        // otherwise over-restrict themselves to admin via CheckAdminAuth.
        // Returns "" on success, error code on failure.
        std::string CheckAuth(crow::request const& req, std::string_view minRole);
        // Same as above but populates `outAuth` on success — for handlers that
        // need the user/role on the success path (e.g. for downstream audit
        // logging or quota lookups).  Both overloads emit the
        // `forbidden reason=insufficient_role …` security log line on a role
        // denial; never roll your own role check inline.
        std::string CheckAuth(crow::request const& req, std::string_view minRole,
                              AuthResult& outAuth);

        // MCP key store lifecycle (shared with the existing KeyManager master password).
        // Returns true if the store is now initialised (loaded from disk, or empty-ready).
        bool InitMcpKeyStore(std::string_view masterPassword);
        // Persist pending changes using the cached master password.
        bool SaveMcpKeyStore();

        // Lookup an MCP auth result from a raw bearer token; returns nullopt if not MCP.
        // Non-const: calls RecordAuthFailure on invalid-key path.
        std::optional<AuthResult> TryMcpAuth(crow::request const& req);
        // Lookup a session result from the request cookie; returns nullopt if no cookie.
        // Non-const for consistency with the rest of the auth funnel — see AttachMcpExpiryHeader.
        std::optional<AuthResult> TrySessionAuth(crow::request const& req);
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
        crow::response HandleMcpHeartbeatPost(crow::request const& req);
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

        // Hermetic-test entry: feed (interface_type, header_buffer, body, status,
        // model) → parsed RateLimitObservation + quota_key + initial_concurrency_probe.
        // Lets test/dispatch/test_rate_limit_observation_parse.py exercise every
        // provider strategy without a live HTTP round-trip.
        crow::response HandleParseRateLimitHeadersPost(crow::request const& req);

        // Hermetic-test readback: dispatcher's bounded ring of recent
        // submissions.  Used by test_size_aware_budget.py to assert
        // QueryData::m_TimeoutMs matches the size-aware budget formula
        // without scraping logs.
        crow::response HandleDebugRecentSubmissionsGet();

        // Renders the completion-callback payload for a given runId without
        // firing the outbound HTTPS POST.  Lets test_negative_paths.py verify
        // the 64 KiB per-output cap + UTF-8-safe truncation without the SSRF
        // gate getting in the way (the gate rejects loopback callbackUrls by
        // design).  Query: runId (required), include_outputs (optional bool).
        crow::response HandleDebugBuildCallbackPayloadGet(crow::request const& req);

        // Observe-idempotent contract test.  Body:
        //   { "initial_concurrency_probe": int, "hard_cap": int,
        //     "observations": [ {observation+was_429}, ... ] }
        // Constructs an ephemeral RateLimitController, applies the observations
        // in order, and returns the resulting controller state (cap, streak,
        // last observation) so the test can compare a single combined call
        // against a sequence of partial calls.
        crow::response HandleDebugTestObserveIdempotentPost(crow::request const& req);

        // Mock-AI-response endpoint for hermetic dispatcher tests.  Lets
        // Python tests configure a real AI interface (API1/API4/etc.)
        // pointing at this URL and exercise the dispatcher with controlled
        // responses without an upstream provider.  Query params:
        //   status         (int, default 200)
        //   delay_ms       (int, default 0)   — sleep before responding
        //   reset_in_sec   (int, default 60)  — substituted into {{RESET_AT_ISO}}
        //   header_fixture (string, optional) — basename in test/dispatch/fixtures/headers/<X>.txt
        //   body_fixture   (string, optional) — basename in test/dispatch/fixtures/responses/<X>.json
        // Auth-free on purpose: the dispatcher hits this with provider-style
        // auth (Bearer/x-api-key/etc.), not an MCP key.  Debug builds only.
        crow::response HandleDebugMockAiResponsePost(crow::request const& req);

        // Hermetic-test isolation: wipes dispatcher controllers, host rate-
        // limit state, and the recent-submissions ring so repeated test runs
        // start from a clean slate.  Does NOT touch in-flight work
        // (m_Active / m_Inbox / m_RetryQueue).
        crow::response HandleDebugResetDispatcherStatePost();
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

        // Sitting-8 Workstream D: per-interface health snapshot for the dashboard's
        // AI Health LED.  Joins config interfaces × pool last-error tracking ×
        // dispatcher AIMD cap state into one response array.
        crow::response HandleProvidersHealthGet();

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
        // Performance hint, NOT a source of truth.  Skips taking m_Mutex on the
        // hot logging path (EnqueueLogLine / Broadcast / BroadcastJSON early-
        // exit) when no dashboard clients are connected — the common case for
        // headless / engine-edition deployments.  Updated under m_Mutex
        // alongside m_Clients in onopen / onclose, but read without the lock,
        // so a load may race with a concurrent connect/disconnect.  Acceptable:
        // the worst case is a benign over-send (a broadcast queued for a client
        // that just disconnected, or skipped for a client that just connected).
        // m_Clients (under m_Mutex) is the authoritative routing set; this
        // atomic must never decide whether a client receives a message — only
        // whether the broadcast machinery runs at all on the producer side.
        std::atomic<size_t> m_ClientCount{0};

        // Per-connection role captured at the WS upgrade handshake (.onaccept).
        // The connection pointer is identity-only — never dereferenced.  Used
        // by message-type branches that mutate disk state (currently
        // ai-write-scripts) so an operator/viewer credential cannot escalate
        // to admin-only operations once the WS is open.  Guarded by m_Mutex.
        std::unordered_map<crow::websocket::connection*, std::string> m_WsClientRoles;

        // WebSocket accumulation statistics (all guarded by m_Mutex)
        size_t m_WsTotalConnects{0};
        size_t m_WsTotalDisconnects{0};
        size_t m_WsPeakClients{0};
        size_t m_WsPeakPendingBroadcasts{0};

        // Diagnostic counters for the dashboard live-update queue path.
        // Per-type enqueue counters answer "is the queue mostly duplicate
        // full-state snapshots?"; drain stats answer "how big are the
        // batches the IO thread sends, and how long does each drain take?".
        size_t m_WsTotalBroadcastsEnqueued{0};
        size_t m_WsTotalRunsSnapshotsEnqueued{0};
        size_t m_WsTotalLastRunsSnapshotsEnqueued{0};
        size_t m_WsTotalAiCallEventsEnqueued{0};
        size_t m_WsTotalPythonStatusEnqueued{0};
        size_t m_WsTotalLogBatchesEnqueued{0};
        size_t m_WsTotalDrains{0};
        size_t m_WsLastDrainBytes{0};
        size_t m_WsLastDrainMessages{0};
        size_t m_WsPeakDrainBytes{0};
        uint64_t m_WsLastDrainDurationUs{0};
        uint64_t m_WsPeakDrainDurationUs{0};

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
