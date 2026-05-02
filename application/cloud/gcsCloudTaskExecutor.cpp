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
#include "cloud/gcsCloudTaskExecutor.h"
#include "cloud/gcsConnector.h"
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlWrapper.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr long kTimeoutSeconds = 300;
    static constexpr curl_off_t kMaxDownloadBytes = 256 * 1024 * 1024; // 256 MB

    // Bound on the response body — closes the audit's HIGH "unbound growth of
    // responseBody in GcsRequest" finding.  64 MB matches the cloud-surface
    // pattern; GCS metadata responses are typically tens of KB.
    static constexpr size_t kMaxGcsResponseBytes = 64 * 1024 * 1024;

    // GCS bucket name validator.  Per Google's bucket-naming rules:
    //   https://cloud.google.com/storage/docs/buckets#naming
    // Allowed chars: lowercase letters, digits, `.`, `_`, `-`.  Length 3..63.
    // Cannot start/end with a hyphen.  This sweep uses the strict allowlist
    // form — bucket names containing URL-meaningful chars (`/`, `?`, `#`, `:`,
    // `@`, `%`) are rejected before any URL build.
    static bool IsValidGcsBucket(std::string const& bucket)
    {
        if (bucket.size() < 3 || bucket.size() > 63)
        {
            return false;
        }
        if (bucket.front() == '-' || bucket.back() == '-')
        {
            return false;
        }
        for (char c : bucket)
        {
            unsigned char const uc = static_cast<unsigned char>(c);
            if (std::islower(uc) || std::isdigit(uc))
            {
                continue;
            }
            if (c == '.' || c == '_' || c == '-')
            {
                continue;
            }
            return false;
        }
        return true;
    }

    // Helper: perform GCS HTTP request with Bearer token auth
    static bool GcsRequest(std::string const& method, std::string const& url, std::string const& accessToken,
                           std::string& responseBody, long& httpCode, std::string const& contentType = {},
                           char const* uploadData = nullptr, size_t uploadSize = 0)
    {
        // Reject CR/LF in the bearer token before splicing into the Authorization
        // header.  Same defensive check as sitting 14's Snowflake JWT.
        if (ICloudTaskExecutor::ContainsCrlf(accessToken))
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] gcs_bearer_crlf_rejected");
            return false;
        }

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
            if (buf->size() + incoming > kMaxGcsResponseBytes)
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
        // Hardened TLS + no redirect-following — GCS JSON API
        // (`storage.googleapis.com`) responds directly; legitimate flows
        // never 30x.  See ConnectorHttp::ApplyHardenedDefaults docstring.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        if (uploadData && uploadSize > 0)
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, uploadData);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(uploadSize));
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + accessToken).c_str());
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

    // Helper: download GCS object to a file
    static bool GcsDownload(std::string const& url, std::string const& accessToken, std::string const& outputPath,
                            std::string& errorMessage)
    {
        // Reject CR/LF in the bearer token (parallel to GcsRequest).
        if (ICloudTaskExecutor::ContainsCrlf(accessToken))
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] gcs_bearer_crlf_rejected (download)");
            errorMessage = "GCS bearer token contains CR/LF — refusing to send";
            return false;
        }

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
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, kMaxDownloadBytes);
        // Hardened TLS + no redirect-following — GCS responds directly on
        // download too.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + accessToken).c_str());
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
            errorMessage = "GCS download failed: HTTP " + std::to_string(httpCode);
            return false;
        }

        return true;
    }

    // URL-encode a string (for object names with special characters)
    static std::string UrlEncode(CURL* curl, std::string const& value)
    {
        char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
        std::string result(encoded);
        curl_free(encoded);
        return result;
    }

    bool GcsCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
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
            taskState.m_LastErrorMessage = "Failed to parse gcs task params JSON";
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
            taskState.m_LastErrorMessage = "Missing required 'operation' in gcs task params (upload or download)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        bool isUpload = (operation == "upload");
        bool isDownload = (operation == "download");

        if (!isUpload && !isDownload)
        {
            taskState.m_LastErrorMessage =
                "Unknown gcs operation '" + operation + "'. Valid: upload, download";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Bucket: from params, or fall back to connection's default
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

        // Validate bucket against GCS naming rules before any URL build.  An
        // attacker-controlled bucket value containing `/`, `?`, `#`, or `:` would
        // otherwise inject URL components past the canonical
        // `https://storage.googleapis.com/upload/storage/v1/b/{bucket}/o?...`
        // path — closes the audit's CRITICAL "Path Traversal via object_name /
        // Unvalidated bucket in URL Construction" finding (the bucket half;
        // object_name is already URL-encoded via the existing UrlEncode lambda).
        if (!IsValidGcsBucket(bucket))
        {
            taskState.m_LastErrorMessage = "Invalid GCS bucket name: must match GCS naming rules "
                                           "([a-z0-9._-], 3-63 chars, no leading/trailing hyphen)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[gcs] task='{}' workflow='{}' run='{}': invalid bucket name (length={})",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, bucket.size());
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] gcs_invalid_bucket task='{}' workflow='{}' run='{}'",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }

        std::string objectName = getStringParam("object_name");
        std::string localPath = getStringParam("local_path");

        if (objectName.empty() || localPath.empty())
        {
            taskState.m_LastErrorMessage = "gcs task requires 'object_name' and 'local_path'";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // local_path resolves relative to the task's working_directory per JCWF
        // spec §3.2.1.  Absolute values (e.g. {{ai.output_file}} templates)
        // pass through; both cases are confined under launchCwd by
        // ValidateLocalPath as the project-tree security boundary.
        std::filesystem::path const workflowBaseDir =
            TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path const workDir =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);
        if (!ValidateLocalPath(localPath, workDir, taskDefinition.m_Id))
        {
            taskState.m_LastErrorMessage =
                "gcs: local_path is invalid or escapes the project tree";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[gcs] task='{}' workflow='{}' run='{}': local_path rejected",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }
        std::filesystem::path const fullLocalPath = workDir / localPath;

        std::string endpointUrl = GcsConnector::BuildEndpointUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        // URL-encode the object name for use in URLs
        CURL* encoderCurl = curl_easy_init();
        std::string encodedObjectName = encoderCurl ? UrlEncode(encoderCurl, objectName) : objectName;
        if (encoderCurl)
        {
            curl_easy_cleanup(encoderCurl);
        }

        if (isUpload)
        {
            // Read file into memory
            std::ifstream file(fullLocalPath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open file for upload: " + fullLocalPath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            auto const fileSize = file.tellg();
            // Cap on upload file size — closes the audit's HIGH "unbounded file
            // read into memory on upload" finding for GCS.  256 MB matches Phase 9b's
            // existing CURLOPT_MAXFILESIZE_LARGE for downloads.
            static constexpr std::streamoff kMaxGcsUploadBytes = 256 * 1024 * 1024;
            if (fileSize < 0 || fileSize > kMaxGcsUploadBytes)
            {
                taskState.m_LastErrorMessage = "gcs upload: file size " + std::to_string(static_cast<long long>(fileSize)) +
                                               " exceeds " + std::to_string(static_cast<long long>(kMaxGcsUploadBytes)) + " byte cap";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[gcs] task='{}' workflow='{}' run='{}': upload size {} exceeds cap",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                              static_cast<long long>(fileSize));
                return false;
            }
            file.seekg(0, std::ios::beg);
            std::string fileData(static_cast<size_t>(fileSize), '\0');
            file.read(fileData.data(), fileSize);
            file.close();

            // GCS JSON API upload: POST /upload/storage/v1/b/{bucket}/o?uploadType=media&name={object}
            std::string url = endpointUrl + "/upload/storage/v1/b/" + bucket + "/o?uploadType=media&name=" +
                              encodedObjectName;

            if (!GcsRequest("POST", url, credentials.m_Token, responseBody, httpCode, "application/octet-stream",
                            fileData.data(), fileData.size()))
            {
                taskState.m_LastErrorMessage = "GCS upload request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "GCS upload failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"upload\",\"bucket\":\"" +
                           JsonHelper::EscapeJsonString(bucket) + "\",\"object_name\":\"" +
                           JsonHelper::EscapeJsonString(objectName) + "\"}";
            LOG_APP_INFO("[gcs] uploaded {} to gs://{}/{}", fullLocalPath.string(), bucket, objectName);
        }
        else // isDownload
        {
            // Ensure output directory exists
            std::filesystem::path outputDir = fullLocalPath.parent_path();
            if (!outputDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(outputDir, ec);
            }

            // GCS JSON API download: GET /storage/v1/b/{bucket}/o/{object}?alt=media
            std::string url =
                endpointUrl + "/storage/v1/b/" + bucket + "/o/" + encodedObjectName + "?alt=media";
            std::string downloadError;

            if (!GcsDownload(url, credentials.m_Token, fullLocalPath.string(), downloadError))
            {
                taskState.m_LastErrorMessage = "GCS download failed: " + downloadError;
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"download\",\"bucket\":\"" +
                           JsonHelper::EscapeJsonString(bucket) + "\",\"object_name\":\"" +
                           JsonHelper::EscapeJsonString(objectName) + "\"}";
            LOG_APP_INFO("[gcs] downloaded gs://{}/{} to {}", bucket, objectName, fullLocalPath.string());
        }

        taskState.m_CapturedStdout = responseBody.substr(0, std::min(responseBody.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write response to task working directory (workflowBaseDir + workDir
        // already resolved earlier in ExecuteCloud).
        if (!workflowBaseDir.empty())
        {
            WriteResponseJson(workDir, taskState, responseBody);
        }

        return true;
    }
} // namespace AIAssistant
