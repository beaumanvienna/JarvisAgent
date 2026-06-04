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

#include "cloud/cloudConnector.h"

#include <string>
#include <vector>

namespace AIAssistant
{
    // PostgreSQL database connector using the libpq C API.
    //
    // CloudConnection.m_Endpoint — connection string or host:port (e.g. "myhost:5432")
    // CloudConnection.m_KeyName  — KeyManager credential (BasicAuth: username/password)
    //
    // CloudConnection.m_Params keys:
    //   "database"   — database name (required)
    //   "sslmode"    — libpq sslmode (optional).  Allowlist: "disable", "allow",
    //                  "prefer", "require", "verify-ca", "verify-full".  Default
    //                  "require" for production posture (TLS mandatory; libpq's
    //                  built-in "prefer" silently falls back to plaintext, which
    //                  is MITM-vulnerable).  For non-localhost hosts, the three
    //                  plaintext-fallback modes ("disable", "allow", "prefer")
    //                  are rejected — only TLS-required modes are acceptable
    //                  (mirrors email's `allowLocal = !useSsl` heuristic).  For
    //                  local-network hosts (loopback / RFC 1918 / link-local),
    //                  any allowlisted mode is accepted as a dev-mode opt-out.
    //
    // The connector builds a libpq connection string from these fields.
    class PostgresConnector : public ICloudConnector
    {
    public:
        std::string GetType() const override;
        [[nodiscard]] std::expected<void, ConnectorError> TestConnection(CloudConnection const& connection) override;
        bool ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                std::string& errorMessage) override;

        // Parse "host[:port]" from connection.m_Endpoint.  Defaults to
        // host="localhost", port="5432" if Endpoint is empty or unparseable.
        // Public so dbQueryCloudTaskExecutor can extract host for IsValidSslMode
        // before calling BuildConnectionString.
        static void ParseHostPort(CloudConnection const& connection, std::string& host, std::string& port);

        // Validate sslmode against libpq's allowlist + j9t's production posture.
        // Returns true if (host, sslmode) is acceptable.  Populates errorMessage
        // on rejection.  Public so dbQueryCloudTaskExecutor can gate before
        // BuildConnectionString.  See class-level docstring for the rules.
        static bool IsValidSslMode(std::string const& host, std::string const& sslmode,
                                    std::string& errorMessage);

        // Reject any libpq keyword/value param that would resolve to a local
        // file path or external file lookup.  j9t's posture: credentials live
        // in the encrypted KeyManager, NOT on the local filesystem; any param
        // that asks libpq to read a path is a path-traversal vector waiting to
        // happen (an attacker-controlled value like `/etc/passwd` for
        // `sslrootcert` would let libpq attempt to read sensitive files
        // server-side).  Forbidden keys: sslcert, sslkey, sslrootcert, sslcrl,
        // sslcrldir, service, passfile, sslpassword.
        //
        // BuildConnectionString currently only forwards `database` and
        // `sslmode` to libpq — everything else in m_Params is silently
        // ignored.  This validator is preventive: if a future PR adds
        // `paramOrDefault("sslcert", ...)` etc. to BuildConnectionString
        // without first removing this gate, the gate fires and rejects the
        // request before it reaches libpq.  Removing this gate without
        // adding `ValidateLocalPath` confinement on the cert/key paths is a
        // code-review reject.
        //
        // Returns `ConnectorError` with `m_Code == InvalidConfig` on
        // rejection.  Public so dbQueryCloudTaskExecutor can gate before
        // BuildConnectionString.
        [[nodiscard]] static std::expected<void, ConnectorError>
            ValidatePostgresParams(CloudConnection const& connection);

        // Output of BuildConnectParams.  Self-contained: holds the storage for the
        // non-secret values (host, port, dbname, user, sslmode, connect_timeout) +
        // the const char* arrays ready for libpq's PQconnectdbParams.  Password is
        // routed directly from CloudCredentials::m_Password.CStr() into m_Values so
        // the secret bytes never appear in a std::string heap allocation (libpq's
        // own internal copy is the irreducible floor — same shape as libcurl's
        // strdup floor).
        //
        // Caller invariant: keep BOTH this struct AND the source CloudCredentials
        // alive for the duration of the PQconnectdbParams call.  Once PQfinish has
        // been called on the resulting PGconn*, libpq has consumed everything and
        // both this struct and the credentials may be destroyed.
        struct ConnectParams
        {
            // Backing storage for the non-secret value strings.  The pointers in
            // m_Values point into these std::strings, so this vector must outlive
            // m_Values and must not be mutated after BuildConnectParams returns.
            std::vector<std::string> m_NonSecretValues;
            // libpq-shaped NULL-terminated arrays.  Pass m_Keys.data() + m_Values.data()
            // to PQconnectdbParams.
            std::vector<char const*> m_Keys;
            std::vector<char const*> m_Values;
        };

        // Build libpq keyword/value arrays from connection config + credentials.  The
        // password slot (when present) points directly at credentials.m_Password.CStr()
        // so the secret bytes never materialise in a std::string heap allocation
        // outside libpq.
        static ConnectParams BuildConnectParams(CloudConnection const& connection,
                                                CloudCredentials const& credentials);
    };
} // namespace AIAssistant
