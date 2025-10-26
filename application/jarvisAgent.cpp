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

namespace AIAssistant
{
    std::unique_ptr<Application> JarvisAgent::Create() { return std::make_unique<JarvisAgent>(); }

    void JarvisAgent::OnStart()
    {
        LOG_APP_INFO("starting JarvisAgent version {}", JARVIS_AGENT_VERSION);

        const auto& queuePath = Core::g_Core->GetConfig().m_QueueFolderFilepath;

        m_FileWatcher = std::make_unique<FileWatcher>(queuePath, 100ms);
        m_FileWatcher->Start();
    }

    void JarvisAgent::OnUpdate()
    {
        for (auto& sessionManager : m_SessionManagers)
        {
            sessionManager.second->OnUpdate();
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

        // event was handled by the file categorizer
        if (!filePath.empty())
        {
            auto sessionManagerName = filePath.parent_path().string();
            if (!m_SessionManagers.contains(sessionManagerName))
            {
                m_SessionManagers[sessionManagerName] = std::make_unique<SessionManager>(sessionManagerName);
            }
            m_SessionManagers[sessionManagerName]->OnEvent(event);
        }
    }

    void JarvisAgent::OnShutdown()
    {
        LOG_APP_INFO("leaving JarvisAgent");
        if (m_FileWatcher)
        {
            m_FileWatcher->Stop();
        }
        Core::g_Core->GetThreadPool().Wait();
    }

    bool JarvisAgent::IsFinished() const { return m_IsFinished; }

    void JarvisAgent::CheckIfFinished()
    {
        // not used
        // m_IsFinished = false;
        // Ctrl+C is caught by the engine and breaks the run loop
    }
} // namespace AIAssistant
