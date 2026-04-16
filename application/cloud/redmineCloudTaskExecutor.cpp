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
#include "cloud/redmineCloudTaskExecutor.h"
#include "cloud/redmineConnector.h"
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
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    }

    static std::string ReadFileToString(std::string const& path)
    {
        std::ifstream ifs(path);
        if (!ifs.is_open())
        {
            return {};
        }
        std::stringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    static std::string TrimWhitespace(std::string s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        {
            s.pop_back();
        }
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
        {
            ++start;
        }
        return s.substr(start);
    }

    static bool RedmineRequest(std::string const& method, std::string const& url, std::string const& apiKey,
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

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
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
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(requestBody.size()));
        }

        struct curl_slist* headers = nullptr;
        std::string apiKeyHeader = "X-Redmine-API-Key: " + apiKey;
        headers = curl_slist_append(headers, apiKeyHeader.c_str());
        headers = curl_slist_append(headers, "Accept: application/json");
        if (!requestBody.empty())
        {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    bool RedmineCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
                                                WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                                                TaskInstanceState& taskState, CloudConnection const& connection,
                                                CloudCredentials const& credentials,
                                                TaskCancellationToken const& cancellationToken)
    {
        (void)workflowRun;
        (void)cancellationToken;

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        if (parser.iterate(paddedJson).get(doc))
        {
            taskState.m_LastErrorMessage = "Failed to parse redmine_issue task params JSON";
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
            taskState.m_LastErrorMessage = "Missing required 'operation' in redmine_issue task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string baseUrl = RedmineConnector::GetBaseUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        if (operation == "list_issues")
        {
            std::string projectIdent = getStringParam("project_identifier");
            if (projectIdent.empty())
            {
                auto it = connection.m_Params.find("project_identifier");
                if (it != connection.m_Params.end())
                {
                    projectIdent = it->second;
                }
            }
            if (projectIdent.empty())
            {
                taskState.m_LastErrorMessage =
                    "redmine_issue/list_issues requires 'project_identifier' (in task params or connection params)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::string status = getStringParam("status");
            if (status.empty())
            {
                status = "open";
            }
            int64_t limitVal = 25;
            if (doc["limit"].get_int64().get(limitVal) != simdjson::SUCCESS)
            {
                limitVal = 25;
            }

            std::string url = baseUrl + "/issues.json?project_id=" + projectIdent + "&status_id=" + status +
                              "&limit=" + std::to_string(limitVal);

            if (!RedmineRequest("GET", url, credentials.m_Token, responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "Redmine list_issues request failed (curl error)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Redmine list_issues failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[redmine] listed issues for project '{}' (status={}, limit={})", projectIdent, status, limitVal);
        }
        else if (operation == "update_issue")
        {
            std::string issueId = getStringParam("issue_id");
            if (issueId.empty())
            {
                int64_t intId = 0;
                if (doc["issue_id"].get_int64().get(intId) == simdjson::SUCCESS)
                {
                    issueId = std::to_string(intId);
                }
            }
            if (issueId.empty())
            {
                taskState.m_LastErrorMessage = "Missing required 'issue_id' in redmine_issue/update_issue task params";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Resolve notes (inline or from file)
            std::string notes = getStringParam("notes");
            std::string notesFile = getStringParam("notes_file");
            if (!notesFile.empty())
            {
                std::string loaded = ReadFileToString(notesFile);
                if (loaded.empty())
                {
                    taskState.m_LastErrorMessage = "redmine_issue/update_issue: cannot open notes_file '" + notesFile + "'";
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    return false;
                }
                notes = TrimWhitespace(std::move(loaded));
            }

            // Resolve assigned_to_id (inline string, inline int, or from file)
            std::string assignedToId = getStringParam("assigned_to_id");
            if (assignedToId.empty())
            {
                int64_t intAssignee = 0;
                if (doc["assigned_to_id"].get_int64().get(intAssignee) == simdjson::SUCCESS)
                {
                    assignedToId = std::to_string(intAssignee);
                }
            }
            std::string assignedToIdFile = getStringParam("assigned_to_id_file");
            if (!assignedToIdFile.empty())
            {
                std::string loaded = ReadFileToString(assignedToIdFile);
                assignedToId = TrimWhitespace(std::move(loaded));
            }

            if (notes.empty() && assignedToId.empty())
            {
                taskState.m_LastErrorMessage =
                    "redmine_issue/update_issue requires at least one of: notes, notes_file, assigned_to_id, assigned_to_id_file";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Build the JSON body: {"issue":{"notes":"...","assigned_to_id":N}}
            std::ostringstream body;
            body << "{\"issue\":{";
            bool first = true;
            if (!notes.empty())
            {
                body << "\"notes\":\"" << JsonEscape(notes) << "\"";
                first = false;
            }
            if (!assignedToId.empty())
            {
                if (!first) body << ",";
                // assigned_to_id is a numeric field in Redmine — emit unquoted if it parses as a number
                bool isNumeric = !assignedToId.empty() &&
                                 std::all_of(assignedToId.begin(), assignedToId.end(),
                                             [](char c) { return c >= '0' && c <= '9'; });
                if (isNumeric)
                {
                    body << "\"assigned_to_id\":" << assignedToId;
                }
                else
                {
                    body << "\"assigned_to_id\":\"" << JsonEscape(assignedToId) << "\"";
                }
            }
            body << "}}";
            std::string requestBody = body.str();

            std::string url = baseUrl + "/issues/" + issueId + ".json";

            if (!RedmineRequest("PUT", url, credentials.m_Token, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "Redmine update_issue request failed (curl error)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Redmine update_issue failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Redmine returns 204 No Content with empty body on a successful PUT — synthesize a useful payload
            if (responseBody.empty())
            {
                responseBody = "{\"ok\":true,\"operation\":\"update_issue\",\"issue_id\":" + issueId + "}";
            }

            LOG_APP_INFO("[redmine] updated issue #{} (notes={} chars, assigned_to_id='{}')", issueId, notes.size(),
                         assignedToId);
        }
        else
        {
            taskState.m_LastErrorMessage =
                "Unknown redmine_issue operation '" + operation + "'. Valid: list_issues, update_issue";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        taskState.m_CapturedStdout = responseBody.substr(0, std::min(responseBody.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        if (!workflowBaseDir.empty())
        {
            std::filesystem::path workDir =
                TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);
            WriteResponseJson(workDir, taskState, responseBody);
        }

        return true;
    }
} // namespace AIAssistant
