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

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/oneDriveCloudTaskExecutor.h"
#include "cloud/oneDriveConnector.h"
#include "curlWrapper/curlWrapper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr long kTimeoutSeconds = 300;        // 5 minutes for large files
    static constexpr curl_off_t kMaxDownloadBytes = 256 * 1024 * 1024; // 256 MB safety limit

    // Helper: perform an authenticated Graph API request
    static bool GraphRequest(std::string const& method, std::string const& url, std::string const& bearerToken,
                             std::string& responseBody, long& httpCode,
                             char const* uploadData = nullptr, size_t uploadSize = 0,
                             std::string const& contentType = {})
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        if (uploadData && uploadSize > 0)
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, uploadData);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(uploadSize));
        }

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + bearerToken;
        headers = curl_slist_append(headers, authHeader.c_str());
        if (!contentType.empty())
        {
            headers = curl_slist_append(headers, ("Content-Type: " + contentType).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    // Helper: download a file from Graph API to disk
    static bool GraphDownload(std::string const& url, std::string const& bearerToken, std::string const& outputPath,
                              std::string& errorMessage)
    {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        FILE* fp = std::fopen(outputPath.c_str(), "wb");
        if (!fp)
        {
            curl_easy_cleanup(curl);
            errorMessage = "Cannot open output file: " + outputPath;
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, kMaxDownloadBytes);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + bearerToken;
        headers = curl_slist_append(headers, authHeader.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        std::fclose(fp);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("curl error: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "OneDrive download failed: HTTP " + std::to_string(httpCode);
            return false;
        }

        return true;
    }

    bool OneDriveCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
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
            taskState.m_LastErrorMessage = "Failed to parse onedrive task params JSON";
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
            taskState.m_LastErrorMessage = "Missing required 'operation' in onedrive task params (upload or download)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string remotePath = getStringParam("remote_path");
        if (remotePath.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'remote_path' in onedrive task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string localPath = getStringParam("local_path");
        if (localPath.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'local_path' in onedrive task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string graphBase = OneDriveConnector::GetGraphBaseUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        if (operation == "upload")
        {
            // Resolve local_path relative to task working directory
            std::filesystem::path workflowBaseDir =
                TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
            std::filesystem::path workDir =
                TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);
            std::filesystem::path fullLocalPath = workDir / localPath;

            // Read file into memory
            std::ifstream file(fullLocalPath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open file for upload: " + fullLocalPath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            auto fileSize = file.tellg();
            file.seekg(0, std::ios::beg);
            std::string fileData(static_cast<size_t>(fileSize), '\0');
            file.read(fileData.data(), fileSize);
            file.close();

            // PUT /me/drive/root:/{remote_path}:/content
            std::string url = graphBase + "/me/drive/root:/" + remotePath + ":/content";

            if (!GraphRequest("PUT", url, credentials.m_Token, responseBody, httpCode, fileData.data(),
                              fileData.size(), "application/octet-stream"))
            {
                taskState.m_LastErrorMessage = "OneDrive upload request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "OneDrive upload failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[onedrive] uploaded {} to onedrive:/{}", fullLocalPath.string(), remotePath);
        }
        else if (operation == "download")
        {
            // Resolve local_path relative to task working directory
            std::filesystem::path workflowBaseDir =
                TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
            std::filesystem::path workDir =
                TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);
            std::filesystem::path fullLocalPath = workDir / localPath;

            // Ensure output directory exists
            std::filesystem::path outputDir = fullLocalPath.parent_path();
            if (!outputDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(outputDir, ec);
            }

            // GET /me/drive/root:/{remote_path}:/content
            std::string url = graphBase + "/me/drive/root:/" + remotePath + ":/content";
            std::string downloadError;

            if (!GraphDownload(url, credentials.m_Token, fullLocalPath.string(), downloadError))
            {
                taskState.m_LastErrorMessage = "OneDrive download failed: " + downloadError;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"download\",\"remote_path\":\"" + remotePath + "\"}";
            LOG_APP_INFO("[onedrive] downloaded onedrive:/{} to {}", remotePath, fullLocalPath.string());
        }
        else
        {
            taskState.m_LastErrorMessage =
                "Unknown onedrive operation '" + operation + "'. Valid: upload, download";
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
