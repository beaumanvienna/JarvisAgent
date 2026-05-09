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
    // Task executor for database queries (PostgreSQL via libpq).
    //
    // JCWF task type: "db_query"
    //
    // Task params JSON keys:
    //   "connection"          — named CloudConnection (required)
    //   "query"               — SQL query string (required)
    //   "format"              — output format: "csv" (default) or "json"
    //   "output_file"         — filename for results (default: "result.csv" or "result.json").
    //                           MUST be a bare filename — path separators are rejected so the
    //                           file always lands inside the task working directory.
    //   "max_rows"            — row cap (default 100000, hard ceiling 1000000).
    //   "max_output_bytes"    — output file byte cap (default 100MB, hard ceiling 1GB).
    //   "statement_timeout_ms"— server-side statement timeout (default 60000, hard ceiling 600000).
    //
    // --- Trust model ---------------------------------------------------------
    // SQL is sent as-is to PostgreSQL — the workflow author is responsible for
    // correctness AND content.  This executor cannot prevent a malicious or buggy
    // SQL string from doing whatever the connection's DB role can do (read,
    // mutate, drop tables, etc.) — by design.  Defense in depth lives in three
    // places:
    //
    //   1. **Operator gate at submission**: db_query tasks reach the runtime only
    //      via JCWFs that the operator either authored or approved. The adhoc
    //      submission path additionally requires `adhoc_enabled` on the MCP key
    //      and `operator` role minimum.
    //
    //   2. **DB-side permissions**: operators MUST configure the DB user named
    //      by `connection.params.user` with the minimum permissions required
    //      for the workflow's queries.  A read-only role for read-only
    //      workloads.  This is the only durable SQL-injection defense — any
    //      string-level "validation" inside this executor would either reject
    //      legitimate queries or miss a clever payload.
    //
    //   3. **Blast-radius caps**: this executor enforces row count, output
    //      bytes, and a server-side `statement_timeout` so a runaway query
    //      cannot exhaust the disk, memory, or DB connection pool — see the
    //      param keys above.  Hard ceilings cap the JCWF-overrideable values
    //      so even an authored-bad workflow can't unbound them.
    //
    // The "parameterized queries" recipe for SQL-injection defense doesn't
    // apply here because there's no template-and-substitution boundary —
    // `query` IS the SQL the author wrote.  Trust model + blast-radius caps
    // are the structural defense.
    class DbQueryCloudTaskExecutor : public ICloudTaskExecutor
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
