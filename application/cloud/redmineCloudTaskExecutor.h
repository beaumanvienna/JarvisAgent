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
    // Task executor for Redmine issue operations via the Redmine REST API.
    //
    // JCWF task type: "redmine_issue"
    //
    // Operations:
    //
    //   "list_issues" -- GET /issues.json (filtered to one project)
    //     Required params:
    //       "connection"          named CloudConnection
    //     Optional params (override the connection's default project):
    //       "project_identifier"  Redmine project identifier (e.g. "j9t-demo")
    //       "status"              "open" (default), "closed", "*"
    //       "limit"               max issues, default 25
    //     Output: writes the raw Redmine response body to response.json in the working directory.
    //
    //   "update_issue" -- PUT /issues/{id}.json
    //     Required params:
    //       "connection"          named CloudConnection
    //       "issue_id"            integer Redmine issue id
    //     Optional params (any combination):
    //       "notes"               inline comment text appended to the issue
    //       "notes_file"          read notes from file (relative to j9t cwd, trims trailing whitespace)
    //       "assigned_to_id"      inline numeric Redmine user id to assign the issue to
    //       "assigned_to_id_file" read assignee id from file (one integer per line)
    //     The two notes/assignee inputs combine into a single Redmine PUT call.
    class RedmineCloudTaskExecutor : public ICloudTaskExecutor
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
