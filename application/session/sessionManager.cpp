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
#include "keys/keyManager.h"
#include "simdjson/simdjson.h"
#include "workflow/aiRequestPool.h"

namespace AIAssistant
{
    SessionManager::SessionManager(std::string const& filePath) : m_Name{filePath}
    {
        auto apiIndex = Core::g_Core->GetConfig().m_ApiIndex;
        auto& api = Core::g_Core->GetConfig().m_ApiInterfaces[apiIndex];

        m_Url = api.m_Url;
        m_Model = api.m_Model;
        m_ApiType = (api.m_InterfaceType == ConfigParser::EngineConfig::InterfaceType::API1) ? "API1" : "API2";

        // Resolve API key from KeyManager default provider
        auto const* defaultProvider = Core::g_Core->GetKeyManager().GetDefaultProvider();
        if (defaultProvider)
        {
            m_ApiKey = defaultProvider->m_ApiKey;
        }
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
                .m_AllResponsesReceived = (m_QueryFutures.size() == 0),          // idle when no futures are outstanding
                .m_HasFailures = (m_FailedQueriesThisRun > 0)                    //
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
                                             m_QueryFutures.size(), m_CompletedQueriesThisRun, m_FailedQueriesThisRun,
                                             m_LastErrorCode, m_LastErrorMessage);
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
            msg["failed"] = m_FailedQueriesThisRun;
            if (m_LastErrorCode != 0)
            {
                msg["last_error_code"] = m_LastErrorCode;
                msg["last_error_message"] = m_LastErrorMessage;
            }
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

    void SessionManager::OnShutdown()
    {
        for (auto& future : m_QueryFutures)
        {
            if (future.valid())
            {
                future.wait_for(std::chrono::milliseconds(50));
            }
        }
        m_QueryFutures.clear();
    }

    bool SessionManager::IsIdle() const
    {
        auto const state = m_StateMachine.GetState();
        return state == StateMachine::State::AllResponsesReceived || state == StateMachine::State::Failed;
    }

    void SessionManager::DispatchQuery(TrackedFile& requirementFile)
    {
        // R"(...)" introduces a raw string literal in C++
        // 👉 No escape sequences (\n, \", \\, etc.) are interpreted.
        // 👉 Everything between the parentheses is taken literally — including newlines, backslashes, and quotes.
        //
        // example request data (model: gpt-4.1, content: Hello from C++!), all quotes are part of json format:
        // {"model": "gpt-4.1","messages": [{"role": "user", "content": "Hello from C++!"}]}
        // -----------+++++++--------------------------------------------+++++++++++++++----
        auto makeRequestDataAPI1 = [](std::string const& model, std::string const& message,
                                      std::optional<double> temperature) -> std::string
        {
            std::string json =
                R"({"model": ")" + model + R"(","messages": [{"role": "user", "content": ")" + message + R"("}])";
            if (temperature.has_value())
            {
                json += R"(,"temperature": )" + std::to_string(temperature.value());
            }
            json += "}";
            return json;
        };

        auto makeRequestDataAPI2 = [](std::string const& model, std::string const& message, std::string const& store,
                                      std::optional<double> temperature) -> std::string
        {
            std::string json = R"({"model": ")" + model + R"(", "input": ")" + message + R"(", "store": )" + store;
            if (temperature.has_value())
            {
                json += R"(, "temperature": )" + std::to_string(temperature.value());
            }
            json += "}";
            return json;
        };

        // retrieve prompt data from queue
        std::string message = m_Environment.GetEnvironmentAndResetDirtyFlag();

        auto fileContent = requirementFile.GetContent();
        message += fileContent;

        auto sanitizedMessage = JsonHelper().SanitizeForJson(message);

        bool store{false};
        std::string requestData;
        if (m_ApiType == "API2")
        {
            requestData = makeRequestDataAPI2(m_Model, sanitizedMessage, store ? "true" : "false", m_Temperature);
        }
        else
        {
            requestData = makeRequestDataAPI1(m_Model, sanitizedMessage, m_Temperature);
        }

        CurlWrapper::QueryData queryData = {
            .m_Url = m_Url,        //
            .m_Data = requestData, //
            .m_ApiKey = m_ApiKey   //
        };

        auto& threadpool = Core::g_Core->GetThreadPool();
        std::string inputFilename = requirementFile.GetPath().string();
        auto query = [this, queryData, inputFilename]() -> QueryResult
        {
            try
            {
                auto& curl = CurlManager::GetThreadCurl();
                curl.Clear();

                QueryResult curlResult = curl.Query(queryData);

                // Always create a parser, even if curl failed (empty buffer)
                auto interfaceType = Core::g_Core->GetInterfaceType();
                m_ReplyParser = ReplyParser::Create(interfaceType, curlResult.m_Ok ? curl.GetBuffer() : "");

                if (!curlResult.m_Ok)
                {
                    LOG_APP_ERROR("Query failed for '{}' (error {}): {}", inputFilename, curlResult.m_ErrorCode,
                                  curlResult.m_ErrorMessage);
                    return curlResult;
                }

                // Parser error?
                if (m_ReplyParser->HasError())
                {
                    return QueryResult::Fail(QueryErrorCode::ParserError,
                                             "Response parser error for '" + inputFilename + "'");
                }

                size_t hasContent = m_ReplyParser->HasContent();
                if (hasContent == 0)
                {
                    LOG_APP_WARN("No content returned for '{}'", inputFilename);
                    return QueryResult::Fail(QueryErrorCode::EmptyResponse,
                                             "Empty response from AI for '" + inputFilename + "'");
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

                return QueryResult::Ok();
            }
            catch (const std::exception& e)
            {
                LOG_APP_ERROR("Exception in query thread for '{}': {}", inputFilename, e.what());
                return QueryResult::Fail(QueryErrorCode::ExceptionThrown, e.what());
            }
        };

        m_QueryFutures.push_back(threadpool.SubmitTask(query));

        // Signal the AiRequestPool that curl was dispatched for this PROB file.
        // This confirms the handoff from file-placement to HTTP dispatch.
        JarvisAgent* jarvisAgent = dynamic_cast<JarvisAgent*>(App::g_App);
        if (jarvisAgent != nullptr)
        {
            AiRequestPool* requestPool = jarvisAgent->GetAiRequestPool();
            if (requestPool != nullptr)
            {
                requestPool->OnCurlDispatched(inputFilename);
            }
        }
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

        // PROV files: resolve provider/model/key overrides.
        // NOT part of the environment (content is never sent to AI).
        if (categorized.m_Provider.GetDirty())
        {
            AssembleProvider();
            m_FileCategorizer.GetCategorizedFiles().m_Provider.SetDirty(false);
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
                        QueryResult result = future.get();
                        if (result.m_Ok)
                        {
                            ++m_CompletedQueriesThisRun;
                        }
                        else
                        {
                            ++m_FailedQueriesThisRun;
                            m_LastErrorCode = result.m_ErrorCode;
                            m_LastErrorMessage = result.m_ErrorMessage;

                            LOG_APP_ERROR("[{}] query failed (error {}): {}", m_Name, result.m_ErrorCode,
                                          result.m_ErrorMessage);

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

    void SessionManager::AssembleProvider()
    {
        auto& provider = m_FileCategorizer.GetCategorizedFiles().m_Provider;
        if (provider.GetModifiedFiles() == 0)
        {
            return;
        }

        auto& map = provider.m_Map;
        for (auto& element : map)
        {
            bool isModified = element.second->IsModified();
            element.second->MarkModified(false);
            if (!isModified)
            {
                continue;
            }
            provider.DecrementModifiedFiles();

            auto content = element.second->GetContent();
            simdjson::ondemand::parser parser;
            simdjson::padded_string const padded(content);

            // simdjson ondemand is single-pass, so re-iterate for each field.
            // IMPORTANT: string_views from get_string() point into the parser's
            // internal buffer, which is overwritten on each parser.iterate() call.
            // Convert to std::string immediately to avoid dangling views.
            std::string urlStr;
            std::string providerStr;
            std::string modelStr;
            std::string apiTypeStr;
            double temperatureVal{};

            {
                auto d = parser.iterate(padded);
                std::string_view sv;
                if (d["url"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    urlStr = std::string(sv);
                }
            }
            {
                auto d = parser.iterate(padded);
                std::string_view sv;
                if (d["provider"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    providerStr = std::string(sv);
                }
            }
            {
                auto d = parser.iterate(padded);
                std::string_view sv;
                if (d["model"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    modelStr = std::string(sv);
                }
            }
            {
                auto d = parser.iterate(padded);
                std::string_view sv;
                if (d["api_type"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    apiTypeStr = std::string(sv);
                }
            }
            bool hasTemp = false;
            {
                auto d = parser.iterate(padded);
                hasTemp = (d["temperature"].get_double().get(temperatureVal) == simdjson::SUCCESS);
            }

            // url (mandatory — read from file, not from KeyManager)
            if (!urlStr.empty())
            {
                m_Url = urlStr;
            }
            else
            {
                LOG_CORE_WARN("SessionManager '{}': PROV file missing mandatory 'url' field", m_Name);
            }

            // model (optional)
            if (!modelStr.empty())
            {
                m_Model = modelStr;
            }

            // api_type (optional)
            if (!apiTypeStr.empty())
            {
                m_ApiType = apiTypeStr;
            }

            // temperature (optional)
            if (hasTemp)
            {
                m_Temperature = temperatureVal;
            }

            // provider name → KeyManager lookup for API key ONLY (credentials never in PROV file)
            if (!providerStr.empty())
            {
                auto const* prov = Core::g_Core->GetKeyManager().GetProvider(providerStr);
                if (prov)
                {
                    if (!prov->m_ApiKey.empty())
                    {
                        m_ApiKey = prov->m_ApiKey;
                    }
                    LOG_CORE_INFO("SessionManager '{}': provider '{}' key resolved from KeyManager", m_Name, providerStr);
                }
                else
                {
                    LOG_CORE_WARN("SessionManager '{}': provider '{}' not found in KeyManager (no API key)", m_Name,
                                  providerStr);
                }
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
                    m_State = stateInfo.m_HasFailures ? State::Failed : State::AllResponsesReceived;
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
            case State::Failed:
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
