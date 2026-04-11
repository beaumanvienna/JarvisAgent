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

#include "cloud/cloudTaskExecutor.h"

namespace AIAssistant
{
    // Task executor for sending Slack messages via the Slack Web API.
    //
    // JCWF task type: "slack_message"
    //
    // Task params JSON keys:
    //   "connection"  — named CloudConnection (required)
    //   "channel"     — Slack channel name or ID (required, e.g. "#alerts" or "C01ABCDEF")
    //   "text"        — message text (required, supports template variables)
    class SlackCloudTaskExecutor : public ICloudTaskExecutor
    {
    public:
        using ICloudTaskExecutor::ICloudTaskExecutor;

    protected:
        bool ExecuteCloud(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                          TaskDef const& taskDefinition, TaskInstanceState& taskState,
                          CloudConnection const& connection, CloudCredentials const& credentials,
                          TaskCancellationToken const& cancellationToken) override;
    };
} // namespace AIAssistant
