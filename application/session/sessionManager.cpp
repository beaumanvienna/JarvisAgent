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

#include "session/sessionManager.h"
#include "event/events.h"
#include "json/jsonHelper.h"

namespace AIAssistant
{
    SessionManager::SessionManager(std::string const& filePath) : m_Name{filePath}
    {

        auto apiIndex = Core::g_Core->GetConfig().m_ApiIndex;
        auto& api = Core::g_Core->GetConfig().m_ApiInterfaces[apiIndex];

        m_Url = api.m_Url;
        m_Model = api.m_Model;
    }
    void SessionManager::OnUpdate()
    {
        CheckForUpdates();
        TrackInFlightQueries();

        { // update statemachine
            auto& requirements = m_FileCategorizer.GetCategorizedFiles().m_Requirements;
            bool queriesChanged = (requirements.GetModifiedFiles() != 0);
            bool allQueriesSent = (requirements.GetModifiedFiles() == 0);

            StateMachine::StateInfo stateInfo = {
                .m_EnvironmentChanged = m_Environment.GetDirty(),                //
                .m_EnvironmentComplete = m_Environment.GetEnvironmentComplete(), //
                .m_QueriesChanged = queriesChanged,                              //
                .m_AllQueriesSent = allQueriesSent,                              //
                .m_AllResponsesReceived = (m_QueryFutures.size() == 0)           // idle when no futures are outstanding
            };
            m_StateMachine.OnUpdate(stateInfo);
        }

        // limit queued queries to 1½ the number of configured threads
        // thread pool has queue but we can limit it here to safe queue memory
        if ((m_QueryFutures.size() < Core::g_Core->GetConfig().m_MaxThreads * 1.5f) &&
            m_Environment.GetEnvironmentComplete())
        {
            auto& map = m_FileCategorizer.GetCategorizedFiles().m_Requirements.Get();
            for (auto& element : map)
            {
                auto& trackedFile = *element.second;
                if (trackedFile.IsModified())
                {
                    DispatchQuery(trackedFile);
                    trackedFile.MarkModified(false);
                    auto& requirements = m_FileCategorizer.GetCategorizedFiles().m_Requirements;
                    requirements.DecrementModifiedFiles();
                }
            }
        }
    }

    void SessionManager::OnEvent(Event& event)
    {
        EventDispatcher dispatcher(event);
        fs::path filePath;

        dispatcher.Dispatch<FileAddedEvent>(
            [&](FileAddedEvent& fileEvent)
            {
                LOG_APP_INFO("New file detected: {}", fileEvent.GetPath());
                filePath = m_FileCategorizer.AddFile(fileEvent.GetPath());
                return true;
            });

        dispatcher.Dispatch<FileModifiedEvent>(
            [&](FileModifiedEvent& fileEvent)
            {
                LOG_APP_INFO("File modified: {}", fileEvent.GetPath());
                filePath = m_FileCategorizer.ModifyFile(fileEvent.GetPath());
                return true;
            });

        dispatcher.Dispatch<FileRemovedEvent>(
            [&](FileRemovedEvent& fileEvent)
            {
                LOG_APP_INFO("File removed: {}", fileEvent.GetPath());
                filePath = m_FileCategorizer.RemoveFile(fileEvent.GetPath());
                return true;
            });
    }

    void SessionManager::OnShutdown() { m_FileCategorizer.PrintCategorizedFiles(); }

    bool SessionManager::IsIdle() const { return m_StateMachine.GetState() == StateMachine::State::AllResponsesReceived; }

    void SessionManager::DispatchQuery(TrackedFile& requirementFile)
    {
        // R"(...)" introduces a raw string literal in C++
        // 👉 No escape sequences (\n, \", \\, etc.) are interpreted.
        // 👉 Everything between the parentheses is taken literally — including newlines, backslashes, and quotes.
        //
        // example request data (model: gpt-4.1, content: Hello from C++!), all quotes are part of json format:
        // {"model": "gpt-4.1","messages": [{"role": "user", "content": "Hello from C++!"}]}
        // -----------+++++++--------------------------------------------+++++++++++++++----
        auto makeRequestDataAPI1 = [](std::string const& model, std::string const& message) -> std::string
        { return R"({"model": ")" + model + R"(","messages": [{"role": "user", "content": ")" + message + R"("}]})"; };

        // example request data (model: gpt-4.1, content: Hello from C++!), all quotes are part of json format:
        // {"model": "gpt-4.1","messages": [{"role": "user", "content": "Hello from C++!"}]}
        // -----------+++++++--------------------------------------------+++++++++++++++----
        //{"model": "gpt-5-nano", "input": "write a haiku about ai", "store": true}
        // ----------++++++++++-------------++++++++++++++++++++++------------++++-
        auto makeRequestDataAPI2 = [](std::string const& model, std::string const& message,
                                      std::string const& store) -> std::string
        { return R"({"model": ")" + model + R"(", "input": ")" + message + R"(", "store": )" + store + "}"; };

        // retrieve prompt data from queue
        std::string message = m_Environment.GetEnvironmentAndResetDirtyFlag();

        auto fileContent = requirementFile.GetContentAndResetModified();
        message += fileContent;

        auto sanitizedMessage = JsonHelper().SanitizeForJson(message);

        bool store{false};
        std::string requestData;
        {
            auto apiIndex = Core::g_Core->GetConfig().m_ApiIndex;
            auto& api = Core::g_Core->GetConfig().m_ApiInterfaces[apiIndex];
            switch (api.m_InterfaceType)
            {
                case ConfigParser::EngineConfig::InterfaceType::API1:
                {
                    requestData = makeRequestDataAPI1(m_Model, sanitizedMessage);
                    break;
                }
                case ConfigParser::EngineConfig::InterfaceType::API2:
                {
                    requestData = makeRequestDataAPI2(m_Model, sanitizedMessage, store ? "true" : "false");
                    break;
                }
                default:
                    break; // no more checking here in the run loop
            };
        }

        CurlWrapper::QueryData queryData = {
            .m_Url = m_Url,       //
            .m_Data = requestData //
        };

        auto& threadpool = Core::g_Core->GetThreadPool();
        auto query = [this, queryData]()
        {
            auto& curl = CurlManager::GetThreadCurl();
            curl.Clear();
            return curl.Query(queryData);
        };
        m_QueryFutures.push_back(threadpool.SubmitTask(query));
    }

    void SessionManager::CheckForUpdates()
    {
        bool environmentUpdate{false};

        if (m_FileCategorizer.GetCategorizedFiles().m_Settings.GetDirty())
        {
            AssembleSettings();
            m_FileCategorizer.GetCategorizedFiles().m_Settings.SetDirty(false);
            environmentUpdate = true;
        }

        if (m_FileCategorizer.GetCategorizedFiles().m_Context.GetDirty())
        {
            AssembleContext();
            m_FileCategorizer.GetCategorizedFiles().m_Context.SetDirty(false);
            environmentUpdate = true;
        }

        if (m_FileCategorizer.GetCategorizedFiles().m_Tasks.GetDirty())
        {
            AssembleTask();
            m_FileCategorizer.GetCategorizedFiles().m_Tasks.SetDirty(false);
            environmentUpdate = true;
        }

        if (environmentUpdate)
        {
            m_Environment.Assemble(m_Settings, m_Context, m_Tasks);
        }
    }
    void SessionManager::TrackInFlightQueries()
    {
        // --- clean up futures and report result ---
        m_QueryFutures.erase(           //
            std::remove_if(             //
                m_QueryFutures.begin(), //
                m_QueryFutures.end(),   //
                [&](auto& future)
                {
                    if (future.wait_for(std::chrono::seconds(0s)) == std::future_status::ready)
                    {
                        bool curleOk = future.get();
                        // report bad curl to engine
                        if (!curleOk)
                        {
                            auto appErrorEvent = std::make_shared<AppErrorEvent>(AppErrorEvent::AppErrorBadCurl);
                            Core::g_Core->PushEvent(appErrorEvent);
                        }
                        return true; // remove from list
                    }
                    return false; // keep in list, we'll check next frame again
                }),
            m_QueryFutures.end());
    }

    void SessionManager::AssembleSettings()
    {
        auto& settings = m_FileCategorizer.GetCategorizedFiles().m_Settings;
        if (settings.GetModifiedFiles() == 0)
        {
            return;
        }

        auto& map = settings.m_Map;
        m_Settings.clear();
        for (auto& element : map)
        {
            bool isModified = element.second->IsModified();
            auto content = element.second->GetContentAndResetModified();
            m_Settings += content;
            if (isModified)
            {
                settings.DecrementModifiedFiles();
            }
        }
    }

    void SessionManager::AssembleContext()
    {
        auto& context = m_FileCategorizer.GetCategorizedFiles().m_Context;
        if (context.GetModifiedFiles() == 0)
        {
            return;
        }

        auto& map = context.m_Map;
        m_Context.clear();
        for (auto& element : map)
        {
            bool isModified = element.second->IsModified();
            auto content = element.second->GetContentAndResetModified();
            m_Context += content;
            if (isModified)
            {
                context.DecrementModifiedFiles();
            }
        }
    }

    void SessionManager::AssembleTask()
    {
        auto& tasks = m_FileCategorizer.GetCategorizedFiles().m_Tasks;
        if (tasks.GetModifiedFiles() == 0)
        {
            return;
        }

        auto& map = tasks.m_Map;
        m_Tasks.clear();
        for (auto& element : map)
        {
            bool isModified = element.second->IsModified();
            auto content = element.second->GetContentAndResetModified();
            m_Tasks += content;
            if (isModified)
            {
                tasks.DecrementModifiedFiles();
            }
        }
    }

    void SessionManager::Environment::Assemble(std::string& settings, std::string& context, std::string& tasks)
    {
        m_Dirty = true;
        m_EnvironmentComplete = false;

        if (settings.empty())
        {
            return;
        }

        if (context.empty())
        {
            return;
        }

        if (tasks.empty())
        {
            return;
        }

        // minimum requirement met:
        // at least one settings, context, tasks found
        m_EnvironmentCombined = settings + context + tasks;
        m_EnvironmentComplete = true;
    }

    std::string& SessionManager::Environment::GetEnvironmentAndResetDirtyFlag()
    {
        m_Dirty = false;
        return m_EnvironmentCombined;
    }

    void SessionManager::Environment::Reset()
    {
        m_Dirty = false;
        m_EnvironmentComplete = false;
    }

    void SessionManager::StateMachine::OnUpdate(StateInfo& stateInfo)
    {
        State oldState = m_State;

        switch (m_State)
        {
            case State::CompilingEnvironment:
            {
                if (stateInfo.m_EnvironmentComplete)
                {
                    m_State = State::SendingQueries;
                }
                break;
            }
            case State::SendingQueries:
            {
                if (stateInfo.m_AllQueriesSent)
                {
                    m_State = State::AllQueriesSent;
                }
                break;
            }
            case State::AllQueriesSent:
            {
                if (stateInfo.m_AllResponsesReceived)
                {
                    m_State = State::AllResponsesReceived;
                }
                break;
            }
            case State::AllResponsesReceived:
            {
                if (stateInfo.m_EnvironmentChanged)
                {
                    m_State = State::CompilingEnvironment;
                }
                else if (stateInfo.m_QueriesChanged)
                {
                    m_State = State::SendingQueries;
                }
                break;
            }
            default:
            {
                break;
            }
        }
        if (oldState != m_State)
        {
            LOG_APP_INFO("State changed: {} → {}", StateNames[oldState], StateNames[m_State]);
        }
    }

} // namespace AIAssistant
