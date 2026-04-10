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

#pragma once

#include <string>
#include <vector>

#include "workflow/filter/filterEngine.h"

namespace AIAssistant
{

    // Performs paginated work-item queries against the Polarion ALM REST API.
    //
    // REST endpoint pattern:
    //   GET {base_url}/rest/v1/projects/{project_id}/workitems
    //     ?query={url_encoded_query}
    //     &fields[workitems]={comma_separated_fields}
    //     &page[size]={page_size}
    //     &page[number]={N}
    //
    // Authentication: HTTP Basic via KeyManager credential lookup (key_name).
    // Results are written to <workflowBaseDir>/<filterID>/<filterID>-<k>.json
    // as each page completes, bounding peak memory to one page.

    class PolarionClient
    {
    public:
        // Fetch all matching work items from Polarion.
        // Paginates automatically; stops when fewer items than page_size
        // are returned or max_items is reached.
        // Writes per-item JSON files under workflowBaseDir/<filterId>/.
        // Returns the full item list as FilterItems.
        std::vector<FilterItem> FetchAll(FilterDef const& filter, std::string const& workflowBaseDir,
                                         std::string& errorMessage) const;

        static constexpr uint32_t DEFAULT_PAGE_SIZE = 100;
        static constexpr int MAX_RETRIES = 3;
        static constexpr long TIMEOUT_SECONDS = 30;

        // Perform a single HTTP GET with Bearer token auth and return the response body.
        bool HttpGet(std::string const& url, std::string const& bearerToken, std::string& responseBody,
                     std::string& errorMessage) const;

        // Perform an HTTP request with an arbitrary method + optional JSON body.
        // Used for PATCH (update) and POST (create) operations.
        bool HttpRequest(std::string const& method, std::string const& url, std::string const& bearerToken,
                         std::string const& requestBody, std::string& responseBody, std::string& errorMessage) const;

        // Download a binary file from the given URL to a local path.
        bool HttpDownloadFile(std::string const& url, std::string const& bearerToken,
                              std::string const& outputPath, std::string& errorMessage) const;

        // Upload a file as a multipart form attachment.
        bool HttpUploadFile(std::string const& url, std::string const& bearerToken,
                            std::string const& filePath, std::string const& fileName,
                            std::string& responseBody, std::string& errorMessage) const;

        // URL-encode a string.
        static std::string UrlEncode(std::string const& value);

        // ----- Write-back: update work item fields -----
        // PATCH /rest/v1/projects/{project_id}/workitems/{work_item_id}
        bool UpdateWorkItem(std::string const& baseUrl, std::string const& projectId,
                            std::string const& workItemId, std::string const& bearerToken,
                            std::string const& jsonApiPatchBody, std::string& responseBody,
                            std::string& errorMessage) const;

        // ----- Creation: create a new work item -----
        // POST /rest/v1/projects/{project_id}/workitems
        bool CreateWorkItem(std::string const& baseUrl, std::string const& projectId,
                            std::string const& bearerToken, std::string const& jsonApiPostBody,
                            std::string& responseBody, std::string& errorMessage) const;

        // ----- Attachments -----
        // GET /rest/v1/projects/{project_id}/workitems/{work_item_id}/attachments/{attachment_id}/content
        bool DownloadAttachment(std::string const& baseUrl, std::string const& projectId,
                                std::string const& workItemId, std::string const& attachmentId,
                                std::string const& bearerToken, std::string const& outputPath,
                                std::string& errorMessage) const;

        // POST /rest/v1/projects/{project_id}/workitems/{work_item_id}/attachments
        bool UploadAttachment(std::string const& baseUrl, std::string const& projectId,
                              std::string const& workItemId, std::string const& bearerToken,
                              std::string const& filePath, std::string const& fileName,
                              std::string& responseBody, std::string& errorMessage) const;

        // ----- Linked items / traceability -----
        // GET /rest/v1/projects/{project_id}/workitems/{work_item_id}/linkedworkitems
        bool FetchLinkedWorkItems(std::string const& baseUrl, std::string const& projectId,
                                  std::string const& workItemId, std::string const& bearerToken,
                                  std::string& responseBody, std::string& errorMessage) const;

    private:
        // Build the full URL for a single page request.
        static std::string BuildPageUrl(FilterSource const& source, uint32_t pageNumber);

        // Parse a JSON:API response page, extract work items into FilterItems.
        static bool ParseJsonApiPage(std::string const& responseBody, FilterDef const& filter, size_t startIndex,
                                     std::vector<FilterItem>& itemsOut, std::string& errorMessage);

        // Write a single item's JSON to <workflowBaseDir>/<filterId>/<filterId>-<k>.json.
        static bool WriteItemFile(FilterItem const& item, FilterDef const& filter, std::string const& workflowBaseDir,
                                  std::string& errorMessage);
    };

} // namespace AIAssistant
