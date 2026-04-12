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
#include <sstream>

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/azureBlobCloudTaskExecutor.h"
#include "cloud/azureBlobConnector.h"
#include "cloud/azureSharedKeySigner.h"
#include "curlWrapper/curlWrapper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr long kTimeoutSeconds = 300;
    static constexpr curl_off_t kMaxDownloadBytes = 256 * 1024 * 1024; // 256 MB

    // Helper: set auth headers on a curl_slist based on credential type
    static struct curl_slist* SetAuthHeaders(struct curl_slist* headers, std::string const& method,
                                              std::string const& url, CloudCredentials const& credentials,
                                              std::map<std::string, std::string> const& extraHeaders = {},
                                              size_t contentLength = 0)
    {
        if (credentials.m_AuthType == CloudAuthType::AzureSharedKey)
        {
            auto signed_ =
                AzureSharedKeySigner::Sign(method, url, credentials.m_AccessKeyId, credentials.m_SecretKey,
                                           extraHeaders, contentLength);
            for (auto const& [key, value] : signed_.m_Headers)
            {
                headers = curl_slist_append(headers, (key + ": " + value).c_str());
            }
        }
        else if (credentials.m_AuthType == CloudAuthType::OAuth2)
        {
            headers = curl_slist_append(headers, ("Authorization: Bearer " + credentials.m_Token).c_str());
            headers = curl_slist_append(headers, "x-ms-version: 2024-11-04");
        }
        return headers;
    }

    // Helper: perform Azure Blob HTTP request
    static bool AzureBlobRequest(std::string const& method, std::string const& url,
                                  CloudCredentials const& credentials, std::string& responseBody, long& httpCode,
                                  std::map<std::string, std::string> const& extraHeaders = {},
                                  char const* uploadData = nullptr, size_t uploadSize = 0)
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
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(uploadSize));
        }

        struct curl_slist* headers = nullptr;
        headers = SetAuthHeaders(headers, method, url, credentials, extraHeaders,
                                  (uploadData && uploadSize > 0) ? uploadSize : 0);

        for (auto const& [key, value] : extraHeaders)
        {
            headers = curl_slist_append(headers, (key + ": " + value).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    // Helper: download blob to a file
    static bool AzureBlobDownload(std::string const& url, CloudCredentials const& credentials,
                                   std::string const& outputPath, std::string& errorMessage)
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
        headers = SetAuthHeaders(headers, "GET", url, credentials);
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
            errorMessage = "Azure Blob download failed: HTTP " + std::to_string(httpCode);
            return false;
        }

        return true;
    }

    bool AzureBlobCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
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
            taskState.m_LastErrorMessage = "Failed to parse azure_blob task params JSON";
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
            taskState.m_LastErrorMessage = "Missing required 'operation' in azure_blob task params (upload or download)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        bool isUpload = (operation == "upload");
        bool isDownload = (operation == "download");

        if (!isUpload && !isDownload)
        {
            taskState.m_LastErrorMessage =
                "Unknown azure_blob operation '" + operation + "'. Valid: upload, download";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Container: from params, or fall back to connection's default
        std::string container = getStringParam("container");
        if (container.empty())
        {
            auto containerIt = connection.m_Params.find("container");
            if (containerIt != connection.m_Params.end())
            {
                container = containerIt->second;
            }
        }

        if (container.empty())
        {
            taskState.m_LastErrorMessage = "No container specified (neither in task params nor connection)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string blobName = getStringParam("blob_name");
        std::string localPath = getStringParam("local_path");

        if (blobName.empty() || localPath.empty())
        {
            taskState.m_LastErrorMessage = "azure_blob task requires 'blob_name' and 'local_path'";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string endpointUrl = AzureBlobConnector::BuildEndpointUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        if (isUpload)
        {
            // Read file into memory
            std::ifstream file(localPath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open file for upload: " + localPath;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            auto fileSize = file.tellg();
            file.seekg(0, std::ios::beg);
            std::string fileData(static_cast<size_t>(fileSize), '\0');
            file.read(fileData.data(), fileSize);
            file.close();

            std::string url = endpointUrl + "/" + container + "/" + blobName;

            std::map<std::string, std::string> extraHeaders;
            extraHeaders["x-ms-blob-type"] = "BlockBlob";
            extraHeaders["Content-Type"] = "application/octet-stream";

            if (!AzureBlobRequest("PUT", url, credentials, responseBody, httpCode, extraHeaders, fileData.data(),
                                   fileData.size()))
            {
                taskState.m_LastErrorMessage = "Azure Blob upload request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "Azure Blob upload failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"upload\",\"container\":\"" + container +
                           "\",\"blob_name\":\"" + blobName + "\"}";
            LOG_APP_INFO("[azure_blob] uploaded {} to {}/{}", localPath, container, blobName);
        }
        else // isDownload
        {
            // Ensure output directory exists
            std::filesystem::path outputDir = std::filesystem::path(localPath).parent_path();
            if (!outputDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(outputDir, ec);
            }

            std::string url = endpointUrl + "/" + container + "/" + blobName;
            std::string downloadError;

            if (!AzureBlobDownload(url, credentials, localPath, downloadError))
            {
                taskState.m_LastErrorMessage = "Azure Blob download failed: " + downloadError;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"download\",\"container\":\"" + container +
                           "\",\"blob_name\":\"" + blobName + "\"}";
            LOG_APP_INFO("[azure_blob] downloaded {}/{} to {}", container, blobName, localPath);
        }

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
