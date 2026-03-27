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

#pragma once

#include "assistant/assistantSession.h"
#include "assistant/assistantMemory.h"
#include "assistant/assistantTools.h"
#include "assistant/workspaceIndexer.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace crow
{
    namespace websocket
    {
        struct connection;
    }
} // namespace crow

namespace AIAssistant
{
    class WorkflowRegistry;
    class WorkflowRuntimeManager;

    // Manages the /ws/assistant WebSocket endpoint.
    //
    // Handles: user messages, slash commands, session management.
    // AI calls run on background threads; responses are queued and drained
    // via DrainPendingMessages() (called from the WS onmessage handler).
    class AssistantController
    {
    public:
        AssistantController();
        ~AssistantController();

        AssistantController(AssistantController const&) = delete;
        AssistantController& operator=(AssistantController const&) = delete;

        // Called by WebServer during WS route setup.
        void OnOpen(crow::websocket::connection& conn);
        void OnClose(crow::websocket::connection& conn);
        void OnMessage(crow::websocket::connection& conn, std::string const& data);

        // Drain queued response messages to connected assistant clients.
        // Called at the end of OnMessage (same pattern as WebServer::DrainPendingBroadcasts).
        void DrainPendingMessages();

        // Shutdown: signal background threads to stop and join them.
        void Shutdown();

        // Set optional pointers for slash command support.
        void SetWorkflowRegistry(WorkflowRegistry* registry);
        void SetWorkflowRuntimeManager(WorkflowRuntimeManager* runtimeManager);

    private:
        // Per-connection state.
        struct ClientState
        {
            std::string activeSessionId;
        };

        void HandleUserMessage(crow::websocket::connection& conn, std::string const& sessionId, std::string const& text);
        void HandleCommand(crow::websocket::connection& conn, std::string const& sessionId, std::string const& command,
                           std::string const& args);
        void HandleListSessions(crow::websocket::connection& conn);
        void HandleResumeSession(crow::websocket::connection& conn, std::string const& sessionId);
        void HandleNewSession(crow::websocket::connection& conn);
        void HandleGetHistory(crow::websocket::connection& conn, int maxEntries);
        void HandleCompletionRequest(crow::websocket::connection& conn, std::string const& prefix, std::string const& kind);

        // Run AI call on background thread, queue response.
        void RunAiCallAsync(std::string const& sessionId, std::string const& userMessage);

        // Make a blocking AI call using the queue-file pipeline (same as AiJcwfService).
        bool RunSingleAiCall(std::string const& subfolderName, std::string const& stngContent,
                             std::string const& taskContent, std::string const& cntxContent, std::string const& probContent,
                             std::string& outResponseText, std::string& outError);

        // Slash command handlers.
        std::string HandleHelpCommand();
        std::string HandleStatusCommand();
        std::string HandleRunsCommand();
        std::string HandleLogCommand(std::string const& args);
        std::string HandleMemoryCommand(std::string const& args);
        std::string HandleIndexCommand(std::string const& args);

        // Blocking AI call helper — used as callback for tool-initiated AI calls
        // (e.g. get_file_summary summarization).  Runs via the queue-file pipeline.
        bool MakeToolAiCall(std::string const& systemPrompt, std::string const& userPrompt, std::string& outResponse,
                            std::string& outError);

        // Tool approval flow — blocks the calling thread until the user responds or timeout.
        // Returns true if approved, false if denied or timed out.
        bool RequestToolApproval(std::string const& sessionId, ToolCall const& call, std::string const& description);

        // Called from OnMessage when the frontend sends an approval_response.
        void HandleApprovalResponse(std::string const& requestId, bool approved);

        // Queue a JSON message for delivery to all assistant clients.
        void QueueMessage(std::string const& jsonMessage);

        // Get or create a session.
        AssistantSession* GetSession(std::string const& sessionId);
        AssistantSession* CreateSession();

        void JoinFinishedThreads();

        std::filesystem::path GetSessionsDir() const;
        static bool WriteFile(std::filesystem::path const& path, std::string const& content, std::string& outError);

        // Response relevance checking (Phase 12).
        // Returns empty string if OK, or a warning to append.
        std::string ValidateResponse(std::string const& userMessage, std::string const& aiResponse) const;

        // Connected assistant clients.
        std::mutex m_ClientsMutex;
        std::unordered_set<crow::websocket::connection*> m_Clients;
        std::unordered_map<crow::websocket::connection*, ClientState> m_ClientStates;

        // Active sessions (loaded on demand).
        std::mutex m_SessionsMutex;
        std::unordered_map<std::string, std::unique_ptr<AssistantSession>> m_Sessions;

        // Pending messages to send to clients.
        std::mutex m_PendingMutex;
        std::vector<std::string> m_PendingMessages;

        // Background AI call threads.
        std::mutex m_ThreadsMutex;
        std::vector<std::thread> m_BackgroundThreads;
        std::atomic<bool> m_ShuttingDown{false};
        std::atomic<int> m_NextRequestSeq{1};

        // Tool system.
        ToolRegistry m_ToolRegistry;

        // Persistent memory.
        MemoryStore m_MemoryStore;

        // Workspace file index.
        WorkspaceIndexer m_WorkspaceIndexer;

        // Optional subsystem pointers (for slash commands).
        WorkflowRegistry* m_WorkflowRegistry = nullptr;
        WorkflowRuntimeManager* m_WorkflowRuntimeManager = nullptr;

        // Pending tool approvals (keyed by requestId).
        struct PendingApproval
        {
            std::string requestId;
            std::mutex mutex;
            std::condition_variable cv;
            bool responded{false};
            bool approved{false};
        };
        std::mutex m_ApprovalsMutex;
        std::unordered_map<std::string, std::shared_ptr<PendingApproval>> m_PendingApprovals;
        std::atomic<int> m_NextApprovalSeq{1};

        static constexpr int AI_CALL_TIMEOUT_MS = 120000; // 2 minutes
        static constexpr int MAX_TOOL_ITERATIONS = 10;    // max tool-call re-sends per turn
        static constexpr int APPROVAL_TIMEOUT_S = 60;     // approval wait timeout
    };
} // namespace AIAssistant
