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
#include "session/fileWriter.h"
#include "web/webServer.h"

#include "core.h"
#include "event/events.h"
#include "json/jsonHelper.h"
#include "log/statusRenderer.h"
#include "auxiliary/file.h"

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
            bool anyQueryDispatched = false;
            auto& map = m_FileCategorizer.GetCategorizedFiles().m_Requirements.Get();
            for (auto& element : map)
            {
                auto& trackedFile = *element.second;
                if (trackedFile.IsModified())
                {
                    if (IsQueryRequired(trackedFile))
                    {
                        DispatchQuery(trackedFile);
                        anyQueryDispatched = true;
                    }
                    trackedFile.MarkModified(false);
                    auto& requirements = m_FileCategorizer.GetCategorizedFiles().m_Requirements;
                    requirements.DecrementModifiedFiles();
                }
            }

            // If environment was dirty but no queries needed to run, reset it
            if (!anyQueryDispatched && m_Environment.GetDirty())
            {
                LOG_APP_INFO("All outputs up-to-date → resetting environment dirty flag");
                m_Environment.SetDirty(false);
            }
        }

        { // status display in terminal (ncurses status panel)
            JarvisAgent* jarvisAgent = dynamic_cast<JarvisAgent*>(App::g_App);
            if (jarvisAgent != nullptr)
            {
                StatusRenderer& statusRenderer = jarvisAgent->GetStatusRenderer();
                statusRenderer.UpdateSession(m_Name, SessionManager::StateMachine::StateNames[m_StateMachine.GetState()],
                                             m_FileCategorizer.GetCategorizedFiles().m_Requirements.m_Map.size(),
                                             m_QueryFutures.size(), m_CompletedQueriesThisRun);
            }
        }

        { // remote status display via web server
            auto& webServer = *App::g_App->GetWebServer();
            crow::json::wvalue msg;
            msg["type"] = "status";
            msg["name"] = m_Name;
            msg["state"] = std::string(SessionManager::StateMachine::StateNames[m_StateMachine.GetState()]);
            msg["outputs"] = m_FileCategorizer.GetCategorizedFiles().m_Requirements.m_Map.size();
            msg["inflight"] = m_QueryFutures.size();
            msg["completed"] = m_CompletedQueriesThisRun;
            webServer.BroadcastJSON(msg.dump());
        }
    }

    bool SessionManager::IsQueryRequired(TrackedFile& requirementFile) const
    {

        fs::path requirementPath = requirementFile.GetPath();
        fs::path outputPath = requirementPath;
        outputPath.replace_filename(requirementPath.stem().string() + ".output" + requirementPath.extension().string());

        try
        {
            // Requirement timestamp
            if (!fs::exists(requirementPath))
            {
                LOG_APP_WARN("Requirement file missing: {}", requirementPath.string());
                return false;
            }

            // If no output yet, definitely re-send
            if (!fs::exists(outputPath))
            {
                LOG_APP_INFO("No output found for '{}', scheduling query", requirementPath.string());
                return true;
            }

            auto requirementTime = fs::last_write_time(requirementPath);
            auto environmentTime = m_Environment.GetTimestamp();

            // Determine the newest relevant input time
            auto newestInputTime = (requirementTime > environmentTime) ? requirementTime : environmentTime;
            auto outputTime = fs::last_write_time(outputPath);

            // Re-send if input newer than output
            if (newestInputTime > outputTime)
            {
                LOG_APP_INFO("Re-scheduling '{}': input/environment newer than output", requirementPath.string());
                return true;
            }

            // Otherwise, up to date
            LOG_APP_INFO("Skipping '{}': output is up-to-date", requirementPath.string());
            return false;
        }
        catch (const std::exception& e)
        {
            LOG_APP_WARN("Failed to check timestamps for '{}': {}", requirementPath.string(), e.what());
            return false; // file not sendable
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

    void SessionManager::OnShutdown() {}

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

        // example request data (model: gpt-5-nano, content: write a haiku about ai), all quotes are part of json format:
        //{"model": "gpt-5-nano", "input": "write a haiku about ai", "store": true}
        // ----------++++++++++-------------++++++++++++++++++++++------------++++-
        auto makeRequestDataAPI2 = [](std::string const& model, std::string const& message,
                                      std::string const& store) -> std::string
        { return R"({"model": ")" + model + R"(", "input": ")" + message + R"(", "store": )" + store + "}"; };

        // retrieve prompt data from queue
        std::string message = m_Environment.GetEnvironmentAndResetDirtyFlag();

        auto fileContent = requirementFile.GetContent();
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
        std::string inputFilename = requirementFile.GetPath().string();
        auto query = [this, queryData, inputFilename]() -> bool
        {
            try
            {
                auto& curl = CurlManager::GetThreadCurl();
                curl.Clear();

                bool ok = curl.Query(queryData);

                // Always create a parser, even if curl failed (empty buffer)
                auto interfaceType = Core::g_Core->GetInterfaceType();
                m_ReplyParser = ReplyParser::Create(interfaceType, ok ? curl.GetBuffer() : "");

                // If curl itself failed → safe exit
                if (!ok)
                {
                    LOG_APP_ERROR("Curl network error while processing: {}", inputFilename);
                    return false;
                }

                // Parser error?
                if (m_ReplyParser->HasError())
                {
                    return false;
                }

                size_t hasContent = m_ReplyParser->HasContent();
                if (hasContent == 0)
                {
                    LOG_APP_WARN("No content returned for '{}'", inputFilename);
                    return false;
                }

                // Write all returned content blocks
                for (size_t index = 0; index < hasContent; ++index)
                {
                    std::string contentText = m_ReplyParser->GetContent(index);
                    LOG_APP_INFO("message:");
                    std::cout << contentText << "\n";

                    fs::path outputPath(inputFilename);
                    outputPath.replace_filename(outputPath.stem().string() + ".output" + outputPath.extension().string());

                    FileWriter::Get().WriteWithHeader(outputPath, contentText, m_Model);
                }

                return true;
            }
            catch (const std::exception& e)
            {
                LOG_APP_ERROR("Exception in query thread for '{}': {}", inputFilename, e.what());
                return false;
            }
        };

        m_QueryFutures.push_back(threadpool.SubmitTask(query));
    }

    void SessionManager::CheckForUpdates()
    {
        bool environmentUpdate{false};

        auto& categorized = m_FileCategorizer.GetCategorizedFiles();

        if (categorized.m_Settings.GetDirty())
        {
            AssembleSettings();
            m_FileCategorizer.GetCategorizedFiles().m_Settings.SetDirty(false);
            environmentUpdate = true;
        }

        if (categorized.m_Context.GetDirty())
        {
            AssembleContext();
            m_FileCategorizer.GetCategorizedFiles().m_Context.SetDirty(false);
            environmentUpdate = true;
        }

        if (categorized.m_Tasks.GetDirty())
        {
            AssembleTask();
            m_FileCategorizer.GetCategorizedFiles().m_Tasks.SetDirty(false);
            environmentUpdate = true;
        }

        if (environmentUpdate)
        {
            m_Environment.Assemble(m_Settings, m_Context, m_Tasks, categorized);

            // Mark all requirements as modified since their environment changed
            auto& requirements = m_FileCategorizer.GetCategorizedFiles().m_Requirements;
            for (auto& element : requirements.m_Map)
            {
                if (!element.second->IsModified())
                {
                    element.second->MarkModified();
                    requirements.IncrementModifiedFiles();
                }
            }

            LOG_APP_INFO("Environment updated → all requirements marked for dependency recheck");
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
                        ++m_CompletedQueriesThisRun;
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
            element.second->MarkModified(false);
            auto content = element.second->GetContent();
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
            element.second->MarkModified(false);
            auto content = element.second->GetContent();
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
            element.second->MarkModified(false);
            auto content = element.second->GetContent();
            m_Tasks += content;
            if (isModified)
            {
                tasks.DecrementModifiedFiles();
            }
        }
    }

    void SessionManager::Environment::Assemble(std::string& settings, std::string& context, std::string& tasks,
                                               CategorizedFiles& categorized)
    {
        SetEnvironmentComplete(false);

        if (settings.empty() || context.empty() || tasks.empty())
        {
            m_Timestamp = fs::file_time_type::min();
            m_Dirty = false;
            return;
        }

        // Combine environment parts
        std::string environmentCombined = settings + context + tasks;

        // Compare with previous
        if (environmentCombined != m_EnvironmentCombined)
        {
            m_EnvironmentCombined = std::move(environmentCombined);
            m_Timestamp = ComputeTimestamp(categorized);
            m_Dirty = true;
        }
        else
        {
            m_Dirty = false;
        }

        SetEnvironmentComplete(true);
    }

    fs::file_time_type SessionManager::Environment::ComputeTimestamp(CategorizedFiles& categorized) const
    {
        std::vector<fs::path> envFiles;

        envFiles.reserve(categorized.m_Settings.m_Map.size() + categorized.m_Context.m_Map.size() +
                         categorized.m_Tasks.m_Map.size());

        for (auto const& keyValue : categorized.m_Settings.m_Map)
        {
            envFiles.push_back(keyValue.second->GetPath());
        }

        for (auto const& keyValue : categorized.m_Context.m_Map)
        {
            envFiles.push_back(keyValue.second->GetPath());
        }

        for (auto const& keyValue : categorized.m_Tasks.m_Map)
        {
            envFiles.push_back(keyValue.second->GetPath());
        }

        return EngineCore::GetNewestTimestamp(envFiles);
    }

    std::string& SessionManager::Environment::GetEnvironmentAndResetDirtyFlag()
    {
        m_Dirty = false;
        return m_EnvironmentCombined;
    }

    void SessionManager::Environment::SetDirty(bool dirty) { m_Dirty = dirty; }

    void SessionManager::Environment::SetEnvironmentComplete(bool complete) { m_EnvironmentComplete = complete; }

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
