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
    // Task executor for Google Sheets read/write operations via Sheets API v4.
    //
    // JCWF task types: "sheets_read", "sheets_write"
    //
    // sheets_read params:
    //   "connection"      — named CloudConnection (required)
    //   "spreadsheet_id"  — spreadsheet ID (optional, defaults to connection)
    //   "range"           — A1 notation range (required, e.g. "Sheet1!A1:D100")
    //   "output_format"   — "csv" (default) or "json"
    //   "output_file"     — output filename (optional)
    //
    // sheets_write params:
    //   "connection"          — named CloudConnection (required)
    //   "spreadsheet_id"      — spreadsheet ID (optional, defaults to connection)
    //   "range"               — A1 notation range (required)
    //   "input_file"          — local CSV file to upload (required)
    //   "value_input_option"  — "USER_ENTERED" (default) or "RAW"
    class GoogleSheetsCloudTaskExecutor : public ICloudTaskExecutor
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
