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

#include "keys/secureString.h"
#include <vector>

#include "cloud/cloudConnector.h"

namespace AIAssistant
{
    // Email connector for SMTP send and IMAP read via libcurl.
    //
    // CloudConnection.m_Params keys:
    //   "smtp_host"     — SMTP server host (e.g. "smtp.gmail.com"), required for send
    //   "smtp_port"     — SMTP port (default: "587" for STARTTLS, "465" for SSL)
    //   "imap_host"     — IMAP server host (e.g. "imap.gmail.com"), required for email_watch
    //   "imap_port"     — IMAP port (default: "993")
    //   "from"          — sender email address (default: username from credential)
    //   "use_ssl"       — "true" (default) or "false"
    //
    // CloudConnection.m_KeyName  — KeyManager credential (BasicAuthCredential with email + password/app password)
    // CloudConnection.m_AuthType — must be BasicAuth
    class EmailConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        [[nodiscard]] std::expected<void, ConnectorError> TestConnection(CloudConnection const& connection) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        // Build the SMTP URL for libcurl (e.g. "smtps://smtp.gmail.com:465" or "smtp://smtp.gmail.com:587")
        static std::string BuildSmtpUrl(CloudConnection const& connection);

        // Build the IMAP URL for libcurl (e.g. "imaps://imap.gmail.com:993")
        static std::string BuildImapUrl(CloudConnection const& connection);

        // Perform a single IMAP command via libcurl and return the response body.
        static bool ImapCommand(std::string const& url, std::string const& username, SecureString const& password,
                                std::string const& customRequest, std::string& responseBody,
                                std::string& errorMessage, bool useSsl);

        // Parse UIDs from an IMAP SEARCH response (format: "* SEARCH 1 2 3\r\n").
        static std::vector<std::string> ParseSearchUids(std::string const& searchResponse);

        // Check an IMAP folder for messages newer than lastSeenUid.
        // Returns the highest UID found, or "" on error / no messages.
        // On first call (lastSeenUid empty), seeds the watermark without reporting new mail
        // (hasNewMail will be false).
        static std::string CheckForNewMail(CloudConnection const& connection,
                                           CloudCredentials const& credentials,
                                           std::string const& folder,
                                           std::string const& subjectFilter,
                                           std::string const& lastSeenUid,
                                           bool& hasNewMail,
                                           std::string& errorMessage);

        // ===================================================================
        // Input validators (used by both this connector and emailCloudTaskExecutor).
        // Public statics so callers across the email surface share a single source
        // of truth for the allowlists; the executor-side and connector-side gates
        // use the same predicates.
        // ===================================================================

        // IMAP mailbox name: alphanumeric + `.` + `_` + `/` + `-`, max 256 bytes,
        // no leading/trailing `/`, no `//`.  Matches RFC 3501 hierarchy delimiters
        // (`.` and `/`) so legitimate folder names like `[Gmail]/Sent Mail` and
        // `Projects.j9t` validate; injection chars `:`, `@`, `?`, `#`, `%`, CR, LF,
        // whitespace are rejected.
        static bool IsValidImapFolder(std::string const& folder);

        // IMAP UID: digits only, max 20 bytes (10^20 > UINT64_MAX so longer values
        // are meaningless).  RFC 3501 nz-number per the protocol grammar.
        static bool IsValidImapUid(std::string const& uid);

        // IMAP SEARCH SUBJECT filter: rejects bytes that would break out of the
        // quoted-string envelope or inject IMAP literal syntax (`"`, `\\`, `\r`,
        // `\n`, `{`).  Empty filter means "no subject filter" and validates as true.
        // Length capped at 256 bytes to bound the SEARCH command size.
        static bool IsValidImapSubjectFilter(std::string const& filter);

        // SMTP/IMAP host: rejects URL-meaningful chars (`:`, `/`, `?`, `#`, `@`,
        // `%`, `\\`, CR, LF, whitespace), empty, and >253-byte values.  When
        // `allowLocalNetwork` is false (production posture, gated on use_ssl=true),
        // also rejects loopback / link-local / private / cloud-metadata IP ranges
        // and the literal `localhost` hostname — closing SSRF to internal
        // services.  When true (gated on use_ssl=false, the local-testing
        // opt-out), local-network hosts are permitted.
        static bool IsValidEmailHost(std::string const& host, bool allowLocalNetwork);

        // SMTP/IMAP port: digits only, [1, 65535], max 5 bytes.
        static bool IsValidEmailPort(std::string const& port);
    };
} // namespace AIAssistant
