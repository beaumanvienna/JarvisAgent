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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AIAssistant
{
    class AiRequestPool;
    class ScriptRegistry;
    struct GeneratedScript;

    // Callback for broadcasting a JSON string to all WebSocket clients.
    using BroadcastFn = std::function<void(std::string const& jsonString)>;

    // Service for AI-powered JCWF explanation and generation.
    //
    // Both operations write queue files (STNG/TASK/CNTX/PROB) to a temporary
    // subfolder under the queue directory, let a SessionManager process them,
    // and wait for the AI response via AiRequestPool.  Progress and results
    // are delivered through the BroadcastFn callback (WebSocket).
    //
    // Thread safety: all public methods are safe to call from any thread.
    class AiJcwfService final
    {
    public:
        AiJcwfService() = default;
        ~AiJcwfService();

        AiJcwfService(AiJcwfService const&) = delete;
        AiJcwfService& operator=(AiJcwfService const&) = delete;

        void SetBroadcastFn(BroadcastFn broadcastFn);

        // Explain: convert JCWF JSON to a human-readable natural language summary.
        // Runs asynchronously; broadcasts ai-explain-progress / ai-explain-result.
        void ExplainAsync(std::string const& jcwfJsonText);

        // Generate: convert a natural language prompt into a valid JCWF JSON.
        // If currentJcwfJson is non-empty, the AI will modify the existing workflow.
        // Runs asynchronously; broadcasts ai-generate-progress / ai-generate-result.
        void GenerateAsync(std::string const& prompt, std::string const& currentJcwfJson);

        // Shutdown: signal all background threads to stop and join them.
        void Shutdown();

    private:
        // Single AI call: writes queue files, waits for completion, returns response text.
        // Returns true on success; on failure, outError describes the issue.
        bool RunSingleAiCall(std::string const& subfolderName, std::string const& stngContent,
                             std::string const& taskContent, std::string const& cntxContent, std::string const& probContent,
                             std::string& outResponseText, std::string& outError);

        // Validate JCWF JSON text and return errors/warnings as a formatted string.
        // Returns true if the JCWF is valid (no errors).
        static bool ValidateJcwf(std::string const& jcwfJsonText, std::string& outValidationSummary,
                                 ScriptRegistry const* scriptRegistry = nullptr,
                                 std::vector<GeneratedScript> const* pendingScripts = nullptr);

        // Load the generation guide from doc/jcwf_generation_guide.md.
        static std::string LoadGenerationGuide();

        void Broadcast(std::string const& jsonString);

        void JoinFinishedThreads();

        BroadcastFn m_BroadcastFn;
        std::mutex m_BroadcastMutex;

        std::mutex m_ThreadsMutex;
        std::vector<std::thread> m_BackgroundThreads;

        std::atomic<bool> m_ShuttingDown{false};
        std::atomic<int> m_NextRequestSeq{1};
    };
} // namespace AIAssistant
