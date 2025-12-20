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
#include "shellTaskExecutor.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <filesystem>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#endif

#include "simdjson/simdjson.h"

namespace AIAssistant
{
    namespace
    {

        std::mutex g_ShellTaskExecutorCurrentPathMutex;

        
        std::filesystem::path const g_JarvisAgentLaunchWorkingDirectoryPath = std::filesystem::current_path().lexically_normal();
class ScopedCurrentPath
        {
        public:
            explicit ScopedCurrentPath(std::filesystem::path const& newPath, std::error_code& errorCode)
            {
                m_OldPath = std::filesystem::current_path(errorCode);
                if (errorCode)
                {
                    return;
                }

                if (!newPath.empty())
                {
                    std::filesystem::current_path(newPath, errorCode);
                }
            }

            ~ScopedCurrentPath()
            {
                std::error_code ignoredErrorCode;
                if (!m_OldPath.empty())
                {
                    std::filesystem::current_path(m_OldPath, ignoredErrorCode);
                }
            }

        private:
            std::filesystem::path m_OldPath;
        };

#if defined(_WIN32)
        FILE* OpenPipe(char const* command, char const* mode) { return _popen(command, mode); }

        int ClosePipe(FILE* pipe) { return _pclose(pipe); }
#else
        FILE* OpenPipe(char const* command, char const* mode) { return popen(command, mode); }

        int ClosePipe(FILE* pipe)
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
        // the make_example.jcwf test.
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
        // Join a list of file paths into a single space-separated string.
        // Example: ["a.cpp","b.cpp"] → "a.cpp b.cpp"
        // (This matches Makefile-style variable expansion semantics.)
        // ------------------------------------------------------------
        std::string JoinFileList(std::vector<std::string> const& files)
        {
            std::string joined;

            for (size_t index = 0; index < files.size(); ++index)
            {
                joined += files[index];

                if (index + 1 < files.size())
                {
                    joined += " ";
                }
            }

            return joined;
        }

        // ------------------------------------------------------------
        // Expand JCWF templates inside a single argument string.
        //
        // Supported patterns:
        //   * ${inputs}        → space-separated list of file_inputs
        //   * ${outputs}       → space-separated list of file_outputs
        //   * ${input[N]}      → N-th file_input (0-based)
        //   * ${output[N]}     → N-th file_output (0-based)
        //   * ${slot.NAME}     → value from taskState.m_InputValues["NAME"]
        //   * ${env.NAME}      → value from taskDefinition.m_Environment.m_Variables["NAME"]
        //                        (empty string if not found)
        //
        // Returns false on:
        //   * malformed pattern (missing closing '}')
        //   * invalid index
        //   * unknown slot.NAME
        //
        // This keeps misconfigurations explicit.
        // ------------------------------------------------------------
        bool ExpandTemplatesStrict(std::string const& raw, TaskDef const& taskDefinition, TaskInstanceState const& taskState,
                                   std::string& expandedOut)
        {
            expandedOut.clear();

            size_t currentIndex = 0;

            while (currentIndex < raw.size())
            {
                size_t startIndex = raw.find("${", currentIndex);
                if (startIndex == std::string::npos)
                {
                    expandedOut += raw.substr(currentIndex);
                    break;
                }

                // Copy literal prefix.
                expandedOut += raw.substr(currentIndex, startIndex - currentIndex);

                size_t closeBraceIndex = raw.find('}', startIndex + 2);
                if (closeBraceIndex == std::string::npos)
                {
                    // Malformed template.
                    LOG_APP_ERROR("ShellTaskExecutor: Malformed template in argument '{}' (missing closing brace)", raw);
                    return false;
                }

                std::string key = raw.substr(startIndex + 2, closeBraceIndex - (startIndex + 2));
                std::string replacement;

                // ${inputs}
                if (key == "inputs")
                {
                    replacement = JoinFileList(taskDefinition.m_FileInputs);
                }
                // ${outputs}
                else if (key == "outputs")
                {
                    replacement = JoinFileList(taskDefinition.m_FileOutputs);
                }
                // ${input[N]}
                else if (key.rfind("input[", 0) == 0 && key.back() == ']')
                {
                    std::string indexString = key.substr(6, key.size() - 7);
                    try
                    {
                        size_t index = static_cast<size_t>(std::stoul(indexString));
                        if (index >= taskDefinition.m_FileInputs.size())
                        {
                            LOG_APP_ERROR("ShellTaskExecutor: input index {} out of range for '{}' in argument '{}'", index,
                                          key, raw);
                            return false;
                        }

                        replacement = taskDefinition.m_FileInputs[index];
                    }
                    catch (...)
                    {
                        LOG_APP_ERROR("ShellTaskExecutor: Failed to parse input index from '{}' in argument '{}'", key, raw);
                        return false;
                    }
                }
                // ${output[N]}
                else if (key.rfind("output[", 0) == 0 && key.back() == ']')
                {
                    std::string indexString = key.substr(7, key.size() - 8);
                    try
                    {
                        size_t index = static_cast<size_t>(std::stoul(indexString));
                        if (index >= taskDefinition.m_FileOutputs.size())
                        {
                            LOG_APP_ERROR("ShellTaskExecutor: output index {} out of range for '{}' in argument '{}'", index,
                                          key, raw);
                            return false;
                        }

                        replacement = taskDefinition.m_FileOutputs[index];
                    }
                    catch (...)
                    {
                        LOG_APP_ERROR("ShellTaskExecutor: Failed to parse output index from '{}' in argument '{}'", key,
                                      raw);
                        return false;
                    }
                }
                // ${slot.NAME}
                else if (key.rfind("slot.", 0) == 0)
                {
                    std::string slotName = key.substr(5);
                    auto iterator = taskState.m_InputValues.find(slotName);
                    if (iterator == taskState.m_InputValues.end())
                    {
                        LOG_APP_ERROR("ShellTaskExecutor: Unknown slot '{}' referenced in argument '{}'", slotName, raw);
                        return false;
                    }

                    replacement = iterator->second;
                }
                // ${env.NAME}
                else if (key.rfind("env.", 0) == 0)
                {
                    std::string envName = key.substr(4);
                    auto iterator = taskDefinition.m_Environment.m_Variables.find(envName);
                    if (iterator != taskDefinition.m_Environment.m_Variables.end())
                    {
                        replacement = iterator->second;
                    }
                    else
                    {
                        // Missing env variable → expand as empty string.
                        LOG_APP_WARN("ShellTaskExecutor: Environment variable '{}' not found for argument '{}'", envName,
                                     raw);
                        replacement.clear();
                    }
                }
                else
                {
                    // Unknown pattern.
                    LOG_APP_ERROR("ShellTaskExecutor: Unknown template pattern '{}' in argument '{}'", key, raw);
                    return false;
                }

                expandedOut += replacement;
                currentIndex = closeBraceIndex + 1;
            }

            return true;
        }

        // ------------------------------------------------------------
        // Build a command string from argv-style vector.
        //
        // For now we assume arguments are already validated as "safe".
        // We simply join them with spaces.
        // ------------------------------------------------------------
        std::string JoinArgumentsForSystem(std::vector<std::string> const& arguments)
        {
            std::string command;

            for (size_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex)
            {
                command += arguments[argumentIndex];

                if (argumentIndex + 1 < arguments.size())
                {
                    command += " ";
                }
            }

            return command;
        }

        // ------------------------------------------------------------
        // Scan raw args for the presence of any input/output macros.
        //
        // Used to implement Option B:
        //   - If no input macro is present, inject "${inputs}" at the front.
        //   - If no output macro is present, append "${outputs}".
        // ------------------------------------------------------------
        void EnsureDefaultInputOutputArgs(std::vector<std::string>& rawArgs)
        {
            bool hasInputMacro = false;
            bool hasOutputMacro = false;

            for (std::string const& argument : rawArgs)
            {
                if (argument.find("${inputs}") != std::string::npos || argument.find("${input[") != std::string::npos)
                {
                    hasInputMacro = true;
                }

                if (argument.find("${outputs}") != std::string::npos || argument.find("${output[") != std::string::npos)
                {
                    hasOutputMacro = true;
                }
            }

            if (!hasInputMacro)
            {
                rawArgs.insert(rawArgs.begin(), std::string("${inputs}"));
            }

            if (!hasOutputMacro)
            {
                rawArgs.push_back(std::string("${outputs}"));
            }
        }

        // ------------------------------------------------------------
        // Execute a command while capturing stdout/stderr, and forward it
        // through the normal JarvisAgent logging path.
        //
        // This avoids external processes writing directly to the terminal
        // (which bypasses TerminalLogStreamBuf / ncurses and can corrupt the UI).
        // ------------------------------------------------------------
        bool ExecuteCommandWithCapturedOutput(std::string const& command, std::string const& taskId, std::filesystem::path const& workingDirectoryPath, int& exitCodeOut)
        {
            std::scoped_lock<std::mutex> const lock(g_ShellTaskExecutorCurrentPathMutex);

            std::error_code errorCode;
            ScopedCurrentPath const scopedCurrentPath(workingDirectoryPath, errorCode);
            if (errorCode)
            {
                LOG_APP_ERROR("[shell:{}] Failed to set current_path to '{}': {}", taskId,
                              workingDirectoryPath.string(), errorCode.message());
                return false;
            }


            exitCodeOut = -1;

            // Redirect stderr into stdout so we capture both streams.
            std::string const commandWithRedirect = command + " 2>&1";

            FILE* pipe = OpenPipe(commandWithRedirect.c_str(), "r");
            if (pipe == nullptr)
            {
                LOG_APP_ERROR("ShellTaskExecutor: Failed to open pipe for command '{}' (task='{}')", command, taskId);
                return false;
            }

            std::string pending;
            pending.reserve(4096);

            char buffer[4096];
            while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr)
            {
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
                        LOG_APP_INFO("[shell:{}] {}", taskId, line);
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
                    LOG_APP_INFO("[shell:{}] {}", taskId, pending);
                }
            }

            exitCodeOut = ClosePipe(pipe);
            return exitCodeOut >= 0;
        }
    } // anonymous namespace

    bool ShellTaskExecutor::ValidateScriptPath(std::string const& path) const
    {
        // Enforce "scripts/" prefix to avoid arbitrary command execution.
        return path.rfind("scripts/", 0) == 0;
    }

    bool ShellTaskExecutor::IsSafeArgument(std::string const& argument) const
    {
        // Conservative safety check:
        //  * Allow typical filename / flag characters and spaces.
        //  * Forbid characters commonly used in shell injection.
        //
        // This is not a perfect sandbox, but combined with ValidateScriptPath
        // it strongly nudges workflows toward simple, safe commands.
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
        
        std::filesystem::path workflowBaseDirectoryPath(workflowDefinition.m_WorkflowBaseDirectory);
        if (workflowBaseDirectoryPath.empty())
        {
            std::filesystem::path const workflowFileDirectoryPath(workflowDefinition.m_WorkflowFileDirectory);
            if (!workflowFileDirectoryPath.empty())
            {
                workflowBaseDirectoryPath = workflowFileDirectoryPath;
            }
        }

        if (workflowBaseDirectoryPath.empty())
        {
            std::filesystem::path const workflowFilePath(workflowDefinition.m_WorkflowFilePath);
            if (!workflowFilePath.empty())
            {
                workflowBaseDirectoryPath = workflowFilePath.parent_path();
            }
        }

        workflowBaseDirectoryPath = workflowBaseDirectoryPath.lexically_normal();

        std::filesystem::path taskWorkingDirectoryPath(taskDefinition.m_WorkingDirectory);
        if (taskWorkingDirectoryPath.empty())
        {
            taskWorkingDirectoryPath = workflowBaseDirectoryPath;
        }
        else if (taskWorkingDirectoryPath.is_relative() && !workflowBaseDirectoryPath.empty())
        {
            taskWorkingDirectoryPath = (workflowBaseDirectoryPath / taskWorkingDirectoryPath).lexically_normal();
        }
        else
        {
            taskWorkingDirectoryPath = taskWorkingDirectoryPath.lexically_normal();
        }

        if (taskWorkingDirectoryPath.empty())
        {
            LOG_APP_ERROR("ShellTaskExecutor: Task '{}' has empty working directory and workflow base directory could not be resolved",
                          taskDefinition.m_Id);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Missing working directory";
            return false;
        }

        std::error_code createErrorCode;
        std::filesystem::create_directories(taskWorkingDirectoryPath, createErrorCode);
        if (createErrorCode)
        {
            LOG_APP_ERROR("ShellTaskExecutor: Failed to create working directory '{}' for task '{}': {}",
                          taskWorkingDirectoryPath.string(), taskDefinition.m_Id, createErrorCode.message());
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Failed to create working directory";
            return false;
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

        if (!ValidateScriptPath(commandPath))
        {
            LOG_APP_ERROR("ShellTaskExecutor: Script path '{}' rejected for task '{}' (must start with 'scripts/')",
                          commandPath, taskDefinition.m_Id);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Script path rejected (must start with 'scripts/')";
            return false;
        }

        std::filesystem::path const commandPathFilesystemPath(commandPath);
        if (commandPathFilesystemPath.is_relative())
        {
            std::filesystem::path const jarvisAgentWorkingDirectoryPath = g_JarvisAgentLaunchWorkingDirectoryPath;
            commandPath = (jarvisAgentWorkingDirectoryPath / commandPathFilesystemPath).lexically_normal().string();
        }

        // ------------------------------------------------------------
        // 3) Derive logical output values up front
        //
        // This ensures templates like ${output[0]} and dataflow outputs
        // are consistent with file_outputs.
        // ------------------------------------------------------------
        std::unordered_map<std::string, std::string> derivedOutputs;
        BuildOutputSlotMap(taskDefinition, taskState, derivedOutputs);

        // ------------------------------------------------------------
        // 4) Collect raw args from JCWF, then apply Option B defaults
        //    (auto-prepend ${inputs} / auto-append ${outputs} if absent).
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
        EnsureDefaultInputOutputArgs(rawArgs);

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

        LOG_APP_INFO("[shell] Command: {}", fullCommand);

        int exitCode = -1;
        bool const executed = ExecuteCommandWithCapturedOutput(fullCommand, taskDefinition.m_Id, taskWorkingDirectoryPath, exitCode);

        if (!executed)
        {
            LOG_APP_ERROR("ShellTaskExecutor: Failed to execute shell command '{}' for task '{}'", fullCommand,
                          taskDefinition.m_Id);
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Failed to execute shell command";
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
            !taskDefinition.m_FileOutputs.empty() &&
            taskDefinition.m_FileOutputs.size() == taskDefinition.m_Outputs.size();

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
                outputPath = (taskWorkingDirectoryPath / outputPath).lexically_normal();
            }
            taskState.m_OutputValues[outputPair.first] = outputPath.string();
        }

        taskState.m_State = TaskInstanceStateKind::Succeeded;
        return true;
    }

} // namespace AIAssistant
