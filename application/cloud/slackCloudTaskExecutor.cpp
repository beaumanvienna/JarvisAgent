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

#include "auxiliary/file.h"
#include "engine.h"
#include "cloud/slackCloudTaskExecutor.h"
#include "cloud/slackConnector.h"
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlWrapper.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;

    // Read entire file into a string. Returns empty string on failure.
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

    // Trim trailing whitespace/newlines
    static std::string TrimTrailing(std::string s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        {
            s.pop_back();
        }
        return s;
    }

    // libcurl write callback
    static size_t WriteBodyCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        auto* buf = static_cast<std::string*>(userp);
        buf->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    // Shared HTTP helper: performs a request to the Slack Web API with Bearer auth.
    // Returns true on CURLE_OK (httpCode set). On failure, populates taskState.m_LastErrorMessage.
    static bool PerformSlackRequest(std::string const& url, std::string const& postBody, std::string const& token,
                                    std::string& responseBody, long& httpCode, TaskInstanceState& taskState,
                                    char const* contextLabel)
    {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            taskState.m_LastErrorMessage = std::string(contextLabel) + ": curl_easy_init() failed";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (!postBody.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody.size()));
        }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBodyCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        // Hardened TLS + no redirect-following — Slack Web API at
        // `slack.com/api/*` responds directly for all endpoints.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            taskState.m_LastErrorMessage = std::string(contextLabel) + " failed: " + curl_easy_strerror(res);
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        if (httpCode >= 400)
        {
            taskState.m_LastErrorMessage =
                std::string(contextLabel) + " failed: HTTP " + std::to_string(httpCode);
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        return true;
    }

    // Check Slack's {"ok": true/false} envelope. Returns false and sets error on ok:false.
    static bool CheckSlackOk(std::string const& responseBody, TaskInstanceState& taskState, char const* contextLabel)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(responseBody);
        simdjson::ondemand::document doc;
        if (parser.iterate(padded).get(doc))
        {
            taskState.m_LastErrorMessage = std::string(contextLabel) + ": failed to parse Slack response JSON";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        bool ok = false;
        if (doc["ok"].get_bool().get(ok) != simdjson::SUCCESS || !ok)
        {
            // Re-parse to read the error field (simdjson ondemand doesn't allow random reads)
            simdjson::ondemand::parser parser2;
            simdjson::padded_string padded2(responseBody);
            simdjson::ondemand::document doc2;
            std::string slackError = "unknown error";
            if (!parser2.iterate(padded2).get(doc2))
            {
                std::string_view sv;
                if (doc2["error"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    slackError = std::string(sv);
                }
            }
            taskState.m_LastErrorMessage = std::string(contextLabel) + " failed: " + slackError;
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        return true;
    }

    bool SlackCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
                                              WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                                              TaskInstanceState& taskState, CloudConnection const& connection,
                                              CloudCredentials const& credentials,
                                              TaskCancellationToken const& cancellationToken)
    {
        (void)workflowRun;
        (void)cancellationToken;

        if (taskDefinition.m_Type == TaskType::SlackRead)
        {
            return ExecuteSlackRead(workflowDefinition, taskDefinition, taskState, connection, credentials);
        }
        return ExecuteSlackPost(workflowDefinition, taskDefinition, taskState, connection, credentials);
    }

    bool SlackCloudTaskExecutor::ExecuteSlackPost(WorkflowDefinition const& workflowDefinition,
                                                  TaskDef const& taskDefinition, TaskInstanceState& taskState,
                                                  CloudConnection const& connection,
                                                  CloudCredentials const& credentials)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            taskState.m_LastErrorMessage = "Failed to parse slack_message task params JSON";
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

        std::string channel = getStringParam("channel");
        if (channel.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'channel' in slack_message task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string text = getStringParam("text");
        std::string textFile = getStringParam("text_file");
        if (!textFile.empty())
        {
            // Containment gate: a hostile JCWF or AI-supplied text_file path
            // (e.g. /etc/passwd, ../../etc/shadow) must not be read.  Same
            // pattern as jira/redmine *_file params.
            std::filesystem::path const confined = ConfineUnderProjectRoot(textFile);
            if (confined.empty())
            {
                taskState.m_LastErrorMessage =
                    "slack_message: text_file does not lie under the project root: " + textFile;
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[slack] text_file rejected task='{}' workflow='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, textFile);
                return false;
            }
            std::string loaded = ReadFileToString(confined.string());
            if (loaded.empty())
            {
                taskState.m_LastErrorMessage = "slack_message: cannot open or empty text_file '" + textFile + "'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[slack] text_file not readable task='{}' workflow='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, textFile);
                return false;
            }
            text = TrimTrailing(std::move(loaded));
        }
        if (text.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'text' or 'text_file' in slack_message task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string threadTs = getStringParam("thread_ts");
        std::string threadTsFile = getStringParam("thread_ts_file");
        if (!threadTsFile.empty())
        {
            std::filesystem::path const confined = ConfineUnderProjectRoot(threadTsFile);
            if (confined.empty())
            {
                taskState.m_LastErrorMessage =
                    "slack_message: thread_ts_file does not lie under the project root: " + threadTsFile;
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[slack] thread_ts_file rejected task='{}' workflow='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, threadTsFile);
                return false;
            }
            std::string loaded = ReadFileToString(confined.string());
            threadTs = TrimTrailing(std::move(loaded));
        }

        // POST /api/chat.postMessage
        std::string url = SlackConnector::GetApiBaseUrl(connection) + "/chat.postMessage";
        std::string requestBody = "{\"channel\":\"" + JsonHelper::EscapeJsonString(channel) +
                                  "\",\"text\":\"" + JsonHelper::EscapeJsonString(text) + "\"";
        if (!threadTs.empty())
        {
            requestBody += ",\"thread_ts\":\"" + JsonHelper::EscapeJsonString(threadTs) + "\"";
        }
        requestBody += "}";

        std::string responseBody;
        long httpCode = 0;
        if (!PerformSlackRequest(url, requestBody, credentials.m_Token, responseBody, httpCode, taskState,
                                 "Slack postMessage"))
        {
            return false;
        }

        if (!CheckSlackOk(responseBody, taskState, "Slack postMessage"))
        {
            return false;
        }

        LOG_APP_INFO("[slack] message sent to {} via connection '{}' (thread_ts='{}')", channel, connection.m_Name,
                     threadTs);

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

    bool SlackCloudTaskExecutor::ExecuteSlackRead(WorkflowDefinition const& workflowDefinition,
                                                  TaskDef const& taskDefinition, TaskInstanceState& taskState,
                                                  CloudConnection const& connection,
                                                  CloudCredentials const& credentials)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        if (parser.iterate(paddedJson).get(doc))
        {
            taskState.m_LastErrorMessage = "Failed to parse slack_read task params JSON";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string channel;
        std::string oldest;
        int64_t limit = 10;
        bool excludeBots = true;

        {
            std::string_view sv;
            if (doc["channel"].get_string().get(sv) == simdjson::SUCCESS)
            {
                channel = std::string(sv);
            }
            if (doc["oldest"].get_string().get(sv) == simdjson::SUCCESS)
            {
                oldest = std::string(sv);
            }
            int64_t limitVal = 0;
            if (doc["limit"].get_int64().get(limitVal) == simdjson::SUCCESS)
            {
                limit = limitVal;
            }
            bool exVal = true;
            if (doc["exclude_bots"].get_bool().get(exVal) == simdjson::SUCCESS)
            {
                excludeBots = exVal;
            }
        }

        if (channel.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'channel' in slack_read task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }
        if (channel[0] == '#')
        {
            taskState.m_LastErrorMessage =
                "slack_read 'channel' must be a channel ID (e.g. C01ABCDEF), not a name like '" + channel + "'";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // GET /api/conversations.history?channel=...&limit=...[&oldest=...]
        std::string url = SlackConnector::GetApiBaseUrl(connection) + "/conversations.history?channel=" + channel +
                          "&limit=" + std::to_string(limit);
        if (!oldest.empty())
        {
            url += "&oldest=" + oldest;
        }

        std::string responseBody;
        long httpCode = 0;
        if (!PerformSlackRequest(url, /*postBody*/ "", credentials.m_Token, responseBody, httpCode, taskState,
                                 "Slack conversations.history"))
        {
            return false;
        }
        if (!CheckSlackOk(responseBody, taskState, "Slack conversations.history"))
        {
            return false;
        }

        // Parse messages array
        struct SlackMsg
        {
            std::string m_Ts;
            std::string m_User;
            std::string m_Text;
            bool m_IsBot = false;
        };
        std::vector<SlackMsg> messages;

        {
            simdjson::ondemand::parser p2;
            simdjson::padded_string padded2(responseBody);
            simdjson::ondemand::document d2;
            if (p2.iterate(padded2).get(d2))
            {
                taskState.m_LastErrorMessage = "slack_read: failed to re-parse response JSON";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            simdjson::ondemand::array arr;
            if (d2["messages"].get_array().get(arr) == simdjson::SUCCESS)
            {
                for (auto elem : arr)
                {
                    SlackMsg m;
                    std::string_view sv;
                    if (elem["ts"].get_string().get(sv) == simdjson::SUCCESS)
                    {
                        m.m_Ts = std::string(sv);
                    }
                    if (elem["user"].get_string().get(sv) == simdjson::SUCCESS)
                    {
                        m.m_User = std::string(sv);
                    }
                    if (elem["text"].get_string().get(sv) == simdjson::SUCCESS)
                    {
                        m.m_Text = std::string(sv);
                    }
                    std::string_view botId;
                    if (elem["bot_id"].get_string().get(botId) == simdjson::SUCCESS && !botId.empty())
                    {
                        m.m_IsBot = true;
                    }
                    if (excludeBots && m.m_IsBot)
                    {
                        continue;
                    }
                    if (m.m_Ts.empty() || m.m_Text.empty())
                    {
                        continue;
                    }
                    messages.push_back(std::move(m));
                }
            }
        }

        // Slack returns messages newest-first. After filtering, messages[0] is the latest.
        std::string latestTs;
        std::string latestText;
        if (!messages.empty())
        {
            latestTs = messages.front().m_Ts;
            latestText = messages.front().m_Text;
        }

        // Build summary JSON (array of {ts, user, text})
        std::ostringstream summary;
        summary << "[";
        for (size_t i = 0; i < messages.size(); ++i)
        {
            if (i > 0) summary << ",";
            summary << "{\"ts\":\"" << JsonHelper::EscapeJsonString(messages[i].m_Ts) << "\""
                    << ",\"user\":\"" << JsonHelper::EscapeJsonString(messages[i].m_User) << "\""
                    << ",\"text\":\"" << JsonHelper::EscapeJsonString(messages[i].m_Text) << "\"}";
        }
        summary << "]";
        std::string summaryStr = summary.str();

        taskState.m_CapturedStdout = summaryStr.substr(0, std::min(summaryStr.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write outputs to task working directory
        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        if (!workflowBaseDir.empty())
        {
            std::filesystem::path workDir =
                TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

            auto writeOutput = [](std::filesystem::path const& path, std::string const& body, char const* label) {
                std::string err;
                if (!EngineCore::AtomicWriteFile(path, body, err))
                {
                    LOG_APP_ERROR("[slack_read] write {} failed: {} path='{}'", label, err, path.string());
                }
            };
            writeOutput(workDir / "messages_summary.json", summaryStr, "messages_summary.json");
            writeOutput(workDir / "latest_message.txt", latestText, "latest_message.txt");
            writeOutput(workDir / "latest_ts.txt", latestTs, "latest_ts.txt");

            std::string responseJson = "{\"ok\":true,\"count\":" + std::to_string(messages.size()) +
                                       ",\"channel\":\"" + JsonHelper::EscapeJsonString(channel) + "\"" +
                                       ",\"latest_ts\":\"" + JsonHelper::EscapeJsonString(latestTs) + "\"}";
            WriteResponseJson(workDir, taskState, responseJson);
        }

        LOG_APP_INFO("[slack_read] fetched {} message(s) from {} via connection '{}' (latest_ts='{}')",
                     messages.size(), channel, connection.m_Name, latestTs);

        return true;
    }
} // namespace AIAssistant
