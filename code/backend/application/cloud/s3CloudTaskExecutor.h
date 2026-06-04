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
    // Task executor for S3 upload/download operations.
    //
    // JCWF task type: "s3"
    //
    // Task params JSON keys:
    //   "connection"  — named CloudConnection (required)
    //   "operation"   — "upload", "download", "list", "delete" (required)
    //   "bucket"      — bucket name (optional, defaults to connection's default bucket)
    //   "key"         — S3 object key (required for upload/download/delete)
    //   "file_path"   — local file path (required for upload/download)
    //   "prefix"      — key prefix for list operation (optional)
    //   "max_keys"    — max items for list (optional, default 1000)
    class S3CloudTaskExecutor : public ICloudTaskExecutor
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
