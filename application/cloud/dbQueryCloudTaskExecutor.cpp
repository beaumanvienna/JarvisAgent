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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <libpq-fe.h>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/dbQueryCloudTaskExecutor.h"
#include "cloud/postgresConnector.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr size_t kMaxLibpqErrorChars = 250; // truncate libpq errors before they reach m_LastErrorMessage

    // Blast-radius caps for db_query — see header trust-model block.
    // Defaults are JCWF-overrideable; ceilings are not.
    static constexpr size_t kDefaultMaxRows = 100'000;
    static constexpr size_t kCeilingMaxRows = 1'000'000;
    static constexpr size_t kDefaultMaxOutputBytes = 100ull * 1024 * 1024; // 100 MB
    static constexpr size_t kCeilingMaxOutputBytes = 1024ull * 1024 * 1024; // 1 GB
    static constexpr int kDefaultStatementTimeoutMs = 60'000;
    static constexpr int kCeilingStatementTimeoutMs = 600'000;

    // RAII deleters for libpq handles so any early return / exception path
    // releases the connection + result.  Pre-fix code had 6 explicit
    // PQfinish/PQclear sites and would leak on any std::bad_alloc /
    // std::filesystem exception between handle acquisition and the explicit
    // free.  unique_ptr<T, deleter> is the canonical idiom for this.
    namespace
    {
        struct PgConnDeleter { void operator()(PGconn* c) const noexcept { if (c) PQfinish(c); } };
        struct PgResultDeleter { void operator()(PGresult* r) const noexcept { if (r) PQclear(r); } };
        using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
        using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

        // Truncate a libpq error message before it reaches m_LastErrorMessage.
        // Connection-string fragments / role names / table layouts can show up
        // in raw PQerrorMessage output; cap the length so a hostile query
        // can't echo the full DB schema into the dashboard via the run analyzer.
        std::string TruncateLibpqError(char const* raw)
        {
            if (raw == nullptr) return {};
            std::string s = raw;
            // Strip trailing newline that libpq always appends.
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.size() > kMaxLibpqErrorChars)
            {
                s.resize(kMaxLibpqErrorChars);
                s += "... (truncated)";
            }
            return s;
        }
    }

    // Escape a value for CSV output (RFC 4180).
    static std::string CsvEscape(std::string const& value)
    {
        bool needsQuoting = false;
        for (char c : value)
        {
            if (c == ',' || c == '"' || c == '\n' || c == '\r')
            {
                needsQuoting = true;
                break;
            }
        }

        if (!needsQuoting)
        {
            return value;
        }

        std::string escaped = "\"";
        for (char c : value)
        {
            if (c == '"')
            {
                escaped += "\"\"";
            }
            else
            {
                escaped += c;
            }
        }
        escaped += "\"";
        return escaped;
    }


    bool DbQueryCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
                                                WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                                                TaskInstanceState& taskState, CloudConnection const& connection,
                                                CloudCredentials const& credentials,
                                                TaskCancellationToken const& cancellationToken)
    {
        // Parse task params
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            taskState.m_LastErrorMessage = "Failed to parse db_query params JSON";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] failed to parse params JSON task='{}' workflow='{}' run='{}': {}",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                          simdjson::error_message(error));
            return false;
        }

        auto getStringParam = [&doc](std::string const& key) -> std::string
        {
            std::string_view sv;
            if (doc[key].get_string().get(sv) == simdjson::SUCCESS)
            {
                return std::string(sv);
            }
            return {};
        };

        auto getUint64Param = [&doc](std::string const& key, uint64_t def, uint64_t ceiling) -> uint64_t
        {
            uint64_t v = 0;
            if (doc[key].get_uint64().get(v) == simdjson::SUCCESS)
            {
                return std::clamp<uint64_t>(v, 1, ceiling);
            }
            return def;
        };

        std::string query = getStringParam("query");
        if (query.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'query' in db_query params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] missing 'query' param task='{}' workflow='{}' run='{}'",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }

        std::string format = getStringParam("format");
        if (format.empty())
        {
            format = "csv";
        }
        if (format != "csv" && format != "json")
        {
            taskState.m_LastErrorMessage = "db_query 'format' must be 'csv' or 'json', got '" + format + "'";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] invalid format task='{}' workflow='{}' run='{}': '{}'",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, format);
            return false;
        }

        std::string outputFile = getStringParam("output_file");
        if (outputFile.empty())
        {
            outputFile = (format == "csv") ? "result.csv" : "result.json";
        }
        // Reject path separators in output_file.  The file is always written
        // INSIDE the task working directory; any embedded `/`, `\`, or `..`
        // segment is a path-traversal attempt.  Combined with the
        // ConfineUnderProjectRoot gate below, even a JCWF that tries to
        // escape via the resolved path can't write outside the workDir.
        if (outputFile.find('/') != std::string::npos || outputFile.find('\\') != std::string::npos ||
            outputFile == "." || outputFile == "..")
        {
            taskState.m_LastErrorMessage = "db_query 'output_file' must be a bare filename (no path separators)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] output_file rejected (contains path separator) task='{}' workflow='{}' "
                          "run='{}': '{}'",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, outputFile);
            return false;
        }

        // Blast-radius caps — see header trust-model block.
        size_t const maxRows = static_cast<size_t>(getUint64Param("max_rows", kDefaultMaxRows, kCeilingMaxRows));
        size_t const maxOutputBytes = static_cast<size_t>(
            getUint64Param("max_output_bytes", kDefaultMaxOutputBytes, kCeilingMaxOutputBytes));
        int const statementTimeoutMs = static_cast<int>(
            getUint64Param("statement_timeout_ms",
                           static_cast<uint64_t>(kDefaultStatementTimeoutMs),
                           static_cast<uint64_t>(kCeilingStatementTimeoutMs)));

        // Tripwire: reject forbidden libpq cert/key/file-path params before
        // building the connection string.  See PostgresConnector::ValidatePostgresParams
        // docstring.  Mirrors the same gate in PostgresConnector::TestConnection.
        if (auto r = PostgresConnector::ValidatePostgresParams(connection); !r)
        {
            taskState.m_LastErrorMessage = "db_query: " + r.error().m_Details;
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] task='{}' workflow='{}' run='{}': {}",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, r.error().m_Details);
            LOG_SECURITY_WARN("[security] postgres_forbidden_param connection='{}' message='{}'",
                              connection.m_Name, r.error().m_Details);
            return false;
        }

        // Gate on sslmode allowlist + non-localhost production posture before
        // building the connection string.  Mirrors the same check in
        // PostgresConnector::TestConnection so an unsafe sslmode never reaches
        // libpq's connect.
        {
            std::string host;
            std::string port;
            PostgresConnector::ParseHostPort(connection, host, port);
            auto const sslmodeIt = connection.m_Params.find("sslmode");
            std::string const sslmode = (sslmodeIt != connection.m_Params.end() && !sslmodeIt->second.empty())
                                             ? sslmodeIt->second
                                             : "require";
            std::string sslErr;
            if (!PostgresConnector::IsValidSslMode(host, sslmode, sslErr))
            {
                taskState.m_LastErrorMessage = "db_query: " + sslErr;
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[db_query] task='{}' workflow='{}' run='{}': {}",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, sslErr);
                LOG_SECURITY_WARN("[security] postgres_invalid_sslmode connection='{}' host='{}' sslmode='{}'",
                                  connection.m_Name, host, sslmode);
                return false;
            }
        }

        // Resolve output path BEFORE opening the DB connection so a path
        // rejection releases no resources.
        std::filesystem::path const workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path const workDir =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

        {
            std::error_code ec;
            std::filesystem::create_directories(workDir, ec);
            if (ec)
            {
                taskState.m_LastErrorMessage = "Cannot create work directory: " + workDir.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[db_query] create_directories failed task='{}' workflow='{}' run='{}' path='{}': {}",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                              workDir.string(), ec.message());
                return false;
            }
        }

        std::filesystem::path const outputPathRaw = workDir / outputFile;
        // Containment gate: even though output_file is filename-only and workDir
        // comes from TaskPathResolver, a hostile workflow_base_directory in
        // workflowDefinition could move workDir out of the project tree.  Pass
        // through ConfineUnderProjectRoot for symmetry with the rest of the
        // hardened cloud surface; rejection is fail-closed with ERROR.
        std::filesystem::path const outputPath = ConfineUnderProjectRoot(outputPathRaw);
        if (outputPath.empty())
        {
            taskState.m_LastErrorMessage = "Output path does not resolve under project root";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] output path rejected task='{}' workflow='{}' run='{}' path='{}'",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, outputPathRaw.string());
            return false;
        }

        // Connect to PostgreSQL.  RAII deleters release the handle on every
        // exit path (early returns, exceptions, scope end).
        std::string const connStr = PostgresConnector::BuildConnectionString(connection, credentials);
        PgConnPtr conn(PQconnectdb(connStr.c_str()));
        if (!conn)
        {
            // libpq returns nullptr only on OOM (extremely rare).  Surface it
            // distinctly from the connection-failed branch so operators can
            // tell "couldn't allocate" from "couldn't connect".
            taskState.m_LastErrorMessage = "PQconnectdb returned nullptr (libpq OOM)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] PQconnectdb returned nullptr task='{}' workflow='{}' run='{}'",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }

        if (PQstatus(conn.get()) != CONNECTION_OK)
        {
            std::string const truncated = TruncateLibpqError(PQerrorMessage(conn.get()));
            taskState.m_LastErrorMessage = "PostgreSQL connection failed: " + truncated;
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] connection failed task='{}' workflow='{}' run='{}' connection='{}': {}",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                          connection.m_Name, truncated);
            return false;
        }

        // Server-side statement timeout — bounds the worst-case runaway query
        // even if the connection's DB role permits long-running statements.
        // Must run BEFORE the user's query.  Use parameterized SET via PQexec
        // with a hand-built integer literal (no string concat from user input).
        {
            std::string const setTimeout = "SET statement_timeout = " + std::to_string(statementTimeoutMs);
            PgResultPtr setRes(PQexec(conn.get(), setTimeout.c_str()));
            if (!setRes || PQresultStatus(setRes.get()) != PGRES_COMMAND_OK)
            {
                std::string const truncated =
                    TruncateLibpqError(setRes ? PQresultErrorMessage(setRes.get()) : "PQexec returned nullptr");
                taskState.m_LastErrorMessage = "Failed to set statement_timeout: " + truncated;
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[db_query] SET statement_timeout failed task='{}' workflow='{}' run='{}': {}",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, truncated);
                return false;
            }
        }

        // Execute query
        PgResultPtr res(PQexec(conn.get(), query.c_str()));
        if (!res)
        {
            taskState.m_LastErrorMessage = "PQexec returned nullptr";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] PQexec returned nullptr task='{}' workflow='{}' run='{}'",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }
        ExecStatusType const status = PQresultStatus(res.get());
        if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
        {
            std::string const truncated = TruncateLibpqError(PQresultErrorMessage(res.get()));
            taskState.m_LastErrorMessage = "PostgreSQL query failed: " + truncated;
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] query failed task='{}' workflow='{}' run='{}': {}",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, truncated);
            return false;
        }

        int const nRows = PQntuples(res.get());
        int const nCols = PQnfields(res.get());

        // Row cap — refuse to write a result set larger than the JCWF-configured
        // cap (default 100k, ceiling 1M).  Bounds disk + memory before the
        // write loop starts.
        if (static_cast<size_t>(nRows) > maxRows)
        {
            taskState.m_LastErrorMessage = "Result set has " + std::to_string(nRows) +
                                           " rows, exceeds max_rows=" + std::to_string(maxRows);
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] row cap exceeded task='{}' workflow='{}' run='{}' rows={} cap={}",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, nRows, maxRows);
            return false;
        }

        // Lambda body for streaming output with a running byte cap.  Returns
        // false (and sets taskState) on cap-exceeded; true on success.  The
        // cap is checked after each row's write so a runaway column-of-blobs
        // query gets caught as soon as the budget is gone.
        size_t writtenBytes = 0;
        auto checkByteCap = [&](std::ofstream& out) -> bool {
            std::streampos const pos = out.tellp();
            if (pos < 0) return true; // tellp failed; trust the higher-level write check
            writtenBytes = static_cast<size_t>(pos);
            if (writtenBytes > maxOutputBytes)
            {
                taskState.m_LastErrorMessage = "Output file exceeds max_output_bytes=" + std::to_string(maxOutputBytes);
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[db_query] output byte cap exceeded task='{}' workflow='{}' run='{}' bytes={} cap={}",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                              writtenBytes, maxOutputBytes);
                return false;
            }
            return true;
        };

        // Write results to file.  This is a streaming writer (running byte
        // cap is checked after each row via tellp), so atomic-rename through
        // the shared helper is not applicable — buffering the entire result
        // in memory would defeat the cap.  We do enable ofstream exceptions
        // so disk-full or short-write conditions surface as a typed failure
        // instead of silently truncating mid-row.
        try
        {
            if (format == "csv")
            {
                std::ofstream out(outputPath, std::ios::trunc);
                if (!out.is_open())
                {
                    taskState.m_LastErrorMessage = "Cannot open output file: " + outputPath.string();
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    LOG_APP_ERROR("[db_query] output file open failed task='{}' workflow='{}' run='{}' path='{}'",
                                  taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                                  outputPath.string());
                    return false;
                }
                out.exceptions(std::ios::failbit | std::ios::badbit);

                // Header row
                for (int col = 0; col < nCols; ++col)
                {
                    if (col > 0)
                    {
                        out << ',';
                    }
                    out << CsvEscape(PQfname(res.get(), col));
                }
                out << '\n';

                // Data rows
                for (int row = 0; row < nRows; ++row)
                {
                    for (int col = 0; col < nCols; ++col)
                    {
                        if (col > 0)
                        {
                            out << ',';
                        }
                        if (PQgetisnull(res.get(), row, col))
                        {
                            // Empty field for NULL
                        }
                        else
                        {
                            out << CsvEscape(PQgetvalue(res.get(), row, col));
                        }
                    }
                    out << '\n';
                    if (!checkByteCap(out)) return false;
                }
            }
            else // json
            {
                std::ofstream out(outputPath, std::ios::trunc);
                if (!out.is_open())
                {
                    taskState.m_LastErrorMessage = "Cannot open output file: " + outputPath.string();
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    LOG_APP_ERROR("[db_query] output file open failed task='{}' workflow='{}' run='{}' path='{}'",
                                  taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                                  outputPath.string());
                    return false;
                }
                out.exceptions(std::ios::failbit | std::ios::badbit);

                // Collect column names
                std::vector<std::string> colNames;
                colNames.reserve(static_cast<size_t>(nCols));
                for (int col = 0; col < nCols; ++col)
                {
                    colNames.emplace_back(PQfname(res.get(), col));
                }

                out << "[\n";
                for (int row = 0; row < nRows; ++row)
                {
                    if (row > 0)
                    {
                        out << ",\n";
                    }
                    out << "  {";
                    for (int col = 0; col < nCols; ++col)
                    {
                        if (col > 0)
                        {
                            out << ", ";
                        }
                        out << "\"" << JsonHelper::EscapeJsonString(colNames[static_cast<size_t>(col)]) << "\": ";

                        if (PQgetisnull(res.get(), row, col))
                        {
                            out << "null";
                        }
                        else
                        {
                            out << "\"" << JsonHelper::EscapeJsonString(PQgetvalue(res.get(), row, col)) << "\"";
                        }
                    }
                    out << "}";
                    if (!checkByteCap(out)) return false;
                }
                out << "\n]\n";
            }
        }
        catch (std::exception const& e)
        {
            taskState.m_LastErrorMessage = std::string("db_query output write failed: ") + e.what();
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[db_query] write failed task='{}' workflow='{}' run='{}' path='{}': {}",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, outputPath.string(),
                          e.what());
            return false;
        }

        // Build summary for captured stdout
        std::string summary = "rows=" + std::to_string(nRows) + " cols=" + std::to_string(nCols) +
                              " format=" + format + " file=" + outputFile;

        taskState.m_CapturedStdout = summary.substr(0, std::min(summary.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        LOG_APP_INFO("[db_query] executed query on connection '{}': {} rows, {} cols -> {}", connection.m_Name, nRows,
                     nCols, outputPath.string());

        // RAII releases conn and res.
        return true;
    }
} // namespace AIAssistant
