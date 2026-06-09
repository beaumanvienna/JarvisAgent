/* Copyright (c) 2025 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
   OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "engine.h"
#include "core.h"
#include "file/pathConfinement.h"
#include "shellTaskExecutor.h"
#include "workflow/templateEngine.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <process.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "simdjson/simdjson.h"
#include "taskPathResolver.h"

namespace AIAssistant
{
    namespace
    {

        // Serializes captured-output execution.  Originally protected
        // process-wide `current_path` mutation; that mutation was retired in
        // favour of `cd` inside the spawned subshell, so today the lock acts
        // as a coarse one-shell-task-at-a-time gate that bounds concurrent
        // pressure on the system.  The per-task unique stderr filename below
        // means we no longer rely on this mutex for race avoidance — it
        // could be relaxed (e.g., per-working-directory) if shell-task
        // throughput becomes a bottleneck.
        std::mutex g_ShellTaskExecutorCurrentPathMutex;

        // Atomic counter providing per-task uniqueness for `.ja_stderr_tmp`
        // filenames.  Pre-fix: a single fixed `.ja_stderr_tmp` per working
        // directory races with concurrent shell tasks in the same dir AND
        // is symlink-baitable in multi-tenant deployments.  Post-fix: the
        // filename is `.ja_stderr_<pid>_<counter>.tmp` — process-PID +
        // monotonic counter is collision-free without needing filesystem
        // calls.  Caller cleans up the file on the success path.
        std::atomic<uint64_t> g_StderrTmpCounter{0};

        // RAII wrapper for popen-returned FILE*.  Pre-fix: an exception
        // anywhere in the captured-output read loop (std::string allocation,
        // logger throwing, etc.) leaves the popen'd FILE* + child process
        // un-reaped, which leaks until process exit.  Post-fix: pclose runs
        // on every exit path.  The deleter returns the exit status into a
        // captured int* on close so the caller can recover it.
        struct PipeCloser
        {
            int* m_ExitOut = nullptr;
            void operator()(FILE* p) const noexcept
            {
                if (p == nullptr) return;
#if defined(_WIN32)
                int const status = _pclose(p);
#else
                int const status = pclose(p);
#endif
                if (m_ExitOut != nullptr) *m_ExitOut = status;
            }
        };
        using PipePtr = std::unique_ptr<FILE, PipeCloser>;
#if defined(_WIN32)
        FILE* OpenPipe(char const* command, char const* mode) { return _popen(command, mode); }
#else
        FILE* OpenPipe(char const* command, char const* mode) { return popen(command, mode); }

        // ClosePipe is now the deleter inside PipeCloser; retained here as a
        // utility for any future non-RAII call site.  Currently unused
        // outside PipeCloser; if it stays unused, this can be removed.
        [[maybe_unused]] int ClosePipe(FILE* pipe)
        {
            int const status = pclose(pipe);
            if (status == -1)
            {
                return -1;
            }

            if (WIFEXITED(status))
            {
                return WEXITSTATUS(status);
            }

            if (WIFSIGNALED(status))
            {
                return 128 + WTERMSIG(status);
            }

            return status;
        }
#endif

        // ------------------------------------------------------------
        // Build a derived output-slot → value map for this task.
        //
        // Strategy:
        //   1) If outputs.size() == file_outputs.size(), zip them by index:
        //        outputSlotName[i] → file_outputs[i]
        //   2) For any remaining outputs, if an input with the same name exists
        //      in taskState.m_InputValues, use that.
        //
        // This provides a deterministic mapping for file-based workflows like
        // the make-example.jcwf test.
        // ------------------------------------------------------------
        void BuildOutputSlotMap(TaskDef const& taskDefinition, TaskInstanceState const& taskState,
                                std::unordered_map<std::string, std::string>& outputSlotMapOut)
        {
            outputSlotMapOut.clear();

            // 1) Zip outputs with file_outputs when sizes match
            if (!taskDefinition.m_FileOutputs.empty() &&
                taskDefinition.m_FileOutputs.size() == taskDefinition.m_Outputs.size())
            {
                size_t fileIndex = 0;
                for (auto const& outputPair : taskDefinition.m_Outputs)
                {
                    if (fileIndex < taskDefinition.m_FileOutputs.size())
                    {
                        outputSlotMapOut[outputPair.first] = taskDefinition.m_FileOutputs[fileIndex];
                    }

                    ++fileIndex;
                }
            }

            // 2) Fallback: use input with the same name
            for (auto const& outputPair : taskDefinition.m_Outputs)
            {
                std::string const& outputName = outputPair.first;

                if (outputSlotMapOut.contains(outputName))
                {
                    continue;
                }

                auto inputIterator = taskState.m_InputValues.find(outputName);
                if (inputIterator != taskState.m_InputValues.end())
                {
                    outputSlotMapOut[outputName] = inputIterator->second;
                }
            }
        }

        // ------------------------------------------------------------
        // Expand JCWF {{...}} templates inside a single argument string
        // using the shared TemplateEngine (strict mode).
        // ------------------------------------------------------------
        bool ExpandTemplatesStrict(std::string const& raw, TaskDef const& taskDefinition, TaskInstanceState const& taskState,
                                   std::string& expandedOut)
        {
            TemplateContext context;
            context.m_FileInputs = &taskDefinition.m_FileInputs;
            context.m_FileOutputs = &taskDefinition.m_FileOutputs;
            context.m_InputValues = &taskState.m_InputValues;
            context.m_EnvVariables = &taskDefinition.m_Environment.m_Variables;

            std::string errorMessage;
            if (!ExpandTemplate(raw, context, TemplateMode::Strict, expandedOut, errorMessage))
            {
                LOG_APP_ERROR("ShellTaskExecutor: Template expansion failed for argument '{}': {}", raw, errorMessage);
                return false;
            }

            return true;
        }

        // ------------------------------------------------------------
        // Build a command string from an argv-style vector for /bin/sh -c
        // (or PowerShell on Windows).
        //
        // Arguments are NOT assumed safe: every argument is single-quoted
        // before joining (QuoteForPosixShell / QuoteForPowerShell), so the
        // shell treats each as a literal regardless of contents.  This
        // single-quoting is the load-bearing defense against injection
        // through `args[]` — see JoinArgumentsForSystem below.
        // ------------------------------------------------------------
#if defined(_WIN32)
        std::string QuoteForPowerShell(std::string const& value)
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

        std::string QuoteForPosixShell(std::string const& value)
        {
            std::string quoted;
            quoted.reserve(value.size() + 2);

            quoted.push_back('\'');
            for (char const character : value)
            {
                if (character == '\'')
                {
                    quoted.append("'\\''");
                }
                else
                {
                    quoted.push_back(character);
                }
            }
            quoted.push_back('\'');
            return quoted;
        }

        std::string JoinArgumentsForSystem(std::vector<std::string> const& arguments)
        {
            // ALWAYS single-quote every argument before joining — this is
            // the load-bearing defense against shell injection through `args[]`.
            // Pre-fix only quoted whitespace-bearing args; a non-whitespace
            // arg containing `$(...)` slipped past unquoted and reached the
            // shell as command substitution.  Post-fix: every arg is wrapped
            // in single quotes via `QuoteForPosixShell` (which handles
            // embedded single quotes via `'\''`), so the shell treats it as
            // a literal regardless of contents.  Workflow-author globbing
            // expectation (`*.txt` etc.) is supported via the `command` field
            // (concatenated raw); `args[]` is for positional arguments and
            // SHOULD be literal.
            //
            // Used to build a command line for /bin/sh -c via popen().
            std::ostringstream stringStream;
            bool isFirst = true;
            for (std::string const& argument : arguments)
            {
                if (!isFirst)
                {
                    stringStream << " ";
                }

                stringStream << QuoteForPosixShell(argument);

                isFirst = false;
            }
            return stringStream.str();
        }

        // ------------------------------------------------------------
        // Scan raw args for the presence of any input/output macros.
        //
        // Used to implement Option B:
        //   - If no input macro is present, inject individual {{input[0]}}, {{input[1]}}, ...
        //   - If no output macro is present, append individual {{output[0]}}, {{output[1]}}, ...
        //
        // We inject indexed macros (not {{inputs}}/{{outputs}}) so that each file
        // becomes a separate positional argument. The plural forms join all files
        // into a single space-separated string, which gets quoted as one arg.
        // ------------------------------------------------------------
        void EnsureDefaultInputOutputArgs(std::vector<std::string>& rawArgs, size_t fileInputCount, size_t fileOutputCount)
        {
            bool hasInputMacro = false;
            bool hasOutputMacro = false;

            for (std::string const& argument : rawArgs)
            {
                if (argument.find("{{inputs}}") != std::string::npos || argument.find("{{input[") != std::string::npos)
                {
                    hasInputMacro = true;
                }

                if (argument.find("{{outputs}}") != std::string::npos || argument.find("{{output[") != std::string::npos)
                {
                    hasOutputMacro = true;
                }
            }

            if (!hasInputMacro)
            {
                // Insert individual {{input[N]}} at front (in reverse order to maintain 0,1,2... ordering)
                for (size_t i = fileInputCount; i > 0; --i)
                {
                    rawArgs.insert(rawArgs.begin(), "{{input[" + std::to_string(i - 1) + "]}}");
                }
            }

            if (!hasOutputMacro)
            {
                for (size_t i = 0; i < fileOutputCount; ++i)
                {
                    rawArgs.push_back("{{output[" + std::to_string(i) + "]}}");
                }
            }
        }

        // ------------------------------------------------------------
        // Execute a command while capturing stdout/stderr, and forward it
        // through the normal JarvisAgent logging path.
        //
        // This avoids external processes writing directly to the terminal
        // (which bypasses TerminalLogStreamBuf / ncurses and can corrupt the UI).
        // ------------------------------------------------------------
        bool ExecuteCommandWithCapturedOutput(std::string const& command, std::string const& taskId,
                                              std::filesystem::path const& workingDirectoryPath,
                                              std::vector<std::string> const& argumentList, int& exitCodeOut,
                                              std::string& stdoutOut, std::string& stderrOut)
        {
            std::scoped_lock<std::mutex> const lock(g_ShellTaskExecutorCurrentPathMutex);

            exitCodeOut = -1;
            stdoutOut.clear();
            stderrOut.clear();

            // Redirect stderr to a temp file so we can capture it separately.
            // Stdout comes through the pipe for live logging.
            // IMPORTANT:
            // Do NOT change the process-wide current_path; other threads (for example the file watcher)
            // rely on it. Instead, run the command inside a subshell that changes directory only for
            // the spawned shell process.
            // Per-task unique stderr temp filename (PID + monotonic counter).
            // Prevents two concurrent shell tasks in the same working
            // directory from clobbering each other's stderr capture, and
            // makes symlink-bait attacks futile because the filename isn't
            // predictable across invocations.
#if defined(_WIN32)
            uint64_t const pid = static_cast<uint64_t>(::_getpid());
#else
            uint64_t const pid = static_cast<uint64_t>(::getpid());
#endif
            uint64_t const counter = g_StderrTmpCounter.fetch_add(1, std::memory_order_relaxed);
            std::string const stderrFileName = ".ja_stderr_" + std::to_string(pid) + "_" +
                                                std::to_string(counter) + ".tmp";
            std::filesystem::path const stderrTmpPath = workingDirectoryPath / stderrFileName;

#if defined(_WIN32)
            std::string commandWithRedirect;
            if (ShellTaskExecutor::GetWindowsShell() == WindowsShell::PowerShell)
            {
                // PowerShell mode: build -Command "Set-Location 'dir'; & 'script.ps1' 'arg1' ..."
                // Stderr redirect is at the cmd.exe level (outside -Command "...") using double-quoted path.
                std::string psCommand;
                psCommand += "Set-Location " + QuoteForPowerShell(workingDirectoryPath.string()) + "; ";
                if (!argumentList.empty())
                {
                    psCommand += "& " + QuoteForPowerShell(argumentList[0]);
                    for (size_t i = 1; i < argumentList.size(); ++i)
                        psCommand += " " + QuoteForPowerShell(argumentList[i]);
                }
                commandWithRedirect =
                    "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                    "-Command \"" + psCommand + "\" 2>\"" + stderrTmpPath.string() + "\"";
            }
            else
            {
                // Bash mode: same as the original Windows path.
                std::string const stderrRedirect = " 2>" + QuoteForPosixShell(stderrTmpPath.string());
                std::string genericDir = workingDirectoryPath.generic_string();
                std::string genericCommand = command;
                std::replace(genericCommand.begin(), genericCommand.end(), '\\', '/');
                commandWithRedirect =
                    "bash -c \"cd " + QuoteForPosixShell(genericDir) + " && " + genericCommand + stderrRedirect + "\"";
            }
#else
            std::string const stderrRedirect = " 2>" + QuoteForPosixShell(stderrTmpPath.string());
            std::string const commandWithRedirect =
                "cd " + QuoteForPosixShell(workingDirectoryPath.string()) + " && " + command + stderrRedirect;
#endif

            // RAII pipe — pclose runs even if the read loop throws (logger
            // alloc, std::string growth, etc.).  Pre-fix code reached the
            // explicit ClosePipe only on the happy path; an exception in
            // between leaked the FILE* and the underlying child process
            // until process exit.
            int closeStatus = -1;
            PipePtr pipe(OpenPipe(commandWithRedirect.c_str(), "r"), PipeCloser{&closeStatus});
            if (!pipe)
            {
                LOG_APP_ERROR("ShellTaskExecutor: Failed to open pipe for command '{}' (task='{}')", command, taskId);
                return false;
            }

            std::string pending;
            pending.reserve(4096);

            char buffer[4096];
            while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe.get()) != nullptr)
            {
                stdoutOut.append(buffer);
                pending.append(buffer);

                for (;;)
                {
                    size_t const newlineIndex = pending.find('\n');
                    if (newlineIndex == std::string::npos)
                    {
                        break;
                    }

                    std::string line = pending.substr(0, newlineIndex);
                    pending.erase(0, newlineIndex + 1);

                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }

                    if (!line.empty())
                    {
                        LOG_APP_INFO("[shell:{}] {}", taskId, SanitizeUtf8(line));
                    }
                }
            }

            if (!pending.empty())
            {
                if (!pending.empty() && pending.back() == '\r')
                {
                    pending.pop_back();
                }

                if (!pending.empty())
                {
                    LOG_APP_INFO("[shell:{}] {}", taskId, SanitizeUtf8(pending));
                }
            }

            // Trigger the RAII close + pull the exit status into our local.
            pipe.reset();
            exitCodeOut = closeStatus;

            // Read stderr from the temp file
            {
                std::error_code ec;
                if (std::filesystem::exists(stderrTmpPath, ec))
                {
                    std::ifstream stderrFile(stderrTmpPath, std::ios::binary);
                    if (stderrFile.is_open())
                    {
                        std::ostringstream ss;
                        ss << stderrFile.rdbuf();
                        stderrOut = ss.str();
                    }
                    std::error_code rmEc;
                    std::filesystem::remove(stderrTmpPath, rmEc);
                    if (rmEc)
                    {
                        LOG_APP_WARN("ShellTaskExecutor: failed to remove stderr temp file '{}' task='{}': {}",
                                     stderrTmpPath.string(), taskId, rmEc.message());
                    }

                    // Log stderr lines
                    if (!stderrOut.empty())
                    {
                        std::istringstream stderrStream(stderrOut);
                        std::string line;
                        while (std::getline(stderrStream, line))
                        {
                            if (!line.empty() && line.back() == '\r')
                            {
                                line.pop_back();
                            }
                            if (!line.empty())
                            {
                                LOG_APP_INFO("[shell:{}:stderr] {}", taskId, SanitizeUtf8(line));
                            }
                        }
                    }
                }
            }

            return exitCodeOut >= 0;
        }
#if !defined(_WIN32)
        // ------------------------------------------------------------
        // Execute a command with inactivity watchdog using fork/exec/poll.
        //
        // - Stdout/stderr activity is an implicit heartbeat.
        // - The TaskWatchdog (if non-null) is also checked for explicit
        //   REST heartbeats from the task code.
        // - If no activity for inactivityTimeoutMs, the process group
        //   is killed and exitCodeOut is set to 124 (timeout).
        // - Environment variables JARVIS_PORT and JARVIS_TASK_ID are
        //   set in the child process for heartbeat API access.
        // ------------------------------------------------------------
        bool ExecuteCommandWithWatchdog(std::string const& command, std::string const& taskId,
                                        std::filesystem::path const& workingDirectoryPath, uint64_t inactivityTimeoutMs,
                                        TaskWatchdog* watchdog, int& exitCodeOut, std::string& stdoutOut,
                                        std::string& stderrOut)
        {
            exitCodeOut = -1;
            stdoutOut.clear();
            stderrOut.clear();

            int stdoutPipe[2];
            int stderrPipe[2];

            if (pipe(stdoutPipe) == -1)
            {
                LOG_APP_ERROR("ShellTaskExecutor: pipe() failed for task '{}' (stdout)", taskId);
                return false;
            }

            if (pipe(stderrPipe) == -1)
            {
                close(stdoutPipe[0]);
                close(stdoutPipe[1]);
                LOG_APP_ERROR("ShellTaskExecutor: pipe() failed for task '{}' (stderr)", taskId);
                return false;
            }

            pid_t const pid = fork();
            if (pid == -1)
            {
                close(stdoutPipe[0]);
                close(stdoutPipe[1]);
                close(stderrPipe[0]);
                close(stderrPipe[1]);
                LOG_APP_ERROR("ShellTaskExecutor: fork() failed for task '{}'", taskId);
                return false;
            }

            if (pid == 0)
            {
                // ── Child process ──────────────────────────────────────────
                setpgid(0, 0); // new process group for clean kill

                close(stdoutPipe[0]); // close read ends
                close(stderrPipe[0]);
                dup2(stdoutPipe[1], STDOUT_FILENO);
                dup2(stderrPipe[1], STDERR_FILENO);
                close(stdoutPipe[1]);
                close(stderrPipe[1]);

                // Set environment variables for heartbeat API.  Pull the port
                // from config so the child reaches the same j9t the parent is
                // running.  Pre-fix hardcoded "8080" — wrong for HTTPS-default
                // deployments (j9t lands on 8443) and discloses the assumed
                // topology.  Note: this branch runs in the forked child after
                // dup2/close — async-signal-safety would be ideal, but
                // setenv() is already non-async-safe and this path is in
                // service today; the config read is bounded and the snprintf
                // is fast.  Acceptable for the existing posture.
                uint16_t const childServerPort =
                    (Core::g_Core != nullptr && Core::g_Core->GetConfig().m_Port != 0)
                        ? Core::g_Core->GetConfig().m_Port
                        : static_cast<uint16_t>(8443);
                char childPortBuf[8] = {};
                std::snprintf(childPortBuf, sizeof(childPortBuf), "%u", childServerPort);
                setenv("JARVIS_PORT", childPortBuf, 1);
                setenv("JARVIS_TASK_ID", taskId.c_str(), 1);

                // Change to working directory and exec
                if (chdir(workingDirectoryPath.c_str()) != 0)
                {
                    _exit(127);
                }

                execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
                _exit(127); // exec failed
            }

            // ── Parent process ─────────────────────────────────────────
            close(stdoutPipe[1]); // close write ends
            close(stderrPipe[1]);

            struct pollfd pfds[2] = {};
            pfds[0].fd = stdoutPipe[0];
            pfds[0].events = POLLIN;
            pfds[1].fd = stderrPipe[0];
            pfds[1].events = POLLIN;

            // Poll interval: check every 500ms or at the timeout, whichever is smaller.
            int const pollIntervalMs = static_cast<int>(std::min(inactivityTimeoutMs, static_cast<uint64_t>(500)));

            std::string stdoutPending;
            std::string stderrPending;
            stdoutPending.reserve(4096);
            stderrPending.reserve(4096);
            char buffer[4096];
            bool timedOut = false;
            int openFds = 2;

            // Kick watchdog at start
            if (watchdog)
            {
                watchdog->Kick();
            }

            // Helper: drain a pipe fd into an accumulator and a pending-line buffer.
            auto drainPipe = [&buffer](int fd, std::string& accumulator, std::string& pending)
            {
                for (;;)
                {
                    ssize_t const n = read(fd, buffer, sizeof(buffer) - 1);
                    if (n <= 0)
                    {
                        break;
                    }
                    buffer[n] = '\0';
                    accumulator.append(buffer, static_cast<size_t>(n));
                    pending.append(buffer, static_cast<size_t>(n));
                }
            };

            // Helper: flush complete lines from a pending buffer and log them.
            auto flushLines = [&taskId](std::string& pending, char const* logTag)
            {
                for (;;)
                {
                    size_t const newlineIndex = pending.find('\n');
                    if (newlineIndex == std::string::npos)
                    {
                        break;
                    }

                    std::string line = pending.substr(0, newlineIndex);
                    pending.erase(0, newlineIndex + 1);

                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }

                    if (!line.empty())
                    {
                        LOG_APP_INFO("[shell:{}{}] {}", taskId, logTag, SanitizeUtf8(line));
                    }
                }
            };

            while (openFds > 0)
            {
                int const ret = poll(pfds, 2, pollIntervalMs);

                if (ret < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    break; // poll error
                }

                if (ret == 0)
                {
                    // No activity this interval — check watchdog
                    if (watchdog)
                    {
                        int64_t const inactiveMs = watchdog->ElapsedSinceLastKickMs();
                        if (static_cast<uint64_t>(inactiveMs) > inactivityTimeoutMs)
                        {
                            LOG_APP_ERROR("ShellTaskExecutor: Task '{}' inactivity timeout ({}ms idle, {}ms limit)", taskId,
                                          inactiveMs, inactivityTimeoutMs);
                            timedOut = true;
                            break;
                        }
                    }
                    continue;
                }

                bool anyActivity = false;

                // ── stdout ──
                if (pfds[0].fd >= 0 && (pfds[0].revents & POLLIN))
                {
                    ssize_t const n = read(stdoutPipe[0], buffer, sizeof(buffer) - 1);
                    if (n > 0)
                    {
                        anyActivity = true;
                        buffer[n] = '\0';
                        stdoutOut.append(buffer, static_cast<size_t>(n));
                        stdoutPending.append(buffer, static_cast<size_t>(n));
                        flushLines(stdoutPending, "");
                    }
                }
                if (pfds[0].fd >= 0 && (pfds[0].revents & (POLLHUP | POLLERR)))
                {
                    drainPipe(stdoutPipe[0], stdoutOut, stdoutPending);
                    flushLines(stdoutPending, "");
                    pfds[0].fd = -1;
                    openFds--;
                }

                // ── stderr ──
                if (pfds[1].fd >= 0 && (pfds[1].revents & POLLIN))
                {
                    ssize_t const n = read(stderrPipe[0], buffer, sizeof(buffer) - 1);
                    if (n > 0)
                    {
                        anyActivity = true;
                        buffer[n] = '\0';
                        stderrOut.append(buffer, static_cast<size_t>(n));
                        stderrPending.append(buffer, static_cast<size_t>(n));
                        flushLines(stderrPending, ":stderr");
                    }
                }
                if (pfds[1].fd >= 0 && (pfds[1].revents & (POLLHUP | POLLERR)))
                {
                    drainPipe(stderrPipe[0], stderrOut, stderrPending);
                    flushLines(stderrPending, ":stderr");
                    pfds[1].fd = -1;
                    openFds--;
                }

                if (anyActivity && watchdog)
                {
                    watchdog->Kick();
                }
            }

            // Flush any remaining partial lines.  Sanitise externally-sourced
            // bytes so malformed UTF-8 / control chars from the spawned command
            // don't reach the log/dashboard pipeline raw.
            if (!stdoutPending.empty())
            {
                if (stdoutPending.back() == '\r')
                {
                    stdoutPending.pop_back();
                }
                if (!stdoutPending.empty())
                {
                    LOG_APP_INFO("[shell:{}] {}", taskId, SanitizeUtf8(stdoutPending));
                }
            }
            if (!stderrPending.empty())
            {
                if (stderrPending.back() == '\r')
                {
                    stderrPending.pop_back();
                }
                if (!stderrPending.empty())
                {
                    LOG_APP_INFO("[shell:{}:stderr] {}", taskId, SanitizeUtf8(stderrPending));
                }
            }

            close(stdoutPipe[0]);
            close(stderrPipe[0]);

            if (timedOut)
            {
                // Kill the entire process group
                kill(-pid, SIGTERM);
                usleep(200000); // 200ms grace period
                kill(-pid, SIGKILL);
                waitpid(pid, nullptr, 0);
                exitCodeOut = 124; // conventional timeout exit code
                return true;
            }

            // Wait for child and extract exit code
            int status = 0;
            waitpid(pid, &status, 0);

            if (WIFEXITED(status))
            {
                exitCodeOut = WEXITSTATUS(status);
            }
            else if (WIFSIGNALED(status))
            {
                exitCodeOut = 128 + WTERMSIG(status);
            }
            else
            {
                exitCodeOut = -1;
            }

            return exitCodeOut >= 0;
        }
#endif // !_WIN32

    } // anonymous namespace

#if defined(_WIN32)
    std::atomic<WindowsShell> ShellTaskExecutor::s_WindowsShell{WindowsShell::PowerShell};

    void ShellTaskExecutor::ProbeWindowsShell(bool useBashConfig)
    {
        if (!useBashConfig)
        {
            s_WindowsShell.store(WindowsShell::PowerShell, std::memory_order_release);
            LOG_CORE_INFO("[shell] Windows shell: PowerShell (use_bash is false)");
            return;
        }

        // Probe PATH for bash
        int const result = std::system("bash --version >NUL 2>&1");
        if (result == 0)
        {
            s_WindowsShell.store(WindowsShell::Bash, std::memory_order_release);
            LOG_CORE_INFO("[shell] Windows shell: Bash (found on PATH)");
        }
        else
        {
            s_WindowsShell.store(WindowsShell::PowerShell, std::memory_order_release);
            LOG_CORE_WARN("[shell] Windows shell: PowerShell (use_bash is true but bash not found on PATH — falling back)");
        }
    }

    WindowsShell ShellTaskExecutor::GetWindowsShell()
    {
        return s_WindowsShell.load(std::memory_order_acquire);
    }
#endif

    bool ShellTaskExecutor::ValidateScriptPath(std::string const& path) const
    {
        // Enforce "scripts/" prefix to avoid arbitrary command execution.
        // Three checks:
        //   1. Raw path must start with "scripts/" (cheap reject of absolute
        //      or unrelated paths before any filesystem operation).
        //   2. After ConfineUnderProjectRoot — which runs `weakly_canonical`
        //      + lexically_relative containment + fail-closes on any error
        //      — the resolved absolute path must land under
        //      `<projectRoot>/scripts/`.  This is what catches a symlink
        //      `scripts/foo` that points at `/etc/passwd` AND a `..`-laced
        //      raw input that survives the prefix check (e.g.,
        //      `scripts/../etc/passwd` lexically-normalises but symlink
        //      semantics could differ; weakly_canonical resolves
        //      consistently).
        if (path.rfind("scripts/", 0) != 0)
        {
            return false;
        }

        std::filesystem::path const confined = ConfineUnderProjectRoot(path);
        if (confined.empty())
        {
            return false;
        }

        std::error_code ec;
        std::filesystem::path const projectRoot = std::filesystem::weakly_canonical(std::filesystem::current_path(ec), ec);
        if (ec || projectRoot.empty())
        {
            return false;
        }
        std::filesystem::path const scriptsRoot = projectRoot / "scripts";

        std::filesystem::path const rel = confined.lexically_relative(scriptsRoot);
        std::string const relStr = rel.string();
        if (relStr.empty() || relStr == ".." || relStr.rfind("..", 0) == 0)
        {
            return false;
        }
        return true;
    }

    bool ShellTaskExecutor::IsSafeArgument(std::string const& argument) const
    {
        // Defense in depth on top of the always-quote in JoinArgumentsForSystem.
        // The single-quote wrap there is the load-bearing protection; this
        // blocklist catches obviously-hostile bytes before they even reach
        // the quoter and serves as an early-fail gate that makes log lines
        // attributable to a specific arg rather than a shell quoting bug.
        //
        // Forbidden chars: standard shell control + globbing/expansion
        // operators that have no legitimate place in a literal positional
        // arg.  Glob-expansion patterns belong in the `command` field.
        for (char character : argument)
        {
            unsigned char const ch = static_cast<unsigned char>(character);

            if (std::iscntrl(ch) != 0)
            {
                return false;
            }

            switch (character)
            {
                case ';':
                case '&':
                case '|':
                case '>':
                case '<':
                case '\'':
                case '"':
                case '`':
                case '$':
                case '(':
                case ')':
                case '\\':
                    return false;
                default:
                    break;
            }
        }

        return true;
    }

    bool ShellTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                    TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        (void)workflowRun;

        std::filesystem::path const workflowBaseDirectoryPath =
            TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);

        std::filesystem::path const taskWorkingDirectoryPathAbsolute =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPath, taskDefinition.m_WorkingDirectory);

        LOG_APP_INFO(
            "[paths debug] debug reason=resolveTaskWorkingDirectory taskId='{}' taskType='shell' "
            "taskWorkingDirectoryRelative='{}' taskWorkingDirectoryAbsolute='{}' workflowBaseDirectoryAbsolute='{}'",
            taskDefinition.m_Id, taskDefinition.m_WorkingDirectory, taskWorkingDirectoryPathAbsolute.string(),
            workflowBaseDirectoryPath.string());

        if (taskWorkingDirectoryPathAbsolute.empty())
        {
            LOG_APP_ERROR(
                "ShellTaskExecutor: Task '{}' has empty working directory and workflow base directory could not be resolved",
                taskDefinition.m_Id);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Missing working directory";
            return false;
        }

        std::error_code createErrorCode;
        LOG_APP_INFO("[paths debug] debug reason=ensureWorkingDirectory taskId='{}' taskType='shell' "
                     "taskWorkingDirectoryAbsolute='{}'",
                     taskDefinition.m_Id, taskWorkingDirectoryPathAbsolute.string());

        std::error_code existsBeforeErrorCode;
        bool const existedBefore = std::filesystem::exists(taskWorkingDirectoryPathAbsolute, existsBeforeErrorCode);
        LOG_APP_INFO(
            "[folder creation debug] debug create_directories attempt path='{}' reason='shell task working_directory'",
            taskWorkingDirectoryPathAbsolute.string());
        std::filesystem::create_directories(taskWorkingDirectoryPathAbsolute, createErrorCode);

        if (createErrorCode)
        {
            LOG_APP_ERROR("[folder creation debug] debug create_directories failed path='{}' ec={} message='{}' "
                          "reason='shell task working_directory'",
                          taskWorkingDirectoryPathAbsolute.string(), createErrorCode.value(), createErrorCode.message());
        }
        else
        {
            std::error_code existsAfterErrorCode;
            bool const existsAfter = std::filesystem::exists(taskWorkingDirectoryPathAbsolute, existsAfterErrorCode);
            bool const created = (!existedBefore && existsAfter);
            LOG_APP_INFO("[folder creation debug] debug create_directories ok path='{}' created={}",
                         taskWorkingDirectoryPathAbsolute.string(), created);
        }

        if (createErrorCode)
        {
            LOG_APP_ERROR("ShellTaskExecutor: Failed to create working directory '{}' for task '{}': {}",
                          taskWorkingDirectoryPathAbsolute.string(), taskDefinition.m_Id, createErrorCode.message());
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Failed to create working directory";
            return false;
        }

        // Hard-error on missing sources (inputs).
        for (std::string const& inputPathString : taskDefinition.m_FileInputs)
        {
            std::filesystem::path inputPath(inputPathString);
            if (inputPath.is_relative())
            {
                inputPath = (taskWorkingDirectoryPathAbsolute / inputPath).lexically_normal();
            }
            else
            {
                inputPath = inputPath.lexically_normal();
            }

            if (!std::filesystem::exists(inputPath))
            {
                LOG_APP_ERROR("ShellTaskExecutor: Missing required input file '{}' for task '{}'", inputPath.string(),
                              taskDefinition.m_Id);
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: Missing required input file";
                return false;
            }
        }

        // Ensure output parent directories exist.
        for (std::string const& outputPathString : taskDefinition.m_FileOutputs)
        {
            std::filesystem::path outputPath(outputPathString);
            if (outputPath.is_relative())
            {
                outputPath = (taskWorkingDirectoryPathAbsolute / outputPath).lexically_normal();
            }
            else
            {
                outputPath = outputPath.lexically_normal();
            }

            std::filesystem::path parentPath = outputPath.parent_path();
            if (!parentPath.empty())
            {
                std::error_code outputError;
                std::filesystem::path const parentPathAbsolute = std::filesystem::absolute(parentPath).lexically_normal();

                LOG_APP_INFO("[paths debug] debug reason=writeOutputEnsureParentDirectory taskId='{}' taskType='shell' "
                             "outputParentDirectoryAbsolute='{}'",
                             taskDefinition.m_Id, parentPathAbsolute.string());

                std::error_code existsBeforeParentErrorCode;
                bool const parentExistedBefore = std::filesystem::exists(parentPathAbsolute, existsBeforeParentErrorCode);
                LOG_APP_INFO(
                    "[folder creation debug] debug create_directories attempt path='{}' reason='shell output parent'",
                    parentPathAbsolute.string());
                std::filesystem::create_directories(parentPathAbsolute, outputError);

                if (outputError)
                {
                    LOG_APP_ERROR("[folder creation debug] debug create_directories failed path='{}' ec={} message='{}' "
                                  "reason='shell output parent'",
                                  parentPathAbsolute.string(), outputError.value(), outputError.message());
                }
                else
                {
                    std::error_code existsAfterParentErrorCode;
                    bool const parentExistsAfter = std::filesystem::exists(parentPathAbsolute, existsAfterParentErrorCode);
                    bool const parentCreated = (!parentExistedBefore && parentExistsAfter);
                    LOG_APP_INFO("[folder creation debug] debug create_directories ok path='{}' created={}",
                                 parentPathAbsolute.string(), parentCreated);
                }
            }
        }

        LOG_APP_INFO("[shell] Executing shell task '{}'", taskDefinition.m_Id);

        // ------------------------------------------------------------
        // 1) Parse params JSON (simdjson::ondemand)
        // ------------------------------------------------------------
        if (taskDefinition.m_ParamsJson.empty())
        {
            LOG_APP_ERROR("ShellTaskExecutor: Missing params JSON for task '{}'", taskDefinition.m_Id);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Missing params JSON";
            return false;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(taskDefinition.m_ParamsJson);

        auto params = parser.iterate(padded);
        if (params.error() != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("ShellTaskExecutor: Invalid params JSON for task '{}': {}", taskDefinition.m_Id,
                          simdjson::error_message(params.error()));
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Invalid params JSON";
            return false;
        }

        // ------------------------------------------------------------
        // 2) Extract command path
        // ------------------------------------------------------------
        std::string commandPath;

        {
            auto commandField = params["command"];

            if (commandField.error() == simdjson::NO_SUCH_FIELD)
            {
                LOG_APP_ERROR("ShellTaskExecutor: Missing 'command' field in params for task '{}'", taskDefinition.m_Id);
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: Missing 'command' field";
                return false;
            }

            if (commandField.error() != simdjson::SUCCESS)
            {
                LOG_APP_ERROR("ShellTaskExecutor: Error accessing 'command' field in params for task '{}': {}",
                              taskDefinition.m_Id, simdjson::error_message(commandField.error()));
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: Invalid 'command' field";
                return false;
            }

            std::string_view commandView;
            auto commandGetError = commandField.get(commandView);
            if (commandGetError != simdjson::SUCCESS)
            {
                LOG_APP_ERROR("ShellTaskExecutor: Failed to read 'command' as string for task '{}': {}", taskDefinition.m_Id,
                              simdjson::error_message(commandGetError));
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: Invalid 'command' field";
                return false;
            }

            commandPath.assign(commandView);
        }

#if defined(_WIN32)
        // On Windows in PowerShell mode, transparently swap .sh scripts for a .ps1 sibling.
        if (s_WindowsShell == WindowsShell::PowerShell)
        {
            std::filesystem::path cmdFsPath(commandPath);
            if (cmdFsPath.extension() == ".sh")
            {
                std::filesystem::path ps1Relative = cmdFsPath;
                ps1Relative.replace_extension(".ps1");
                std::filesystem::path const launchCwd =
                    (Core::g_Core != nullptr) ? Core::g_Core->GetLaunchCWDAbsolute() : std::filesystem::path{};
                std::filesystem::path const ps1Absolute = (launchCwd / ps1Relative).lexically_normal();
                if (std::filesystem::exists(ps1Absolute))
                {
                    std::string const originalPath = commandPath;
                    commandPath = ps1Relative.generic_string();
                    LOG_APP_INFO("[shell] Windows PowerShell: '{}' resolved to sibling '{}'", originalPath, commandPath);
                }
                else
                {
                    LOG_APP_ERROR("ShellTaskExecutor: Script '{}' referenced but PowerShell is active and no .ps1 sibling "
                                  "exists. Add '{}' or set use_bash: true in config.json.",
                                  commandPath, ps1Relative.generic_string());
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    taskState.m_LastErrorMessage =
                        "ShellTaskExecutor: '" + commandPath +
                        "' requires bash but PowerShell is active (no .ps1 sibling found)";
                    return false;
                }
            }
        }
#endif

        if (!ValidateScriptPath(commandPath))
        {
            LOG_APP_ERROR(
                "ShellTaskExecutor: Script path '{}' rejected for task '{}' (must be inside the 'scripts/' directory)",
                commandPath, taskDefinition.m_Id);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage =
                "ShellTaskExecutor: Script path rejected (must be inside the 'scripts/' directory)";
            return false;
        }

        LOG_APP_INFO("[paths debug] debug reason=resolveShellCommandPath taskId='{}' taskType='shell' "
                     "commandPathRelative='{}' commandPathIsRelative='{}' launchCWDAbsolute='{}'",
                     taskDefinition.m_Id, commandPath, std::filesystem::path(commandPath).is_relative(),
                     Core::g_Core ? Core::g_Core->GetLaunchCWDAbsolute().string() : std::filesystem::path{}.string());
        std::filesystem::path const commandPathFilesystemPath(commandPath);
        if (commandPathFilesystemPath.is_relative())
        {
            std::filesystem::path const jarvisAgentWorkingDirectoryPath =
                (Core::g_Core != nullptr) ? Core::g_Core->GetLaunchCWDAbsolute() : std::filesystem::path{};
            commandPath = (jarvisAgentWorkingDirectoryPath / commandPathFilesystemPath).lexically_normal().string();
            LOG_APP_INFO("[paths debug] debug reason=resolveShellCommandPathResolved taskId='{}' taskType='shell' "
                         "commandPathAbsolute='{}'",
                         taskDefinition.m_Id, commandPath);
        }

        if (!std::filesystem::exists(std::filesystem::path(commandPath)))
        {
            LOG_APP_ERROR("ShellTaskExecutor: Missing required script file '{}' for task '{}'", commandPath,
                          taskDefinition.m_Id);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Missing required script file";
            return false;
        }

        // ------------------------------------------------------------
        // 3) Derive logical output values up front
        //
        // This ensures templates like {{output[0]}} and dataflow outputs
        // are consistent with file_outputs.
        // ------------------------------------------------------------
        std::unordered_map<std::string, std::string> derivedOutputs;
        BuildOutputSlotMap(taskDefinition, taskState, derivedOutputs);

        // ------------------------------------------------------------
        // 4) Collect raw args from JCWF, then apply Option B defaults
        //    (auto-prepend {{inputs}} / auto-append {{outputs}} if absent).
        // ------------------------------------------------------------
        std::vector<std::string> rawArgs;

        {
            auto argsField = params["args"];

            if (argsField.error() == simdjson::SUCCESS)
            {
                simdjson::ondemand::value argsValue = argsField.value();

                // Ensure it's an array
                auto typeResult = argsValue.type();
                if (typeResult.error() != simdjson::SUCCESS || typeResult.value() != simdjson::ondemand::json_type::array)
                {
                    LOG_APP_ERROR("ShellTaskExecutor: 'args' field must be an array for task '{}'", taskDefinition.m_Id);
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    taskState.m_LastErrorMessage = "ShellTaskExecutor: 'args' must be an array if present";
                    return false;
                }

                auto arrayResult = argsValue.get_array();
                if (arrayResult.error() != simdjson::SUCCESS)
                {
                    LOG_APP_ERROR("ShellTaskExecutor: Failed to read 'args' array for task '{}': {}", taskDefinition.m_Id,
                                  simdjson::error_message(arrayResult.error()));
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    taskState.m_LastErrorMessage = "ShellTaskExecutor: 'args' must be an array if present";
                    return false;
                }

                simdjson::ondemand::array argsArray = arrayResult.value();

                for (simdjson::ondemand::value entry : argsArray)
                {
                    std::string_view argView;
                    auto entryError = entry.get(argView);
                    if (entryError != simdjson::SUCCESS)
                    {
                        LOG_APP_ERROR("ShellTaskExecutor: Non-string value in 'args' array for task '{}': {}",
                                      taskDefinition.m_Id, simdjson::error_message(entryError));
                        taskState.m_State = TaskInstanceStateKind::Failed;
                        taskState.m_LastErrorMessage = "ShellTaskExecutor: Non-string value in 'args' array";
                        return false;
                    }

                    rawArgs.emplace_back(argView);
                }
            }
            else if (argsField.error() != simdjson::NO_SUCH_FIELD)
            {
                // 'args' exists but is malformed at the top level
                LOG_APP_ERROR("ShellTaskExecutor: Invalid 'args' field in params for task '{}': {}", taskDefinition.m_Id,
                              simdjson::error_message(argsField.error()));
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: Invalid 'args' field";
                return false;
            }
            // NO_SUCH_FIELD → no args → OK, just leave rawArgs empty
        }

        // Option B: inject default input/output macros if none are present.
        EnsureDefaultInputOutputArgs(rawArgs, taskDefinition.m_FileInputs.size(), taskDefinition.m_FileOutputs.size());

        // ------------------------------------------------------------
        // 5) Build argv-style list: [commandPath, expanded args...]
        // ------------------------------------------------------------
        std::vector<std::string> argumentList;
        argumentList.push_back(commandPath);

        for (std::string const& rawArgument : rawArgs)
        {
            std::string expandedArgument;
            if (!ExpandTemplatesStrict(rawArgument, taskDefinition, taskState, expandedArgument))
            {
                LOG_APP_ERROR("ShellTaskExecutor: Failed to expand argument template '{}' for task '{}'", rawArgument,
                              taskDefinition.m_Id);
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: Failed to expand argument template '" + rawArgument + "'";
                return false;
            }

            if (!IsSafeArgument(expandedArgument))
            {
                LOG_APP_ERROR("ShellTaskExecutor: Argument '{}' failed safety check for task '{}'", expandedArgument,
                              taskDefinition.m_Id);
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage =
                    "ShellTaskExecutor: Argument contains unsupported characters (safety check failed)";
                return false;
            }

            if (!expandedArgument.empty())
            {
                argumentList.push_back(expandedArgument);
            }
        }

        // ------------------------------------------------------------
        // 6) Execute while capturing stdout/stderr.
        //
        // We must not allow external commands to write directly to the terminal,
        // because that bypasses the std::cout → TerminalLogStreamBuf pipeline and
        // can corrupt the ncurses dashboard.
        // ------------------------------------------------------------
        std::string const fullCommand = JoinArgumentsForSystem(argumentList);

        LOG_APP_INFO("[paths debug] debug reason=spawnShellTask taskId='{}' taskType='shell' "
                     "taskWorkingDirectoryRelative='{}' taskWorkingDirectoryAbsolute='{}' command='{}'",
                     taskDefinition.m_Id, taskDefinition.m_WorkingDirectory, taskWorkingDirectoryPathAbsolute.string(),
                     fullCommand);

        for (std::string const& inputPathString : taskDefinition.m_FileInputs)
        {
            std::filesystem::path inputPath(inputPathString);
            std::filesystem::path const inputPathAbsolute =
                inputPath.empty()
                    ? inputPath
                    : (inputPath.is_relative() ? (taskWorkingDirectoryPathAbsolute / inputPath).lexically_normal()
                                               : inputPath.lexically_normal());
            LOG_APP_INFO("[paths debug] debug reason=spawnShellTaskInput taskId='{}' taskType='shell' "
                         "inputPathRelative='{}' inputPathAbsolute='{}'",
                         taskDefinition.m_Id, inputPathString, inputPathAbsolute.string());
        }

        for (std::string const& outputPathString : taskDefinition.m_FileOutputs)
        {
            std::filesystem::path outputPath(outputPathString);
            std::filesystem::path const outputPathAbsolute =
                outputPath.empty()
                    ? outputPath
                    : (outputPath.is_relative() ? (taskWorkingDirectoryPathAbsolute / outputPath).lexically_normal()
                                                : outputPath.lexically_normal());
            LOG_APP_INFO("[paths debug] debug reason=spawnShellTaskOutput taskId='{}' taskType='shell' "
                         "outputPathRelative='{}' outputPathAbsolute='{}'",
                         taskDefinition.m_Id, outputPathString, outputPathAbsolute.string());
        }
        LOG_APP_INFO("[shell] Command: {}", fullCommand);

        int exitCode = -1;
        bool executed = false;
        std::string capturedStdout;
        std::string capturedStderr;

#if !defined(_WIN32)
        // Use fork/exec/poll watchdog when timeout is configured (inactivity-based).
        if (taskDefinition.m_TimeoutMs > 0 && taskState.m_Watchdog)
        {
            LOG_APP_INFO("[shell] Task '{}' inactivity watchdog: {}ms", taskDefinition.m_Id, taskDefinition.m_TimeoutMs);
            executed = ExecuteCommandWithWatchdog(fullCommand, taskDefinition.m_Id, taskWorkingDirectoryPathAbsolute,
                                                  taskDefinition.m_TimeoutMs, taskState.m_Watchdog.get(), exitCode,
                                                  capturedStdout, capturedStderr);
        }
        else
#endif
        {
            executed = ExecuteCommandWithCapturedOutput(fullCommand, taskDefinition.m_Id, taskWorkingDirectoryPathAbsolute,
                                                        argumentList, exitCode, capturedStdout, capturedStderr);
        }

        // Sanitize at the external-byte boundary: child-process stdout/stderr can emit non-UTF-8
        // bytes (binary blobs, mojibake from C locale).  Downstream consumers — stdout.txt /
        // stderr.txt files, dashboard JSON, ncurses TUI — all assume well-formed UTF-8.
        capturedStdout = SanitizeUtf8(capturedStdout);
        capturedStderr = SanitizeUtf8(capturedStderr);

        // Write stdout.txt and stderr.txt to the task working directory (full size).
        // Stream good() check after write so a disk-full / quota-loss
        // mid-write surfaces as an ERROR log with task context instead of
        // silently producing a truncated file.
        {
            std::filesystem::path const stdoutPath = taskWorkingDirectoryPathAbsolute / "stdout.txt";
            std::filesystem::path const stderrPath = taskWorkingDirectoryPathAbsolute / "stderr.txt";
            std::error_code ec;
            if (!capturedStdout.empty())
            {
                try
                {
                    std::ofstream stdoutFile(stdoutPath, std::ios::binary | std::ios::trunc);
                    if (stdoutFile.is_open())
                    {
                        stdoutFile.exceptions(std::ios::failbit | std::ios::badbit);
                        stdoutFile.write(capturedStdout.data(), static_cast<std::streamsize>(capturedStdout.size()));
                    }
                }
                catch (std::exception const& e)
                {
                    LOG_APP_ERROR("ShellTaskExecutor: write to '{}' failed task='{}': {}", stdoutPath.string(),
                                  taskDefinition.m_Id, e.what());
                }
            }
            else
            {
                std::error_code rmEc;
                std::filesystem::remove(stdoutPath, rmEc);
                if (rmEc)
                {
                    LOG_APP_WARN("ShellTaskExecutor: failed to remove empty stdout.txt at '{}' task='{}': {}",
                                 stdoutPath.string(), taskDefinition.m_Id, rmEc.message());
                }
            }

            if (!capturedStderr.empty())
            {
                try
                {
                    std::ofstream stderrFile(stderrPath, std::ios::binary | std::ios::trunc);
                    if (stderrFile.is_open())
                    {
                        stderrFile.exceptions(std::ios::failbit | std::ios::badbit);
                        stderrFile.write(capturedStderr.data(), static_cast<std::streamsize>(capturedStderr.size()));
                    }
                }
                catch (std::exception const& e)
                {
                    LOG_APP_ERROR("ShellTaskExecutor: write to '{}' failed task='{}': {}", stderrPath.string(),
                                  taskDefinition.m_Id, e.what());
                }
            }
            else
            {
                std::error_code rmEc;
                std::filesystem::remove(stderrPath, rmEc);
                if (rmEc)
                {
                    LOG_APP_WARN("ShellTaskExecutor: failed to remove empty stderr.txt at '{}' task='{}': {}",
                                 stderrPath.string(), taskDefinition.m_Id, rmEc.message());
                }
            }
            (void)ec;
        }

        // Store first 1024 characters for the frontend tooltip.
        static constexpr size_t kMaxCaptureChars = 1024;
        taskState.m_CapturedStdout = TruncateUtf8Safe(capturedStdout, kMaxCaptureChars);
        taskState.m_CapturedStderr = TruncateUtf8Safe(capturedStderr, kMaxCaptureChars);

        LOG_APP_INFO("[paths debug] debug reason=shellTaskCompleted taskId='{}' taskType='shell' exitCode='{}' "
                     "taskWorkingDirectoryAbsolute='{}' command='{}'",
                     taskDefinition.m_Id, exitCode, taskWorkingDirectoryPathAbsolute.string(), fullCommand);

        if (!executed)
        {
            LOG_APP_ERROR("ShellTaskExecutor: Failed to execute shell command '{}' for task '{}'", fullCommand,
                          taskDefinition.m_Id);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Failed to execute shell command";
            return false;
        }

        if (exitCode == 124)
        {
            LOG_APP_ERROR("ShellTaskExecutor: Task '{}' timed out (inactivity > {}ms)", taskDefinition.m_Id,
                          taskDefinition.m_TimeoutMs);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage =
                "ShellTaskExecutor: Task timed out (inactivity > " + std::to_string(taskDefinition.m_TimeoutMs) + "ms)";
            return false;
        }

        if (exitCode != 0)
        {
            LOG_APP_ERROR("ShellTaskExecutor: Shell command '{}' for task '{}' returned non-zero exit status {}",
                          fullCommand, taskDefinition.m_Id, exitCode);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Shell command returned non-zero exit status";
            return false;
        }

        // ------------------------------------------------------------
        // 7) Populate taskState.m_OutputValues for downstream dataflow
        // ------------------------------------------------------------
        bool const outputsMapToFiles =
            !taskDefinition.m_FileOutputs.empty() && taskDefinition.m_FileOutputs.size() == taskDefinition.m_Outputs.size();

        for (auto const& outputPair : derivedOutputs)
        {
            if (!outputsMapToFiles)
            {
                taskState.m_OutputValues[outputPair.first] = outputPair.second;
                continue;
            }

            std::filesystem::path outputPath(outputPair.second);
            if (outputPath.is_relative())
            {
                outputPath = (taskWorkingDirectoryPathAbsolute / outputPath).lexically_normal();
            }
            taskState.m_OutputValues[outputPair.first] = outputPath.string();
        }

        taskState.m_State = TaskInstanceStateKind::Succeeded;
        return true;
    }

} // namespace AIAssistant