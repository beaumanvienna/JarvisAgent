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

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/jiraCloudTaskExecutor.h"
#include "cloud/jiraConnector.h"
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlWrapper.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;

    namespace
    {
        // Jira issue keys: `^[A-Z][A-Z0-9_]+-\d+$`.  Project key is an
        // uppercase short-prefix (2-10 chars typically); issue number is
        // a positive decimal.  Caps: 32-char prefix + `-` + 10-digit ID =
        // 43 max to stay well within sane URL component lengths.
        bool IsValidJiraIssueKey(std::string_view value)
        {
            if (value.empty() || value.size() > 43)
            {
                return false;
            }
            size_t i = 0;
            if (!(value[i] >= 'A' && value[i] <= 'Z'))
            {
                return false;
            }
            ++i;
            // Prefix: uppercase + digits + underscore, then a single '-'.
            while (i < value.size())
            {
                char c = value[i];
                if (c == '-')
                {
                    break;
                }
                bool const ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                if (!ok)
                {
                    return false;
                }
                ++i;
            }
            if (i == value.size() || value[i] != '-' || i < 2 || i > 32)
            {
                // Prefix must be at least 2 chars (first is upper-alpha + at
                // least one more from the prefix set) and at most 32.
                return false;
            }
            ++i; // skip '-'
            if (i == value.size())
            {
                return false; // need at least one digit
            }
            for (size_t suffixLen = 0; i < value.size(); ++i, ++suffixLen)
            {
                if (suffixLen >= 10)
                {
                    return false;
                }
                if (value[i] < '0' || value[i] > '9')
                {
                    return false;
                }
            }
            return true;
        }

        // Jira project keys: `^[A-Z][A-Z0-9_]+$`, 2-32 chars.  Same charset
        // as the prefix portion of the issue key above; reused for the bare
        // project_key param that lands in JSON body (not URL), but kept as a
        // structural allowlist so the value can't smuggle JSON-injection
        // shapes either (defense-in-depth alongside JsonHelper::Escape).
        bool IsValidJiraProjectKey(std::string_view value)
        {
            if (value.size() < 2 || value.size() > 32)
            {
                return false;
            }
            if (!(value[0] >= 'A' && value[0] <= 'Z'))
            {
                return false;
            }
            for (size_t i = 1; i < value.size(); ++i)
            {
                char c = value[i];
                bool const ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }

        // Jira transition IDs are positive integers in the Atlassian REST API.
        bool IsValidJiraTransitionId(std::string_view value)
        {
            if (value.empty() || value.size() > 10)
            {
                return false;
            }
            for (char c : value)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    static bool JiraRequest(std::string const& method, std::string const& url,
                            CloudCredentials const& credentials, std::string& responseBody, long& httpCode,
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        // Hardened TLS + no redirect-following — Atlassian Jira REST at the
        // user's `*.atlassian.net` host responds directly for `/rest/api/3/*`
        // endpoints; legitimate flows never 30x.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        if (!requestBody.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        }

        struct curl_slist* headers = nullptr;
        if (credentials.m_AuthType == CloudAuthType::BasicAuth)
        {
            curl_easy_setopt(curl, CURLOPT_USERNAME, credentials.m_Username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, credentials.m_Password.c_str());
        }
        else
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

    bool JiraCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
                                             WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                                             TaskInstanceState& taskState, CloudConnection const& connection,
                                             CloudCredentials const& credentials,
                                             TaskCancellationToken const& cancellationToken)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            taskState.m_LastErrorMessage = "Failed to parse jira_issue task params JSON";
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
            taskState.m_LastErrorMessage = "Missing required 'operation' in jira_issue task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string baseUrl = JiraConnector::GetBaseUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        if (operation == "create")
        {
            std::string projectKey = getStringParam("project_key");
            if (projectKey.empty())
            {
                auto it = connection.m_Params.find("project_key");
                if (it != connection.m_Params.end())
                {
                    projectKey = it->second;
                }
            }
            if (projectKey.empty())
            {
                taskState.m_LastErrorMessage = "jira_issue 'create' requires 'project_key'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (!IsValidJiraProjectKey(projectKey))
            {
                taskState.m_LastErrorMessage = "jira_issue 'create': invalid 'project_key' (must match "
                                               "[A-Z][A-Z0-9_]+, 2-32 chars)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[jira] invalid project_key task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, workflowRun.m_RunId, projectKey);
                return false;
            }

            std::string summary = getStringParam("summary");
            if (summary.empty())
            {
                taskState.m_LastErrorMessage = "jira_issue 'create' requires 'summary'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::string issueType = getStringParam("issue_type");
            if (issueType.empty()) issueType = "Task";

            std::string description = getStringParam("description");
            std::string priority = getStringParam("priority");

            // description_file: read description from a file (takes precedence over inline
            // description).  Mirrors polarion_write's field_value_file, enabling per-task
            // AI output piping via {{upstreamTask.output_file}}.
            std::string descriptionFile = getStringParam("description_file");
            if (!descriptionFile.empty())
            {
                // Containment gate: a JCWF or AI-supplied `description_file`
                // value of `/etc/shadow` or `../../etc/passwd` must not be
                // read.  ConfineUnderProjectRoot is the canonical project-root
                // boundary per `feedback_path_containment_scope`.
                std::filesystem::path const confined = ConfineUnderProjectRoot(descriptionFile);
                if (confined.empty())
                {
                    taskState.m_LastErrorMessage =
                        "jira_issue 'create': description_file does not lie under the project root: " +
                        descriptionFile;
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    LOG_APP_ERROR("[jira] description_file rejected task='{}' workflow='{}' run='{}': '{}'",
                                  taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, descriptionFile);
                    return false;
                }
                std::ifstream in(confined);
                if (!in.is_open())
                {
                    taskState.m_LastErrorMessage =
                        "jira_issue 'create': description_file not readable: " + descriptionFile;
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    LOG_APP_ERROR("[jira] description_file not readable task='{}' workflow='{}' run='{}': '{}'",
                                  taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, descriptionFile);
                    return false;
                }
                std::ostringstream oss;
                oss << in.rdbuf();
                description = oss.str();
                while (!description.empty() && (description.back() == '\n' || description.back() == '\r'))
                {
                    description.pop_back();
                }
            }

            // ADF text nodes cannot contain embedded newlines — collapse to spaces so the
            // single-paragraph description body below stays valid.  Multi-paragraph ADF is a
            // possible future enhancement.
            for (char& c : description)
            {
                if (c == '\n' || c == '\r' || c == '\t')
                {
                    c = ' ';
                }
            }

            // Build Jira REST API v3 create issue body
            std::string requestBody = "{\"fields\":{";
            requestBody += "\"project\":{\"key\":\"" + JsonHelper::EscapeJsonString(projectKey) + "\"}";
            requestBody += ",\"summary\":\"" + JsonHelper::EscapeJsonString(summary) + "\"";
            requestBody += ",\"issuetype\":{\"name\":\"" + JsonHelper::EscapeJsonString(issueType) + "\"}";

            if (!description.empty())
            {
                // Jira v3 uses ADF for description; use simple paragraph
                requestBody += ",\"description\":{\"type\":\"doc\",\"version\":1,\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"" + JsonHelper::EscapeJsonString(description) + "\"}]}]}";
            }

            if (!priority.empty())
            {
                requestBody += ",\"priority\":{\"name\":\"" + JsonHelper::EscapeJsonString(priority) + "\"}";
            }

            // Labels
            simdjson::ondemand::array labelsArray;
            if (!doc["labels"].get_array().get(labelsArray))
            {
                requestBody += ",\"labels\":[";
                bool first = true;
                for (auto label : labelsArray)
                {
                    std::string_view sv;
                    if (label.get_string().get(sv) == simdjson::SUCCESS)
                    {
                        if (!first) requestBody += ",";
                        requestBody += "\"" + JsonHelper::EscapeJsonString(std::string(sv)) + "\"";
                        first = false;
                    }
                }
                requestBody += "]";
            }

            requestBody += "}}";

            std::string url = baseUrl + "/rest/api/3/issue";

            if (!JiraRequest("POST", url, credentials, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "Jira create issue request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Jira create issue failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[jira] created issue in project {}", projectKey);
        }
        else if (operation == "update")
        {
            std::string issueKey = getStringParam("issue_key");
            if (issueKey.empty())
            {
                taskState.m_LastErrorMessage = "jira_issue 'update' requires 'issue_key'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (!IsValidJiraIssueKey(issueKey))
            {
                taskState.m_LastErrorMessage = "jira_issue 'update': invalid 'issue_key' (must match "
                                               "[A-Z][A-Z0-9_]+-\\d+, e.g. 'PROJ-123')";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[jira] invalid issue_key task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, workflowRun.m_RunId, issueKey);
                return false;
            }

            // The 'fields' param is a raw JSON object passed through directly
            std::string fieldsJson = getStringParam("fields");
            if (fieldsJson.empty())
            {
                taskState.m_LastErrorMessage = "jira_issue 'update' requires 'fields' (JSON object)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Defense-in-depth percent-encode after the allowlist: the
            // allowlist already excludes `/`, `?`, `&` from issueKey, but the
            // encode keeps a future allowlist relaxation from silently
            // re-introducing URL injection.
            std::string const issueKeyEnc = ConnectorHttp::UrlEncodeComponent(issueKey);
            std::string requestBody = "{\"fields\":" + fieldsJson + "}";
            std::string url = baseUrl + "/rest/api/3/issue/" + issueKeyEnc;

            if (!JiraRequest("PUT", url, credentials, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "Jira update issue request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Jira update failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (responseBody.empty())
            {
                responseBody = "{\"ok\":true,\"issue_key\":\"" + JsonHelper::EscapeJsonString(issueKey) +
                               "\",\"operation\":\"update\"}";
            }

            LOG_APP_INFO("[jira] updated issue {}", issueKey);
        }
        else if (operation == "transition")
        {
            std::string issueKey = getStringParam("issue_key");
            std::string transitionId = getStringParam("transition_id");
            if (issueKey.empty() || transitionId.empty())
            {
                taskState.m_LastErrorMessage = "jira_issue 'transition' requires 'issue_key' and 'transition_id'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (!IsValidJiraIssueKey(issueKey))
            {
                taskState.m_LastErrorMessage = "jira_issue 'transition': invalid 'issue_key' (must match "
                                               "[A-Z][A-Z0-9_]+-\\d+)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[jira] invalid issue_key task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, workflowRun.m_RunId, issueKey);
                return false;
            }
            if (!IsValidJiraTransitionId(transitionId))
            {
                taskState.m_LastErrorMessage = "jira_issue 'transition': invalid 'transition_id' (must be a positive "
                                               "integer)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[jira] invalid transition_id task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, workflowRun.m_RunId, transitionId);
                return false;
            }

            std::string const issueKeyEnc = ConnectorHttp::UrlEncodeComponent(issueKey);
            std::string requestBody = "{\"transition\":{\"id\":\"" + JsonHelper::EscapeJsonString(transitionId) + "\"}}";
            std::string url = baseUrl + "/rest/api/3/issue/" + issueKeyEnc + "/transitions";

            if (!JiraRequest("POST", url, credentials, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "Jira transition request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Jira transition failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (responseBody.empty())
            {
                responseBody = "{\"ok\":true,\"issue_key\":\"" + JsonHelper::EscapeJsonString(issueKey) +
                               "\",\"transition_id\":\"" + JsonHelper::EscapeJsonString(transitionId) + "\"}";
            }

            LOG_APP_INFO("[jira] transitioned issue {} with transition {}", issueKey, transitionId);
        }
        else if (operation == "comment")
        {
            std::string issueKey = getStringParam("issue_key");
            std::string body = getStringParam("body");
            if (issueKey.empty() || body.empty())
            {
                taskState.m_LastErrorMessage = "jira_issue 'comment' requires 'issue_key' and 'body'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (!IsValidJiraIssueKey(issueKey))
            {
                taskState.m_LastErrorMessage = "jira_issue 'comment': invalid 'issue_key' (must match "
                                               "[A-Z][A-Z0-9_]+-\\d+)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[jira] invalid issue_key task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, workflowRun.m_RunId, issueKey);
                return false;
            }

            std::string const issueKeyEnc = ConnectorHttp::UrlEncodeComponent(issueKey);
            // Jira v3 comment uses ADF format
            std::string requestBody = "{\"body\":{\"type\":\"doc\",\"version\":1,\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"" + JsonHelper::EscapeJsonString(body) + "\"}]}]}}";
            std::string url = baseUrl + "/rest/api/3/issue/" + issueKeyEnc + "/comment";

            if (!JiraRequest("POST", url, credentials, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "Jira comment request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Jira comment failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[jira] commented on issue {}", issueKey);
        }
        else if (operation == "get")
        {
            std::string issueKey = getStringParam("issue_key");
            if (issueKey.empty())
            {
                taskState.m_LastErrorMessage = "jira_issue 'get' requires 'issue_key'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (!IsValidJiraIssueKey(issueKey))
            {
                taskState.m_LastErrorMessage = "jira_issue 'get': invalid 'issue_key' (must match "
                                               "[A-Z][A-Z0-9_]+-\\d+)";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[jira] invalid issue_key task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, workflowRun.m_RunId, issueKey);
                return false;
            }

            std::string const issueKeyEnc = ConnectorHttp::UrlEncodeComponent(issueKey);
            std::string url = baseUrl + "/rest/api/3/issue/" + issueKeyEnc;

            if (!JiraRequest("GET", url, credentials, responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "Jira get issue request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Jira get issue failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[jira] retrieved issue {}", issueKey);
        }
        else
        {
            taskState.m_LastErrorMessage =
                "Unknown jira_issue operation '" + operation + "'. Valid: create, update, transition, comment, get";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        taskState.m_CapturedStdout = responseBody.substr(0, std::min(responseBody.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write response to task working directory
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
