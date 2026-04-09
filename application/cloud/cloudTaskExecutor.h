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

#pragma once

#include "workflow/taskExecutor.h"
#include "cloud/cloudConnector.h"
#include "cloud/taskCancellationToken.h"

namespace AIAssistant
{
    class CloudConnectorRegistry;
    class CloudConnectionManager;

    // Base for task executors that operate on cloud connections.
    // The Execute() method resolves the connection and credentials automatically,
    // then delegates to ExecuteCloud() which subclasses implement.
    class ICloudTaskExecutor : public ITaskExecutor
    {
    public:
        ICloudTaskExecutor(CloudConnectorRegistry& connectorRegistry, CloudConnectionManager& connectionManager);
        virtual ~ICloudTaskExecutor() = default;

        bool Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                     TaskDef const& taskDefinition, TaskInstanceState& taskState) override;

    protected:
        // Subclasses implement this with the actual cloud operation.
        // The cancellationToken allows cooperative cancellation of long-running operations.
        virtual bool ExecuteCloud(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                  TaskDef const& taskDefinition, TaskInstanceState& taskState,
                                  CloudConnection const& connection, CloudCredentials const& credentials,
                                  TaskCancellationToken const& cancellationToken) = 0;

        CloudConnectorRegistry& m_ConnectorRegistry;
        CloudConnectionManager& m_ConnectionManager;
    };
} // namespace AIAssistant
