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
#include <sstream>

// simdjson for incoming message parsing
#include "simdjson/simdjson.h"

namespace
{
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
    AssistantController::AssistantController() : m_MemoryStore(fs::absolute("assistant/memory.json"))
    {
        m_ToolRegistry.SetMemoryStore(&m_MemoryStore);
        LOG_APP_INFO("[assistant] AssistantController created ({} memories loaded)", m_MemoryStore.Size());
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
        auto ids = AssistantSession::ListSessions(GetSessionsDir());

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

                // --- Tool-use loop ---
                // After each AI response, check for <tool_call> blocks.
                // Execute tools, append results, and re-send to AI.
                // Max iterations prevent infinite loops.

                std::string currentProb = prompt.prob;
                std::string accumulatedToolContext;

                // STNG without tool descriptions — used for follow-up iterations
                // so the AI cannot call tools again and must give a final answer.
                std::string const stngNoTools = ContextAssembler::Assemble(recentTurns, msg).stng;

                for (int iteration = 0; iteration < MAX_TOOL_ITERATIONS; ++iteration)
                {
                    if (m_ShuttingDown.load())
                        return;

                    int const seq = m_NextRequestSeq.fetch_add(1);

                    // Unique folder name: UTC timestamp (YYYYMMDD_HHMMSS) + atomic seq.
                    // Avoids collisions across restarts.
                    auto const nowUtc = std::chrono::system_clock::now();
                    auto const nowTime = std::chrono::system_clock::to_time_t(nowUtc);
                    std::tm utcTm{};
                    gmtime_r(&nowTime, &utcTm);
                    char timeBuf[32];
                    std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &utcTm);
                    std::string const subfolder = "_assistant/call_" + std::string(timeBuf) + "_" + std::to_string(seq);

                    LOG_APP_INFO("[assistant] Starting AI call iteration {} (seq={}, STNG={} bytes, PROB={} bytes)",
                                 iteration, seq, prompt.stng.size(), currentProb.size());

                    // Build CNTX: original conversation context + accumulated tool results.
                    std::string fullCntx = prompt.cntx;
                    if (!accumulatedToolContext.empty())
                    {
                        fullCntx += "\n\n" + accumulatedToolContext;
                    }

                    // On follow-up iterations (after tools ran), strip tool descriptions
                    // from STNG so the AI cannot call tools again. This forces a final answer.
                    std::string const iterTask =
                        (iteration == 0)
                            ? prompt.task
                            : std::string(
                                  "You have received tool results. Provide your final answer to the user. "
                                  "Do NOT call any tools. Present the raw data as-is — do not summarize or paraphrase it.");

                    std::string responseText;
                    std::string error;
                    bool const ok = RunSingleAiCall(subfolder, (iteration == 0) ? prompt.stng : stngNoTools, iterTask,
                                                    fullCntx, currentProb, responseText, error);

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

                    // Only parse tool calls on iteration 0.  On follow-up iterations
                    // the response is always treated as the final answer — even if the
                    // AI still emits <tool_call> blocks (it can learn the syntax from
                    // the <tool_result> tags in the PROB).
                    std::string cleanText;
                    auto toolCalls =
                        (iteration == 0) ? ToolRegistry::ParseToolCalls(responseText, cleanText) : std::vector<ToolCall>{};

                    if (toolCalls.empty())
                    {
                        // No tool calls (or follow-up iteration) — this is the final response.
                        // Strip any stray <tool_call> tags the AI may have echoed.
                        std::string const& finalText = cleanText.empty() ? responseText : cleanText;
                        session->AddAssistantMessage(finalText);
                        QueueMessage("{\"type\":\"assistant_done\",\"sessionId\":\"" + JsonEscape(sid) + "\",\"text\":\"" +
                                     JsonEscape(finalText) + "\"}");
                        return;
                    }

                    // Execute each tool call.
                    LOG_APP_INFO("[assistant] AI response contains {} tool call(s) (iteration {})", toolCalls.size(),
                                 iteration + 1);

                    std::string toolResultsBlock;
                    for (auto const& call : toolCalls)
                    {
                        // Notify frontend about tool execution.
                        QueueMessage("{\"type\":\"tool_status\",\"sessionId\":\"" + JsonEscape(sid) + "\",\"tool\":\"" +
                                     JsonEscape(call.name) + "\",\"status\":\"running\"}");

                        // Check if tool requires approval.
                        bool needsApproval = false;
                        for (auto const& def : m_ToolRegistry.GetToolDefs())
                        {
                            if (def.name == call.name)
                            {
                                needsApproval = def.requiresApproval;
                                break;
                            }
                        }

                        ToolResult result;
                        if (needsApproval)
                        {
                            // For now, skip tools that require approval with a message.
                            // Full approval flow will come later.
                            result = {call.name, false, "Tool requires user approval (not yet implemented). Skipped."};
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

                    // Re-send to AI with the original user message + tool results.
                    // The instruction is explicit: answer now, do not call more tools.
                    currentProb = msg +
                                  "\n\n[SYSTEM: Tool results are below. You MUST now answer the user's "
                                  "question using these results. Do NOT emit any more <tool_call> blocks. "
                                  "Provide your final answer directly.]\n\n" +
                                  toolResultsBlock;

                    // If there was clean text before tool calls, record it.
                    if (!cleanText.empty())
                    {
                        // Trim whitespace.
                        while (!cleanText.empty() && (cleanText.back() == '\n' || cleanText.back() == ' '))
                            cleanText.pop_back();
                    }
                }

                // Exhausted max iterations — send whatever we have.
                session->AddAssistantMessage("Reached maximum tool call iterations. Please try a simpler query.");
                QueueMessage("{\"type\":\"assistant_done\",\"sessionId\":\"" + JsonEscape(sid) +
                             "\",\"text\":\"Reached maximum tool call iterations. Please try a simpler query.\"}");
            });
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

        if (!taskContent.empty())
        {
            if (!WriteFile(queueDir / "TASK_instructions.txt", taskContent, writeError))
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
        std::array<char, 4096> buffer;
        std::string result;

        FILE* pipe = popen(cmd.c_str(), "r");
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
