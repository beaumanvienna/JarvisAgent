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

#include "cloud/emailConnector.h"

#include <curl/curl.h>
#include <cctype>
#include <sstream>
#include <stdexcept>

#include "core.h"
#include "engine.h"
#include "keys/keyManager.h"
#include "curlWrapper/curlWrapper.h"
#include "cloud/connectorHttp.h"

namespace AIAssistant
{
    namespace
    {
        // Used at the IMAP send-buffer cap.  10 MB matches the audit recommendation
        // (a SEARCH response that large is already pathological — production
        // mailboxes rarely produce more than tens of KB per response).
        constexpr size_t kMaxImapResponseBytes = 10 * 1024 * 1024;
    } // namespace

    bool EmailConnector::IsValidImapFolder(std::string const& folder)
    {
        if (folder.empty() || folder.size() > 256)
        {
            return false;
        }
        if (folder.front() == '/' || folder.back() == '/')
        {
            return false;
        }
        if (folder.find("//") != std::string::npos)
        {
            return false;
        }
        for (char c : folder)
        {
            unsigned char const uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc))
            {
                continue;
            }
            if (c == '.' || c == '_' || c == '-' || c == '/')
            {
                continue;
            }
            return false;
        }
        return true;
    }

    bool EmailConnector::IsValidImapUid(std::string const& uid)
    {
        if (uid.empty() || uid.size() > 20)
        {
            return false;
        }
        for (char c : uid)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                return false;
            }
        }
        return true;
    }

    bool EmailConnector::IsValidImapSubjectFilter(std::string const& filter)
    {
        if (filter.size() > 256)
        {
            return false;
        }
        for (char c : filter)
        {
            // `"` and `\\` would break out of the SEARCH SUBJECT quoted string;
            // CR / LF would terminate the IMAP command and inject the next one;
            // `{` is the IMAP literal-syntax sentinel.
            if (c == '"' || c == '\\' || c == '\r' || c == '\n' || c == '{')
            {
                return false;
            }
        }
        return true;
    }

    bool EmailConnector::IsValidEmailHost(std::string const& host, bool allowLocalNetwork)
    {
        if (host.empty() || host.size() > 253)
        {
            return false;
        }
        for (char c : host)
        {
            // URL-meaningful chars that would let `host = "evil.com:465/path?x="`
            // smuggle additional URL components past the SMTP/IMAP libcurl URL.
            if (c == ':' || c == '/' || c == '?' || c == '#' || c == '@' || c == '%' || c == '\\')
            {
                return false;
            }
            // Whitespace / line terminators have no place in a hostname.
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
            {
                return false;
            }
        }
        if (!allowLocalNetwork && ConnectorHttp::IsLocalNetworkHost(host))
        {
            return false;
        }
        return true;
    }

    bool EmailConnector::IsValidEmailPort(std::string const& port)
    {
        if (port.empty() || port.size() > 5)
        {
            return false;
        }
        int value = 0;
        for (char c : port)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                return false;
            }
            value = value * 10 + (c - '0');
            if (value > 65535)
            {
                return false;
            }
        }
        return value >= 1;
    }

    std::string EmailConnector::GetType() const
    {
        return "email";
    }

    bool EmailConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                            std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Email connection '" + connection.m_Name + "'";
            return false;
        }

        auto const* provider = Core::g_Core->GetKeyManager().GetProvider(connection.m_KeyName);
        if (!provider)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }

        if (provider->m_Username.empty())
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' has no username — Email requires a credentials (username + password) entry";
            return false;
        }

        credentials.m_AuthType = CloudAuthType::BasicAuth;
        credentials.m_Username = provider->m_Username;
        credentials.m_Password = provider->m_Password;
        return true;
    }

    std::string EmailConnector::BuildSmtpUrl(CloudConnection const& connection)
    {
        auto hostIt = connection.m_Params.find("smtp_host");
        std::string host = (hostIt != connection.m_Params.end()) ? hostIt->second : "";

        auto portIt = connection.m_Params.find("smtp_port");
        std::string port = (portIt != connection.m_Params.end() && !portIt->second.empty()) ? portIt->second : "587";

        auto sslIt = connection.m_Params.find("use_ssl");
        bool useSsl = (sslIt == connection.m_Params.end() || sslIt->second != "false");

        // Local-network hosts (loopback / link-local / private / cloud-metadata IP) are
        // accepted only when use_ssl is explicitly false — the same opt-out the SMTP TLS
        // gate uses.  Production posture (use_ssl=true) rejects them as SSRF vectors.
        bool const allowLocal = !useSsl;
        if (!IsValidEmailHost(host, allowLocal) || !IsValidEmailPort(port))
        {
            LOG_SECURITY_WARN("[security] email_invalid_smtp_target connection='{}' use_ssl={}",
                              connection.m_Name, useSsl ? "true" : "false");
            return {};
        }

        std::string scheme = (useSsl && port == "465") ? "smtps" : "smtp";
        return scheme + "://" + host + ":" + port;
    }

    std::string EmailConnector::BuildImapUrl(CloudConnection const& connection)
    {
        auto hostIt = connection.m_Params.find("imap_host");
        std::string host = (hostIt != connection.m_Params.end()) ? hostIt->second : "";

        auto portIt = connection.m_Params.find("imap_port");
        std::string port = (portIt != connection.m_Params.end() && !portIt->second.empty()) ? portIt->second : "993";

        auto sslIt = connection.m_Params.find("use_ssl");
        bool useSsl = (sslIt == connection.m_Params.end() || sslIt->second != "false");

        // Same SSRF + URL-injection gate as BuildSmtpUrl above.
        bool const allowLocal = !useSsl;
        if (!IsValidEmailHost(host, allowLocal) || !IsValidEmailPort(port))
        {
            LOG_SECURITY_WARN("[security] email_invalid_imap_target connection='{}' use_ssl={}",
                              connection.m_Name, useSsl ? "true" : "false");
            return {};
        }

        std::string scheme = useSsl ? "imaps" : "imap";
        return scheme + "://" + host + ":" + port;
    }

    bool EmailConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        auto smtpHostIt = connection.m_Params.find("smtp_host");
        if (smtpHostIt == connection.m_Params.end() || smtpHostIt->second.empty())
        {
            errorMessage = "Email connection requires 'smtp_host' parameter";
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        // Test SMTP connectivity with EHLO handshake via libcurl
        std::string smtpUrl = BuildSmtpUrl(connection);
        if (smtpUrl.empty())
        {
            // BuildSmtpUrl already emitted a SECURITY_WARN with the rejection reason.
            errorMessage = "Email SMTP target rejected: invalid host or port (see security log)";
            return false;
        }

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, smtpUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, credentials.m_Username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, credentials.m_Password.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        // TLS posture: same shape as the SMTP send path in emailCloudTaskExecutor —
        // gated on the connection's use_ssl param.  Strict mode unconditionally
        // requires TLS plus full peer + hostname verification; opt-out emits a
        // SECURITY_WARN so an operator running plaintext sees the deviation.
        auto sslIt = connection.m_Params.find("use_ssl");
        bool const smtpUseTls = (sslIt == connection.m_Params.end() || sslIt->second != "false");
        if (smtpUseTls)
        {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        }
        else
        {
            LOG_SECURITY_WARN("[security] email_test_tls_disabled connection='{}'", connection.m_Name);
        }

        // CONNECT_ONLY: just establish connection and authenticate, don't send mail
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("Email SMTP test failed: ") + curl_easy_strerror(res);
            return false;
        }

        return true;
    }

    // ========================================================================
    // IMAP utilities (shared by email_read task executor and email_watch trigger)
    // ========================================================================

    static size_t ImapWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        auto* buf = static_cast<std::string*>(userp);
        size_t const incoming = size * nmemb;
        // Cap the response buffer so a hostile or compromised IMAP server cannot
        // exhaust process memory by streaming an unbounded SEARCH response.
        // Returning 0 from the libcurl write callback aborts the transfer with
        // CURLE_WRITE_ERROR, which the caller surfaces as a regular IMAP error.
        if (buf->size() + incoming > kMaxImapResponseBytes)
        {
            return 0;
        }
        buf->append(static_cast<char*>(contents), incoming);
        return incoming;
    }

    bool EmailConnector::ImapCommand(std::string const& url, std::string const& username,
                                     std::string const& password, std::string const& customRequest,
                                     std::string& responseBody, std::string& errorMessage, bool useSsl)
    {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        responseBody.clear();

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ImapWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        if (!customRequest.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, customRequest.c_str());
        }

        // TLS posture: matches the SMTP path's gate.  When useSsl is true (production
        // default), unconditionally require TLS + full cert + hostname verification —
        // refuse to fall through to libcurl's build-defaults, which can fail-open on
        // builds where the trust store is empty or unreachable.  When useSsl is
        // false (local-testing opt-out), explicitly select CURLUSESSL_NONE and emit
        // a SECURITY_WARN so the deviation is observable in the security log.
        if (useSsl)
        {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        }
        else
        {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_NONE));
            LOG_SECURITY_WARN("[security] email_imap_tls_disabled url_scheme='{}'",
                              url.substr(0, std::min<size_t>(url.size(), 6)));
        }

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("IMAP request failed: ") + curl_easy_strerror(res);
            return false;
        }

        return true;
    }

    std::vector<std::string> EmailConnector::ParseSearchUids(std::string const& searchResponse)
    {
        std::vector<std::string> uids;
        size_t pos = searchResponse.find("* SEARCH ");
        if (pos == std::string::npos)
        {
            return uids;
        }

        size_t start = pos + 9; // length of "* SEARCH "
        size_t lineEnd = searchResponse.find('\r', start);
        if (lineEnd == std::string::npos)
        {
            lineEnd = searchResponse.find('\n', start);
        }
        if (lineEnd == std::string::npos)
        {
            lineEnd = searchResponse.size();
        }

        std::string uidLine = searchResponse.substr(start, lineEnd - start);
        std::istringstream ss(uidLine);
        std::string uid;
        while (ss >> uid)
        {
            if (!uid.empty() && std::isdigit(static_cast<unsigned char>(uid[0])))
            {
                uids.push_back(uid);
            }
        }

        return uids;
    }

    std::string EmailConnector::CheckForNewMail(CloudConnection const& connection,
                                                CloudCredentials const& credentials,
                                                std::string const& folder,
                                                std::string const& subjectFilter,
                                                std::string const& lastSeenUid,
                                                bool& hasNewMail,
                                                std::string& errorMessage)
    {
        hasNewMail = false;

        // Defense-in-depth folder validation.  The executor (sitting 11) already
        // validates upstream, but the connector's public API can be invoked from
        // future call sites with no prior validation — and the folder is
        // interpolated directly into the IMAP URL below.
        if (!IsValidImapFolder(folder))
        {
            errorMessage = "Invalid IMAP folder name: contains characters outside the allowed "
                           "set [A-Za-z0-9._/-] or violates structural rules";
            ConnectorHttp::IncrementInputValidationRejection();
            LOG_SECURITY_WARN("[security] email_check_invalid_folder connection='{}' folder_length={}",
                              connection.m_Name, folder.size());
            return {};
        }

        // SearchFilter is interpolated into a quoted IMAP SEARCH SUBJECT argument; rejecting
        // `"`, `\\`, `\r`, `\n`, `{` closes the IMAP command-injection vector.
        if (!IsValidImapSubjectFilter(subjectFilter))
        {
            errorMessage = "Invalid IMAP subject filter: contains characters that would break the "
                           "SEARCH command (\\\", \\\\, CR, LF, {)";
            LOG_SECURITY_WARN("[security] email_check_invalid_subject_filter connection='{}' filter_length={}",
                              connection.m_Name, subjectFilter.size());
            return {};
        }

        // The watermark is read back from the trigger-engine's persisted state — sanitize before
        // any std::stoull call so a tampered watermark doesn't crash the polling thread.
        if (!lastSeenUid.empty() && !IsValidImapUid(lastSeenUid))
        {
            errorMessage = "Invalid lastSeenUid watermark: must be a non-empty digit string";
            LOG_SECURITY_WARN("[security] email_check_invalid_watermark connection='{}' watermark_length={}",
                              connection.m_Name, lastSeenUid.size());
            return {};
        }

        std::string imapBaseUrl = BuildImapUrl(connection);
        if (imapBaseUrl.empty())
        {
            // BuildImapUrl already emitted a SECURITY_WARN.
            errorMessage = "IMAP target rejected: invalid host or port (see security log)";
            return {};
        }
        auto sslIt = connection.m_Params.find("use_ssl");
        bool useSsl = (sslIt == connection.m_Params.end() || sslIt->second != "false");

        std::string searchUrl = imapBaseUrl + "/" + folder;

        // Always use SEARCH ALL — UID SEARCH is not universally supported (e.g. GreenMail).
        // We filter by watermark in code after parsing the results.
        std::string searchCommand = "SEARCH ALL";

        // Append subject filter if set
        if (!subjectFilter.empty())
        {
            searchCommand += " SUBJECT \"" + subjectFilter + "\"";
        }

        std::string searchResponse;
        if (!ImapCommand(searchUrl, credentials.m_Username, credentials.m_Password,
                         searchCommand, searchResponse, errorMessage, useSsl))
        {
            return {};
        }

        std::vector<std::string> uids = ParseSearchUids(searchResponse);
        if (uids.empty())
        {
            // No messages found — return the existing watermark unchanged
            return lastSeenUid;
        }

        // Highest UID is the last element (IMAP UIDs are returned in ascending order)
        std::string const& highestUid = uids.back();

        if (lastSeenUid.empty())
        {
            // First poll: seed the watermark silently — don't fire for existing mail
            return highestUid;
        }

        // Compare numeric UIDs.  ParseSearchUids does an isdigit check on the first
        // byte so the per-element validity is mostly assured, but stoull can still
        // throw out_of_range on a 30-byte all-digits string the parser accepts —
        // wrap the comparisons defensively so a malformed server response cannot
        // crash the polling thread.
        try
        {
            uint64_t const watermark = std::stoull(lastSeenUid);
            for (auto const& uid : uids)
            {
                if (!IsValidImapUid(uid))
                {
                    LOG_APP_WARN("[email_watch] connection='{}': skipping malformed UID (length={})",
                                 connection.m_Name, uid.size());
                    continue;
                }
                if (std::stoull(uid) > watermark)
                {
                    hasNewMail = true;
                    break;
                }
            }
        }
        catch (std::invalid_argument const& e)
        {
            errorMessage = std::string("Invalid IMAP UID format in poll response: ") + e.what();
            LOG_APP_WARN("[email_watch] connection='{}': stoull invalid_argument — treating as no-new-mail",
                         connection.m_Name);
            return highestUid;
        }
        catch (std::out_of_range const& e)
        {
            errorMessage = std::string("IMAP UID overflow in poll response: ") + e.what();
            LOG_APP_WARN("[email_watch] connection='{}': stoull out_of_range — treating as no-new-mail",
                         connection.m_Name);
            return highestUid;
        }

        return highestUid;
    }
} // namespace AIAssistant
