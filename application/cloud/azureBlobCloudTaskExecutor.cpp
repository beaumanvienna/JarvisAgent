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
#include "cloud/connectorError.h"
#include "cloud/azureBlobConnector.h"
#include "cloud/azureSharedKeySigner.h"
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlSlistHelper.h"
#include "curlWrapper/curlWrapper.h"
#include "json/jsonHelper.h"
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
            [[maybe_unused]] SecureString authScratch_inline;
AppendSecretHeader(headers, "Authorization: Bearer ", credentials.m_Token, authScratch_inline);
            headers = curl_slist_append(headers, "x-ms-version: 2024-11-04");
        }
        return headers;
    }

    // Bound on the response body the writeCallback will accept.  Closes the
    // unbounded-responseBody DoS — a hostile or compromised endpoint can otherwise
    // stream arbitrary bytes into our process.  64 MB matches the cloud-surface
    // pattern; Azure Blob list/copy responses are well under this cap in practice.
    static constexpr size_t kMaxAzureBlobResponseBytes = 64 * 1024 * 1024;

    // Azure Blob container name validator.  Per Microsoft's container-naming
    // rules:
    //   https://learn.microsoft.com/azure/storage/blobs/storage-blobs-introduction#container-naming
    // Length 3..63.  Lowercase letters + digits + single hyphen.  Must start
    // and end with a letter or digit.  No consecutive hyphens.  This sweep
    // uses the strict allowlist form — container names containing
    // URL-meaningful chars (`/`, `?`, `#`, `:`, `@`, `%`) are rejected before
    // the URL build at `endpointUrl + "/" + container + "/" + blobName`.
    static bool IsValidAzureContainer(std::string const& container)
    {
        if (container.size() < 3 || container.size() > 63)
        {
            return false;
        }
        char const first = container.front();
        char const last = container.back();
        unsigned char const ufirst = static_cast<unsigned char>(first);
        unsigned char const ulast = static_cast<unsigned char>(last);
        if (!(std::islower(ufirst) || std::isdigit(ufirst)))
        {
            return false;
        }
        if (!(std::islower(ulast) || std::isdigit(ulast)))
        {
            return false;
        }
        for (size_t i = 0; i < container.size(); ++i)
        {
            char const c = container[i];
            unsigned char const uc = static_cast<unsigned char>(c);
            if (std::islower(uc) || std::isdigit(uc))
            {
                continue;
            }
            if (c == '-')
            {
                // Reject consecutive hyphens.
                if (i + 1 < container.size() && container[i + 1] == '-')
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        return true;
    }

    // Azure Blob blob_name validator.  Per Microsoft:
    //   https://learn.microsoft.com/rest/api/storageservices/naming-and-referencing-containers--blobs--and-metadata
    // Length 1..1024.  Allowed: alphanumeric + `.` + `_` + `-` + `/` (hierarchy
    // delimiter) + space.  Reject `..` segments (path traversal in the blob
    // URL — Azure resolves them server-side which would let a hostile blob
    // name escape the intended folder), URL-meaningful chars (`?` `#` `:`
    // `@` `%`), backslash, and CR / LF / NUL (header-injection vectors when
    // the value flows into the request URL or any header field).
    static bool IsValidAzureBlobName(std::string const& blobName)
    {
        if (blobName.empty() || blobName.size() > 1024)
        {
            return false;
        }
        if (blobName.find("..") != std::string::npos)
        {
            return false;
        }
        for (char c : blobName)
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
            size_t const incoming = size * nmemb;
            if (buf->size() + incoming > kMaxAzureBlobResponseBytes)
            {
                return 0; // CURLE_WRITE_ERROR; caller surfaces as a request failure
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
        // Hardened TLS + no redirect-following — Azure Blob's data path
        // (`*.blob.core.windows.net`) responds directly; legitimate flows
        // never 30x.  See ConnectorHttp::ApplyHardenedDefaults docstring.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

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
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, kMaxDownloadBytes);
        // Hardened TLS + no redirect-following — Azure Blob's data path
        // (`*.blob.core.windows.net`) responds directly on download too.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

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
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
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
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
            return false;
        }

        bool isUpload = (operation == "upload");
        bool isDownload = (operation == "download");

        if (!isUpload && !isDownload)
        {
            taskState.m_LastErrorMessage =
                "Unknown azure_blob operation '" + operation + "'. Valid: upload, download";
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
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
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
            return false;
        }

        std::string blobName = getStringParam("blob_name");
        std::string localPath = getStringParam("local_path");

        if (blobName.empty() || localPath.empty())
        {
            taskState.m_LastErrorMessage = "azure_blob task requires 'blob_name' and 'local_path'";
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
            return false;
        }

        // Validate container + blob_name before any URL build.  Both flow
        // unencoded into `endpointUrl + "/" + container + "/" + blobName`; an
        // attacker-controlled value containing `/`, `?`, `#`, or `:` would
        // otherwise inject URL components past the canonical
        // `https://{account}.blob.core.windows.net/{container}/{blob}` layout.
        if (!IsValidAzureContainer(container))
        {
            taskState.m_LastErrorMessage = "Invalid Azure Blob container name: must match Azure naming rules "
                                           "([a-z0-9-], 3-63 chars, no consecutive/leading/trailing hyphens)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
            LOG_APP_ERROR("[azure_blob] task='{}' workflow='{}' run='{}': invalid container name (length={})",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, container.size());
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] azure_blob_invalid_container task='{}' workflow='{}' run='{}'",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }
        if (!IsValidAzureBlobName(blobName))
        {
            taskState.m_LastErrorMessage = "Invalid Azure Blob blob_name: must be 1-1024 chars, "
                                           "[A-Za-z0-9._-/ ] only, no `..` segments";
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
            LOG_APP_ERROR("[azure_blob] task='{}' workflow='{}' run='{}': invalid blob_name (length={})",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, blobName.size());
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] azure_blob_invalid_blob_name task='{}' workflow='{}' run='{}'",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }

        // local_path resolves relative to the task's working_directory per JCWF
        // spec §3.2.1.  Absolute values (e.g. {{ai.output_file}} templates) pass
        // through; both cases are confined under launchCwd by ValidateLocalPath
        // as the project-tree security boundary.
        std::filesystem::path const workflowBaseDir =
            TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path const workDir =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);
        if (!ValidateLocalPath(localPath, workDir, taskDefinition.m_Id))
        {
            taskState.m_LastErrorMessage =
                "azure_blob: local_path is invalid or escapes the project tree";
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
            LOG_APP_ERROR("[azure_blob] task='{}' workflow='{}' run='{}': local_path rejected",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }
        std::filesystem::path const fullLocalPath = workDir / localPath;

        std::string endpointUrl = AzureBlobConnector::BuildEndpointUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        if (isUpload)
        {
            // Read file into memory
            std::ifstream file(fullLocalPath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open file for upload: " + fullLocalPath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
                return false;
            }

            auto const fileSize = file.tellg();
            // Cap on upload file size.  Without this an oversized local file
            // (`/dev/zero`, a multi-GB log, etc.) reachable through the path-
            // traversal gate above would exhaust process memory.  256 MB matches
            // Phase 9b's existing CURLOPT_MAXFILESIZE_LARGE for downloads.
            static constexpr std::streamoff kMaxAzureBlobUploadBytes = 256 * 1024 * 1024;
            if (fileSize < 0 || fileSize > kMaxAzureBlobUploadBytes)
            {
                taskState.m_LastErrorMessage = "azure_blob upload: file size " + std::to_string(static_cast<long long>(fileSize)) +
                                               " exceeds " + std::to_string(static_cast<long long>(kMaxAzureBlobUploadBytes)) + " byte cap";
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::ValueOutOfRange;
                LOG_APP_ERROR("[azure_blob] task='{}' workflow='{}' run='{}': upload size {} exceeds cap",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                              static_cast<long long>(fileSize));
                return false;
            }
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
                taskState.m_LastFailureCode = ConnectorErrorCode::NetworkError;
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
                taskState.m_LastFailureCode = (httpCode == 401 || httpCode == 403)
                                                  ? ConnectorErrorCode::AuthFailure
                                                  : ConnectorErrorCode::HttpError;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"upload\",\"container\":\"" +
                           JsonHelper::EscapeJsonString(container) + "\",\"blob_name\":\"" +
                           JsonHelper::EscapeJsonString(blobName) + "\"}";
            LOG_APP_INFO("[azure_blob] uploaded {} to {}/{}", fullLocalPath.string(), container, blobName);
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

            std::string url = endpointUrl + "/" + container + "/" + blobName;
            std::string downloadError;

            if (!AzureBlobDownload(url, credentials, fullLocalPath.string(), downloadError))
            {
                taskState.m_LastErrorMessage = "Azure Blob download failed: " + downloadError;
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::NetworkError;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"download\",\"container\":\"" +
                           JsonHelper::EscapeJsonString(container) + "\",\"blob_name\":\"" +
                           JsonHelper::EscapeJsonString(blobName) + "\"}";
            LOG_APP_INFO("[azure_blob] downloaded {}/{} to {}", container, blobName, fullLocalPath.string());
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
