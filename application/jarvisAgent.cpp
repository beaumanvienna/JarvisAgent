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
#ifndef _WIN32
#include <unistd.h> // write, STDERR_FILENO (raw shutdown diagnostics)
#else
#include <io.h> // _write, _fileno
#endif

#include "engine.h"
#include "jarvisAgent.h"
#include "event/events.h"
#include "web/webServer.h"
#include "session/sessionManager.h"
#include "log/terminalManager.h"
#include "file/fileWatcher.h"
#include "file/probUtils.h"
#include "file/scriptRegistry.h"
#include "web/chatMessages.h"
#include "python/pythonEnginePool.h"
#include "task/carMaintenanceTask.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowTriggerBinder.h"
#include "workflow/taskExecutorRegistry.h"
#include "workflow/shellTaskExecutor.h"
#include "workflow/aiCallTaskExecutor.h"
#include "workflow/internalTaskExecutor.h"
#include "workflow/pythonTaskExecutor.h"
#include "workflow/subWorkflowTaskExecutor.h"
#include "workflow/triggerEngine.h"
#include "cloud/polarionConnector.h"
#include "cloud/polarionWriteTaskExecutor.h"
#include "cloud/s3Connector.h"
#include "cloud/s3CloudTaskExecutor.h"
#include "cloud/postgresConnector.h"
#include "cloud/dbQueryCloudTaskExecutor.h"
#include "cloud/oneDriveConnector.h"
#include "cloud/oneDriveCloudTaskExecutor.h"
#include "cloud/snowflakeConnector.h"
#include "cloud/snowflakeCloudTaskExecutor.h"
#include "cloud/slackConnector.h"
#include "cloud/slackCloudTaskExecutor.h"
#include "cloud/emailConnector.h"
#include "cloud/emailCloudTaskExecutor.h"
#include "cloud/gitHubConnector.h"
#include "cloud/gitHubCloudTaskExecutor.h"
#include "cloud/jiraConnector.h"
#include "cloud/jiraCloudTaskExecutor.h"
#include "cloud/googleSheetsConnector.h"
#include "cloud/googleSheetsCloudTaskExecutor.h"
#include "cloud/cloudConnectorRegistry.h"
#include "cloud/cloudConnectionManager.h"
#include "workflow/aiRequestPool.h"
#include "curlWrapper/curlMultiDispatcher.h"
#include "workflow/workflowFileIndex.h"
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
#if defined(_WIN32)
        ShellTaskExecutor::ProbeWindowsShell(Core::g_Core->GetConfig().m_UseBashOnWindows);
#endif

        const auto& queuePath = Core::g_Core->GetConfig().m_QueueFolderFilepath;
        std::filesystem::path const absoluteQueuePath = std::filesystem::absolute(queuePath);

        m_FileWatcher = std::make_unique<FileWatcher>(absoluteQueuePath, 100ms);
        m_FileWatcher->Start();

        // Script registry: scan scripts/ at startup, keep live via second FileWatcher
        m_ScriptRegistry = std::make_unique<ScriptRegistry>();
        std::filesystem::path const absoluteScriptsPath = std::filesystem::absolute("scripts");
        m_ScriptRegistry->ScanDirectory(absoluteScriptsPath);
        m_ScriptFileWatcher = std::make_unique<FileWatcher>(absoluteScriptsPath, 100ms);
        m_ScriptFileWatcher->Start();

        // Workflow file index: scan workflows/ at startup for basename lookup
        m_WorkflowFileIndex = std::make_unique<WorkflowFileIndex>();
        m_WorkflowFileIndex->ScanDirectory(std::filesystem::absolute("workflows"));

        // Create assistant storage directory (sessions are persisted as JSONL)
        {
            std::error_code ec;
            std::filesystem::create_directories("assistant/sessions", ec);
            if (!ec)
            {
                LOG_APP_INFO("assistant/sessions directory ready");
            }
        }

        m_WebServer = std::make_unique<WebServer>();
        if (!m_WebServer->Start())
        {
            LOG_APP_CRITICAL("WebServer failed to start (port in use or TLS misconfigured?). Shutting down.");
            m_FatalStartupMessage = "[FATAL] WebServer failed to start — check port availability and TLS config.";
            m_IsFinished = true;
            return;
        }

        // Stream log lines to dashboard via WebSocket instead of 500ms REST polling
        {
            WebServer* ws = m_WebServer.get();
            Core::g_Core->GetTerminalLogStreamBuf()->SetLogBroadcastCallback([ws](std::string const& line)
                                                                             { ws->EnqueueLogLine(line); });
        }

        m_ChatMessagePool = std::make_unique<ChatMessagePool>();

        { // initialize Python engine pool (N sub-interpreters, each with own GIL)
            m_PythonEnginePool = std::make_unique<PythonEnginePool>();

            std::string const scriptPath = "scripts/main.py";
            size_t const engineCount = Core::g_Core->GetConfig().m_PythonEngines;
            bool pythonOk = m_PythonEnginePool->Initialize(scriptPath, engineCount);

            if (!pythonOk)
            {
                LOG_APP_CRITICAL("PythonEnginePool failed to initialize. Continuing without Python scripting.");
            }
            else
            {
                m_PythonEnginePool->OnStart();
            }
        }

        m_AiRequestPool = std::make_unique<AiRequestPool>();
        m_CurlMultiDispatcher = std::make_unique<CurlMultiDispatcher>();

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

            // SubWorkflow executor (TaskType::SubWorkflow)
            // Note: runtime manager is set below after construction via late-binding setter.
            {
                m_SubWorkflowExecutor = std::make_shared<SubWorkflowTaskExecutor>(m_WorkflowRegistry.get());
                executorRegistry.RegisterExecutor(TaskType::SubWorkflow, m_SubWorkflowExecutor);
            }

            // PolarionWrite executor (TaskType::PolarionWrite)
            {
                auto& connectorRegistry = Core::g_Core->GetCloudConnectorRegistry();
                auto& connectionManager = Core::g_Core->GetCloudConnectionManager();

                // Register cloud connectors
                connectorRegistry.Register(std::make_unique<PolarionConnector>());
                connectorRegistry.Register(std::make_unique<S3Connector>());

                // Polarion write executor
                std::shared_ptr<ITaskExecutor> polarionWriteExecutor =
                    std::make_shared<PolarionWriteTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::PolarionWrite, polarionWriteExecutor);

                // S3 executor
                std::shared_ptr<ITaskExecutor> s3Executor =
                    std::make_shared<S3CloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::S3, s3Executor);

                // PostgreSQL connector + db_query executor
                connectorRegistry.Register(std::make_unique<PostgresConnector>());

                std::shared_ptr<ITaskExecutor> dbQueryExecutor =
                    std::make_shared<DbQueryCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::DbQuery, dbQueryExecutor);

                // OneDrive connector + executor
                connectorRegistry.Register(std::make_unique<OneDriveConnector>());

                std::shared_ptr<ITaskExecutor> oneDriveExecutor =
                    std::make_shared<OneDriveCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::OneDrive, oneDriveExecutor);

                // Snowflake connector + executor
                connectorRegistry.Register(std::make_unique<SnowflakeConnector>());

                std::shared_ptr<ITaskExecutor> snowflakeExecutor =
                    std::make_shared<SnowflakeCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::SnowflakeQuery, snowflakeExecutor);

                // Slack connector + executor
                connectorRegistry.Register(std::make_unique<SlackConnector>());

                std::shared_ptr<ITaskExecutor> slackExecutor =
                    std::make_shared<SlackCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::SlackMessage, slackExecutor);

                // Email connector + executor
                connectorRegistry.Register(std::make_unique<EmailConnector>());

                std::shared_ptr<ITaskExecutor> emailExecutor =
                    std::make_shared<EmailCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::EmailSend, emailExecutor);
                executorRegistry.RegisterExecutor(TaskType::EmailRead, emailExecutor);

                // GitHub connector + executor
                connectorRegistry.Register(std::make_unique<GitHubConnector>());

                std::shared_ptr<ITaskExecutor> githubExecutor =
                    std::make_shared<GitHubCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::GitHubIssue, githubExecutor);

                // Jira connector + executor
                connectorRegistry.Register(std::make_unique<JiraConnector>());

                std::shared_ptr<ITaskExecutor> jiraExecutor =
                    std::make_shared<JiraCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::JiraIssue, jiraExecutor);

                // Google Sheets connector + executor
                connectorRegistry.Register(std::make_unique<GoogleSheetsConnector>());

                auto sheetsExecutor =
                    std::make_shared<GoogleSheetsCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::SheetsRead, sheetsExecutor);
                executorRegistry.RegisterExecutor(TaskType::SheetsWrite, sheetsExecutor);
            }
        }

        m_WebServer->SetWorkflowRegistry(m_WorkflowRegistry.get());

        m_WorkflowRuntimeManager = std::make_unique<WorkflowRuntimeManager>();
        m_WorkflowRuntimeManager->SetRegistry(m_WorkflowRegistry.get());
        m_WorkflowRuntimeManager->Start();

        // Late-bind the runtime manager to the sub-workflow executor.
        if (m_SubWorkflowExecutor)
        {
            m_SubWorkflowExecutor->SetRuntimeManager(m_WorkflowRuntimeManager.get());
        }

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

        m_WebServer->SetTriggerEngine(m_TriggerEngine.get());

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
        if (m_IsFinished || Core::g_Core->IsQuitRequested())
        {
            return;
        }

        // Update all session managers (state machines for REQ/STNG/TASK)
        for (auto& sessionManager : m_SessionManagers)
        {
            sessionManager.second->OnUpdate();
        }

        // Remove stale session managers: idle, no tracked files, queue folder gone.
        {
            size_t const smCountBefore = m_SessionManagers.size();
            for (auto it = m_SessionManagers.begin(); it != m_SessionManagers.end();)
            {
                auto& sm = *it->second;
                bool const hasFiles = sm.HasTrackedFiles();
                bool const dirExists = fs::is_directory(it->first);
                if (!hasFiles && !dirExists)
                {
                    LOG_APP_INFO("Removing stale session manager '{}'", it->first);
                    m_StatusRenderer.RemoveSession(it->first);
                    it = m_SessionManagers.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            size_t const removed = smCountBefore - m_SessionManagers.size();
            if (removed > 0)
            {
                LOG_APP_INFO("Stale session cleanup: removed {} of {} session managers", removed, smCountBefore);
            }
        }

        // Clean old chat messages
        m_ChatMessagePool->RemoveExpired();

        // --- Python OnUpdate disabled ---
        // m_PythonEnginePool->OnUpdate();

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

        // NOTE: WebSocket broadcasts are drained inside Crow's onmessage handler
        // (triggered by the editor's 500ms poll).  Calling DrainPendingBroadcasts()
        // here on the main thread caused silent message loss because Crow's
        // send_text overlapped with the IO-thread drain (see webServer.cpp:2712).

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
                m_PythonEnginePool->Stop();
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
                // Guard against stale .output files from previous runs: if the file
                // was last written before this JA session started, ignore it.
                bool staleOutputFile = false;
                {
                    std::error_code ec;
                    auto const lwt = std::filesystem::last_write_time(filePath, ec);
                    if (!ec && ToSystemClock(lwt) < m_StartupTime)
                    {
                        staleOutputFile = true;
                        LOG_APP_INFO("Ignoring stale .output file (pre-startup): '{}'", filePath.string());
                    }
                }

                if (!staleOutputFile)
                {
                    if (m_AiRequestPool->OnOutputFileCreated(filePath.string()))
                    {
                        LOG_APP_INFO("AiRequestPool: path-based completion matched '{}'", filePath.string());
                    }
                }
            }
        }

        // -----------------------------------------------------------------------------------
        // Script registry: route scripts/ file events to ScriptRegistry
        // -----------------------------------------------------------------------------------
        if (!filePath.empty() && m_ScriptRegistry != nullptr)
        {
            std::string const pathStr = filePath.string();
            std::filesystem::path const absoluteScriptsPath = std::filesystem::absolute("scripts");
            std::string const scriptsPrefix = absoluteScriptsPath.string();
            if (pathStr.rfind(scriptsPrefix, 0) == 0)
            {
                if (fileEventType == TriggerEngine::FileEventType::Deleted)
                {
                    m_ScriptRegistry->Remove(filePath);
                }
                else
                {
                    m_ScriptRegistry->AddOrUpdate(filePath);
                }
                return;
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
        if (m_PythonEnginePool)
            m_PythonEnginePool->OnEvent(eventPtr);
    }

    //--------------------------------------------------------------------

    void JarvisAgent::OnShutdown()
    {
        // --- Raw diagnostics: bypass spdlog to catch hangs inside OnShutdown ---
#ifndef _WIN32
#define RAW_ONSHUTDOWN(literal)                                                           \
    do                                                                                    \
    {                                                                                     \
        [[maybe_unused]] auto rc_ = ::write(STDERR_FILENO, literal, sizeof(literal) - 1); \
    } while (0)
#else
#define RAW_ONSHUTDOWN(literal) _write(_fileno(stderr), literal, sizeof(literal) - 1)
#endif

        RAW_ONSHUTDOWN("[OnShutdown] entered\n");
        LOG_APP_INFO("leaving JarvisAgent");

        // ── Phase 1: signal all subsystems (non-blocking) ──────────────────
        RAW_ONSHUTDOWN("[OnShutdown] SignalShutdown...\n");
        Core::g_Core->SignalShutdown(); // sets global flag + ThreadPool::RequestStop()

        RAW_ONSHUTDOWN("[OnShutdown] AiRequestPool::Shutdown...\n");
        if (m_AiRequestPool != nullptr)
        {
            m_AiRequestPool->Shutdown();
        }

        RAW_ONSHUTDOWN("[OnShutdown] CurlMultiDispatcher::SignalStop...\n");
        if (m_CurlMultiDispatcher != nullptr)
        {
            m_CurlMultiDispatcher->SignalStop();
        }

#ifdef J9T_STUDIO
        // Shut down assistant controller early: its background threads hold pointers
        // to WRM and WorkflowRegistry, so it must be joined before those are reset.
        RAW_ONSHUTDOWN("[OnShutdown] AssistantController::Shutdown...\n");
        if (m_WebServer != nullptr)
        {
            m_WebServer->ShutdownAssistantController();
        }
        RAW_ONSHUTDOWN("[OnShutdown] AssistantController stopped\n");
#endif

        LOG_APP_INFO("[shutdown] phase 1: signalling all subsystems...");
        RAW_ONSHUTDOWN("[OnShutdown] WRM::SignalStop...\n");
        if (m_WorkflowRuntimeManager != nullptr)
        {
            m_WorkflowRuntimeManager->SignalStop();
        }
        RAW_ONSHUTDOWN("[OnShutdown] PythonEnginePool::SignalStop...\n");
        if (m_PythonEnginePool != nullptr)
        {
            m_PythonEnginePool->SignalStop();
        }
        RAW_ONSHUTDOWN("[OnShutdown] FileWatcher::SignalStop...\n");
        if (m_FileWatcher != nullptr)
        {
            m_FileWatcher->SignalStop();
        }
        if (m_ScriptFileWatcher != nullptr)
        {
            m_ScriptFileWatcher->SignalStop();
        }
        RAW_ONSHUTDOWN("[OnShutdown] WebServer::SignalStop...\n");
        if (m_WebServer != nullptr)
        {
            m_WebServer->SignalStop();
        }
        LOG_APP_INFO("[shutdown] phase 1 complete — all subsystems signalled");

        // ── Phase 2: wait for all subsystems (blocking, but parallel) ──────
        LOG_APP_INFO("[shutdown] phase 2: waiting for subsystems...");

        RAW_ONSHUTDOWN("[OnShutdown] WRM::WaitStop...\n");
        if (m_WorkflowRuntimeManager != nullptr)
        {
            m_WorkflowRuntimeManager->WaitStop();
            m_WorkflowRuntimeManager.reset();
        }
        RAW_ONSHUTDOWN("[OnShutdown] WRM stopped\n");
        LOG_APP_INFO("[shutdown] WorkflowRuntimeManager stopped");

        m_AiRequestPool.reset();

        RAW_ONSHUTDOWN("[OnShutdown] CurlMultiDispatcher::WaitStop...\n");
        if (m_CurlMultiDispatcher != nullptr)
        {
            m_CurlMultiDispatcher->WaitStop();
            m_CurlMultiDispatcher.reset();
        }
        RAW_ONSHUTDOWN("[OnShutdown] CurlMultiDispatcher stopped\n");
        LOG_APP_INFO("[shutdown] CurlMultiDispatcher stopped");

        App::g_App = nullptr;

        RAW_ONSHUTDOWN("[OnShutdown] session managers...\n");
        for (auto& sessionManager : m_SessionManagers)
        {
            sessionManager.second->OnShutdown();
        }
        RAW_ONSHUTDOWN("[OnShutdown] session managers stopped\n");
        LOG_APP_INFO("[shutdown] session managers stopped");

        RAW_ONSHUTDOWN("[OnShutdown] PythonEnginePool::WaitStop...\n");
        if (m_PythonEnginePool != nullptr)
        {
            m_PythonEnginePool->WaitStop();
            m_PythonEnginePool.reset();
        }
        RAW_ONSHUTDOWN("[OnShutdown] PythonEnginePool stopped\n");
        LOG_APP_INFO("[shutdown] PythonEnginePool stopped");

        RAW_ONSHUTDOWN("[OnShutdown] FileWatcher::WaitStop...\n");
        if (m_FileWatcher != nullptr)
        {
            m_FileWatcher->WaitStop();
        }
        if (m_ScriptFileWatcher != nullptr)
        {
            m_ScriptFileWatcher->WaitStop();
        }
        RAW_ONSHUTDOWN("[OnShutdown] FileWatchers stopped\n");
        LOG_APP_INFO("[shutdown] FileWatchers stopped");

        RAW_ONSHUTDOWN("[OnShutdown] WebServer::WaitStop...\n");
        if (m_WebServer != nullptr)
        {
            m_WebServer->WaitStop();
        }
        RAW_ONSHUTDOWN("[OnShutdown] WebServer stopped\n");
        LOG_APP_INFO("[shutdown] WebServer stopped");

        RAW_ONSHUTDOWN("[OnShutdown] done\n");
#undef RAW_ONSHUTDOWN
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