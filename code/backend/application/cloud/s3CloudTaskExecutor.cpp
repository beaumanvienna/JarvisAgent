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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "auxiliary/sha256.h"
#include "cloud/connectorError.h"
#include "cloud/s3CloudTaskExecutor.h"
#include "cloud/s3Connector.h"
#include "keys/secureString.h"
#include "curlWrapper/awsSigV4.h"
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlSlistHelper.h"
#include "curlWrapper/curlWrapper.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;
    static constexpr long kTimeoutSeconds = 300;        // 5 minutes for large objects
    static constexpr curl_off_t kMaxDownloadBytes = 256 * 1024 * 1024; // 256 MB safety limit

    // URL-encode an S3 key or prefix for safe interpolation into a request URL.
    // Per RFC 3986 + AWS S3 conventions, `/` is preserved as the path delimiter
    // (S3 keys can contain `/` to express prefixes); every other byte is
    // percent-encoded via curl_easy_escape.  Ensures no injection chars
    // (`?`, `#`, `&`, `=`, `%`, whitespace, control chars, etc.) reach the URL
    // unencoded.
    static std::string UrlEncodeS3Key(std::string const& key)
    {
        CURL* encoderCurl = curl_easy_init();
        if (!encoderCurl)
        {
            return key; // Safe fallback: return unmodified.  Caller should
                        // never see this in practice (curl_easy_init failure
                        // means the actual request will also fail).
        }
        std::string out;
        out.reserve(key.size() + 16);
        std::string segment;
        segment.reserve(key.size());
        auto flushSegment = [&]() {
            if (segment.empty())
            {
                return;
            }
            char* escaped = curl_easy_escape(encoderCurl, segment.data(), static_cast<int>(segment.size()));
            if (escaped)
            {
                out += escaped;
                curl_free(escaped);
            }
            else
            {
                out += segment;
            }
            segment.clear();
        };
        for (char c : key)
        {
            if (c == '/')
            {
                flushSegment();
                out += '/';
            }
            else
            {
                segment += c;
            }
        }
        flushSegment();
        curl_easy_cleanup(encoderCurl);
        return out;
    }

    // Helper: perform S3 HTTP request with SigV4 signing
    static bool S3Request(std::string const& method, std::string const& url, std::string const& region,
                          std::string const& accessKeyId, SecureString const& secretKey,
                          std::string const& payloadHash, std::string& responseBody, long& httpCode,
                          std::map<std::string, std::string> const& extraHeaders = {},
                          char const* uploadData = nullptr, size_t uploadSize = 0)
    {
        SigV4Signer::Inputs sigInputs;
        sigInputs.m_Method = method;
        sigInputs.m_Url = url;
        sigInputs.m_AccessKey = accessKeyId;
        sigInputs.m_SecretKey.Set(secretKey.Get());
        sigInputs.m_Region = region;
        sigInputs.m_Service = "s3";
        sigInputs.m_ContentSha256Override = payloadHash;
        sigInputs.m_ExtraHeadersToSign = extraHeaders;
        SigV4Signer::SignedHeaders signed_ = SigV4Signer::Sign(sigInputs);
        if (signed_.m_Authorization.empty())
        {
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
            // Bound on the response body — closes the unbounded-responseBody DoS.
            // 64 MB matches the cloud-surface pattern; S3 list-objects responses
            // are well under this cap (limited by max_keys, max 1000 entries).
            static constexpr size_t kMaxS3ResponseBytes = 64 * 1024 * 1024;
            if (buf->size() + incoming > kMaxS3ResponseBytes)
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
        // Hardened TLS + redirect-following capped to https — S3 legitimately
        // emits `301 Moved Permanently` with `Location` on cross-region bucket
        // mismatches and presigned-URL flows.  `ApplyExecutorRedirectDefaults`
        // restricts redirect targets to https only (no http downgrade),
        // caps follow depth at 10, and applies explicit verify-peer +
        // verify-host.  S3-compatible alternatives (MinIO etc.) over `http://`
        // are unaffected — the TLS verify setopts only kick in when libcurl
        // actually negotiates TLS, and http-MinIO doesn't 30x in practice.
        ConnectorHttp::ApplyExecutorRedirectDefaults(curl, url);

        if (uploadData && uploadSize > 0)
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, uploadData);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(uploadSize));
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Host: " + signed_.m_Host).c_str());
        headers = curl_slist_append(headers, ("X-Amz-Date: " + signed_.m_AmzDate).c_str());
        headers = curl_slist_append(headers, ("X-Amz-Content-Sha256: " + signed_.m_ContentSha256).c_str());
        headers = curl_slist_append(headers, ("Authorization: " + signed_.m_Authorization).c_str());
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
                           SecureString const& secretKey, std::string const& outputPath, std::string& errorMessage)
    {
        SigV4Signer::Inputs sigInputs;
        sigInputs.m_Method = "GET";
        sigInputs.m_Url = url;
        sigInputs.m_AccessKey = accessKeyId;
        sigInputs.m_SecretKey.Set(secretKey.Get());
        sigInputs.m_Region = region;
        sigInputs.m_Service = "s3";
        // m_Body empty → signer derives the canonical empty-string SHA-256.
        SigV4Signer::SignedHeaders signed_ = SigV4Signer::Sign(sigInputs);
        if (signed_.m_Authorization.empty())
        {
            errorMessage = "SigV4 signing failed";
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
        // Hardened TLS + redirect-following capped to https — same posture as
        // the S3Request helper above.  Download path also legitimately 30x's
        // on cross-region bucket mismatches.
        ConnectorHttp::ApplyExecutorRedirectDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Host: " + signed_.m_Host).c_str());
        headers = curl_slist_append(headers, ("X-Amz-Date: " + signed_.m_AmzDate).c_str());
        headers = curl_slist_append(headers, ("X-Amz-Content-Sha256: " + signed_.m_ContentSha256).c_str());
        headers = curl_slist_append(headers, ("Authorization: " + signed_.m_Authorization).c_str());
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
            taskState.m_LastErrorMessage = "Missing required 'operation' in s3 task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
            return false;
        }

        // Resolve the task's working_directory once — JCWF spec §3.2.1 says
        // task-scoped relative file paths resolve relative to working_directory.
        std::filesystem::path const workflowBaseDir =
            TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path const workDir =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

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
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
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
                taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
                return false;
            }

            // file_path resolves relative to working_directory per JCWF spec §3.2.1.
            // Absolute values (e.g. {{ai.output_file}}) pass through; both cases
            // are confined under launchCwd by ValidateLocalPath as the project-tree
            // security boundary.
            if (!ValidateLocalPath(filePath, workDir, taskDefinition.m_Id))
            {
                taskState.m_LastErrorMessage =
                    "s3 upload: file_path is invalid or escapes the project tree";
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
                LOG_APP_ERROR("[s3] task='{}' workflow='{}' run='{}': file_path rejected (upload)",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
                return false;
            }

            std::filesystem::path const fullFilePath = workDir / filePath;
            // Read file into memory
            std::ifstream file(fullFilePath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                taskState.m_LastErrorMessage = "Cannot open file for upload: " + fullFilePath.string();
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
                return false;
            }

            auto const fileSize = file.tellg();
            // Cap on upload file size to bound peak RAM during the read-and-
            // POST cycle.  256 MB matches the existing CURLOPT_MAXFILESIZE_LARGE
            // for downloads.
            static constexpr std::streamoff kMaxS3UploadBytes = 256 * 1024 * 1024;
            if (fileSize < 0 || fileSize > kMaxS3UploadBytes)
            {
                taskState.m_LastErrorMessage = "s3 upload: file size " + std::to_string(static_cast<long long>(fileSize)) +
                                               " exceeds " + std::to_string(static_cast<long long>(kMaxS3UploadBytes)) + " byte cap";
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::ValueOutOfRange;
                LOG_APP_ERROR("[s3] task='{}' workflow='{}' run='{}': upload size {} exceeds cap",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                              static_cast<long long>(fileSize));
                return false;
            }
            file.seekg(0, std::ios::beg);
            std::string fileData(static_cast<size_t>(fileSize), '\0');
            file.read(fileData.data(), fileSize);
            file.close();

            std::string payloadHash = EngineCore::Sha256Hex(fileData);
            std::string url = endpointUrl + "/" + UrlEncodeS3Key(key);

            std::map<std::string, std::string> extraHeaders;
            extraHeaders["Content-Type"] = "application/octet-stream";

            if (!S3Request("PUT", url, region, credentials.m_AccessKeyId, credentials.m_SecretKey, payloadHash,
                           responseBody, httpCode, extraHeaders, fileData.data(), fileData.size()))
            {
                taskState.m_LastErrorMessage = "S3 upload request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::NetworkError;
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
                taskState.m_LastFailureCode = (httpCode == 401 || httpCode == 403)
                                                  ? ConnectorErrorCode::AuthFailure
                                                  : ConnectorErrorCode::HttpError;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"upload\",\"bucket\":\"" +
                           JsonHelper::EscapeJsonString(bucket) + "\",\"key\":\"" +
                           JsonHelper::EscapeJsonString(key) + "\"}";
            LOG_APP_INFO("[s3] uploaded {} to s3://{}/{}", fullFilePath.string(), bucket, key);
        }
        else if (operation == "download")
        {
            std::string key = getStringParam("key");
            std::string filePath = getStringParam("file_path");

            if (key.empty() || filePath.empty())
            {
                taskState.m_LastErrorMessage = "s3 'download' requires 'key' and 'file_path'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
                return false;
            }

            // Same gate as the upload branch — file_path resolves relative to
            // working_directory per JCWF spec §3.2.1.  An attacker-controlled
            // path on download is even more dangerous than on upload because
            // it would WRITE arbitrary files (e.g. overwrite /etc/cron.d/evil).
            if (!ValidateLocalPath(filePath, workDir, taskDefinition.m_Id))
            {
                taskState.m_LastErrorMessage =
                    "s3 download: file_path is invalid or escapes the project tree";
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
                LOG_APP_ERROR("[s3] task='{}' workflow='{}' run='{}': file_path rejected (download)",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
                return false;
            }

            std::filesystem::path const fullFilePath = workDir / filePath;

            // Ensure output directory exists
            std::filesystem::path outputDir = fullFilePath.parent_path();
            if (!outputDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(outputDir, ec);
            }

            std::string url = endpointUrl + "/" + UrlEncodeS3Key(key);
            std::string downloadError;

            if (!S3Download(url, region, credentials.m_AccessKeyId, credentials.m_SecretKey,
                            fullFilePath.string(), downloadError))
            {
                taskState.m_LastErrorMessage = "S3 download failed: " + downloadError;
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::NetworkError;
                return false;
            }

            responseBody =
                "{\"ok\":true,\"operation\":\"download\",\"bucket\":\"" +
                JsonHelper::EscapeJsonString(bucket) + "\",\"key\":\"" +
                JsonHelper::EscapeJsonString(key) + "\"}";
            LOG_APP_INFO("[s3] downloaded s3://{}/{} to {}", bucket, key, fullFilePath.string());
        }
        else if (operation == "list")
        {
            std::string prefix = getStringParam("prefix");
            std::string maxKeysStr = getStringParam("max_keys");
            // S3 ListObjectsV2 caps max-keys at 1000 server-side.  Parse defensively:
            // pre-fix `std::stoi(maxKeysStr)` would throw on hostile values like
            // `"abc"` (invalid_argument) or `"99999999999"` (out_of_range), crashing
            // the worker thread.  Validate digits-only first, wrap stoi in
            // try/catch, clamp to the API's [1, 1000] range, default to 1000 on
            // any parse error or invalid input.
            int maxKeys = 1000;
            if (!maxKeysStr.empty())
            {
                bool digitsOnly = true;
                for (char c : maxKeysStr)
                {
                    if (!std::isdigit(static_cast<unsigned char>(c)))
                    {
                        digitsOnly = false;
                        break;
                    }
                }
                if (!digitsOnly || maxKeysStr.size() > 10)
                {
                    LOG_APP_WARN("[s3] task='{}' workflow='{}' run='{}': invalid max_keys (length={}); using default 1000",
                                 taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, maxKeysStr.size());
                }
                else
                {
                    try
                    {
                        int const parsed = std::stoi(maxKeysStr);
                        maxKeys = std::clamp(parsed, 1, 1000);
                    }
                    catch (std::invalid_argument const&)
                    {
                        LOG_APP_WARN("[s3] task='{}' workflow='{}' run='{}': max_keys parse failed; using default 1000",
                                     taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
                    }
                    catch (std::out_of_range const&)
                    {
                        LOG_APP_WARN("[s3] task='{}' workflow='{}' run='{}': max_keys out of range; using default 1000",
                                     taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
                    }
                }
            }

            std::string url = endpointUrl + "/?list-type=2&max-keys=" + std::to_string(maxKeys);
            if (!prefix.empty())
            {
                url += "&prefix=" + UrlEncodeS3Key(prefix);
            }

            // Empty payload hash override → signer derives from m_Body (empty),
            // producing the canonical empty-string SHA-256.
            if (!S3Request("GET", url, region, credentials.m_AccessKeyId, credentials.m_SecretKey,
                           /*payloadHash=*/{}, responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "S3 list request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::NetworkError;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "S3 list failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = (httpCode == 401 || httpCode == 403)
                                                  ? ConnectorErrorCode::AuthFailure
                                                  : ConnectorErrorCode::HttpError;
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
                taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
                return false;
            }

            std::string url = endpointUrl + "/" + UrlEncodeS3Key(key);

            if (!S3Request("DELETE", url, region, credentials.m_AccessKeyId, credentials.m_SecretKey,
                           /*payloadHash=*/{}, responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "S3 delete request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = ConnectorErrorCode::NetworkError;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "S3 delete failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastFailureCode = (httpCode == 401 || httpCode == 403)
                                                  ? ConnectorErrorCode::AuthFailure
                                                  : ConnectorErrorCode::HttpError;
                return false;
            }

            responseBody = "{\"ok\":true,\"operation\":\"delete\",\"bucket\":\"" +
                           JsonHelper::EscapeJsonString(bucket) + "\",\"key\":\"" +
                           JsonHelper::EscapeJsonString(key) + "\"}";
            LOG_APP_INFO("[s3] deleted s3://{}/{}", bucket, key);
        }
        else
        {
            taskState.m_LastErrorMessage =
                "Unknown s3 operation '" + operation + "'. Valid: upload, download, list, delete";
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastFailureCode = ConnectorErrorCode::InvalidConfig;
            return false;
        }

        taskState.m_CapturedStdout = responseBody.substr(0, std::min(responseBody.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write response to task working directory (workDir + workflowBaseDir
        // already resolved at the top of ExecuteCloud).
        if (!workflowBaseDir.empty())
        {
            WriteResponseJson(workDir, taskState, responseBody);
        }

        return true;
    }
} // namespace AIAssistant
