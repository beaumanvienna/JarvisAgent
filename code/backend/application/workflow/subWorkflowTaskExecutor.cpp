/* Copyright (c) 2026 JC Technolabs
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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "subWorkflowTaskExecutor.h"

#include <filesystem>

#include "engine.h"
#include "file/pathConfinement.h"
#include "workflowRegistry.h"
#include "workflowRuntimeManager.h"

namespace AIAssistant
{
    SubWorkflowTaskExecutor::SubWorkflowTaskExecutor(WorkflowRegistry const* workflowRegistry)
        : m_WorkflowRegistry(workflowRegistry)
    {
    }

    void SubWorkflowTaskExecutor::SetRuntimeManager(WorkflowRuntimeManager* runtimeManager)
    {
        m_RuntimeManager = runtimeManager;
    }

    bool SubWorkflowTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                          TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        if (m_RuntimeManager == nullptr)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "SubWorkflowTaskExecutor: runtime manager not set";
            LOG_APP_ERROR("SubWorkflowTaskExecutor: runtime manager not set (task '{}')", taskDefinition.m_Id);
            return false;
        }

        if (m_WorkflowRegistry == nullptr)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "SubWorkflowTaskExecutor: workflow registry not set";
            LOG_APP_ERROR("SubWorkflowTaskExecutor: workflow registry not set (task '{}')", taskDefinition.m_Id);
            return false;
        }

        if (taskDefinition.m_WorkflowFile.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "SubWorkflowTaskExecutor: workflow_file is empty";
            LOG_APP_ERROR("SubWorkflowTaskExecutor: workflow_file is empty (task '{}')", taskDefinition.m_Id);
            return false;
        }

        // Resolve the workflow file path relative to the parent workflow's directory.
        std::filesystem::path const workflowFilePath = [&]()
        {
            std::filesystem::path const raw(taskDefinition.m_WorkflowFile);
            if (raw.is_absolute())
            {
                return raw;
            }
            return std::filesystem::path(workflowDefinition.m_WorkflowFileDirectoryAbsolute) / raw;
        }();

        // Containment gate: m_WorkflowFile is JCWF-authored.  A hostile JCWF
        // could supply `..`-laced or absolute paths to reference workflows
        // outside the project tree.  ConfineUnderProjectRoot rejects with an
        // empty path on traversal/symlink-escape; fail-closed with ERROR.
        std::filesystem::path const confinedPath = ConfineUnderProjectRoot(workflowFilePath);
        if (confinedPath.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage =
                "SubWorkflowTaskExecutor: workflow_file '" + taskDefinition.m_WorkflowFile +
                "' does not resolve under project root";
            LOG_APP_ERROR("SubWorkflowTaskExecutor: workflow_file '{}' rejected — does not resolve under project "
                          "root (task '{}')",
                          taskDefinition.m_WorkflowFile, taskDefinition.m_Id);
            return false;
        }
        std::string const absolutePath = confinedPath.string();

        // Look up the child workflow in the registry by file path.
        std::optional<std::string> const childWorkflowId = m_WorkflowRegistry->TryGetWorkflowIdByFilePath(absolutePath);

        if (!childWorkflowId.has_value())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage =
                "SubWorkflowTaskExecutor: child workflow not found in registry for file '" + absolutePath + "'";
            LOG_APP_ERROR("SubWorkflowTaskExecutor: child workflow not found in registry for file '{}' (task '{}')",
                          absolutePath, taskDefinition.m_Id);
            return false;
        }

        // Enqueue the child workflow run.
        EnqueueRunResult const enqueueResult = m_RuntimeManager->EnqueueWorkflowRunAndGetRunId(childWorkflowId.value());

        if (enqueueResult.m_Status != EnqueueStatus::Ok)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "SubWorkflowTaskExecutor: failed to enqueue child workflow '" +
                                           childWorkflowId.value() + "': " + enqueueResult.m_Message;
            LOG_APP_ERROR("SubWorkflowTaskExecutor: failed to enqueue child workflow '{}' (task '{}'): {}",
                          childWorkflowId.value(), taskDefinition.m_Id, enqueueResult.m_Message);
            return false;
        }

        // Register the parent-child link so the runtime manager can propagate completion.
        m_RuntimeManager->RegisterSubWorkflowLink(enqueueResult.m_RunId, workflowRun.m_RunId,
                                                  taskState.m_TaskInstanceId);

        LOG_APP_INFO("SubWorkflowTaskExecutor: enqueued child workflow '{}' as run '{}' for parent task '{}'",
                     childWorkflowId.value(), enqueueResult.m_RunId, taskDefinition.m_Id);

        // Transition to WaitingExternal — the runtime manager will complete this task
        // when the child run finishes.
        taskState.m_State = TaskInstanceStateKind::WaitingExternal;
        taskState.m_LastErrorMessage.clear();
        taskState.m_CapturedStdout = "child_run_id=" + enqueueResult.m_RunId;

        return true;
    }

} // namespace AIAssistant
