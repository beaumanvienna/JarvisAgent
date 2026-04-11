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
#include "cloud/slackCloudTaskExecutor.h"
#include "cloud/slackConnector.h"
#include "curlWrapper/curlWrapper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;

    // JSON-escape a string for embedding in a JSON body
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

    bool SlackCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
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
        if (text.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'text' in slack_message task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // POST /api/chat.postMessage
        std::string url = SlackConnector::GetApiBaseUrl(connection) + "/chat.postMessage";
        std::string requestBody = "{\"channel\":\"" + JsonEscape(channel) +
                                  "\",\"text\":\"" + JsonEscape(text) + "\"}";

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            taskState.m_LastErrorMessage = "curl_easy_init() failed";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string responseBody;
        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            buf->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        };
        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + credentials.m_Token;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            taskState.m_LastErrorMessage = std::string("Slack postMessage failed: ") + curl_easy_strerror(res);
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        if (httpCode >= 400)
        {
            taskState.m_LastErrorMessage = "Slack postMessage failed: HTTP " + std::to_string(httpCode);
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Slack returns {"ok": true/false} on HTTP 200
        simdjson::ondemand::parser respParser;
        simdjson::padded_string respPadded(responseBody);
        simdjson::ondemand::document respDoc;

        if (!respParser.iterate(respPadded).get(respDoc))
        {
            bool ok = false;
            if (!respDoc["ok"].get_bool().get(ok) && !ok)
            {
                std::string_view slackError;
                auto err = respDoc["error"].get_string().get(slackError);
                (void)err;
                taskState.m_LastErrorMessage =
                    "Slack postMessage failed: " + (slackError.empty() ? std::string("unknown error") : std::string(slackError));
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
        }

        LOG_APP_INFO("[slack] message sent to {} via connection '{}'", channel, connection.m_Name);

        taskState.m_CapturedStdout = responseBody.substr(0, std::min(responseBody.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write response to task working directory
        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        if (!workflowBaseDir.empty())
        {
            std::filesystem::path workDir =
                TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

            std::error_code ec;
            std::filesystem::create_directories(workDir, ec);

            std::ofstream responseFile(workDir / "response.json", std::ios::trunc);
            if (responseFile.is_open())
            {
                responseFile << responseBody;
            }
        }

        return true;
    }
} // namespace AIAssistant
