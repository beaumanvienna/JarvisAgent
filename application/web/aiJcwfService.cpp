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
#include "file/scriptRegistry.h"
#include "simdjson/simdjson.h"
#include "workflow/aiCallTaskExecutor.h"
#include "workflow/aiRequestPool.h"
#include "workflow/workflowJsonParser.h"
#include "workflow/workflowFileIndex.h"
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

        // GeneratedScript is defined in workflow/workflowValidator.h

        // Extract script paths referenced by shell/python tasks in a JCWF JSON string.
        static std::vector<std::string> ExtractScriptPaths(std::string const& jcwfJson)
        {
            std::vector<std::string> paths;

            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(jcwfJson);
            simdjson::ondemand::document doc;
            if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
            {
                return paths;
            }

            simdjson::ondemand::object tasksObj;
            if (doc["tasks"].get_object().get(tasksObj) != simdjson::SUCCESS)
            {
                return paths;
            }

            for (auto field : tasksObj)
            {
                simdjson::ondemand::object taskObj;
                if (field.value().get_object().get(taskObj) != simdjson::SUCCESS)
                {
                    continue;
                }

                std::string_view typeView;
                if (taskObj["type"].get_string().get(typeView) != simdjson::SUCCESS)
                {
                    continue;
                }

                if (typeView != "shell" && typeView != "python")
                {
                    continue;
                }

                // Shell tasks: extract script path from "command" field.
                std::string_view commandView;
                if (taskObj["command"].get_string().get(commandView) == simdjson::SUCCESS)
                {
                    std::string command(commandView);
                    if (command.rfind("scripts/", 0) == 0)
                    {
                        // Strip arguments after the script path (first space-separated token)
                        size_t spacePos = command.find(' ');
                        std::string scriptPath = (spacePos != std::string::npos) ? command.substr(0, spacePos) : command;
                        paths.push_back(std::move(scriptPath));
                    }
                }

                // Python tasks: extract script path from "params.module" field.
                // Module "scripts.parseSSHLog" → file "scripts/parseSSHLog.py".
                if (typeView == "python")
                {
                    simdjson::ondemand::object paramsObj;
                    if (taskObj["params"].get_object().get(paramsObj) == simdjson::SUCCESS)
                    {
                        std::string_view moduleView;
                        if (paramsObj["module"].get_string().get(moduleView) == simdjson::SUCCESS)
                        {
                            std::string modulePath(moduleView);
                            // Convert dots to slashes and append .py
                            for (char& c : modulePath)
                            {
                                if (c == '.')
                                {
                                    c = '/';
                                }
                            }
                            modulePath += ".py";
                            if (modulePath.rfind("scripts/", 0) == 0)
                            {
                                paths.push_back(std::move(modulePath));
                            }
                        }
                    }
                }
            }

            return paths;
        }

    } // namespace

    AiJcwfService::~AiJcwfService() { Shutdown(); }

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
            LOG_APP_INFO("[AiJcwfService] Broadcast: queuing message (len={}, preview='{}')", jsonString.size(),
                         jsonString.substr(0, 120));
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

    bool AiJcwfService::ValidateJcwf(std::string const& jcwfJsonText, std::string& outValidationSummary,
                                     ScriptRegistry const* scriptRegistry,
                                     std::vector<GeneratedScript> const* pendingScripts)
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

        WorkflowFileIndex const* fileIndex = nullptr;
        if (JarvisAgent* app = App::g_App)
        {
            fileIndex = app->GetWorkflowFileIndex();
        }

        std::vector<WorkflowValidationIssue> issues;
        WorkflowValidator::Validate(parsedWorkflow, scriptRegistry, pendingScripts, issues, fileIndex);

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
            if (!issue.m_TaskId.empty())
            {
                ss << " (task: " << issue.m_TaskId << ")";
            }
            ss << "\n";
            if (!issue.m_SuggestedFix.empty())
            {
                ss << "  FIX: " << issue.m_SuggestedFix << "\n";
            }
            if (!issue.m_Context.empty())
            {
                ss << "  CONTEXT: " << issue.m_Context << "\n";
            }
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
                                        std::string const& probContent, std::string& outResponseText, std::string& outError)
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
        std::string const probFilename = "PROB_" + std::to_string(requestId) + "_" + std::to_string(timestampNs) + ".txt";
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
        LOG_APP_INFO("[AiJcwfService] WaitForCompletion: returned success={} responseLen={} error='{}'", success,
                     outResponseText.size(), errorMessage);

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
                int const seq = m_NextRequestSeq.fetch_add(1);
                std::string const seqStr = std::to_string(seq);

                LOG_APP_INFO("[workflow] run '{}' started (workflow '{}')", runId, workflowId);

                // ----------------------------------------------------------
                // Stage 1: High-level explanation
                // ----------------------------------------------------------
                Broadcast(R"({"type":"ai-explain-progress","message":"Generating explanation..."})");

                std::string const stng1 = "Be succinct. No embellishments. No preamble. No closing remarks. "
                                          "Output ONLY the structured explanation — nothing else.";

                std::string const task1 =
                    "Produce a brief, structured explanation of this JCWF workflow.\n"
                    "Use these sections ONLY:\n"
                    "1) Overview (2-3 sentences max)\n"
                    "2) Tasks — for each task you MUST state: id, type, working_directory, "
                    "queue_binding content (exact STNG/TASK/CNTX/PROB text), file_inputs, "
                    "materialize mappings, expose_error_signal value, depends_on list.\n"
                    "3) Dependencies and controlflow — MUST list every controlflow edge "
                    "(from, to, kind, from_port, to_port). MUST list every depends_on.\n"
                    "4) Error handling — MUST state which tasks expose error signals and how branches route.\n"
                    "Rules:\n"
                    "- MUST reproduce every queue_binding file content verbatim — do NOT truncate.\n"
                    "- MUST reproduce every file_inputs path verbatim.\n"
                    "- MUST reproduce every materialize mapping verbatim.\n"
                    "- MUST note shared working directories.\n"
                    "- Use SHORT sentences. No filler. No commentary. No examples.";

                std::string const cntx1 = "--- JCWF Workflow JSON ---\n" + jcwfText;
                std::string const prob1 = "Explain this JCWF workflow. Be brief.";

                LOG_APP_INFO("[workflow] task 'explain_stage1' executing in run '{}' (workflow '{}')", runId, workflowId);

                std::string stage1Response;
                std::string stage1Error;
                bool const stage1Ok = RunSingleAiCall("explain_" + seqStr + "_stage1", stng1, task1, cntx1, prob1,
                                                      stage1Response, stage1Error);

                if (!stage1Ok)
                {
                    LOG_APP_WARN("[workflow] task 'explain_stage1' failed in run '{}': {}", runId, stage1Error);
                    Broadcast(R"({"type":"ai-explain-result","ok":false,"error":")" + JsonEscape(stage1Error) + R"("})");
                    LOG_APP_WARN("[workflow] run '{}' failed (workflow '{}')", runId, workflowId);
                    return;
                }
                LOG_APP_INFO("[workflow] task 'explain_stage1' completed in run '{}' (workflow '{}')", runId, workflowId);

                if (m_ShuttingDown.load())
                {
                    Broadcast(R"({"type":"ai-explain-result","ok":false,"error":"Service is shutting down"})");
                    return;
                }

                // ----------------------------------------------------------
                // Stage 2: Review / refine / enrich with spec awareness
                // ----------------------------------------------------------
                Broadcast(R"({"type":"ai-explain-progress","message":"Reviewing and enriching explanation..."})");

                std::string const generationGuide = LoadGenerationGuide();

                std::string const stng2 = "Be succinct. No embellishments. No preamble. No closing remarks. "
                                          "Output ONLY the corrected explanation — nothing else.";

                std::string const task2 =
                    "Review the explanation against the JCWF JSON and the JCWF specification.\n"
                    "Rules:\n"
                    "- MUST fix any inaccuracy.\n"
                    "- MUST verify every queue_binding content string matches the JSON verbatim.\n"
                    "- MUST verify every file_inputs path matches the JSON verbatim.\n"
                    "- MUST verify every materialize mapping matches the JSON verbatim.\n"
                    "- MUST verify every controlflow edge (from, to, kind, ports) matches the JSON.\n"
                    "- MUST verify expose_error_signal values match the JSON.\n"
                    "- MUST verify shared vs unique working directories.\n"
                    "- MUST verify depends_on lists match the JSON.\n"
                    "- MUST keep same structure: Overview, Tasks, Dependencies, Error handling.\n"
                    "- MUST keep output brief — short sentences, no filler, no commentary.\n"
                    "- The result MUST be precise enough to recreate the JCWF from the explanation alone.";

                std::string const cntx2 = "--- JCWF Workflow JSON ---\n" + jcwfText +
                                          "\n\n--- JCWF Specification (condensed) ---\n" + generationGuide +
                                          "\n\n--- Explanation to review and enrich ---\n" + stage1Response;

                std::string const prob2 = "Review and correct this explanation. Keep it brief.";

                LOG_APP_INFO("[workflow] task 'explain_stage2' executing in run '{}' (workflow '{}')", runId, workflowId);

                std::string stage2Response;
                std::string stage2Error;
                bool const stage2Ok = RunSingleAiCall("explain_" + seqStr + "_stage2", stng2, task2, cntx2, prob2,
                                                      stage2Response, stage2Error);

                if (stage2Ok)
                {
                    LOG_APP_INFO("[workflow] task 'explain_stage2' completed in run '{}' (workflow '{}')", runId,
                                 workflowId);
                    Broadcast(R"({"type":"ai-explain-result","ok":true,"summary":")" + JsonEscape(stage2Response) + R"("})");
                    LOG_APP_INFO("[workflow] run '{}' completed (workflow '{}')", runId, workflowId);
                }
                else
                {
                    // Stage 2 failed — fall back to Stage 1 result (still useful).
                    LOG_APP_WARN("[workflow] task 'explain_stage2' failed in run '{}': {} — returning stage 1 result", runId,
                                 stage2Error);
                    Broadcast(R"({"type":"ai-explain-result","ok":true,"summary":")" + JsonEscape(stage1Response) + R"("})");
                    LOG_APP_INFO("[workflow] run '{}' completed with stage 1 fallback (workflow '{}')", runId, workflowId);
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
                int totalStages = 5;

                LOG_APP_INFO("[workflow] run '{}' started (workflow '{}')", runId, workflowId);

                auto broadcastProgress = [&](int stage, std::string const& message)
                {
                    std::ostringstream ss;
                    ss << R"({"type":"ai-generate-progress","stage":)" << stage << R"(,"totalStages":)" << totalStages
                       << R"(,"message":")" << JsonEscape(message) << R"("})";
                    Broadcast(ss.str());
                };

                std::vector<GeneratedScript> generatedScripts;

                auto broadcastResult = [&](bool ok, std::string const& jcwfOrError, int retries)
                {
                    if (ok)
                    {
                        // jcwfOrError is raw JSON — embed directly (not string-escaped).
                        std::ostringstream ss;
                        ss << R"({"type":"ai-generate-result","ok":true,"jcwf":)" << jcwfOrError << R"(,"retries":)"
                           << retries;

                        if (!generatedScripts.empty())
                        {
                            ss << R"(,"scripts":[)";
                            for (size_t i = 0; i < generatedScripts.size(); ++i)
                            {
                                if (i > 0)
                                    ss << ",";
                                ss << R"({"path":")" << JsonEscape(generatedScripts[i].path) << R"(","content":")"
                                   << JsonEscape(generatedScripts[i].content) << R"(","executable":)"
                                   << (generatedScripts[i].executable ? "true" : "false") << "}";
                            }
                            ss << "]";
                        }

                        ss << "}";
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

                std::string const decomposeStng = "Be succinct. No embellishments. No preamble. No closing remarks. "
                                                  "Output ONLY the structured task breakdown — nothing else.";

                std::string decomposeTask =
                    "Produce a structured task breakdown from the user's request.\n"
                    "For each task you MUST specify:\n"
                    "- task_id (short slug)\n"
                    "- type (shell | ai_call | python | internal)\n"
                    "- label\n"
                    "- working_directory (ai_call: '../queue/<wfId>/<NN>_<taskId>', shell: '<wfId>/<NN>_<taskId>')\n"
                    "- depends_on list\n"
                    "- expose_error_signal (true/false)\n"
                    "- For ai_call: exact STNG, TASK, CNTX, PROB file content. "
                    "Use cntx_files string paths (not inline objects) to feed upstream outputs to the AI.\n"
                    "- For shell: command (MUST start with 'scripts/'), args, file_inputs paths, materialize mappings\n"
                    "- For python: module (MUST start with 'scripts.'), function name, file_inputs, file_outputs. "
                    "The runtime calls function(**kwargs, context=dict) — NOT via CLI.\n"
                    "- If error handling needed: which branch node, which controlflow edges (from, to, kind, ports)\n"
                    "Rules:\n"
                    "- Every ai_call STNG content MUST include 'No markdown fences, no explanations.' "
                    "because AI output is consumed directly by compilers/tools, not humans.\n"
                    "- Branch nodes MUST appear ONLY in control_nodes, NOT in tasks.\n"
                    "- Every controlflow edge MUST specify from, to, kind, from_port, to_port.\n"
                    "- Use MUST and SHALL for hard constraints. Leave no ambiguity.\n";

                if (!currentJcwf.empty())
                {
                    decomposeTask += "\nThe user wants to MODIFY an existing workflow. Here is the current JCWF:\n"
                                     "--- Current JCWF ---\n" +
                                     currentJcwf + "\n--- End Current JCWF ---\n";
                }

                std::string scriptRegistryTable;
                if (JarvisAgent* app = App::g_App; app && app->GetScriptRegistry())
                {
                    scriptRegistryTable = app->GetScriptRegistry()->SerializeMarkdownTable();
                }

                std::string workflowFileListing;
                if (JarvisAgent* app = App::g_App; app && app->GetWorkflowFileIndex())
                {
                    // Re-scan so the listing is fresh
                    app->GetWorkflowFileIndex()->ScanDirectory(app->GetWorkflowFileIndex()->GetRootDirectory());
                    workflowFileListing = app->GetWorkflowFileIndex()->SerializeMarkdownListing();
                }

                std::string decomposeCntx = "--- JCWF Generation Guide (condensed spec) ---\n" + generationGuide;
                if (!scriptRegistryTable.empty())
                {
                    decomposeCntx += "\n\n--- Script Registry ---\n" + scriptRegistryTable;
                }
                if (!workflowFileListing.empty())
                {
                    decomposeCntx += "\n\n--- Workflow File Inventory (paths relative to workflows/) ---\n"
                                     "These files already exist on disk. Use them in file_inputs when appropriate.\n" +
                                     workflowFileListing;
                }
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

                std::string const generateStng = "Output ONLY valid JSON. No markdown fences. No explanations. No comments. "
                                                 "The output MUST parse as a complete JCWF file.";

                std::string const generateTask =
                    "Generate a complete JCWF JSON file from the task breakdown below.\n"
                    "MUST rules:\n"
                    "- Every task 'id' field MUST match its key in the 'tasks' map.\n"
                    "- ai_call working_directory MUST be '../queue/<workflowId>/<NN>_<taskId>'.\n"
                    "- shell working_directory MUST be '<workflowId>/<NN>_<taskId>'.\n"
                    "- shell command MUST start with 'scripts/'.\n"
                    "- python params.module MUST start with 'scripts.' (e.g. 'scripts.parseLog').\n"
                    "- python params.function MUST name the actual callable in the script.\n"
                    "- ai_call cntx_files: use string paths to upstream outputs, NOT inline objects with placeholders.\n"
                    "- ai_call cntx_files crossing from queue to workflows: use '../../../workflows/<pythonWorkDir>/<file>' "
                    "(3 levels up from queue/X/Y to root, then into workflows/). NEVER use only '../../'.\n"
                    "- file_inputs values are bare filenames relative to working_directory (e.g. 'input.log'). "
                    "NEVER prefix with the working_directory path — that doubles the path at runtime.\n"
                    "- Prefer a SINGLE combined JSON output file over splitting into many files.\n"
                    "- version MUST be '1.1' if using control_nodes or controlflow.\n"
                    "- depends_on MUST form a DAG (no cycles).\n"
                    "- Branch nodes MUST appear ONLY in control_nodes, NEVER in tasks.\n"
                    "- expose_error_signal + controlflow edges for error branches.\n"
                    "- Every controlflow edge MUST have from, to, kind, from_port, to_port.\n"
                    "- Port names: dep-source, error-signal, cf-in-normal, cf-in-error, "
                    "cf-out-normal, cf-out-error, dep-target.\n"
                    "- Every ai_call stng_files content MUST include 'No markdown fences, no explanations.' "
                    "because AI output is consumed directly by compilers/tools.\n"
                    "Output ONLY the JSON. Nothing else.";

                std::string generateCntx =
                    "--- Task Breakdown ---\n" + decomposition + "\n\n--- JCWF Generation Guide ---\n" + generationGuide;
                if (!scriptRegistryTable.empty())
                {
                    generateCntx += "\n\n--- Script Registry ---\n" + scriptRegistryTable;
                }
                if (!workflowFileListing.empty())
                {
                    generateCntx += "\n\n--- Workflow File Inventory (paths relative to workflows/) ---\n"
                                    "These files already exist on disk. Use them in file_inputs when appropriate.\n" +
                                    workflowFileListing;
                }

                if (!currentJcwf.empty())
                {
                    generateCntx += "\n\n--- Current JCWF (modify this) ---\n" + currentJcwf;
                }

                std::string const generateProb = "Generate the JCWF JSON.";

                std::string generatedJcwf;
                std::string generateError;
                if (!RunSingleAiCall("gen_" + seqStr + "_generate", generateStng, generateTask, generateCntx, generateProb,
                                     generatedJcwf, generateError))
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
                // Stage 3: Generate companion scripts
                // ----------------------------------------------------------
                if (m_ShuttingDown.load())
                {
                    broadcastResult(false, "Service is shutting down", 0);
                    return;
                }

                {
                    broadcastProgress(3, "Checking for new scripts...");
                    LOG_APP_INFO("[workflow] task 'generate_scripts' executing in run '{}' (workflow '{}')", runId,
                                 workflowId);

                    std::vector<std::string> scriptPaths = ExtractScriptPaths(generatedJcwf);

                    // Filter to only scripts that don't exist on disk
                    std::vector<std::string> newScripts;
                    for (auto const& sp : scriptPaths)
                    {
                        if (!fs::exists(sp))
                        {
                            newScripts.push_back(sp);
                        }
                    }

                    if (!newScripts.empty())
                    {
                        LOG_APP_INFO("[workflow] generating {} new script(s) in run '{}'", newScripts.size(), runId);

                        for (size_t i = 0; i < newScripts.size(); ++i)
                        {
                            if (m_ShuttingDown.load())
                            {
                                broadcastResult(false, "Service is shutting down", 0);
                                return;
                            }

                            std::string const& scriptPath = newScripts[i];
                            bool const isShell = scriptPath.ends_with(".sh");

                            broadcastProgress(3, "Generating " + scriptPath + " (" + std::to_string(i + 1) + "/" +
                                                     std::to_string(newScripts.size()) + ")...");

                            std::string const scriptStng =
                                "Output ONLY the raw script file content. No markdown fences. No explanations. "
                                "No introductory or closing commentary. The output must be a valid, runnable script.";

                            std::string scriptTask;
                            if (isShell)
                            {
                                scriptTask = "Generate a bash script for '" + scriptPath +
                                             "'.\n"
                                             "Rules:\n"
                                             "- First line MUST be: #!/usr/bin/env bash\n"
                                             "- Second line MUST be: # @jarvis-script\n"
                                             "- Include metadata with COLON format: # @short: ..., # @params: ..., "
                                             "# @description: ..., # @outputs: ... (if any).\n"
                                             "- After the metadata header: set -euo pipefail\n"
                                             "- Use positional args ($1, $2, ...) for parameters.\n"
                                             "- Output ONLY the script. Nothing else.";
                            }
                            else
                            {
                                scriptTask = "Generate a Python script for '" + scriptPath +
                                             "'.\n"
                                             "Rules:\n"
                                             "- First line MUST be: #!/usr/bin/env python3\n"
                                             "- Second line MUST be: # @jarvis-script\n"
                                             "- Include metadata with COLON format: # @short: ..., # @description: ..., "
                                             "# @outputs: ... (if any).\n"
                                             "- The runtime calls the function programmatically: "
                                             "module.function(**kwargs, context=dict). Do NOT use sys.argv, argparse, "
                                             "or main().\n"
                                             "- The function name MUST match the 'function' field in the JCWF params.\n"
                                             "- Accept `context=None` and `**kwargs` as parameters.\n"
                                             "- Read file inputs via context['_file_input_0'], context['_file_input_1'], "
                                             "etc. (absolute resolved paths from file_inputs).\n"
                                             "- Get working directory via context['_task_working_directory'].\n"
                                             "- Write output files to the working directory using os.path.join().\n"
                                             "- Output ONLY the script. Nothing else.";
                            }

                            std::string const scriptCntx =
                                "--- JCWF Workflow ---\n" + generatedJcwf + "\n\n--- User Request ---\n" + userPrompt;

                            std::string const scriptProb = "Generate the script: " + scriptPath;

                            std::string scriptContent;
                            std::string scriptError;
                            if (!RunSingleAiCall("gen_" + seqStr + "_script_" + std::to_string(i), scriptStng, scriptTask,
                                                 scriptCntx, scriptProb, scriptContent, scriptError))
                            {
                                LOG_APP_WARN("[workflow] script generation failed for '{}' in run '{}': {}", scriptPath,
                                             runId, scriptError);
                                // Non-fatal — continue with remaining scripts
                                continue;
                            }

                            // Strip markdown fences if the AI wrapped the output
                            {
                                std::string trimmed = scriptContent;
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
                                scriptContent = trimmed;
                            }

                            GeneratedScript gs;
                            gs.path = scriptPath;
                            gs.content = scriptContent;
                            gs.executable = isShell;
                            generatedScripts.push_back(std::move(gs));

                            LOG_APP_INFO("[workflow] generated script '{}' in run '{}'", scriptPath, runId);
                        }
                    }

                    LOG_APP_INFO("[workflow] task 'generate_scripts' completed in run '{}' ({} scripts generated)", runId,
                                 generatedScripts.size());
                }

                // Minimum annunciation time for stages 4 and 5 (so the user can see them).
                static constexpr int64_t MIN_STAGE_DISPLAY_MS = 500;

                auto ensureMinDisplay = [](std::chrono::steady_clock::time_point const& start)
                {
                    auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                            .count();
                    if (elapsed < MIN_STAGE_DISPLAY_MS)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(MIN_STAGE_DISPLAY_MS - elapsed));
                    }
                };

                // Helper: run validation and return {valid, summary}
                auto runValidation = [&](std::string const& taskLabel) -> std::pair<bool, std::string>
                {
                    ScriptRegistry const* scriptRegistry = nullptr;
                    if (JarvisAgent* app = App::g_App; app != nullptr)
                    {
                        scriptRegistry = app->GetScriptRegistry();
                    }
                    std::string summary;
                    bool valid = ValidateJcwf(generatedJcwf, summary, scriptRegistry, &generatedScripts);
                    if (!summary.empty())
                    {
                        LOG_APP_WARN("[workflow] task '{}' in run '{}':\n{}", taskLabel, runId, summary);
                    }
                    return {valid, summary};
                };

                // Helper: strip markdown fences from AI response
                auto stripMarkdownFences = [](std::string const& input) -> std::string
                {
                    std::string trimmed = input;
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
                    return trimmed;
                };

                // ----------------------------------------------------------
                // Stage 4: Validate (first pass)
                // ----------------------------------------------------------
                {
                    if (m_ShuttingDown.load())
                    {
                        broadcastResult(false, "Service is shutting down", 0);
                        return;
                    }

                    auto stageStart = std::chrono::steady_clock::now();
                    broadcastProgress(4, "Validating...");
                    LOG_APP_INFO("[workflow] task 'validate' executing in run '{}' (workflow '{}')", runId, workflowId);

                    auto [valid, validationSummary] = runValidation("validate");

                    LOG_APP_INFO("[workflow] task 'validate' completed in run '{}' (workflow '{}') — {} errors, {} warnings",
                                 runId, workflowId, valid ? "no" : "has", validationSummary.empty() ? "no" : "has");
                    ensureMinDisplay(stageStart);

                    // ----------------------------------------------------------
                    // Stage 5: Fix-It (if there are any errors or warnings)
                    // ----------------------------------------------------------
                    if (m_ShuttingDown.load())
                    {
                        broadcastResult(false, "Service is shutting down", 0);
                        return;
                    }

                    if (validationSummary.empty())
                    {
                        // No errors, no warnings — announce and finish
                        auto fixStart = std::chrono::steady_clock::now();
                        broadcastProgress(5, "No errors, no warnings to fix");
                        LOG_APP_INFO("[workflow] task 'fix' skipped in run '{}' — nothing to fix", runId);
                        ensureMinDisplay(fixStart);
                        broadcastResult(true, generatedJcwf, 0);
                        return;
                    }

                    // There are issues (errors and/or warnings) — ask AI to fix them
                    auto fixStart = std::chrono::steady_clock::now();
                    broadcastProgress(5, "Fixing errors and warnings...");
                    LOG_APP_INFO("[workflow] task 'fix' executing in run '{}' (workflow '{}')", runId, workflowId);

                    std::string const fixStng =
                        "You are a JCWF code fixer. Output ONLY valid JSON — no markdown fences, no explanations, "
                        "no introductory or closing commentary. Fix all validation errors AND warnings while "
                        "preserving the workflow's intended behavior.";

                    std::string const fixTask =
                        "The JCWF JSON below has validation issues. Fix ALL errors AND warnings, then output the "
                        "corrected JCWF JSON. Output ONLY the fixed JSON, nothing else.\n\n"
                        "Validation issues:\n" +
                        validationSummary;

                    std::string const fixCntx =
                        "--- Current JCWF ---\n" + generatedJcwf + "\n\n--- JCWF Generation Guide ---\n" + generationGuide;

                    std::string const fixProb = "Fix the JCWF JSON.";

                    std::string fixedJcwf;
                    std::string fixError;
                    if (!RunSingleAiCall("gen_" + seqStr + "_fix", fixStng, fixTask, fixCntx, fixProb, fixedJcwf, fixError))
                    {
                        LOG_APP_WARN("[workflow] task 'fix' failed in run '{}': {}", runId, fixError);
                        ensureMinDisplay(fixStart);
                        // Return the unfixed JCWF — it may still be usable
                        broadcastResult(!WorkflowValidator::HasErrors({}), generatedJcwf, 1);
                        return;
                    }

                    generatedJcwf = stripMarkdownFences(fixedJcwf);
                    LOG_APP_INFO("[workflow] task 'fix' completed in run '{}' (workflow '{}')", runId, workflowId);
                    ensureMinDisplay(fixStart);
                }

                // ----------------------------------------------------------
                // Stage 4 (second pass): Re-validate after fix
                // ----------------------------------------------------------
                {
                    if (m_ShuttingDown.load())
                    {
                        broadcastResult(false, "Service is shutting down", 1);
                        return;
                    }

                    auto stageStart = std::chrono::steady_clock::now();
                    broadcastProgress(4, "Re-validating...");
                    LOG_APP_INFO("[workflow] task 'revalidate' executing in run '{}' (workflow '{}')", runId, workflowId);

                    auto [valid, validationSummary] = runValidation("revalidate");

                    if (!validationSummary.empty())
                    {
                        LOG_APP_WARN("[workflow] task 'revalidate' still has issues in run '{}':\n{}", runId,
                                     validationSummary);
                    }
                    LOG_APP_INFO("[workflow] task 'revalidate' completed in run '{}' (workflow '{}')", runId, workflowId);
                    ensureMinDisplay(stageStart);

                    // Accept result regardless — we only do one fix iteration
                    broadcastResult(true, generatedJcwf, 1);
                }
            });
    }

} // namespace AIAssistant
