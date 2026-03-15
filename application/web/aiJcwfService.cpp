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

#include "web/aiJcwfService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "engine.h"
#include "jarvisAgent.h"
#include "workflow/aiCallTaskExecutor.h"
#include "workflow/aiRequestPool.h"
#include "workflow/workflowJsonParser.h"
#include "workflow/workflowValidator.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {
        static constexpr uint64_t AI_CALL_TIMEOUT_MS = 120000; // 2 minutes per AI call
        static constexpr int MAX_GENERATE_RETRIES = 2;

        // Workflow IDs used for logging (visible in Run Analyser).
        static constexpr char const* WF_ID_EXPLAIN = "_ai_explain";
        static constexpr char const* WF_ID_GENERATE = "_ai_generate";

        static std::string GenerateRunId(std::string const& workflowId)
        {
            auto const now = std::chrono::system_clock::now();
            auto const nowTimeT = std::chrono::system_clock::to_time_t(now);
            return workflowId + "_" + std::to_string(static_cast<long long>(nowTimeT));
        }

        static int64_t NowTimestampNs()
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        }

        static fs::path GetQueueBasePath()
        {
            if (Core::g_Core == nullptr)
            {
                return {};
            }
            return fs::absolute(fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath)).lexically_normal();
        }

        static bool WriteFile(fs::path const& filePath, std::string const& content, std::string& outError)
        {
            return AiCallTaskExecutor::WriteTextFile(filePath.string(), content, outError);
        }

        static bool ReadFile(fs::path const& filePath, std::string& outContent)
        {
            std::ifstream stream(filePath, std::ios::in | std::ios::binary);
            if (!stream)
            {
                return false;
            }
            std::ostringstream ss;
            ss << stream.rdbuf();
            outContent = ss.str();
            return true;
        }

        // Build a simple JSON string for WebSocket broadcast.
        // This avoids pulling in crow::json for a background thread.
        static std::string JsonEscape(std::string const& input)
        {
            std::string out;
            out.reserve(input.size() + 32);
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
                            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
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

    } // namespace

    AiJcwfService::~AiJcwfService()
    {
        Shutdown();
    }

    void AiJcwfService::Shutdown()
    {
        m_ShuttingDown.store(true);
        std::lock_guard<std::mutex> lock(m_ThreadsMutex);
        for (auto& thread : m_BackgroundThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        m_BackgroundThreads.clear();
    }

    void AiJcwfService::SetBroadcastFn(BroadcastFn broadcastFn)
    {
        std::lock_guard<std::mutex> lock(m_BroadcastMutex);
        m_BroadcastFn = std::move(broadcastFn);
    }

    void AiJcwfService::Broadcast(std::string const& jsonString)
    {
        std::lock_guard<std::mutex> lock(m_BroadcastMutex);
        if (m_BroadcastFn)
        {
            LOG_APP_INFO("[AiJcwfService] Broadcast: queuing message (len={}, preview='{}')",
                         jsonString.size(), jsonString.substr(0, 120));
            m_BroadcastFn(jsonString);
        }
        else
        {
            LOG_APP_WARN("[AiJcwfService] Broadcast: m_BroadcastFn is null, message dropped (len={})", jsonString.size());
        }
    }

    void AiJcwfService::JoinFinishedThreads()
    {
        // Thread count is bounded by user-initiated requests (one per button click).
        // Threads are joined in Shutdown(). No active cleanup needed here.
    }

    std::string AiJcwfService::LoadGenerationGuide()
    {
        // Try several candidate paths for the generation guide.
        std::vector<fs::path> candidates;

        if (Core::g_Core != nullptr)
        {
            fs::path const launchCwd = Core::g_Core->GetLaunchCWDAbsolute();
            candidates.push_back(launchCwd / "doc" / "jcwf_generation_guide.md");
            candidates.push_back(launchCwd / ".." / "doc" / "jcwf_generation_guide.md");
        }

        candidates.push_back(fs::path("doc") / "jcwf_generation_guide.md");

        for (auto const& path : candidates)
        {
            std::string content;
            if (ReadFile(path, content) && !content.empty())
            {
                LOG_APP_INFO("[AiJcwfService] Loaded generation guide from '{}'", path.string());
                return content;
            }
        }

        LOG_APP_WARN("[AiJcwfService] Could not load jcwf_generation_guide.md from any candidate path");
        return "(Generation guide not available — generate valid JCWF JSON based on your knowledge of the spec.)";
    }

    bool AiJcwfService::ValidateJcwf(std::string const& jcwfJsonText, std::string& outValidationSummary)
    {
        outValidationSummary.clear();

        WorkflowJsonParser parser;
        WorkflowDefinition parsedWorkflow;
        std::string parseError;
        if (!parser.ParseWorkflowJson(jcwfJsonText, parsedWorkflow, parseError))
        {
            outValidationSummary = "Parse error: " + parseError;
            return false;
        }

        std::vector<WorkflowValidationIssue> issues;
        WorkflowValidator::Validate(parsedWorkflow, issues);

        bool hasErrors = false;
        bool hasWarnings = false;

        std::ostringstream ss;
        for (auto const& issue : issues)
        {
            if (issue.m_Severity == WorkflowValidationSeverity::Error)
            {
                ss << "ERROR [" << issue.m_Code << "]: " << issue.m_Message;
                hasErrors = true;
            }
            else if (issue.m_Severity == WorkflowValidationSeverity::Warning)
            {
                ss << "WARNING [" << issue.m_Code << "]: " << issue.m_Message;
                hasWarnings = true;
            }
            else
            {
                continue;
            }

            if (!issue.m_Path.empty())
            {
                ss << " (path: " << issue.m_Path << ")";
            }
            ss << "\n";
        }

        if (!hasErrors && !hasWarnings)
        {
            return true;
        }

        outValidationSummary = ss.str();
        return !hasErrors;
    }

    bool AiJcwfService::RunSingleAiCall(std::string const& subfolderName, std::string const& stngContent,
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
        if (app == nullptr)
        {
            outError = "Application not available";
            return false;
        }

        AiRequestPool* requestPool = app->GetAiRequestPool();
        if (requestPool == nullptr)
        {
            outError = "AiRequestPool not available";
            return false;
        }

        // Create the queue subfolder.
        fs::path const queueDir = queueBase / "_ai_jcwf_service" / subfolderName;
        std::error_code ec;
        fs::create_directories(queueDir, ec);
        if (ec)
        {
            outError = "Failed to create queue directory: " + queueDir.string() + " (" + ec.message() + ")";
            return false;
        }

        // Clean any previous files in the directory.
        for (auto const& entry : fs::directory_iterator(queueDir, ec))
        {
            fs::remove(entry.path(), ec);
        }

        // Register with AiRequestPool BEFORE writing files.
        int64_t const requestId = requestPool->AllocateRequestId();
        int64_t const timestampNs = NowTimestampNs();

        AiRequestHandle handle{};
        handle.requestId = requestId;
        handle.requestTimestampNs = timestampNs;

        // Use PROB_<requestId>_<timestampNs>.txt naming so OnProbFileEvent matches the output.
        std::string const probFilename =
            "PROB_" + std::to_string(requestId) + "_" + std::to_string(timestampNs) + ".txt";
        fs::path const probPath = queueDir / probFilename;

        AiRequestHandle const registered = requestPool->RegisterPending(handle, AI_CALL_TIMEOUT_MS);
        if (!registered.IsValid())
        {
            outError = "Failed to register AI request";
            return false;
        }

        // No PROV file needed — the SessionManager constructor reads the default
        // provider from config.json. A PROV override would only be needed if we
        // later add a per-generator provider setting in the UI.

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

        if (!cntxContent.empty())
        {
            if (!WriteFile(queueDir / "CNTX_context.txt", cntxContent, writeError))
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

        LOG_APP_INFO("[AiJcwfService] AI call dispatched: subfolder='{}' probFile='{}' requestId={} timestampNs={}",
                     subfolderName, probPath.string(), handle.requestId, handle.requestTimestampNs);

        // Wait for completion (blocking — we're on a background thread).
        LOG_APP_INFO("[AiJcwfService] WaitForCompletion: waiting for requestId={} timestampNs={} (timeout={}ms)",
                     handle.requestId, handle.requestTimestampNs, AI_CALL_TIMEOUT_MS);
        std::string errorMessage;
        bool const success = requestPool->WaitForCompletion(handle, AI_CALL_TIMEOUT_MS, outResponseText, errorMessage);
        LOG_APP_INFO("[AiJcwfService] WaitForCompletion: returned success={} responseLen={} error='{}'",
                     success, outResponseText.size(), errorMessage);

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

    // ----------------------------------------------------------------
    // Explain: JCWF → natural language
    // ----------------------------------------------------------------

    void AiJcwfService::ExplainAsync(std::string const& jcwfJsonText)
    {
        JoinFinishedThreads();

        std::string jcwfCopy = jcwfJsonText;

        std::lock_guard<std::mutex> lock(m_ThreadsMutex);
        m_BackgroundThreads.emplace_back(
            [this, jcwfText = std::move(jcwfCopy)]()
            {
                std::string const workflowId = WF_ID_EXPLAIN;
                std::string const runId = GenerateRunId(workflowId);

                LOG_APP_INFO("[workflow] run '{}' started (workflow '{}')", runId, workflowId);

                Broadcast(R"({"type":"ai-explain-progress","message":"Generating explanation..."})");

                std::string const stng =
                    "You are a workflow documentation expert. Explain workflows clearly and concisely in structured English. "
                    "Focus on what the workflow does, the task pipeline, dependencies, data flow, and any error handling.";

                std::string const task = "Describe what this JCWF workflow does in plain English. "
                                         "Structure your explanation with: 1) Overview, 2) Tasks and their roles, "
                                         "3) Dependencies and data flow, 4) Error handling (if any). "
                                         "Keep it concise but complete.";

                std::string const cntx = "--- JCWF Workflow JSON ---\n" + jcwfText;

                std::string const prob = "Explain this JCWF workflow.";

                int const seq = m_NextRequestSeq.fetch_add(1);
                std::string const subfolder = "explain_" + std::to_string(seq);

                LOG_APP_INFO("[workflow] task 'explain' executing in run '{}' (workflow '{}')", runId, workflowId);

                std::string responseText;
                std::string errorMessage;
                bool const ok = RunSingleAiCall(subfolder, stng, task, cntx, prob, responseText, errorMessage);

                if (ok)
                {
                    LOG_APP_INFO("[workflow] task 'explain' completed in run '{}' (workflow '{}')", runId, workflowId);
                    Broadcast(R"({"type":"ai-explain-result","ok":true,"summary":")" + JsonEscape(responseText) + R"("})");
                    LOG_APP_INFO("[workflow] run '{}' completed (workflow '{}')", runId, workflowId);
                }
                else
                {
                    LOG_APP_WARN("[workflow] task 'explain' failed in run '{}': {}", runId, errorMessage);
                    Broadcast(R"({"type":"ai-explain-result","ok":false,"error":")" + JsonEscape(errorMessage) + R"("})");
                    LOG_APP_WARN("[workflow] run '{}' failed (workflow '{}')", runId, workflowId);
                }
            });
    }

    // ----------------------------------------------------------------
    // Generate: natural language → JCWF
    // ----------------------------------------------------------------

    void AiJcwfService::GenerateAsync(std::string const& prompt, std::string const& currentJcwfJson)
    {
        JoinFinishedThreads();

        std::string promptCopy = prompt;
        std::string currentJcwfCopy = currentJcwfJson;

        std::lock_guard<std::mutex> lock(m_ThreadsMutex);
        m_BackgroundThreads.emplace_back(
            [this, userPrompt = std::move(promptCopy), currentJcwf = std::move(currentJcwfCopy)]()
            {
                std::string const workflowId = WF_ID_GENERATE;
                std::string const runId = GenerateRunId(workflowId);
                int const seq = m_NextRequestSeq.fetch_add(1);
                std::string const seqStr = std::to_string(seq);
                int totalStages = 4;

                LOG_APP_INFO("[workflow] run '{}' started (workflow '{}')", runId, workflowId);

                auto broadcastProgress = [&](int stage, std::string const& message)
                {
                    std::ostringstream ss;
                    ss << R"({"type":"ai-generate-progress","stage":)" << stage << R"(,"totalStages":)" << totalStages
                       << R"(,"message":")" << JsonEscape(message) << R"("})";
                    Broadcast(ss.str());
                };

                auto broadcastResult = [&](bool ok, std::string const& jcwfOrError, int retries)
                {
                    if (ok)
                    {
                        // jcwfOrError is raw JSON — embed directly (not string-escaped).
                        std::ostringstream ss;
                        ss << R"({"type":"ai-generate-result","ok":true,"jcwf":)" << jcwfOrError << R"(,"retries":)"
                           << retries << "}";
                        Broadcast(ss.str());
                        LOG_APP_INFO("[workflow] run '{}' completed (workflow '{}')", runId, workflowId);
                    }
                    else
                    {
                        Broadcast(R"({"type":"ai-generate-result","ok":false,"error":")" + JsonEscape(jcwfOrError) +
                                  R"("})");
                        LOG_APP_WARN("[workflow] run '{}' failed (workflow '{}')", runId, workflowId);
                    }
                };

                if (m_ShuttingDown.load())
                {
                    broadcastResult(false, "Service is shutting down", 0);
                    return;
                }

                std::string const generationGuide = LoadGenerationGuide();

                // ----------------------------------------------------------
                // Stage 1: Decompose the prompt
                // ----------------------------------------------------------
                broadcastProgress(1, "Analyzing prompt...");
                LOG_APP_INFO("[workflow] task 'decompose' executing in run '{}' (workflow '{}')", runId, workflowId);

                std::string const decomposeStng =
                    "You are a workflow architect. Break down the user's request into a structured specification "
                    "for a JCWF workflow. Identify the tasks, their types (shell, ai_call, python, internal), "
                    "dependencies between tasks, any error handling needed, and data flow.";

                std::string decomposeTask =
                    "Analyze the user's request and produce a structured task breakdown. For each task, specify:\n"
                    "- Task ID (short slug)\n"
                    "- Task type (shell, ai_call, python, internal)\n"
                    "- What it does\n"
                    "- Dependencies (which tasks must complete first)\n"
                    "- Whether it needs error handling (expose_error_signal + branch)\n"
                    "- For ai_call: what STNG/TASK/CNTX/PROB content should be\n"
                    "- For shell: what command/script to run\n";

                if (!currentJcwf.empty())
                {
                    decomposeTask += "\nThe user wants to MODIFY an existing workflow. Here is the current JCWF:\n"
                                     "--- Current JCWF ---\n" +
                                     currentJcwf + "\n--- End Current JCWF ---\n";
                }

                std::string const decomposeCntx = "--- JCWF Generation Guide (condensed spec) ---\n" + generationGuide;
                std::string const decomposeProb = "User request: " + userPrompt;

                std::string decomposition;
                std::string decomposeError;
                if (!RunSingleAiCall("gen_" + seqStr + "_decompose", decomposeStng, decomposeTask, decomposeCntx,
                                     decomposeProb, decomposition, decomposeError))
                {
                    LOG_APP_WARN("[workflow] task 'decompose' failed in run '{}': {}", runId, decomposeError);
                    broadcastResult(false, "Decomposition failed: " + decomposeError, 0);
                    return;
                }
                LOG_APP_INFO("[workflow] task 'decompose' completed in run '{}' (workflow '{}')", runId, workflowId);

                if (m_ShuttingDown.load())
                {
                    broadcastResult(false, "Service is shutting down", 0);
                    return;
                }

                // ----------------------------------------------------------
                // Stage 2: Generate JCWF JSON
                // ----------------------------------------------------------
                broadcastProgress(2, "Generating JCWF...");
                LOG_APP_INFO("[workflow] task 'generate' executing in run '{}' (workflow '{}')", runId, workflowId);

                std::string const generateStng =
                    "You are a JCWF code generator. Output ONLY valid JSON — no markdown fences, no explanations, "
                    "no comments. The output must parse as a complete JCWF file.";

                std::string const generateTask =
                    "Generate a complete, valid JCWF JSON file based on the task breakdown below. "
                    "Follow the JCWF specification exactly. Key rules:\n"
                    "- Every task's 'id' field must match its key in the 'tasks' map\n"
                    "- ai_call working_directory: '../queue/<workflowId>/<NN>_<taskId>'\n"
                    "- shell working_directory: '<workflowId>/<NN>_<taskId>'\n"
                    "- shell command must start with 'scripts/'\n"
                    "- Use version '1.1' if using control_nodes or controlflow\n"
                    "- depends_on must form a DAG (no cycles)\n"
                    "- expose_error_signal + controlflow edges for error branches\n"
                    "- Include all required controlflow port names (dep-source, error-signal, cf-in-normal, etc.)\n"
                    "Output ONLY the JSON. Nothing else.";

                std::string generateCntx = "--- Task Breakdown ---\n" + decomposition +
                                           "\n\n--- JCWF Generation Guide ---\n" + generationGuide;

                if (!currentJcwf.empty())
                {
                    generateCntx += "\n\n--- Current JCWF (modify this) ---\n" + currentJcwf;
                }

                std::string const generateProb = "Generate the JCWF JSON now.";

                std::string generatedJcwf;
                std::string generateError;
                if (!RunSingleAiCall("gen_" + seqStr + "_generate", generateStng, generateTask, generateCntx,
                                     generateProb, generatedJcwf, generateError))
                {
                    LOG_APP_WARN("[workflow] task 'generate' failed in run '{}': {}", runId, generateError);
                    broadcastResult(false, "Generation failed: " + generateError, 0);
                    return;
                }
                LOG_APP_INFO("[workflow] task 'generate' completed in run '{}' (workflow '{}')", runId, workflowId);

                // Strip markdown fences if the AI wrapped the output.
                {
                    std::string trimmed = generatedJcwf;

                    // Remove leading whitespace.
                    size_t start = trimmed.find_first_not_of(" \t\n\r");
                    if (start != std::string::npos)
                    {
                        trimmed = trimmed.substr(start);
                    }

                    // Remove ```json ... ``` wrapper.
                    if (trimmed.rfind("```", 0) == 0)
                    {
                        size_t firstNewline = trimmed.find('\n');
                        if (firstNewline != std::string::npos)
                        {
                            trimmed = trimmed.substr(firstNewline + 1);
                        }
                    }

                    // Remove trailing ``` .
                    size_t lastFence = trimmed.rfind("```");
                    if (lastFence != std::string::npos && lastFence > 0)
                    {
                        trimmed = trimmed.substr(0, lastFence);
                    }

                    // Trim trailing whitespace.
                    size_t end = trimmed.find_last_not_of(" \t\n\r");
                    if (end != std::string::npos)
                    {
                        trimmed = trimmed.substr(0, end + 1);
                    }

                    generatedJcwf = trimmed;
                }

                // ----------------------------------------------------------
                // Stage 3+: Validate and fix loop
                // ----------------------------------------------------------
                int retries = 0;
                for (int attempt = 0; attempt <= MAX_GENERATE_RETRIES; ++attempt)
                {
                    if (m_ShuttingDown.load())
                    {
                        broadcastResult(false, "Service is shutting down", retries);
                        return;
                    }

                    std::string const validateTaskId = attempt == 0 ? "validate" : "validate_retry_" + std::to_string(attempt);
                    broadcastProgress(3, attempt == 0 ? "Validating..." : "Validating (retry " + std::to_string(attempt) +
                                                                              "/" + std::to_string(MAX_GENERATE_RETRIES) +
                                                                              ")...");
                    LOG_APP_INFO("[workflow] task '{}' executing in run '{}' (workflow '{}')", validateTaskId, runId, workflowId);

                    std::string validationSummary;
                    bool const valid = ValidateJcwf(generatedJcwf, validationSummary);

                    if (valid)
                    {
                        LOG_APP_INFO("[workflow] task '{}' completed in run '{}' (workflow '{}')", validateTaskId, runId, workflowId);
                        broadcastResult(true, generatedJcwf, retries);
                        return;
                    }

                    LOG_APP_WARN("[workflow] task '{}' failed in run '{}': {}", validateTaskId, runId, validationSummary);

                    if (attempt == MAX_GENERATE_RETRIES)
                    {
                        // Out of retries — return what we have with validation info.
                        LOG_APP_WARN("[workflow] run '{}' validation failed after {} retries (workflow '{}')",
                                     runId, MAX_GENERATE_RETRIES, workflowId);

                        // Try to return it anyway if it's parseable JSON.
                        broadcastResult(false, "Validation failed after " + std::to_string(MAX_GENERATE_RETRIES) +
                                                   " retries: " + validationSummary,
                                        retries);
                        return;
                    }

                    // ----------------------------------------------------------
                    // Fix: ask AI to correct the validation errors
                    // ----------------------------------------------------------
                    retries = attempt + 1;
                    std::string const fixTaskId = "fix_" + std::to_string(retries);
                    broadcastProgress(3, "Fixing errors (retry " + std::to_string(retries) + "/" +
                                             std::to_string(MAX_GENERATE_RETRIES) + ")...");
                    LOG_APP_INFO("[workflow] task '{}' executing in run '{}' (workflow '{}')", fixTaskId, runId, workflowId);

                    std::string const fixStng =
                        "You are a JCWF code fixer. Output ONLY valid JSON — no markdown fences, no explanations. "
                        "Fix all validation errors while preserving the workflow's intended behavior.";

                    std::string const fixTask =
                        "The JCWF JSON below has validation errors. Fix them and output the corrected JCWF JSON. "
                        "Output ONLY the fixed JSON, nothing else.\n\n"
                        "Validation errors:\n" +
                        validationSummary;

                    std::string const fixCntx = "--- Current (broken) JCWF ---\n" + generatedJcwf +
                                                "\n\n--- JCWF Generation Guide ---\n" + generationGuide;

                    std::string const fixProb = "Fix the JCWF JSON.";

                    std::string fixedJcwf;
                    std::string fixError;
                    if (!RunSingleAiCall("gen_" + seqStr + "_fix_" + std::to_string(retries), fixStng, fixTask, fixCntx,
                                         fixProb, fixedJcwf, fixError))
                    {
                        LOG_APP_WARN("[workflow] task '{}' failed in run '{}': {}", fixTaskId, runId, fixError);
                        broadcastResult(false, "Fix attempt " + std::to_string(retries) + " failed: " + fixError,
                                        retries);
                        return;
                    }
                    LOG_APP_INFO("[workflow] task '{}' completed in run '{}' (workflow '{}')", fixTaskId, runId, workflowId);

                    // Strip markdown fences again.
                    {
                        std::string trimmed = fixedJcwf;
                        size_t start = trimmed.find_first_not_of(" \t\n\r");
                        if (start != std::string::npos)
                        {
                            trimmed = trimmed.substr(start);
                        }
                        if (trimmed.rfind("```", 0) == 0)
                        {
                            size_t firstNewline = trimmed.find('\n');
                            if (firstNewline != std::string::npos)
                            {
                                trimmed = trimmed.substr(firstNewline + 1);
                            }
                        }
                        size_t lastFence = trimmed.rfind("```");
                        if (lastFence != std::string::npos && lastFence > 0)
                        {
                            trimmed = trimmed.substr(0, lastFence);
                        }
                        size_t end = trimmed.find_last_not_of(" \t\n\r");
                        if (end != std::string::npos)
                        {
                            trimmed = trimmed.substr(0, end + 1);
                        }
                        fixedJcwf = trimmed;
                    }

                    generatedJcwf = fixedJcwf;
                }
            });
    }

} // namespace AIAssistant
