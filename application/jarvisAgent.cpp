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
        m_Url = Core::g_Core->GetConfig().m_Url;
        m_Model = Core::g_Core->GetConfig().m_Model;
        const auto& queuePath = Core::g_Core->GetConfig().m_QueueFolderFilepath;

        m_FileWatcher = std::make_unique<FileWatcher>(queuePath, 100ms);
        m_FileWatcher->Start();
    }

    void JarvisAgent::OnUpdate()
    {
        m_Curl.Clear();

        // R"(...)" introduces a raw string literal in C++
        // 👉 No escape sequences (\n, \", \\, etc.) are interpreted.
        // 👉 Everything between the parentheses is taken literally — including newlines, backslashes, and quotes.
        //
        // example request data (model: gpt-4.1, content: Hello from C++!), all quotes are part of json format:
        // {"model": "gpt-4.1","messages": [{"role": "user", "content": "Hello from C++!"}]}
        // -----------+++++++--------------------------------------------+++++++++++++++----
        auto makeRequestData = [](std::string const& model, std::string const& message) -> std::string
        { return R"({"model": ")" + model + R"(","messages": [{"role": "user", "content": ")" + message + R"("}]})"; };

        // retrieve prompt data from queue
        std::string message = "Hello from C++!";
        CurlWrapper::QueryData queryData = {
            .m_Url = m_Url,                             //
            .m_Data = makeRequestData(m_Model, message) //
        };

        bool curleOk = true; // m_Curl.Query(queryData);

        if (!curleOk)
        {
            auto appErrorEvent = std::make_shared<AppErrorEvent>(AppErrorEvent::AppErrorBadCurl);
            Core::g_Core->PushEvent(appErrorEvent);
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
                    LOG_CORE_INFO("App received shutdown request");
                    m_IsFinished = true;
                }
                else
                {
                    LOG_CORE_ERROR("unhandled engine event");
                }
                return true;
            });

        dispatcher.Dispatch<FileAddedEvent>(
            [&](FileAddedEvent& fileEvent)
            {
                LOG_APP_INFO("New file detected: {}", fileEvent.GetPath());
                m_FileCategorizer.AddFile(fileEvent.GetPath());
                return true;
            });

        dispatcher.Dispatch<FileModifiedEvent>(
            [&](FileModifiedEvent& fileEvent)
            {
                LOG_APP_INFO("File modified: {}", fileEvent.GetPath());
                m_FileCategorizer.ModifyFile(fileEvent.GetPath());
                return true;
            });

        dispatcher.Dispatch<FileRemovedEvent>(
            [&](FileRemovedEvent& fileEvent)
            {
                LOG_APP_INFO("File removed: {}", fileEvent.GetPath());
                m_FileCategorizer.RemoveFile(fileEvent.GetPath());
                return true;
            });
    }

    void JarvisAgent::OnShutdown()
    {
        if (m_FileWatcher)
        {
            m_FileWatcher->Stop();
        }
        LOG_APP_INFO("leaving JarvisAgent");
    }

    bool JarvisAgent::IsFinished() { return m_IsFinished; }

    void JarvisAgent::CheckIfFinished()
    {
        // not used
        // m_IsFinished = false;
        // Ctrl+C is caught by the engine and breaks the run loop
    }
} // namespace AIAssistant
