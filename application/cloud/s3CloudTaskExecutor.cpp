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
#include "cloud/s3CloudTaskExecutor.h"
#include "cloud/s3Connector.h"
#include "cloud/sigV4Signer.h"
#include "curlWrapper/curlWrapper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr long kTimeoutSeconds = 300;        // 5 minutes for large objects
    static constexpr curl_off_t kMaxDownloadBytes = 256 * 1024 * 1024; // 256 MB safety limit

    // Helper: perform S3 HTTP request with SigV4 signing
    static bool S3Request(std::string const& method, std::string const& url, std::string const& region,
                          std::string const& accessKeyId, std::string const& secretKey,
                          std::string const& payloadHash, std::string& responseBody, long& httpCode,
                          std::map<std::string, std::string> const& extraHeaders = {},
                          char const* uploadData = nullptr, size_t uploadSize = 0)
    {
        auto signed_ = SigV4Signer::Sign(method, url, region, "s3", accessKeyId, secretKey, payloadHash, extraHeaders);

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
        for (auto const& [key, value] : signed_.m_Headers)
        {
            headers = curl_slist_append(headers, (key + ": " + value).c_str());
        }
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

    // Helper: download S3 object to a file
    static bool S3Download(std::string const& url, std::string const& region, std::string const& accessKeyId,
                           std::string const& secretKey, std::string const& outputPath, std::string& errorMessage)
    {
        auto signed_ = SigV4Signer::Sign("GET", url, region, "s3", accessKeyId, secretKey,
                                          SigV4Signer::EmptyPayloadHash());

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
        for (auto const& [key, value] : signed_.m_Headers)
        {
            headers = curl_slist_append(headers, (key + ": " + value).c_str());
        }
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
            errorMessage = "S3 download failed: HTTP " + std::to_string(httpCode);
            return false;
        }

        return true;
    }

    bool S3CloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                           TaskDef const& taskDefinition, TaskInstanceState& taskState,
                                           CloudConnection const& connection, CloudCredentials const& credentials,
                                           TaskCancellationToken const& cancellationToken)
    {
        // Parse task params
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            taskState.m_LastErrorMessage = "Failed to parse s3 task params JSON";
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
            taskState.m_LastErrorMessage = "Missing required 'operation' in s3 task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Bucket: from params, or fall back to connection's default bucket
        std::string bucket = getStringParam("bucket");
        if (bucket.empty())
        {
            auto bucketIt = connection.m_Params.find("bucket");
            if (bucketIt != connection.m_Params.end())
            {
                bucket = bucketIt->second;
            }
        }

        if (bucket.empty())
        {
            taskState.m_LastErrorMessage = "No bucket specified (neither in task params nor connection)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        auto regionIt = connection.m_Params.find("region");
        std::string region = (regionIt != connection.m_Params.end()) ? regionIt->second : "us-east-1";

        std::string endpointUrl = S3Connector::BuildEndpointUrl(connection, bucket);
        std::string responseBody;
        long httpCode = 0;

        if (operation == "upload")
        {
            std::string key = getStringParam("key");
            std::string filePath = getStringParam("file_path");

            if (key.empty() || filePath.empty())
            {
                taskState.m_LastErrorMessage = "s3 'upload' requires 'key' and 'file_path'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Read file into memory
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open file for upload: " + filePath;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            auto fileSize = file.tellg();
            file.seekg(0, std::ios::beg);
            std::string fileData(static_cast<size_t>(fileSize), '\0');
            file.read(fileData.data(), fileSize);
            file.close();

            std::string payloadHash = SigV4Signer::Sha256Hex(fileData);
            std::string url = endpointUrl + "/" + key;

            std::map<std::string, std::string> extraHeaders;
            extraHeaders["Content-Type"] = "application/octet-stream";

            if (!S3Request("PUT", url, region, credentials.m_AccessKeyId, credentials.m_SecretKey, payloadHash,
                           responseBody, httpCode, extraHeaders, fileData.data(), fileData.size()))
            {
                taskState.m_LastErrorMessage = "S3 upload request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "S3 upload failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"upload\",\"bucket\":\"" + bucket + "\",\"key\":\"" + key + "\"}";
            LOG_APP_INFO("[s3] uploaded {} to s3://{}/{}", filePath, bucket, key);
        }
        else if (operation == "download")
        {
            std::string key = getStringParam("key");
            std::string filePath = getStringParam("file_path");

            if (key.empty() || filePath.empty())
            {
                taskState.m_LastErrorMessage = "s3 'download' requires 'key' and 'file_path'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Ensure output directory exists
            std::filesystem::path outputDir = std::filesystem::path(filePath).parent_path();
            if (!outputDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(outputDir, ec);
            }

            std::string url = endpointUrl + "/" + key;
            std::string downloadError;

            if (!S3Download(url, region, credentials.m_AccessKeyId, credentials.m_SecretKey, filePath, downloadError))
            {
                taskState.m_LastErrorMessage = "S3 download failed: " + downloadError;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody =
                "{\"ok\":true,\"operation\":\"download\",\"bucket\":\"" + bucket + "\",\"key\":\"" + key + "\"}";
            LOG_APP_INFO("[s3] downloaded s3://{}/{} to {}", bucket, key, filePath);
        }
        else if (operation == "list")
        {
            std::string prefix = getStringParam("prefix");
            std::string maxKeysStr = getStringParam("max_keys");
            int maxKeys = maxKeysStr.empty() ? 1000 : std::stoi(maxKeysStr);

            std::string url = endpointUrl + "/?list-type=2&max-keys=" + std::to_string(maxKeys);
            if (!prefix.empty())
            {
                url += "&prefix=" + prefix;
            }

            if (!S3Request("GET", url, region, credentials.m_AccessKeyId, credentials.m_SecretKey,
                           SigV4Signer::EmptyPayloadHash(), responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "S3 list request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "S3 list failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[s3] listed objects in s3://{}/{}", bucket, prefix);
        }
        else if (operation == "delete")
        {
            std::string key = getStringParam("key");
            if (key.empty())
            {
                taskState.m_LastErrorMessage = "s3 'delete' requires 'key'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::string url = endpointUrl + "/" + key;

            if (!S3Request("DELETE", url, region, credentials.m_AccessKeyId, credentials.m_SecretKey,
                           SigV4Signer::EmptyPayloadHash(), responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "S3 delete request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "S3 delete failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"delete\",\"bucket\":\"" + bucket + "\",\"key\":\"" + key + "\"}";
            LOG_APP_INFO("[s3] deleted s3://{}/{}", bucket, key);
        }
        else
        {
            taskState.m_LastErrorMessage =
                "Unknown s3 operation '" + operation + "'. Valid: upload, download, list, delete";
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
