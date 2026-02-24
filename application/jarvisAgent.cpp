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

#include <filesystem>
#include <system_error>

#include "engine.h"
#include "jarvisAgent.h"
#include "event/events.h"
#include "web/webServer.h"
#include "session/sessionManager.h"
#include "log/terminalManager.h"
#include "file/fileWatcher.h"
#include "file/probUtils.h"
#include "web/chatMessages.h"
#include "python/pythonEngine.h"
#include "task/carMaintenanceTask.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowTriggerBinder.h"
#include "workflow/taskExecutorRegistry.h"
#include "workflow/shellTaskExecutor.h"
#include "workflow/aiCallTaskExecutor.h"
#include "workflow/internalTaskExecutor.h"
#include "workflow/pythonTaskExecutor.h"
#include "workflow/triggerEngine.h"
#include "workflow/aiRequestPool.h"
#include "workflow/workflowRuntimeManager.h"

namespace
{
    std::chrono::system_clock::time_point ToSystemClock(std::filesystem::file_time_type const& fileTime)
    {
        using FileClock = std::filesystem::file_time_type::clock;

        auto const fileNow = FileClock::now();
        auto const systemNow = std::chrono::system_clock::now();

        auto const adjustedTime = fileTime - fileNow + systemNow;
        return std::chrono::time_point_cast<std::chrono::system_clock::duration>(adjustedTime);
    }
} // namespace

namespace AIAssistant
{
    JarvisAgent* App::g_App = nullptr;
    std::unique_ptr<Application> JarvisAgent::Create() { return std::make_unique<JarvisAgent>(); }

    void JarvisAgent::OnStart()
    {
        CORE_ASSERT(Core::g_Core != nullptr, "Core must exist before JarvisAgent start!");

        // capture application startup time
        m_StartupTime = std::chrono::system_clock::now();

        LOG_APP_INFO("starting JarvisAgent version {}", JARVIS_AGENT_VERSION);
        App::g_App = this;

        // ---------------------------------------------------------
        // Internal task registrations
        // ---------------------------------------------------------
        m_InternalTaskRegistry.RegisterFactory("carMaintenance", []() { return std::make_unique<CarMaintenanceTask>(); });

        // ---------------------------------------------------------
        // Hook StatusRenderer → TerminalManager (engine-owned)
        // ---------------------------------------------------------
        {
            TerminalManager* terminal = Core::g_Core->GetTerminalManager();
            {
                terminal->SetStatusCallbacks(
                    // Build status lines dynamically
                    [this](std::vector<std::string>& lines, int maxWidth)
                    { m_StatusRenderer.BuildStatusLines(lines, maxWidth); },

                    // Compute status window height dynamically
                    [this](int totalRows) -> int
                    {
                        size_t sessionCount = m_StatusRenderer.GetSessionCount();
                        if (sessionCount == 0)
                        {
                            sessionCount = 1;
                        }

                        int statusHeight = static_cast<int>(sessionCount);

                        // ensure at least 1 line, and leave at least 1 for log
                        if (statusHeight >= totalRows)
                        {
                            statusHeight = std::max(1, totalRows - 1);
                        }

                        return statusHeight;
                    });
            }
        }

        // ---------------------------------------------------------
        // Start all other subsystems
        // ---------------------------------------------------------
        const auto& queuePath = Core::g_Core->GetConfig().m_QueueFolderFilepath;
        std::filesystem::path const absoluteQueuePath = std::filesystem::absolute(queuePath);

        m_FileWatcher = std::make_unique<FileWatcher>(absoluteQueuePath, 100ms);
        m_FileWatcher->Start();

        m_WebServer = std::make_unique<WebServer>();
        if (!m_WebServer->Start())
        {
            LOG_APP_CRITICAL("WebServer failed to start (port already in use?). Shutting down.");
            m_FatalStartupMessage = "[FATAL] Port 8080 is already in use — is another JarvisAgent running? Exiting.";
            m_IsFinished = true;
            return;
        }

        m_ChatMessagePool = std::make_unique<ChatMessagePool>();

        { // initialize Python
            m_PythonEngine = std::make_unique<PythonEngine>();

            std::string const scriptPath = "scripts/main.py";
            bool pythonOk = m_PythonEngine->Initialize(scriptPath);

            if (!pythonOk)
            {
                LOG_APP_CRITICAL("PythonEngine failed to initialize. Continuing without Python scripting.");
            }
            else
            {
                m_PythonEngine->OnStart();
            }
        }

        m_AiRequestPool = std::make_unique<AiRequestPool>();

        // ---------------------------------------------------------
        // Initialize workflow system (registry + runtime manager + triggers)
        // ---------------------------------------------------------
        InitializeWorkflows();
    }

    //--------------------------------------------------------------------

    void JarvisAgent::InitializeWorkflows()
    {
        m_WorkflowRegistry = std::make_unique<WorkflowRegistry>();

        std::filesystem::path workflowsDirectory = Core::g_Core->GetConfig().m_WorkflowsFolderFilepath;

        if (!m_WorkflowRegistry->LoadDirectory(workflowsDirectory))
        {
            LOG_APP_WARN("JarvisAgent::InitializeWorkflows: no workflows loaded from '{}'", workflowsDirectory.string());
        }
        else
        {
            LOG_APP_INFO("Loaded {} workflow(s) from '{}'", m_WorkflowRegistry->GetWorkflowIds().size(),
                         workflowsDirectory.string());
            if (!m_WorkflowRegistry->ValidateAll())
            {
                LOG_APP_WARN("JarvisAgent::InitializeWorkflows: one or more workflows failed validation");
            }
        }

        // ---------------------------------------------------------
        // Register task executors
        // ---------------------------------------------------------
        {
            TaskExecutorRegistry& executorRegistry = TaskExecutorRegistry::Get();

            // Shell executor (TaskType::Shell)
            {
                std::shared_ptr<ITaskExecutor> shellExecutor = std::make_shared<ShellTaskExecutor>();
                executorRegistry.RegisterExecutor(TaskType::Shell, shellExecutor);
            }

            // AiCall executor (TaskType::AiCall)
            {
                std::shared_ptr<ITaskExecutor> aiCallExecutor = std::make_shared<AiCallTaskExecutor>();
                executorRegistry.RegisterExecutor(TaskType::AiCall, aiCallExecutor);
            }
            // Python executor (TaskType::Python)
            {
                std::shared_ptr<ITaskExecutor> pythonExecutor = std::make_shared<PythonTaskExecutor>();
                executorRegistry.RegisterExecutor(TaskType::Python, pythonExecutor);
            }

            // Internal executor (TaskType::Internal)
            // Note: m_InternalTaskRegistry is owned by JarvisAgent; wrap it with a no-op deleter.
            {
                std::shared_ptr<IInternalTaskRegistry> internalTaskRegistryPtr(
                    static_cast<IInternalTaskRegistry*>(&m_InternalTaskRegistry), [](IInternalTaskRegistry* const) {});

                std::shared_ptr<ITaskExecutor> internalExecutor =
                    std::make_shared<InternalTaskExecutor>(internalTaskRegistryPtr);

                executorRegistry.RegisterExecutor(TaskType::Internal, internalExecutor);
            }
        }

        m_WebServer->SetWorkflowRegistry(m_WorkflowRegistry.get());

        m_WorkflowRuntimeManager = std::make_unique<WorkflowRuntimeManager>();
        m_WorkflowRuntimeManager->SetRegistry(m_WorkflowRegistry.get());
        m_WorkflowRuntimeManager->Start();

        m_WebServer->SetWorkflowRuntimeManager(m_WorkflowRuntimeManager.get());

        m_TriggerEngine = std::make_unique<TriggerEngine>(
            [this](TriggerEngine::TriggerFiredEvent const& triggerEvent)
            {
                LOG_APP_INFO("JarvisAgent: Trigger fired for workflow '{}' (trigger id '{}')", triggerEvent.m_WorkflowId,
                             triggerEvent.m_TriggerId);

                if (m_WorkflowRuntimeManager != nullptr)
                {
                    m_WorkflowRuntimeManager->EnqueueWorkflowRun(triggerEvent.m_WorkflowId);
                }
                else
                {
                    LOG_APP_ERROR("JarvisAgent: Trigger fired for workflow '{}' but WorkflowRuntimeManager is null",
                                  triggerEvent.m_WorkflowId);
                }
            });

        // -----------------------------------------------------------------
        // Bind all JCWF triggers into TriggerEngine
        // -----------------------------------------------------------------
        if (m_WorkflowRegistry && m_TriggerEngine)
        {
            WorkflowTriggerBinder workflowTriggerBinder;
            workflowTriggerBinder.RegisterAll(*m_WorkflowRegistry, *m_TriggerEngine);
        }
        else
        {
            LOG_APP_WARN("JarvisAgent::InitializeWorkflows: skipping trigger registration (registry or engine missing)");
        }
    }

    //--------------------------------------------------------------------

    void JarvisAgent::OnUpdate()
    {
        if (m_IsFinished)
        {
            return;
        }

        // Update all session managers (state machines for REQ/STNG/TASK)
        for (auto& sessionManager : m_SessionManagers)
        {
            sessionManager.second->OnUpdate();
        }

        // Clean old chat messages
        m_ChatMessagePool->RemoveExpired();

        // --- Python OnUpdate disabled ---
        // m_PythonEngine->OnUpdate();

        if (m_AiRequestPool != nullptr)
        {
            m_AiRequestPool->Update();
        }

        // Tick trigger engine (cron-based triggers)
        if (m_TriggerEngine)
        {
            auto const now = std::chrono::system_clock::now();
            m_TriggerEngine->Tick(now);
        }

        if (m_WorkflowRuntimeManager != nullptr)
        {
            if (m_WorkflowRuntimeManager->Update())
            {
                m_WebServer->BroadcastWorkflowRunsSnapshot();
                m_WebServer->BroadcastWorkflowRunsLastSnapshot();
            }
        }

        // Termination logic
        CheckIfFinished();
    }

    //--------------------------------------------------------------------

    void JarvisAgent::OnEvent(std::shared_ptr<Event>& eventPtr)
    {
        auto& event = *eventPtr.get();
        EventDispatcher dispatcher(event);

        // ---------------------------------------------------------
        // App-level event handling
        // ---------------------------------------------------------
        dispatcher.Dispatch<EngineEvent>(
            [&](EngineEvent& engineEvent)
            {
                if (engineEvent.GetEngineCode() == EngineEvent::EngineEventShutdown)
                {
                    LOG_APP_INFO("App received shutdown request");
                    m_IsFinished = true;
                }
                else
                {
                    LOG_APP_ERROR("unhandled engine event");
                }
                return true;
            });

        fs::path filePath;
        bool hasFileEvent = false;
        TriggerEngine::FileEventType fileEventType = TriggerEngine::FileEventType::Created;

        dispatcher.Dispatch<FileAddedEvent>(
            [&](FileAddedEvent& fileEvent)
            {
                filePath = fileEvent.GetPath();
                fileEventType = TriggerEngine::FileEventType::Created;
                hasFileEvent = true;
                return false;
            });

        dispatcher.Dispatch<FileModifiedEvent>(
            [&](FileModifiedEvent& fileEvent)
            {
                filePath = fileEvent.GetPath();
                fileEventType = TriggerEngine::FileEventType::Modified;
                hasFileEvent = true;
                return false;
            });

        dispatcher.Dispatch<FileRemovedEvent>(
            [&](FileRemovedEvent& fileEvent)
            {
                filePath = fileEvent.GetPath();
                fileEventType = TriggerEngine::FileEventType::Deleted;
                hasFileEvent = true;
                return false;
            });

        dispatcher.Dispatch<PythonCrashedEvent>(
            [&](PythonCrashedEvent& evt)
            {
                LOG_APP_CRITICAL("Python crashed: {}", evt.GetMessage());
                m_PythonEngine->Stop();
                return true;
            });

        // ---------------------------------------------------------
        // Forward file events into TriggerEngine (file_watch triggers)
        // ---------------------------------------------------------
        if (hasFileEvent && m_TriggerEngine)
        {
            auto const now = std::chrono::system_clock::now();

            bool suppressTriggerEvent = false;
            if (fileEventType != TriggerEngine::FileEventType::Deleted)
            {
                auto const timeSinceStartup = now - m_StartupTime;
                if (timeSinceStartup <= std::chrono::seconds(10))
                {
                    std::error_code errorCode;
                    std::filesystem::file_time_type const lastWriteTime =
                        std::filesystem::last_write_time(filePath, errorCode);
                    if (!errorCode)
                    {
                        auto const lastWriteSystemTime = ToSystemClock(lastWriteTime);
                        if (lastWriteSystemTime < m_StartupTime)
                        {
                            suppressTriggerEvent = true;
                        }
                    }
                }
            }

            if (suppressTriggerEvent)
            {
                LOG_APP_INFO("JarvisAgent: ignoring file event for '{}' during startup (pre-existing file)",
                             filePath.string());
            }
            else
            {
                m_TriggerEngine->NotifyFileEvent(filePath.string(), fileEventType, now);
            }
        }

        // -----------------------------------------------------------------------------------
        // PROB handling (ai_call completion consumes output files first, then chat)
        // -----------------------------------------------------------------------------------

        if (!filePath.empty())
        {
            std::string filename = filePath.filename().string();

            std::optional<ProbUtils::ProbFileInfo> parsedProbFileInfo = ProbUtils::ParseProbFilename(filename);

            if (parsedProbFileInfo.has_value())
            {
                const ProbUtils::ProbFileInfo& probFileInfo = parsedProbFileInfo.value();

                int64_t startupTimestamp = App::g_App->GetStartupTimestamp();
                int64_t fileTimestamp = probFileInfo.timestamp;

                // Suppress stale files
                if (fileTimestamp < startupTimestamp)
                {
                    return;
                }

                // PROB OUTPUT
                if (probFileInfo.isOutput)
                {
                    if (m_AiRequestPool != nullptr)
                    {
                        if (m_AiRequestPool->OnProbFileEvent(probFileInfo, filePath.string()))
                        {
                            return;
                        }
                    }

                    std::ifstream inputStream(filePath);
                    std::stringstream outputBuffer;
                    outputBuffer << inputStream.rdbuf();

                    std::string responseText = outputBuffer.str();

                    m_ChatMessagePool->MarkAnswered(probFileInfo.id, responseText);

                    LOG_APP_INFO("ChatMessagePool: answered id {} via {}", probFileInfo.id, filename);

                    return;
                }
            }
        }

        // -----------------------------------------------------------------------------------
        // Path-based AI completion routing for non-PROB_<id>_<ts> output files
        // (e.g., PROB_NVDA.output.txt written by SessionManager for workflow ai_call tasks)
        // -----------------------------------------------------------------------------------
        if (!filePath.empty() && m_AiRequestPool != nullptr)
        {
            std::string const stem = filePath.stem().string();
            if (stem.ends_with(".output"))
            {
                if (m_AiRequestPool->OnOutputFileCreated(filePath.string()))
                {
                    LOG_APP_INFO("AiRequestPool: path-based completion matched '{}'", filePath.string());
                }
            }
        }

        // -----------------------------------------------------------------------------------
        // Forward remaining file events to correct SessionManager
        // -----------------------------------------------------------------------------------

        if (!filePath.empty())
        {
            auto sessionManagerName = fs::relative(filePath.parent_path()).string();

            if (!m_SessionManagers.contains(sessionManagerName))
            {
                m_SessionManagers[sessionManagerName] = std::make_unique<SessionManager>(sessionManagerName);
            }

            m_SessionManagers[sessionManagerName]->OnEvent(event);
        }

        // Forward event to Python
        m_PythonEngine->OnEvent(eventPtr);
    }

    //--------------------------------------------------------------------

    void JarvisAgent::OnShutdown()
    {
        LOG_APP_INFO("leaving JarvisAgent");

        // ── Phase 1: signal all subsystems (non-blocking) ──────────────────
        Core::g_Core->SignalShutdown(); // sets global flag + ThreadPool::RequestStop()

        if (m_AiRequestPool != nullptr)
        {
            m_AiRequestPool->Shutdown();
        }

        LOG_APP_INFO("[shutdown] phase 1: signalling all subsystems...");
        if (m_WorkflowRuntimeManager != nullptr)
        {
            m_WorkflowRuntimeManager->SignalStop();
        }
        if (m_PythonEngine != nullptr)
        {
            m_PythonEngine->SignalStop();
        }
        if (m_FileWatcher != nullptr)
        {
            m_FileWatcher->SignalStop();
        }
        if (m_WebServer != nullptr)
        {
            m_WebServer->SignalStop();
        }
        LOG_APP_INFO("[shutdown] phase 1 complete — all subsystems signalled");

        // ── Phase 2: wait for all subsystems (blocking, but parallel) ──────
        LOG_APP_INFO("[shutdown] phase 2: waiting for subsystems...");

        if (m_WorkflowRuntimeManager != nullptr)
        {
            m_WorkflowRuntimeManager->WaitStop();
            m_WorkflowRuntimeManager.reset();
        }
        LOG_APP_INFO("[shutdown] WorkflowRuntimeManager stopped");

        m_AiRequestPool.reset();
        App::g_App = nullptr;

        for (auto& sessionManager : m_SessionManagers)
        {
            sessionManager.second->OnShutdown();
        }
        LOG_APP_INFO("[shutdown] session managers stopped");

        if (m_PythonEngine != nullptr)
        {
            m_PythonEngine->WaitStop();
            m_PythonEngine.reset();
        }
        LOG_APP_INFO("[shutdown] PythonEngine stopped");

        if (m_FileWatcher != nullptr)
        {
            m_FileWatcher->WaitStop();
        }
        LOG_APP_INFO("[shutdown] FileWatcher stopped");

        if (m_WebServer != nullptr)
        {
            m_WebServer->WaitStop();
        }
        LOG_APP_INFO("[shutdown] WebServer stopped");
    }

    //--------------------------------------------------------------------

    bool JarvisAgent::IsFinished() const { return m_IsFinished; }

    void JarvisAgent::CheckIfFinished()
    {
        // Ctrl+C is caught by engine and breaks run loop
    }

    int64_t JarvisAgent::GetStartupTimestamp() const
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(m_StartupTime.time_since_epoch()).count();
    }

    size_t JarvisAgent::GetSessionManagerInflightTotal() const
    {
        size_t total = 0;
        for (auto const& [name, sm] : m_SessionManagers)
        {
            total += sm->GetInflightCount();
        }
        return total;
    }

    size_t JarvisAgent::GetSessionManagersWithInflight() const
    {
        size_t count = 0;
        for (auto const& [name, sm] : m_SessionManagers)
        {
            if (sm->GetInflightCount() > 0)
            {
                ++count;
            }
        }
        return count;
    }

} // namespace AIAssistant