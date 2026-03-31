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
#include "assistant/workspaceIndexer.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "python/pythonEngine.h"
#include "web/aiJcwfService.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowRuntimeManager.h"
#include "workflow/workflowTypes.h"
#include "workflow/workflowValidator.h"
#include "workflow/shellTaskExecutor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
// POSIX headers for run_shell (fork/exec/waitpid/poll/pipe/kill)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#else
// Windows headers for run_shell (CreateProcess/WaitForSingleObject/TerminateProcess).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// MSVC equivalents for popen/pclose (still used by search_files / list_files / get_log_tail).
#define popen _popen
#define pclose _pclose
#endif

namespace fs = std::filesystem;

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

#if defined(_WIN32)
    // Single-quote quoting for PowerShell: wrap in single quotes, double inner single quotes.
    std::string QuoteForPowerShellArg(std::string const& value)
    {
        std::string quoted;
        quoted.reserve(value.size() + 2);
        quoted.push_back('\'');
        for (char c : value)
        {
            if (c == '\'')
                quoted.append("''");
            else
                quoted.push_back(c);
        }
        quoted.push_back('\'');
        return quoted;
    }
#endif
} // namespace

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

        // --- Indexing / summary tools ---
        m_ToolDefs.push_back({"get_file_summary",
                              "Returns an AI-generated summary of a source file. If a cached summary exists, it is "
                              "returned immediately. Otherwise the file is read and summarized via an AI call (may take "
                              "a few seconds). Use this to understand unfamiliar files.",
                              "Args: path (string, required, relative to workspace root)", false});
        m_ToolFns["get_file_summary"] = [this](auto const& a) { return ExecGetFileSummary(a); };

        m_ToolDefs.push_back({"get_folder_summary",
                              "Returns summaries for all indexed files in a directory. Only files that already have "
                              "cached summaries are included. Use get_file_summary on individual files to generate "
                              "missing summaries.",
                              "Args: path (string, required, relative to workspace root)", false});
        m_ToolFns["get_folder_summary"] = [this](auto const& a) { return ExecGetFolderSummary(a); };

        // --- L3 mutating tools (all require approval) ---
        m_ToolDefs.push_back({"run_shell",
                              "Executes a shell command and returns stdout/stderr. The user will be shown the exact "
                              "command and must approve it before execution. Timeout: 30 seconds.",
                              "Args: command (string, required), cwd (string, optional, working directory)", true});
        m_ToolFns["run_shell"] = [this](auto const& a) { return ExecRunShell(a); };

        m_ToolDefs.push_back({"write_file",
                              "Writes content to a file. Creates parent directories if needed. Overwrites existing "
                              "files. The user must approve the operation.",
                              "Args: path (string, required, relative to project root), content (string, required)", true});
        m_ToolFns["write_file"] = [this](auto const& a) { return ExecWriteFile(a); };

        m_ToolDefs.push_back({"edit_file",
                              "Replaces a specific text span in a file with new content. The old_text must match "
                              "exactly once in the file. The user must approve the operation.",
                              "Args: path (string, required), old_text (string, required), new_text (string, required)",
                              true});
        m_ToolFns["edit_file"] = [this](auto const& a) { return ExecEditFile(a); };

        // --- L3 runtime control tools ---
        m_ToolDefs.push_back({"workflow_pause", "Pauses a currently running workflow run. The run can be resumed later.",
                              "Args: run_id (string, required)", true});
        m_ToolFns["workflow_pause"] = [this](auto const& a) { return ExecWorkflowPause(a); };

        m_ToolDefs.push_back({"workflow_resume", "Resumes a paused workflow run.", "Args: run_id (string, required)", true});
        m_ToolFns["workflow_resume"] = [this](auto const& a) { return ExecWorkflowResume(a); };

        m_ToolDefs.push_back({"workflow_stop", "Stops/cancels a running or paused workflow run. This cannot be undone.",
                              "Args: run_id (string, required)", true});
        m_ToolFns["workflow_stop"] = [this](auto const& a) { return ExecWorkflowStop(a); };

        m_ToolDefs.push_back({"get_dashboard_status",
                              "Returns a comprehensive system status report: registered workflows, active/completed/"
                              "failed runs, Python engine state, uptime, and memory usage.",
                              "No arguments.", false});
        m_ToolFns["get_dashboard_status"] = [this](auto const& a) { return ExecGetDashboardStatus(a); };

        // --- L3 JCWF development tools ---
        m_ToolDefs.push_back({"jcwf_read", "Reads a JCWF workflow file and returns the full JSON content.",
                              "Args: workflow_id (string, required)", false});
        m_ToolFns["jcwf_read"] = [this](auto const& a) { return ExecJcwfRead(a); };

        m_ToolDefs.push_back({"jcwf_explain",
                              "Returns a human-readable explanation of a workflow: tasks, edges, data flow, "
                              "trigger type, and script details.",
                              "Args: workflow_id (string, required)", false});
        m_ToolFns["jcwf_explain"] = [this](auto const& a) { return ExecJcwfExplain(a); };

        m_ToolDefs.push_back({"jcwf_validate",
                              "Validates a JCWF workflow against the JC Workflow Spec. Returns errors and warnings.",
                              "Args: workflow_id (string, required)", false});
        m_ToolFns["jcwf_validate"] = [this](auto const& a) { return ExecJcwfValidate(a); };

        m_ToolDefs.push_back({"jcwf_read_plan",
                              "Reads the development plan (.plan.md) for a workflow. Returns the plan content "
                              "or a message if no plan exists.",
                              "Args: workflow_id (string, required)", false});
        m_ToolFns["jcwf_read_plan"] = [this](auto const& a) { return ExecJcwfReadPlan(a); };

        m_ToolDefs.push_back({"jcwf_write_plan",
                              "Creates or updates the development plan (.plan.md) for a workflow. The plan "
                              "guides JCWF generation. Requires approval.",
                              "Args: workflow_id (string, required), content (string, required)", true});
        m_ToolFns["jcwf_write_plan"] = [this](auto const& a) { return ExecJcwfWritePlan(a); };

        m_ToolDefs.push_back({"jcwf_generate",
                              "Generates or regenerates a JCWF workflow from its development plan using AI. "
                              "The plan must exist (use jcwf_write_plan first). Requires approval.",
                              "Args: workflow_id (string, required)", true});
        m_ToolFns["jcwf_generate"] = [this](auto const& a) { return ExecJcwfGenerate(a); };

        m_ToolDefs.push_back({"jcwf_fix_task",
                              "Fixes a specific task in a JCWF workflow based on instructions. Reads the "
                              "current task definition, applies the fix via AI, and updates the workflow file. "
                              "Requires approval.",
                              "Args: workflow_id (string, required), task_id (string, required), "
                              "instructions (string, required)",
                              true});
        m_ToolFns["jcwf_fix_task"] = [this](auto const& a) { return ExecJcwfFixTask(a); };

        m_ToolDefs.push_back({"jcwf_write_script",
                              "Writes a Python or shell script file to the scripts/ directory. Validates "
                              "that shell scripts have proper shebang and set -euo pipefail. Requires approval.",
                              "Args: path (string, required, e.g. \"scripts/myscript.sh\"), "
                              "content (string, required), type (string, required, \"shell\" or \"python\")",
                              true});
        m_ToolFns["jcwf_write_script"] = [this](auto const& a) { return ExecJcwfWriteScript(a); };
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

            std::string jsonStr;
            if (closePos == std::string::npos)
            {
                // No closing tag — attempt to extract a complete JSON object by
                // counting braces from the first '{'.
                size_t braceStart = responseText.find('{', jsonStart);
                if (braceStart == std::string::npos)
                {
                    // Nothing usable — include remaining text as-is and stop.
                    outCleanText.append(responseText, openPos, std::string::npos);
                    break;
                }
                int depth = 0;
                size_t braceEnd = std::string::npos;
                for (size_t i = braceStart; i < responseText.size(); ++i)
                {
                    if (responseText[i] == '{')
                        ++depth;
                    else if (responseText[i] == '}')
                    {
                        --depth;
                        if (depth == 0)
                        {
                            braceEnd = i;
                            break;
                        }
                    }
                }
                if (braceEnd == std::string::npos)
                {
                    // Incomplete JSON — include remaining text as-is and stop.
                    outCleanText.append(responseText, openPos, std::string::npos);
                    break;
                }
                jsonStr = responseText.substr(braceStart, braceEnd - braceStart + 1);
                // Consume the rest of the response (the truncated block was the last thing).
                pos = braceEnd + 1;
            }
            else
            {
                jsonStr = responseText.substr(jsonStart, closePos - jsonStart);
                pos = closePos + closeTag.size();
            }

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

        std::string TaskTypeToString(TaskType type)
        {
            switch (type)
            {
                case TaskType::Python:
                    return "python";
                case TaskType::Shell:
                    return "shell";
                case TaskType::AiCall:
                    return "ai_call";
                case TaskType::Internal:
                    return "internal";
                default:
                    return "unknown";
            }
        }

        std::string TriggerTypeToString(WorkflowTriggerType type)
        {
            switch (type)
            {
                case WorkflowTriggerType::Auto:
                    return "auto";
                case WorkflowTriggerType::Cron:
                    return "cron";
                case WorkflowTriggerType::FileWatch:
                    return "file_watch";
                case WorkflowTriggerType::Structure:
                    return "structure";
                case WorkflowTriggerType::Manual:
                    return "manual";
                case WorkflowTriggerType::Webhook:
                    return "webhook";
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
        std::string normalized = filePath.string();

        // Reject path traversal and absolute paths.
        if (normalized.find("..") != std::string::npos)
            return {"read_file", false, "Path traversal not allowed: " + path};
        if (filePath.is_absolute())
            return {"read_file", false, "Absolute paths not allowed. Use paths relative to project root."};

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

        // Execute via popen (routed through bash on Windows).
        std::string const shellCmd = WrapForBash(cmd);
        std::array<char, 256> buffer;
        std::string result;

        FILE* pipe = popen(shellCmd.c_str(), "r");
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

        std::string const shellCmd = WrapForBash(cmd);
        std::array<char, 256> buffer;
        std::string result;

        FILE* pipe = popen(shellCmd.c_str(), "r");
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

        std::string const shellCmd = WrapForBash(cmd);
        std::array<char, 256> buffer;
        std::string result;

        FILE* pipe = popen(shellCmd.c_str(), "r");
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

    // -----------------------------------------------------------------
    // get_file_summary
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecGetFileSummary(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_WorkspaceIndexer)
            return {"get_file_summary", false, "Workspace indexer not available."};

        auto pathIt = args.find("path");
        if (pathIt == args.end() || pathIt->second.empty())
            return {"get_file_summary", false, "Missing required arg: path"};

        std::string const& filePath = pathIt->second;

        // Check for cached summary first.
        std::string cached = m_WorkspaceIndexer->GetFileSummary(filePath);
        if (!cached.empty())
        {
            return {"get_file_summary", true, "Summary of " + filePath + ":\n\n" + cached};
        }

        // No cached summary — generate one via AI call.
        if (!m_AiCallFn)
            return {"get_file_summary", false, "AI call function not configured. Cannot generate summary."};

        // Read file content.
        std::string content = WorkspaceIndexer::ReadFileContent(filePath, 32768);
        if (content.empty())
            return {"get_file_summary", false, "Cannot read file: " + filePath};

        // Build a summarization prompt.
        std::string systemPrompt =
            "You are a code summarizer. Given a source file, produce a concise summary (2-5 sentences) "
            "describing: what the file does, its key classes/functions/types, and its role in the project. "
            "Output ONLY the summary text, no markdown fences, no preamble.";

        std::string userPrompt = "File: " + filePath + "\n\n" + content;

        std::string response;
        std::string error;
        bool ok = m_AiCallFn(systemPrompt, userPrompt, response, error);

        if (!ok || response.empty())
        {
            return {"get_file_summary", false,
                    "Failed to generate summary for " + filePath + ": " + (error.empty() ? "empty response" : error)};
        }

        // Trim whitespace from response.
        while (!response.empty() && (response.back() == '\n' || response.back() == ' '))
            response.pop_back();
        while (!response.empty() && (response.front() == '\n' || response.front() == ' '))
            response.erase(response.begin());

        // Cache the summary.
        m_WorkspaceIndexer->SetFileSummary(filePath, response);

        LOG_APP_INFO("[tools] Generated summary for '{}' ({} chars)", filePath, response.size());
        return {"get_file_summary", true, "Summary of " + filePath + ":\n\n" + response};
    }

    // -----------------------------------------------------------------
    // get_folder_summary
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecGetFolderSummary(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_WorkspaceIndexer)
            return {"get_folder_summary", false, "Workspace indexer not available."};

        auto pathIt = args.find("path");
        if (pathIt == args.end() || pathIt->second.empty())
            return {"get_folder_summary", false, "Missing required arg: path"};

        std::string const& folderPath = pathIt->second;
        auto entries = m_WorkspaceIndexer->GetAllEntries();

        // Normalize folder path: ensure it ends with '/'.
        std::string prefix = folderPath;
        if (!prefix.empty() && prefix.back() != '/')
            prefix += '/';

        std::ostringstream oss;
        int withSummary = 0;
        int withoutSummary = 0;

        for (auto const& entry : entries)
        {
            if (!entry.relativePath.starts_with(prefix))
                continue;

            if (!entry.summary.empty())
            {
                oss << "- " << entry.relativePath << ": " << entry.summary << "\n\n";
                ++withSummary;
            }
            else
            {
                ++withoutSummary;
            }
        }

        if (withSummary == 0 && withoutSummary == 0)
            return {"get_folder_summary", true, "No indexed files found in: " + folderPath};

        std::ostringstream result;
        result << "Folder: " << folderPath << " (" << withSummary << " summarized, " << withoutSummary << " pending)\n\n";
        result << oss.str();

        if (withoutSummary > 0)
        {
            result << "(" << withoutSummary << " files without summaries. Use get_file_summary to generate them.)\n";
        }

        return {"get_folder_summary", true, TruncateOutput(result.str(), 8192)};
    }

    // =================================================================
    // L3 mutating tools
    // =================================================================

    // -----------------------------------------------------------------
    // run_shell
    // -----------------------------------------------------------------

    namespace
    {
        // Blocklist patterns — reject obviously dangerous commands.
        bool IsCommandBlocked(std::string const& cmd)
        {
            static std::vector<std::string> const patterns = {
                "rm -rf /",  "rm -rf /*",      "mkfs",      "dd if=", ":(){ :|:& };:", "> /dev/sd",
                "> /dev/nv", "chmod -R 777 /", "chown -R ", "sudo ",  "su -",          "passwd",
                "shutdown",  "reboot",         "init ",     "halt",   "poweroff",
            };
            for (auto const& p : patterns)
            {
                if (cmd.find(p) != std::string::npos)
                    return true;
            }
            return false;
        }
    } // namespace

#if !defined(_WIN32)
    ToolResult ToolRegistry::ExecRunShell(std::unordered_map<std::string, std::string> const& args)
    {
        auto cmdIt = args.find("command");
        if (cmdIt == args.end() || cmdIt->second.empty())
            return {"run_shell", false, "Missing required argument: command"};

        std::string const& command = cmdIt->second;

        // Security: blocklist check.
        if (IsCommandBlocked(command))
            return {"run_shell", false, "Command blocked by security policy: " + command};

        // Determine working directory.
        std::string cwd = ".";
        if (auto cwdIt = args.find("cwd"); cwdIt != args.end() && !cwdIt->second.empty())
        {
            cwd = cwdIt->second;
        }

        fs::path cwdPath = fs::path(cwd).lexically_normal();
        std::error_code ec;
        if (!fs::exists(cwdPath, ec) || !fs::is_directory(cwdPath, ec))
            return {"run_shell", false, "Working directory does not exist: " + cwd};

        // Reject path traversal outside project.
        std::string cwdNorm = cwdPath.string();
        if (cwdNorm.find("..") != std::string::npos)
            return {"run_shell", false, "Path traversal not allowed in cwd"};

        // Build the full command: cd into cwd, then run the command.
        // Use a subshell to capture both stdout and stderr, with a timeout.
        std::string fullCmd = "cd '" + cwdPath.string() + "' && " + command;
        fullCmd += " 2>&1";

        // Create pipe for reading output.
        int pipeFds[2];
        if (pipe(pipeFds) != 0)
            return {"run_shell", false, "Failed to create pipe"};

        pid_t pid = fork();
        if (pid < 0)
        {
            close(pipeFds[0]);
            close(pipeFds[1]);
            return {"run_shell", false, "Failed to fork process"};
        }

        if (pid == 0)
        {
            // Child process — new process group for clean kill.
            setpgid(0, 0);
            close(pipeFds[0]); // close read end
            dup2(pipeFds[1], STDOUT_FILENO);
            dup2(pipeFds[1], STDERR_FILENO);
            close(pipeFds[1]);
            execl("/bin/sh", "sh", "-c", fullCmd.c_str(), nullptr);
            _exit(127); // exec failed
        }

        // Parent process.
        close(pipeFds[1]); // close write end

        // Read output with a 30-second timeout using poll.
        static constexpr int kTimeoutMs = 30000;
        std::string output;
        output.reserve(1024);

        auto startTime = std::chrono::steady_clock::now();
        bool timedOut = false;

        struct pollfd pfd;
        pfd.fd = pipeFds[0];
        pfd.events = POLLIN;

        while (true)
        {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);
            int remaining = kTimeoutMs - static_cast<int>(elapsed.count());
            if (remaining <= 0)
            {
                timedOut = true;
                break;
            }

            int ret = poll(&pfd, 1, std::min(remaining, 200));
            if (ret > 0 && (pfd.revents & POLLIN))
            {
                char buf[4096];
                ssize_t n = read(pipeFds[0], buf, sizeof(buf));
                if (n <= 0)
                    break; // EOF or error
                output.append(buf, static_cast<size_t>(n));

                // Cap output at 16 KB.
                if (output.size() > 16384)
                {
                    output += "\n... [output truncated at 16 KB]";
                    break;
                }
            }
            else if (ret == 0)
            {
                // Timeout on poll — check if child is still alive.
                int status;
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w > 0)
                    break; // Child exited
            }
            else
            {
                break; // poll error
            }
        }

        close(pipeFds[0]);

        if (timedOut)
        {
            // Kill the entire process group.
            kill(-pid, SIGTERM);
            usleep(100000); // 100ms grace
            kill(-pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            output += "\n[Process killed: exceeded 30-second timeout]";
            return {"run_shell", false, TruncateOutput(output, 16384)};
        }

        // Wait for child to finish.
        int status = 0;
        waitpid(pid, &status, 0);

        int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        std::string header = "Exit code: " + std::to_string(exitCode) + "\n";
        if (!cwd.empty() && cwd != ".")
            header += "Working directory: " + cwdPath.string() + "\n";
        header += "\n";

        return {"run_shell", exitCode == 0, TruncateOutput(header + output, 16384)};
    }
#else
    // Windows implementation: route through PowerShell or bash depending on config.
    // Uses CreateProcess() + a reader thread + WaitForSingleObject() for a 30-second timeout.
    ToolResult ToolRegistry::ExecRunShell(std::unordered_map<std::string, std::string> const& args)
    {
        auto cmdIt = args.find("command");
        if (cmdIt == args.end() || cmdIt->second.empty())
            return {"run_shell", false, "Missing required argument: command"};

        std::string const& command = cmdIt->second;

        // Security: blocklist check.
        if (IsCommandBlocked(command))
            return {"run_shell", false, "Command blocked by security policy: " + command};

        // Determine working directory.
        std::string cwd = ".";
        if (auto cwdIt = args.find("cwd"); cwdIt != args.end() && !cwdIt->second.empty())
        {
            cwd = cwdIt->second;
        }

        fs::path cwdPath = fs::path(cwd).lexically_normal();
        std::error_code ec;
        if (!fs::exists(cwdPath, ec) || !fs::is_directory(cwdPath, ec))
            return {"run_shell", false, "Working directory does not exist: " + cwd};

        // Reject path traversal outside project.
        std::string cwdNorm = cwdPath.string();
        if (cwdNorm.find("..") != std::string::npos)
            return {"run_shell", false, "Path traversal not allowed in cwd"};

        std::string shellCmd;
        if (ShellTaskExecutor::GetWindowsShell() == WindowsShell::PowerShell)
        {
            // PowerShell inline command: Set-Location then run the command, merge stderr.
            std::string psCommand =
                "Set-Location " + QuoteForPowerShellArg(cwdPath.string()) + "; " + command + " 2>&1";
            shellCmd = "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                       "-Command \"" + psCommand + "\"";
        }
        else
        {
            // Bash mode: cd into cwd, then run the command.
            std::string genericCwd = cwdPath.generic_string();
            std::string fullCmd = "cd '" + genericCwd + "' && " + command + " 2>&1";
            shellCmd = WrapForBash(fullCmd);
        }

        // Set up an anonymous pipe for stdout + stderr capture.
        HANDLE hReadPipe  = INVALID_HANDLE_VALUE;
        HANDLE hWritePipe = INVALID_HANDLE_VALUE;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength              = sizeof(sa);
        sa.bInheritHandle       = TRUE; // Child inherits the write end.
        sa.lpSecurityDescriptor = nullptr;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
            return {"run_shell", false, "Failed to create pipe"};
        // Parent's read end must not be inherited.
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError  = hWritePipe;
        si.hStdInput  = INVALID_HANDLE_VALUE;

        PROCESS_INFORMATION pi{};
        std::string cmdMutable = shellCmd; // CreateProcessA requires non-const lpCommandLine.
        BOOL created = CreateProcessA(
            nullptr,
            cmdMutable.data(),
            nullptr,
            nullptr,
            TRUE,             // bInheritHandles — child inherits the pipe write end.
            CREATE_NO_WINDOW, // Don't flash a console window.
            nullptr,
            cwdPath.string().c_str(),
            &si,
            &pi);

        // Close the write end in the parent — when the child exits its copy is also closed,
        // which causes ReadFile in the reader thread to return with a broken-pipe error.
        CloseHandle(hWritePipe);

        if (!created)
        {
            CloseHandle(hReadPipe);
            return {"run_shell", false, "Failed to create process"};
        }

        // Reader thread: drains the pipe into `output` until the pipe is closed.
        std::string output;
        output.reserve(1024);
        std::thread reader([&]()
        {
            char buf[4096];
            DWORD bytesRead = 0;
            while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0)
            {
                output.append(buf, bytesRead);
                if (output.size() > 16384)
                {
                    output += "\n... [output truncated at 16 KB]";
                    break;
                }
            }
        });

        // Wait for the process with a 30-second hard timeout.
        constexpr DWORD k_TimeoutMs = 30000;
        bool const timedOut = (WaitForSingleObject(pi.hProcess, k_TimeoutMs) == WAIT_TIMEOUT);

        if (timedOut)
        {
            TerminateProcess(pi.hProcess, 1);
            // Give the OS up to 5 s to confirm termination so the child's pipe handle is closed,
            // which unblocks the reader thread's ReadFile call.
            WaitForSingleObject(pi.hProcess, 5000);
        }

        // Reader thread exits once the pipe is broken (child dead → write end closed).
        reader.join();
        CloseHandle(hReadPipe);

        if (timedOut)
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            output += "\n[Process killed: exceeded 30-second timeout]";
            return {"run_shell", false, TruncateOutput(output, 16384)};
        }

        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        std::string header = "Exit code: " + std::to_string(exitCode) + "\n";
        if (!cwd.empty() && cwd != ".")
            header += "Working directory: " + cwdPath.string() + "\n";
        header += "\n";

        return {"run_shell", exitCode == 0, TruncateOutput(header + output, 16384)};
    }
#endif

    // -----------------------------------------------------------------
    // write_file
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecWriteFile(std::unordered_map<std::string, std::string> const& args)
    {
        auto pathIt = args.find("path");
        if (pathIt == args.end() || pathIt->second.empty())
            return {"write_file", false, "Missing required argument: path"};

        auto contentIt = args.find("content");
        if (contentIt == args.end())
            return {"write_file", false, "Missing required argument: content"};

        std::string const& path = pathIt->second;
        std::string const& content = contentIt->second;

        // Security: deny-list and path validation.
        if (IsPathDenied(path))
            return {"write_file", false, "Access denied: " + path + " (sensitive file)"};

        fs::path filePath = fs::path(path).lexically_normal();
        std::string normalized = filePath.string();

        // Reject path traversal.
        if (normalized.find("..") != std::string::npos)
            return {"write_file", false, "Path traversal not allowed: " + path};

        // Reject absolute paths.
        if (filePath.is_absolute())
            return {"write_file", false, "Absolute paths not allowed. Use paths relative to project root."};

        // Create parent directories.
        std::error_code ec;
        if (filePath.has_parent_path())
        {
            fs::create_directories(filePath.parent_path(), ec);
            if (ec)
                return {"write_file", false, "Failed to create directories: " + ec.message()};
        }

        // Backup existing file.
        bool existed = fs::exists(filePath, ec);
        if (existed)
        {
            fs::path bakPath = filePath;
            bakPath += ".bak";
            fs::copy_file(filePath, bakPath, fs::copy_options::overwrite_existing, ec);
            // Backup failure is not fatal — proceed with write.
        }

        // Atomic write: write to .tmp, then rename.
        fs::path tmpPath = filePath;
        tmpPath += ".tmp";

        {
            std::ofstream ofs(tmpPath, std::ios::out | std::ios::binary);
            if (!ofs)
                return {"write_file", false, "Cannot open for writing: " + tmpPath.string()};
            ofs << content;
            if (!ofs.good())
                return {"write_file", false, "Write failed: " + tmpPath.string()};
        }

        fs::rename(tmpPath, filePath, ec);
        if (ec)
        {
            fs::remove(tmpPath, ec);
            return {"write_file", false, "Rename failed: " + ec.message()};
        }

        std::string action = existed ? "Overwritten" : "Created";
        return {"write_file", true, action + ": " + normalized + " (" + std::to_string(content.size()) + " bytes)"};
    }

    // -----------------------------------------------------------------
    // edit_file
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecEditFile(std::unordered_map<std::string, std::string> const& args)
    {
        auto pathIt = args.find("path");
        if (pathIt == args.end() || pathIt->second.empty())
            return {"edit_file", false, "Missing required argument: path"};

        auto oldIt = args.find("old_text");
        if (oldIt == args.end() || oldIt->second.empty())
            return {"edit_file", false, "Missing required argument: old_text"};

        auto newIt = args.find("new_text");
        if (newIt == args.end())
            return {"edit_file", false, "Missing required argument: new_text"};

        std::string const& path = pathIt->second;
        std::string const& oldText = oldIt->second;
        std::string const& newText = newIt->second;

        // Security checks.
        if (IsPathDenied(path))
            return {"edit_file", false, "Access denied: " + path + " (sensitive file)"};

        fs::path filePath = fs::path(path).lexically_normal();
        std::string normalized = filePath.string();

        if (normalized.find("..") != std::string::npos)
            return {"edit_file", false, "Path traversal not allowed: " + path};

        if (filePath.is_absolute())
            return {"edit_file", false, "Absolute paths not allowed. Use paths relative to project root."};

        std::error_code ec;
        if (!fs::exists(filePath, ec))
            return {"edit_file", false, "File not found: " + path};

        if (!fs::is_regular_file(filePath, ec))
            return {"edit_file", false, "Not a regular file: " + path};

        // Read file content.
        std::string content;
        {
            std::ifstream ifs(filePath, std::ios::binary);
            if (!ifs)
                return {"edit_file", false, "Cannot open file: " + path};
            content.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }

        // Find old_text — must match exactly once.
        size_t pos = content.find(oldText);
        if (pos == std::string::npos)
            return {"edit_file", false, "old_text not found in file. Make sure it matches exactly (including whitespace)."};

        // Check for multiple matches.
        size_t secondPos = content.find(oldText, pos + 1);
        if (secondPos != std::string::npos)
            return {"edit_file", false,
                    "old_text matches multiple locations in the file. Provide more context to make it unique."};

        // Perform replacement.
        std::string newContent = content.substr(0, pos) + newText + content.substr(pos + oldText.size());

        // Backup existing file.
        {
            fs::path bakPath = filePath;
            bakPath += ".bak";
            fs::copy_file(filePath, bakPath, fs::copy_options::overwrite_existing, ec);
        }

        // Atomic write.
        fs::path tmpPath = filePath;
        tmpPath += ".tmp";

        {
            std::ofstream ofs(tmpPath, std::ios::out | std::ios::binary);
            if (!ofs)
                return {"edit_file", false, "Cannot open for writing: " + tmpPath.string()};
            ofs << newContent;
            if (!ofs.good())
                return {"edit_file", false, "Write failed: " + tmpPath.string()};
        }

        fs::rename(tmpPath, filePath, ec);
        if (ec)
        {
            fs::remove(tmpPath, ec);
            return {"edit_file", false, "Rename failed: " + ec.message()};
        }

        // Calculate line info for the summary.
        int lineNumber = 1;
        for (size_t i = 0; i < pos; ++i)
        {
            if (content[i] == '\n')
                ++lineNumber;
        }
        int oldLines = 1;
        for (char c : oldText)
        {
            if (c == '\n')
                ++oldLines;
        }
        int newLines = 1;
        for (char c : newText)
        {
            if (c == '\n')
                ++newLines;
        }

        std::ostringstream oss;
        oss << "Edited: " << normalized << "\n";
        oss << "  At line " << lineNumber << ": replaced " << oldLines << " line(s) with " << newLines << " line(s)\n";
        oss << "  File size: " << content.size() << " → " << newContent.size() << " bytes";
        return {"edit_file", true, oss.str()};
    }

    // =================================================================
    // L3 runtime control tools
    // =================================================================

    // -----------------------------------------------------------------
    // workflow_pause
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecWorkflowPause(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_RuntimeManager)
            return {"workflow_pause", false, "Runtime manager not available"};

        auto it = args.find("run_id");
        if (it == args.end() || it->second.empty())
            return {"workflow_pause", false, "Missing required argument: run_id"};

        std::string const& runId = it->second;

        // Verify the run exists.
        WorkflowRun run;
        if (!m_RuntimeManager->TryGetRunById(runId, run))
            return {"workflow_pause", false, "Run not found: " + runId};

        if (run.m_State != WorkflowRunState::Running)
            return {"workflow_pause", false,
                    "Run is not in 'running' state (current: " + RunStateToString(run.m_State) + ")"};

        bool ok = m_RuntimeManager->RequestPauseRun(runId);
        if (!ok)
            return {"workflow_pause", false, "Failed to pause run: " + runId};

        return {"workflow_pause", true, "Pause requested for run: " + runId + " (workflow: " + run.m_WorkflowId + ")"};
    }

    // -----------------------------------------------------------------
    // workflow_resume
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecWorkflowResume(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_RuntimeManager)
            return {"workflow_resume", false, "Runtime manager not available"};

        auto it = args.find("run_id");
        if (it == args.end() || it->second.empty())
            return {"workflow_resume", false, "Missing required argument: run_id"};

        std::string const& runId = it->second;

        WorkflowRun run;
        if (!m_RuntimeManager->TryGetRunById(runId, run))
            return {"workflow_resume", false, "Run not found: " + runId};

        if (run.m_State != WorkflowRunState::Paused)
            return {"workflow_resume", false,
                    "Run is not in 'paused' state (current: " + RunStateToString(run.m_State) + ")"};

        bool ok = m_RuntimeManager->RequestResumeRun(runId);
        if (!ok)
            return {"workflow_resume", false, "Failed to resume run: " + runId};

        return {"workflow_resume", true, "Resume requested for run: " + runId + " (workflow: " + run.m_WorkflowId + ")"};
    }

    // -----------------------------------------------------------------
    // workflow_stop
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecWorkflowStop(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_RuntimeManager)
            return {"workflow_stop", false, "Runtime manager not available"};

        auto it = args.find("run_id");
        if (it == args.end() || it->second.empty())
            return {"workflow_stop", false, "Missing required argument: run_id"};

        std::string const& runId = it->second;

        WorkflowRun run;
        if (!m_RuntimeManager->TryGetRunById(runId, run))
            return {"workflow_stop", false, "Run not found: " + runId};

        // Allow stopping from running, paused, or pending states.
        if (run.m_State != WorkflowRunState::Running && run.m_State != WorkflowRunState::Paused &&
            run.m_State != WorkflowRunState::Pending)
        {
            return {"workflow_stop", false, "Run cannot be stopped (current state: " + RunStateToString(run.m_State) + ")"};
        }

        bool ok = m_RuntimeManager->RequestStopRun(runId);
        if (!ok)
            return {"workflow_stop", false, "Failed to stop run: " + runId};

        return {"workflow_stop", true, "Stop requested for run: " + runId + " (workflow: " + run.m_WorkflowId + ")"};
    }

    // -----------------------------------------------------------------
    // get_dashboard_status
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecGetDashboardStatus(std::unordered_map<std::string, std::string> const& /*args*/)
    {
        JarvisAgent* app = App::g_App;
        if (!app)
            return {"get_dashboard_status", false, "Application not available"};

        std::ostringstream oss;
        oss << "=== JarvisAgent Dashboard ===\n\n";

        // Version & uptime
        oss << "Version: " << JARVIS_AGENT_VERSION << "\n";
        auto now = std::chrono::system_clock::now();
        auto uptime = now - app->GetStartupTime();
        auto hours = std::chrono::duration_cast<std::chrono::hours>(uptime).count();
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(uptime).count() % 60;
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(uptime).count() % 60;
        oss << "Uptime: " << hours << "h " << minutes << "m " << seconds << "s\n";
        oss << "Session managers: " << app->GetSessionManagerCount() << "\n\n";

        // Workflow registry
        if (m_WorkflowRegistry)
        {
            auto ids = m_WorkflowRegistry->GetWorkflowIds();
            oss << "Registered workflows: " << ids.size() << "\n";
            for (auto const& id : ids)
            {
                auto wf = m_WorkflowRegistry->GetWorkflow(id);
                oss << "  - " << id;
                if (wf.has_value() && !wf->m_Label.empty())
                    oss << " (" << wf->m_Label << ")";
                oss << "\n";
            }
            oss << "\n";
        }

        // Active runs
        if (m_RuntimeManager)
        {
            auto activeRuns = m_RuntimeManager->GetActiveRunsSnapshot();
            oss << "Active runs: " << activeRuns.size() << "\n";
            for (auto const& run : activeRuns)
            {
                oss << "  - " << run.m_RunId << "  " << run.m_WorkflowId << "  [" << RunStateToString(run.m_State) << "]";
                if (!run.m_StartedAtIso8601.empty())
                    oss << "  started=" << run.m_StartedAtIso8601;
                oss << "\n";
            }

            // Run counters
            uint64_t completed = 0, failed = 0;
            m_RuntimeManager->GetRunCounters(completed, failed);
            oss << "\nRun history: " << completed << " completed, " << failed << " failed\n";

            // Last runs
            auto lastRuns = m_RuntimeManager->GetLastRunsSnapshot();
            if (!lastRuns.empty())
            {
                oss << "\nLast completed run per workflow:\n";
                for (auto const& [wfId, run] : lastRuns)
                {
                    oss << "  - " << run.m_RunId << "  " << wfId << "  [" << RunStateToString(run.m_State) << "]";
                    if (!run.m_CompletedAtIso8601.empty())
                        oss << "  " << run.m_CompletedAtIso8601;
                    oss << "\n";
                }
            }
            oss << "\n";
        }

        // Python engine
        PythonEngine* py = app->GetPythonEngine();
        oss << "Python engine: " << (py ? "ready" : "not available") << "\n";

        return {"get_dashboard_status", true, TruncateOutput(oss.str(), 8192)};
    }

    // =================================================================
    // L3 JCWF development tools
    // =================================================================

    // -----------------------------------------------------------------
    // Helper: resolve workflow_id → absolute .jcwf file path
    // -----------------------------------------------------------------

    std::string ToolRegistry::ResolveWorkflowPath(std::string const& workflowId, std::string& outError) const
    {
        if (!m_WorkflowRegistry)
        {
            outError = "Workflow registry not available";
            return {};
        }

        // First try the registry (knows loaded workflows).
        auto absPath = m_WorkflowRegistry->TryGetWorkflowFilePathAbsolute(workflowId);
        if (absPath.has_value())
            return absPath.value();

        // Fallback: check workflows/<id>.jcwf on disk.
        fs::path candidate = fs::path("workflows") / (workflowId + ".jcwf");
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return fs::absolute(candidate).string();

        outError = "Workflow not found: " + workflowId + ". Use list_workflows to see available workflows.";
        return {};
    }

    // -----------------------------------------------------------------
    // jcwf_read
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecJcwfRead(std::unordered_map<std::string, std::string> const& args)
    {
        auto idIt = args.find("workflow_id");
        if (idIt == args.end() || idIt->second.empty())
            return {"jcwf_read", false, "Missing required argument: workflow_id"};

        std::string error;
        std::string filePath = ResolveWorkflowPath(idIt->second, error);
        if (filePath.empty())
            return {"jcwf_read", false, error};

        std::ifstream ifs(filePath);
        if (!ifs)
            return {"jcwf_read", false, "Cannot open file: " + filePath};

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (content.empty())
            return {"jcwf_read", true, "File is empty: " + filePath};

        return {"jcwf_read", true, TruncateOutput(content, 16384)};
    }

    // -----------------------------------------------------------------
    // jcwf_explain
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecJcwfExplain(std::unordered_map<std::string, std::string> const& args)
    {
        auto idIt = args.find("workflow_id");
        if (idIt == args.end() || idIt->second.empty())
            return {"jcwf_explain", false, "Missing required argument: workflow_id"};

        std::string const& workflowId = idIt->second;

        if (!m_WorkflowRegistry)
            return {"jcwf_explain", false, "Workflow registry not available"};

        auto wfOpt = m_WorkflowRegistry->GetWorkflow(workflowId);
        if (!wfOpt.has_value())
            return {"jcwf_explain", false, "Workflow not found: " + workflowId};

        auto const& wf = wfOpt.value();

        std::ostringstream oss;
        oss << "=== Workflow: " << wf.m_Id << " ===\n";
        if (!wf.m_Label.empty())
            oss << "Label: " << wf.m_Label << "\n";
        if (!wf.m_Doc.empty())
            oss << "Description: " << wf.m_Doc << "\n";
        oss << "Version: " << wf.m_Version << "\n";
        oss << "Manual start: " << (wf.m_ManualStart ? "yes" : "no") << "\n";

        // Triggers
        if (!wf.m_Triggers.empty())
        {
            oss << "Triggers (" << wf.m_Triggers.size() << "):\n";
            for (auto const& trigger : wf.m_Triggers)
            {
                oss << "  - " << TriggerTypeToString(trigger.m_Type);
                if (!trigger.m_Id.empty())
                    oss << " (id: " << trigger.m_Id << ")";
                if (!trigger.m_ParamsJson.empty())
                    oss << "  params: " << trigger.m_ParamsJson;
                oss << "\n";
            }
        }
        oss << "\n";

        // Tasks (unordered_map<string, TaskDef>)
        oss << "Tasks (" << wf.m_Tasks.size() << "):\n";
        size_t taskIdx = 0;
        for (auto const& [taskId, task] : wf.m_Tasks)
        {
            ++taskIdx;
            oss << "  " << taskIdx << ". " << taskId << " [" << TaskTypeToString(task.m_Type) << "]";
            if (!task.m_Label.empty())
                oss << " — " << task.m_Label;
            oss << "\n";

            if (!task.m_WorkingDirectory.empty())
                oss << "     Working dir: " << task.m_WorkingDirectory << "\n";
            if (!task.m_DependsOn.empty())
            {
                oss << "     Depends on: ";
                for (size_t d = 0; d < task.m_DependsOn.size(); ++d)
                {
                    if (d > 0)
                        oss << ", ";
                    oss << task.m_DependsOn[d];
                }
                oss << "\n";
            }
            if (!task.m_FileInputs.empty())
            {
                oss << "     File inputs: ";
                for (size_t fi = 0; fi < task.m_FileInputs.size(); ++fi)
                {
                    if (fi > 0)
                        oss << ", ";
                    oss << task.m_FileInputs[fi];
                }
                oss << "\n";
            }
            if (!task.m_FileOutputs.empty())
            {
                oss << "     File outputs: ";
                for (size_t fo = 0; fo < task.m_FileOutputs.size(); ++fo)
                {
                    if (fo > 0)
                        oss << ", ";
                    oss << task.m_FileOutputs[fo];
                }
                oss << "\n";
            }
            if (task.m_TimeoutMs > 0)
                oss << "     Timeout: " << task.m_TimeoutMs << "ms\n";
            if (!task.m_Filter.empty())
                oss << "     Filter: " << task.m_Filter << "\n";
        }

        // Dataflow edges
        if (!wf.m_Dataflows.empty())
        {
            oss << "\nDataflow (" << wf.m_Dataflows.size() << "):\n";
            for (auto const& df : wf.m_Dataflows)
            {
                oss << "  " << df.m_FromTask << "." << df.m_FromOutput << " → " << df.m_ToTask << "." << df.m_ToInput
                    << "\n";
            }
        }

        // Controlflow edges
        if (!wf.m_ControlflowEdges.empty())
        {
            oss << "\nControlflow (" << wf.m_ControlflowEdges.size() << "):\n";
            for (auto const& edge : wf.m_ControlflowEdges)
            {
                oss << "  " << edge.m_From << " → " << edge.m_To << "\n";
            }
        }

        return {"jcwf_explain", true, TruncateOutput(oss.str(), 8192)};
    }

    // -----------------------------------------------------------------
    // jcwf_validate
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecJcwfValidate(std::unordered_map<std::string, std::string> const& args)
    {
        auto idIt = args.find("workflow_id");
        if (idIt == args.end() || idIt->second.empty())
            return {"jcwf_validate", false, "Missing required argument: workflow_id"};

        std::string error;
        std::string filePath = ResolveWorkflowPath(idIt->second, error);
        if (filePath.empty())
            return {"jcwf_validate", false, error};

        // Read the file content.
        std::ifstream ifs(filePath);
        if (!ifs)
            return {"jcwf_validate", false, "Cannot open file: " + filePath};

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        // Get script registry for validation.
        JarvisAgent* app = App::g_App;
        ScriptRegistry* scriptRegistry = app ? app->GetScriptRegistry() : nullptr;

        std::string validationSummary;
        std::vector<WorkflowValidationIssue> issues;
        bool valid = AiJcwfService::ValidateJcwf(content, validationSummary, scriptRegistry, nullptr, &issues);

        std::ostringstream oss;
        oss << "Validation of " << idIt->second << ": " << (valid ? "VALID" : "INVALID") << "\n";

        if (validationSummary.empty())
        {
            oss << "No errors or warnings.\n";
        }
        else
        {
            oss << "\n" << validationSummary;
        }

        return {"jcwf_validate", true, TruncateOutput(oss.str(), 8192)};
    }

    // -----------------------------------------------------------------
    // jcwf_read_plan
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecJcwfReadPlan(std::unordered_map<std::string, std::string> const& args)
    {
        auto idIt = args.find("workflow_id");
        if (idIt == args.end() || idIt->second.empty())
            return {"jcwf_read_plan", false, "Missing required argument: workflow_id"};

        // Plan file is workflows/<id>.plan.md
        fs::path planPath = fs::path("workflows") / (idIt->second + ".plan.md");
        std::error_code ec;
        if (!fs::exists(planPath, ec))
            return {"jcwf_read_plan", true,
                    "No development plan found for " + idIt->second + ". Use jcwf_write_plan to create one."};

        std::ifstream ifs(planPath);
        if (!ifs)
            return {"jcwf_read_plan", false, "Cannot open plan file: " + planPath.string()};

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (content.empty())
            return {"jcwf_read_plan", true, "Plan file is empty: " + planPath.string()};

        return {"jcwf_read_plan", true, TruncateOutput(content, 8192)};
    }

    // -----------------------------------------------------------------
    // jcwf_write_plan
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecJcwfWritePlan(std::unordered_map<std::string, std::string> const& args)
    {
        auto idIt = args.find("workflow_id");
        if (idIt == args.end() || idIt->second.empty())
            return {"jcwf_write_plan", false, "Missing required argument: workflow_id"};

        auto contentIt = args.find("content");
        if (contentIt == args.end() || contentIt->second.empty())
            return {"jcwf_write_plan", false, "Missing required argument: content"};

        fs::path planPath = fs::path("workflows") / (idIt->second + ".plan.md");

        // Ensure workflows/ directory exists.
        std::error_code ec;
        fs::create_directories(planPath.parent_path(), ec);

        // Atomic write.
        fs::path tmpPath = planPath;
        tmpPath += ".tmp";

        {
            std::ofstream ofs(tmpPath, std::ios::out | std::ios::binary);
            if (!ofs)
                return {"jcwf_write_plan", false, "Cannot open for writing: " + tmpPath.string()};
            ofs << contentIt->second;
            if (!ofs.good())
                return {"jcwf_write_plan", false, "Write failed: " + tmpPath.string()};
        }

        fs::rename(tmpPath, planPath, ec);
        if (ec)
        {
            fs::remove(tmpPath, ec);
            return {"jcwf_write_plan", false, "Rename failed: " + ec.message()};
        }

        return {"jcwf_write_plan", true,
                "Plan written: " + planPath.string() + " (" + std::to_string(contentIt->second.size()) + " bytes)"};
    }

    // -----------------------------------------------------------------
    // jcwf_generate
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecJcwfGenerate(std::unordered_map<std::string, std::string> const& args)
    {
        auto idIt = args.find("workflow_id");
        if (idIt == args.end() || idIt->second.empty())
            return {"jcwf_generate", false, "Missing required argument: workflow_id"};

        std::string const& workflowId = idIt->second;

        // Read the plan.
        fs::path planPath = fs::path("workflows") / (workflowId + ".plan.md");
        std::error_code ec;
        if (!fs::exists(planPath, ec))
            return {"jcwf_generate", false, "No plan found for " + workflowId + ". Create one first with jcwf_write_plan."};

        std::string planContent;
        {
            std::ifstream ifs(planPath);
            if (!ifs)
                return {"jcwf_generate", false, "Cannot read plan: " + planPath.string()};
            planContent.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }

        if (planContent.empty())
            return {"jcwf_generate", false, "Plan file is empty: " + planPath.string()};

        // Use AI to generate JCWF from the plan.
        if (!m_AiCallFn)
            return {"jcwf_generate", false, "AI call function not configured."};

        std::string systemPrompt =
            "You are a JCWF workflow generator. Given a development plan, produce a valid JC Workflow "
            "JSON file. Output ONLY the raw JSON — no markdown fences, no explanations, no preamble.\n\n"
            "Key rules:\n"
            "- The JSON must have: id, version, label, trigger, tasks[], edges[]\n"
            "- Each task needs: id, type (\"shell\" or \"python\" or \"ai_call\"), label\n"
            "- Shell tasks need: script (path starting with \"scripts/\")\n"
            "- Python tasks need: script (path starting with \"scripts/\")\n"
            "- ai_call tasks need: stng_files, prob_files or prob_inline\n"
            "- Edges define execution order: {from, to}\n"
            "- file_inputs are relative to working_directory\n"
            "- Use version \"1.0\" unless using filters/control_nodes (then \"1.1\")\n";

        std::string userPrompt =
            "Generate a JCWF workflow with id \"" + workflowId + "\" based on this plan:\n\n" + planContent;

        std::string response;
        std::string aiError;
        bool ok = m_AiCallFn(systemPrompt, userPrompt, response, aiError);

        if (!ok || response.empty())
            return {"jcwf_generate", false, "AI generation failed: " + (aiError.empty() ? "empty response" : aiError)};

        // Clean up: remove markdown fences if present.
        if (response.starts_with("```"))
        {
            auto firstNewline = response.find('\n');
            if (firstNewline != std::string::npos)
                response = response.substr(firstNewline + 1);
        }
        if (response.ends_with("```"))
            response = response.substr(0, response.size() - 3);
        while (!response.empty() && (response.back() == '\n' || response.back() == ' '))
            response.pop_back();

        // Validate the generated JCWF.
        std::string validationSummary;
        AiJcwfService::ValidateJcwf(response, validationSummary);

        // Write the JCWF file.
        fs::path jcwfPath = fs::path("workflows") / (workflowId + ".jcwf");

        // Backup existing.
        if (fs::exists(jcwfPath, ec))
        {
            fs::path bakPath = jcwfPath;
            bakPath += ".bak";
            fs::copy_file(jcwfPath, bakPath, fs::copy_options::overwrite_existing, ec);
        }

        // Atomic write.
        fs::path tmpPath = jcwfPath;
        tmpPath += ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::out | std::ios::binary);
            if (!ofs)
                return {"jcwf_generate", false, "Cannot write: " + tmpPath.string()};
            ofs << response;
        }
        fs::rename(tmpPath, jcwfPath, ec);
        if (ec)
        {
            fs::remove(tmpPath, ec);
            return {"jcwf_generate", false, "Rename failed: " + ec.message()};
        }

        std::ostringstream oss;
        oss << "Generated: " << jcwfPath.string() << " (" << response.size() << " bytes)\n";
        if (!validationSummary.empty())
            oss << "\nValidation issues:\n" << validationSummary;
        else
            oss << "Validation: OK";

        LOG_APP_INFO("[tools] Generated JCWF '{}' ({} bytes)", workflowId, response.size());
        return {"jcwf_generate", true, oss.str()};
    }

    // -----------------------------------------------------------------
    // jcwf_fix_task
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecJcwfFixTask(std::unordered_map<std::string, std::string> const& args)
    {
        auto idIt = args.find("workflow_id");
        if (idIt == args.end() || idIt->second.empty())
            return {"jcwf_fix_task", false, "Missing required argument: workflow_id"};

        auto taskIt = args.find("task_id");
        if (taskIt == args.end() || taskIt->second.empty())
            return {"jcwf_fix_task", false, "Missing required argument: task_id"};

        auto instrIt = args.find("instructions");
        if (instrIt == args.end() || instrIt->second.empty())
            return {"jcwf_fix_task", false, "Missing required argument: instructions"};

        std::string const& workflowId = idIt->second;
        std::string const& taskId = taskIt->second;
        std::string const& instructions = instrIt->second;

        // Read the JCWF file.
        std::string resolveError;
        std::string filePath = ResolveWorkflowPath(workflowId, resolveError);
        if (filePath.empty())
            return {"jcwf_fix_task", false, resolveError};

        std::string jcwfContent;
        {
            std::ifstream ifs(filePath, std::ios::binary);
            if (!ifs)
                return {"jcwf_fix_task", false, "Cannot open: " + filePath};
            jcwfContent.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }

        // Verify the task_id exists in the JSON.
        if (jcwfContent.find("\"" + taskId + "\"") == std::string::npos)
            return {"jcwf_fix_task", false,
                    "Task \"" + taskId + "\" not found in workflow " + workflowId +
                        ". Use jcwf_explain to see available tasks."};

        // Use AI to fix the task.
        if (!m_AiCallFn)
            return {"jcwf_fix_task", false, "AI call function not configured."};

        std::string systemPrompt =
            "You are a JCWF workflow editor. You are given a complete JCWF JSON file and instructions "
            "to fix a specific task. Apply the fix and output the COMPLETE updated JCWF JSON.\n"
            "Output ONLY the raw JSON — no markdown fences, no explanations, no preamble.\n"
            "Do NOT remove or change any other tasks or edges — only modify the specified task.";

        std::string userPrompt = "Fix task \"" + taskId +
                                 "\" in this workflow.\n\n"
                                 "Instructions: " +
                                 instructions +
                                 "\n\n"
                                 "Current JCWF JSON:\n" +
                                 jcwfContent;

        std::string response;
        std::string aiError;
        bool ok = m_AiCallFn(systemPrompt, userPrompt, response, aiError);

        if (!ok || response.empty())
            return {"jcwf_fix_task", false, "AI fix failed: " + (aiError.empty() ? "empty response" : aiError)};

        // Clean up markdown fences.
        if (response.starts_with("```"))
        {
            auto firstNewline = response.find('\n');
            if (firstNewline != std::string::npos)
                response = response.substr(firstNewline + 1);
        }
        if (response.ends_with("```"))
            response = response.substr(0, response.size() - 3);
        while (!response.empty() && (response.back() == '\n' || response.back() == ' '))
            response.pop_back();

        // Validate.
        std::string validationSummary;
        AiJcwfService::ValidateJcwf(response, validationSummary);

        // Backup and write.
        fs::path jcwfPath(filePath);
        std::error_code ec;
        {
            fs::path bakPath = jcwfPath;
            bakPath += ".bak";
            fs::copy_file(jcwfPath, bakPath, fs::copy_options::overwrite_existing, ec);
        }

        fs::path tmpPath = jcwfPath;
        tmpPath += ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::out | std::ios::binary);
            if (!ofs)
                return {"jcwf_fix_task", false, "Cannot write: " + tmpPath.string()};
            ofs << response;
        }
        fs::rename(tmpPath, jcwfPath, ec);
        if (ec)
        {
            fs::remove(tmpPath, ec);
            return {"jcwf_fix_task", false, "Rename failed: " + ec.message()};
        }

        std::ostringstream oss;
        oss << "Fixed task \"" << taskId << "\" in " << workflowId << "\n";
        if (!validationSummary.empty())
            oss << "\nValidation issues:\n" << validationSummary;
        else
            oss << "Validation: OK";

        LOG_APP_INFO("[tools] Fixed task '{}' in workflow '{}'", taskId, workflowId);
        return {"jcwf_fix_task", true, oss.str()};
    }

    // -----------------------------------------------------------------
    // jcwf_write_script
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecJcwfWriteScript(std::unordered_map<std::string, std::string> const& args)
    {
        auto pathIt = args.find("path");
        if (pathIt == args.end() || pathIt->second.empty())
            return {"jcwf_write_script", false, "Missing required argument: path"};

        auto contentIt = args.find("content");
        if (contentIt == args.end() || contentIt->second.empty())
            return {"jcwf_write_script", false, "Missing required argument: content"};

        auto typeIt = args.find("type");
        if (typeIt == args.end() || typeIt->second.empty())
            return {"jcwf_write_script", false, "Missing required argument: type (\"shell\" or \"python\")"};

        std::string const& path = pathIt->second;
        std::string const& content = contentIt->second;
        std::string const& type = typeIt->second;

        // Validate type.
        if (type != "shell" && type != "python")
            return {"jcwf_write_script", false, "Invalid type: \"" + type + "\". Must be \"shell\" or \"python\"."};

        // Path must start with "scripts/".
        fs::path scriptPath = fs::path(path).lexically_normal();
        std::string normalized = scriptPath.string();
        if (!normalized.starts_with("scripts/") && !normalized.starts_with("scripts\\"))
            return {"jcwf_write_script", false, "Script path must start with \"scripts/\". Got: " + path};

        // Reject absolute paths and traversal.
        if (scriptPath.is_absolute())
            return {"jcwf_write_script", false, "Absolute paths not allowed."};
        if (normalized.find("..") != std::string::npos)
            return {"jcwf_write_script", false, "Path traversal not allowed."};

        // Validate content based on type.
        if (type == "shell")
        {
            if (!content.starts_with("#!/"))
                return {"jcwf_write_script", false, "Shell scripts must start with a shebang (e.g. #!/usr/bin/env bash)."};
            if (content.find("set -euo pipefail") == std::string::npos)
                return {"jcwf_write_script", false, "Shell scripts must include 'set -euo pipefail' for safety."};
        }
        else if (type == "python")
        {
            // Python scripts should have the @jarvis-script marker or shebang.
            // We don't enforce strictly but warn.
        }

        // Create parent directories.
        std::error_code ec;
        fs::create_directories(scriptPath.parent_path(), ec);

        // Backup existing.
        bool existed = fs::exists(scriptPath, ec);
        if (existed)
        {
            fs::path bakPath = scriptPath;
            bakPath += ".bak";
            fs::copy_file(scriptPath, bakPath, fs::copy_options::overwrite_existing, ec);
        }

        // Atomic write.
        fs::path tmpPath = scriptPath;
        tmpPath += ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::out | std::ios::binary);
            if (!ofs)
                return {"jcwf_write_script", false, "Cannot write: " + tmpPath.string()};
            ofs << content;
        }
        fs::rename(tmpPath, scriptPath, ec);
        if (ec)
        {
            fs::remove(tmpPath, ec);
            return {"jcwf_write_script", false, "Rename failed: " + ec.message()};
        }

        // Make shell scripts executable.
        if (type == "shell")
        {
            fs::permissions(scriptPath, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
        }

        std::string action = existed ? "Overwritten" : "Created";
        return {"jcwf_write_script", true,
                action + ": " + normalized + " (" + std::to_string(content.size()) + " bytes, " + type + ")"};
    }

} // namespace AIAssistant
