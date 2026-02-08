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
#include <atomic>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <unordered_set>

namespace AIAssistant
{
    class WorkflowRegistry;
    class WorkflowRuntimeManager;
    class WebServer
    {
    public:
        WebServer();
        ~WebServer();

        void Start();
        void Stop();

        // Optional pointers for Workflow Editor API (set by JarvisAgent during startup).
        // If not set, editor run-monitoring endpoints will return "not configured".
        void SetWorkflowRegistry(WorkflowRegistry* workflowRegistry);
        void SetWorkflowRuntimeManager(WorkflowRuntimeManager* workflowRuntimeManager);

        // Workflow Editor: optional server-side push of run snapshots (call periodically from main thread).
        void BroadcastWorkflowRunsSnapshot();

        void Broadcast(std::string const& jsonMessage);
        void BroadcastJSON(const std::string& jsonString);
        void BroadcastPythonStatus(bool pythonRunning);

    private:
        void RegisterRoutes();
        void RegisterWebSocket();

        // Static file serving (Workflow Editor UI)
        crow::response ServeStaticFile(std::filesystem::path const& filePath) const;
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

        crow::response HandleWorkflowRunPost(std::string const& workflowId);
        crow::response HandleWorkflowRunsActiveGet();
        crow::response HandleWorkflowRunsLastGet();
        crow::response HandleWorkflowRunGet(std::string const& runId);
        crow::response HandleWorkflowRunCancelPost(std::string const& runId);

        // Integrations: n8n
        crow::response HandleN8nStartPost(crow::request const& req);

        crow::response HandleWorkflowsListGet();
        crow::response HandleWorkflowsReloadPost();
        crow::response HandleWorkflowsCreatePost(crow::request const& req);
        crow::response HandleWorkflowGet(std::string const& workflowId);
        crow::response HandleWorkflowUpdatePut(crow::request const& req, std::string const& workflowId);
        crow::response HandleWorkflowDelete(std::string const& workflowId);

        // AI interfaces API (config.json "API interfaces")
        crow::response HandleAiInterfacesListGet();
        crow::response HandleAiInterfaceCreatePost(crow::request const& req);
        crow::response HandleAiInterfaceUpdatePut(crow::request const& req, std::string const& name);
        crow::response HandleAiInterfaceDeleteDelete(std::string const& name);
        crow::response HandleAiInterfacesSavePost();
        crow::response HandleConfigReloadPost();

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
        std::future<void> m_ServerTask;
        std::mutex m_Mutex;

        std::unordered_set<crow::websocket::connection*> m_Clients;

        WorkflowRegistry* m_WorkflowRegistry = nullptr;
        WorkflowRuntimeManager* m_WorkflowRuntimeManager = nullptr;
    };
} // namespace AIAssistant
