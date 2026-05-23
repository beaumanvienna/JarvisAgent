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
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "auxiliary/file.h"
#include "engine.h"
#include "cloud/snowflakeCloudTaskExecutor.h"
#include "cloud/snowflakeConnector.h"
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlSlistHelper.h"
#include "curlWrapper/curlWrapper.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr long kCurlTimeoutSeconds = 60;
    static constexpr int kDefaultPollIntervalSeconds = 2;
    static constexpr int kDefaultStatementTimeoutSeconds = 3600;
    // Bound on the response body the writeCallback will accept.  Snowflake result
    // sets are paged, so a single response larger than this is already pathological
    // (or attacker-controlled).  64 MB matches the rest of the cloud surface.
    static constexpr size_t kMaxSnowflakeResponseBytes = 64 * 1024 * 1024;
    // Bounds on the timeout / poll interval params.  Pre-clamp the uint64_t value
    // BEFORE any int cast so values larger than INT_MAX cannot wrap to negative
    // (which disables the timeout entirely) and cannot pin a worker thread for
    // years on a typo'd workflow.
    static constexpr int kMaxStatementTimeoutSeconds = 24 * 3600; // 24 hours
    static constexpr int kMaxPollIntervalSeconds = 60;

    // Snowflake statement handles look like UUIDs (alphanumeric + `-`).  Validate
    // before interpolating into the poll / cancel URL — the value comes from the
    // Snowflake server response, which is a trusted boundary in practice but
    // hostile in threat-model terms (compromised endpoint or response tampering).
    static bool IsValidSnowflakeHandle(std::string const& handle)
    {
        if (handle.empty() || handle.size() > 64)
        {
            return false;
        }
        for (char c : handle)
        {
            unsigned char const uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc))
            {
                continue;
            }
            if (c == '-')
            {
                continue;
            }
            return false;
        }
        return true;
    }

    // Helper: perform an authenticated Snowflake REST API request
    static bool SnowflakeRequest(std::string const& method, std::string const& url, SecureString const& jwt,
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
            size_t const incoming = size * nmemb;
            // Bound the response buffer.  Without this an attacker-controlled or
            // compromised Snowflake endpoint can stream arbitrary bytes into our
            // process.  Returning 0 from the libcurl write callback aborts the
            // transfer with CURLE_WRITE_ERROR, which the caller surfaces as a
            // regular Snowflake error.
            if (buf->size() + incoming > kMaxSnowflakeResponseBytes)
            {
                return 0;
            }
            buf->append(static_cast<char*>(contents), incoming);
            return incoming;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, kCurlTimeoutSeconds);
        // Hardened TLS + no redirect-following + DNS post-resolve check —
        // Snowflake's API never legitimately redirects, so any 30x is treated
        // as hostile.  Snowflake is always reached via `https://*.snowflakecomputing.com`
        // (per `SnowflakeConnector::BuildApiBaseUrl`), so the post-resolve
        // callback installs and rejects any DNS resolution to a local-network IP.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        if (!requestBody.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        }

        struct curl_slist* headers = nullptr;
        SecureString authScratch;
        AppendSecretHeader(headers, "Authorization: Bearer ", jwt, authScratch);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "X-Snowflake-Authorization-Token-Type: KEYPAIR_JWT");
        headers = curl_slist_append(headers, "User-Agent: j9t/1.0");
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
                // Pre-clamp BEFORE int cast: values > INT_MAX wrap to negative
                // (disabling the timeout entirely) which lets a hostile workflow
                // pin a worker thread for years.  Cap at 24 hours; lower bound 1.
                if (val > static_cast<uint64_t>(kMaxStatementTimeoutSeconds))
                {
                    val = kMaxStatementTimeoutSeconds;
                }
                statementTimeout = std::clamp(static_cast<int>(val), 1, kMaxStatementTimeoutSeconds);
            }
        }

        int pollInterval = kDefaultPollIntervalSeconds;
        {
            uint64_t val;
            if (doc["poll_interval"].get_uint64().get(val) == simdjson::SUCCESS)
            {
                if (val > static_cast<uint64_t>(kMaxPollIntervalSeconds))
                {
                    val = kMaxPollIntervalSeconds;
                }
                pollInterval = std::clamp(static_cast<int>(val), 1, kMaxPollIntervalSeconds);
            }
        }

        // JWT must contain no CR/LF before splicing into the Authorization header.
        // ResolveCredentials produces the JWT via JwtGenerator which is
        // well-behaved, but a future credential type or renewal flow could
        // inject hostile bytes; check defensively.
        if (ICloudTaskExecutor::ContainsCrlf(credentials.m_Token.Get()))
        {
            taskState.m_LastErrorMessage = "Snowflake JWT contains CR/LF — refusing to send";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[snowflake] task='{}' workflow='{}' run='{}': JWT CRLF rejected",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] snowflake_jwt_crlf_rejected task='{}' workflow='{}' run='{}'",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
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

        // Build the SQL submit request body — RFC 8259-compliant escape via
        // the canonical helper (also escapes control bytes 0x00..0x1F that the
        // prior inline 5-case switch passed through raw, producing technically
        // invalid JSON when the query contained e.g. an embedded NUL).
        std::string requestBody = "{\"statement\":\"" + JsonHelper::EscapeJsonString(query) + "\"";
        requestBody += ",\"timeout\":" + std::to_string(statementTimeout);
        requestBody += ",\"resultSetMetaData\":{\"format\":\"jsonv2\"}";
        // Escape warehouse / database / schema with the canonical helper.  Pre-fix
        // these were spliced raw — a value containing `","timeout":0,"x":"` would
        // override the timeout (or any other request field) by closing the JSON
        // string early.  JsonHelper produces RFC 8259 §7-compliant escapes.
        if (!warehouse.empty())
        {
            requestBody += ",\"warehouse\":\"" + JsonHelper::EscapeJsonString(warehouse) + "\"";
        }
        if (!database.empty())
        {
            requestBody += ",\"database\":\"" + JsonHelper::EscapeJsonString(database) + "\"";
        }
        if (!schema.empty())
        {
            requestBody += ",\"schema\":\"" + JsonHelper::EscapeJsonString(schema) + "\"";
        }
        requestBody += "}";

        // Submit the statement.  BuildApiBaseUrl validates the endpoint allowlist
        // and returns "" on rejection (already logged at the connector layer).
        std::string apiBase = SnowflakeConnector::BuildApiBaseUrl(connection.m_Endpoint);
        if (apiBase.empty())
        {
            taskState.m_LastErrorMessage =
                "Snowflake endpoint rejected: invalid account locator (see security log)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[snowflake] task='{}' workflow='{}' run='{}': endpoint rejected",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }
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
            // Don't embed the raw response body.  Snowflake error responses contain
            // schema names, partial query data, and other operational metadata that
            // shouldn't leak into m_LastErrorMessage (which is persisted to the
            // workflow state store and may surface in the dashboard / API).  The
            // structured error code + message is parsed below as a separate path
            // when the response is well-formed JSON.
            taskState.m_LastErrorMessage = "Snowflake statement submission failed: HTTP " + std::to_string(httpCode);
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

        // Server-supplied handle, but interpolated into the poll + cancel URL —
        // a compromised endpoint or response tampering could inject URL components
        // (path traversal, query string).  Validate before any URL build.
        if (!IsValidSnowflakeHandle(handle))
        {
            taskState.m_LastErrorMessage = "Snowflake server returned an invalid statement handle";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[snowflake] task='{}' workflow='{}' run='{}': server-supplied handle rejected (length={})",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, handle.size());
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] snowflake_invalid_handle task='{}' workflow='{}' run='{}'",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }

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
                    // Same rationale as the submit error path: don't embed the raw
                    // Snowflake response in m_LastErrorMessage.
                    taskState.m_LastErrorMessage =
                        "Snowflake poll failed: HTTP " + std::to_string(httpCode);
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

        // Confine outputFile under workDir.  An attacker-controlled value such as
        // `../../etc/cron.d/evil` or an absolute path would otherwise let the task
        // overwrite arbitrary files writable by the j9t process.  Same gate as
        // the email attachment path-traversal fix.
        if (!ValidateLocalPath(outputFile, workDir, taskDefinition.m_Id))
        {
            taskState.m_LastErrorMessage =
                "snowflake_query: output_file path is invalid or escapes the working directory";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[snowflake] task='{}' workflow='{}' run='{}': output_file path rejected",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }
        std::filesystem::path outputPath = workDir / outputFile;

        // Build output body
        std::ostringstream body;
        if (outputFormat == "json")
        {
            body << "[\n";
            for (size_t r = 0; r < rows.size(); ++r)
            {
                body << "  {";
                for (size_t c = 0; c < columnNames.size() && c < rows[r].size(); ++c)
                {
                    if (c > 0)
                    {
                        body << ", ";
                    }
                    body << "\"" << JsonHelper::EscapeJsonString(columnNames[c]) << "\": ";
                    if (rows[r][c].empty())
                    {
                        body << "null";
                    }
                    else
                    {
                        // RFC 8259 escape via canonical helper — superset of the prior
                        // inline 5-case switch (also handles control bytes 0x00..0x1F).
                        body << "\"" << JsonHelper::EscapeJsonString(rows[r][c]) << "\"";
                    }
                }
                body << "}" << (r + 1 < rows.size() ? "," : "") << "\n";
            }
            body << "]\n";
        }
        else
        {
            // CSV output (RFC 4180)
            for (size_t c = 0; c < columnNames.size(); ++c)
            {
                if (c > 0)
                {
                    body << ",";
                }
                body << CsvEscape(columnNames[c]);
            }
            body << "\n";

            for (auto const& row : rows)
            {
                for (size_t c = 0; c < row.size(); ++c)
                {
                    if (c > 0)
                    {
                        body << ",";
                    }
                    body << CsvEscape(row[c]);
                }
                body << "\n";
            }
        }

        std::string writeError;
        if (!EngineCore::AtomicWriteFile(outputPath, body.str(), writeError))
        {
            taskState.m_LastErrorMessage = "Cannot write output file: " + writeError;
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[snowflake] task='{}' workflow='{}' run='{}': write output failed: {} path='{}'",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, writeError,
                          outputPath.string());
            return false;
        }

        // Build summary for captured stdout
        std::string summary = "{\"ok\":true,\"rows\":" + std::to_string(rows.size()) +
                              ",\"columns\":" + std::to_string(columnNames.size()) +
                              ",\"output\":\"" + outputFile + "\"}";

        taskState.m_CapturedStdout = summary.substr(0, std::min(summary.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write raw response to response.json
        WriteResponseJson(workDir, taskState, responseBody);

        return true;
    }
} // namespace AIAssistant
