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
#include "cloud/gitHubCloudTaskExecutor.h"
#include "cloud/gitHubConnector.h"
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlWrapper.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;

    namespace
    {
        // GitHub's owner / repo character set (per the public API docs and the
        // GitHub web UI signup form): alphanumerics plus `_` `.` `-`, must NOT
        // start with `-` or `.`.  Owner cap is 39 chars in practice (login),
        // repo cap is 100; we use 100 as the upper bound for both to keep a
        // single helper.  An allowlist forecloses URL-injection via `/`, `?`,
        // `#`, `..`, whitespace, or control bytes before the value ever touches
        // string concatenation.
        bool IsValidGitHubName(std::string_view name)
        {
            if (name.empty() || name.size() > 100)
            {
                return false;
            }
            if (name.front() == '-' || name.front() == '.')
            {
                return false;
            }
            for (char c : name)
            {
                bool const ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }

        // GitHub issue / PR numbers are positive integers; cap at 10 digits to
        // stay safely under INT32_MAX without depending on stoi/atoi parse
        // errors.
        bool IsValidIssueNumber(std::string_view value)
        {
            if (value.empty() || value.size() > 10)
            {
                return false;
            }
            for (char c : value)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
            }
            return true;
        }

        // GitHub refs (branch / tag / SHA / refs/heads/x) allow `/` between
        // segments and the standard alnum + `_` `.` `-` set within segments.
        // We reject empty segments and `..` segments structurally — those
        // patterns are not legal Git refs (per git-check-ref-format) and an
        // attacker passing them is trying to break URL containment.
        bool IsValidGitHubRef(std::string_view value)
        {
            if (value.empty() || value.size() > 255)
            {
                return false;
            }
            size_t pos = 0;
            while (pos < value.size())
            {
                size_t const sep = value.find('/', pos);
                std::string_view const segment = (sep == std::string_view::npos)
                                                     ? value.substr(pos)
                                                     : value.substr(pos, sep - pos);
                if (segment.empty() || segment == "..")
                {
                    return false;
                }
                for (char c : segment)
                {
                    bool const ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                    if (!ok)
                    {
                        return false;
                    }
                }
                if (sep == std::string_view::npos)
                {
                    break;
                }
                pos = sep + 1;
            }
            return true;
        }
    } // namespace


    // Base64-decode (for GitHub file content which is base64-encoded)
    static std::string Base64Decode(std::string const& input)
    {
        // Strip whitespace/newlines from GitHub's base64 output
        std::string clean;
        clean.reserve(input.size());
        for (char c : input)
        {
            if (c != '\n' && c != '\r' && c != ' ')
            {
                clean += c;
            }
        }

        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new_mem_buf(clean.data(), static_cast<int>(clean.size()));
        bmem = BIO_push(b64, bmem);
        BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);

        std::string result(clean.size(), '\0');
        int len = BIO_read(bmem, result.data(), static_cast<int>(result.size()));
        BIO_free_all(bmem);

        if (len > 0)
        {
            result.resize(static_cast<size_t>(len));
        }
        else
        {
            result.clear();
        }
        return result;
    }

    static bool GitHubRequest(std::string const& method, std::string const& url, std::string const& token,
                              std::string& responseBody, long& httpCode, std::string const& requestBody = {})
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "j9t/1.0");
        // Hardened TLS + no redirect-following — the GitHub REST API at
        // `api.github.com` responds directly for JSON endpoints; legitimate
        // flows never 30x.  See ConnectorHttp::ApplyHardenedDefaults docstring.
        ConnectorHttp::ApplyHardenedDefaults(curl, url);

        if (!requestBody.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        }

        struct curl_slist* headers = nullptr;
        std::string authHeader = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
        if (!requestBody.empty())
        {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    bool GitHubCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
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
            taskState.m_LastErrorMessage = "Failed to parse github_issue task params JSON";
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
            taskState.m_LastErrorMessage = "Missing required 'operation' in github_issue task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        // Owner/repo: task params override connection defaults
        std::string owner = getStringParam("owner");
        std::string repo = getStringParam("repo");
        auto getConnParam = [&connection](std::string const& key) -> std::string
        {
            auto it = connection.m_Params.find(key);
            return (it != connection.m_Params.end()) ? it->second : std::string{};
        };
        if (owner.empty()) owner = getConnParam("owner");
        if (repo.empty()) repo = getConnParam("repo");

        // Allowlist-validate owner / repo BEFORE they touch any URL or log line.
        // Both are user-supplied (task params override connection defaults), so
        // any URL component that flows from here must be allowlist-shaped first
        // (URL-injection / SSRF defense per `feedback_allowlist_not_blocklist`).
        // The op-specific "missing param" check still applies in each branch.
        if (!owner.empty() && !IsValidGitHubName(owner))
        {
            taskState.m_LastErrorMessage = "github_issue: invalid 'owner' (must match [A-Za-z0-9._-], 1-100 chars, not "
                                           "starting with '-' or '.')";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[github] invalid owner task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                          workflowDefinition.m_Id, workflowRun.m_RunId, owner);
            return false;
        }
        if (!repo.empty() && !IsValidGitHubName(repo))
        {
            taskState.m_LastErrorMessage = "github_issue: invalid 'repo' (must match [A-Za-z0-9._-], 1-100 chars, not "
                                           "starting with '-' or '.')";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[github] invalid repo task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                          workflowDefinition.m_Id, workflowRun.m_RunId, repo);
            return false;
        }

        // Owner / repo are now allowlist-clean; encode for URL splice.  This
        // is defense-in-depth: the allowlist already excludes URL-meta chars,
        // but percent-encoding ensures a future allowlist relaxation can't
        // silently re-introduce URL injection.  Encode-after-validate is the
        // canonical pattern from the dbQuery / polarionWrite hardening.
        std::string const ownerEnc = ConnectorHttp::UrlEncodeComponent(owner);
        std::string const repoEnc = ConnectorHttp::UrlEncodeComponent(repo);

        std::string apiBase = GitHubConnector::GetApiBaseUrl(connection);
        std::string responseBody;
        long httpCode = 0;

        if (operation == "create")
        {
            if (owner.empty() || repo.empty())
            {
                taskState.m_LastErrorMessage = "github_issue 'create' requires 'owner' and 'repo'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::string title = getStringParam("title");
            std::string body = getStringParam("body");
            if (title.empty())
            {
                taskState.m_LastErrorMessage = "github_issue 'create' requires 'title'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            std::string requestBody = "{\"title\":\"" + JsonHelper::EscapeJsonString(title) + "\"";
            if (!body.empty())
            {
                requestBody += ",\"body\":\"" + JsonHelper::EscapeJsonString(body) + "\"";
            }

            // Labels array
            simdjson::ondemand::array labelsArray;
            if (!doc["labels"].get_array().get(labelsArray))
            {
                requestBody += ",\"labels\":[";
                bool first = true;
                for (auto label : labelsArray)
                {
                    std::string_view sv;
                    if (label.get_string().get(sv) == simdjson::SUCCESS)
                    {
                        if (!first) requestBody += ",";
                        requestBody += "\"" + JsonHelper::EscapeJsonString(std::string(sv)) + "\"";
                        first = false;
                    }
                }
                requestBody += "]";
            }
            requestBody += "}";

            std::string url = apiBase + "/repos/" + ownerEnc + "/" + repoEnc + "/issues";

            if (!GitHubRequest("POST", url, credentials.m_Token, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "GitHub create issue request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "GitHub create issue failed: HTTP " + std::to_string(httpCode);
                if (!responseBody.empty() && responseBody.size() < 500)
                {
                    taskState.m_LastErrorMessage += ": " + responseBody;
                }
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[github] created issue in {}/{}", owner, repo);
        }
        else if (operation == "comment")
        {
            std::string issueNumber = getStringParam("issue_number");
            std::string body = getStringParam("body");
            if (issueNumber.empty() || body.empty())
            {
                taskState.m_LastErrorMessage = "github_issue 'comment' requires 'issue_number' and 'body'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (!IsValidIssueNumber(issueNumber))
            {
                taskState.m_LastErrorMessage = "github_issue 'comment': 'issue_number' must be a positive integer";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[github] invalid issue_number task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, workflowRun.m_RunId, issueNumber);
                return false;
            }

            std::string url = apiBase + "/repos/" + ownerEnc + "/" + repoEnc + "/issues/" + issueNumber + "/comments";
            std::string requestBody = "{\"body\":\"" + JsonHelper::EscapeJsonString(body) + "\"}";

            if (!GitHubRequest("POST", url, credentials.m_Token, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "GitHub comment request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "GitHub comment failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[github] commented on {}/{}#{}", owner, repo, issueNumber);
        }
        else if (operation == "close")
        {
            std::string issueNumber = getStringParam("issue_number");
            if (issueNumber.empty())
            {
                taskState.m_LastErrorMessage = "github_issue 'close' requires 'issue_number'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            if (!IsValidIssueNumber(issueNumber))
            {
                taskState.m_LastErrorMessage = "github_issue 'close': 'issue_number' must be a positive integer";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[github] invalid issue_number task='{}' workflow='{}' run='{}': '{}'", taskDefinition.m_Id,
                              workflowDefinition.m_Id, workflowRun.m_RunId, issueNumber);
                return false;
            }

            std::string url = apiBase + "/repos/" + ownerEnc + "/" + repoEnc + "/issues/" + issueNumber;
            std::string requestBody = "{\"state\":\"closed\"}";

            if (!GitHubRequest("PATCH", url, credentials.m_Token, responseBody, httpCode, requestBody))
            {
                taskState.m_LastErrorMessage = "GitHub close issue request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "GitHub close failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[github] closed {}/{}#{}", owner, repo, issueNumber);
        }
        else if (operation == "get_file")
        {
            std::string path = getStringParam("path");
            if (path.empty())
            {
                taskState.m_LastErrorMessage = "github_issue 'get_file' requires 'path'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Per-segment validate + percent-encode the GitHub repo-relative
            // path.  Rejects `..`, `.`, empty segments, embedded NUL.  The
            // encoded form has every byte that isn't unreserved (except `/`)
            // percent-encoded, so a path containing `?` / `#` / `&` / unicode
            // can't escape the URL path into the query / fragment.
            std::string pathEncodeError;
            std::string const pathEnc = ConnectorHttp::UrlEncodePathSegments(path, pathEncodeError);
            if (pathEnc.empty())
            {
                taskState.m_LastErrorMessage =
                    "github_issue 'get_file': invalid 'path' (" + pathEncodeError + ")";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[github] invalid get_file path task='{}' workflow='{}' run='{}': '{}' ({})",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, path,
                              pathEncodeError);
                return false;
            }

            std::string url = apiBase + "/repos/" + ownerEnc + "/" + repoEnc + "/contents/" + pathEnc;
            std::string ref = getStringParam("ref");
            if (!ref.empty())
            {
                if (!IsValidGitHubRef(ref))
                {
                    taskState.m_LastErrorMessage = "github_issue 'get_file': invalid 'ref' (must match git-check-ref-"
                                                   "format-ish: [A-Za-z0-9._/-], no '..' segment, 1-255 chars)";
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    LOG_APP_ERROR("[github] invalid get_file ref task='{}' workflow='{}' run='{}': '{}'",
                                  taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, ref);
                    return false;
                }
                std::string const refEnc = ConnectorHttp::UrlEncodeComponent(ref);
                url += "?ref=" + refEnc;
            }

            if (!GitHubRequest("GET", url, credentials.m_Token, responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "GitHub get_file request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "GitHub get_file failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            // Decode base64 content and write to disk
            simdjson::ondemand::parser respParser;
            simdjson::padded_string respPadded(responseBody);
            simdjson::ondemand::document respDoc;

            if (!respParser.iterate(respPadded).get(respDoc))
            {
                std::string_view content;
                if (!respDoc["content"].get_string().get(content))
                {
                    std::string decoded = Base64Decode(std::string(content));

                    std::filesystem::path workflowBaseDir =
                        TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
                    std::filesystem::path workDir = TaskPathResolver::ResolveTaskWorkingDirectoryPath(
                        workflowBaseDir, taskDefinition.m_WorkingDirectory);

                    std::error_code ec;
                    std::filesystem::create_directories(workDir, ec);

                    // Filename derivation: take just the trailing segment of the
                    // validated path.  Since the validator already rejected `..`
                    // segments, the resulting filename is a relative leaf name.
                    // ConfineUnderProjectRoot is the canonical containment gate
                    // (see `feedback_path_containment_scope`) — even though the
                    // workDir is task-local, a hostile workflow_base_directory
                    // in `workflowDefinition` could move workDir outside the
                    // project tree.  Fail-closed if the resolved write path
                    // escapes the project root.
                    std::string filename = std::filesystem::path(path).filename().string();
                    if (filename.empty() || filename == "." || filename == "..")
                    {
                        taskState.m_LastErrorMessage =
                            "github_issue 'get_file': cannot derive a filename from 'path'";
                        taskState.m_State = TaskInstanceStateKind::Failed;
                        LOG_APP_ERROR("[github] cannot derive filename task='{}' workflow='{}' run='{}': '{}'",
                                      taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, path);
                        return false;
                    }
                    std::filesystem::path const writePathRaw = workDir / filename;
                    std::filesystem::path const writePath = ConfineUnderProjectRoot(writePathRaw);
                    if (writePath.empty())
                    {
                        taskState.m_LastErrorMessage =
                            "github_issue 'get_file': resolved write path does not lie under the project root";
                        taskState.m_State = TaskInstanceStateKind::Failed;
                        LOG_APP_ERROR("[github] write path rejected task='{}' workflow='{}' run='{}' path='{}'",
                                      taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                                      writePathRaw.string());
                        return false;
                    }
                    std::ofstream outFile(writePath, std::ios::binary | std::ios::trunc);
                    if (outFile.is_open())
                    {
                        outFile.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
                    }
                }
            }

            LOG_APP_INFO("[github] retrieved file {}/{}/{}", owner, repo, path);
        }
        else if (operation == "list_issues")
        {
            std::string url = apiBase + "/repos/" + ownerEnc + "/" + repoEnc + "/issues?state=open&per_page=100";

            if (!GitHubRequest("GET", url, credentials.m_Token, responseBody, httpCode))
            {
                taskState.m_LastErrorMessage = "GitHub list_issues request failed";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            if (httpCode >= 400)
            {
                taskState.m_LastErrorMessage = "GitHub list_issues failed: HTTP " + std::to_string(httpCode);
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }

            LOG_APP_INFO("[github] listed issues for {}/{}", owner, repo);
        }
        else
        {
            taskState.m_LastErrorMessage =
                "Unknown github_issue operation '" + operation + "'. Valid: create, comment, close, get_file, list_issues";
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
