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

#include "assistant/assistantController.h"
#include "assistant/contextAssembler.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "python/pythonEngine.h"
#include "workflow/aiRequestPool.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowRuntimeManager.h"
#include "crow.h"

#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

// simdjson for incoming message parsing
#include "simdjson/simdjson.h"

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace
{
    // On Windows, _popen routes through cmd.exe which cannot run POSIX commands.
    // Wrap the command in bash (MSYS2 / Git Bash) to match the workflow engine.
    std::string WrapForBash([[maybe_unused]] std::string const& cmd)
    {
#if defined(_WIN32)
        return "bash -c \"" + cmd + "\"";
#else
        return cmd;
#endif
    }

    namespace fs = std::filesystem;

    std::string JsonEscape(std::string const& input)
    {
        std::string out;
        out.reserve(input.size() + 16);
        for (char c : input)
        {
            switch (c)
            {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    }

    int64_t NowTimestampNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    fs::path GetQueueBasePath() { return fs::absolute(AIAssistant::Core::g_Core->GetConfig().m_QueueFolderFilepath); }
} // namespace

namespace AIAssistant
{
    AssistantController::AssistantController()
        : m_MemoryStore(fs::absolute("assistant/memory.json")), m_WorkspaceIndexer(fs::absolute("assistant/index"))
    {
        m_ToolRegistry.SetMemoryStore(&m_MemoryStore);
        m_ToolRegistry.SetWorkspaceIndexer(&m_WorkspaceIndexer);

        // Provide the tool registry with a callback for making blocking AI calls
        // (used by get_file_summary to generate summaries on-demand).
        m_ToolRegistry.SetAiCallFn([this](std::string const& sysPrompt, std::string const& userPrompt, std::string& outResp,
                                          std::string& outErr) -> bool
                                   { return MakeToolAiCall(sysPrompt, userPrompt, outResp, outErr); });

        // Run initial workspace scan (indexes files, preserves cached summaries).
        m_WorkspaceIndexer.ScanWorkspace();

        LOG_APP_INFO("[assistant] AssistantController created ({} memories, {} indexed files, {} summaries)",
                     m_MemoryStore.Size(), m_WorkspaceIndexer.FileCount(), m_WorkspaceIndexer.SummaryCount());
    }

    AssistantController::~AssistantController() { Shutdown(); }

    void AssistantController::SetWorkflowRegistry(WorkflowRegistry* registry)
    {
        m_WorkflowRegistry = registry;
        m_ToolRegistry.SetWorkflowRegistry(registry);
    }

    void AssistantController::SetWorkflowRuntimeManager(WorkflowRuntimeManager* runtimeManager)
    {
        m_WorkflowRuntimeManager = runtimeManager;
        m_ToolRegistry.SetWorkflowRuntimeManager(runtimeManager);
    }

    // -----------------------------------------------------------------
    // WebSocket callbacks
    // -----------------------------------------------------------------

    void AssistantController::OnOpen(crow::websocket::connection& conn)
    {
        {
            std::lock_guard<std::mutex> lock(m_ClientsMutex);
            m_Clients.insert(&conn);
            m_ClientStates[&conn] = ClientState{};
        }
        LOG_APP_INFO("[assistant] Client connected (total: {})", m_Clients.size());
    }

    void AssistantController::OnClose(crow::websocket::connection& conn)
    {
        {
            std::lock_guard<std::mutex> lock(m_ClientsMutex);
            m_Clients.erase(&conn);
            m_ClientStates.erase(&conn);
        }
        LOG_APP_INFO("[assistant] Client disconnected (remaining: {})", m_Clients.size());
    }

    void AssistantController::OnMessage(crow::websocket::connection& conn, std::string const& data)
    {
        try
        {
            simdjson::ondemand::parser parser;
            simdjson::padded_string json(data);
            auto doc = parser.iterate(json);

            std::string type = std::string(doc["type"].get_string().value());

            if (type == "ping")
            {
                // Heartbeat — just drain pending messages.
            }
            else if (type == "user_message")
            {
                std::string sessionId;
                {
                    std::string_view sv;
                    if (doc["sessionId"].get_string().get(sv) == simdjson::SUCCESS)
                        sessionId = std::string(sv);
                }
                std::string text = std::string(doc["text"].get_string().value());
                HandleUserMessage(conn, sessionId, text);
            }
            else if (type == "command")
            {
                std::string sessionId;
                {
                    std::string_view sv;
                    if (doc["sessionId"].get_string().get(sv) == simdjson::SUCCESS)
                        sessionId = std::string(sv);
                }
                std::string command = std::string(doc["command"].get_string().value());
                std::string args;
                {
                    std::string_view sv;
                    if (doc["args"].get_string().get(sv) == simdjson::SUCCESS)
                        args = std::string(sv);
                }
                HandleCommand(conn, sessionId, command, args);
            }
            else if (type == "list_sessions")
            {
                HandleListSessions(conn);
            }
            else if (type == "resume_session")
            {
                std::string sessionId = std::string(doc["sessionId"].get_string().value());
                HandleResumeSession(conn, sessionId);
            }
            else if (type == "new_session")
            {
                HandleNewSession(conn);
            }
            else if (type == "approval_response")
            {
                std::string requestId = std::string(doc["requestId"].get_string().value());
                bool approved = false;
                {
                    bool val;
                    if (doc["approved"].get_bool().get(val) == simdjson::SUCCESS)
                        approved = val;
                }
                HandleApprovalResponse(requestId, approved);
            }
            else if (type == "get_history")
            {
                int maxEntries = 500;
                {
                    int64_t val;
                    if (doc["maxEntries"].get_int64().get(val) == simdjson::SUCCESS)
                        maxEntries = static_cast<int>(val);
                }
                HandleGetHistory(conn, maxEntries);
            }
            else if (type == "completion_request")
            {
                std::string prefix = std::string(doc["prefix"].get_string().value());
                std::string kind;
                {
                    std::string_view sv;
                    if (doc["kind"].get_string().get(sv) == simdjson::SUCCESS)
                        kind = std::string(sv);
                }
                HandleCompletionRequest(conn, prefix, kind);
            }
            else
            {
                QueueMessage(R"({"type":"error","message":"Unknown message type: )" + JsonEscape(type) + "\"}");
            }
        }
        catch (std::exception const& e)
        {
            LOG_APP_WARN("[assistant] Error parsing message: {}", e.what());
            QueueMessage(std::string(R"({"type":"error","message":"Parse error: )") + JsonEscape(e.what()) + "\"}");
        }

        DrainPendingMessages();
    }

    // -----------------------------------------------------------------
    // Message handlers
    // -----------------------------------------------------------------

    void AssistantController::HandleUserMessage(crow::websocket::connection& conn, std::string const& sessionId,
                                                std::string const& text)
    {
        if (text.empty())
            return;

        // Resolve session — create if empty/missing.
        AssistantSession* session = nullptr;
        std::string resolvedSessionId = sessionId;

        if (sessionId.empty())
        {
            session = CreateSession();
            resolvedSessionId = session->GetSessionId();
        }
        else
        {
            session = GetSession(sessionId);
            if (!session)
            {
                session = CreateSession();
                resolvedSessionId = session->GetSessionId();
            }
        }

        // Update per-connection state.
        {
            std::lock_guard<std::mutex> lock(m_ClientsMutex);
            if (m_ClientStates.count(&conn))
                m_ClientStates[&conn].activeSessionId = resolvedSessionId;
        }

        // Notify client which session is active.
        QueueMessage("{\"type\":\"session_active\",\"sessionId\":\"" + JsonEscape(resolvedSessionId) + "\"}");

        // Check for slash commands.
        if (text.size() > 1 && text[0] == '/')
        {
            auto spacePos = text.find(' ');
            std::string cmd = text.substr(1, spacePos == std::string::npos ? std::string::npos : spacePos - 1);
            std::string cmdArgs = spacePos != std::string::npos ? text.substr(spacePos + 1) : "";

            // Record in session history.
            session->AddUserMessage(text);

            HandleCommand(conn, resolvedSessionId, cmd, cmdArgs);
            return;
        }

        // Record user message.
        session->AddUserMessage(text);

        // Send "thinking" indicator.
        QueueMessage("{\"type\":\"assistant_thinking\",\"sessionId\":\"" + JsonEscape(resolvedSessionId) + "\"}");

        // Dispatch AI call on background thread.
        RunAiCallAsync(resolvedSessionId, text);
    }

    void AssistantController::HandleCommand(crow::websocket::connection& /*conn*/, std::string const& sessionId,
                                            std::string const& command, std::string const& args)
    {
        std::string response;

        if (command == "help")
        {
            response = HandleHelpCommand();
        }
        else if (command == "status")
        {
            response = HandleStatusCommand();
        }
        else if (command == "runs")
        {
            response = HandleRunsCommand();
        }
        else if (command == "clear")
        {
            QueueMessage("{\"type\":\"clear\",\"sessionId\":\"" + JsonEscape(sessionId) + "\"}");
            return;
        }
        else if (command == "new")
        {
            auto* session = CreateSession();
            QueueMessage("{\"type\":\"session_active\",\"sessionId\":\"" + JsonEscape(session->GetSessionId()) + "\"}");
            QueueMessage("{\"type\":\"assistant_done\",\"sessionId\":\"" + JsonEscape(session->GetSessionId()) +
                         "\",\"text\":\"New session started.\"}");
            return;
        }
        else if (command == "log")
        {
            response = HandleLogCommand(args);
        }
        else if (command == "memory")
        {
            response = HandleMemoryCommand(args);
        }
        else if (command == "index")
        {
            response = HandleIndexCommand(args);
        }
        else if (command == "sessions")
        {
            auto ids = AssistantSession::ListSessions(GetSessionsDir());
            std::string list = "Available sessions:\n";
            for (auto const& id : ids)
            {
                list += "  " + id + "\n";
            }
            QueueMessage("{\"type\":\"assistant_done\",\"sessionId\":\"" + JsonEscape(sessionId) + "\",\"text\":\"" +
                         JsonEscape(list) + "\"}");
            return;
        }
        else
        {
            response = "Unknown command: /" + command + "\nType /help for available commands.";
        }

        // Record response in session if we have one.
        if (!sessionId.empty())
        {
            auto* session = GetSession(sessionId);
            if (session)
            {
                session->AddAssistantMessage(response);
            }
        }

        QueueMessage("{\"type\":\"assistant_done\",\"sessionId\":\"" + JsonEscape(sessionId) + "\",\"text\":\"" +
                     JsonEscape(response) + "\"}");
    }

    void AssistantController::HandleListSessions(crow::websocket::connection& /*conn*/)
    {
        // Start with sessions persisted to disk.
        auto ids = AssistantSession::ListSessions(GetSessionsDir());

        // Also include in-memory sessions that have not yet written a file
        // (e.g. newly created sessions with no messages).
        {
            std::lock_guard<std::mutex> lock(m_SessionsMutex);
            for (auto const& [sid, _] : m_Sessions)
            {
                bool found = false;
                for (auto const& id : ids)
                {
                    if (id == sid)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    ids.push_back(sid);
            }
        }

        // Re-sort newest first (session IDs are timestamp-based).
        std::sort(ids.begin(), ids.end(), std::greater<>());

        std::string json = "{\"type\":\"session_list\",\"sessions\":[";
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i > 0)
                json += ",";
            auto* session = GetSession(ids[i]);
            size_t turns = session ? session->GetTurnCount() : 0;
            json += "{\"id\":\"" + JsonEscape(ids[i]) + "\",\"turns\":" + std::to_string(turns) + "}";
        }
        json += "]}";
        QueueMessage(json);
    }

    void AssistantController::HandleResumeSession(crow::websocket::connection& conn, std::string const& sessionId)
    {
        auto* session = GetSession(sessionId);
        if (!session)
        {
            QueueMessage("{\"type\":\"error\",\"message\":\"Session not found: " + JsonEscape(sessionId) + "\"}");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_ClientsMutex);
            if (m_ClientStates.count(&conn))
                m_ClientStates[&conn].activeSessionId = sessionId;
        }

        // Send session history to client.
        auto turns = session->GetAllTurns();
        std::string json = "{\"type\":\"session_history\",\"sessionId\":\"" + JsonEscape(sessionId) + "\",\"turns\":[";
        for (size_t i = 0; i < turns.size(); ++i)
        {
            if (i > 0)
                json += ",";
            json += "{\"role\":\"" + JsonEscape(turns[i].role) + "\",\"text\":\"" + JsonEscape(turns[i].text) +
                    "\",\"ts\":\"" + JsonEscape(turns[i].timestamp) + "\"}";
        }
        json += "]}";
        QueueMessage(json);
        QueueMessage("{\"type\":\"session_active\",\"sessionId\":\"" + JsonEscape(sessionId) + "\"}");
    }

    void AssistantController::HandleNewSession(crow::websocket::connection& conn)
    {
        auto* session = CreateSession();
        {
            std::lock_guard<std::mutex> lock(m_ClientsMutex);
            if (m_ClientStates.count(&conn))
                m_ClientStates[&conn].activeSessionId = session->GetSessionId();
        }
        QueueMessage("{\"type\":\"session_active\",\"sessionId\":\"" + JsonEscape(session->GetSessionId()) + "\"}");
    }

    // -----------------------------------------------------------------
    // get_history — return deduplicated user messages across all sessions
    // -----------------------------------------------------------------

    void AssistantController::HandleGetHistory(crow::websocket::connection& /*conn*/, int maxEntries)
    {
        // Collect user messages from all sessions (newest first).
        auto sessionIds = AssistantSession::ListSessions(GetSessionsDir());

        // Use an ordered vector to preserve recency; dedup with a set.
        std::vector<std::string> history;
        std::unordered_set<std::string> seen;
        history.reserve(static_cast<size_t>(maxEntries));

        for (auto const& sid : sessionIds)
        {
            if (static_cast<int>(history.size()) >= maxEntries)
                break;

            auto* session = GetSession(sid);
            if (!session)
                continue;

            auto turns = session->GetAllTurns();
            // Iterate newest-first within this session.
            for (auto it = turns.rbegin(); it != turns.rend(); ++it)
            {
                if (it->role != "user")
                    continue;
                std::string trimmed = it->text;
                if (trimmed.empty())
                    continue;
                if (seen.count(trimmed))
                    continue;
                seen.insert(trimmed);
                history.push_back(trimmed);
                if (static_cast<int>(history.size()) >= maxEntries)
                    break;
            }
        }

        // Build JSON response.
        std::string json = "{\"type\":\"history\",\"entries\":[";
        for (size_t i = 0; i < history.size(); ++i)
        {
            if (i > 0)
                json += ",";
            json += "\"" + JsonEscape(history[i]) + "\"";
        }
        json += "]}";
        QueueMessage(json);
    }

    // -----------------------------------------------------------------
    // completion_request — return completions for the current input prefix
    // -----------------------------------------------------------------

    void AssistantController::HandleCompletionRequest(crow::websocket::connection& /*conn*/, std::string const& prefix,
                                                      std::string const& kind)
    {
        // Slash commands.
        static std::vector<std::string> const slashCommands = {"/help",   "/status",       "/runs",        "/log",
                                                               "/memory", "/index",        "/sessions",    "/new",
                                                               "/clear",  "/index rescan", "/memory clear"};

        std::vector<std::string> candidates;
        constexpr int kMaxCandidates = 10;

        if (kind.empty() || kind == "slash")
        {
            if (!prefix.empty() && prefix[0] == '/')
            {
                for (auto const& cmd : slashCommands)
                {
                    if (cmd.size() > prefix.size() && cmd.rfind(prefix, 0) == 0)
                    {
                        candidates.push_back(cmd);
                        if (static_cast<int>(candidates.size()) >= kMaxCandidates)
                            break;
                    }
                }
            }
        }

        if ((kind.empty() || kind == "workflow") && static_cast<int>(candidates.size()) < kMaxCandidates)
        {
            // Workflow IDs as completions (useful when user starts typing a workflow name).
            if (m_WorkflowRegistry)
            {
                auto ids = m_WorkflowRegistry->GetWorkflowIds();
                for (auto const& id : ids)
                {
                    if (id.size() > prefix.size() && id.rfind(prefix, 0) == 0)
                    {
                        candidates.push_back(id);
                        if (static_cast<int>(candidates.size()) >= kMaxCandidates)
                            break;
                    }
                }
            }
        }

        if ((kind.empty() || kind == "history") && static_cast<int>(candidates.size()) < kMaxCandidates)
        {
            // Match against recent user messages in the active session.
            std::lock_guard<std::mutex> lock(m_SessionsMutex);
            for (auto const& [sid, session] : m_Sessions)
            {
                auto turns = session->GetAllTurns();
                for (auto it = turns.rbegin(); it != turns.rend(); ++it)
                {
                    if (it->role != "user")
                        continue;
                    if (it->text.size() > prefix.size() && it->text.rfind(prefix, 0) == 0)
                    {
                        // Avoid duplicates.
                        bool dupe = false;
                        for (auto const& c : candidates)
                        {
                            if (c == it->text)
                            {
                                dupe = true;
                                break;
                            }
                        }
                        if (!dupe)
                        {
                            candidates.push_back(it->text);
                            if (static_cast<int>(candidates.size()) >= kMaxCandidates)
                                break;
                        }
                    }
                }
                if (static_cast<int>(candidates.size()) >= kMaxCandidates)
                    break;
            }
        }

        // Build JSON response.
        std::string json = "{\"type\":\"completion_response\",\"prefix\":\"" + JsonEscape(prefix) + "\",\"candidates\":[";
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (i > 0)
                json += ",";
            json += "\"" + JsonEscape(candidates[i]) + "\"";
        }
        json += "]}";
        QueueMessage(json);
    }

    // -----------------------------------------------------------------
    // AI call
    // -----------------------------------------------------------------

    void AssistantController::RunAiCallAsync(std::string const& sessionId, std::string const& userMessage)
    {
        JoinFinishedThreads();

        std::string sessionIdCopy = sessionId;
        std::string messageCopy = userMessage;

        std::lock_guard<std::mutex> lock(m_ThreadsMutex);
        m_BackgroundThreads.emplace_back(
            [this, sid = std::move(sessionIdCopy), msg = std::move(messageCopy)]()
            {
                if (m_ShuttingDown.load())
                    return;

                // Get session and recent turns for context.
                AssistantSession* session = GetSession(sid);
                if (!session)
                {
                    QueueMessage("{\"type\":\"error\",\"sessionId\":\"" + JsonEscape(sid) +
                                 "\",\"message\":\"Session not found\"}");
                    return;
                }

                auto recentTurns = session->GetRecentTurns(4000);

                // Remove the last turn (it's the user message we're about to send).
                // The user message goes in PROB, not in conversation context.
                if (!recentTurns.empty() && recentTurns.back().role == "user" && recentTurns.back().text == msg)
                {
                    recentTurns.pop_back();
                }

                // Build tool descriptions for the system prompt.
                std::string const toolDescriptions = m_ToolRegistry.BuildToolDescriptions();

                // Inject relevant memories into the conversation context.
                std::string memoryContext;
                {
                    auto relevantMemories = m_MemoryStore.GetRelevant(msg, 5);
                    LOG_APP_INFO("[assistant] Memory recall for '{}': {} matches (total store: {})", msg.substr(0, 60),
                                 relevantMemories.size(), m_MemoryStore.Size());
                    if (!relevantMemories.empty())
                    {
                        memoryContext = "\n\n=== Recalled Memories ===\n";
                        for (auto const& mem : relevantMemories)
                        {
                            memoryContext += "- [" + mem.key + "]: " + mem.value + "\n";
                            LOG_APP_INFO("[assistant]   injected memory: [{}] = {}", mem.key, mem.value.substr(0, 80));
                        }
                        memoryContext += "=== End Memories ===\n";
                    }
                }

                // Assemble prompt (with tool descriptions).
                auto prompt = ContextAssembler::Assemble(recentTurns, msg, toolDescriptions);

                // Append recalled memories to the conversation context.
                if (!memoryContext.empty())
                    prompt.cntx += memoryContext;

                // Inject relevant file summaries based on query keywords.
                {
                    auto relevantFiles = m_WorkspaceIndexer.GetRelevantFiles(msg, 3);
                    if (!relevantFiles.empty())
                    {
                        prompt.cntx += "\n\n=== Relevant File Summaries ===\n";
                        for (auto const& file : relevantFiles)
                        {
                            prompt.cntx += "- " + file.relativePath + ": " + file.summary + "\n";
                        }
                        prompt.cntx += "=== End File Summaries ===\n";
                        LOG_APP_INFO("[assistant] Injected {} file summaries into context", relevantFiles.size());
                    }
                }

                // --- Multi-step tool-use loop ---
                // After each AI response, check for <tool_call> blocks.
                // Execute tools (with approval where required), append results, and
                // re-send to AI.  Tools are allowed on every iteration except the
                // last one, which forces a final answer.

                std::string currentProb = prompt.prob;
                std::string accumulatedToolContext;
                bool anyToolsUsed = false;

                // STNG without tool descriptions — used on the final iteration
                // to force a final answer without tool calls.
                std::string const stngNoTools = ContextAssembler::Assemble(recentTurns, msg).stng;

                // Loop detection: track (tool_name, sorted_args) → call count.
                std::map<std::string, int> toolCallSignatureCounts;

                for (int iteration = 0; iteration < MAX_TOOL_ITERATIONS; ++iteration)
                {
                    if (m_ShuttingDown.load())
                        return;

                    int const seq = m_NextRequestSeq.fetch_add(1);

                    // Unique folder name: UTC timestamp (YYYYMMDD_HHMMSS) + atomic seq.
                    auto const nowUtc = std::chrono::system_clock::now();
                    auto const nowTime = std::chrono::system_clock::to_time_t(nowUtc);
                    std::tm utcTm{};
#ifdef _WIN32
                    gmtime_s(&utcTm, &nowTime);
#else
                    gmtime_r(&nowTime, &utcTm);
#endif
                    char timeBuf[32];
                    std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &utcTm);
                    std::string const subfolder = "_assistant/call_" + std::string(timeBuf) + "_" + std::to_string(seq);

                    bool const isFinalIteration = (iteration == MAX_TOOL_ITERATIONS - 1);

                    LOG_APP_INFO(
                        "[assistant] Starting AI call iteration {} (seq={}, STNG={} bytes, PROB={} bytes, final={})",
                        iteration, seq, prompt.stng.size(), currentProb.size(), isFinalIteration);

                    // Build CNTX: original conversation context + accumulated tool results.
                    std::string fullCntx = prompt.cntx;
                    if (!accumulatedToolContext.empty())
                    {
                        fullCntx += "\n\n" + accumulatedToolContext;
                    }

                    // On the final iteration, strip tool descriptions to force a final answer.
                    // On all other iterations (including follow-ups), keep tools available.
                    std::string const& iterStng = isFinalIteration ? stngNoTools : prompt.stng;
                    std::string const iterTask =
                        isFinalIteration
                            ? std::string("You have received tool results. Provide your final answer to the user. "
                                          "Do NOT call any tools. Present the information clearly and directly.")
                            : prompt.task;

                    std::string responseText;
                    std::string error;
                    bool const ok =
                        RunSingleAiCall(subfolder, iterStng, iterTask, fullCntx, currentProb, responseText, error);

                    LOG_APP_INFO("[assistant] AI call iteration {} complete: ok={}, response={} bytes", iteration, ok,
                                 responseText.size());

                    if (m_ShuttingDown.load())
                        return;

                    if (!ok)
                    {
                        std::string errorMsg = "AI call failed: " + error;
                        session->AddAssistantMessage(errorMsg);
                        QueueMessage("{\"type\":\"error\",\"sessionId\":\"" + JsonEscape(sid) + "\",\"message\":\"" +
                                     JsonEscape(errorMsg) + "\"}");
                        return;
                    }

                    // Parse tool calls on every iteration except the final one.
                    std::string cleanText;
                    auto toolCalls =
                        isFinalIteration ? std::vector<ToolCall>{} : ToolRegistry::ParseToolCalls(responseText, cleanText);

                    if (toolCalls.empty())
                    {
                        // No tool calls — this is the final response.
                        std::string finalText = cleanText.empty() ? responseText : cleanText;

                        // Detect "described but didn't call" pattern: the AI said it would execute/run
                        // something but produced no tool call.  Append a clear note so the user isn't misled.
                        if (!anyToolsUsed && iteration == 0)
                        {
                            std::string lower;
                            lower.reserve(finalText.size());
                            for (char ch : finalText)
                                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

                            static std::vector<std::string> const falseActionPhrases = {
                                "proceeding to execute",
                                "proceeding to run",
                                "will execute the command",
                                "will run the command",
                                "executing the command",
                                "running the command",
                            };
                            bool falseAction = false;
                            for (auto const& phrase : falseActionPhrases)
                            {
                                if (lower.find(phrase) != std::string::npos)
                                {
                                    falseAction = true;
                                    break;
                                }
                            }
                            if (falseAction)
                            {
                                finalText += "\n\n\xe2\x9a\xa0 Note: no command was actually executed. "
                                             "The AI described an action but did not call the tool.";
                            }
                        }

                        // Response relevance check (Phase 12).
                        // Skip if tools were used — the response reflects tool output, not the raw user message keywords.
                        if (!anyToolsUsed)
                        {
                            std::string validationWarnings = ValidateResponse(msg, finalText);
                            if (!validationWarnings.empty())
                                finalText += validationWarnings;
                        }

                        session->AddAssistantMessage(finalText);
                        QueueMessage("{\"type\":\"assistant_done\",\"sessionId\":\"" + JsonEscape(sid) + "\",\"text\":\"" +
                                     JsonEscape(finalText) + "\"}");
                        return;
                    }

                    // Execute each tool call.
                    anyToolsUsed = true;
                    LOG_APP_INFO("[assistant] AI response contains {} tool call(s) (iteration {})", toolCalls.size(),
                                 iteration + 1);

                    std::string toolResultsBlock;
                    bool loopDetected = false;

                    for (auto const& call : toolCalls)
                    {
                        // --- Loop detection ---
                        // Build a signature from tool name + sorted args.
                        std::string sig = call.name + "(";
                        {
                            std::map<std::string, std::string> sortedArgs(call.args.begin(), call.args.end());
                            bool first = true;
                            for (auto const& [k, v] : sortedArgs)
                            {
                                if (!first)
                                    sig += ",";
                                sig += k + "=" + v;
                                first = false;
                            }
                        }
                        sig += ")";

                        int& callCount = toolCallSignatureCounts[sig];
                        ++callCount;
                        if (callCount > 3)
                        {
                            LOG_APP_WARN("[assistant] Loop detected: tool {} called {} times with identical args", call.name,
                                         callCount);
                            toolResultsBlock +=
                                "<tool_result name=\"" + call.name +
                                "\" status=\"error\">\n"
                                "Loop detected: this tool has been called " +
                                std::to_string(callCount) +
                                " times with identical arguments. Provide your final answer now.\n</tool_result>\n\n";
                            loopDetected = true;
                            continue;
                        }

                        // Notify frontend about tool execution.
                        QueueMessage("{\"type\":\"tool_status\",\"sessionId\":\"" + JsonEscape(sid) + "\",\"tool\":\"" +
                                     JsonEscape(call.name) + "\",\"status\":\"running\"}");

                        // Check if tool requires approval.
                        bool needsApproval = false;
                        std::string toolDescription;
                        for (auto const& def : m_ToolRegistry.GetToolDefs())
                        {
                            if (def.name == call.name)
                            {
                                needsApproval = def.requiresApproval;
                                toolDescription = def.description;
                                break;
                            }
                        }

                        ToolResult result;
                        if (needsApproval)
                        {
                            // Build human-readable description for the approval prompt.
                            std::string approvalDesc = call.name + "(";
                            {
                                bool first = true;
                                for (auto const& [k, v] : call.args)
                                {
                                    if (!first)
                                        approvalDesc += ", ";
                                    approvalDesc += k + "=\"" + v + "\"";
                                    first = false;
                                }
                            }
                            approvalDesc += ")";

                            bool approved = RequestToolApproval(sid, call, approvalDesc);
                            if (approved)
                            {
                                result = m_ToolRegistry.Execute(call);
                            }
                            else
                            {
                                result = {call.name, false, "Tool execution denied by user."};
                            }
                        }
                        else
                        {
                            result = m_ToolRegistry.Execute(call);
                        }

                        // Notify frontend of result.
                        QueueMessage("{\"type\":\"tool_result\",\"sessionId\":\"" + JsonEscape(sid) + "\",\"tool\":\"" +
                                     JsonEscape(call.name) + "\",\"ok\":" + (result.ok ? "true" : "false") +
                                     ",\"summary\":\"" + JsonEscape(result.output.substr(0, 200)) + "\"}");

                        // Build tool results text for next AI iteration.
                        toolResultsBlock += "<tool_result name=\"" + call.name + "\"";
                        toolResultsBlock += result.ok ? " status=\"ok\"" : " status=\"error\"";
                        toolResultsBlock += ">\n" + result.output + "\n</tool_result>\n\n";

                        LOG_APP_INFO("[assistant] Tool {} → {} ({} bytes)", call.name, result.ok ? "ok" : "error",
                                     result.output.size());
                    }

                    // Accumulate tool results for the next iteration.
                    accumulatedToolContext += toolResultsBlock;

                    // Build PROB for the next iteration.
                    // Include the original user message + tool results.
                    // If this is NOT the second-to-last iteration, allow more tool calls.
                    bool const nextIsFinal = (iteration + 1 >= MAX_TOOL_ITERATIONS - 1);
                    if (loopDetected || nextIsFinal)
                    {
                        currentProb = msg +
                                      "\n\n[SYSTEM: Tool results are below. Provide your final answer to the user "
                                      "using these results. Do NOT call any more tools.]\n\n" +
                                      toolResultsBlock;
                    }
                    else
                    {
                        currentProb = msg +
                                      "\n\n[SYSTEM: Tool results are below. Examine them carefully. If you have "
                                      "enough information, provide your final answer. If you need more data, "
                                      "you may call additional tools.]\n\n" +
                                      toolResultsBlock;
                    }
                }

                // Exhausted max iterations — send whatever we have.
                session->AddAssistantMessage("Reached maximum tool call iterations (" + std::to_string(MAX_TOOL_ITERATIONS) +
                                             "). Please try a simpler query.");
                QueueMessage("{\"type\":\"assistant_done\",\"sessionId\":\"" + JsonEscape(sid) +
                             "\",\"text\":\"Reached maximum tool call iterations. Please try a simpler query.\"}");
            });
    }

    // -----------------------------------------------------------------
    // Response relevance checking (Phase 12)
    // -----------------------------------------------------------------

    namespace
    {
        // Tokenise text into lowercase words (≥3 chars), skip common stop words.
        std::vector<std::string> ExtractKeywords(std::string const& text)
        {
            static std::unordered_set<std::string> const stopWords = {
                "the",   "and",   "for",   "are",   "but",    "not",   "you",   "all",  "can",   "her",   "was",
                "one",   "our",   "out",   "has",   "have",   "had",   "its",   "let",  "may",   "who",   "how",
                "any",   "new",   "get",   "from",  "been",   "call",  "come",  "each", "make",  "like",  "than",
                "them",  "then",  "this",  "that",  "what",   "when",  "will",  "with", "would", "could", "should",
                "there", "their", "about", "which", "these",  "those", "other", "some", "just",  "also",  "into",
                "your",  "does",  "very",  "here",  "much",   "more",  "most",  "only", "over",  "such",  "take",
                "than",  "they",  "well",  "were",  "please", "want",  "need",  "help", "tell"};

            std::vector<std::string> keywords;
            std::string word;
            for (char c : text)
            {
                if (std::isalnum(static_cast<unsigned char>(c)))
                {
                    word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                else
                {
                    if (word.size() >= 3 && stopWords.find(word) == stopWords.end())
                        keywords.push_back(word);
                    word.clear();
                }
            }
            if (word.size() >= 3 && stopWords.find(word) == stopWords.end())
                keywords.push_back(word);
            return keywords;
        }

        // Extract potential file paths from text using a simple heuristic.
        // Looks for tokens containing '/' with common source extensions.
        std::vector<std::string> ExtractFilePaths(std::string const& text)
        {
            static std::unordered_set<std::string> const knownExtensions = {
                ".cpp",  ".h",    ".hpp",  ".c",   ".cc",   ".cxx", ".py",   ".sh",  ".ts",   ".tsx", ".js",
                ".jsx",  ".json", ".yaml", ".yml", ".toml", ".md",  ".txt",  ".cfg", ".conf", ".ini", ".xml",
                ".html", ".css",  ".jcwf", ".lua", ".rs",   ".go",  ".java", ".rb",  ".pl"};

            std::vector<std::string> paths;
            std::istringstream iss(text);
            std::string token;
            while (iss >> token)
            {
                // Strip surrounding punctuation (quotes, backticks, parens, brackets, commas).
                while (!token.empty() && (token.front() == '`' || token.front() == '\'' || token.front() == '"' ||
                                          token.front() == '(' || token.front() == '['))
                    token.erase(token.begin());
                while (!token.empty() &&
                       (token.back() == '`' || token.back() == '\'' || token.back() == '"' || token.back() == ')' ||
                        token.back() == ']' || token.back() == ',' || token.back() == ';' || token.back() == ':'))
                    token.pop_back();

                if (token.find('/') == std::string::npos)
                    continue;

                // Check if it ends with a known extension.
                bool hasKnownExt = false;
                for (auto const& ext : knownExtensions)
                {
                    if (token.size() > ext.size() && token.compare(token.size() - ext.size(), ext.size(), ext) == 0)
                    {
                        hasKnownExt = true;
                        break;
                    }
                }
                if (hasKnownExt)
                    paths.push_back(token);
            }
            return paths;
        }
    } // anonymous namespace

    std::string AssistantController::ValidateResponse(std::string const& userMessage, std::string const& aiResponse) const
    {
        std::string warnings;

        // --- 1. Keyword overlap check ---
        auto userKeywords = ExtractKeywords(userMessage);
        if (userKeywords.size() >= 3) // only check if the user message has enough substance
        {
            std::string responseLower;
            responseLower.reserve(aiResponse.size());
            for (char c : aiResponse)
                responseLower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            int matchCount = 0;
            std::unordered_set<std::string> seen;
            for (auto const& kw : userKeywords)
            {
                if (seen.count(kw))
                    continue;
                seen.insert(kw);
                if (responseLower.find(kw) != std::string::npos)
                    matchCount++;
            }

            double overlap = static_cast<double>(matchCount) / static_cast<double>(seen.size());
            if (overlap < 0.30)
            {
                warnings += "\n\n\xe2\x9a\xa0 This response may not be relevant to your question. Consider rephrasing.";
            }
        }

        // --- 2. File path existence check ---
        auto mentionedPaths = ExtractFilePaths(aiResponse);
        if (!mentionedPaths.empty())
        {
            fs::path basePath;
            if (Core::g_Core)
                basePath = Core::g_Core->GetLaunchCWDAbsolute();

            std::vector<std::string> missing;
            std::unordered_set<std::string> checked;
            for (auto const& p : mentionedPaths)
            {
                if (checked.count(p))
                    continue;
                checked.insert(p);

                // Resolve relative paths against the workspace root.
                fs::path resolved = p;
                if (resolved.is_relative() && !basePath.empty())
                    resolved = basePath / resolved;

                std::error_code ec;
                if (!fs::exists(resolved, ec))
                    missing.push_back(p);
            }

            for (auto const& m : missing)
            {
                warnings += "\nNote: " + m + " was not found in the workspace.";
            }
        }

        return warnings;
    }

    bool AssistantController::RunSingleAiCall(std::string const& subfolderName, std::string const& stngContent,
                                              std::string const& taskContent, std::string const& cntxContent,
                                              std::string const& probContent, std::string& outResponseText,
                                              std::string& outError)
    {
        outResponseText.clear();
        outError.clear();

        fs::path const queueBase = GetQueueBasePath();
        if (queueBase.empty())
        {
            outError = "Queue base path is not configured";
            return false;
        }

        JarvisAgent* app = App::g_App;
        if (!app)
        {
            outError = "Application not available";
            return false;
        }

        AiRequestPool* requestPool = app->GetAiRequestPool();
        if (!requestPool)
        {
            outError = "AiRequestPool not available";
            return false;
        }

        // Create queue subfolder.
        fs::path const queueDir = queueBase / subfolderName;
        std::error_code ec;
        fs::create_directories(queueDir, ec);
        if (ec)
        {
            outError = "Failed to create queue directory: " + ec.message();
            return false;
        }

        // Clean previous files.
        for (auto const& entry : fs::directory_iterator(queueDir, ec))
            fs::remove(entry.path(), ec);

        // Register with AiRequestPool.
        int64_t const requestId = requestPool->AllocateRequestId();
        int64_t const timestampNs = NowTimestampNs();

        AiRequestHandle handle{};
        handle.requestId = requestId;
        handle.requestTimestampNs = timestampNs;

        std::string const probFilename = "PROB_" + std::to_string(requestId) + "_" + std::to_string(timestampNs) + ".txt";
        fs::path const probPath = queueDir / probFilename;

        AiRequestHandle const registered = requestPool->RegisterPending(handle, AI_CALL_TIMEOUT_MS);
        if (!registered.IsValid())
        {
            outError = "Failed to register AI request";
            return false;
        }

        // Write queue files.
        std::string writeError;

        if (!stngContent.empty())
        {
            if (!WriteFile(queueDir / "STNG_settings.txt", stngContent, writeError))
            {
                requestPool->Forget(handle);
                outError = "Failed to write STNG file: " + writeError;
                return false;
            }
        }
        requestPool->KickFileActivityWatchdog(handle);

        // TASK file must always be written — the SessionManager
        // requires STNG + TASK + CNTX + PROB for a complete environment.
        {
            std::string const task = taskContent.empty() ? "Process the request." : taskContent;
            if (!WriteFile(queueDir / "TASK_instructions.txt", task, writeError))
            {
                requestPool->Forget(handle);
                outError = "Failed to write TASK file: " + writeError;
                return false;
            }
        }
        requestPool->KickFileActivityWatchdog(handle);

        // CNTX file must always be written (even if empty) — the SessionManager
        // requires STNG + TASK + CNTX + PROB for a complete environment.
        {
            std::string const cntx = cntxContent.empty() ? "No prior conversation." : cntxContent;
            if (!WriteFile(queueDir / "CNTX_context.txt", cntx, writeError))
            {
                requestPool->Forget(handle);
                outError = "Failed to write CNTX file: " + writeError;
                return false;
            }
        }
        requestPool->KickFileActivityWatchdog(handle);

        if (!WriteFile(probPath, probContent, writeError))
        {
            requestPool->Forget(handle);
            outError = "Failed to write PROB file: " + writeError;
            return false;
        }
        requestPool->KickFileActivityWatchdog(handle);

        LOG_APP_INFO("[assistant] AI call dispatched: subfolder='{}' requestId={}", subfolderName, handle.requestId);

        // Wait for completion (blocking).
        std::string errorMessage;
        bool const success = requestPool->WaitForCompletion(handle, AI_CALL_TIMEOUT_MS, outResponseText, errorMessage);

        requestPool->Forget(handle);

        if (!success)
        {
            outError = errorMessage.empty() ? "AI call timed out or failed" : errorMessage;
            return false;
        }

        if (outResponseText.empty())
        {
            outError = "AI returned empty response";
            return false;
        }

        return true;
    }

    // -----------------------------------------------------------------
    // Slash commands
    // -----------------------------------------------------------------

    std::string AssistantController::HandleHelpCommand()
    {
        return "Available commands:\n"
               "  /help          — Show this help message\n"
               "  /status        — Show JarvisAgent system status\n"
               "  /runs          — List recent workflow runs\n"
               "  /log [N]       — Show last N lines of the log (default 20)\n"
               "  /memory        — List saved memories\n"
               "  /memory clear  — Clear all memories\n"
               "  /index         — Show file index status\n"
               "  /index rescan  — Re-scan workspace files\n"
               "  /sessions      — List previous sessions\n"
               "  /new           — Start a new session\n"
               "  /clear         — Clear terminal display\n"
               "\n"
               "Or just type a question and press Enter to chat with the AI assistant.";
    }

    std::string AssistantController::HandleStatusCommand()
    {
        JarvisAgent* app = App::g_App;
        if (!app)
            return "Error: JarvisAgent not available.";

        std::ostringstream oss;
        oss << "JarvisAgent Status\n";
        oss << std::string(40, '-') << "\n";
        oss << "Version:            " << JARVIS_AGENT_VERSION << "\n";

        // Session managers
        size_t smCount = app->GetSessionManagerCount();
        size_t smInflight = app->GetSessionManagerInflightTotal();
        oss << "Session managers:   " << smCount << "\n";
        oss << "AI queries inflight:" << smInflight << "\n";

        // Workflows
        if (m_WorkflowRegistry)
        {
            auto ids = m_WorkflowRegistry->GetWorkflowIds();
            oss << "Registered JCWFs:   " << ids.size() << "\n";
        }

        // Runtime
        if (m_WorkflowRuntimeManager)
        {
            auto activeRuns = m_WorkflowRuntimeManager->GetActiveRunsSnapshot();
            size_t running = 0;
            size_t paused = 0;
            for (auto const& run : activeRuns)
            {
                if (run.m_State == WorkflowRunState::Running)
                    ++running;
                if (run.m_State == WorkflowRunState::Paused)
                    ++paused;
            }
            oss << "Active runs:        " << activeRuns.size();
            if (running > 0 || paused > 0)
            {
                oss << " (";
                if (running > 0)
                    oss << running << " running";
                if (running > 0 && paused > 0)
                    oss << ", ";
                if (paused > 0)
                    oss << paused << " paused";
                oss << ")";
            }
            oss << "\n";
        }

        // Python
        PythonEngine* python = app->GetPythonEngine();
        oss << "Python engine:      " << (python ? "ready" : "not available") << "\n";

        // Uptime
        auto now = std::chrono::system_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - app->GetStartupTime());
        auto hours = std::chrono::duration_cast<std::chrono::hours>(uptime);
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(uptime - hours);
        oss << "Uptime:             " << hours.count() << "h " << minutes.count() << "m\n";

        return oss.str();
    }

    std::string AssistantController::HandleRunsCommand()
    {
        if (!m_WorkflowRuntimeManager)
            return "Workflow runtime manager not available.";

        auto activeRuns = m_WorkflowRuntimeManager->GetActiveRunsSnapshot();
        if (activeRuns.empty())
            return "No active workflow runs.";

        std::ostringstream oss;
        oss << "Active workflow runs:\n";
        for (auto const& run : activeRuns)
        {
            std::string stateStr;
            switch (run.m_State)
            {
                case WorkflowRunState::Pending:
                    stateStr = "pending";
                    break;
                case WorkflowRunState::Running:
                    stateStr = "running";
                    break;
                case WorkflowRunState::Paused:
                    stateStr = "paused";
                    break;
                case WorkflowRunState::Stopping:
                    stateStr = "stopping";
                    break;
                case WorkflowRunState::Succeeded:
                    stateStr = "succeeded";
                    break;
                case WorkflowRunState::Failed:
                    stateStr = "failed";
                    break;
                case WorkflowRunState::Cancelled:
                    stateStr = "cancelled";
                    break;
                case WorkflowRunState::Stopped:
                    stateStr = "stopped";
                    break;
                default:
                    stateStr = "unknown";
                    break;
            }
            oss << "  " << run.m_RunId << "  " << run.m_WorkflowId << "  [" << stateStr << "]\n";
        }
        return oss.str();
    }

    std::string AssistantController::HandleLogCommand(std::string const& args)
    {
        int lines = 20;
        if (!args.empty())
        {
            try
            {
                lines = std::stoi(args);
                if (lines < 1)
                    lines = 1;
                if (lines > 500)
                    lines = 500;
            }
            catch (...)
            {
                return "Usage: /log [N]  (N = number of lines, default 20, max 500)";
            }
        }

        namespace fs = std::filesystem;
        fs::path logPath = "log/log.txt";
        std::error_code ec;
        if (!fs::exists(logPath, ec))
            return "Log file not found: " + logPath.string();

        std::string cmd = "tail -" + std::to_string(lines) + " '" + logPath.string() + "' 2>/dev/null";
        std::string const shellCmd = WrapForBash(cmd);
        std::array<char, 4096> buffer;
        std::string result;

        FILE* pipe = popen(shellCmd.c_str(), "r");
        if (!pipe)
            return "Failed to read log file.";

        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            result += buffer.data();
            if (result.size() > 32768)
                break;
        }
        pclose(pipe);

        if (result.empty())
            return "Log file is empty.";

        return "Last " + std::to_string(lines) + " lines of log/log.txt:\n\n" + result;
    }

    std::string AssistantController::HandleMemoryCommand(std::string const& args)
    {
        if (args == "clear")
        {
            m_MemoryStore.ClearAll();
            return "All memories cleared.";
        }

        auto entries = m_MemoryStore.ListAll();
        if (entries.empty())
            return "No memories stored. The AI can save memories via save_memory tool.";

        std::ostringstream oss;
        oss << entries.size() << " stored memor" << (entries.size() == 1 ? "y" : "ies") << ":\n\n";
        for (auto const& entry : entries)
        {
            oss << "  [" << entry.key << "] " << entry.value;
            if (!entry.tags.empty())
            {
                oss << "  (tags:";
                for (auto const& tag : entry.tags)
                    oss << " " << tag;
                oss << ")";
            }
            oss << "  — " << entry.createdAt << "\n";
        }
        oss << "\nUse /memory clear to delete all memories.";
        return oss.str();
    }

    std::string AssistantController::HandleIndexCommand(std::string const& args)
    {
        if (args == "rescan")
        {
            m_WorkspaceIndexer.ScanWorkspace();
            return "Workspace re-scan complete: " + std::to_string(m_WorkspaceIndexer.FileCount()) + " files indexed, " +
                   std::to_string(m_WorkspaceIndexer.SummaryCount()) + " cached summaries.";
        }

        size_t fileCount = m_WorkspaceIndexer.FileCount();
        size_t summaryCount = m_WorkspaceIndexer.SummaryCount();

        std::ostringstream oss;
        oss << "Workspace Index Status\n";
        oss << std::string(40, '-') << "\n";
        oss << "Indexed files:      " << fileCount << "\n";
        oss << "Cached summaries:   " << summaryCount << "\n";
        oss << "Coverage:           ";
        if (fileCount > 0)
            oss << (summaryCount * 100 / fileCount) << "%";
        else
            oss << "N/A";
        oss << "\n\n";

        // Show extension breakdown.
        auto entries = m_WorkspaceIndexer.GetAllEntries();
        std::map<std::string, int> extCounts;
        for (auto const& e : entries)
            extCounts[e.extension]++;

        oss << "By extension:\n";
        for (auto const& [ext, count] : extCounts)
            oss << "  " << ext << "  " << count << " files\n";

        oss << "\nUse /index rescan to re-scan workspace files.\n";
        oss << "The AI can use get_file_summary to generate summaries on demand.";
        return oss.str();
    }

    bool AssistantController::MakeToolAiCall(std::string const& systemPrompt, std::string const& userPrompt,
                                             std::string& outResponse, std::string& outError)
    {
        if (m_ShuttingDown.load())
        {
            outError = "Shutting down";
            return false;
        }

        int const seq = m_NextRequestSeq.fetch_add(1);

        // Build a unique subfolder name for this tool AI call.
        auto const nowUtc = std::chrono::system_clock::now();
        auto const nowTime = std::chrono::system_clock::to_time_t(nowUtc);
        std::tm utcTm{};
#ifdef _WIN32
        gmtime_s(&utcTm, &nowTime);
#else
        gmtime_r(&nowTime, &utcTm);
#endif
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &utcTm);
        std::string const subfolder = "_assistant/tool_" + std::string(timeBuf) + "_" + std::to_string(seq);

        LOG_APP_INFO("[assistant] MakeToolAiCall: subfolder='{}' sysPrompt={} bytes, userPrompt={} bytes", subfolder,
                     systemPrompt.size(), userPrompt.size());

        return RunSingleAiCall(subfolder, systemPrompt, /*taskContent=*/"", /*cntxContent=*/"", userPrompt, outResponse,
                               outError);
    }

    // -----------------------------------------------------------------
    // Tool approval flow
    // -----------------------------------------------------------------

    bool AssistantController::RequestToolApproval(std::string const& sessionId, ToolCall const& call,
                                                  std::string const& description)
    {
        int const seq = m_NextApprovalSeq.fetch_add(1);
        std::string const requestId = "apr_" + std::to_string(seq);

        auto approval = std::make_shared<PendingApproval>();
        approval->requestId = requestId;

        {
            std::lock_guard<std::mutex> lock(m_ApprovalsMutex);
            m_PendingApprovals[requestId] = approval;
        }

        // Build args JSON for the approval request message.
        std::string argsJson = "{";
        {
            bool first = true;
            for (auto const& [k, v] : call.args)
            {
                if (!first)
                    argsJson += ",";
                argsJson += "\"" + JsonEscape(k) + "\":\"" + JsonEscape(v) + "\"";
                first = false;
            }
        }
        argsJson += "}";

        // Send approval_request to frontend.
        QueueMessage("{\"type\":\"approval_request\",\"sessionId\":\"" + JsonEscape(sessionId) + "\",\"requestId\":\"" +
                     JsonEscape(requestId) + "\",\"tool\":\"" + JsonEscape(call.name) + "\",\"args\":" + argsJson +
                     ",\"description\":\"" + JsonEscape(description) + "\"}");

        LOG_APP_INFO("[assistant] Approval requested: {} — {}", requestId, description);

        // Block until the user responds or timeout.
        bool approved = false;
        {
            std::unique_lock<std::mutex> lock(approval->mutex);
            bool notTimedOut = approval->cv.wait_for(lock, std::chrono::seconds(APPROVAL_TIMEOUT_S),
                                                     [&] { return approval->responded || m_ShuttingDown.load(); });
            if (notTimedOut && approval->responded)
            {
                approved = approval->approved;
            }
            else
            {
                LOG_APP_WARN("[assistant] Approval timed out or shutdown: {}", requestId);
                QueueMessage("{\"type\":\"tool_result\",\"sessionId\":\"" + JsonEscape(sessionId) + "\",\"tool\":\"" +
                             JsonEscape(call.name) + "\",\"ok\":false,\"summary\":\"Approval timed out after " +
                             std::to_string(APPROVAL_TIMEOUT_S) + " seconds.\"}");
            }
        }

        // Remove from pending map.
        {
            std::lock_guard<std::mutex> lock(m_ApprovalsMutex);
            m_PendingApprovals.erase(requestId);
        }

        LOG_APP_INFO("[assistant] Approval result: {} — {}", requestId, approved ? "approved" : "denied");
        return approved;
    }

    void AssistantController::HandleApprovalResponse(std::string const& requestId, bool approved)
    {
        std::shared_ptr<PendingApproval> approval;
        {
            std::lock_guard<std::mutex> lock(m_ApprovalsMutex);
            auto it = m_PendingApprovals.find(requestId);
            if (it == m_PendingApprovals.end())
            {
                LOG_APP_WARN("[assistant] Approval response for unknown request: {}", requestId);
                return;
            }
            approval = it->second;
        }

        {
            std::lock_guard<std::mutex> lock(approval->mutex);
            approval->responded = true;
            approval->approved = approved;
        }
        approval->cv.notify_one();
        LOG_APP_INFO("[assistant] Approval response received: {} — {}", requestId, approved ? "approved" : "denied");
    }

    // -----------------------------------------------------------------
    // Message queuing and draining
    // -----------------------------------------------------------------

    void AssistantController::QueueMessage(std::string const& jsonMessage)
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        m_PendingMessages.push_back(jsonMessage);
    }

    void AssistantController::DrainPendingMessages()
    {
        std::vector<std::string> pending;
        std::unordered_set<crow::websocket::connection*> clients;

        {
            std::lock_guard<std::mutex> lock(m_PendingMutex);
            if (m_PendingMessages.empty())
                return;
            pending.swap(m_PendingMessages);
        }

        {
            std::lock_guard<std::mutex> lock(m_ClientsMutex);
            clients = m_Clients;
        }

        // Build a single batch envelope (same pattern as WebServer).
        std::string batch = R"({"type":"batch","messages":[)";
        for (size_t i = 0; i < pending.size(); ++i)
        {
            if (i > 0)
                batch += ',';
            batch += pending[i];
        }
        batch += "]}";

        for (auto* client : clients)
        {
            try
            {
                client->send_text(batch);
            }
            catch (std::exception const& e)
            {
                LOG_APP_WARN("[assistant] Failed to send to client: {}", e.what());
            }
        }
    }

    // -----------------------------------------------------------------
    // Session management
    // -----------------------------------------------------------------

    AssistantSession* AssistantController::GetSession(std::string const& sessionId)
    {
        std::lock_guard<std::mutex> lock(m_SessionsMutex);

        auto it = m_Sessions.find(sessionId);
        if (it != m_Sessions.end())
            return it->second.get();

        // Try to load from disk.
        fs::path const sessionsDir = GetSessionsDir();
        fs::path const filePath = sessionsDir / (sessionId + ".jsonl");
        if (fs::exists(filePath))
        {
            auto session = std::make_unique<AssistantSession>(sessionsDir, sessionId);
            auto* ptr = session.get();
            m_Sessions[sessionId] = std::move(session);
            return ptr;
        }

        return nullptr;
    }

    AssistantSession* AssistantController::CreateSession()
    {
        std::lock_guard<std::mutex> lock(m_SessionsMutex);

        auto session = std::make_unique<AssistantSession>(GetSessionsDir());
        auto* ptr = session.get();
        m_Sessions[session->GetSessionId()] = std::move(session);
        return ptr;
    }

    std::filesystem::path AssistantController::GetSessionsDir() const { return fs::absolute("assistant/sessions"); }

    bool AssistantController::WriteFile(fs::path const& path, std::string const& content, std::string& outError)
    {
        std::ofstream ofs(path, std::ios::out | std::ios::binary);
        if (!ofs)
        {
            outError = "Failed to open " + path.string();
            return false;
        }
        ofs << content;
        if (!ofs.good())
        {
            outError = "Write failed for " + path.string();
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------

    void AssistantController::Shutdown()
    {
        // Idempotent: only the first call does real work.
        if (m_ShuttingDown.exchange(true))
            return;

        // Wake all pending approvals so background threads can finish.
        {
            std::lock_guard<std::mutex> lock(m_ApprovalsMutex);
            for (auto& [id, approval] : m_PendingApprovals)
            {
                approval->cv.notify_all();
            }
        }

        // Force-close assistant WS connections.
        {
            std::lock_guard<std::mutex> lock(m_ClientsMutex);
            for (auto* client : m_Clients)
            {
                try
                {
                    client->close("server shutting down");
                }
                catch (...)
                {
                }
            }
        }

        // Join background threads.
        {
            std::lock_guard<std::mutex> lock(m_ThreadsMutex);
            for (auto& t : m_BackgroundThreads)
            {
                if (t.joinable())
                    t.join();
            }
            m_BackgroundThreads.clear();
        }

        LOG_APP_INFO("[assistant] AssistantController shutdown complete");
    }

    void AssistantController::JoinFinishedThreads()
    {
        std::lock_guard<std::mutex> lock(m_ThreadsMutex);
        // Simple: try to join threads that are done. Move non-joinable ones to a new vector.
        // This is best-effort cleanup — Shutdown() does the authoritative join.
        std::vector<std::thread> remaining;
        for (auto& t : m_BackgroundThreads)
        {
            // We can't easily check if a thread is "done" in C++, so we just
            // keep all threads and let Shutdown() join them. This avoids blocking.
            remaining.push_back(std::move(t));
        }
        m_BackgroundThreads = std::move(remaining);
    }
} // namespace AIAssistant
