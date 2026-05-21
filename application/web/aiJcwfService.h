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
    struct WorkflowValidationIssue;

    // Callback for broadcasting a JSON string to all WebSocket clients.
    using BroadcastFn = std::function<void(std::string const& jsonString)>;

    // Service for AI-powered JCWF explanation and generation.
    //
    // Both operations build an AiInvocation envelope and submit it via AiRequestPool.
    // Queue files (STNG/TASK/CNTX/PROB) are written to a per-call subfolder under the
    // queue directory for replay/debug — the envelope is authoritative for dispatch.
    // Progress and results are delivered through the BroadcastFn callback (WebSocket).
    //
    // THREADING & LIFETIME CONTRACT
    //
    // - All public methods are safe to call from any thread.
    // - `ExplainAsync` / `GenerateAsync` / `FixFailedScriptAsync` each spawn a
    //   background `std::thread` that captures `[this, ...by value]` in its
    //   lambda.  The captures are NOT detached — every spawned thread is
    //   tracked in `m_BackgroundThreads` and joined at `Shutdown()` time.
    // - `[this]` capture is safe under the invariant: every path that destroys
    //   an `AiJcwfService` instance routes through `~AiJcwfService`, which
    //   calls `Shutdown()`, which joins ALL outstanding threads BEFORE any
    //   member is destroyed.  In other words: a captured `this` outlives every
    //   member it might dereference.  Per `feedback_capture_by_value_async`
    //   this is the documented lifetime guarantee that makes `[this]` capture
    //   acceptable here.
    // - The class is non-copyable and non-movable (=delete below), so the
    //   instance cannot be relocated mid-flight.  The owning pointer must
    //   destroy via the destructor (unique_ptr, automatic storage, etc.) —
    //   abandoning an instance via `delete`-of-base-via-cast or similar would
    //   skip Shutdown and break the contract.  No such path exists today; the
    //   service is owned by the WebServer subsystem in JarvisAgent.
    // - `Shutdown()` is idempotent: calling it more than once (e.g. an
    //   external operator Shutdown followed by ~AiJcwfService) is safe; the
    //   second call observes an empty `m_BackgroundThreads`.
    // - Background lambdas observe `m_ShuttingDown` at every safe break point
    //   (pre-broadcast, between AI stages) and bail early; the join in
    //   `Shutdown` therefore waits at most for the currently-active AI call
    //   to time out via `RunSingleAiCall`'s `wait_for`.
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

        // Fix a failed script: reads the script from disk, sends it + stderr to AI for a fix.
        // Runs asynchronously; broadcasts ai-fix-script-progress / ai-fix-script-result.
        void FixFailedScriptAsync(std::string const& scriptPath, std::string const& stderrContent,
                                  std::string const& taskType);

        // Shutdown: signal all background threads to stop and join them.
        void Shutdown();

        // Validate JCWF JSON text and return errors/warnings as a formatted string.
        // Returns true if the JCWF is valid (no errors).
        // Public so assistant tools can call it directly.
        static bool ValidateJcwf(std::string const& jcwfJsonText, std::string& outValidationSummary,
                                 ScriptRegistry const* scriptRegistry = nullptr,
                                 std::vector<GeneratedScript> const* pendingScripts = nullptr,
                                 std::vector<WorkflowValidationIssue>* outIssues = nullptr);

    private:
        // Single AI call: writes queue files to disk (for replay/debug), builds an AiInvocation
        // envelope, submits it via AiRequestPool::Submit, and waits for the reply.
        // Returns true on success; on failure, outError describes the issue.
        // The JCWF-configured AI interface (config.json `jcwf_ai_interface_index`) is used —
        // empty means "default interface".
        //
        // outputSchemaJson: when non-empty, the envelope declares this schema on its
        // m_OutputSchemaJson field and the reply path validates + retries on schema
        // failure (inside AiRequestPool::Submit).  On structured success, outResponseText
        // receives the validated JSON payload.
        bool RunSingleAiCall(std::string const& subfolderName, std::string const& stngContent,
                             std::string const& taskContent, std::string const& cntxContent, std::string const& probContent,
                             std::string& outResponseText, std::string& outError,
                             std::string const& outputSchemaJson = "");

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
