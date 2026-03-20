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

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#include "workflowValidator.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "core.h"
#include "engine.h"
#include "file/scriptRegistry.h"
#include "workflow/taskPathResolver.h"
#include "workflow/workflowFileIndex.h"
#include "workflow/workflowTypes.h"

#include "simdjson/simdjson.h"

namespace
{
    using namespace AIAssistant;
    namespace fs = std::filesystem;

    WorkflowValidationTier DefaultTierForSeverity(WorkflowValidationSeverity const severity)
    {
        switch (severity)
        {
            case WorkflowValidationSeverity::Error:
                return WorkflowValidationTier::B;
            case WorkflowValidationSeverity::Warning:
                return WorkflowValidationTier::C;
            case WorkflowValidationSeverity::Info:
                return WorkflowValidationTier::D;
            default:
                return WorkflowValidationTier::B;
        }
    }

    bool IsValidId(std::string const& value)
    {
        if (value.empty())
        {
            return false;
        }

        for (char const ch : value)
        {
            bool const isAlphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
            bool const isAllowed = isAlphaNumeric || ch == '_' || ch == '-' || ch == '.';
            if (!isAllowed)
            {
                return false;
            }
        }

        return true;
    }

    void AddIssue(std::vector<WorkflowValidationIssue>& issues, WorkflowValidationSeverity const severity,
                  WorkflowValidationTier const tier, std::string const& code, std::string const& message,
                  std::string const& path, std::string const& taskId = std::string())
    {
        WorkflowValidationIssue issue;
        issue.m_Severity = severity;
        issue.m_Tier = tier;
        issue.m_Code = code;
        issue.m_Message = message;
        issue.m_Path = path;
        issue.m_TaskId = taskId;
        issues.push_back(std::move(issue));
    }

    void AddIssue(std::vector<WorkflowValidationIssue>& issues, WorkflowValidationSeverity const severity,
                  std::string const& code, std::string const& message, std::string const& path,
                  std::string const& taskId = std::string())
    {
        AddIssue(issues, severity, DefaultTierForSeverity(severity), code, message, path, taskId);
    }

    bool StartsWith(std::string const& value, std::string const& prefix) { return value.rfind(prefix, 0) == 0; }

    bool ContainsPathTraversal(std::string const& path)
    {
        // Conservative check: block any ".." segment.
        // We do not normalize here because params.command is a runtime-policy path.
        return path.find("..") != std::string::npos;
    }

    bool TryGetParamsString(std::string const& paramsJson, char const* fieldName, std::string& outValue)
    {
        outValue.clear();

        if (paramsJson.empty())
        {
            return false;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(paramsJson);
        auto doc = parser.iterate(padded);
        if (doc.error() != simdjson::SUCCESS)
        {
            return false;
        }

        auto field = doc[fieldName].get_string();
        if (field.error() != simdjson::SUCCESS)
        {
            return false;
        }

        outValue = std::string(field.value());
        return true;
    }

    void ValidateTrigger(std::vector<WorkflowValidationIssue>& issues, WorkflowTrigger const& trigger, size_t const index)
    {
        std::string const basePath = "$.triggers[" + std::to_string(index) + "]";

        if (trigger.m_Type == WorkflowTriggerType::Unknown)
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "invalid_trigger_type", "Trigger has unknown type",
                     basePath + ".type");
        }

        if (trigger.m_Id.empty())
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "missing_trigger_id", "Trigger missing required field: id",
                     basePath + ".id");
        }
        else if (!IsValidId(trigger.m_Id))
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "invalid_trigger_id",
                     "Trigger id contains invalid characters", basePath + ".id");
        }

        // Best-effort params checks.
        if ((trigger.m_Type == WorkflowTriggerType::Cron) && !trigger.m_ParamsJson.empty())
        {
            std::string expression;
            if (!TryGetParamsString(trigger.m_ParamsJson, "expression", expression) || expression.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_cron_expression",
                         "Cron trigger params missing required field: expression", basePath + ".params.expression");
            }
        }

        if ((trigger.m_Type == WorkflowTriggerType::FileWatch) && !trigger.m_ParamsJson.empty())
        {
            std::string path;
            if (!TryGetParamsString(trigger.m_ParamsJson, "path", path) || path.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_file_watch_path",
                         "file_watch trigger params missing required field: path", basePath + ".params.path");
            }
        }
    }

    void ValidateTaskParams(std::vector<WorkflowValidationIssue>& issues, WorkflowDefinition const& workflow,
                            std::string const& taskId, TaskDef const& task)
    {
        std::string const taskPath = "$.tasks." + taskId;

        if (task.m_Type == TaskType::Shell)
        {
            if (task.m_ParamsJson.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_shell_params",
                         "Shell task is missing required field: params", taskPath + ".params", taskId);
                return;
            }

            std::string command;
            if (!TryGetParamsString(task.m_ParamsJson, "command", command) || command.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_shell_command",
                         "Shell task params missing required field: command", taskPath + ".params.command", taskId);
                return;
            }

            // Runtime policy: shell commands must be under scripts/
            bool const commandIsInScripts = StartsWith(command, "scripts/");

            if (!commandIsInScripts)
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "shell_command_policy",
                         "Shell task params.command must start with 'scripts/'", taskPath + ".params.command", taskId);
            }

            bool const hasTraversal = ContainsPathTraversal(command);
            if (hasTraversal)
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "shell_command_path_traversal",
                         "Shell task params.command must not contain '..'", taskPath + ".params.command", taskId);
            }

            // Feasibility: verify script exists (best-effort)
            if (commandIsInScripts && !hasTraversal && (Core::g_Core != nullptr))
            {
                fs::path const launchCwd = Core::g_Core->GetLaunchCWDAbsolute();
                if (!launchCwd.empty())
                {
                    fs::path const scriptPath = (launchCwd / fs::path(command)).lexically_normal();
                    std::error_code ec;
                    if (!fs::exists(scriptPath, ec))
                    {
                        AddIssue(issues, WorkflowValidationSeverity::Warning, "shell_command_not_found",
                                 "Shell task script not found: " + scriptPath.string(), taskPath + ".params.command",
                                 taskId);
                    }
                }
            }
        }
        else if (task.m_Type == TaskType::Python)
        {
            if (task.m_ParamsJson.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_python_params",
                         "Python task is missing required field: params (expected module/function)", taskPath + ".params",
                         taskId);
                return;
            }

            std::string module;
            std::string function;
            if (!TryGetParamsString(task.m_ParamsJson, "module", module) || module.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_python_module",
                         "Python task params missing required field: module", taskPath + ".params.module", taskId);
            }
            if (!TryGetParamsString(task.m_ParamsJson, "function", function) || function.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_python_function",
                         "Python task params missing required field: function", taskPath + ".params.function", taskId);
            }
        }
        else if (task.m_Type == TaskType::Internal)
        {
            if (task.m_ParamsJson.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_internal_params",
                         "Internal task params are empty (expected JSON with field 'action')", taskPath + ".params", taskId);
                return;
            }

            std::string action;
            if (!TryGetParamsString(task.m_ParamsJson, "action", action) || action.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_internal_action",
                         "Internal task params missing required field: action", taskPath + ".params.action", taskId);
            }
        }
        else if (task.m_Type == TaskType::AiCall)
        {
            // Best-effort: ai_call needs a PROB payload source.
            if (task.m_QueueBinding.m_ProbFiles.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_prob_files",
                         "ai_call task requires queue_binding.prob_files (non-empty)",
                         taskPath + ".queue_binding.prob_files", taskId);
            }
        }

        (void)workflow;
    }

    void ValidateDataflows(std::vector<WorkflowValidationIssue>& issues, WorkflowDefinition const& workflow)
    {
        std::unordered_set<std::string> boundInputs;

        std::unordered_set<std::string> toSlotKeys;

        for (size_t index = 0; index < workflow.m_Dataflows.size(); ++index)
        {
            DataflowDef const& df = workflow.m_Dataflows[index];
            std::string const basePath = "$.dataflow[" + std::to_string(index) + "]";

            auto fromTaskIt = workflow.m_Tasks.find(df.m_FromTask);
            if (fromTaskIt == workflow.m_Tasks.end())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_from_task",
                         "dataflow references unknown from_task: " + df.m_FromTask, basePath + ".from_task");
                continue;
            }

            auto toTaskIt = workflow.m_Tasks.find(df.m_ToTask);
            if (toTaskIt == workflow.m_Tasks.end())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_to_task",
                         "dataflow references unknown to_task: " + df.m_ToTask, basePath + ".to_task");
                continue;
            }

            TaskDef const& fromTask = fromTaskIt->second;
            TaskDef const& toTask = toTaskIt->second;

            if (!fromTask.m_Outputs.empty() && (fromTask.m_Outputs.find(df.m_FromOutput) == fromTask.m_Outputs.end()))
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_from_output",
                         "dataflow references unknown from_output: " + df.m_FromOutput, basePath + ".from_output");
            }

            if (!toTask.m_Inputs.empty() && (toTask.m_Inputs.find(df.m_ToInput) == toTask.m_Inputs.end()))
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_to_input",
                         "dataflow references unknown to_input: " + df.m_ToInput, basePath + ".to_input");
            }

            std::string const toKey = df.m_ToTask + "." + df.m_ToInput;
            if (toSlotKeys.find(toKey) != toSlotKeys.end())
            {
                AddIssue(issues, WorkflowValidationSeverity::Warning, "duplicate_dataflow_binding",
                         "Multiple dataflow entries bind the same destination input: " + toKey, basePath + ".to_input");
            }
            else
            {
                toSlotKeys.insert(toKey);
            }

            boundInputs.insert(toKey);

            // Type compatibility (best-effort)
            auto fromOutIt = fromTask.m_Outputs.find(df.m_FromOutput);
            auto toInIt = toTask.m_Inputs.find(df.m_ToInput);
            if (fromOutIt != fromTask.m_Outputs.end() && toInIt != toTask.m_Inputs.end())
            {
                std::string const& fromType = fromOutIt->second.m_Type;
                std::string const& toType = toInIt->second.m_Type;

                if (!fromType.empty() && !toType.empty() && fromType != toType)
                {
                    AddIssue(issues, WorkflowValidationSeverity::Warning, "dataflow_type_mismatch",
                             "dataflow type mismatch: " + fromType + " -> " + toType, basePath);
                }
            }
        }

        // Required inputs must be bound by dataflow (best-effort)
        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            for (auto const& [inputName, inputDef] : task.m_Inputs)
            {
                if (!inputDef.m_IsRequired)
                {
                    continue;
                }

                std::string const toKey = taskId + "." + inputName;
                if (boundInputs.find(toKey) == boundInputs.end())
                {
                    AddIssue(issues, WorkflowValidationSeverity::Warning, "required_input_unbound",
                             "Required input is not bound by dataflow: " + toKey,
                             "$.tasks." + taskId + ".inputs." + inputName, taskId);
                }
            }
        }
    }

    // Cycle detection in directed graph using DFS colors.
    bool DetectCycle(std::unordered_map<std::string, std::vector<std::string>> const& adjacency,
                     std::vector<std::string>& cycleOut)
    {
        enum class Color
        {
            White = 0,
            Gray = 1,
            Black = 2
        };

        std::unordered_map<std::string, Color> colors;
        colors.reserve(adjacency.size());
        for (auto const& [node, _] : adjacency)
        {
            colors[node] = Color::White;
        }

        std::vector<std::string> stack;
        std::unordered_map<std::string, size_t> stackIndex;

        std::function<bool(std::string const&)> dfs = [&](std::string const& node) -> bool
        {
            colors[node] = Color::Gray;
            stackIndex[node] = stack.size();
            stack.push_back(node);

            auto const it = adjacency.find(node);
            if (it != adjacency.end())
            {
                for (std::string const& next : it->second)
                {
                    auto const colorIt = colors.find(next);
                    if (colorIt == colors.end())
                    {
                        continue;
                    }

                    if (colorIt->second == Color::Gray)
                    {
                        // Found a back edge. Extract cycle nodes from stack.
                        size_t const start = stackIndex[next];
                        cycleOut.assign(stack.begin() + static_cast<long>(start), stack.end());
                        cycleOut.push_back(next);
                        return true;
                    }

                    if (colorIt->second == Color::White)
                    {
                        if (dfs(next))
                        {
                            return true;
                        }
                    }
                }
            }

            stack.pop_back();
            stackIndex.erase(node);
            colors[node] = Color::Black;
            return false;
        };

        for (auto const& [node, _] : adjacency)
        {
            if (colors[node] == Color::White)
            {
                if (dfs(node))
                {
                    return true;
                }
            }
        }

        return false;
    }
    // Extended AddIssue with suggested fix and context
    void AddIssueEx(std::vector<WorkflowValidationIssue>& issues, WorkflowValidationSeverity const severity,
                    WorkflowValidationTier const tier, std::string const& code, std::string const& message,
                    std::string const& path, std::string const& taskId, std::string const& suggestedFix,
                    std::string const& context = std::string())
    {
        WorkflowValidationIssue issue;
        issue.m_Severity = severity;
        issue.m_Tier = tier;
        issue.m_Code = code;
        issue.m_Message = message;
        issue.m_Path = path;
        issue.m_TaskId = taskId;
        issue.m_SuggestedFix = suggestedFix;
        issue.m_Context = context;
        issues.push_back(std::move(issue));
    }

    // Convert Python module path to relative file path: "scripts.parseLog" -> "scripts/parseLog.py"
    std::string ModuleToFilePath(std::string const& modulePath)
    {
        std::string filePath = modulePath;
        for (char& c : filePath)
        {
            if (c == '.')
            {
                c = '/';
            }
        }
        filePath += ".py";
        return filePath;
    }

    // Check if a generated-script list contains a given path (suffix match)
    bool IsInPendingScripts(std::vector<GeneratedScript> const* pendingScripts, std::string const& filePath)
    {
        if (pendingScripts == nullptr)
        {
            return false;
        }

        for (auto const& gs : *pendingScripts)
        {
            if (gs.path == filePath)
            {
                return true;
            }
            // Suffix match
            if (gs.path.size() >= filePath.size())
            {
                size_t offset = gs.path.size() - filePath.size();
                if (gs.path.compare(offset, filePath.size(), filePath) == 0 && (offset == 0 || gs.path[offset - 1] == '/'))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Check if a function name exists in a pending script's source content
    bool FunctionExistsInPendingScript(std::vector<GeneratedScript> const* pendingScripts, std::string const& filePath,
                                       std::string const& functionName)
    {
        if (pendingScripts == nullptr)
        {
            return false;
        }

        std::string const pattern = "def " + functionName + "(";
        for (auto const& gs : *pendingScripts)
        {
            bool match = (gs.path == filePath);
            if (!match && gs.path.size() >= filePath.size())
            {
                size_t offset = gs.path.size() - filePath.size();
                match =
                    gs.path.compare(offset, filePath.size(), filePath) == 0 && (offset == 0 || gs.path[offset - 1] == '/');
            }
            if (match && gs.content.find(pattern) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // ---- Tier B extended: python module policy, script+function existence ----
    void ValidatePythonScriptRegistry(std::vector<WorkflowValidationIssue>& issues, WorkflowDefinition const& workflow,
                                      ScriptRegistry const* scriptRegistry,
                                      std::vector<GeneratedScript> const* pendingScripts)
    {
        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            if (task.m_Type != TaskType::Python)
            {
                continue;
            }

            std::string const taskPath = "$.tasks." + taskId;

            std::string module;
            if (!TryGetParamsString(task.m_ParamsJson, "module", module) || module.empty())
            {
                continue; // Already reported by existing checks
            }

            std::string function;
            TryGetParamsString(task.m_ParamsJson, "function", function);

            // B-1: Module must start with "scripts."
            if (!StartsWith(module, "scripts."))
            {
                AddIssueEx(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::B, "python_module_policy",
                           "Python module '" + module + "' must start with 'scripts.'", taskPath + ".params.module", taskId,
                           "Change params.module to 'scripts." + module +
                               "' or move the script into the scripts/ directory.");
                continue;
            }

            std::string const scriptFilePath = ModuleToFilePath(module);

            // B-2: Script file exists (disk, registry, or pending)
            bool foundOnDisk = false;
            bool foundInRegistry = false;
            bool foundInPending = false;

            ScriptRegistryEntry const* registryEntry = nullptr;
            if (scriptRegistry != nullptr)
            {
                registryEntry = scriptRegistry->FindByModulePath(module);
                foundInRegistry = (registryEntry != nullptr);
            }

            if (!foundInRegistry)
            {
                if (Core::g_Core != nullptr)
                {
                    fs::path const launchCwd = Core::g_Core->GetLaunchCWDAbsolute();
                    if (!launchCwd.empty())
                    {
                        std::error_code ec;
                        foundOnDisk = fs::exists((launchCwd / scriptFilePath).lexically_normal(), ec);
                    }
                }
            }

            if (!foundInRegistry && !foundOnDisk)
            {
                foundInPending = IsInPendingScripts(pendingScripts, scriptFilePath);
            }

            if (!foundInRegistry && !foundOnDisk && !foundInPending)
            {
                AddIssueEx(issues, WorkflowValidationSeverity::Warning, WorkflowValidationTier::B, "python_script_not_found",
                           "Python script '" + scriptFilePath + "' not found on disk or in script registry",
                           taskPath + ".params.module", taskId,
                           "Ensure the file '" + scriptFilePath +
                               "' exists in the scripts/ directory with a @jarvis-script header.");
                continue; // Can't check function if script doesn't exist
            }

            // B-3: Function exists in script
            if (!function.empty())
            {
                bool functionFound = false;

                if (foundInRegistry && registryEntry != nullptr)
                {
                    auto const& funcs = registryEntry->m_ExportedFunctions;
                    functionFound = std::find(funcs.begin(), funcs.end(), function) != funcs.end();
                }

                if (!functionFound && (foundInPending || foundOnDisk))
                {
                    functionFound = FunctionExistsInPendingScript(pendingScripts, scriptFilePath, function);
                }

                if (!functionFound && foundInRegistry && registryEntry != nullptr)
                {
                    std::string availFuncs;
                    for (size_t i = 0; i < registryEntry->m_ExportedFunctions.size(); ++i)
                    {
                        if (i > 0)
                            availFuncs += ", ";
                        availFuncs += registryEntry->m_ExportedFunctions[i];
                    }

                    AddIssueEx(issues, WorkflowValidationSeverity::Warning, WorkflowValidationTier::B,
                               "python_function_not_found",
                               "Function '" + function + "' not found in script '" + scriptFilePath + "'",
                               taskPath + ".params.function", taskId,
                               "Change params.function to one of the available functions, or add 'def " + function +
                                   "(_context=None, **kwargs)' to the script.",
                               availFuncs.empty() ? "(no exported functions found)" : "Available functions: " + availFuncs);
                }
            }
        }
    }

    // ---- Tier B extended: ai_call STNG content check ----
    void ValidateAiCallStng(std::vector<WorkflowValidationIssue>& issues, WorkflowDefinition const& workflow)
    {
        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            if (task.m_Type != TaskType::AiCall)
            {
                continue;
            }

            std::string const taskPath = "$.tasks." + taskId;

            for (size_t i = 0; i < task.m_QueueBinding.m_StngFiles.size(); ++i)
            {
                auto const& stng = task.m_QueueBinding.m_StngFiles[i];
                if (!stng.m_Content.empty() && stng.m_Content.find("No markdown fences") == std::string::npos)
                {
                    AddIssueEx(issues, WorkflowValidationSeverity::Warning, WorkflowValidationTier::B,
                               "stng_missing_no_fences",
                               "ai_call STNG content should include 'No markdown fences, no explanations.'",
                               taskPath + ".queue_binding.stng_files[" + std::to_string(i) + "]", taskId,
                               "Add 'No markdown fences, no explanations.' to the STNG content because AI output is "
                               "consumed directly by tools.");
                }
            }
        }
    }

    // ---- Tier C: working_directory conventions ----
    void ValidateWorkingDirectoryConventions(std::vector<WorkflowValidationIssue>& issues,
                                             WorkflowDefinition const& workflow)
    {
        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            std::string const taskPath = "$.tasks." + taskId;

            if (task.m_WorkingDirectory.empty())
            {
                continue;
            }

            bool const startsWithQueue = StartsWith(task.m_WorkingDirectory, "../queue/");

            if (task.m_Type == TaskType::AiCall && !startsWithQueue)
            {
                AddIssueEx(issues, WorkflowValidationSeverity::Warning, WorkflowValidationTier::C,
                           "aicall_workdir_convention",
                           "ai_call working_directory should start with '../queue/' for queue watcher pickup",
                           taskPath + ".working_directory", taskId,
                           "Change working_directory to '../queue/" + workflow.m_Id + "/<NN>_" + taskId + "'.");
            }

            if (task.m_Type == TaskType::Python && startsWithQueue)
            {
                AddIssueEx(issues, WorkflowValidationSeverity::Info, WorkflowValidationTier::C, "python_workdir_no_queue",
                           "Python task working_directory should not be in ../queue/ (queue dirs are for ai_call tasks)",
                           taskPath + ".working_directory", taskId,
                           "Change working_directory to '" + workflow.m_Id + "/<NN>_" + taskId + "'.");
            }
        }
    }

    // ---- Tier C: file_inputs reachability ----
    // Build a map of task_id -> set of files that task produces (relative to workflow base).
    struct TaskOutputInfo
    {
        std::string workingDirectory;
        std::vector<std::string> fileOutputs;
    };

    std::unordered_map<std::string, TaskOutputInfo> BuildTaskOutputMap(WorkflowDefinition const& workflow)
    {
        std::unordered_map<std::string, TaskOutputInfo> result;
        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            TaskOutputInfo info;
            info.workingDirectory = task.m_WorkingDirectory;
            info.fileOutputs = task.m_FileOutputs;
            result[taskId] = std::move(info);
        }
        return result;
    }

    // Check if any upstream task (in depends_on chain) could produce the given file.
    // resolvedPath is relative to workflow base, expected to match workdir/file_output.
    // Both paths may contain ".." segments (e.g. ../workflows/X vs X) that are semantically
    // equivalent when resolved from the workflows/ base directory, so we resolve to absolute
    // before comparing.
    bool UpstreamProducesFile(WorkflowDefinition const& workflow,
                              std::unordered_map<std::string, TaskOutputInfo> const& outputMap, TaskDef const& task,
                              std::string const& resolvedPath)
    {
        // Determine the workflow base directory for absolute resolution.
        // Use the canonical resolver; fall back to <launchCWD>/workflows/ when the
        // WorkflowDefinition has no path fields populated (e.g. fresh JSON parse in aiJcwfService).
        fs::path workflowBase = TaskPathResolver::ResolveWorkflowBaseDirectory(workflow);
        if (workflowBase.empty() && Core::g_Core != nullptr)
        {
            workflowBase = Core::g_Core->GetLaunchCWDAbsolute() / "workflows";
        }

        fs::path const absResolvedPath =
            workflowBase.empty() ? fs::path(resolvedPath) : (workflowBase / resolvedPath).lexically_normal();

        for (std::string const& depId : task.m_DependsOn)
        {
            auto it = outputMap.find(depId);
            if (it == outputMap.end())
            {
                continue;
            }

            for (std::string const& output : it->second.fileOutputs)
            {
                // Construct the output's resolved path: depWorkDir / output
                std::string depOutputPath;
                if (!it->second.workingDirectory.empty())
                {
                    depOutputPath = (fs::path(it->second.workingDirectory) / output).lexically_normal().string();
                }
                else
                {
                    depOutputPath = output;
                }

                // First try simple string comparison (fast path)
                if (depOutputPath == resolvedPath)
                {
                    return true;
                }

                // Then try absolute comparison to handle ../workflows/X == X cases
                if (!workflowBase.empty())
                {
                    fs::path const absDepOutput = (workflowBase / depOutputPath).lexically_normal();
                    if (absDepOutput == absResolvedPath)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void ValidateFileInputReachability(std::vector<WorkflowValidationIssue>& issues, WorkflowDefinition const& workflow,
                                       WorkflowFileIndex const* workflowFileIndex)
    {
        auto const outputMap = BuildTaskOutputMap(workflow);

        // Resolve the workflow base directory (typically <launchCwd>/workflows)
        fs::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflow);
        if (workflowBaseDir.empty() && Core::g_Core != nullptr)
        {
            // Fallback: launchCwd / "workflows"
            workflowBaseDir = fs::path(Core::g_Core->GetLaunchCWDAbsolute()) / "workflows";
        }

        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            std::string const taskPath = "$.tasks." + taskId;

            for (size_t i = 0; i < task.m_FileInputs.size(); ++i)
            {
                std::string const& fileInput = task.m_FileInputs[i];
                std::string const inputPath = "$.tasks." + taskId + ".file_inputs[" + std::to_string(i) + "]";

                // Resolve relative to task working directory (which itself is relative to workflowBaseDir)
                std::string resolvedRelative;
                if (!task.m_WorkingDirectory.empty())
                {
                    resolvedRelative = (fs::path(task.m_WorkingDirectory) / fileInput).lexically_normal().string();
                }
                else
                {
                    resolvedRelative = fileInput;
                }

                // Check 1: does the file exist on disk? (best-effort)
                // Resolve against workflow base directory, not bare launchCwd
                bool existsOnDisk = false;
                fs::path absoluteCheckPath;
                if (!workflowBaseDir.empty())
                {
                    absoluteCheckPath = (workflowBaseDir / resolvedRelative).lexically_normal();
                    std::error_code ec;
                    existsOnDisk = fs::exists(absoluteCheckPath, ec);
                }

                LOG_APP_INFO("Validator::ValidateFileInputReachability: taskId='{}' file_inputs[{}]='{}' "
                             "resolvedRelative='{}' absoluteCheckPath='{}' existsOnDisk={}",
                             taskId, i, fileInput, resolvedRelative, absoluteCheckPath.string(),
                             existsOnDisk ? "true" : "false");

                if (existsOnDisk)
                {
                    continue;
                }

                // Check 2: does an upstream task produce this file?
                bool upstreamProduces = UpstreamProducesFile(workflow, outputMap, task, resolvedRelative);

                if (!upstreamProduces)
                {
                    // Detect doubled path: file_inputs[i] starts with working_directory prefix
                    std::string suggestedFix;
                    if (!task.m_WorkingDirectory.empty() && StartsWith(fileInput, task.m_WorkingDirectory))
                    {
                        // The file_input contains the working_directory prefix — strip it
                        std::string stripped = fileInput.substr(task.m_WorkingDirectory.size());
                        if (!stripped.empty() && stripped[0] == '/')
                        {
                            stripped = stripped.substr(1);
                        }
                        suggestedFix = "file_inputs values are relative to working_directory. "
                                       "Remove the working_directory prefix: change '" +
                                       fileInput + "' to '" + stripped + "'";
                    }
                    else if (workflowFileIndex != nullptr)
                    {
                        // Try to find the file by basename in the workflow file index
                        std::string const basename = fs::path(fileInput).filename().string();
                        auto matches = workflowFileIndex->FindByBasename(basename);
                        if (!matches.empty())
                        {
                            fs::path const rootDir = workflowFileIndex->GetRootDirectory();
                            fs::path const relMatch = matches[0].lexically_relative(rootDir);

                            // Compute the exact relative path from working_directory to the file
                            // so the fix AI doesn't have to do the directory-traversal math.
                            if (!task.m_WorkingDirectory.empty())
                            {
                                fs::path const correctRel = relMatch.lexically_relative(fs::path(task.m_WorkingDirectory));
                                suggestedFix = "File '" + basename + "' exists at '" + relMatch.generic_string() +
                                               "' (relative to workflows/). Change file_inputs[" + std::to_string(i) +
                                               "] to '" + correctRel.generic_string() + "'.";
                            }
                            else
                            {
                                suggestedFix = "File '" + basename + "' exists at '" + relMatch.generic_string() +
                                               "' (relative to workflows/). Change file_inputs[" + std::to_string(i) +
                                               "] to '" + relMatch.generic_string() + "'.";
                            }
                        }
                    }

                    if (suggestedFix.empty())
                    {
                        suggestedFix = "Ensure the file exists at '" + resolvedRelative +
                                       "' relative to workflow base, or add a depends_on task that produces it.";
                    }

                    AddIssueEx(issues, WorkflowValidationSeverity::Warning, WorkflowValidationTier::C,
                               "file_input_unreachable",
                               "file_inputs[" + std::to_string(i) + "] '" + fileInput +
                                   "' not found on disk and no upstream task produces it",
                               inputPath, taskId, suggestedFix, "Resolved path: " + resolvedRelative);
                }
            }
        }
    }

    // ---- Tier C: cntx_files string-path reachability ----
    void ValidateCntxFilesReachability(std::vector<WorkflowValidationIssue>& issues, WorkflowDefinition const& workflow,
                                       WorkflowFileIndex const* workflowFileIndex)
    {
        (void)workflowFileIndex; // reserved for future basename-lookup enhancements
        auto const outputMap = BuildTaskOutputMap(workflow);

        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            if (task.m_Type != TaskType::AiCall)
            {
                continue;
            }

            std::string const taskPath = "$.tasks." + taskId;

            for (size_t i = 0; i < task.m_QueueBinding.m_CntxFiles.size(); ++i)
            {
                auto const& cntxFile = task.m_QueueBinding.m_CntxFiles[i];

                // Only check string-path references (not inline objects with content)
                if (!cntxFile.m_Content.empty() || cntxFile.m_Path.empty())
                {
                    continue;
                }

                // cntx_files string paths are resolved relative to the ai_call's working_directory
                std::string const& cntxPath = cntxFile.m_Path;
                std::string resolvedRelative;
                if (!task.m_WorkingDirectory.empty())
                {
                    resolvedRelative = (fs::path(task.m_WorkingDirectory) / cntxPath).lexically_normal().string();
                }
                else
                {
                    resolvedRelative = cntxPath;
                }

                // Check if any upstream task produces this file
                bool upstreamProduces = UpstreamProducesFile(workflow, outputMap, task, resolvedRelative);

                if (!upstreamProduces)
                {
                    // Also check on disk (best-effort)
                    bool existsOnDisk = false;
                    if (Core::g_Core != nullptr)
                    {
                        fs::path const launchCwd = Core::g_Core->GetLaunchCWDAbsolute();
                        if (!launchCwd.empty())
                        {
                            std::error_code ec;
                            existsOnDisk = fs::exists((launchCwd / resolvedRelative).lexically_normal(), ec);
                        }
                    }

                    if (!existsOnDisk)
                    {
                        // Try to find the correct path by checking if an upstream task produces
                        // a file with the same basename — if so, compute the correct relative path.
                        std::string suggestedFix;
                        std::string const cntxBaseName = fs::path(cntxPath).filename().string();

                        for (std::string const& depId : task.m_DependsOn)
                        {
                            auto depIt = outputMap.find(depId);
                            if (depIt == outputMap.end())
                            {
                                continue;
                            }
                            for (std::string const& output : depIt->second.fileOutputs)
                            {
                                if (fs::path(output).filename().string() == cntxBaseName)
                                {
                                    // Found a matching output — compute correct path
                                    // Upstream output is at: workflows/<depWorkDir>/<output>
                                    // ai_call working_dir is: ../queue/<X>/<Y> → absolute: queue/<X>/<Y>
                                    // Correct path: ../../../workflows/<depWorkDir>/<output>
                                    std::string correctPath;
                                    if (!depIt->second.workingDirectory.empty())
                                    {
                                        correctPath = "../../../workflows/" + depIt->second.workingDirectory + "/" + output;
                                    }
                                    else
                                    {
                                        correctPath = "../../../workflows/" + output;
                                    }
                                    suggestedFix = "Change cntx_files path to '" + correctPath +
                                                   "' (3 levels up from queue/<X>/<Y> to root, then into workflows/)";
                                    break;
                                }
                            }
                            if (!suggestedFix.empty())
                            {
                                break;
                            }
                        }

                        if (suggestedFix.empty())
                        {
                            suggestedFix = "Verify the path is correct relative to the ai_call working_directory. "
                                           "To reach files in workflows/ from queue/<X>/<Y>, use "
                                           "'../../../workflows/<taskWorkDir>/<file>' (3 levels up to root).";
                        }

                        AddIssueEx(issues, WorkflowValidationSeverity::Warning, WorkflowValidationTier::C,
                                   "cntx_path_unreachable",
                                   "cntx_files[" + std::to_string(i) + "] path '" + cntxPath +
                                       "' not found on disk and no upstream task produces it",
                                   taskPath + ".queue_binding.cntx_files[" + std::to_string(i) + "]", taskId, suggestedFix,
                                   "Resolved path: " + resolvedRelative);
                    }
                }
            }
        }
    }

    // ---- Tier D: cross-task file chain validation ----
    void ValidateCrossTaskFileChains(std::vector<WorkflowValidationIssue>& issues, WorkflowDefinition const& workflow)
    {
        auto const outputMap = BuildTaskOutputMap(workflow);

        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            std::string const taskPath = "$.tasks." + taskId;

            // Check file_outputs don't collide with another task's file_outputs
            for (size_t i = 0; i < task.m_FileOutputs.size(); ++i)
            {
                std::string resolved;
                if (!task.m_WorkingDirectory.empty())
                {
                    resolved = (fs::path(task.m_WorkingDirectory) / task.m_FileOutputs[i]).lexically_normal().string();
                }
                else
                {
                    resolved = task.m_FileOutputs[i];
                }

                for (auto const& [otherId, otherTask] : workflow.m_Tasks)
                {
                    if (otherId == taskId)
                    {
                        continue;
                    }

                    for (std::string const& otherOutput : otherTask.m_FileOutputs)
                    {
                        std::string otherResolved;
                        if (!otherTask.m_WorkingDirectory.empty())
                        {
                            otherResolved =
                                (fs::path(otherTask.m_WorkingDirectory) / otherOutput).lexically_normal().string();
                        }
                        else
                        {
                            otherResolved = otherOutput;
                        }

                        if (resolved == otherResolved)
                        {
                            AddIssueEx(issues, WorkflowValidationSeverity::Warning, WorkflowValidationTier::D,
                                       "file_output_collision",
                                       "file_outputs[" + std::to_string(i) + "] '" + task.m_FileOutputs[i] +
                                           "' collides with task '" + otherId + "'",
                                       taskPath + ".file_outputs[" + std::to_string(i) + "]", taskId,
                                       "Use distinct output filenames or different working directories.");
                        }
                    }
                }
            }
        }
    }

} // namespace

namespace AIAssistant
{
    bool WorkflowValidator::HasErrors(std::vector<WorkflowValidationIssue> const& issues)
    {
        for (WorkflowValidationIssue const& issue : issues)
        {
            if (issue.m_Severity == WorkflowValidationSeverity::Error)
            {
                return true;
            }
        }

        return false;
    }

    void WorkflowValidator::Validate(WorkflowDefinition const& workflow, std::vector<WorkflowValidationIssue>& issues)
    {
        issues.clear();

        // ---- Tier 1: structural / JCWF ----

        if (workflow.m_Version.empty())
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "missing_version", "Missing required field: version",
                     "$.version");
        }

        if (workflow.m_Id.empty())
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "missing_workflow_id", "Missing required field: id", "$.id");
        }
        else if (!IsValidId(workflow.m_Id))
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "invalid_workflow_id",
                     "Workflow id contains invalid characters", "$.id");
        }

        if (workflow.m_Tasks.empty())
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "missing_tasks", "Workflow has no tasks", "$.tasks");
            return;
        }

        // Collect task ids
        std::unordered_set<std::string> taskIds;
        taskIds.reserve(workflow.m_Tasks.size());

        std::unordered_set<std::string> controlNodeIds;
        controlNodeIds.reserve(workflow.m_ControlNodes.size());

        for (auto const& node : workflow.m_ControlNodes)
        {
            if (node.m_Id.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_control_node_id", "Control node id is empty",
                         "$.control_nodes");
                continue;
            }

            if (!IsValidId(node.m_Id))
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "invalid_control_node_id",
                         "Control node id contains invalid characters", "$.control_nodes." + node.m_Id);
            }

            if (!controlNodeIds.insert(node.m_Id).second)
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "duplicate_control_node_id",
                         "Duplicate control node id: " + node.m_Id, "$.control_nodes." + node.m_Id);
            }

            if (node.m_Type == ControlNodeType::Unknown)
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "unknown_control_node_type",
                         "Control node has unknown type", "$.control_nodes." + node.m_Id + ".type");
            }
        }

        for (auto const& [taskKey, task] : workflow.m_Tasks)
        {
            std::string const& taskId = taskKey;

            if (taskId.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_task_id", "Task id is empty", "$.tasks",
                         taskId);
                continue;
            }

            if (!IsValidId(taskId))
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "invalid_task_id", "Task id contains invalid characters",
                         "$.tasks." + taskId, taskId);
            }

            if (task.m_Id.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Warning, "task_field_id_missing",
                         "Task object is missing field: id (using map key)", "$.tasks." + taskId + ".id", taskId);
            }
            else if (task.m_Id != taskId)
            {
                AddIssue(issues, WorkflowValidationSeverity::Warning, "task_id_mismatch",
                         "Task object id does not match tasks map key", "$.tasks." + taskId + ".id", taskId);
            }

            if (!taskIds.insert(taskId).second)
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "duplicate_task_id",
                         "Duplicate task id: " + taskId, "$.tasks." + taskId, taskId);
            }

            if (task.m_Type == TaskType::Unknown)
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_task_type",
                         "Task is missing required field: type", "$.tasks." + taskId + ".type", taskId);
            }

            // working_directory is optional in parsing; treat empty as info (defaults to workflow base dir).
            if (task.m_WorkingDirectory.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Info, "working_directory_missing",
                         "Task working_directory is not set (defaults to workflow base directory)",
                         "$.tasks." + taskId + ".working_directory", taskId);
            }

            // Task-type checks (params, queue bindings, etc.)
            ValidateTaskParams(issues, workflow, taskId, task);
        }

        // Cross-check: task ids must not collide with control_node ids (JC spec §3.8.1).
        for (auto const& id : taskIds)
        {
            if (controlNodeIds.count(id))
            {
                AddIssue(
                    issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "task_control_node_id_collision",
                    "Task id collides with control_node id: " + id + " — control nodes must not appear in the tasks map",
                    "$.tasks." + id, id);
            }
        }

        // Depends_on existence + adjacency for DAG validation (includes controlflow)
        std::unordered_map<std::string, std::vector<std::string>> adjacency;
        adjacency.reserve(workflow.m_Tasks.size() + workflow.m_ControlNodes.size());

        auto const nodeExists = [&](std::string const& id) -> bool {
            return (workflow.m_Tasks.find(id) != workflow.m_Tasks.end()) ||
                   (controlNodeIds.find(id) != controlNodeIds.end());
        };

        // Seed adjacency with all nodes so disconnected nodes participate in cycle detection.
        for (auto const& [taskId, _task] : workflow.m_Tasks)
        {
            adjacency[taskId];
        }
        for (auto const& cnId : controlNodeIds)
        {
            adjacency[cnId];
        }

        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            std::vector<std::string>& edges = adjacency[taskId];
            edges.reserve(task.m_DependsOn.size());

            for (size_t i = 0; i < task.m_DependsOn.size(); ++i)
            {
                std::string const& depId = task.m_DependsOn[i];
                if (depId.empty())
                {
                    AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "empty_dependency",
                             "depends_on contains an empty task id",
                             "$.tasks." + taskId + ".depends_on[" + std::to_string(i) + "]", taskId);
                    continue;
                }

                if (workflow.m_Tasks.find(depId) == workflow.m_Tasks.end())
                {
                    AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "missing_dependency",
                             "depends_on references unknown task id: " + depId,
                             "$.tasks." + taskId + ".depends_on[" + std::to_string(i) + "]", taskId);
                    continue;
                }

                // Edge direction: task depends on dep => dep -> task in adjacency for cycle detection
                adjacency[depId].push_back(taskId);
            }
        }

        // Validate controlflow edges + add them to adjacency for cycle detection.
        for (size_t i = 0; i < workflow.m_ControlflowEdges.size(); ++i)
        {
            ControlflowEdgeDef const& edge = workflow.m_ControlflowEdges[i];
            std::string const path = "$.controlflow[" + std::to_string(i) + "]";

            if (edge.m_From.empty() || edge.m_To.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A,
                         "controlflow_missing_endpoints", "controlflow edge missing 'from' or 'to'", path);
                continue;
            }

            if (!nodeExists(edge.m_From))
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "controlflow_from_missing",
                         "controlflow edge references unknown from node id: " + edge.m_From, path);
                continue;
            }

            if (!nodeExists(edge.m_To))
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "controlflow_to_missing",
                         "controlflow edge references unknown to node id: " + edge.m_To, path);
                continue;
            }

            if (edge.m_Kind == ControlflowKind::Unknown)
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "controlflow_kind_unknown",
                         "controlflow edge has unknown kind", path + ".kind");
                continue;
            }

            // Add to adjacency for cycle detection.
            adjacency[edge.m_From].push_back(edge.m_To);
        }

        std::vector<std::string> cycleNodes;
        if (DetectCycle(adjacency, cycleNodes))
        {
            std::string message = "Workflow graph contains a cycle: ";
            for (size_t i = 0; i < cycleNodes.size(); ++i)
            {
                message += cycleNodes[i];
                if (i + 1 < cycleNodes.size())
                {
                    message += " -> ";
                }
            }

            AddIssue(issues, WorkflowValidationSeverity::Error, WorkflowValidationTier::A, "cycle_detected", message,
                     "$.tasks");
        }

        // Dataflow checks (slot references and required input bindings)
        if (!workflow.m_Dataflows.empty())
        {
            ValidateDataflows(issues, workflow);
        }

        // Trigger checks
        for (size_t triggerIndex = 0; triggerIndex < workflow.m_Triggers.size(); ++triggerIndex)
        {
            ValidateTrigger(issues, workflow.m_Triggers[triggerIndex], triggerIndex);
        }

        // ---- Tier 2: runtime policy checks (best-effort) ----

        // Unique working_directory per task (policy from editor plan)
        std::unordered_map<std::string, std::string> workingDirToTaskId;
        for (auto const& [taskId, task] : workflow.m_Tasks)
        {
            if (task.m_WorkingDirectory.empty())
            {
                continue;
            }

            auto const it = workingDirToTaskId.find(task.m_WorkingDirectory);
            if (it != workingDirToTaskId.end() && it->second != taskId)
            {
                AddIssue(issues, WorkflowValidationSeverity::Warning, "duplicate_working_directory",
                         "Multiple tasks share the same working_directory: '" + task.m_WorkingDirectory + "'",
                         "$.tasks." + taskId + ".working_directory", taskId);
            }
            else
            {
                workingDirToTaskId[task.m_WorkingDirectory] = taskId;
            }
        }
    }

    void WorkflowValidator::Validate(WorkflowDefinition const& workflow, ScriptRegistry const* scriptRegistry,
                                     std::vector<GeneratedScript> const* pendingScripts,
                                     std::vector<WorkflowValidationIssue>& issues,
                                     WorkflowFileIndex const* workflowFileIndex)
    {
        // Run all existing checks (Tiers A + B baseline)
        Validate(workflow, issues);

        // ---- Tier B extended: python module policy, script+function existence ----
        ValidatePythonScriptRegistry(issues, workflow, scriptRegistry, pendingScripts);

        // ---- Tier B extended: ai_call STNG content check ----
        ValidateAiCallStng(issues, workflow);

        // ---- Tier C: working_directory conventions ----
        ValidateWorkingDirectoryConventions(issues, workflow);

        // ---- Tier C: file_inputs reachability ----
        ValidateFileInputReachability(issues, workflow, workflowFileIndex);

        // ---- Tier C: cntx_files string-path reachability ----
        ValidateCntxFilesReachability(issues, workflow, workflowFileIndex);

        // ---- Tier D: cross-task file chain (output collision) ----
        ValidateCrossTaskFileChains(issues, workflow);
    }

} // namespace AIAssistant
