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
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "auxiliary/file.h"
#include "engine.h"
#include "cloud/emailCloudTaskExecutor.h"
#include "cloud/emailConnector.h"
#include "cloud/connectorHttp.h"
#include "curlWrapper/curlSlistHelper.h"
#include "curlWrapper/curlWrapper.h"
#include "json/jsonHelper.h"
#include "workflow/taskPathResolver.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

namespace AIAssistant
{
    static constexpr size_t kMaxCaptureChars = 1024;

    // Base64-encode binary data (for MIME attachments)
    static std::string Base64Encode(std::string const& input)
    {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, bmem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, input.data(), static_cast<int>(input.size()));
        (void)BIO_flush(b64);

        BUF_MEM* bptr = nullptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string result(bptr->data, bptr->length);
        BIO_free_all(b64);
        return result;
    }

    // Split a comma-separated string into a vector of trimmed strings
    static std::vector<std::string> SplitRecipients(std::string const& input)
    {
        std::vector<std::string> result;
        std::istringstream stream(input);
        std::string token;
        while (std::getline(stream, token, ','))
        {
            // Trim whitespace
            size_t start = token.find_first_not_of(" \t");
            size_t end = token.find_last_not_of(" \t");
            if (start != std::string::npos)
            {
                result.push_back(token.substr(start, end - start + 1));
            }
        }
        return result;
    }

    // Generate an RFC 2822 date string
    static std::string Rfc2822Date()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm {};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &tm);
        return buf;
    }

    // Build RFC 2822 email message with optional MIME attachments
    static std::string BuildEmailMessage(std::string const& from, std::string const& to, std::string const& cc,
                                          std::string const& subject, std::string const& body,
                                          std::vector<std::pair<std::string, std::string>> const& attachments)
    {
        std::string boundary = "j9t-boundary-" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count());

        std::ostringstream msg;
        msg << "Date: " << Rfc2822Date() << "\r\n";
        msg << "From: " << from << "\r\n";
        msg << "To: " << to << "\r\n";
        if (!cc.empty())
        {
            msg << "Cc: " << cc << "\r\n";
        }
        msg << "Subject: " << subject << "\r\n";
        msg << "MIME-Version: 1.0\r\n";

        if (attachments.empty())
        {
            msg << "Content-Type: text/plain; charset=UTF-8\r\n";
            msg << "\r\n";
            msg << body << "\r\n";
        }
        else
        {
            msg << "Content-Type: multipart/mixed; boundary=\"" << boundary << "\"\r\n";
            msg << "\r\n";

            // Body part
            msg << "--" << boundary << "\r\n";
            msg << "Content-Type: text/plain; charset=UTF-8\r\n";
            msg << "\r\n";
            msg << body << "\r\n";

            // Attachment parts
            for (auto const& [filename, content] : attachments)
            {
                msg << "--" << boundary << "\r\n";
                msg << "Content-Type: application/octet-stream; name=\"" << filename << "\"\r\n";
                msg << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n";
                msg << "Content-Transfer-Encoding: base64\r\n";
                msg << "\r\n";

                // Line-wrap base64 at 76 chars
                std::string encoded = Base64Encode(content);
                for (size_t i = 0; i < encoded.size(); i += 76)
                {
                    msg << encoded.substr(i, 76) << "\r\n";
                }
            }

            msg << "--" << boundary << "--\r\n";
        }

        return msg.str();
    }

    // Curl read callback for uploading email data
    struct EmailUploadContext
    {
        std::string data;
        size_t offset{0};
    };

    static size_t EmailReadCallback(char* buffer, size_t size, size_t nitems, void* userp)
    {
        auto* ctx = static_cast<EmailUploadContext*>(userp);
        size_t maxBytes = size * nitems;
        size_t remaining = ctx->data.size() - ctx->offset;
        size_t toSend = std::min(maxBytes, remaining);
        if (toSend == 0)
        {
            return 0;
        }
        std::memcpy(buffer, ctx->data.data() + ctx->offset, toSend);
        ctx->offset += toSend;
        return toSend;
    }

    // IMAP write callback and command/parse functions are now in EmailConnector.
    // Local aliases for brevity within this file.
    static bool ImapCommand(std::string const& url, std::string const& username, SecureString const& password,
                            std::string const& customRequest, std::string& responseBody,
                            std::string& errorMessage, bool useSsl)
    {
        return EmailConnector::ImapCommand(url, username, password, customRequest, responseBody, errorMessage, useSsl);
    }

    // Extract a header value from raw email text
    static std::string ExtractHeader(std::string const& raw, std::string const& headerName)
    {
        std::string search = headerName + ": ";
        size_t pos = raw.find(search);
        if (pos == std::string::npos)
        {
            // Case-insensitive fallback
            std::string rawLower = raw;
            std::string searchLower = search;
            for (auto& c : rawLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : searchLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            pos = rawLower.find(searchLower);
            if (pos == std::string::npos)
            {
                return {};
            }
        }
        size_t valueStart = pos + search.size();
        size_t lineEnd = raw.find('\r', valueStart);
        if (lineEnd == std::string::npos)
        {
            lineEnd = raw.find('\n', valueStart);
        }
        if (lineEnd == std::string::npos)
        {
            return raw.substr(valueStart);
        }
        return raw.substr(valueStart, lineEnd - valueStart);
    }

    // Extract body from raw email (everything after the first blank line)
    static std::string ExtractBody(std::string const& raw)
    {
        // RFC 2822: headers and body separated by a blank line (\r\n\r\n)
        size_t pos = raw.find("\r\n\r\n");
        if (pos != std::string::npos)
        {
            return raw.substr(pos + 4);
        }
        pos = raw.find("\n\n");
        if (pos != std::string::npos)
        {
            return raw.substr(pos + 2);
        }
        return {};
    }

    // IMAP folder + UID validators live as public static methods on EmailConnector
    // — single source of truth shared between the executor and the connector
    // layer's defense-in-depth gates.  Use those directly.

    static std::vector<std::string> ParseSearchUids(std::string const& searchResponse)
    {
        return EmailConnector::ParseSearchUids(searchResponse);
    }

    bool EmailCloudTaskExecutor::ExecuteEmailRead(WorkflowDefinition const& workflowDefinition,
                                                   TaskDef const& taskDefinition, TaskInstanceState& taskState,
                                                   CloudConnection const& connection,
                                                   CloudCredentials const& credentials)
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            taskState.m_LastErrorMessage = "Failed to parse email_read task params JSON";
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

        std::string folder = getStringParam("folder");
        if (folder.empty())
        {
            folder = "INBOX";
        }

        // Folder is interpolated into the IMAP URL path; an attacker-controlled value
        // such as `INBOX@evil.internal:1234` redirects the connection.  Reject any
        // value outside the strict allowlist before any URL composition.
        if (!EmailConnector::IsValidImapFolder(folder))
        {
            taskState.m_LastErrorMessage = "Invalid IMAP folder name: contains characters outside the allowed "
                                           "set [A-Za-z0-9._/-] or violates structural rules";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[email_read] task='{}' workflow='{}': invalid folder name rejected (length={})",
                          taskDefinition.m_Id, workflowDefinition.m_Id, folder.size());
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] email_read_invalid_folder task='{}' workflow='{}' folder_length={}",
                              taskDefinition.m_Id, workflowDefinition.m_Id, folder.size());
            return false;
        }

        std::string subjectFilter = getStringParam("subject_filter");

        int maxMessages = 10;
        {
            uint64_t val;
            if (doc["max_messages"].get_uint64().get(val) == simdjson::SUCCESS)
            {
                // Clamp to a sane range.  Pre-clamp the unbounded uint64_t cast to int
                // wrapped to a negative number on inputs > INT_MAX (silent corruption);
                // values in the 1..INT_MAX range allowed an attacker to fetch enormous
                // numbers of emails per task, exhausting memory and IMAP server quota.
                static constexpr int kMaxMessageCap = 500;
                if (val > static_cast<uint64_t>(kMaxMessageCap))
                {
                    val = kMaxMessageCap;
                }
                maxMessages = std::clamp(static_cast<int>(val), 1, kMaxMessageCap);
            }
        }

        // Build base IMAP URL
        std::string imapBaseUrl = EmailConnector::BuildImapUrl(connection);
        auto sslIt = connection.m_Params.find("use_ssl");
        bool useSsl = (sslIt == connection.m_Params.end() || sslIt->second != "false");

        // Step 1: SEARCH for messages
        std::string searchUrl = imapBaseUrl + "/" + folder;
        std::string searchCommand = "SEARCH ALL";
        std::string searchResponse;
        std::string imapError;

        if (!ImapCommand(searchUrl, credentials.m_Username, credentials.m_Password, searchCommand, searchResponse,
                         imapError, useSsl))
        {
            taskState.m_LastErrorMessage = "IMAP SEARCH failed: " + imapError;
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::vector<std::string> uids = ParseSearchUids(searchResponse);
        LOG_APP_INFO("[email_read] SEARCH returned {} message(s) in {}", uids.size(), folder);

        // Limit to maxMessages (take the most recent)
        if (static_cast<int>(uids.size()) > maxMessages)
        {
            uids.erase(uids.begin(), uids.end() - maxMessages);
        }

        // Step 2: FETCH each message
        std::ostringstream summaryJson;
        summaryJson << "[";

        int fetchCount = 0;

        // Resolve working directory for output files
        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        std::filesystem::path workDir;
        if (!workflowBaseDir.empty())
        {
            workDir = TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir,
                                                                        taskDefinition.m_WorkingDirectory);
            std::error_code ec;
            std::filesystem::create_directories(workDir, ec);
        }

        for (auto const& uid : uids)
        {
            // UID comes from the server's SEARCH response but is still untrusted input
            // for this module — interpolating an unvalidated value into the FETCH URL
            // would let a malicious or buggy IMAP server inject URL components.
            if (!EmailConnector::IsValidImapUid(uid))
            {
                LOG_APP_WARN("[email_read] task='{}' workflow='{}': skipping UID with invalid format (length={})",
                             taskDefinition.m_Id, workflowDefinition.m_Id, uid.size());
                continue;
            }

            std::string fetchUrl = imapBaseUrl + "/" + folder + "/;UID=" + uid;
            std::string rawEmail;

            if (!ImapCommand(fetchUrl, credentials.m_Username, credentials.m_Password, "", rawEmail, imapError, useSsl))
            {
                LOG_APP_WARN("[email_read] failed to fetch UID {}: {}", uid, imapError);
                continue;
            }

            std::string fromAddr = ExtractHeader(rawEmail, "From");
            std::string toAddr = ExtractHeader(rawEmail, "To");
            std::string subject = ExtractHeader(rawEmail, "Subject");
            std::string date = ExtractHeader(rawEmail, "Date");
            std::string body = ExtractBody(rawEmail);

            // Trim trailing whitespace from body
            while (!body.empty() && (body.back() == '\r' || body.back() == '\n' || body.back() == ' '))
            {
                body.pop_back();
            }

            // Apply subject filter
            if (!subjectFilter.empty())
            {
                if (subject.find(subjectFilter) == std::string::npos)
                {
                    continue;
                }
            }

            if (fetchCount > 0)
            {
                summaryJson << ",";
            }

            summaryJson << "{\"uid\":\"" << JsonHelper::EscapeJsonString(uid) << "\""
                        << ",\"from\":\"" << JsonHelper::EscapeJsonString(fromAddr) << "\""
                        << ",\"to\":\"" << JsonHelper::EscapeJsonString(toAddr) << "\""
                        << ",\"subject\":\"" << JsonHelper::EscapeJsonString(subject) << "\""
                        << ",\"date\":\"" << JsonHelper::EscapeJsonString(date) << "\""
                        << ",\"body\":\"" << JsonHelper::EscapeJsonString(body) << "\"}";

            ++fetchCount;
        }

        summaryJson << "]";

        std::string summaryStr = summaryJson.str();
        taskState.m_CapturedStdout = summaryStr.substr(0, std::min(summaryStr.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write output files
        if (!workDir.empty())
        {
            std::filesystem::path const summaryPath = workDir / "emails_summary.json";
            std::string summaryError;
            if (!EngineCore::AtomicWriteFile(summaryPath, summaryStr, summaryError))
            {
                LOG_APP_ERROR("[email_read] write emails_summary.json failed: {} path='{}'", summaryError,
                              summaryPath.string());
            }

            std::string responseStr = "{\"ok\":true,\"count\":" + std::to_string(fetchCount) +
                                      ",\"folder\":\"" + JsonHelper::EscapeJsonString(folder) + "\"}";
            WriteResponseJson(workDir, taskState, responseStr);
        }

        LOG_APP_INFO("[email_read] fetched {} message(s) from {} via connection '{}'", fetchCount, folder,
                     connection.m_Name);

        return true;
    }

    bool EmailCloudTaskExecutor::ExecuteCloud(WorkflowDefinition const& workflowDefinition,
                                              WorkflowRun& workflowRun, TaskDef const& taskDefinition,
                                              TaskInstanceState& taskState, CloudConnection const& connection,
                                              CloudCredentials const& credentials,
                                              TaskCancellationToken const& cancellationToken)
    {
        // Route by task type: email_read uses IMAP, email_send uses SMTP
        if (taskDefinition.m_Type == TaskType::EmailRead)
        {
            return ExecuteEmailRead(workflowDefinition, taskDefinition, taskState, connection, credentials);
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(taskDefinition.m_ParamsJson);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            taskState.m_LastErrorMessage = "Failed to parse email_send task params JSON";
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

        std::string to = getStringParam("to");
        if (to.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'to' in email_send task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string subject = getStringParam("subject");
        if (subject.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'subject' in email_send task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string body = getStringParam("body");
        std::string bodyFile = getStringParam("body_file");
        if (!bodyFile.empty())
        {
            // body_file resolves relative to the task's working_directory per
            // JCWF spec §3.2.1.  Absolute values (e.g. {{ai_reply.output_file}}
            // templates) pass through; both cases are confined under launchCwd
            // by ValidateLocalPath as the project-tree security boundary.  Same
            // convention as the attachments branch below.
            std::filesystem::path const workflowBaseDir =
                TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
            std::filesystem::path const workDir = TaskPathResolver::ResolveTaskWorkingDirectoryPath(
                workflowBaseDir, taskDefinition.m_WorkingDirectory);
            if (!ValidateLocalPath(bodyFile, workDir, taskDefinition.m_Id))
            {
                taskState.m_LastErrorMessage = "email_send: body_file path is invalid or escapes the project tree";
                taskState.m_State = TaskInstanceStateKind::Failed;
                LOG_APP_ERROR("[email_send] task='{}' workflow='{}' run='{}': body_file path rejected",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
                return false;
            }
            std::filesystem::path const fullBodyPath = workDir / bodyFile;
            std::ifstream ifs(fullBodyPath);
            if (!ifs.is_open())
            {
                taskState.m_LastErrorMessage = "email_send: cannot open body_file '" + fullBodyPath.string() + "'";
                taskState.m_State = TaskInstanceStateKind::Failed;
                return false;
            }
            body.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }
        if (body.empty())
        {
            taskState.m_LastErrorMessage = "Missing required 'body' or 'body_file' in email_send task params";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string cc = getStringParam("cc");

        // Determine sender
        auto fromIt = connection.m_Params.find("from");
        std::string from = (fromIt != connection.m_Params.end() && !fromIt->second.empty())
                               ? fromIt->second
                               : credentials.m_Username;

        // Reject CR/LF in any header field value.  An attacker supplying e.g.
        // `subject: "Hello\r\nBcc: victim@example.com"` would otherwise inject an
        // arbitrary BCC into the outgoing message.  Header fields concatenated by
        // BuildEmailMessage are: from, to, cc, subject.  Body is separately allowed
        // to contain newlines (it's the body content, not a header).
        if (ICloudTaskExecutor::ContainsCrlf(from) || ICloudTaskExecutor::ContainsCrlf(to) ||
            ICloudTaskExecutor::ContainsCrlf(cc) || ICloudTaskExecutor::ContainsCrlf(subject))
        {
            taskState.m_LastErrorMessage = "email_send: header field value contains CR/LF "
                                           "(from/to/cc/subject must not contain newlines)";
            taskState.m_State = TaskInstanceStateKind::Failed;
            LOG_APP_ERROR("[email_send] task='{}' workflow='{}' run='{}': CRLF rejected in header field",
                          taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            LOG_SECURITY_WARN("[security] email_send_header_injection task='{}' workflow='{}' run='{}'",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
            return false;
        }

        // Load attachments if specified
        std::vector<std::pair<std::string, std::string>> attachments;
        {
            simdjson::ondemand::array attachArray;
            if (!doc["attachments"].get_array().get(attachArray))
            {
                std::filesystem::path workflowBaseDir =
                    TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
                std::filesystem::path workDir = TaskPathResolver::ResolveTaskWorkingDirectoryPath(
                    workflowBaseDir, taskDefinition.m_WorkingDirectory);

                for (auto item : attachArray)
                {
                    std::string_view attachPath;
                    if (item.get_string().get(attachPath) == simdjson::SUCCESS)
                    {
                        // Confine each attachment path under workDir.  An absolute
                        // path or `../etc/shadow` would otherwise escape the task's
                        // working directory and exfiltrate arbitrary files as
                        // base64-encoded MIME parts to whatever recipient the task
                        // params name.
                        std::string attachPathStr(attachPath);
                        if (!ValidateLocalPath(attachPathStr, workDir, taskDefinition.m_Id))
                        {
                            LOG_APP_WARN("[email_send] task='{}' workflow='{}' run='{}': attachment path rejected",
                                         taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId);
                            continue;
                        }
                        std::filesystem::path fullPath = workDir / attachPathStr;
                        std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
                        if (file.is_open())
                        {
                            auto const fileSize = file.tellg();
                            // Bound the attachment read.  Without this cap a hostile
                            // (or buggy) workflow could place a multi-GB file in workDir
                            // and exhaust process memory — the contents are read into a
                            // std::string and then base64-encoded (1.33x growth) into
                            // the MIME body.  25 MB matches typical SMTP server limits
                            // (Gmail / Outlook / most providers) and still leaves
                            // headroom for the encoded MIME message size.
                            static constexpr std::streamoff kMaxAttachmentBytes = 25 * 1024 * 1024;
                            if (fileSize < 0 || fileSize > kMaxAttachmentBytes)
                            {
                                LOG_APP_WARN("[email_send] task='{}' workflow='{}' run='{}': attachment '{}' size {} "
                                             "bytes exceeds {} byte cap; skipping",
                                             taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                                             attachPathStr, static_cast<long long>(fileSize),
                                             static_cast<long long>(kMaxAttachmentBytes));
                                continue;
                            }
                            file.seekg(0, std::ios::beg);
                            std::string content(static_cast<size_t>(fileSize), '\0');
                            file.read(content.data(), fileSize);
                            attachments.emplace_back(std::filesystem::path(attachPathStr).filename().string(),
                                                     std::move(content));
                        }
                        else
                        {
                            LOG_APP_WARN("[email] attachment not found: {}", fullPath.string());
                        }
                    }
                }
            }
        }

        // Build the email message
        std::string emailMessage = BuildEmailMessage(from, to, cc, subject, body, attachments);

        // Send via SMTP
        std::string smtpUrl = EmailConnector::BuildSmtpUrl(connection);

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            taskState.m_LastErrorMessage = "curl_easy_init() failed";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, smtpUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, credentials.m_Username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, credentials.m_Password.CStr());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        // TLS posture follows the connection's use_ssl param (matches the IMAP path
        // above).  Default is true — production deployments without an explicit
        // opt-out get strict TLS-required transport with full certificate +
        // hostname verification, regardless of the SMTP port.  An attacker-
        // configured non-587 port (or a typo on a real connection) can no longer
        // silently drop the TLS gate.  use_ssl=false is the local-testing escape
        // hatch (GreenMail / Mailpit on plaintext ports) and emits a SECURITY_WARN
        // so an operator running this against a real server sees the deviation.
        auto sslIt = connection.m_Params.find("use_ssl");
        bool const smtpUseTls = (sslIt == connection.m_Params.end() || sslIt->second != "false");
        if (smtpUseTls)
        {
            // CURLUSESSL_ALL — refuse to proceed without TLS.  Covers both 465
            // (smtps:// implicit TLS via libcurl scheme detection) and 587
            // (STARTTLS).  CURLUSESSL_TRY would silently fall back to plaintext
            // on a MITM-stripped STARTTLS, which is exactly the attack we want
            // to close.
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
            // VERIFYPEER + VERIFYHOST set explicitly: libcurl's defaults can
            // silently fail-open on builds where the trust store is empty or
            // unreachable.  Asserting the values means an unverified TLS session
            // becomes a hard error rather than a silent compromise.
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        }
        else
        {
            LOG_SECURITY_WARN("[security] email_send_tls_disabled task='{}' workflow='{}' run='{}' connection='{}'",
                              taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId,
                              connection.m_Name);
        }

        // Sender
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from.c_str());

        // Recipients
        struct curl_slist* recipients = nullptr;
        auto toList = SplitRecipients(to);
        for (auto const& addr : toList)
        {
            recipients = curl_slist_append(recipients, addr.c_str());
        }
        if (!cc.empty())
        {
            auto ccList = SplitRecipients(cc);
            for (auto const& addr : ccList)
            {
                recipients = curl_slist_append(recipients, addr.c_str());
            }
        }
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

        // Upload the email body
        EmailUploadContext uploadCtx;
        uploadCtx.data = emailMessage;
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, EmailReadCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &uploadCtx);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            taskState.m_LastErrorMessage = std::string("Email send failed: ") + curl_easy_strerror(res);
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        LOG_APP_INFO("[email] sent to {} via connection '{}' (subject: {})", to, connection.m_Name, subject);

        // Escape `to` and `subject` before splicing — the upstream CRLF gate
        // stops the worst injection (newlines), but `"` and `\` would still
        // corrupt the JSON or smuggle additional fields.  JsonHelper produces
        // RFC 8259 §7-compliant escapes.
        std::string summary = "{\"ok\":true,\"to\":\"" + JsonHelper::EscapeJsonString(to) +
                              "\",\"subject\":\"" + JsonHelper::EscapeJsonString(subject) +
                              "\",\"attachments\":" + std::to_string(attachments.size()) + "}";

        taskState.m_CapturedStdout = summary.substr(0, std::min(summary.size(), kMaxCaptureChars));
        taskState.m_State = TaskInstanceStateKind::Succeeded;

        // Write summary to task working directory
        std::filesystem::path workflowBaseDir = TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
        if (!workflowBaseDir.empty())
        {
            std::filesystem::path workDir =
                TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, taskDefinition.m_WorkingDirectory);

            WriteResponseJson(workDir, taskState, summary);
        }

        return true;
    }
} // namespace AIAssistant
