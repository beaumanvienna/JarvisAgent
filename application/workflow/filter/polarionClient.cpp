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

#include "workflow/filter/polarionClient.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <curl/curl.h>

#include "auxiliary/file.h"
#include "core.h"
#include "engine.h"
#include "curlWrapper/curlWrapper.h"
#include "cloud/connectorHttp.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "simdjson/simdjson.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    namespace
    {
        // Cap on response body fragment included in error messages.  Pre-fix
        // was 500 bytes — a JSON:API error body can include tokens or
        // identifiers we don't want in dashboard logs.  200 bytes is enough
        // to convey "what kind of error" without leaking content; the full
        // body is still available via debug logging if a maintainer needs it.
        constexpr size_t kMaxErrorBodyBytes = 200;

        // CURLOPT_MAXFILESIZE for HttpDownloadFile.  100 MB is well above
        // realistic Polarion attachment sizes (PDFs, screenshots, requirement
        // exports) and well below RAM/disk-budget concerns.
        constexpr long kMaxDownloadBytes = 100ll * 1024 * 1024;

        // Filesystem-safe filter.m_Id allowlist: alphanumeric + `._-`, no `/`,
        // no `..`, capped at 64 bytes.  Matches AdhocWorkflowManager::
        // SanitizeUserSlug's character set for cross-component consistency.
        // Used in WriteItemFile to defend against a hostile JCWF putting `..`
        // in filter.m_Id and escaping `<workflowBaseDir>/<filter.m_Id>/`.
        bool IsValidFilesystemId(std::string const& id)
        {
            if (id.empty() || id.size() > 64) return false;
            if (id == "." || id == "..") return false;
            if (id.front() == '.') return false;
            for (char c : id)
            {
                bool const ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                if (!ok) return false;
            }
            return true;
        }

        // URL-path-component allowlist for Polarion IDs (project, workitem,
        // attachment).  These flow into the URL after UrlEncode — UrlEncode
        // is the standard defense against URL injection, but defense in
        // depth: refuse anything that doesn't match the documented Polarion
        // ID shape.  Allows alphanumeric + `._-/` (Polarion work-item IDs
        // can include `/` for project-qualified form like "Proj/REQ-001").
        // Caps length at 256 bytes; rejects `..` segments outright.
        bool IsValidPolarionId(std::string const& id)
        {
            if (id.empty() || id.size() > 256) return false;
            // Reject `..` as a substring — Polarion IDs don't have parent
            // directories, and `..` in any segment indicates a traversal
            // attempt rather than a legitimate identifier.
            if (id.find("..") != std::string::npos) return false;
            for (char c : id)
            {
                bool const ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/';
                if (!ok) return false;
            }
            return true;
        }

        // Truncate-and-sanitize a response body fragment for inclusion in an
        // error message.  Caps at kMaxErrorBodyBytes and runs through
        // SanitizeUtf8 so malformed bytes don't leak into the dashboard / log.
        std::string SanitizeErrorBody(std::string const& body)
        {
            if (body.empty()) return {};
            std::string truncated = body.size() > kMaxErrorBodyBytes
                                        ? body.substr(0, kMaxErrorBodyBytes) + "... (truncated)"
                                        : body;
            return SanitizeUtf8(truncated);
        }
    } // anonymous namespace

    // =================================================================
    // FetchAll — paginated work-item retrieval
    // =================================================================

    std::vector<FilterItem> PolarionClient::FetchAll(FilterDef const& filter, std::string const& workflowBaseDir,
                                                     std::string& errorMessage) const
    {
        FilterSource const& source = filter.m_Source;

        // Resolve credentials via KeyManager
        if (source.m_KeyName.empty())
        {
            errorMessage = "filter '" + filter.m_Id + "': polarion_query requires 'key_name'";
            return {};
        }

        if (Core::g_Core == nullptr)
        {
            errorMessage = "filter '" + filter.m_Id + "': Core::g_Core is null, cannot resolve credentials";
            LOG_APP_ERROR("[polarion] Core::g_Core is null filter='{}'", filter.m_Id);
            return {};
        }

        // Polarion PAT: api_key stores the Bearer token (Personal Access Token).  Materialise
        // SecureString into request-scoped std::string for the HTTPS calls below.
        std::string bearerToken;
        bool const found = Core::g_Core->GetKeyManager().WithCredential(source.m_KeyName,
            [&](ICredential const& cred)
            {
                auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred);
                if (!api)
                {
                    errorMessage = "filter '" + filter.m_Id + "': credential '" + source.m_KeyName +
                                   "' must be ApiKeyCredential — Polarion requires a PAT";
                    return;
                }
                if (api->m_ApiKey.IsEmpty())
                {
                    errorMessage = "filter '" + filter.m_Id + "': credential '" + source.m_KeyName +
                                   "' is missing api_key (Bearer token)";
                    return;
                }
                bearerToken.assign(api->m_ApiKey.Get());
            });
        if (!found)
        {
            errorMessage = "filter '" + filter.m_Id + "': credential '" + source.m_KeyName +
                           "' not found in KeyManager";
            return {};
        }
        if (!errorMessage.empty())
        {
            return {};
        }

        uint32_t const pageSize = (source.m_PageSize > 0) ? source.m_PageSize : DEFAULT_PAGE_SIZE;
        size_t const maxItems = filter.m_MaxItems;

        // Ensure output directory exists
        std::filesystem::path outDir = std::filesystem::path(workflowBaseDir) / filter.m_Id;
        {
            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            if (ec)
            {
                errorMessage =
                    "filter '" + filter.m_Id + "': cannot create output dir '" + outDir.string() + "': " + ec.message();
                return {};
            }
        }

        std::vector<FilterItem> allItems;
        uint32_t pageNumber = 1;

        while (true)
        {
            std::string const url = BuildPageUrl(source, pageNumber);
            std::string responseBody;

            // Retry logic
            bool pageOk = false;
            for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt)
            {
                if (HttpGet(url, bearerToken, responseBody, errorMessage))
                {
                    pageOk = true;
                    break;
                }

                LOG_APP_WARN("[polarion] filter '{}': page {} attempt {}/{} failed: {}", filter.m_Id, pageNumber, attempt,
                             MAX_RETRIES, errorMessage);

                if (attempt < MAX_RETRIES)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    errorMessage.clear();
                }
            }

            if (!pageOk)
            {
                errorMessage = "filter '" + filter.m_Id + "': page " + std::to_string(pageNumber) + " failed after " +
                               std::to_string(MAX_RETRIES) + " retries: " + errorMessage;
                return {};
            }

            // Parse page
            size_t const startIndex = allItems.size();
            std::vector<FilterItem> pageItems;
            if (!ParseJsonApiPage(responseBody, filter, startIndex, pageItems, errorMessage))
            {
                errorMessage = "filter '" + filter.m_Id + "': JSON parse error on page " + std::to_string(pageNumber) +
                               ": " + errorMessage;
                return {};
            }

            // Write per-item files and collect results
            for (auto& item : pageItems)
            {
                if (maxItems > 0 && allItems.size() >= maxItems)
                {
                    LOG_APP_INFO("[polarion] filter '{}': max_items ({}) reached", filter.m_Id, maxItems);
                    goto done; // break out of both loops
                }

                std::string writeErr;
                if (!WriteItemFile(item, filter, workflowBaseDir, writeErr))
                {
                    LOG_APP_WARN("[polarion] filter '{}': failed to write item file: {}", filter.m_Id, writeErr);
                }

                allItems.push_back(std::move(item));
            }

            // If fewer items returned than page_size, this is the last page
            if (pageItems.size() < pageSize)
            {
                break;
            }

            ++pageNumber;
        }

    done:
        LOG_APP_INFO("[polarion] filter '{}' fetched {} work items across {} pages", filter.m_Id, allItems.size(),
                     pageNumber);

        return allItems;
    }

    // =================================================================
    // BuildPageUrl
    // =================================================================

    std::string PolarionClient::BuildPageUrl(FilterSource const& source, uint32_t pageNumber)
    {
        // GET {base_url}/rest/v1/projects/{project_id}/workitems
        //   ?query={url_encoded_query}
        //   &fields[workitems]={comma_separated_fields}
        //   &page[size]={page_size}
        //   &page[number]={N}

        std::ostringstream url;
        url << source.m_BaseUrl;

        // Ensure no trailing slash
        std::string base = source.m_BaseUrl;
        if (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }

        url.str("");
        url << base << "/rest/v1/projects/" << UrlEncode(source.m_ProjectId) << "/workitems";
        url << "?query=" << UrlEncode(source.m_Query);

        if (!source.m_Fields.empty())
        {
            std::string fieldsCsv;
            for (size_t i = 0; i < source.m_Fields.size(); ++i)
            {
                if (i > 0)
                {
                    fieldsCsv += ",";
                }
                fieldsCsv += source.m_Fields[i];
            }
            url << "&fields%5Bworkitems%5D=" << UrlEncode(fieldsCsv);
        }

        uint32_t const pageSize = (source.m_PageSize > 0) ? source.m_PageSize : DEFAULT_PAGE_SIZE;
        url << "&page%5Bsize%5D=" << pageSize;
        url << "&page%5Bnumber%5D=" << pageNumber;

        return url.str();
    }

    // =================================================================
    // HttpGet — single GET request with Bearer token (PAT) auth
    // =================================================================

    bool PolarionClient::HttpGet(std::string const& url, std::string const& bearerToken, std::string& responseBody,
                                 std::string& errorMessage) const
    {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        responseBody.clear();

        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            size_t totalSize = size * nmemb;
            buf->append(static_cast<char*>(contents), totalSize);
            return totalSize;
        };

        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_SECONDS);
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        std::string const authHeader = "Authorization: Bearer " + bearerToken;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(headers, "Accept: application/vnd.api+json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("curl error: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "HTTP " + std::to_string(httpCode);
            if (!responseBody.empty())
            {
                std::string const sanitized = SanitizeErrorBody(responseBody);
                if (!sanitized.empty()) errorMessage += ": " + sanitized;
            }
            return false;
        }

        return true;
    }

    // =================================================================
    // HttpRequest — generic HTTP method with JSON body
    // =================================================================

    bool PolarionClient::HttpRequest(std::string const& method, std::string const& url,
                                     std::string const& bearerToken, std::string const& requestBody,
                                     std::string& responseBody, std::string& errorMessage) const
    {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        responseBody.clear();

        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            size_t totalSize = size * nmemb;
            buf->append(static_cast<char*>(contents), totalSize);
            return totalSize;
        };

        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_SECONDS);
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        std::string const authHeader = "Authorization: Bearer " + bearerToken;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/vnd.api+json");
        headers = curl_slist_append(headers, "Accept: application/vnd.api+json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        if (!requestBody.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(requestBody.size()));
        }

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("curl error: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "HTTP " + std::to_string(httpCode);
            if (!responseBody.empty())
            {
                std::string const sanitized = SanitizeErrorBody(responseBody);
                if (!sanitized.empty()) errorMessage += ": " + sanitized;
            }
            return false;
        }

        return true;
    }

    // =================================================================
    // HttpDownloadFile — download binary content to a local file
    // =================================================================

    bool PolarionClient::HttpDownloadFile(std::string const& url, std::string const& bearerToken,
                                          std::string const& outputPath, std::string& errorMessage) const
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_SECONDS * 3); // longer timeout for downloads
        // Bound peak file size — prevents a hostile or buggy upstream from
        // streaming gigabytes into the local filesystem.  libcurl aborts the
        // transfer with CURLE_FILESIZE_EXCEEDED when the body grows past this.
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, kMaxDownloadBytes);
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        std::string const authHeader = "Authorization: Bearer " + bearerToken;
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
            errorMessage = "HTTP " + std::to_string(httpCode);
            return false;
        }

        return true;
    }

    // =================================================================
    // HttpUploadFile — multipart form upload
    // =================================================================

    bool PolarionClient::HttpUploadFile(std::string const& url, std::string const& bearerToken,
                                        std::string const& filePath, std::string const& fileName,
                                        std::string& responseBody, std::string& errorMessage) const
    {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        responseBody.clear();

        auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
        {
            auto* buf = static_cast<std::string*>(userp);
            size_t totalSize = size * nmemb;
            buf->append(static_cast<char*>(contents), totalSize);
            return totalSize;
        };

        using WriteFunc = size_t (*)(void*, size_t, size_t, void*);

        curl_mime* mime = curl_mime_init(curl);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filedata(part, filePath.c_str());
        curl_mime_filename(part, fileName.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<WriteFunc>(writeCallback));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_SECONDS * 3);
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        struct curl_slist* headers = nullptr;
        std::string const authHeader = "Authorization: Bearer " + bearerToken;
        headers = curl_slist_append(headers, authHeader.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("curl error: ") + curl_easy_strerror(res);
            return false;
        }

        if (httpCode >= 400)
        {
            errorMessage = "HTTP " + std::to_string(httpCode);
            if (!responseBody.empty())
            {
                std::string const sanitized = SanitizeErrorBody(responseBody);
                if (!sanitized.empty()) errorMessage += ": " + sanitized;
            }
            return false;
        }

        return true;
    }

    // =================================================================
    // UpdateWorkItem — PATCH work item fields
    // =================================================================

    bool PolarionClient::UpdateWorkItem(std::string const& baseUrl, std::string const& projectId,
                                        std::string const& workItemId, std::string const& bearerToken,
                                        std::string const& jsonApiPatchBody, std::string& responseBody,
                                        std::string& errorMessage) const
    {
        if (!IsValidPolarionId(projectId) || !IsValidPolarionId(workItemId))
        {
            errorMessage = "invalid Polarion identifier (allowlist [A-Za-z0-9._-/], no '..', length 1-256)";
            LOG_APP_ERROR("[polarion] UpdateWorkItem rejected ids project='{}' workItem='{}'", projectId, workItemId);
            return false;
        }

        std::string base = baseUrl;
        if (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }

        std::string url = base + "/rest/v1/projects/" + UrlEncode(projectId) +
                          "/workitems/" + UrlEncode(workItemId);

        return HttpRequest("PATCH", url, bearerToken, jsonApiPatchBody, responseBody, errorMessage);
    }

    // =================================================================
    // CreateWorkItem — POST a new work item
    // =================================================================

    bool PolarionClient::CreateWorkItem(std::string const& baseUrl, std::string const& projectId,
                                        std::string const& bearerToken, std::string const& jsonApiPostBody,
                                        std::string& responseBody, std::string& errorMessage) const
    {
        if (!IsValidPolarionId(projectId))
        {
            errorMessage = "invalid Polarion projectId (allowlist [A-Za-z0-9._-/], no '..', length 1-256)";
            LOG_APP_ERROR("[polarion] CreateWorkItem rejected projectId '{}'", projectId);
            return false;
        }

        std::string base = baseUrl;
        if (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }

        std::string url = base + "/rest/v1/projects/" + UrlEncode(projectId) + "/workitems";

        return HttpRequest("POST", url, bearerToken, jsonApiPostBody, responseBody, errorMessage);
    }

    // =================================================================
    // DownloadAttachment — download attachment content to local file
    // =================================================================

    bool PolarionClient::DownloadAttachment(std::string const& baseUrl, std::string const& projectId,
                                            std::string const& workItemId, std::string const& attachmentId,
                                            std::string const& bearerToken, std::string const& outputPath,
                                            std::string& errorMessage) const
    {
        if (!IsValidPolarionId(projectId) || !IsValidPolarionId(workItemId) || !IsValidPolarionId(attachmentId))
        {
            errorMessage = "invalid Polarion identifier (allowlist [A-Za-z0-9._-/], no '..', length 1-256)";
            LOG_APP_ERROR("[polarion] DownloadAttachment rejected ids project='{}' workItem='{}' attachment='{}'",
                          projectId, workItemId, attachmentId);
            return false;
        }
        // Output path containment — defends against an attacker-supplied
        // outputPath escaping the project tree.  Combined with the
        // CURLOPT_MAXFILESIZE cap inside HttpDownloadFile, the worst case is
        // a sized file inside the project tree.
        std::filesystem::path const confinedOutput = ConfineUnderProjectRoot(outputPath);
        if (confinedOutput.empty())
        {
            errorMessage = "attachment output path does not resolve under project root: " + outputPath;
            LOG_APP_ERROR("[polarion] DownloadAttachment outputPath rejected '{}'", outputPath);
            return false;
        }

        std::string base = baseUrl;
        if (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }

        std::string url = base + "/rest/v1/projects/" + UrlEncode(projectId) +
                          "/workitems/" + UrlEncode(workItemId) +
                          "/attachments/" + UrlEncode(attachmentId) + "/content";

        return HttpDownloadFile(url, bearerToken, confinedOutput.string(), errorMessage);
    }

    // =================================================================
    // UploadAttachment — upload a file as work item attachment
    // =================================================================

    bool PolarionClient::UploadAttachment(std::string const& baseUrl, std::string const& projectId,
                                          std::string const& workItemId, std::string const& bearerToken,
                                          std::string const& filePath, std::string const& fileName,
                                          std::string& responseBody, std::string& errorMessage) const
    {
        if (!IsValidPolarionId(projectId) || !IsValidPolarionId(workItemId))
        {
            errorMessage = "invalid Polarion identifier (allowlist [A-Za-z0-9._-/], no '..', length 1-256)";
            LOG_APP_ERROR("[polarion] UploadAttachment rejected ids project='{}' workItem='{}'",
                          projectId, workItemId);
            return false;
        }

        std::string base = baseUrl;
        if (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }

        std::string url = base + "/rest/v1/projects/" + UrlEncode(projectId) +
                          "/workitems/" + UrlEncode(workItemId) + "/attachments";

        return HttpUploadFile(url, bearerToken, filePath, fileName, responseBody, errorMessage);
    }

    // =================================================================
    // FetchLinkedWorkItems — get linked items for traceability traversal
    // =================================================================

    bool PolarionClient::FetchLinkedWorkItems(std::string const& baseUrl, std::string const& projectId,
                                              std::string const& workItemId, std::string const& bearerToken,
                                              std::string& responseBody, std::string& errorMessage) const
    {
        if (!IsValidPolarionId(projectId) || !IsValidPolarionId(workItemId))
        {
            errorMessage = "invalid Polarion identifier (allowlist [A-Za-z0-9._-/], no '..', length 1-256)";
            LOG_APP_ERROR("[polarion] FetchLinkedWorkItems rejected ids project='{}' workItem='{}'",
                          projectId, workItemId);
            return false;
        }

        std::string base = baseUrl;
        if (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }

        std::string url = base + "/rest/v1/projects/" + UrlEncode(projectId) +
                          "/workitems/" + UrlEncode(workItemId) + "/linkedworkitems";

        return HttpGet(url, bearerToken, responseBody, errorMessage);
    }

    // =================================================================
    // ParseJsonApiPage — extract work items from JSON:API response
    // =================================================================

    bool PolarionClient::ParseJsonApiPage(std::string const& responseBody, FilterDef const& filter, size_t startIndex,
                                          std::vector<FilterItem>& itemsOut, std::string& errorMessage)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(responseBody);
        simdjson::ondemand::document document;

        simdjson::error_code ec = parser.iterate(paddedJson).get(document);
        if (ec)
        {
            errorMessage = std::string("JSON parse: ") + simdjson::error_message(ec);
            return false;
        }

        // JSON:API: { "data": [ { "type": "workitems", "id": "...", "attributes": { ... } }, ... ] }
        simdjson::ondemand::array dataArray;
        ec = document["data"].get_array().get(dataArray);
        if (ec)
        {
            errorMessage = "missing or invalid 'data' array in response";
            return false;
        }

        size_t itemIndex = startIndex;
        std::vector<std::string> const& requestedFields = filter.m_Source.m_Fields;

        for (simdjson::ondemand::value element : dataArray)
        {
            simdjson::ondemand::object obj;
            if (element.get_object().get(obj) != simdjson::SUCCESS)
            {
                continue;
            }

            FilterItem item;
            item.m_Index = itemIndex;
            item.m_SourceIndex = itemIndex;

            // Extract work item ID (JSON:API id is "Project/WI-ID")
            std::string_view idSv;
            if (obj["id"].get_string().get(idSv) == simdjson::SUCCESS)
            {
                item.m_Key = std::string(idSv);
                item.m_Values["id"] = std::string(idSv);

                // Derive a filename-safe work_item_id by stripping the project prefix.
                // e.g. "GoKartProcurement/REQ-003" → "REQ-003"
                std::string fullId(idSv);
                auto const slashPos = fullId.rfind('/');
                item.m_Values["work_item_id"] = (slashPos != std::string::npos) ? fullId.substr(slashPos + 1) : fullId;
            }

            // Extract attributes
            simdjson::ondemand::object attributes;
            if (obj["attributes"].get_object().get(attributes) == simdjson::SUCCESS)
            {
                if (requestedFields.empty())
                {
                    // Extract all attributes (strings or rich-text objects with "value" key)
                    for (auto field : attributes)
                    {
                        std::string_view keySv = field.unescaped_key();
                        std::string_view valSv;
                        if (field.value().get_string().get(valSv) == simdjson::SUCCESS)
                        {
                            item.m_Values[std::string(keySv)] = std::string(valSv);
                        }
                        else
                        {
                            // Polarion rich-text fields: {"type":"text/plain","value":"..."}
                            simdjson::ondemand::object richObj;
                            if (field.value().get_object().get(richObj) == simdjson::SUCCESS)
                            {
                                std::string_view richVal;
                                if (richObj["value"].get_string().get(richVal) == simdjson::SUCCESS)
                                {
                                    item.m_Values[std::string(keySv)] = std::string(richVal);
                                }
                            }
                        }
                    }
                }
                else
                {
                    for (auto const& fieldName : requestedFields)
                    {
                        std::string_view valSv;
                        if (attributes[fieldName].get_string().get(valSv) == simdjson::SUCCESS)
                        {
                            item.m_Values[fieldName] = std::string(valSv);
                        }
                        else
                        {
                            // Polarion rich-text fields: {"type":"text/plain","value":"..."}
                            simdjson::ondemand::object richObj;
                            if (attributes[fieldName].get_object().get(richObj) == simdjson::SUCCESS)
                            {
                                std::string_view richVal;
                                if (richObj["value"].get_string().get(richVal) == simdjson::SUCCESS)
                                {
                                    item.m_Values[fieldName] = std::string(richVal);
                                }
                            }
                        }
                    }
                }
            }

            item.m_Values["index"] = std::to_string(itemIndex);
            item.m_SourcePath = filter.m_Source.m_BaseUrl; // for freshness: the API origin

            itemsOut.push_back(std::move(item));
            ++itemIndex;
        }

        return true;
    }

    // =================================================================
    // WriteItemFile — write <filterID>/<filterID>-<k>.json
    // =================================================================

    bool PolarionClient::WriteItemFile(FilterItem const& item, FilterDef const& filter, std::string const& workflowBaseDir,
                                       std::string& errorMessage)
    {
        // Defense in depth on filter.m_Id.  filter.m_Id is JCWF-authored and
        // generally trusted, but a hostile (or buggy) JCWF could place a
        // traversal sequence here and write outside <workflowBaseDir>/.
        // Strict allowlist + path containment close the path-traversal vector.
        if (!IsValidFilesystemId(filter.m_Id))
        {
            errorMessage = "filter.m_Id '" + filter.m_Id + "' contains characters disallowed for filesystem use "
                           "(allowlist: [A-Za-z0-9._-], no leading dot, length 1-64)";
            LOG_APP_ERROR("[polarion] WriteItemFile rejected filter.m_Id '{}' — fails allowlist", filter.m_Id);
            return false;
        }

        std::filesystem::path const rawFilePath = std::filesystem::path(workflowBaseDir) / filter.m_Id /
                                                  (filter.m_Id + "-" + std::to_string(item.m_Index) + ".json");

        // Project-root containment — rejects any symlink escape or `..`
        // sequence that survived the allowlist (defense in depth).
        std::filesystem::path const filePath = ConfineUnderProjectRoot(rawFilePath);
        if (filePath.empty())
        {
            errorMessage = "polarion item file path rejected (escapes project root)";
            LOG_APP_ERROR("[polarion] WriteItemFile path rejected filter='{}' path='{}' — does not resolve under "
                          "project root",
                          filter.m_Id, rawFilePath.string());
            return false;
        }

        // Build a simple JSON object with all values.  Use the canonical
        // JsonHelper::EscapeJsonString for full RFC 8259 escape coverage —
        // pre-fix the local switch handled only 5 characters, leaving raw
        // control bytes (< 0x20) to land verbatim in the JSON output.
        std::ostringstream body;
        body << "{\n";
        size_t count = 0;
        for (auto const& [key, value] : item.m_Values)
        {
            body << "  \"" << JsonHelper::EscapeJsonString(key) << "\": \""
                 << JsonHelper::EscapeJsonString(value) << "\"";
            if (++count < item.m_Values.size())
            {
                body << ",";
            }
            body << "\n";
        }
        body << "}\n";

        // Atomic write through the shared helper: opens <path>.tmp.<counter>,
        // enables ofstream exceptions, writes, closes, then renames.  A
        // SIGKILL or disk-full mid-write leaves the previous version intact.
        std::string writeError;
        if (!EngineCore::AtomicWriteFile(filePath, body.str(), writeError))
        {
            errorMessage = "polarion item file write failed (filter='" + filter.m_Id + "'): " + writeError;
            LOG_APP_ERROR("[polarion] WriteItemFile: {} filter='{}' path='{}'", writeError, filter.m_Id,
                          filePath.string());
            return false;
        }

        return true;
    }

    // =================================================================
    // UrlEncode
    // =================================================================

    std::string PolarionClient::UrlEncode(std::string const& value)
    {
        // Use curl for proper URL encoding
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            // Fallback: minimal manual encoding
            std::string encoded;
            for (char c : value)
            {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~')
                {
                    encoded += c;
                }
                else
                {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                    encoded += buf;
                }
            }
            return encoded;
        }

        char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
        std::string result = escaped ? escaped : value;
        curl_free(escaped);
        curl_easy_cleanup(curl);
        return result;
    }

} // namespace AIAssistant
