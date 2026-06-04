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
    // Task executor for the Slack Web API. Handles two JCWF task types:
    //
    // 1. "slack_message" -- POST /api/chat.postMessage
    //    Params:
    //      "connection"      (required) named CloudConnection
    //      "channel"         (required) channel name or ID (e.g. "#alerts" or "C01ABCDEF")
    //      "text"            (required unless "text_file" set) message text
    //      "text_file"       (optional) read text from file instead (relative to j9t cwd)
    //      "thread_ts"       (optional) post as a threaded reply to this message ts
    //      "thread_ts_file"  (optional) read thread_ts from file (relative to j9t cwd)
    //
    // 2. "slack_read" -- GET /api/conversations.history
    //    Params:
    //      "connection"      (required) named CloudConnection
    //      "channel"         (required) channel ID (must be "C...", not "#name")
    //      "limit"           (optional, default 10) max messages
    //      "oldest"          (optional) only return messages with ts strictly greater than this
    //      "exclude_bots"    (optional, default true) filter out messages with a bot_id field
    //    Outputs written to the task working directory:
    //      messages_summary.json -- array of {ts, user, text}
    //      latest_message.txt    -- text of most recent message after filtering
    //      latest_ts.txt         -- ts of most recent message after filtering
    //      response.json         -- {ok, count, latest_ts}
    class SlackCloudTaskExecutor : public ICloudTaskExecutor
    {
    public:
        using ICloudTaskExecutor::ICloudTaskExecutor;

    protected:
        bool ExecuteCloud(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                          TaskDef const& taskDefinition, TaskInstanceState& taskState,
                          CloudConnection const& connection, CloudCredentials const& credentials,
                          TaskCancellationToken const& cancellationToken) override;

    private:
        bool ExecuteSlackPost(WorkflowDefinition const& workflowDefinition, TaskDef const& taskDefinition,
                              TaskInstanceState& taskState, CloudConnection const& connection,
                              CloudCredentials const& credentials);

        bool ExecuteSlackRead(WorkflowDefinition const& workflowDefinition, TaskDef const& taskDefinition,
                              TaskInstanceState& taskState, CloudConnection const& connection,
                              CloudCredentials const& credentials);
    };
} // namespace AIAssistant
