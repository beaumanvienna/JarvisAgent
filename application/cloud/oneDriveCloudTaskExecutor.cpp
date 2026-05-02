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
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlWrapper.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr long kTimeoutSeconds = 300;        // 5 minutes for large files
    static constexpr curl_off_t kMaxDownloadBytes = 256 * 1024 * 1024; // 256 MB safety limit

    // Helper: perform an authenticated Graph API request
    // Bound on the response body — same DoS posture as the other cloud
    // executors; 64 MB matches the established pattern.
    static constexpr size_t kMaxOneDriveResponseBytes = 64 * 1024 * 1024;

    // Validator for OneDrive remote_path.  The path is interpolated into
    //   /me/drive/root:/{remote_path}:/content
    // — Microsoft Graph's path-based addressing.  Pre-fix, an attacker-controlled
    // `remote_path` like `../../../foo:/content?x=` could escape the path
    // segment and inject URL components.  Allow only safe path-component chars:
    // alphanumeric, `.`, `_`, `-`, `/` (hierarchy delimiter), space (legitimate
    // in OneDrive folder/file names).  Reject `..` segments (path traversal in
    // the Graph URL), `?` `#` `:` `@` `%` (URL-meaningful), `\\` `\r` `\n`.
    static bool IsValidOneDriveRemotePath(std::string const& path)
    {
        if (path.empty() || path.size() > 1024)
        {
            return false;
        }
        if (path.find("..") != std::string::npos)
        {
            return false;
        }
        for (char c : path)
        {
            unsigned char const uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc))
            {
                continue;
            }
            if (c == '.' || c == '_' || c == '-' || c == '/' || c == ' ')
            {
                continue;
            }
            return false;
        }
        return true;
    }

    static bool GraphRequest(std::string const& method, std::string const& url, std::string const& bearerToken,
                             std::string& responseBody, long& httpCode,
                             char const* uploadData = nullptr, size_t uploadSize = 0,
                             std::string const& contentType = {})
    {
        if (ICloudTaskExecutor::ContainsCrlf(bearerToken))
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] onedrive_bearer_crlf_rejected");
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
            if (buf->size() + incoming > kMaxOneDriveResponseBytes)
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
        // Hardened TLS + redirect-following capped to https — Microsoft Graph
        // legitimately 30x's: `GET /me/drive/items/{id}/content` returns 302
        // to a SharePoint / `download.microsoft*` CDN URL, and large-file
        // upload sessions can pivot to `*.up.1drv.com` endpoints.
        // `ApplyExecutorRedirectDefaults` restricts redirect targets to https,
        // caps follow depth at 10, and applies explicit verify-peer +
        // verify-host.  Microsoft Graph is HTTPS-only.
        ConnectorHttp::ApplyExecutorRedirectDefaults(curl, url);

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
        if (ICloudTaskExecutor::ContainsCrlf(bearerToken))
        {
            ConnectorHttp::IncrementCredentialCrlfRejection();
            LOG_SECURITY_WARN("[security] onedrive_bearer_crlf_rejected (download)");
            errorMessage = "OneDrive bearer token contains CR/LF — refusing to send";
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
        // Hardened TLS + redirect-following capped to https — same posture
        // as the GraphRequest helper above.  This download path is THE
        // primary reason redirects must be allowed: Graph's content endpoint
        // 302's to a SharePoint / `download.microsoft*` CDN URL on every
        // download request.
        ConnectorHttp::ApplyExecutorRedirectDefaults(curl, url);

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

        // Validate remote_path before any URL build — closes the audit's CRITICAL
        // "Path Traversal via remote_path in URL Construction (SSRF / Injection)"
        // finding.  Pre-fix `graphBase + "/me/drive/root:/" + remotePath + ":/content"`
        // would interpolate `../../etc/passwd` or `?x=&y=` directly.
        if (!IsValidOneDriveRemotePath(remotePath))
        {
            taskState.m_LastErrorMessage =
                "Invalid OneDrive remote_path: must be alphanumeric + ._-/ + space, no `..` segments";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[onedrive] task='{}' workflow='{}' run='{}': invalid remote_path (length={})",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, remotePath.size());
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] onedrive_invalid_remote_path task='{}' workflow='{}' run='{}'",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }

        std::string localPath = getStringParam("local_path");
        if (localPath.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'local_path' in onedrive task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Resolve workDir once and confine local_path under it.  Both upload and
        // download branches need the same resolved fullLocalPath; lifting both
        // out of the per-branch blocks lets a single ValidateLocalPath gate cover
        // both file-I/O sites and removes the duplicated TaskPathResolver calls.
        std::filesystem::path const workflowBaseDir =
            TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path const workDir =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);
        if (!ValidateLocalPath(localPath, workDir, taskDefinition.m_Id))
        {
            taskState.m_LastErrorMessage =
                "onedrive: local_path is invalid or escapes the working directory";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[onedrive] task='{}' workflow='{}' run='{}': local_path rejected",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }
        std::filesystem::path const fullLocalPath = workDir / localPath;

        std::string graphBase = OneDriveConnector::GetGraphBaseUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        if (operation == "upload")
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
            // Cap on upload file size — closes the audit's HIGH "uncontrolled file
            // read into memory on upload" finding.  256 MB matches Phase 9b's
            // existing CURLOPT_MAXFILESIZE_LARGE for downloads.
            static constexpr std::streamoff kMaxOneDriveUploadBytes = 256 * 1024 * 1024;
            if (fileSize < 0 || fileSize > kMaxOneDriveUploadBytes)
            {
                taskState.m_LastErrorMessage = "onedrive upload: file size " + std::to_string(static_cast<long long>(fileSize)) +
                                               " exceeds " + std::to_string(static_cast<long long>(kMaxOneDriveUploadBytes)) + " byte cap";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[onedrive] task='{}' workflow='{}' run='{}': upload size {} exceeds cap",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                              static_cast<long long>(fileSize));
                return false;
            }
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
            // workDir + fullLocalPath were resolved + validated above the branch;
            // both branches share the same confined fullLocalPath.

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

            responseBody = "{\"ok\":true,\"operation\":\"download\",\"remote_path\":\"" +
                           JsonHelper::EscapeJsonString(remotePath) + "\"}";
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

        // Write response to task working directory.  Reuses the workDir resolved
        // above the upload/download branch.
        if (!workflowBaseDir.empty())
        {
            WriteResponseJson(workDir, taskState, responseBody);
        }

        return true;
    }
} // namespace AIAssistant
