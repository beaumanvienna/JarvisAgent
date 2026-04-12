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
    // Task executor for Polarion write operations (update, create, attachment, linked items).
    //
    // JCWF task type: "polarion_write"
    //
    // Task params JSON keys:
    //   "connection"     — named CloudConnection (required)
    //   "operation"      — "update", "create", "upload_attachment", "download_attachment",
    //                      "linked_items" (required)
    //   "work_item_id"   — target work item ID (required for update/attachment/linked_items)
    //   "attachment_id"  — attachment ID (required for download_attachment)
    //   "file_path"      — local file path (required for upload/download_attachment)
    //   "file_name"      — attachment filename (optional for upload, defaults to basename)
    //   "body"           — JSON:API request body string (required for update/create,
    //                      unless field_name + field_value/field_value_file are used)
    //   "field_name"     — attribute name to update (convenience alternative to body)
    //   "field_value"    — plain text value for the field (JSON-escaped internally)
    //   "field_value_file" — path to a file whose contents become the field value
    //                      (takes precedence over field_value; supports per-item output piping)
    //
    // Outputs:
    //   "response"       — raw JSON:API response body (written to task output)
    class PolarionWriteTaskExecutor : public ICloudTaskExecutor
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
