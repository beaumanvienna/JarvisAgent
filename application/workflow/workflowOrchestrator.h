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

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <future>

#include "workflowTypes.h"

namespace AIAssistant
{
    class WorkflowRegistry;

    // -------------------------------------------------------------------------
    // WorkflowOrchestrator
    // -------------------------------------------------------------------------
    // Responsibilities:
    //  * Use WorkflowRegistry to look up WorkflowDefinition by id
    //  * Create WorkflowRun instances (ephemeral run state)
    //  * Perform dependency readiness checks based on depends_on
    //  * Perform Makefile-style freshness checks based on file_inputs / file_outputs
    //  * Dispatch task execution onto the ThreadPool
    //  * Maintain active workflow runs that can progress over time (tick-based)
    //  * Track last completed run per workflow for inspection (UI, tests)
    //
    // Critical for ai_call:
    //  * The orchestrator MUST NOT block main-thread event processing.
    //    Therefore: StartWorkflowRun() + Tick() instead of synchronous "run to completion".
    // -------------------------------------------------------------------------
    class WorkflowOrchestrator
    {
    public:
        static WorkflowOrchestrator& Get();

        void SetRegistry(WorkflowRegistry const* workflowRegistry);

        std::vector<std::string> GetWorkflowIds() const;

        // Non-blocking: starts a workflow run and returns the run id (empty string on failure).
        std::string StartWorkflowRun(std::string const& workflowId, std::string const& runId = std::string());

        // Progresses all active runs. This should be called periodically (e.g. from JarvisAgent::OnUpdate()).
        void Tick();

        // Backward compatibility: this now behaves like StartWorkflowRun and returns immediately.
        // It returns true if the run was successfully started.
        bool RunWorkflowOnce(std::string const& workflowId, std::string const& runId = std::string());

        bool TryGetLastRun(std::string const& workflowId, WorkflowRun& outRun) const;

        // Allows event-driven systems (AiRequestPool) to locate an in-flight run and mutate its task states.
        // Returns false if the run id is unknown or already completed.
        bool TryGetActiveRun(std::string const& runId, WorkflowRun*& outRun);

    private:
        WorkflowOrchestrator() = default;

        bool TickActiveRun(std::string const& activeRunId);

        bool IsTaskReady(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                         TaskDef const& taskDefinition) const;

        bool ExecuteTaskInstance(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                 TaskDef const& taskDefinition, std::string const& taskId, TaskInstanceState& taskState);

        std::string GenerateRunId(WorkflowDefinition const& workflowDefinition) const;

    private:
        struct InFlightTask
        {
            std::string m_TaskId;
            TaskInstanceState* m_TaskState = nullptr;
            std::future<bool> m_Future;
        };

        struct ActiveWorkflowRun
        {
            WorkflowDefinition m_WorkflowDefinition;
            WorkflowRun m_WorkflowRun;
            std::vector<InFlightTask> m_InFlightTasks;
        };

        WorkflowRegistry const* m_WorkflowRegistry{nullptr};

        // Map: workflow id -> last completed run for that workflow.
        std::unordered_map<std::string, WorkflowRun> m_LastRuns;

        // Map: run id -> active run state (tick-driven).
        std::unordered_map<std::string, ActiveWorkflowRun> m_ActiveRuns;
    };

} // namespace AIAssistant
