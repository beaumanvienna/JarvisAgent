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

#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "core.h"
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

        // Depends_on existence + adjacency for DAG validation
        std::unordered_map<std::string, std::vector<std::string>> adjacency;
        adjacency.reserve(workflow.m_Tasks.size());

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

} // namespace AIAssistant
