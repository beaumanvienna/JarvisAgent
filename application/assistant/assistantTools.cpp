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

#include "assistant/assistantTools.h"
#include "assistant/assistantMemory.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "python/pythonEngine.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowRuntimeManager.h"
#include "workflow/workflowTypes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace AIAssistant
{
    // -----------------------------------------------------------------
    // ToolRegistry constructor — register all tools
    // -----------------------------------------------------------------

    ToolRegistry::ToolRegistry()
    {
        // L1 tools
        m_ToolDefs.push_back(
            {"get_system_status",
             "Returns JarvisAgent system status: version, uptime, active runs, Python engine state, session manager count.",
             "No arguments.", false});
        m_ToolFns["get_system_status"] = [this](auto const& a) { return ExecGetSystemStatus(a); };

        m_ToolDefs.push_back(
            {"list_workflows", "Returns a list of all registered workflow IDs and their labels.", "No arguments.", false});
        m_ToolFns["list_workflows"] = [this](auto const& a) { return ExecListWorkflows(a); };

        m_ToolDefs.push_back({"get_run_status", "Returns the state of a specific workflow run including per-task states.",
                              "Args: run_id (string, required)", false});
        m_ToolFns["get_run_status"] = [this](auto const& a) { return ExecGetRunStatus(a); };

        m_ToolDefs.push_back({"list_recent_runs", "Returns the last completed/failed run per workflow and all active runs.",
                              "Args: count (integer, optional, default 10)", false});
        m_ToolFns["list_recent_runs"] = [this](auto const& a) { return ExecListRecentRuns(a); };

        m_ToolDefs.push_back({"get_task_output", "Returns captured stdout/stderr for a task in a workflow run.",
                              "Args: run_id (string, required), task_id (string, required)", false});
        m_ToolFns["get_task_output"] = [this](auto const& a) { return ExecGetTaskOutput(a); };

        m_ToolDefs.push_back({"run_workflow", "Starts a workflow run. Returns the assigned run ID.",
                              "Args: workflow_id (string, required)", true});
        m_ToolFns["run_workflow"] = [this](auto const& a) { return ExecRunWorkflow(a); };

        // L2 read-only tools
        m_ToolDefs.push_back({"read_file", "Reads file content. Returns the text content of a file with line numbers.",
                              "Args: path (string, required), start (integer, optional, 1-indexed start line), end "
                              "(integer, optional, 1-indexed end line)",
                              false});
        m_ToolFns["read_file"] = [this](auto const& a) { return ExecReadFile(a); };

        m_ToolDefs.push_back({"search_files",
                              "Searches for a text pattern across files using ripgrep. Returns matching lines with file "
                              "paths and line numbers.",
                              "Args: query (string, required), glob (string, optional, e.g. \"*.cpp\")", false});
        m_ToolFns["search_files"] = [this](auto const& a) { return ExecSearchFiles(a); };

        m_ToolDefs.push_back({"list_files", "Lists files and directories at a given path.",
                              "Args: path (string, optional, default \".\"), depth (integer, optional, default 2)", false});
        m_ToolFns["list_files"] = [this](auto const& a) { return ExecListFiles(a); };

        // get_log_tail removed from AI tools — use /log [N] slash command instead.
        // The AI tool was self-defeating: the AI call itself generates log lines,
        // so the user sees AI-internal noise instead of pre-existing log content.

        // --- Memory tools ---
        m_ToolDefs.push_back({"save_memory",
                              "Saves a fact or preference to persistent memory. If a memory with the same key exists, "
                              "it is updated.",
                              "Args: key (string, required), value (string, required), tags (string, optional, "
                              "comma-separated)",
                              false});
        m_ToolFns["save_memory"] = [this](auto const& a) { return ExecSaveMemory(a); };

        m_ToolDefs.push_back({"recall_memory",
                              "Searches persistent memory by keyword. Returns matching memories sorted by relevance.",
                              "Args: query (string, required)", false});
        m_ToolFns["recall_memory"] = [this](auto const& a) { return ExecRecallMemory(a); };

        m_ToolDefs.push_back({"list_memories", "Lists all saved memories (keys and timestamps).", "Args: none", false});
        m_ToolFns["list_memories"] = [this](auto const& a) { return ExecListMemories(a); };

        m_ToolDefs.push_back({"delete_memory", "Deletes a memory by its key.", "Args: key (string, required)", false});
        m_ToolFns["delete_memory"] = [this](auto const& a) { return ExecDeleteMemory(a); };
    }

    // -----------------------------------------------------------------
    // BuildToolDescriptions — for system prompt
    // -----------------------------------------------------------------

    std::string ToolRegistry::BuildToolDescriptions() const
    {
        std::ostringstream oss;
        oss << "Available tools:\n\n";
        for (auto const& tool : m_ToolDefs)
        {
            oss << "- " << tool.name;
            if (tool.requiresApproval)
                oss << " [REQUIRES APPROVAL]";
            oss << "\n  " << tool.description << "\n  " << tool.argsDescription << "\n\n";
        }
        return oss.str();
    }

    // -----------------------------------------------------------------
    // Execute — dispatch tool call
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::Execute(ToolCall const& call)
    {
        auto it = m_ToolFns.find(call.name);
        if (it == m_ToolFns.end())
        {
            return {call.name, false, "Unknown tool: " + call.name};
        }

        try
        {
            return it->second(call.args);
        }
        catch (std::exception const& e)
        {
            return {call.name, false, std::string("Tool execution error: ") + e.what()};
        }
    }

    // -----------------------------------------------------------------
    // ParseToolCalls — extract <tool_call> blocks from AI response
    // -----------------------------------------------------------------

    // Forward-declare the anonymous-namespace helper so ParseToolCalls can call it.
    namespace
    {
        bool ParseToolCallJson(std::string const& json, ToolCall& out);
    }

    std::vector<ToolCall> ToolRegistry::ParseToolCalls(std::string const& responseText, std::string& outCleanText)
    {
        std::vector<ToolCall> calls;
        outCleanText.clear();
        outCleanText.reserve(responseText.size());

        static std::string const openTag = "<tool_call>";
        static std::string const closeTag = "</tool_call>";

        size_t pos = 0;
        while (pos < responseText.size())
        {
            size_t openPos = responseText.find(openTag, pos);
            if (openPos == std::string::npos)
            {
                outCleanText.append(responseText, pos, std::string::npos);
                break;
            }

            // Append text before the tool_call tag.
            outCleanText.append(responseText, pos, openPos - pos);

            size_t jsonStart = openPos + openTag.size();
            size_t closePos = responseText.find(closeTag, jsonStart);
            if (closePos == std::string::npos)
            {
                // Malformed — no closing tag. Include remaining text as-is.
                outCleanText.append(responseText, openPos, std::string::npos);
                break;
            }

            std::string jsonStr = responseText.substr(jsonStart, closePos - jsonStart);

            // Trim whitespace.
            while (!jsonStr.empty() && (jsonStr.front() == ' ' || jsonStr.front() == '\n' || jsonStr.front() == '\r'))
                jsonStr.erase(jsonStr.begin());
            while (!jsonStr.empty() && (jsonStr.back() == ' ' || jsonStr.back() == '\n' || jsonStr.back() == '\r'))
                jsonStr.pop_back();

            // Parse JSON manually (simple object with "name" and "args").
            ToolCall call;
            if (ParseToolCallJson(jsonStr, call))
            {
                calls.push_back(std::move(call));
            }
            else
            {
                // Failed to parse — include the raw block in output as a note.
                outCleanText += "[Failed to parse tool call: " + jsonStr + "]";
            }

            pos = closePos + closeTag.size();
        }

        return calls;
    }

    // -----------------------------------------------------------------
    // Simple JSON parser for tool call objects
    // -----------------------------------------------------------------

    namespace
    {
        // Skip whitespace.
        void SkipWs(std::string const& s, size_t& i)
        {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
                ++i;
        }

        // Parse a JSON string value (expects opening quote at s[i]).
        bool ParseJsonString(std::string const& s, size_t& i, std::string& out)
        {
            if (i >= s.size() || s[i] != '"')
                return false;
            ++i; // skip opening quote
            out.clear();
            while (i < s.size())
            {
                if (s[i] == '\\' && i + 1 < s.size())
                {
                    char c = s[i + 1];
                    if (c == '"')
                        out += '"';
                    else if (c == '\\')
                        out += '\\';
                    else if (c == 'n')
                        out += '\n';
                    else if (c == 't')
                        out += '\t';
                    else if (c == 'r')
                        out += '\r';
                    else
                    {
                        out += '\\';
                        out += c;
                    }
                    i += 2;
                }
                else if (s[i] == '"')
                {
                    ++i; // skip closing quote
                    return true;
                }
                else
                {
                    out += s[i];
                    ++i;
                }
            }
            return false; // unterminated string
        }

        // Parse a JSON value as string (handles numbers, booleans, etc. by converting to string).
        bool ParseJsonValue(std::string const& s, size_t& i, std::string& out)
        {
            SkipWs(s, i);
            if (i >= s.size())
                return false;

            if (s[i] == '"')
                return ParseJsonString(s, i, out);

            // Number, boolean, null — read until delimiter.
            size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n')
                ++i;
            out = s.substr(start, i - start);
            return !out.empty();
        }

        bool ParseToolCallJson(std::string const& json, ToolCall& out)
        {
            size_t i = 0;
            SkipWs(json, i);
            if (i >= json.size() || json[i] != '{')
                return false;
            ++i; // skip {

            while (i < json.size())
            {
                SkipWs(json, i);
                if (i < json.size() && json[i] == '}')
                    break;

                // Parse key.
                std::string key;
                if (!ParseJsonString(json, i, key))
                    return false;

                SkipWs(json, i);
                if (i >= json.size() || json[i] != ':')
                    return false;
                ++i; // skip :
                SkipWs(json, i);

                if (key == "name")
                {
                    if (!ParseJsonString(json, i, out.name))
                        return false;
                }
                else if (key == "args")
                {
                    // Parse args object.
                    if (i >= json.size() || json[i] != '{')
                        return false;
                    ++i; // skip {

                    while (i < json.size())
                    {
                        SkipWs(json, i);
                        if (i < json.size() && json[i] == '}')
                        {
                            ++i;
                            break;
                        }

                        std::string argKey;
                        if (!ParseJsonString(json, i, argKey))
                            return false;

                        SkipWs(json, i);
                        if (i >= json.size() || json[i] != ':')
                            return false;
                        ++i;
                        SkipWs(json, i);

                        std::string argVal;
                        if (!ParseJsonValue(json, i, argVal))
                            return false;

                        out.args[argKey] = argVal;

                        SkipWs(json, i);
                        if (i < json.size() && json[i] == ',')
                            ++i;
                    }
                }
                else
                {
                    // Skip unknown value.
                    std::string dummy;
                    if (!ParseJsonValue(json, i, dummy))
                        return false;
                }

                SkipWs(json, i);
                if (i < json.size() && json[i] == ',')
                    ++i;
            }

            return !out.name.empty();
        }
    } // namespace

    // -----------------------------------------------------------------
    // Security: deny-list for read_file
    // -----------------------------------------------------------------

    bool ToolRegistry::IsPathDenied(std::string const& path)
    {
        // Normalize the path.
        fs::path p = fs::path(path).lexically_normal();
        std::string normalized = p.string();

        // Block sensitive files.
        static std::vector<std::string> const denyList = {"config.json", "keys.json", "keys.json.enc", ".env"};

        for (auto const& denied : denyList)
        {
            if (normalized == denied || normalized.ends_with("/" + denied))
                return true;
        }

        // Block sensitive extensions.
        std::string ext = p.extension().string();
        if (ext == ".pem" || ext == ".key")
            return true;

        // Block reading assistant's own memory (prevents AI from reading its own prompt).
        if (normalized.starts_with("assistant/"))
            return true;

        return false;
    }

    std::string ToolRegistry::TruncateOutput(std::string const& output, size_t maxLen)
    {
        if (output.size() <= maxLen)
            return output;
        return output.substr(0, maxLen) + "\n... [truncated, " + std::to_string(output.size()) + " bytes total]";
    }

    // -----------------------------------------------------------------
    // Helper: WorkflowRunState to string
    // -----------------------------------------------------------------

    namespace
    {
        std::string RunStateToString(WorkflowRunState state)
        {
            switch (state)
            {
                case WorkflowRunState::Pending:
                    return "pending";
                case WorkflowRunState::Running:
                    return "running";
                case WorkflowRunState::Paused:
                    return "paused";
                case WorkflowRunState::Stopping:
                    return "stopping";
                case WorkflowRunState::Succeeded:
                    return "succeeded";
                case WorkflowRunState::Failed:
                    return "failed";
                case WorkflowRunState::Cancelled:
                    return "cancelled";
                case WorkflowRunState::Stopped:
                    return "stopped";
                default:
                    return "unknown";
            }
        }

        std::string TaskStateToString(TaskInstanceStateKind state)
        {
            switch (state)
            {
                case TaskInstanceStateKind::Pending:
                    return "pending";
                case TaskInstanceStateKind::Ready:
                    return "ready";
                case TaskInstanceStateKind::Running:
                    return "running";
                case TaskInstanceStateKind::Succeeded:
                    return "succeeded";
                case TaskInstanceStateKind::Failed:
                    return "failed";
                case TaskInstanceStateKind::Skipped:
                    return "skipped";
                case TaskInstanceStateKind::WaitingExternal:
                    return "waiting_external";
                default:
                    return "unknown";
            }
        }
    } // namespace

    // =================================================================
    // Tool implementations
    // =================================================================

    // -----------------------------------------------------------------
    // get_system_status
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecGetSystemStatus(std::unordered_map<std::string, std::string> const& /*args*/)
    {
        JarvisAgent* app = App::g_App;
        if (!app)
            return {"get_system_status", false, "Application not available"};

        std::ostringstream oss;
        oss << "JarvisAgent Status:\n";
        oss << "  Version: " << JARVIS_AGENT_VERSION << "\n";

        // Uptime
        auto now = std::chrono::system_clock::now();
        auto uptime = now - app->GetStartupTime();
        auto hours = std::chrono::duration_cast<std::chrono::hours>(uptime).count();
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(uptime).count() % 60;
        oss << "  Uptime: " << hours << "h " << minutes << "m\n";

        // Session managers
        oss << "  Session managers: " << app->GetSessionManagerCount() << "\n";

        // Workflow registry
        if (m_WorkflowRegistry)
            oss << "  Registered workflows: " << m_WorkflowRegistry->GetWorkflowIds().size() << "\n";

        // Active runs
        if (m_RuntimeManager)
        {
            auto activeRuns = m_RuntimeManager->GetActiveRunsSnapshot();
            oss << "  Active runs: " << activeRuns.size() << "\n";

            uint64_t completed = 0, failed = 0;
            m_RuntimeManager->GetRunCounters(completed, failed);
            oss << "  Completed runs: " << completed << "\n";
            oss << "  Failed runs: " << failed << "\n";
        }

        // Python engine
        PythonEngine* py = app->GetPythonEngine();
        oss << "  Python engine: " << (py ? "ready" : "not available") << "\n";

        return {"get_system_status", true, oss.str()};
    }

    // -----------------------------------------------------------------
    // list_workflows
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecListWorkflows(std::unordered_map<std::string, std::string> const& /*args*/)
    {
        if (!m_WorkflowRegistry)
            return {"list_workflows", false, "Workflow registry not available"};

        auto ids = m_WorkflowRegistry->GetWorkflowIds();
        if (ids.empty())
            return {"list_workflows", true, "No workflows registered."};

        std::ostringstream oss;
        oss << "Registered workflows (" << ids.size() << "):\n\n";
        for (auto const& id : ids)
        {
            auto wf = m_WorkflowRegistry->GetWorkflow(id);
            if (wf.has_value())
            {
                oss << "  " << id;
                if (!wf->m_Label.empty())
                    oss << "  (" << wf->m_Label << ")";
                oss << "\n";
            }
        }
        return {"list_workflows", true, oss.str()};
    }

    // -----------------------------------------------------------------
    // get_run_status
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecGetRunStatus(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_RuntimeManager)
            return {"get_run_status", false, "Runtime manager not available"};

        auto it = args.find("run_id");
        if (it == args.end() || it->second.empty())
            return {"get_run_status", false, "Missing required argument: run_id"};

        WorkflowRun run;
        if (!m_RuntimeManager->TryGetRunById(it->second, run))
            return {"get_run_status", false, "Run not found: " + it->second};

        std::ostringstream oss;
        oss << "Run: " << run.m_RunId << "\n";
        oss << "  Workflow: " << run.m_WorkflowId << "\n";
        oss << "  State: " << RunStateToString(run.m_State) << "\n";
        if (!run.m_StartedAtIso8601.empty())
            oss << "  Started: " << run.m_StartedAtIso8601 << "\n";
        if (!run.m_CompletedAtIso8601.empty())
            oss << "  Completed: " << run.m_CompletedAtIso8601 << "\n";

        if (!run.m_TaskStates.empty())
        {
            oss << "  Tasks:\n";
            for (auto const& [taskId, taskState] : run.m_TaskStates)
            {
                oss << "    " << taskId << ": " << TaskStateToString(taskState.m_State);
                if (!taskState.m_LastErrorMessage.empty())
                    oss << " — " << taskState.m_LastErrorMessage;
                oss << "\n";
            }
        }

        return {"get_run_status", true, oss.str()};
    }

    // -----------------------------------------------------------------
    // list_recent_runs
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecListRecentRuns(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_RuntimeManager)
            return {"list_recent_runs", false, "Runtime manager not available"};

        std::ostringstream oss;

        // Active runs
        auto activeRuns = m_RuntimeManager->GetActiveRunsSnapshot();
        if (!activeRuns.empty())
        {
            oss << "Active runs (" << activeRuns.size() << "):\n";
            for (auto const& run : activeRuns)
            {
                oss << "  " << run.m_RunId << "  " << run.m_WorkflowId << "  [" << RunStateToString(run.m_State) << "]\n";
            }
            oss << "\n";
        }

        // Last completed runs
        auto lastRuns = m_RuntimeManager->GetLastRunsSnapshot();
        if (!lastRuns.empty())
        {
            oss << "Last completed runs (" << lastRuns.size() << "):\n";
            for (auto const& [wfId, run] : lastRuns)
            {
                oss << "  " << run.m_RunId << "  " << run.m_WorkflowId << "  [" << RunStateToString(run.m_State) << "]";
                if (!run.m_CompletedAtIso8601.empty())
                    oss << "  " << run.m_CompletedAtIso8601;
                oss << "\n";
            }
        }

        if (activeRuns.empty() && lastRuns.empty())
            oss << "No recent runs.";

        return {"list_recent_runs", true, oss.str()};
    }

    // -----------------------------------------------------------------
    // get_task_output
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecGetTaskOutput(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_RuntimeManager)
            return {"get_task_output", false, "Runtime manager not available"};

        auto runIt = args.find("run_id");
        auto taskIt = args.find("task_id");
        if (runIt == args.end() || runIt->second.empty())
            return {"get_task_output", false, "Missing required argument: run_id"};
        if (taskIt == args.end() || taskIt->second.empty())
            return {"get_task_output", false, "Missing required argument: task_id"};

        WorkflowRun run;
        if (!m_RuntimeManager->TryGetRunById(runIt->second, run))
            return {"get_task_output", false, "Run not found: " + runIt->second};

        auto stateIt = run.m_TaskStates.find(taskIt->second);
        if (stateIt == run.m_TaskStates.end())
            return {"get_task_output", false, "Task not found in run: " + taskIt->second};

        std::ostringstream oss;
        auto const& ts = stateIt->second;
        oss << "Task: " << taskIt->second << "  State: " << TaskStateToString(ts.m_State) << "\n";

        if (!ts.m_LastErrorMessage.empty())
            oss << "Error: " << ts.m_LastErrorMessage << "\n";

        // Show captured stdout/stderr from the task instance state.
        if (!ts.m_CapturedStdout.empty())
        {
            oss << "\n--- stdout ---\n";
            oss << TruncateOutput(ts.m_CapturedStdout);
        }
        if (!ts.m_CapturedStderr.empty())
        {
            oss << "\n--- stderr ---\n";
            oss << TruncateOutput(ts.m_CapturedStderr);
        }

        return {"get_task_output", true, oss.str()};
    }

    // -----------------------------------------------------------------
    // run_workflow
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecRunWorkflow(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_RuntimeManager || !m_WorkflowRegistry)
            return {"run_workflow", false, "Runtime manager or registry not available"};

        auto it = args.find("workflow_id");
        if (it == args.end() || it->second.empty())
            return {"run_workflow", false, "Missing required argument: workflow_id"};

        std::string const& workflowId = it->second;

        // Verify workflow exists.
        auto wf = m_WorkflowRegistry->GetWorkflow(workflowId);
        if (!wf.has_value())
            return {"run_workflow", false, "Workflow not found: " + workflowId};

        std::string runId = m_RuntimeManager->EnqueueWorkflowRunAndGetRunId(workflowId);
        return {"run_workflow", true, "Workflow run started: " + workflowId + " (run ID: " + runId + ")"};
    }

    // -----------------------------------------------------------------
    // read_file
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecReadFile(std::unordered_map<std::string, std::string> const& args)
    {
        auto pathIt = args.find("path");
        if (pathIt == args.end() || pathIt->second.empty())
            return {"read_file", false, "Missing required argument: path"};

        std::string const& path = pathIt->second;

        if (IsPathDenied(path))
            return {"read_file", false, "Access denied: " + path + " (sensitive file)"};

        // Resolve relative to CWD.
        fs::path filePath = fs::path(path).lexically_normal();
        std::error_code ec;
        if (!fs::exists(filePath, ec))
            return {"read_file", false, "File not found: " + path};
        if (!fs::is_regular_file(filePath, ec))
            return {"read_file", false, "Not a regular file: " + path};

        // Check file size (limit to 100 KB).
        auto fileSize = fs::file_size(filePath, ec);
        if (fileSize > 100 * 1024)
            return {"read_file", false,
                    "File too large: " + std::to_string(fileSize) +
                        " bytes (limit 100 KB). Use start/end args to read a portion."};

        std::ifstream ifs(filePath);
        if (!ifs.is_open())
            return {"read_file", false, "Cannot open file: " + path};

        int startLine = 1, endLine = INT_MAX;
        if (auto startIt = args.find("start"); startIt != args.end())
        {
            try
            {
                startLine = std::max(1, std::stoi(startIt->second));
            }
            catch (...)
            {
            }
        }
        if (auto endIt = args.find("end"); endIt != args.end())
        {
            try
            {
                endLine = std::max(startLine, std::stoi(endIt->second));
            }
            catch (...)
            {
            }
        }

        std::ostringstream oss;
        std::string line;
        int lineNum = 0;
        int linesIncluded = 0;
        while (std::getline(ifs, line))
        {
            ++lineNum;
            if (lineNum < startLine)
                continue;
            if (lineNum > endLine)
                break;

            oss << lineNum << "\t" << line << "\n";
            ++linesIncluded;

            // Safety: cap at ~200 lines per read.
            if (linesIncluded >= 200)
            {
                oss << "... [truncated at 200 lines]\n";
                break;
            }
        }

        if (linesIncluded == 0)
            return {"read_file", true, "File is empty or line range is out of bounds."};

        return {"read_file", true, TruncateOutput(oss.str(), 8192)}; // Allow larger output for file reads
    }

    // -----------------------------------------------------------------
    // search_files
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecSearchFiles(std::unordered_map<std::string, std::string> const& args)
    {
        auto queryIt = args.find("query");
        if (queryIt == args.end() || queryIt->second.empty())
            return {"search_files", false, "Missing required argument: query"};

        // Build rg command. Falls back to grep if rg not available.
        std::string cmd = "rg --no-heading --line-number --max-count 30 --max-columns 200 --color never";

        // Add glob filter if provided.
        if (auto globIt = args.find("glob"); globIt != args.end() && !globIt->second.empty())
        {
            cmd += " --glob '" + globIt->second + "'";
        }

        // Exclude common noise directories.
        cmd += " --glob '!node_modules' --glob '!bin' --glob '!bin-int' --glob '!vendor' --glob '!.git'";

        // Add the query (escaped in single quotes).
        std::string query = queryIt->second;
        // Simple shell escaping: replace single quotes.
        for (auto& c : query)
        {
            if (c == '\'')
                c = '"';
        }
        cmd += " -- '" + query + "' . 2>/dev/null || grep -rn --max-count=30 '" + query +
               "' --include='*.cpp' --include='*.h' --include='*.ts' --include='*.tsx' --include='*.py' --include='*.md' "
               "--include='*.jcwf' . 2>/dev/null";

        // Execute via popen.
        std::array<char, 256> buffer;
        std::string result;

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return {"search_files", false, "Failed to execute search command"};

        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            result += buffer.data();
            if (result.size() > kMaxToolOutputSize)
            {
                result += "\n... [output truncated]";
                break;
            }
        }
        pclose(pipe);

        if (result.empty())
            return {"search_files", true, "No matches found for: " + queryIt->second};

        return {"search_files", true, TruncateOutput(result)};
    }

    // -----------------------------------------------------------------
    // list_files
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecListFiles(std::unordered_map<std::string, std::string> const& args)
    {
        std::string path = ".";
        int maxDepth = 2;

        if (auto pathIt = args.find("path"); pathIt != args.end() && !pathIt->second.empty())
            path = pathIt->second;
        if (auto depthIt = args.find("depth"); depthIt != args.end())
        {
            try
            {
                maxDepth = std::clamp(std::stoi(depthIt->second), 1, 5);
            }
            catch (...)
            {
            }
        }

        fs::path dirPath = fs::path(path).lexically_normal();
        std::error_code ec;
        if (!fs::exists(dirPath, ec) || !fs::is_directory(dirPath, ec))
            return {"list_files", false, "Not a directory: " + path};

        // Use fd or find for listing.
        std::string cmd = "find '" + dirPath.string() + "' -maxdepth " + std::to_string(maxDepth) +
                          " -not -path '*/node_modules/*' -not -path '*/.git/*' -not -path '*/bin-int/*'"
                          " -not -path '*/vendor/*' -not -path '*/bin/*'"
                          " -printf '%y %p\\n' 2>/dev/null | head -100";

        std::array<char, 256> buffer;
        std::string result;

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return {"list_files", false, "Failed to list directory"};

        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            result += buffer.data();
            if (result.size() > kMaxToolOutputSize)
                break;
        }
        pclose(pipe);

        if (result.empty())
            return {"list_files", true, "Directory is empty: " + path};

        return {"list_files", true, TruncateOutput(result)};
    }

    // -----------------------------------------------------------------
    // get_log_tail
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecGetLogTail(std::unordered_map<std::string, std::string> const& args)
    {
        int lines = 50;
        if (auto linesIt = args.find("lines"); linesIt != args.end())
        {
            try
            {
                lines = std::clamp(std::stoi(linesIt->second), 1, 200);
            }
            catch (...)
            {
            }
        }

        fs::path logPath = "log/log.txt";
        std::error_code ec;
        if (!fs::exists(logPath, ec))
            return {"get_log_tail", false, "Log file not found: " + logPath.string()};

        std::string cmd = "tail -" + std::to_string(lines) + " '" + logPath.string() + "' 2>/dev/null";

        std::array<char, 256> buffer;
        std::string result;

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return {"get_log_tail", false, "Failed to read log"};

        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            result += buffer.data();
            if (result.size() > kMaxToolOutputSize)
                break;
        }
        pclose(pipe);

        return {"get_log_tail", true, TruncateOutput(result)};
    }

    // -----------------------------------------------------------------
    // save_memory
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecSaveMemory(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_MemoryStore)
            return {"save_memory", false, "Memory store not available."};

        auto keyIt = args.find("key");
        auto valueIt = args.find("value");

        if (keyIt == args.end() || keyIt->second.empty())
            return {"save_memory", false, "Missing required arg: key"};
        if (valueIt == args.end() || valueIt->second.empty())
            return {"save_memory", false, "Missing required arg: value"};

        std::vector<std::string> tags;
        if (auto tagsIt = args.find("tags"); tagsIt != args.end() && !tagsIt->second.empty())
        {
            std::istringstream iss(tagsIt->second);
            std::string tag;
            while (std::getline(iss, tag, ','))
            {
                // Trim whitespace.
                size_t start = tag.find_first_not_of(" \t");
                size_t end = tag.find_last_not_of(" \t");
                if (start != std::string::npos)
                    tags.push_back(tag.substr(start, end - start + 1));
            }
        }

        std::string id = m_MemoryStore->Save(keyIt->second, valueIt->second, tags);
        return {"save_memory", true, "Saved memory: \"" + keyIt->second + "\" (id=" + id + ")"};
    }

    // -----------------------------------------------------------------
    // recall_memory
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecRecallMemory(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_MemoryStore)
            return {"recall_memory", false, "Memory store not available."};

        auto queryIt = args.find("query");
        std::string query = (queryIt != args.end()) ? queryIt->second : "";

        auto results = m_MemoryStore->Recall(query);

        if (results.empty())
            return {"recall_memory", true, "No memories found matching: " + query};

        std::ostringstream oss;
        oss << "Found " << results.size() << " memor" << (results.size() == 1 ? "y" : "ies") << ":\n\n";
        for (auto const& entry : results)
        {
            oss << "- [" << entry.key << "]: " << entry.value;
            if (!entry.tags.empty())
            {
                oss << " (tags: ";
                for (size_t t = 0; t < entry.tags.size(); ++t)
                {
                    if (t > 0)
                        oss << ", ";
                    oss << entry.tags[t];
                }
                oss << ")";
            }
            oss << "\n";
        }
        return {"recall_memory", true, TruncateOutput(oss.str())};
    }

    // -----------------------------------------------------------------
    // list_memories
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecListMemories(std::unordered_map<std::string, std::string> const& /*args*/)
    {
        if (!m_MemoryStore)
            return {"list_memories", false, "Memory store not available."};

        auto entries = m_MemoryStore->ListAll();

        if (entries.empty())
            return {"list_memories", true, "No memories stored."};

        std::ostringstream oss;
        oss << entries.size() << " memor" << (entries.size() == 1 ? "y" : "ies") << " stored:\n\n";
        for (auto const& entry : entries)
        {
            oss << "- " << entry.key << "  (" << entry.createdAt << ")\n";
        }
        return {"list_memories", true, oss.str()};
    }

    // -----------------------------------------------------------------
    // delete_memory
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecDeleteMemory(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_MemoryStore)
            return {"delete_memory", false, "Memory store not available."};

        auto keyIt = args.find("key");
        if (keyIt == args.end() || keyIt->second.empty())
            return {"delete_memory", false, "Missing required arg: key"};

        bool deleted = m_MemoryStore->Delete(keyIt->second);
        if (deleted)
            return {"delete_memory", true, "Deleted memory: \"" + keyIt->second + "\""};
        else
            return {"delete_memory", false, "Memory not found: \"" + keyIt->second + "\""};
    }

} // namespace AIAssistant
