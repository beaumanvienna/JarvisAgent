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

namespace AIAssistant
{
    JarvisAgent* App::g_App = nullptr;
    std::unique_ptr<Application> JarvisAgent::Create() { return std::make_unique<JarvisAgent>(); }

    void JarvisAgent::OnStart()
    {
        // capture application startup time
        m_StartupTime = std::chrono::system_clock::now();

        LOG_APP_INFO("starting JarvisAgent version {}", JARVIS_AGENT_VERSION);
        App::g_App = this;

        // ---------------------------------------------------------
        // Hook StatusRenderer → TerminalManager (engine-owned)
        // ---------------------------------------------------------
        if (Core::g_Core != nullptr)
        {
            TerminalManager* terminal = Core::g_Core->GetTerminalManager();
            if (terminal != nullptr)
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

        m_FileWatcher = std::make_unique<FileWatcher>(queuePath, 100ms);
        m_FileWatcher->Start();

        m_WebServer = std::make_unique<WebServer>();
        m_WebServer->Start();

        m_ChatMessagePool = std::make_unique<ChatMessagePool>();

        // initialize Python
        m_PythonEngine = std::make_unique<PythonEngine>();

        std::string const scriptPath = "scripts/main.py";
        bool pythonOk = m_PythonEngine->Initialize(scriptPath);

        if (!pythonOk)
        {
            LOG_APP_CRITICAL("PythonEngine failed to initialize. Continuing without Python scripting.");
            m_WebServer->BroadcastPythonStatus(false);
        }
        else
        {
            m_PythonEngine->OnStart();
            m_WebServer->BroadcastPythonStatus(true);
        }
    }

    //--------------------------------------------------------------------

    void JarvisAgent::OnUpdate()
    {
        // Update all session managers (state machines for REQ/STNG/TASK)
        for (auto& sessionManager : m_SessionManagers)
        {
            sessionManager.second->OnUpdate();
        }

        // Clean old chat messages
        m_ChatMessagePool->RemoveExpired();

        // --- Python OnUpdate disabled ---
        // if (m_PythonEngine)
        // {
        //     m_PythonEngine->OnUpdate();
        // }

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

        dispatcher.Dispatch<FileAddedEvent>(
            [&](FileAddedEvent& fileEvent)
            {
                filePath = fileEvent.GetPath();
                return false;
            });

        dispatcher.Dispatch<FileModifiedEvent>(
            [&](FileModifiedEvent& fileEvent)
            {
                filePath = fileEvent.GetPath();
                return false;
            });

        dispatcher.Dispatch<FileRemovedEvent>(
            [&](FileRemovedEvent& fileEvent)
            {
                filePath = fileEvent.GetPath();
                return false;
            });

        // -----------------------------------------------------------------------------------
        // ChatMessagePool handling (PROB_xxx files)
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
        // Forward remaining file events to correct SessionManager
        // -----------------------------------------------------------------------------------

        if (!filePath.empty())
        {
            auto sessionManagerName = filePath.parent_path().string();

            if (!m_SessionManagers.contains(sessionManagerName))
            {
                m_SessionManagers[sessionManagerName] = std::make_unique<SessionManager>(sessionManagerName);
            }

            m_SessionManagers[sessionManagerName]->OnEvent(event);
        }

        // Forward event to Python
        if (m_PythonEngine)
        {
            m_PythonEngine->OnEvent(eventPtr);
        }
    }

    //--------------------------------------------------------------------

    void JarvisAgent::OnShutdown()
    {
        LOG_APP_INFO("leaving JarvisAgent");
        App::g_App = nullptr;

        for (auto& sessionManager : m_SessionManagers)
        {
            sessionManager.second->OnShutdown();
        }

        if (m_PythonEngine)
        {
            m_PythonEngine->Stop();
            m_PythonEngine.reset();
            m_WebServer->BroadcastPythonStatus(false);
        }

        if (m_FileWatcher)
        {
            m_FileWatcher->Stop();
        }

        if (m_WebServer)
        {
            m_WebServer->Stop();
        }

        // No terminal shutdown here — engine handles it
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
