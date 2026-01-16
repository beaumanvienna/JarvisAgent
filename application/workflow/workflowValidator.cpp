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

#include <unordered_map>
#include <unordered_set>

namespace
{
    using namespace AIAssistant;

    bool IsValidId(std::string const& value)
    {
        if (value.empty())
        {
            return false;
        }

        for (char const ch : value)
        {
            bool const isAlphaNumeric = (ch >= 'a' && ch <= 'z')
                                       || (ch >= 'A' && ch <= 'Z')
                                       || (ch >= '0' && ch <= '9');
            bool const isAllowed = isAlphaNumeric || ch == '_' || ch == '-' || ch == '.';
            if (!isAllowed)
            {
                return false;
            }
        }

        return true;
    }

    void AddIssue(std::vector<WorkflowValidationIssue>& issues,
                  WorkflowValidationSeverity const severity,
                  std::string const& code,
                  std::string const& message,
                  std::string const& path,
                  std::string const& taskId = std::string())
    {
        WorkflowValidationIssue issue;
        issue.m_Severity = severity;
        issue.m_Code = code;
        issue.m_Message = message;
        issue.m_Path = path;
        issue.m_TaskId = taskId;
        issues.push_back(std::move(issue));
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
            AddIssue(issues, WorkflowValidationSeverity::Error, "missing_version",
                     "Missing required field: version", "$.version");
        }

        if (workflow.m_Id.empty())
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "missing_workflow_id",
                     "Missing required field: id", "$.id");
        }
        else if (!IsValidId(workflow.m_Id))
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "invalid_workflow_id",
                     "Workflow id contains invalid characters", "$.id");
        }

        if (workflow.m_Tasks.empty())
        {
            AddIssue(issues, WorkflowValidationSeverity::Error, "missing_tasks",
                     "Workflow has no tasks", "$.tasks");
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
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_task_id",
                         "Task id is empty", "$.tasks", taskId);
                continue;
            }

            if (!IsValidId(taskId))
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "invalid_task_id",
                         "Task id contains invalid characters", "$.tasks." + taskId, taskId);
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
                AddIssue(issues, WorkflowValidationSeverity::Error, "duplicate_task_id",
                         "Duplicate task id: " + taskId, "$.tasks." + taskId, taskId);
            }

            if (task.m_Type == TaskType::Unknown)
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_task_type",
                         "Task is missing required field: type", "$.tasks." + taskId + ".type", taskId);
            }

            if (task.m_WorkingDirectory.empty())
            {
                AddIssue(issues, WorkflowValidationSeverity::Error, "missing_working_directory",
                         "Task is missing required field: working_directory", "$.tasks." + taskId + ".working_directory",
                         taskId);
            }
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
                    AddIssue(issues, WorkflowValidationSeverity::Error, "empty_dependency",
                             "depends_on contains an empty task id",
                             "$.tasks." + taskId + ".depends_on[" + std::to_string(i) + "]", taskId);
                    continue;
                }

                if (workflow.m_Tasks.find(depId) == workflow.m_Tasks.end())
                {
                    AddIssue(issues, WorkflowValidationSeverity::Error, "missing_dependency",
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

            AddIssue(issues, WorkflowValidationSeverity::Error, "cycle_detected", message, "$.tasks");
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
