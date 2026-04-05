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
#include <chrono>
#include <memory>
#include <unordered_map>

#include "application.h"
#include "file/fileCategory.h"
#include "log/statusRenderer.h"
#include "task/internalTaskRegistry.h"

namespace AIAssistant
{
    class SessionManager;
    class FileWatcher;
    class WebServer;
    class ChatMessagePool;
    class PythonEnginePool;
    class WorkflowRegistry;
    class TriggerEngine;
    class ScriptRegistry;

    class AiRequestPool;
    class CurlMultiDispatcher;
    class WorkflowFileIndex;
    class WorkflowRuntimeManager;
    class SubWorkflowTaskExecutor;

    class JarvisAgent : public Application
    {
    public:
        JarvisAgent() = default;
        virtual ~JarvisAgent() = default;

        virtual void OnStart() override;
        virtual void OnUpdate() override;
        virtual void OnEvent(std::shared_ptr<Event>&) override;
        virtual void OnShutdown() override;

        virtual bool IsFinished() const override;
        static std::unique_ptr<Application> Create();

        WebServer* GetWebServer() const { return m_WebServer.get(); }
        ChatMessagePool* GetChatMessagePool() const { return m_ChatMessagePool.get(); }
        std::chrono::system_clock::time_point GetStartupTime() const { return m_StartupTime; }
        int64_t GetStartupTimestamp() const;
        StatusRenderer& GetStatusRenderer() { return m_StatusRenderer; }
        PythonEnginePool* GetPythonEnginePool() { return m_PythonEnginePool.get(); }
        WorkflowRegistry* GetWorkflowRegistry() { return m_WorkflowRegistry.get(); }
        ScriptRegistry* GetScriptRegistry() { return m_ScriptRegistry.get(); }
        WorkflowFileIndex* GetWorkflowFileIndex() { return m_WorkflowFileIndex.get(); }

        AiRequestPool* GetAiRequestPool() const { return m_AiRequestPool.get(); }
        CurlMultiDispatcher* GetCurlMultiDispatcher() const { return m_CurlMultiDispatcher.get(); }
        WorkflowRuntimeManager* GetWorkflowRuntimeManager() const { return m_WorkflowRuntimeManager.get(); }

        IInternalTaskRegistry* GetInternalTaskRegistry() { return &m_InternalTaskRegistry; }

        size_t GetSessionManagerCount() const { return m_SessionManagers.size(); }
        size_t GetSessionManagerInflightTotal() const;
        size_t GetSessionManagersWithInflight() const;

        template <typename Func> void ForEachSessionManager(Func&& func)
        {
            for (auto& [name, sm] : m_SessionManagers)
            {
                func(*sm);
            }
        }

    private:
        void CheckIfFinished();
        void InitializeWorkflows();

    private:
        bool m_IsFinished{false};

    private:
        StatusRenderer m_StatusRenderer;
        std::chrono::system_clock::time_point m_StartupTime;

        // submodules
        std::unordered_map<std::string, std::unique_ptr<SessionManager>> m_SessionManagers;
        std::unique_ptr<FileWatcher> m_FileWatcher;
        std::unique_ptr<FileWatcher> m_ScriptFileWatcher;
        std::unique_ptr<ScriptRegistry> m_ScriptRegistry;
        std::unique_ptr<WebServer> m_WebServer;
        std::unique_ptr<ChatMessagePool> m_ChatMessagePool;
        std::unique_ptr<PythonEnginePool> m_PythonEnginePool;

        std::unique_ptr<WorkflowRegistry> m_WorkflowRegistry;
        std::unique_ptr<TriggerEngine> m_TriggerEngine;

        InternalTaskRegistry m_InternalTaskRegistry;

        std::unique_ptr<AiRequestPool> m_AiRequestPool;
        std::unique_ptr<CurlMultiDispatcher> m_CurlMultiDispatcher;
        std::unique_ptr<WorkflowRuntimeManager> m_WorkflowRuntimeManager;
        std::unique_ptr<WorkflowFileIndex> m_WorkflowFileIndex;

        std::shared_ptr<SubWorkflowTaskExecutor> m_SubWorkflowExecutor;
    };

    class App
    {
    public:
        static JarvisAgent* g_App;
    };
} // namespace AIAssistant
