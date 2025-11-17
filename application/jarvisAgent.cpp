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
#include "session/sessionManager.h"
#include "file/fileWatcher.h"
#include "web/webServer.h"
#include "web/chatMessages.h"
#include "python/pythonEngine.h"
#include "file/probUtils.h"

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
        }
        else
        {
            m_PythonEngine->OnStart();
        }
    }

    void JarvisAgent::OnUpdate()
    {
        for (auto& sessionManager : m_SessionManagers)
        {
            sessionManager.second->OnUpdate();
        }

        m_ChatMessagePool->RemoveExpired();

        if (m_PythonEngine)
        {
            m_PythonEngine->OnUpdate();
        }

        // check if app should terminate
        CheckIfFinished();
    }

    void JarvisAgent::OnEvent(std::shared_ptr<Event>& eventPtr)
    {
        auto& event = *eventPtr.get();
        EventDispatcher dispatcher(event);

        // app-level event handling
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
        // Handle ChatMessagePool answer files: PROB_<id>_<timestamp>.output.txt
        // Also suppress stale input files PROB_<id>_<timestamp>.txt created before startup.
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

                // -----------------------------------------------------------------------
                // Suppress stale PROB files (input or output)
                // -----------------------------------------------------------------------
                if (fileTimestamp < startupTimestamp)
                {
                    return; // silent ignore
                }

                // -----------------------------------------------------------------------
                // PROB OUTPUT → forward directly to ChatMessagePool
                // -----------------------------------------------------------------------
                if (probFileInfo.isOutput)
                {
                    std::ifstream inputStream(filePath);
                    std::stringstream outputBuffer;
                    outputBuffer << inputStream.rdbuf();

                    std::string responseText = outputBuffer.str();

                    m_ChatMessagePool->MarkAnswered(probFileInfo.id, responseText);

                    LOG_APP_INFO("ChatMessagePool: answered id {} via {}", probFileInfo.id, filename);

                    return; // stop here → do NOT send to SessionManager
                }

                // -----------------------------------------------------------------------
                // PROB INPUT (non-stale) → allow SessionManager to handle it below
                // -----------------------------------------------------------------------
                // fall through to SessionManager handling
            }
        }

        // -----------------------------------------------------------------------------------
        // Forward all other file events to the SessionManager system
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

        // ---------------------------------------------------------
        // Forward event to Python
        // ---------------------------------------------------------
        if (m_PythonEngine)
        {
            m_PythonEngine->OnEvent(eventPtr);
        }
    }

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
            m_PythonEngine->OnShutdown();
        }

        if (m_FileWatcher)
        {
            m_FileWatcher->Stop();
        }

        if (m_WebServer)
        {
            m_WebServer->Stop();
        }
    }

    bool JarvisAgent::IsFinished() const { return m_IsFinished; }

    void JarvisAgent::CheckIfFinished()
    {
        // not used
        // m_IsFinished = false;
        // Ctrl+C is caught by the engine and breaks the run loop
        // Also, `q` can be used to quit
    }

    int64_t JarvisAgent::GetStartupTimestamp() const
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(m_StartupTime.time_since_epoch()).count();
    }

} // namespace AIAssistant
