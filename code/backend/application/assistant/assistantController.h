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
#include <future>
#include <memory>
#include <mutex>
#include <string>
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
        // originConn is recorded with any approval requests so only the
        // originating client can approve them.
        void RunAiCallAsync(std::string const& sessionId, std::string const& userMessage,
                            crow::websocket::connection* originConn);

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

        // Blocks the calling thread until the user responds or timeout.
        // Only the connection in originConn can approve.  Returns true if
        // approved, false if denied, timed out, or the originating client
        // disconnected.  Pointer identity only — never dereferenced.
        bool RequestToolApproval(std::string const& sessionId, ToolCall const& call, std::string const& description,
                                 crow::websocket::connection* originConn);

        // Called from OnMessage when the frontend sends an approval_response.
        // conn must match the PendingApproval's originConn or the response is
        // rejected.
        void HandleApprovalResponse(crow::websocket::connection& conn, std::string const& requestId, bool approved);

        // Queue a JSON message for delivery to all assistant clients.
        void QueueMessage(std::string const& jsonMessage);

        // Get or create a session.  shared_ptr so background AI threads can
        // hold a session safely across blocking calls without racing with
        // shutdown / session eviction.
        std::shared_ptr<AssistantSession> GetSession(std::string const& sessionId);
        std::shared_ptr<AssistantSession> CreateSession();

        // Drop already-finished AI lambda futures from m_BackgroundFutures.
        // Bounds the vector so a long-running session doesn't accrete entries
        // for every completed turn.  Authoritative join still happens in Shutdown().
        void JoinFinishedFutures();

        // Drain loop body: pulled out of the controller-owned thread so the
        // engine ThreadPool can host it.  Returns when m_ShuttingDown is set.
        void DrainLoop();

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
        std::unordered_map<std::string, std::shared_ptr<AssistantSession>> m_Sessions;

        // Pending messages to send to clients.  m_DrainCv lets QueueMessage
        // wake the drain loop immediately rather than waiting on the next
        // OnMessage to call DrainPendingMessages — without this, an AI reply
        // that lands after the user's last message stays queued indefinitely.
        std::mutex m_PendingMutex;
        std::condition_variable m_DrainCv;
        std::vector<std::string> m_PendingMessages;

        // Background AI lambdas, dispatched onto Core::g_Core->GetThreadPool().
        // Reuses the engine threadpool instead of spawning a fresh std::thread per
        // AI turn — keeps thread lifetime managed by Core's existing shutdown
        // ordering and avoids the platform-dependence of std::jthread.  Future is
        // shared so JoinFinishedFutures can poll wait_for(0ms) without
        // consuming the result.
        std::mutex m_ThreadsMutex;
        std::vector<std::shared_future<void>> m_BackgroundFutures;
        std::shared_future<void> m_DrainLoopFuture;
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

        // Pending tool approvals (keyed by requestId).  originConn pins each
        // approval to the connection that triggered it — only that connection
        // may answer.  The pointer is never dereferenced; OnClose cancels any
        // approvals owned by a closing connection so a future reuse of the
        // same address can't match by identity.
        struct PendingApproval
        {
            std::string requestId;
            std::string originSessionId;
            crow::websocket::connection* originConn{nullptr};
            std::mutex mutex;
            std::condition_variable cv;
            bool responded{false};
            bool approved{false};
        };
        std::mutex m_ApprovalsMutex;
        std::unordered_map<std::string, std::shared_ptr<PendingApproval>> m_PendingApprovals;

        // Fail-close every pending approval owned by `conn` (called from
        // OnClose) so the background AI loop unblocks immediately rather
        // than waiting for the 60-second timeout.
        void CancelApprovalsForConnection(crow::websocket::connection* conn);

        static constexpr int AI_CALL_TIMEOUT_MS = 120000; // 2 minutes
        static constexpr int MAX_TOOL_ITERATIONS = 10;    // max tool-call re-sends per turn
        static constexpr int APPROVAL_TIMEOUT_S = 60;     // approval wait timeout
    };
} // namespace AIAssistant
