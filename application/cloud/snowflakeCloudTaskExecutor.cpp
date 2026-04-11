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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/snowflakeCloudTaskExecutor.h"
#include "cloud/snowflakeConnector.h"
#include "curlWrapper/curlWrapper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr long kCurlTimeoutSeconds = 60;
    static constexpr int kDefaultPollIntervalSeconds = 2;
    static constexpr int kDefaultStatementTimeoutSeconds = 3600;

    // Helper: perform an authenticated Snowflake REST API request
    static bool SnowflakeRequest(std::string const& method, std::string const& url, std::string const& jwt,
                                 std::string& responseBody, long& httpCode,
                                 std::string const& requestBody = {})
    {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            return false;
        }

        responseBody.clear();

        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            buf->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, kCurlTimeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        if (!requestBody.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        }

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + jwt;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "X-Snowflake-Authorization-Token-Type: KEYPAIR_JWT");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    // Escape a CSV field per RFC 4180
    static std::string CsvEscape(std::string_view value)
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
            return std::string(value);
        }

        std::string result = "\"";
        for (char c : value)
        {
            if (c == '"')
            {
                result += "\"\"";
            }
            else
            {
                result += c;
            }
        }
        result += '"';
        return result;
    }

    bool SnowflakeCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
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
            taskState.m_LastErrorMessage = "Failed to parse snowflake_query task params JSON";
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
            taskState.m_LastErrorMessage = "Missing required 'query' in snowflake_query task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string outputFormat = getStringParam("output_format");
        if (outputFormat.empty())
        {
            outputFormat = "csv";
        }

        std::string outputFile = getStringParam("output_file");
        if (outputFile.empty())
        {
            outputFile = (outputFormat == "json") ? "result.json" : "result.csv";
        }

        int statementTimeout = kDefaultStatementTimeoutSeconds;
        {
            uint64_t val;
            if (doc["timeout"].get_uint64().get(val) == simdjson::SUCCESS)
            {
                statementTimeout = static_cast<int>(val);
            }
        }

        int pollInterval = kDefaultPollIntervalSeconds;
        {
            uint64_t val;
            if (doc["poll_interval"].get_uint64().get(val) == simdjson::SUCCESS)
            {
                pollInterval = static_cast<int>(std::max(val, uint64_t(1)));
            }
        }

        // Build warehouse/database/schema context — task params override connection defaults
        std::string warehouse = getStringParam("warehouse");
        std::string database = getStringParam("database");
        std::string schema = getStringParam("schema");

        auto getConnectionParam = [&connection](std::string const& key) -> std::string
        {
            auto it = connection.m_Params.find(key);
            return (it != connection.m_Params.end()) ? it->second : std::string{};
        };

        if (warehouse.empty())
        {
            warehouse = getConnectionParam("warehouse");
        }
        if (database.empty())
        {
            database = getConnectionParam("database");
        }
        if (schema.empty())
        {
            schema = getConnectionParam("schema");
        }

        // Build the SQL submit request body
        // Escape the query for JSON embedding
        std::string escapedQuery;
        for (char c : query)
        {
            switch (c)
            {
                case '"': escapedQuery += "\\\""; break;
                case '\\': escapedQuery += "\\\\"; break;
                case '\n': escapedQuery += "\\n"; break;
                case '\r': escapedQuery += "\\r"; break;
                case '\t': escapedQuery += "\\t"; break;
                default: escapedQuery += c; break;
            }
        }

        std::string requestBody = "{\"statement\":\"" + escapedQuery + "\"";
        requestBody += ",\"timeout\":" + std::to_string(statementTimeout);
        requestBody += ",\"resultSetMetaData\":{\"format\":\"jsonv2\"}";
        if (!warehouse.empty())
        {
            requestBody += ",\"warehouse\":\"" + warehouse + "\"";
        }
        if (!database.empty())
        {
            requestBody += ",\"database\":\"" + database + "\"";
        }
        if (!schema.empty())
        {
            requestBody += ",\"schema\":\"" + schema + "\"";
        }
        requestBody += "}";

        // Submit the statement
        std::string apiBase = SnowflakeConnector::BuildApiBaseUrl(connection.m_Endpoint);
        std::string submitUrl = apiBase + "/api/v2/statements";

        std::string responseBody;
        long httpCode = 0;

        if (!SnowflakeRequest("POST", submitUrl, credentials.m_Token, responseBody, httpCode, requestBody))
        {
            taskState.m_LastErrorMessage = "Snowflake statement submission failed (curl error)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        if (httpCode >= 400)
        {
            taskState.m_LastErrorMessage = "Snowflake statement submission failed: HTTP " + std::to_string(httpCode);
            if (!responseBody.empty() && responseBody.size() < 500)
            {
                taskState.m_LastErrorMessage += ": " + responseBody;
            }
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Parse the submit response to get the statement handle and status
        simdjson::ondemand::parser submitParser;
        simdjson::padded_string submitPadded(responseBody);
        simdjson::ondemand::document submitDoc;

        if (submitParser.iterate(submitPadded).get(submitDoc))
        {
            taskState.m_LastErrorMessage = "Failed to parse Snowflake submit response";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string_view statementHandle;
        if (submitDoc["statementHandle"].get_string().get(statementHandle))
        {
            taskState.m_LastErrorMessage = "Snowflake response missing statementHandle";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string handle(statementHandle);

        std::string_view statusCode;
        submitDoc["statementStatusUrl"].get_string(); // consume if present
        auto statusErr = submitDoc["code"].get_string().get(statusCode);
        (void)statusErr;

        LOG_APP_INFO("[snowflake] submitted statement, handle={}", handle);

        // Check if the result is already available (synchronous execution for small queries)
        std::string_view message;
        auto msgErr = submitDoc["message"].get_string().get(message);
        (void)msgErr;

        bool resultReady = (message == "Statement executed successfully.");

        // Poll until the result is available
        if (!resultReady)
        {
            std::string pollUrl = apiBase + "/api/v2/statements/" + handle;

            auto pollStart = std::chrono::steady_clock::now();
            auto maxDuration = std::chrono::seconds(statementTimeout);

            while (!resultReady)
            {
                if (cancellationToken.IsCancelled())
                {
                    // Cancel the statement on Snowflake side
                    std::string cancelUrl = apiBase + "/api/v2/statements/" + handle + "/cancel";
                    std::string cancelResponse;
                    long cancelCode = 0;
                    SnowflakeRequest("POST", cancelUrl, credentials.m_Token, cancelResponse, cancelCode);
                    LOG_APP_INFO("[snowflake] cancelled statement handle={}", handle);

                    taskState.m_LastErrorMessage = "Snowflake query cancelled";
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    return false;
                }

                if (std::chrono::steady_clock::now() - pollStart > maxDuration)
                {
                    taskState.m_LastErrorMessage =
                        "Snowflake query timed out after " + std::to_string(statementTimeout) + " seconds";
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    return false;
                }

                std::this_thread::sleep_for(std::chrono::seconds(pollInterval));

                responseBody.clear();
                if (!SnowflakeRequest("GET", pollUrl, credentials.m_Token, responseBody, httpCode))
                {
                    taskState.m_LastErrorMessage = "Snowflake poll request failed";
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    return false;
                }

                if (httpCode >= 400)
                {
                    taskState.m_LastErrorMessage =
                        "Snowflake poll failed: HTTP " + std::to_string(httpCode);
                    if (!responseBody.empty() && responseBody.size() < 500)
                    {
                        taskState.m_LastErrorMessage += ": " + responseBody;
                    }
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    return false;
                }

                // Check poll response status
                simdjson::ondemand::parser pollParser;
                simdjson::padded_string pollPadded(responseBody);
                simdjson::ondemand::document pollDoc;

                if (pollParser.iterate(pollPadded).get(pollDoc))
                {
                    taskState.m_LastErrorMessage = "Failed to parse Snowflake poll response";
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    return false;
                }

                std::string_view pollMessage;
                if (!pollDoc["message"].get_string().get(pollMessage))
                {
                    if (pollMessage == "Statement executed successfully.")
                    {
                        resultReady = true;
                    }
                }

                // Also check for error status
                std::string_view pollCode;
                if (!pollDoc["code"].get_string().get(pollCode))
                {
                    if (pollCode != "090001" && pollCode != "333334") // 333334 = still running
                    {
                        taskState.m_LastErrorMessage = "Snowflake query failed with code " + std::string(pollCode);
                        std::string_view errMsg;
                        if (!pollDoc["message"].get_string().get(errMsg))
                        {
                            taskState.m_LastErrorMessage += ": " + std::string(errMsg);
                        }
                        taskState.m_State = TaskInstanceStateKind::Failed;
                        return false;
                    }
                }
            }
        }

        // Parse the result set from responseBody (either the submit response or the final poll response)
        simdjson::ondemand::parser resultParser;
        simdjson::padded_string resultPadded(responseBody);
        simdjson::ondemand::document resultDoc;

        if (resultParser.iterate(resultPadded).get(resultDoc))
        {
            taskState.m_LastErrorMessage = "Failed to parse Snowflake result response";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Extract column names from resultSetMetaData
        std::vector<std::string> columnNames;
        {
            simdjson::ondemand::object metaData;
            if (!resultDoc["resultSetMetaData"].get_object().get(metaData))
            {
                simdjson::ondemand::array rowType;
                if (!metaData["rowType"].get_array().get(rowType))
                {
                    for (auto col : rowType)
                    {
                        simdjson::ondemand::object colObj;
                        if (!col.get_object().get(colObj))
                        {
                            std::string_view colName;
                            if (!colObj["name"].get_string().get(colName))
                            {
                                columnNames.emplace_back(colName);
                            }
                        }
                    }
                }
            }
        }

        // Extract rows from the "data" array (array of arrays of strings in jsonv2 format)
        std::vector<std::vector<std::string>> rows;
        {
            simdjson::ondemand::array dataArray;
            if (!resultDoc["data"].get_array().get(dataArray))
            {
                for (auto row : dataArray)
                {
                    std::vector<std::string> rowValues;
                    simdjson::ondemand::array rowArray;
                    if (!row.get_array().get(rowArray))
                    {
                        for (auto val : rowArray)
                        {
                            std::string_view sv;
                            if (!val.get_string().get(sv))
                            {
                                rowValues.emplace_back(sv);
                            }
                            else
                            {
                                // null values
                                rowValues.emplace_back();
                            }
                        }
                    }
                    rows.push_back(std::move(rowValues));
                }
            }
        }

        LOG_APP_INFO("[snowflake] query completed: {} columns, {} rows", columnNames.size(), rows.size());

        // Resolve output path
        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path workDir =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

        {
            std::error_code ec;
            std::filesystem::create_directories(workDir, ec);
        }

        std::filesystem::path outputPath = workDir / outputFile;

        // Write output
        if (outputFormat == "json")
        {
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open output file: " + outputPath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            out << "[\n";
            for (size_t r = 0; r < rows.size(); ++r)
            {
                out << "  {";
                for (size_t c = 0; c < columnNames.size() && c < rows[r].size(); ++c)
                {
                    if (c > 0)
                    {
                        out << ", ";
                    }
                    out << "\"" << columnNames[c] << "\": ";
                    if (rows[r][c].empty())
                    {
                        out << "null";
                    }
                    else
                    {
                        // JSON-escape the value
                        out << "\"";
                        for (char ch : rows[r][c])
                        {
                            switch (ch)
                            {
                                case '"': out << "\\\""; break;
                                case '\\': out << "\\\\"; break;
                                case '\n': out << "\\n"; break;
                                case '\r': out << "\\r"; break;
                                case '\t': out << "\\t"; break;
                                default: out << ch; break;
                            }
                        }
                        out << "\"";
                    }
                }
                out << "}" << (r + 1 < rows.size() ? "," : "") << "\n";
            }
            out << "]\n";
        }
        else
        {
            // CSV output (RFC 4180)
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open output file: " + outputPath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Header row
            for (size_t c = 0; c < columnNames.size(); ++c)
            {
                if (c > 0)
                {
                    out << ",";
                }
                out << CsvEscape(columnNames[c]);
            }
            out << "\n";

            // Data rows
            for (auto const& row : rows)
            {
                for (size_t c = 0; c < row.size(); ++c)
                {
                    if (c > 0)
                    {
                        out << ",";
                    }
                    out << CsvEscape(row[c]);
                }
                out << "\n";
            }
        }

        // Build summary for captured stdout
        std::string summary = "{\"ok\":true,\"rows\":" + std::to_string(rows.size()) +
                              ",\"columns\":" + std::to_string(columnNames.size()) +
                              ",\"output\":\"" + outputFile + "\"}";

        taskState.m_CapturedStdout = summary.substr(0, std::min(summary.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write raw response to response.json
        {
            std::ofstream responseFile(workDir / "response.json", std::ios::trunc);
            if (responseFile.is_open())
            {
                responseFile << responseBody;
            }
        }

        return true;
    }
} // namespace AIAssistant
