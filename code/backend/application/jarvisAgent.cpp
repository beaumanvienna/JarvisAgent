/* Copyright (c) 2026 JC Technolabs

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
#include "log/terminalManager.h"
#include "file/fileWatcher.h"
#include "file/scriptRegistry.h"
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
#include "cloud/redmineConnector.h"
#include "cloud/redmineCloudTaskExecutor.h"
#include "cloud/googleSheetsConnector.h"
#include "cloud/googleSheetsCloudTaskExecutor.h"
#include "cloud/azureBlobConnector.h"
#include "cloud/azureBlobCloudTaskExecutor.h"
#include "cloud/gcsConnector.h"
#include "cloud/gcsCloudTaskExecutor.h"
#include "cloud/cloudConnectorRegistry.h"
#include "cloud/cloudConnectionManager.h"
#include "workflow/aiCallEvents.h"
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
    std::atomic<JarvisAgent*> App::g_App{nullptr};
    std::unique_ptr<Application> JarvisAgent::Create() { return std::make_unique<JarvisAgent>(); }

    void JarvisAgent::OnStart()
    {
        CORE_ASSERT(Core::g_Core != nullptr, "Core must exist before JarvisAgent start!");

        // capture application startup time
        m_StartupTime = std::chrono::system_clock::now();

        LOG_APP_INFO("starting JarvisAgent version {}", JARVIS_AGENT_VERSION);
        App::g_App.store(this, std::memory_order_release);

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
                // Lifetime contract: the [this] captures below are read on the
                // TUI redraw thread (engine-owned).  TerminalManager outlives
                // JarvisAgent during shutdown, so JarvisAgent::OnShutdown's
                // first action is to call SetStatusCallbacks({}, {}) — without
                // that, a redraw after JarvisAgent destruction would deref a
                // dangling `this`.
                terminal->SetStatusCallbacks(
                    // Build status lines dynamically
                    [this](std::vector<std::string>& lines, int maxWidth)
                    { m_StatusRenderer.BuildStatusLines(lines, maxWidth); },

                    // Compute status window height dynamically
                    [this](int totalRows) -> int
                    {
                        size_t rowCount = m_StatusRenderer.GetRowCount();
                        if (rowCount == 0)
                        {
                            rowCount = 1;
                        }

                        int statusHeight = static_cast<int>(rowCount);

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

        // The queue folder is no longer watched at the application level — direct
        // envelope dispatch handles its own output files, and file_watch triggers
        // use the TriggerEngine-owned watcher instead.  The queue + workflows roots
        // themselves are created on demand by ConfigChecker (earliest startup point)
        // since they're runtime-owned and untracked in git.

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

        { // initialize Python engine pool (N sub-interpreters, each with own GIL)
            m_PythonEnginePool = std::make_unique<PythonEnginePool>();

            std::string const scriptPath = "scripts/main.py";
            size_t const engineCount = Core::g_Core->GetConfig().m_PythonEngines;
            bool pythonOk = m_PythonEnginePool->Initialize(scriptPath, engineCount, m_ScriptRegistry.get());

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

        // Sitting-8 Workstream D close-out: wire the cap-changed wake signal.
        // Dispatcher fires the callback from the I/O thread when an AIMD
        // observation mutates m_CurrentConcurrencyCap; we translate to an
        // event so the dashboard's AI Health LED refetches /api/providers/health
        // within milliseconds instead of waiting for the next 5s poll cycle.
        m_CurlMultiDispatcher->SetOnCapChangedCallback(
            []()
            {
                if (Core::g_Core != nullptr)
                {
                    Core::g_Core->PushEvent(std::make_shared<AiCapChangedEvent>(), ProducerId::JarvisAgent);
                }
            });

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
            // ERROR-level so the dashboard's Run Analyzer surfaces the load
            // failure rather than burying it in WARN.  Includes the resolved
            // directory as a literal substring so log queries can attribute
            // it; downstream workflow runs will fail because no JCWFs are
            // registered, and JC needs the upstream cause visible.
            LOG_APP_ERROR("JarvisAgent::InitializeWorkflows: failed to load workflows from directory='{}' "
                          "(downstream runs will fail with 'workflow not found')",
                          workflowsDirectory.string());
        }
        else
        {
            LOG_APP_INFO("Loaded {} workflow(s) from '{}'", m_WorkflowRegistry->GetWorkflowIds().size(),
                         workflowsDirectory.string());
            if (!m_WorkflowRegistry->ValidateAll())
            {
                // ERROR-level: a workflow that fails validation will fail at
                // run time too.  Surfacing this at boot lets JC see the bad
                // JCWF before a trigger fires it.
                LOG_APP_ERROR("JarvisAgent::InitializeWorkflows: one or more workflows failed validation in "
                              "directory='{}' — check earlier validator log lines for the offending workflow id",
                              workflowsDirectory.string());
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
                executorRegistry.RegisterExecutor(TaskType::SlackRead, slackExecutor);

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

                // Redmine connector + executor
                connectorRegistry.Register(std::make_unique<RedmineConnector>());

                std::shared_ptr<ITaskExecutor> redmineExecutor =
                    std::make_shared<RedmineCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::RedmineIssue, redmineExecutor);

                // Google Sheets connector + executor
                connectorRegistry.Register(std::make_unique<GoogleSheetsConnector>());

                auto sheetsExecutor =
                    std::make_shared<GoogleSheetsCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::SheetsRead, sheetsExecutor);
                executorRegistry.RegisterExecutor(TaskType::SheetsWrite, sheetsExecutor);

                // Azure Blob connector + executor
                connectorRegistry.Register(std::make_unique<AzureBlobConnector>());

                std::shared_ptr<ITaskExecutor> azureBlobExecutor =
                    std::make_shared<AzureBlobCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::AzureBlob, azureBlobExecutor);

                // GCS connector + executor
                connectorRegistry.Register(std::make_unique<GcsConnector>());

                std::shared_ptr<ITaskExecutor> gcsExecutor =
                    std::make_shared<GcsCloudTaskExecutor>(connectorRegistry, connectionManager);
                executorRegistry.RegisterExecutor(TaskType::Gcs, gcsExecutor);
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

        // Feed the TUI status window the same signals the dashboard consumes so
        // both stay in sync (keys / AI in flight / runs / MCP / cloud / totals).
        {
            WorkflowRuntimeManager* runtime = m_WorkflowRuntimeManager.get();
            WebServer* webServer = m_WebServer.get();
            AiRequestPool* requestPool = m_AiRequestPool.get();
            PythonEnginePool* pythonPool = m_PythonEnginePool.get();

            m_StatusRenderer.SetRuntimeSnapshotProvider(
                [runtime, webServer, requestPool, pythonPool]() -> StatusRenderer::RuntimeSnapshot
                {
                    StatusRenderer::RuntimeSnapshot snap;

                    if (Core::g_Core != nullptr)
                    {
                        auto const& keyManager = Core::g_Core->GetKeyManager();
                        switch (keyManager.GetKeyLoadStatus())
                        {
                            case KeyManager::KeyLoadStatus::Ok: snap.keysStatus = "ok"; break;
                            case KeyManager::KeyLoadStatus::NoPassword:
                                snap.keysStatus = "no_password";
                                break;
                            case KeyManager::KeyLoadStatus::WrongPassword:
                                snap.keysStatus = "wrong_password";
                                break;
                            case KeyManager::KeyLoadStatus::NoKeysFile:
                                snap.keysStatus = "no_keys_file";
                                break;
                        }
                        // Usable = keyless loopback interface OR keyed interface
                        // with its credential present (not mere credential count).
                        snap.hasProviders = Core::g_Core->HasUsableAiInterface();

                        auto const& breaker = Core::g_Core->GetCloudCircuitBreaker();
                        auto health = breaker.GetHealthSummary();
                        for (auto const& h : health)
                        {
                            switch (h.m_State)
                            {
                                case CloudCircuitBreaker::State::Closed: ++snap.cloudHealthy; break;
                                case CloudCircuitBreaker::State::HalfOpen:
                                    ++snap.cloudRecovering;
                                    break;
                                case CloudCircuitBreaker::State::Open: ++snap.cloudOpen; break;
                            }
                        }
                    }

                    if (requestPool != nullptr)
                    {
                        snap.aiInflight = requestPool->GetDirectDispatchInflight();
                    }

                    if (runtime != nullptr)
                    {
                        snap.activeRuns = runtime->GetActiveRunsSnapshot().size();
                        runtime->GetRunCounters(snap.totalCompleted, snap.totalFailed);
                    }

                    if (webServer != nullptr)
                    {
                        snap.mcpConnected = webServer->IsMcpConnected();
                    }

                    if (pythonPool != nullptr)
                    {
                        snap.pythonRunning = pythonPool->IsRunning();
                    }

                    return snap;
                });

            // Rolling last-3 runs (same shape the dashboard's LastRunsBar shows).
            m_StatusRenderer.SetLastRunsProvider(
                [runtime](size_t maxCount) -> std::vector<StatusRenderer::LastRunSummary>
                {
                    std::vector<StatusRenderer::LastRunSummary> out;
                    if (runtime == nullptr) return out;

                    auto snapshot = runtime->GetLastRunsSnapshot();
                    std::vector<WorkflowRun> runs;
                    runs.reserve(snapshot.size());
                    for (auto& entry : snapshot)
                    {
                        runs.push_back(std::move(entry.second));
                    }
                    std::sort(runs.begin(), runs.end(),
                              [](WorkflowRun const& a, WorkflowRun const& b)
                              { return a.m_CompletedAtIso8601 > b.m_CompletedAtIso8601; });
                    if (runs.size() > maxCount) runs.resize(maxCount);

                    for (auto const& run : runs)
                    {
                        StatusRenderer::LastRunSummary summary;
                        summary.isAdhoc = run.m_RunId.rfind("adhoc_", 0) == 0
                                          || run.m_WorkflowId.rfind("_adhoc_", 0) == 0;
                        if (summary.isAdhoc)
                        {
                            size_t const lastUnderscore = run.m_WorkflowId.find_last_of('_');
                            if (lastUnderscore != std::string::npos
                                && lastUnderscore + 1 < run.m_WorkflowId.size())
                            {
                                summary.displayId =
                                    "adhoc #" + run.m_WorkflowId.substr(lastUnderscore + 1);
                            }
                            else
                            {
                                summary.displayId = run.m_WorkflowId;
                            }
                        }
                        else
                        {
                            summary.displayId = run.m_WorkflowId;
                        }
                        switch (run.m_State)
                        {
                            case WorkflowRunState::Succeeded: summary.state = "succeeded"; break;
                            case WorkflowRunState::Failed: summary.state = "failed"; break;
                            case WorkflowRunState::Cancelled: summary.state = "cancelled"; break;
                            case WorkflowRunState::Stopped: summary.state = "stopped"; break;
                            case WorkflowRunState::Running: summary.state = "running"; break;
                            case WorkflowRunState::Pending: summary.state = "pending"; break;
                            case WorkflowRunState::Paused: summary.state = "paused"; break;
                            case WorkflowRunState::Stopping: summary.state = "stopping"; break;
                        }
                        summary.completedAtIso = run.m_CompletedAtIso8601;
                        out.push_back(std::move(summary));
                    }
                    return out;
                });
        }

        // Lifetime contract: this [this] capture fires from a TriggerEngine-
        // owned thread (cron / file-watch / webhook / manual).  m_TriggerEngine
        // is reset in OnShutdown phase 2 (after WebServer::WaitStop, before
        // engine thread-pool drain), so the lambda body cannot run after
        // JarvisAgent destruction — the unique_ptr reset blocks until the
        // trigger thread joins.  Body still null-checks m_WorkflowRuntimeManager
        // because it is reset earlier in phase 2 than m_TriggerEngine.
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
            // ERROR-level: missing registry or trigger engine at this point
            // means the agent will boot but cannot fire any cron / file-watch
            // / webhook / manual trigger — silent dead-air for users.
            LOG_APP_ERROR("JarvisAgent::InitializeWorkflows: skipping trigger registration "
                          "(registry={}, engine={}) — workflows will not auto-fire",
                          m_WorkflowRegistry != nullptr ? "ok" : "null",
                          m_TriggerEngine != nullptr ? "ok" : "null");
        }
    }

    //--------------------------------------------------------------------

    void JarvisAgent::OnUpdate()
    {
        if (m_IsFinished || Core::g_Core->IsQuitRequested())
        {
            return;
        }

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

        // Forward AI-call lifecycle events to the dashboard WebSocket so per-call
        // detail (interface, tokens, finish reason, error) is visible beyond the
        // aggregate "queries in flight" LED.  The TUI reads the same counters the
        // dashboard does via the runtime snapshot provider set during startup —
        // no separate event wiring needed.
        if (m_WebServer != nullptr)
        {
            dispatcher.Dispatch<AiCallStartedEvent>(
                [&](AiCallStartedEvent& evt)
                {
                    m_WebServer->BroadcastAiCallStarted(evt.GetProbName(), evt.GetInterfaceName());
                    return false;
                });
            dispatcher.Dispatch<AiCallCompletedEvent>(
                [&](AiCallCompletedEvent& evt)
                {
                    auto const& usage = evt.GetUsage();
                    m_WebServer->BroadcastAiCallCompleted(evt.GetProbName(), evt.GetInterfaceName(),
                                                          usage.m_InputTokens, usage.m_OutputTokens,
                                                          usage.m_TotalTokens, evt.GetFinishReason());
                    return false;
                });
            dispatcher.Dispatch<AiCallFailedEvent>(
                [&](AiCallFailedEvent& evt)
                {
                    auto const& err = evt.GetError();
                    m_WebServer->BroadcastAiCallFailed(evt.GetProbName(), static_cast<int>(err.m_Kind),
                                                       err.m_HttpStatus, err.m_Message,
                                                       err.m_ProviderErrorCode, err.m_ProviderErrorType,
                                                       CategoryToString(err.m_Category),
                                                       err.m_RetryAfterSeconds,
                                                       evt.GetInterfaceName());
                    return false;
                });
            dispatcher.Dispatch<AiCapChangedEvent>(
                [&](AiCapChangedEvent&)
                {
                    // Sitting-8 Workstream D close-out: payload-free wake signal
                    // for the dashboard's AI Health LED — receiver refetches
                    // /api/providers/health for authoritative state.
                    m_WebServer->BroadcastCapChanged();
                    return false;
                });
        }

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

        // Detach status callbacks BEFORE any subsystem teardown.  TerminalManager
        // is engine-owned and outlives JarvisAgent during the engine's own
        // shutdown phase; without this clear, a TUI redraw triggered after
        // JarvisAgent destruction would dereference `this` through the captured
        // [this] in the lambdas wired in OnStart and run-into-use-after-free.
        // Both invocation sites in TerminalManager null-check the std::function
        // so an empty assignment is the explicit "no more status updates" signal.
        if (Core::g_Core != nullptr)
        {
            if (TerminalManager* terminal = Core::g_Core->GetTerminalManager(); terminal != nullptr)
            {
                terminal->SetStatusCallbacks({}, {});
            }
        }

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

        App::g_App.store(nullptr, std::memory_order_release);

        RAW_ONSHUTDOWN("[OnShutdown] PythonEnginePool::WaitStop...\n");
        if (m_PythonEnginePool != nullptr)
        {
            m_PythonEnginePool->WaitStop();
            m_PythonEnginePool.reset();
        }
        RAW_ONSHUTDOWN("[OnShutdown] PythonEnginePool stopped\n");
        LOG_APP_INFO("[shutdown] PythonEnginePool stopped");

        RAW_ONSHUTDOWN("[OnShutdown] FileWatcher::WaitStop...\n");
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

        // TriggerEngine owns a FileWatcher whose polling task lives on the
        // thread pool.  Reset it AFTER WebServer::WaitStop so any in-flight
        // request that held a raw pointer has returned, and BEFORE the engine
        // drains the thread pool so the watcher task can exit cleanly.
        RAW_ONSHUTDOWN("[OnShutdown] TriggerEngine::reset...\n");
        m_TriggerEngine.reset();
        RAW_ONSHUTDOWN("[OnShutdown] TriggerEngine stopped\n");
        LOG_APP_INFO("[shutdown] TriggerEngine stopped");

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

} // namespace AIAssistant