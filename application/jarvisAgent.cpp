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

namespace AIAssistant
{
    JarvisAgent* App::g_App = nullptr;
    std::unique_ptr<Application> JarvisAgent::Create() { return std::make_unique<JarvisAgent>(); }

    void JarvisAgent::OnStart()
    {
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
            LOG_APP_WARN("PythonEngine failed to initialize. Continuing without Python scripting.");
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

    void JarvisAgent::OnEvent(Event& event)
    {
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
        // -----------------------------------------------------------------------------------

        if (!filePath.empty())
        {
            std::string filename = filePath.filename().string();

            if (filename.starts_with("PROB_") && filename.ends_with(".output.txt"))
            {
                try
                {
                    // Extract the ID
                    size_t pos1 = filename.find('_');           // after PROB
                    size_t pos2 = filename.find('_', pos1 + 1); // after <id>
                    std::string idStr = filename.substr(pos1 + 1, pos2 - (pos1 + 1));
                    uint64_t id = std::stoull(idStr);

                    // Read response text
                    std::ifstream in(filePath);
                    std::stringstream buffer;
                    buffer << in.rdbuf();
                    std::string answer = buffer.str();

                    // Pass to chat pool
                    m_ChatMessagePool->MarkAnswered(id, answer);

                    LOG_APP_INFO("ChatMessagePool: answered id {} via {}", id, filename);
                }
                catch (const std::exception& e)
                {
                    LOG_APP_ERROR("Failed processing PROB_.output: {} ({})", filePath.string(), e.what());
                }

                // we do NOT forward this to SessionManager
                return;
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
            m_PythonEngine->OnEvent(event);
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
} // namespace AIAssistant
