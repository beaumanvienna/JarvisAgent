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
#include "assistant/assistantHelpers.h"
#include "assistant/assistantMemory.h"
#include "assistant/workspaceIndexer.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "json/jsonHelper.h"
#include "python/pythonEnginePool.h"
#include "simdjson/simdjson.h"
#include "web/aiJcwfService.h"
#include "workflow/workflowRegistry.h"
#include "workflow/workflowRuntimeManager.h"
#include "workflow/workflowTypes.h"
#include "workflow/workflowValidator.h"
#include "workflow/jcwfContainer.h"
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
// MSVC equivalent for popen/pclose, used only on the search_files
// Windows-via-bash fallback.  list_files uses std::filesystem directly and
// get_log_tail no longer exists.  The Windows search_files path single-quote
// escapes its arguments (PosixSingleQuote); the argv exec port is pending.
#define popen _popen
#define pclose _pclose
#endif

namespace fs = std::filesystem;

namespace
{
#if defined(_WIN32)
    // On Windows, _popen routes through cmd.exe which cannot run POSIX commands.
    // Wrap the command in bash (MSYS2 / Git Bash) to match the workflow engine.
    std::string WrapForBash(std::string const& cmd)
    {
        return "bash -c \"" + cmd + "\"";
    }

    // POSIX single-quote escape: every "'" becomes "'\''", whole value wrapped in '...'.
    // Renders an arbitrary string safe for inclusion in a single-quoted /bin/sh argument.
    // Used on the Windows-via-bash path where we still build a shell command string;
    // POSIX paths use argv exec (RunArgvCapture below) and never need this.
    std::string PosixSingleQuote(std::string const& value)
    {
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('\'');
        for (char c : value)
        {
            if (c == '\'')
                out.append("'\\''");
            else
                out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    // Base64 encode (RFC 4648, standard alphabet, no line breaks).  Used to build
    // PowerShell's `-EncodedCommand` payload, which expects UTF-16LE bytes encoded
    // as base64.  Inputs are bounded by the run_shell command size (16 KB tool-arg
    // ceiling), so the simple table-driven encoder is the right tool — adding an
    // OpenSSL or wincrypt dependency for one call site is over-engineered.
    std::string Base64EncodeBytes(std::string const& bytes)
    {
        static char const alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((bytes.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 3 <= bytes.size(); i += 3)
        {
            uint32_t const triple = (static_cast<uint8_t>(bytes[i])     << 16) |
                                    (static_cast<uint8_t>(bytes[i + 1]) << 8)  |
                                     static_cast<uint8_t>(bytes[i + 2]);
            out += alphabet[(triple >> 18) & 0x3F];
            out += alphabet[(triple >> 12) & 0x3F];
            out += alphabet[(triple >> 6)  & 0x3F];
            out += alphabet[ triple        & 0x3F];
        }
        if (i < bytes.size())
        {
            uint32_t triple = static_cast<uint8_t>(bytes[i]) << 16;
            if (i + 1 < bytes.size())
                triple |= static_cast<uint8_t>(bytes[i + 1]) << 8;
            out += alphabet[(triple >> 18) & 0x3F];
            out += alphabet[(triple >> 12) & 0x3F];
            out += (i + 1 < bytes.size()) ? alphabet[(triple >> 6) & 0x3F] : '=';
            out += '=';
        }
        return out;
    }

    // Encode a UTF-8 PowerShell command for the `-EncodedCommand` switch:
    //   1. UTF-8  → UTF-16LE  (PowerShell's required input encoding for -EncodedCommand)
    //   2. UTF-16LE bytes → base64
    // The resulting argv token is opaque to PowerShell's quoting layer — the command
    // body never re-enters the shell parser, eliminating the in-string `"`-escape
    // injection class that the legacy `-Command "..."` path was vulnerable to.
    // Returns empty string on UTF-8 conversion failure (caller treats as parse error).
    std::string EncodePowerShellCommand(std::string const& utf8Command)
    {
        if (utf8Command.empty())
            return {};
        int const wcharCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                   utf8Command.data(),
                                                   static_cast<int>(utf8Command.size()),
                                                   nullptr, 0);
        if (wcharCount <= 0)
            return {};
        std::vector<wchar_t> wbuf(static_cast<size_t>(wcharCount));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                utf8Command.data(), static_cast<int>(utf8Command.size()),
                                wbuf.data(), wcharCount) != wcharCount)
            return {};
        // wchar_t on Windows is 2-byte UTF-16LE — take the raw byte view directly.
        std::string const utf16Bytes(reinterpret_cast<char const*>(wbuf.data()),
                                     wbuf.size() * sizeof(wchar_t));
        return Base64EncodeBytes(utf16Bytes);
    }
#endif

    // Resolve `cwd` against the process's current working directory and verify the
    // result lies under the project root (the process CWD).  Defends against
    //   - `..` segments
    //   - absolute paths to outside-project locations
    //   - symlinks pointing outside the project root
    // Returns true on success and writes the resolved canonical path to canonicalOut;
    // false on any rejection with a human-readable reason in reasonOut.
    bool IsCwdInsideProjectRoot(fs::path const& cwd, fs::path& canonicalOut, std::string& reasonOut)
    {
        std::error_code ec;
        fs::path const root = fs::weakly_canonical(fs::current_path(ec), ec);
        if (ec)
        {
            reasonOut = "cannot resolve project root: " + ec.message();
            return false;
        }
        fs::path const candidate = fs::weakly_canonical(cwd, ec);
        if (ec)
        {
            reasonOut = "cannot resolve cwd: " + ec.message();
            return false;
        }
        // lexically_relative returns "" when no common base exists (e.g. different drives on Windows),
        // "." for equal paths, "<sub>" for child paths, and ".."/"../<x>" when candidate is outside root.
        fs::path const rel = candidate.lexically_relative(root);
        std::string const relStr = rel.string();
        if (relStr.empty() || relStr.rfind("..", 0) == 0)
        {
            reasonOut = "cwd is outside project root: " + candidate.string();
            return false;
        }
        canonicalOut = candidate;
        return true;
    }

#if !defined(_WIN32)
    // Argv-array exec result.  Captures combined stdout+stderr.
    struct ArgvExecResult
    {
        int exitCode = -1;
        bool execFailed = false; // exec syscall failed (no such file, perms)
        bool timedOut = false;
        std::string output;
    };

    // POSIX argv-array exec: child does setpgid + chdir + execvp.  No /bin/sh
    // involvement, no shell metacharacter parsing — argv elements pass
    // straight to execvp.  Replaces the popen-with-string-composed-command
    // pattern; every tool that needs to spawn a process funnels through here
    // for the same fork/exec/poll plumbing.
    //
    // Caller responsibilities:
    //   - validate cwd policy (e.g. inside-project-root) before calling
    //   - choose argv[0] so execvp's PATH search finds the right binary
    //   - cap maxOutputBytes appropriately for the tool
    ArgvExecResult RunArgvCapture(std::vector<std::string> const& argv, fs::path const& cwd,
                                  int timeoutMs, size_t maxOutputBytes)
    {
        ArgvExecResult result;
        if (argv.empty())
        {
            result.execFailed = true;
            return result;
        }

        int pipeFds[2];
        if (pipe(pipeFds) != 0)
        {
            result.execFailed = true;
            return result;
        }

        pid_t const pid = fork();
        if (pid < 0)
        {
            close(pipeFds[0]);
            close(pipeFds[1]);
            result.execFailed = true;
            return result;
        }

        if (pid == 0)
        {
            // Child: new process group for clean kill, dup pipe ends, chdir, exec.
            setpgid(0, 0);
            close(pipeFds[0]);
            dup2(pipeFds[1], STDOUT_FILENO);
            dup2(pipeFds[1], STDERR_FILENO);
            close(pipeFds[1]);

            if (!cwd.empty())
            {
                if (chdir(cwd.c_str()) != 0)
                    _exit(126); // chdir failed — distinct from exec failure (127)
            }

            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for (auto const& a : argv)
                cargv.push_back(const_cast<char*>(a.c_str()));
            cargv.push_back(nullptr);

            execvp(cargv[0], cargv.data());
            _exit(127); // exec failed
        }

        // Parent.
        close(pipeFds[1]);
        auto const start = std::chrono::steady_clock::now();
        struct pollfd pfd
        {
            pipeFds[0], POLLIN, 0
        };

        for (;;)
        {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            int const remaining = timeoutMs - static_cast<int>(elapsed);
            if (remaining <= 0)
            {
                result.timedOut = true;
                break;
            }
            int const r = poll(&pfd, 1, std::min(remaining, 200));
            if (r > 0 && (pfd.revents & POLLIN))
            {
                char buf[4096];
                ssize_t const n = read(pipeFds[0], buf, sizeof(buf));
                if (n <= 0)
                    break;
                result.output.append(buf, static_cast<size_t>(n));
                if (result.output.size() > maxOutputBytes)
                {
                    result.output += "\n... [output truncated]";
                    break;
                }
            }
            else if (r == 0)
            {
                int status = 0;
                pid_t const w = waitpid(pid, &status, WNOHANG);
                if (w > 0)
                    break;
            }
            else
            {
                break;
            }
        }

        close(pipeFds[0]);

        if (result.timedOut)
        {
            kill(-pid, SIGTERM);
            usleep(100000);
            kill(-pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return result;
        }

        int status = 0;
        waitpid(pid, &status, 0);
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        // exit code 127 from execvp = exec failed; 126 from chdir = chdir failed.
        if (result.exitCode == 127 || result.exitCode == 126)
            result.execFailed = true;
        return result;
    }
#endif // !_WIN32

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
                              "Executes a shell command and returns stdout/stderr. Just call this tool — the system "
                              "automatically shows an approval dialog to the user. Do NOT ask for permission yourself. "
                              "Timeout: 30 seconds.",
                              "Args: command (string, required), cwd (string, optional, working directory)", true});
        m_ToolFns["run_shell"] = [this](auto const& a) { return ExecRunShell(a); };

        m_ToolDefs.push_back({"write_file",
                              "Writes content to a file. Creates parent directories if needed. Overwrites existing "
                              "files. Just call this tool — the system handles approval automatically.",
                              "Args: path (string, required, relative to project root), content (string, required)", true});
        m_ToolFns["write_file"] = [this](auto const& a) { return ExecWriteFile(a); };

        m_ToolDefs.push_back({"edit_file",
                              "Replaces a specific text span in a file with new content. The old_text must match "
                              "exactly once in the file. Just call this tool — the system handles approval automatically.",
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

        m_ToolDefs.push_back({"workflow_clean",
                              "Removes intermediate and output files from a workflow's working directory (e.g. .o files, "
                              "compiled artifacts). The workflow must not have an active run. Just call this tool — "
                              "the system handles approval.",
                              "Args: workflow_id (string, required)", true});
        m_ToolFns["workflow_clean"] = [this](auto const& a) { return ExecWorkflowClean(a); };

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
                              "guides JCWF generation. Just call this tool — the system handles approval.",
                              "Args: workflow_id (string, required), content (string, required)", true});
        m_ToolFns["jcwf_write_plan"] = [this](auto const& a) { return ExecJcwfWritePlan(a); };

        m_ToolDefs.push_back({"jcwf_generate",
                              "Generates or regenerates a JCWF workflow from its development plan using AI. "
                              "The plan must exist (use jcwf_write_plan first). Just call this tool — the system handles approval.",
                              "Args: workflow_id (string, required)", true});
        m_ToolFns["jcwf_generate"] = [this](auto const& a) { return ExecJcwfGenerate(a); };

        m_ToolDefs.push_back({"jcwf_fix_task",
                              "Fixes a specific task in a JCWF workflow based on instructions. Reads the "
                              "current task definition, applies the fix via AI, and updates the workflow file. "
                              "Just call this tool — the system handles approval.",
                              "Args: workflow_id (string, required), task_id (string, required), "
                              "instructions (string, required)",
                              true});
        m_ToolFns["jcwf_fix_task"] = [this](auto const& a) { return ExecJcwfFixTask(a); };

        m_ToolDefs.push_back({"jcwf_write_script",
                              "Writes a Python or shell script file to the scripts/ directory. Validates "
                              "that shell scripts have proper shebang and set -euo pipefail. Just call this tool — the system handles approval.",
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
                oss << " [APPROVAL HANDLED BY SYSTEM]";
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

    std::string ToolRegistry::DefangToolMarkers(std::string const& text)
    {
        if (text.empty())
            return text;
        struct Replace
        {
            char const* from;
            char const* to;
        };
        // U+27E8 = E2 9F A8, U+27E9 = E2 9F A9.
        static Replace const subs[] = {
            {"<tool_call>", "\xE2\x9F\xA8tool_call\xE2\x9F\xA9"},
            {"</tool_call>", "\xE2\x9F\xA8/tool_call\xE2\x9F\xA9"},
            {"<tool_result>", "\xE2\x9F\xA8tool_result\xE2\x9F\xA9"},
            {"</tool_result>", "\xE2\x9F\xA8/tool_result\xE2\x9F\xA9"},
        };
        std::string out = text;
        for (auto const& sub : subs)
        {
            size_t pos = 0;
            std::string const fromStr = sub.from;
            std::string const toStr = sub.to;
            while ((pos = out.find(fromStr, pos)) != std::string::npos)
            {
                out.replace(pos, fromStr.size(), toStr);
                pos += toStr.size();
            }
        }
        return out;
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
    // simdjson-backed parser for tool call objects
    // -----------------------------------------------------------------

    namespace
    {
        // Stringify a single tool-arg value into the form callers expect:
        //   string  → unescaped UTF-8 bytes
        //   integer → decimal (preserves the integer representation that downstream
        //             std::stoi callers in run_shell / read_file etc. depend on)
        //   double  → std::to_string(double)
        //   boolean → "true" / "false"
        //   null    → "null"
        //   object/array → raw JSON serialisation (rare — tool args are scalars by
        //             convention; this branch keeps the parser robust if a future
        //             tool definition ever needs a structured arg).
        bool StringifyToolArgValue(simdjson::ondemand::value& val, std::string& out)
        {
            using namespace simdjson;
            ondemand::json_type t;
            if (val.type().get(t) != SUCCESS)
                return false;
            switch (t)
            {
                case ondemand::json_type::string:
                {
                    std::string_view sv;
                    if (val.get_string().get(sv) != SUCCESS)
                        return false;
                    out.assign(sv.data(), sv.size());
                    return true;
                }
                case ondemand::json_type::number:
                {
                    ondemand::number num;
                    if (val.get_number().get(num) != SUCCESS)
                        return false;
                    if (num.is_int64())
                        out = std::to_string(num.get_int64());
                    else if (num.is_uint64())
                        out = std::to_string(num.get_uint64());
                    else
                        out = std::to_string(num.get_double());
                    return true;
                }
                case ondemand::json_type::boolean:
                {
                    bool b;
                    if (val.get_bool().get(b) != SUCCESS)
                        return false;
                    out = b ? "true" : "false";
                    return true;
                }
                case ondemand::json_type::null:
                {
                    out = "null";
                    return true;
                }
                case ondemand::json_type::array:
                case ondemand::json_type::object:
                {
                    std::string_view raw;
                    if (val.raw_json().get(raw) != SUCCESS)
                        return false;
                    out.assign(raw.data(), raw.size());
                    while (!out.empty() && (out.back() == ' ' || out.back() == '\n' ||
                                            out.back() == '\r' || out.back() == '\t'))
                        out.pop_back();
                    return true;
                }
                default:
                    // simdjson surfaces json_type::unknown for a value at EOF or in
                    // an iterator-error state; treat as a parse failure.
                    return false;
            }
        }

        bool ParseToolCallJson(std::string const& json, ToolCall& out)
        {
            using namespace simdjson;
            if (json.empty())
                return false;

            try
            {
                ondemand::parser parser;
                padded_string padded(json);
                ondemand::document doc;
                if (parser.iterate(padded).get(doc) != SUCCESS)
                    return false;

                ondemand::object root;
                if (doc.get_object().get(root) != SUCCESS)
                    return false;

                for (auto field : root)
                {
                    std::string_view keyView;
                    if (field.unescaped_key().get(keyView) != SUCCESS)
                        continue;

                    if (keyView == "name")
                    {
                        std::string_view nameView;
                        if (field.value().get_string().get(nameView) != SUCCESS)
                            return false;
                        out.name.assign(nameView.data(), nameView.size());
                    }
                    else if (keyView == "args")
                    {
                        ondemand::object argsObj;
                        if (field.value().get_object().get(argsObj) != SUCCESS)
                            return false;

                        for (auto argField : argsObj)
                        {
                            std::string_view argKeyView;
                            if (argField.unescaped_key().get(argKeyView) != SUCCESS)
                                continue;

                            ondemand::value argVal;
                            if (argField.value().get(argVal) != SUCCESS)
                                continue;

                            std::string serialised;
                            if (!StringifyToolArgValue(argVal, serialised))
                                continue;

                            out.args.emplace(std::string(argKeyView), std::move(serialised));
                        }
                    }
                    // Unknown keys: ignored.  field.value() is implicitly consumed
                    // by simdjson when the iterator advances on the next loop.
                }

                return !out.name.empty();
            }
            catch (simdjson_error const&)
            {
                return false;
            }
        }
    } // namespace

    // -----------------------------------------------------------------
    // Security: deny-list for read_file
    // -----------------------------------------------------------------

    // Deny-list policy for tool-driven file access.  Resolves the input via
    // fs::weakly_canonical (so symlinks in any existing prefix collapse to
    // their target) and case-folds filename + extension before comparison.
    // Anything that resolves outside the project root is denied; backup/temp
    // extensions (.bak, .tmp) and per-base backup filenames (config.json.bak
    // etc.) are denied even if the underlying base file is allowed.  Fail
    // closed on any resolution error.
    bool ToolRegistry::IsPathDenied(std::string const& path)
    {
        if (path.empty())
            return true;

        std::error_code ec;
        fs::path const projectRoot = fs::weakly_canonical(fs::current_path(ec), ec);
        if (ec)
            return true; // fail closed if we cannot resolve our own cwd

        // Resolve `path` to absolute canonical form, following any existing symlink
        // segments.  weakly_canonical canonicalizes the existing prefix and leaves
        // a dangling tail intact, so non-existent files still get the symlink-resolved
        // parent component for confinement checks.
        fs::path const absInput = fs::path(path).is_absolute() ? fs::path(path) : (projectRoot / path);
        fs::path const resolved = fs::weakly_canonical(absInput, ec);
        if (ec)
            return true; // fail closed

        // 1. Reject anything outside the project root — defends against the symlink
        //    "safe.txt → /etc/passwd" exfiltration and explicit absolute-path inputs.
        fs::path const rel = resolved.lexically_relative(projectRoot);
        std::string const relStr = rel.string();
        if (relStr.empty() || relStr.rfind("..", 0) == 0)
            return true;

        // 2. Subtree deny: assistant/ holds the assistant's own memory + index +
        //    summaries; the AI must not read its own prompt material.  Both POSIX
        //    and Windows separator forms checked because lexically_relative returns
        //    the native separator.
        static char const* const subtreePrefixes[] = {"assistant/", "assistant\\"};
        for (char const* p : subtreePrefixes)
            if (relStr.rfind(p, 0) == 0)
                return true;

        // Lowercase the filename + extension once for case-insensitive comparisons —
        // case-insensitive filesystems (Windows, default macOS APFS) would otherwise
        // let `Config.JSON` slip past a case-sensitive literal compare.
        auto toLower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        std::string const fnameLower = toLower(resolved.filename().string());
        std::string const extLower = toLower(resolved.extension().string());

        // 3. Filename-base denies — sensitive files plus their auto-generated .bak / .tmp
        //    siblings (ExecWriteFile / ExecEditFile create these on every overwrite, and
        //    they retain prior contents of the named base).
        static char const* const deniedFilenames[] = {
            "config.json",   "config.json.bak",   "config.json.tmp",
            "keys.json",     "keys.json.bak",     "keys.json.tmp",
            "keys.json.enc", "keys.json.enc.bak", "keys.json.enc.tmp",
            ".env",          ".env.bak",          ".env.tmp",
        };
        for (char const* d : deniedFilenames)
            if (fnameLower == d)
                return true;

        // 4. Extension denies — `.bak` / `.tmp` are auto-generated by the write tools and
        //    may contain prior contents of any file the AI wrote (including ones the
        //    deny-list missed at write time).  `.pem` / `.key` cover crypto material.
        static char const* const deniedExts[] = {".pem", ".key", ".bak", ".tmp"};
        for (char const* e : deniedExts)
            if (extLower == e)
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
                case TaskType::SubWorkflow:
                    return "sub_workflow";
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
        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
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

        // Python engine pool
        PythonEnginePool* pyPool = app->GetPythonEnginePool();
        oss << "  Python engine pool: " << (pyPool ? std::to_string(pyPool->GetEngineCount()) + " engine(s)" : "not available")
            << "\n";

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
                    oss << " — " << DefangToolMarkers(taskState.m_LastErrorMessage);
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

        // Defang structural tokens before reflecting external bytes back into the AI's
        // context — a script that printed `<tool_call>...</tool_call>` to stdout would
        // otherwise become a parsed tool call on the next AI turn.  See DefangToolMarkers.
        if (!ts.m_LastErrorMessage.empty())
            oss << "Error: " << DefangToolMarkers(ts.m_LastErrorMessage) << "\n";

        if (!ts.m_CapturedStdout.empty())
        {
            oss << "\n--- stdout ---\n";
            oss << DefangToolMarkers(TruncateOutput(ts.m_CapturedStdout));
        }
        if (!ts.m_CapturedStderr.empty())
        {
            oss << "\n--- stderr ---\n";
            oss << DefangToolMarkers(TruncateOutput(ts.m_CapturedStderr));
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

        EnqueueRunResult const enqueueResult = m_RuntimeManager->EnqueueWorkflowRunAndGetRunId(workflowId);
        if (enqueueResult.m_Status != EnqueueStatus::Ok)
        {
            return {"run_workflow", false,
                    "Failed to start workflow '" + workflowId + "': " + enqueueResult.m_Message};
        }
        return {"run_workflow", true,
                "Workflow run started: " + workflowId + " (run ID: " + enqueueResult.m_RunId + ")"};
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

        std::string const& query = queryIt->second;
        std::string glob;
        if (auto globIt = args.find("glob"); globIt != args.end() && !globIt->second.empty())
            glob = globIt->second;

#if !defined(_WIN32)
        // POSIX: argv exec, no shell.  query and glob are passed as discrete
        // argv elements, never concatenated into a shell command.  Missing rg
        // returns a clean error rather than falling back to grep with the
        // same unescaped query.
        std::vector<std::string> argv = {"rg", "--no-heading",  "--line-number", "--max-count",
                                         "30", "--max-columns", "200",           "--color",
                                         "never"};
        if (!glob.empty())
        {
            argv.emplace_back("--glob");
            argv.emplace_back(glob);
        }
        for (char const* exclude :
             {"!node_modules", "!bin", "!bin-int", "!vendor", "!.git"})
        {
            argv.emplace_back("--glob");
            argv.emplace_back(exclude);
        }
        argv.emplace_back("--");
        argv.emplace_back(query);
        argv.emplace_back(".");

        ArgvExecResult const r = RunArgvCapture(argv, fs::current_path(), 30000, kMaxToolOutputSize);
        if (r.execFailed)
        {
            return {"search_files", false,
                    "ripgrep (rg) is not installed or not on PATH. Install ripgrep to use search_files."};
        }
        if (r.timedOut)
            return {"search_files", false, "search_files timed out (30s)"};
        if (r.output.empty())
            return {"search_files", true, "No matches found for: " + query};
        return {"search_files", true, TruncateOutput(r.output)};
#else
        // Windows: still routes through popen-via-bash (MSYS2/Git Bash).
        // Pending the argv-exec port, apply full POSIX single-quote escaping
        // so query and glob can't break out of their quoted positions.
        std::string cmd = "rg --no-heading --line-number --max-count 30 --max-columns 200 --color never";
        if (!glob.empty())
            cmd += " --glob " + PosixSingleQuote(glob);
        for (char const* exclude :
             {"!node_modules", "!bin", "!bin-int", "!vendor", "!.git"})
        {
            cmd += " --glob ";
            cmd += PosixSingleQuote(exclude);
        }
        cmd += " -- " + PosixSingleQuote(query) + " . 2>/dev/null";

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
            return {"search_files", true, "No matches found for: " + query};

        return {"search_files", true, TruncateOutput(result)};
#endif
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

        // No exec is invoked — std::filesystem walks the tree with no shell
        // parser anywhere on the path.
        fs::path const dirPath = fs::path(path).lexically_normal();
        std::error_code ec;
        if (!fs::exists(dirPath, ec) || !fs::is_directory(dirPath, ec))
            return {"list_files", false, "Not a directory: " + path};

        // Excluded directory components: must match the leaf name to keep the walk fast (we
        // disable_recursion_pending() at the boundary instead of filtering each entry's full
        // path string).  Same set as the prior `find -not -path` flags.
        static auto const isExcluded = [](std::string const& leaf) {
            return leaf == "node_modules" || leaf == ".git" || leaf == "bin-int" ||
                   leaf == "vendor" || leaf == "bin";
        };

        std::string result;
        result.reserve(2048);
        size_t const kMaxLines = 100;
        size_t lineCount = 0;

        // Include the root entry itself first to match the prior `find` shape (it printed the
        // start path as the first line).
        result += "d ";
        result += dirPath.string();
        result += '\n';
        ++lineCount;

        try
        {
            fs::recursive_directory_iterator it(
                dirPath, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator const end;
            for (; it != end && !ec; it.increment(ec))
            {
                if (lineCount >= kMaxLines || result.size() > kMaxToolOutputSize)
                    break;

                fs::directory_entry const& entry = *it;
                fs::path const& entryPath = entry.path();
                std::string const leaf = entryPath.filename().string();

                if (entry.is_directory(ec) && isExcluded(leaf))
                {
                    it.disable_recursion_pending();
                    continue;
                }

                // Depth: 1 = direct children of dirPath; cap at maxDepth (1..5).
                int const depth = it.depth() + 1;
                if (depth > maxDepth)
                {
                    it.disable_recursion_pending();
                    continue;
                }

                char const typeChar = entry.is_directory(ec) ? 'd' : (entry.is_regular_file(ec) ? 'f' : 'l');
                result += typeChar;
                result += ' ';
                result += entryPath.string();
                result += '\n';
                ++lineCount;
            }
        }
        catch (std::exception const& e)
        {
            // recursive_directory_iterator can throw on non-permission errors even with the
            // skip_permission_denied flag.  Treat as partial success rather than total failure.
            result += "[walk aborted: ";
            result += e.what();
            result += "]\n";
        }

        if (lineCount <= 1)
            return {"list_files", true, "Directory is empty: " + path};

        return {"list_files", true, TruncateOutput(result)};
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

        // Gate the path through IsPathDenied — file content is forwarded to
        // the external AI provider for summarization, so the same deny rules
        // that guard ExecReadFile apply here.  Run the deny check before the
        // cached-summary lookup too, in case the cache was populated before
        // the rule landed.
        if (IsPathDenied(filePath))
            return {"get_file_summary", false, "Access denied: " + filePath + " (sensitive file)"};

        // Check for cached summary first.
        std::string cached = m_WorkspaceIndexer->GetFileSummary(filePath);
        if (!cached.empty())
        {
            return {"get_file_summary", true, "Summary of " + filePath + ":\n\n" + cached};
        }

        // No cached summary — generate one via AI call.
        if (!m_AiCallFn)
            return {"get_file_summary", false, "AI call function not configured. Cannot generate summary."};

        // Read file content (workspace-confined).
        std::string content = m_WorkspaceIndexer->ReadFileContent(filePath, 32768);
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
    //
    // run_shell is the deliberate "AI invokes an arbitrary shell command" tool;
    // `/bin/sh -c <command>` IS its contract.  Static analyzers (and any future
    // Sonnet/audit run) WILL flag the `execl("/bin/sh", "sh", "-c", command, ...)`
    // line on every pass — that is correct flagging of the right shape, not a
    // regression.  The reviewer's job here is to confirm the four defenses
    // below are still load-bearing, NOT to remove `/bin/sh`.
    //
    // Defense layering — all four must remain in place:
    //
    //   (a) Human-in-the-loop approval at the controller layer.  The ToolDef
    //       at the top of this file marks run_shell with `requiresApproval`,
    //       and `AssistantController` routes every approval-required tool call
    //       through `RequestToolApproval` before `m_ToolRegistry.Execute` runs.
    //       No MCP / REST / WebSocket path reaches `ExecRunShell` directly —
    //       the only ingress is through the controller.  This is the primary,
    //       load-bearing defense.
    //
    //   (b) Allowlist-over-blocklist discipline.  We do NOT filter the
    //       `command` string with a "dangerous chars" blocklist — blocklists
    //       fail open (an attacker only needs one bypass to break the gate).
    //       The gate is the approval flow above; the command body is opaque
    //       to this layer.
    //
    //   (c) Canonical-path gate on the working directory.  `cwd` is resolved
    //       through `IsCwdInsideProjectRoot` (canonical-path comparison
    //       against the process CWD), which rejects `..` segments, absolute
    //       escapes, and symlinks pointing outside the project root.  The
    //       resolved canonical cwd is then applied via `chdir()` in the
    //       child — never composed into the shell command string — so a
    //       hostile cwd value cannot smuggle `&& <other-command>` chains
    //       through string concatenation.
    //
    //   (d) Bounded execution.  30-second wall-clock timeout enforced by the
    //       parent poll loop; the child is placed in its own process group
    //       via `setpgid(0, 0)` so the timeout path can `kill(-pid, SIGTERM)`
    //       followed by `SIGKILL` to reap the entire subtree (closes the
    //       runaway-process and orphaned-grandchild vectors).  Output is
    //       capped at 16 KB.
    //
    // The two `chdir()` sites in this file are: the one below (gated by
    // IsCwdInsideProjectRoot), and `RunArgvCapture` at the top of the
    // anonymous namespace (always invoked with `fs::current_path()` from
    // trusted call sites, never with user input).  Any third `chdir()` here
    // would need to route through `IsCwdInsideProjectRoot` for the same
    // reason.

#if !defined(_WIN32)
    ToolResult ToolRegistry::ExecRunShell(std::unordered_map<std::string, std::string> const& args)
    {
        auto cmdIt = args.find("command");
        if (cmdIt == args.end() || cmdIt->second.empty())
            return {"run_shell", false, "Missing required argument: command"};

        std::string const& command = cmdIt->second;

        // Determine + validate working directory against project root.
        std::string cwdArg = ".";
        if (auto cwdIt = args.find("cwd"); cwdIt != args.end() && !cwdIt->second.empty())
            cwdArg = cwdIt->second;

        fs::path canonicalCwd;
        std::string reason;
        if (!IsCwdInsideProjectRoot(cwdArg, canonicalCwd, reason))
            return {"run_shell", false, "cwd rejected: " + reason};

        std::error_code ec;
        if (!fs::is_directory(canonicalCwd, ec))
            return {"run_shell", false, "Working directory does not exist: " + cwdArg};

        // Run command in subshell with the validated cwd applied via chdir(), not via
        // string composition.  Combined stdout+stderr captured; 30s timeout enforced by
        // the parent poll loop (kill the whole process group on timeout).
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
            // Child.
            setpgid(0, 0);
            close(pipeFds[0]);
            dup2(pipeFds[1], STDOUT_FILENO);
            dup2(pipeFds[1], STDERR_FILENO);
            close(pipeFds[1]);

            if (chdir(canonicalCwd.c_str()) != 0)
                _exit(126); // chdir failed

            execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            _exit(127); // exec failed
        }

        // Parent.
        close(pipeFds[1]);

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
                    break;
                output.append(buf, static_cast<size_t>(n));
                if (output.size() > 16384)
                {
                    output += "\n... [output truncated at 16 KB]";
                    break;
                }
            }
            else if (ret == 0)
            {
                int status;
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w > 0)
                    break;
            }
            else
            {
                break;
            }
        }

        close(pipeFds[0]);

        if (timedOut)
        {
            kill(-pid, SIGTERM);
            usleep(100000);
            kill(-pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            output += "\n[Process killed: exceeded 30-second timeout]";
            return {"run_shell", false, TruncateOutput(output, 16384)};
        }

        int status = 0;
        waitpid(pid, &status, 0);
        int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        std::string header = "Exit code: " + std::to_string(exitCode) + "\n";
        if (cwdArg != ".")
            header += "Working directory: " + canonicalCwd.string() + "\n";
        header += "\n";

        return {"run_shell", exitCode == 0, TruncateOutput(header + output, 16384)};
    }
#else
    // Windows implementation: route through PowerShell or bash depending on
    // config.  CreateProcess() + reader thread + WaitForSingleObject() for a
    // 30-second timeout.
    //
    // Security parity with POSIX:
    //   - cwd validated via IsCwdInsideProjectRoot (canonical-path comparison)
    //   - cwd applied via CreateProcessA's lpCurrentDirectory, never composed
    //     into the shell command (no "Set-Location <cwd>;" / "cd <cwd> &&"
    //     prefix that could become a CWD-injection vector).
    //   - PowerShell mode encodes the command via -EncodedCommand (UTF-16LE +
    //     base64).  The command body is a single argv token that PowerShell's
    //     parser does not re-process, eliminating the in-string `"`-escape
    //     injection class the legacy -Command path was vulnerable to.
    ToolResult ToolRegistry::ExecRunShell(std::unordered_map<std::string, std::string> const& args)
    {
        auto cmdIt = args.find("command");
        if (cmdIt == args.end() || cmdIt->second.empty())
            return {"run_shell", false, "Missing required argument: command"};

        std::string const& command = cmdIt->second;

        // Determine + validate working directory against project root.
        std::string cwdArg = ".";
        if (auto cwdIt = args.find("cwd"); cwdIt != args.end() && !cwdIt->second.empty())
            cwdArg = cwdIt->second;

        fs::path canonicalCwd;
        std::string reason;
        if (!IsCwdInsideProjectRoot(cwdArg, canonicalCwd, reason))
            return {"run_shell", false, "cwd rejected: " + reason};

        std::error_code ec;
        if (!fs::is_directory(canonicalCwd, ec))
            return {"run_shell", false, "Working directory does not exist: " + cwdArg};

        std::string shellCmd;
        if (ShellTaskExecutor::GetWindowsShell() == WindowsShell::PowerShell)
        {
            // PowerShell -EncodedCommand: UTF-8 → UTF-16LE → base64.  The command
            // body is opaque to PowerShell's quoting layer; cwd is set via
            // lpCurrentDirectory below.  stderr merged into stdout.
            std::string const psCommand = command + " 2>&1";
            std::string const encoded = EncodePowerShellCommand(psCommand);
            if (encoded.empty())
                return {"run_shell", false, "Command contains invalid UTF-8"};
            shellCmd = "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                       "-EncodedCommand " + encoded;
        }
        else
        {
            // Bash mode.  cwd inherited from CreateProcessA's lpCurrentDirectory; no cd prefix.
            std::string const fullCmd = command + " 2>&1";
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
            canonicalCwd.string().c_str(),
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
        if (cwdArg != ".")
            header += "Working directory: " + canonicalCwd.string() + "\n";
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
    // workflow_clean
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecWorkflowClean(std::unordered_map<std::string, std::string> const& args)
    {
        if (!m_RuntimeManager)
            return {"workflow_clean", false, "Runtime manager not available"};

        auto it = args.find("workflow_id");
        if (it == args.end() || it->second.empty())
            return {"workflow_clean", false, "Missing required argument: workflow_id"};

        std::string const& workflowId = it->second;
        std::string errorMessage;
        bool const ok = m_RuntimeManager->CleanWorkflow(workflowId, errorMessage);

        if (!ok)
            return {"workflow_clean", false, "Failed to clean workflow '" + workflowId + "': " + errorMessage};

        return {"workflow_clean", true, "Workflow '" + workflowId + "' cleaned successfully."};
    }

    // -----------------------------------------------------------------
    // get_dashboard_status
    // -----------------------------------------------------------------

    ToolResult ToolRegistry::ExecGetDashboardStatus(std::unordered_map<std::string, std::string> const& /*args*/)
    {
        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
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
        oss << "Uptime: " << hours << "h " << minutes << "m " << seconds << "s\n\n";

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

        // Python engine pool
        PythonEnginePool* pyPool = app->GetPythonEnginePool();
        oss << "Python engine pool: " << (pyPool ? std::to_string(pyPool->GetEngineCount()) + " engine(s)" : "not available")
            << "\n";

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

    // Helper: Read the root canvas JSON content from a .jcwf zip container.
    static bool ReadJcwfContent(std::string const& filePath, std::string& outContent, std::string& outError)
    {
        // List entries to find the root canvas JSON (any .json that isn't global.json).
        auto entries = JcwfContainer::ListEntries(filePath);
        std::string rootCanvasEntry;

        for (auto const& entry : entries)
        {
            // Skip directories, global.json, and files in subdirectories.
            if (entry.back() == '/')
                continue;
            if (entry == "global.json")
                continue;
            if (entry.find('/') != std::string::npos)
                continue;
            if (entry.size() > 5 && entry.substr(entry.size() - 5) == ".json")
            {
                rootCanvasEntry = entry;
                break;
            }
        }

        if (rootCanvasEntry.empty())
        {
            outError = "No root canvas JSON found in container: " + filePath;
            return false;
        }

        return JcwfContainer::ReadFile(filePath, rootCanvasEntry, outContent, outError);
    }

    // Helper: Write JSON content back to a .jcwf zip container.
    // Updates the root canvas JSON in the extracted directory and repacks.
    static bool WriteJcwfContent(std::string const& filePath, std::string const& jsonContent, std::string& outError)
    {
        fs::path const jcwfPath(filePath);
        fs::path const extractedDir = jcwfPath.parent_path() / jcwfPath.stem();

        std::error_code ec;

        // Ensure the extracted directory exists (extract the zip if needed).
        if (!fs::is_directory(extractedDir, ec))
        {
            if (!JcwfContainer::Extract(jcwfPath, extractedDir, outError))
                return false;
        }

        // Find the root canvas JSON file in the extracted dir.
        fs::path rootCanvasPath;
        for (auto const& entry : fs::directory_iterator(extractedDir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension().string() != ".json")
                continue;
            if (entry.path().filename().string() == "global.json")
                continue;
            rootCanvasPath = entry.path();
            break;
        }

        if (rootCanvasPath.empty())
        {
            rootCanvasPath = extractedDir / (jcwfPath.stem().string() + ".json");
        }

        // Write the canvas JSON.
        {
            std::ofstream ofs(rootCanvasPath, std::ios::out | std::ios::binary);
            if (!ofs)
            {
                outError = "Cannot write: " + rootCanvasPath.string();
                return false;
            }
            ofs << jsonContent;
        }

        // Repack the container.
        return JcwfContainer::Pack(extractedDir, jcwfPath, outError);
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

        std::string content;
        if (!ReadJcwfContent(filePath, content, error))
            return {"jcwf_read", false, error};

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

        // Read the canvas JSON content (handles zip containers and legacy JSON).
        std::string content;
        if (!ReadJcwfContent(filePath, content, error))
            return {"jcwf_validate", false, error};

        // Get script registry for validation.
        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
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
            "- The root JSON object has: \"tasks\" (an OBJECT keyed by task ID, NOT an array)\n"
            "- Example structure:\n"
            "  {\"tasks\": {\"my_task\": {\"id\": \"my_task\", \"type\": \"shell\", ...}}}\n"
            "- WRONG: \"tasks\": [{...}]  — NEVER use an array for tasks\n"
            "- Each task needs: id, type (\"shell\" or \"python\" or \"ai_call\"), label\n"
            "- Shell tasks need: params.command (path starting with \"scripts/\")\n"
            "- Python tasks need: params.command (path starting with \"scripts/\")\n"
            "- ai_call tasks need: stng_files, prob_files or prob_inline\n"
            "- ai_call tasks MUST NOT declare \"file_outputs\" (they resolve into the watched queue folder and "
            "trigger a wasted second AI call). To expose the AI response to a downstream task, add an "
            "\"outputs\" slot: \"outputs\": { \"slot_name\": { \"type\": \"string\" } } — it auto-maps to the "
            "natural PROB_*.output.txt file, referenceable as {{taskId.output_file}} downstream.\n"
            "- Task dependencies use \"depends_on\": [\"other_task_id\"]\n"
            "- file_inputs are relative to working_directory\n"
            "- working_directory must be present (use \"\" for workflow root)\n"
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

        // Write the JCWF container (zip) with global.json + canvas JSON.
        fs::path jcwfPath = fs::path("workflows") / (workflowId + ".jcwf");
        fs::path extractedDir = fs::path("workflows") / workflowId;

        // Backup existing.
        if (fs::exists(jcwfPath, ec))
        {
            fs::path bakPath = jcwfPath;
            bakPath += ".bak";
            fs::copy_file(jcwfPath, bakPath, fs::copy_options::overwrite_existing, ec);
        }

        // Create the extracted directory structure.
        fs::create_directories(extractedDir, ec);

        // Write global.json (minimal metadata).  workflowId is JSON-escaped
        // before embedding; otherwise a value containing a quote, backslash,
        // or control char would produce malformed JSON or flip surrounding
        // metadata (e.g. manual_start) by injection.
        {
            std::ofstream ofs(extractedDir / "global.json", std::ios::out | std::ios::binary);
            if (ofs)
            {
                ofs << "{\n  \"version\": \"1.1\",\n  \"id\": \"" << JsonHelper::EscapeJsonString(workflowId)
                    << "\",\n  \"manual_start\": true\n}";
            }
        }

        // Write the root canvas JSON.
        {
            std::ofstream ofs(extractedDir / (workflowId + ".json"), std::ios::out | std::ios::binary);
            if (!ofs)
                return {"jcwf_generate", false, "Cannot write canvas JSON to: " + extractedDir.string()};
            ofs << response;
        }

        // Pack into zip container.
        std::string packError;
        if (!JcwfContainer::Pack(extractedDir, jcwfPath, packError))
            return {"jcwf_generate", false, "Failed to pack container: " + packError};

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

        // Reject task_id values outside the strict opaque-id shape (alphanumerics +
        // `_` + `-`, 1..128 chars).  A task_id is downstream-embedded into the AI
        // user prompt below, so any JSON metacharacter or whitespace in it would
        // give an attacker-controlled string the ability to steer the workflow-
        // editor AI.  Allowlist over blocklist.
        if (!IsValidOpaqueId(taskId))
        {
            LOG_APP_ERROR("[tools] jcwf_fix_task rejected: invalid task_id shape "
                          "workflow='{}' task='{}' (must match [A-Za-z0-9_-]{{1,128}})",
                          workflowId, taskId);
            return {"jcwf_fix_task", false,
                    "Invalid task_id \"" + taskId + "\": must be 1-128 chars of [A-Za-z0-9_-]"};
        }

        // Read the JCWF canvas JSON.
        std::string resolveError;
        std::string filePath = ResolveWorkflowPath(workflowId, resolveError);
        if (filePath.empty())
        {
            LOG_APP_ERROR("[tools] jcwf_fix_task failed: workflow not found "
                          "workflow='{}' task='{}': {}",
                          workflowId, taskId, resolveError);
            return {"jcwf_fix_task", false, resolveError};
        }

        std::string jcwfContent;
        if (!ReadJcwfContent(filePath, jcwfContent, resolveError))
        {
            LOG_APP_ERROR("[tools] jcwf_fix_task failed: cannot read JCWF "
                          "workflow='{}' task='{}': {}",
                          workflowId, taskId, resolveError);
            return {"jcwf_fix_task", false, resolveError};
        }

        // Structural existence check against the `tasks` object — the prior
        // `jcwfContent.find("\"" + taskId + "\"")` matched any quoted occurrence
        // anywhere in the file (depends_on references, inline content payloads,
        // sub-string collisions in CNTX/STNG strings, etc.), so a present-but-
        // unreferenced taskId could still drive the AI rewrite path.
        {
            using namespace simdjson;
            ondemand::parser jsonParser;
            padded_string padded(jcwfContent);
            ondemand::document doc;
            if (jsonParser.iterate(padded).get(doc) != SUCCESS)
            {
                LOG_APP_ERROR("[tools] jcwf_fix_task failed: canvas JSON did not parse "
                              "workflow='{}' task='{}'",
                              workflowId, taskId);
                return {"jcwf_fix_task", false,
                        "Workflow " + workflowId + " canvas JSON failed to parse"};
            }

            ondemand::object tasksObj;
            if (doc["tasks"].get_object().get(tasksObj) != SUCCESS)
            {
                LOG_APP_ERROR("[tools] jcwf_fix_task failed: canvas has no 'tasks' object "
                              "workflow='{}' task='{}'",
                              workflowId, taskId);
                return {"jcwf_fix_task", false,
                        "Workflow " + workflowId + " has no tasks object in its canvas"};
            }

            ondemand::value taskVal;
            if (tasksObj[taskId].get(taskVal) != SUCCESS)
            {
                LOG_APP_ERROR("[tools] jcwf_fix_task failed: task not in tasks object "
                              "workflow='{}' task='{}'",
                              workflowId, taskId);
                return {"jcwf_fix_task", false,
                        "Task \"" + taskId + "\" not found in workflow " + workflowId +
                            ". Use jcwf_explain to see available tasks."};
            }
        }

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

        // Backup and write (handles zip containers and legacy JSON).
        fs::path jcwfPath(filePath);
        std::error_code ec;
        {
            fs::path bakPath = jcwfPath;
            bakPath += ".bak";
            fs::copy_file(jcwfPath, bakPath, fs::copy_options::overwrite_existing, ec);
        }

        std::string writeError;
        if (!WriteJcwfContent(filePath, response, writeError))
            return {"jcwf_fix_task", false, "Write failed: " + writeError};

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

        // Path validation:
        //   1. Reject absolute paths (scripts are project-relative).
        //   2. Canonicalise so symlinks and `.`/`..` segments collapse.
        //   3. Assert the resolved path lies under canonical scripts/.
        //   4. Run through IsPathDenied (catches `.bak` / `.tmp` and any
        //      sensitive base that lands under scripts/ via a crafted path).

        if (fs::path(path).is_absolute())
            return {"jcwf_write_script", false, "Absolute paths not allowed."};

        std::error_code ecValidate;
        fs::path const projectRoot = fs::weakly_canonical(fs::current_path(ecValidate), ecValidate);
        if (ecValidate)
            return {"jcwf_write_script", false, "Cannot resolve project root."};

        fs::path const scriptPath = fs::weakly_canonical(projectRoot / fs::path(path), ecValidate);
        if (ecValidate)
            return {"jcwf_write_script", false, "Cannot resolve script path: " + ecValidate.message()};

        fs::path const scriptsRoot = fs::weakly_canonical(projectRoot / "scripts", ecValidate);
        if (ecValidate)
            return {"jcwf_write_script", false, "Cannot resolve scripts/ root."};

        fs::path const relUnderScripts = scriptPath.lexically_relative(scriptsRoot);
        std::string const relUnderScriptsStr = relUnderScripts.string();
        if (relUnderScriptsStr.empty() || relUnderScriptsStr.rfind("..", 0) == 0)
            return {"jcwf_write_script", false, "Script path must resolve under scripts/. Got: " + path};

        if (IsPathDenied(scriptPath.string()))
            return {"jcwf_write_script", false, "Access denied: " + path + " (sensitive file or extension)"};

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
                action + ": scripts/" + relUnderScriptsStr + " (" + std::to_string(content.size()) + " bytes, " + type +
                    ")"};
    }

} // namespace AIAssistant
