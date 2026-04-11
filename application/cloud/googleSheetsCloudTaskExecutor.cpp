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

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/googleSheetsCloudTaskExecutor.h"
#include "cloud/googleSheetsConnector.h"
#include "curlWrapper/curlWrapper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;

    static std::string JsonEscape(std::string const& input)
    {
        std::string out;
        out.reserve(input.size() + 16);
        for (char c : input)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    // URL-encode a string for query parameters
    static std::string UrlEncode(std::string const& input)
    {
        std::string out;
        out.reserve(input.size() * 3);
        for (unsigned char c : input)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~')
            {
                out += static_cast<char>(c);
            }
            else
            {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                out += buf;
            }
        }
        return out;
    }

    // CSV escape per RFC 4180
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
        if (!needsQuoting) return std::string(value);

        std::string result = "\"";
        for (char c : value)
        {
            if (c == '"') result += "\"\"";
            else result += c;
        }
        result += '"';
        return result;
    }

    // Parse a simple CSV line (no embedded newlines in fields)
    static std::vector<std::string> ParseCsvLine(std::string const& line)
    {
        std::vector<std::string> fields;
        std::string field;
        bool inQuotes = false;

        for (size_t i = 0; i < line.size(); ++i)
        {
            char c = line[i];
            if (inQuotes)
            {
                if (c == '"')
                {
                    if (i + 1 < line.size() && line[i + 1] == '"')
                    {
                        field += '"';
                        ++i;
                    }
                    else
                    {
                        inQuotes = false;
                    }
                }
                else
                {
                    field += c;
                }
            }
            else
            {
                if (c == '"')
                {
                    inQuotes = true;
                }
                else if (c == ',')
                {
                    fields.push_back(std::move(field));
                    field.clear();
                }
                else
                {
                    field += c;
                }
            }
        }
        fields.push_back(std::move(field));
        return fields;
    }

    static bool SheetsRequest(std::string const& method, std::string const& url,
                              CloudCredentials const& credentials, CloudConnection const& connection,
                              std::string& responseBody, long& httpCode, std::string const& requestBody = {})
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

        // For API key auth, append to URL; for OAuth, use Bearer header
        bool useApiKeyParam = (credentials.m_AuthType == CloudAuthType::BearerToken &&
                               connection.m_AuthType != CloudAuthType::OAuth2);
        std::string finalUrl = url;
        if (useApiKeyParam)
        {
            finalUrl += (finalUrl.find('?') != std::string::npos ? "&" : "?");
            finalUrl += "key=" + credentials.m_Token;
        }

        curl_easy_setopt(curl, CURLOPT_URL, finalUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
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
        if (!useApiKeyParam)
        {
            std::string authHeader = "Authorization: Bearer " + credentials.m_Token;
            headers = curl_slist_append(headers, authHeader.c_str());
        }
        headers = curl_slist_append(headers, "Accept: application/json");
        if (!requestBody.empty())
        {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    bool GoogleSheetsCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
                                                     WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                                                     TaskInstanceState& taskState,
                                                     CloudConnection const& connection,
                                                     CloudCredentials const& credentials,
                                                     TaskCancellationToken const& cancellationToken)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            taskState.m_LastErrorMessage = "Failed to parse sheets task params JSON";
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

        std::string operation = getStringParam("operation");
        if (operation.empty())
        {
            // Infer from task type name stored in params, or default to "read"
            operation = "read";
        }

        std::string spreadsheetId = getStringParam("spreadsheet_id");
        if (spreadsheetId.empty())
        {
            auto it = connection.m_Params.find("spreadsheet_id");
            if (it != connection.m_Params.end())
            {
                spreadsheetId = it->second;
            }
        }
        if (spreadsheetId.empty())
        {
            taskState.m_LastErrorMessage = "Missing 'spreadsheet_id' (in task params or connection)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string range = getStringParam("range");
        if (range.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'range' in sheets task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string apiBase = GoogleSheetsConnector::GetApiBaseUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        // Resolve working directory
        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path workDir =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

        {
            std::error_code ec;
            std::filesystem::create_directories(workDir, ec);
        }

        if (operation == "read")
        {
            std::string outputFormat = getStringParam("output_format");
            if (outputFormat.empty()) outputFormat = "csv";

            std::string outputFile = getStringParam("output_file");
            if (outputFile.empty())
            {
                outputFile = (outputFormat == "json") ? "result.json" : "result.csv";
            }

            // GET /{spreadsheetId}/values/{range}
            std::string url = apiBase + "/" + spreadsheetId + "/values/" + UrlEncode(range);

            if (!SheetsRequest("GET", url, credentials, connection, responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "Google Sheets read request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Google Sheets read failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Parse the values array from the response
            simdjson::ondemand::parser respParser;
            simdjson::padded_string respPadded(responseBody);
            simdjson::ondemand::document respDoc;

            std::vector<std::vector<std::string>> rows;
            if (!respParser.iterate(respPadded).get(respDoc))
            {
                simdjson::ondemand::array valuesArray;
                if (!respDoc["values"].get_array().get(valuesArray))
                {
                    for (auto row : valuesArray)
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
                                    rowValues.emplace_back();
                                }
                            }
                        }
                        rows.push_back(std::move(rowValues));
                    }
                }
            }

            // Write output
            std::filesystem::path outputPath = workDir / outputFile;

            if (outputFormat == "json")
            {
                std::ofstream out(outputPath, std::ios::trunc);
                if (!out.is_open())
                {
                    taskState.m_LastErrorMessage = "Cannot open output file: " + outputPath.string();
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    return false;
                }

                // Use first row as headers if available
                std::vector<std::string> headers;
                size_t dataStart = 0;
                if (!rows.empty())
                {
                    headers = rows[0];
                    dataStart = 1;
                }

                out << "[\n";
                for (size_t r = dataStart; r < rows.size(); ++r)
                {
                    out << "  {";
                    for (size_t c = 0; c < headers.size() && c < rows[r].size(); ++c)
                    {
                        if (c > 0) out << ", ";
                        out << "\"" << JsonEscape(headers[c]) << "\": \"" << JsonEscape(rows[r][c]) << "\"";
                    }
                    out << "}" << (r + 1 < rows.size() ? "," : "") << "\n";
                }
                out << "]\n";
            }
            else
            {
                std::ofstream out(outputPath, std::ios::trunc);
                if (!out.is_open())
                {
                    taskState.m_LastErrorMessage = "Cannot open output file: " + outputPath.string();
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    return false;
                }

                for (auto const& row : rows)
                {
                    for (size_t c = 0; c < row.size(); ++c)
                    {
                        if (c > 0) out << ",";
                        out << CsvEscape(row[c]);
                    }
                    out << "\n";
                }
            }

            std::string summary = "{\"ok\":true,\"operation\":\"read\",\"rows\":" + std::to_string(rows.size()) +
                                  ",\"output\":\"" + outputFile + "\"}";
            taskState.m_CapturedStdout = summary.substr(0, std::min(summary.size(), kMaxCaptureChars));

            LOG_APP_INFO("[sheets] read {} rows from range '{}'", rows.size(), range);
        }
        else if (operation == "write")
        {
            std::string inputFile = getStringParam("input_file");
            if (inputFile.empty())
            {
                taskState.m_LastErrorMessage = "sheets_write requires 'input_file'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::string valueInputOption = getStringParam("value_input_option");
            if (valueInputOption.empty()) valueInputOption = "USER_ENTERED";

            // Read CSV from input file
            std::filesystem::path inputPath = workDir / inputFile;
            std::ifstream in(inputPath);
            if (!in.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open input file: " + inputPath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::vector<std::vector<std::string>> values;
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (!line.empty())
                {
                    values.push_back(ParseCsvLine(line));
                }
            }

            // Build the JSON body: {"values": [[...], [...]]}
            std::string requestBody = "{\"values\":[";
            for (size_t r = 0; r < values.size(); ++r)
            {
                if (r > 0) requestBody += ",";
                requestBody += "[";
                for (size_t c = 0; c < values[r].size(); ++c)
                {
                    if (c > 0) requestBody += ",";
                    requestBody += "\"" + JsonEscape(values[r][c]) + "\"";
                }
                requestBody += "]";
            }
            requestBody += "]}";

            // PUT /{spreadsheetId}/values/{range}?valueInputOption=...
            std::string url = apiBase + "/" + spreadsheetId + "/values/" + UrlEncode(range) +
                              "?valueInputOption=" + valueInputOption;

            if (!SheetsRequest("PUT", url, credentials, connection, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "Google Sheets write request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Google Sheets write failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::string summary = "{\"ok\":true,\"operation\":\"write\",\"rows\":" + std::to_string(values.size()) +
                                  ",\"range\":\"" + JsonEscape(range) + "\"}";
            taskState.m_CapturedStdout = summary.substr(0, std::min(summary.size(), kMaxCaptureChars));

            LOG_APP_INFO("[sheets] wrote {} rows to range '{}'", values.size(), range);
        }
        else
        {
            taskState.m_LastErrorMessage =
                "Unknown sheets operation '" + operation + "'. Valid: read, write";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write raw response
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
