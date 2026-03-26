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
#include "web/aiJcwfService.h"
#include "assistant/assistantController.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <string>
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

        // Shut down the assistant controller early (before WRM/AiRequestPool are reset).
        void ShutdownAssistantController();

        // Drain queued broadcasts to connected WS clients.
        // Must be called periodically from the main thread (JarvisAgent::OnUpdate).
        void DrainPendingBroadcasts();

    private:
        void RegisterRoutes();
        void RegisterWebSocket();
        void RegisterAssistantWebSocket();

        // Static file serving (Dashboard + Workflow Editor UI)
        crow::response ServeStaticFile(std::filesystem::path const& filePath) const;
        crow::response ServeDashboardIndex() const;
        crow::response ServeDashboardStatic(std::string const& requestPath) const;
        crow::response ServeWorkflowEditorIndex() const;
        crow::response ServeWorkflowEditorStatic(std::string const& requestPath) const;

        // Handlers
        crow::response HandleChatPost(crow::request const& req);
        crow::response HandleStatusGet();

        // Workflow editor API (Phase 1: CRUD)
        // All /api/workflows endpoints return application/json.
        // Workflow editor API (Phase 2: validation + run monitoring/control)
        crow::response HandleWorkflowValidatePost(crow::request const& req);
        crow::response HandleWorkflowValidateGet(std::string const& workflowId);

        crow::response HandleWorkflowRunPost(crow::request const& req, std::string const& workflowId);
        crow::response HandleWorkflowCleanDelete(std::string const& workflowId);
        crow::response HandleWorkflowRunsActiveGet();
        crow::response HandleWorkflowRunsLastGet();
        crow::response HandleWorkflowRunGet(std::string const& runId);
        crow::response HandleWorkflowRunCancelPost(std::string const& runId);
        crow::response HandleWorkflowRunPausePost(std::string const& runId);
        crow::response HandleWorkflowRunResumePost(std::string const& runId);
        crow::response HandleWorkflowRunStopPost(std::string const& runId);

        // Integrations: n8n
        crow::response HandleN8nStartPost(crow::request const& req);

        // Webhook trigger endpoint
        crow::response HandleWebhookPost(crow::request const& req, std::string const& workflowId);

        crow::response HandleWorkflowsListGet();
        crow::response HandleWorkflowsReloadPost();
        crow::response HandleWorkflowsCreatePost(crow::request const& req);
        crow::response HandleWorkflowGet(std::string const& workflowId);
        crow::response HandleWorkflowUpdatePut(crow::request const& req, std::string const& workflowId);
        crow::response HandleWorkflowDelete(std::string const& workflowId);

        // Workflow versioning
        crow::response HandleWorkflowVersionsListGet(std::string const& workflowId);
        crow::response HandleWorkflowVersionGetGet(std::string const& workflowId, std::string const& timestamp);
        crow::response HandleWorkflowVersionRestorePost(std::string const& workflowId, std::string const& timestamp);

        // AI interfaces API (config.json "API interfaces")
        crow::response HandleAiInterfacesListGet();
        crow::response HandleAiInterfaceCreatePost(crow::request const& req);
        crow::response HandleAiInterfaceUpdatePut(crow::request const& req, std::string const& name);
        crow::response HandleAiInterfaceDeleteDelete(std::string const& name);
        crow::response HandleAiInterfacesSavePost();
        crow::response HandleAiInterfaceTestPost(crow::request const& req);
        crow::response HandleConfigReloadPost();
        crow::response HandleConfigSettingsGet();
        crow::response HandleConfigSettingsPut(crow::request const& req);

        // Script check API (Workflow Editor)
        crow::response HandleScriptCheckGet(crow::request const& req);
        crow::response HandleScriptRegistryGet();

        // File existence check API (Workflow Editor — static file_inputs)
        crow::response HandleFileCheckGet(crow::request const& req);

        // Log viewer API
        crow::response HandleLogGet(crow::request const& req);
        crow::response HandleLogAnalyzeLastRunGet(crow::request const& req);

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

    private:
        crow::SimpleApp m_Server;
        std::atomic<bool> m_Running{false};
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

        AiJcwfService m_AiJcwfService;
        AssistantController m_AssistantController;
    };
} // namespace AIAssistant
