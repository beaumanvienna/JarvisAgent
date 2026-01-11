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
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#include "taskExecutorRegistry.h"

#include "engine.h"

namespace AIAssistant
{
    TaskExecutorRegistry& TaskExecutorRegistry::Get()
    {
        static TaskExecutorRegistry instance;
        return instance;
    }

    void TaskExecutorRegistry::RegisterExecutor(TaskType type, std::shared_ptr<ITaskExecutor> const& executorPtr)
    {
        m_Executors[type] = executorPtr;
    }

    bool TaskExecutorRegistry::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                       TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        auto iterator = m_Executors.find(taskDefinition.m_Type);

        if (iterator == m_Executors.end())
        {
            LOG_APP_ERROR("TaskExecutorRegistry: No executor registered for task type {}",
                          static_cast<int>(taskDefinition.m_Type));

            taskState.m_LastErrorMessage = "No executor registered";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        return iterator->second->Execute(workflowDefinition, workflowRun, taskDefinition, taskState);
    }

} // namespace AIAssistant