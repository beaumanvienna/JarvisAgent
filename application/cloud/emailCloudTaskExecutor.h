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
    // Task executor for email operations via SMTP/IMAP using libcurl.
    //
    // JCWF task types: "email_send", "email_read"
    //
    // email_send params:
    //   "connection"    — named CloudConnection (required)
    //   "to"            — recipient email address(es), comma-separated (required)
    //   "subject"       — email subject (required)
    //   "body"          — email body text (required unless body_file is set)
    //   "body_file"     — path to file whose contents become the body (takes precedence over body)
    //   "cc"            — CC recipients, comma-separated (optional)
    //   "attachments"   — array of file paths relative to working directory (optional)
    //
    // email_read params:
    //   "connection"    — named CloudConnection with imap_host/imap_port (required)
    //   "folder"        — IMAP folder to read from (default: "INBOX")
    //   "max_messages"  — maximum messages to fetch (default: 10)
    //   "subject_filter" — only include messages whose subject contains this (optional)
    //
    // email_read outputs:
    //   emails_summary.json — array of fetched messages (uid, from, subject, date, body)
    //   response.json       — {ok, count, folder}
    class EmailCloudTaskExecutor : public ICloudTaskExecutor
    {
    public:
        using ICloudTaskExecutor::ICloudTaskExecutor;

    protected:
        bool ExecuteCloud(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                          TaskDef const& taskDefinition, TaskInstanceState& taskState,
                          CloudConnection const& connection, CloudCredentials const& credentials,
                          TaskCancellationToken const& cancellationToken) override;

    private:
        bool ExecuteEmailRead(WorkflowDefinition const& workflowDefinition, TaskDef const& taskDefinition,
                              TaskInstanceState& taskState, CloudConnection const& connection,
                              CloudCredentials const& credentials);
    };
} // namespace AIAssistant
