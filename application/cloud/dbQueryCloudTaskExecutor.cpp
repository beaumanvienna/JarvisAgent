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

#include <filesystem>
#include <fstream>
#include <sstream>

#include <libpq-fe.h>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/dbQueryCloudTaskExecutor.h"
#include "cloud/postgresConnector.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;

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

    // Escape a string for JSON output.
    static std::string JsonEscape(std::string const& value)
    {
        std::string escaped;
        for (char c : value)
        {
            switch (c)
            {
                case '"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += c; break;
            }
        }
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

        std::string query = getStringParam("query");
        if (query.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'query' in db_query params";
            taskState.m_State = TaskInstanceStateKind::Failed;
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
            return false;
        }

        std::string outputFile = getStringParam("output_file");
        if (outputFile.empty())
        {
            outputFile = (format == "csv") ? "result.csv" : "result.json";
        }

        // Connect to PostgreSQL
        std::string connStr = PostgresConnector::BuildConnectionString(connection, credentials);
        PGconn* conn = PQconnectdb(connStr.c_str());

        if (PQstatus(conn) != CONNECTION_OK)
        {
            taskState.m_LastErrorMessage = "PostgreSQL connection failed: " + std::string(PQerrorMessage(conn));
            taskState.m_State = TaskInstanceStateKind::Failed;
            PQfinish(conn);
            return false;
        }

        // Execute query
        PGresult* res = PQexec(conn, query.c_str());
        ExecStatusType status = PQresultStatus(res);

        if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
        {
            taskState.m_LastErrorMessage = "PostgreSQL query failed: " + std::string(PQresultErrorMessage(res));
            taskState.m_State = TaskInstanceStateKind::Failed;
            PQclear(res);
            PQfinish(conn);
            return false;
        }

        int nRows = PQntuples(res);
        int nCols = PQnfields(res);

        // Resolve output path
        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path workDir =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

        {
            std::error_code ec;
            std::filesystem::create_directories(workDir, ec);
        }

        std::filesystem::path outputPath = workDir / outputFile;

        // Write results to file
        if (format == "csv")
        {
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open output file: " + outputPath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                PQclear(res);
                PQfinish(conn);
                return false;
            }

            // Header row
            for (int col = 0; col < nCols; ++col)
            {
                if (col > 0)
                {
                    out << ',';
                }
                out << CsvEscape(PQfname(res, col));
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
                    if (PQgetisnull(res, row, col))
                    {
                        // Empty field for NULL
                    }
                    else
                    {
                        out << CsvEscape(PQgetvalue(res, row, col));
                    }
                }
                out << '\n';
            }
        }
        else // json
        {
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open output file: " + outputPath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                PQclear(res);
                PQfinish(conn);
                return false;
            }

            // Collect column names
            std::vector<std::string> colNames;
            colNames.reserve(static_cast<size_t>(nCols));
            for (int col = 0; col < nCols; ++col)
            {
                colNames.emplace_back(PQfname(res, col));
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
                    out << "\"" << JsonEscape(colNames[static_cast<size_t>(col)]) << "\": ";

                    if (PQgetisnull(res, row, col))
                    {
                        out << "null";
                    }
                    else
                    {
                        out << "\"" << JsonEscape(PQgetvalue(res, row, col)) << "\"";
                    }
                }
                out << "}";
            }
            out << "\n]\n";
        }

        // Build summary for captured stdout
        std::string summary = "rows=" + std::to_string(nRows) + " cols=" + std::to_string(nCols) +
                              " format=" + format + " file=" + outputFile;

        taskState.m_CapturedStdout = summary.substr(0, std::min(summary.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        LOG_APP_INFO("[db_query] executed query on connection '{}': {} rows, {} cols -> {}", connection.m_Name, nRows,
                     nCols, outputPath.string());

        PQclear(res);
        PQfinish(conn);
        return true;
    }
} // namespace AIAssistant
