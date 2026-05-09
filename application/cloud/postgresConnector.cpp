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

#include "cloud/postgresConnector.h"

#include <libpq-fe.h>

#include "core.h"
#include "engine.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "cloud/connectorHttp.h"

namespace AIAssistant
{
    std::string PostgresConnector::GetType() const
    {
        return "postgres";
    }

    bool PostgresConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                               std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for PostgreSQL connection '" + connection.m_Name + "'";
            return false;
        }

        auto const* cred = Core::g_Core->GetKeyManager().GetCredential(connection.m_KeyName);
        if (!cred)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }

        auto const* basic = dynamic_cast<BasicAuthCredential const*>(cred);
        if (!basic)
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' must be BasicAuthCredential — PostgreSQL requires username + password";
            return false;
        }

        credentials.m_AuthType = CloudAuthType::BasicAuth;
        credentials.m_Username = basic->m_Username;
        credentials.m_Password = std::string(basic->m_Password.Get());

        if (credentials.m_Username.empty())
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' has no username for PostgreSQL";
            return false;
        }

        return true;
    }

    void PostgresConnector::ParseHostPort(CloudConnection const& connection, std::string& host, std::string& port)
    {
        host = "localhost";
        port = "5432";

        if (connection.m_Endpoint.empty())
        {
            return;
        }

        // Bracketed IPv6 literal — `[fc00::1]:5432` or just `[fc00::1]`.  The
        // brackets are URL syntax to disambiguate the inner colons from the
        // host:port separator; libpq's libpq-fe accepts the bare IPv6 form,
        // so we strip the brackets after extracting the optional port.  Without
        // bracket-stripping, `host = "[fc00::1]"` would have a leading `[` that
        // fails IsLocalNetworkHost's IPv6-literal classifier.
        if (connection.m_Endpoint.front() == '[')
        {
            std::size_t const closeBracket = connection.m_Endpoint.find(']');
            if (closeBracket == std::string::npos)
            {
                // Malformed (open bracket, no close) — fall through to the
                // generic path which will likely produce something libpq
                // rejects.  Don't try to second-guess.
                host = connection.m_Endpoint;
                return;
            }
            host = connection.m_Endpoint.substr(1, closeBracket - 1);
            // Optional `:port` after the closing bracket.
            if (closeBracket + 1 < connection.m_Endpoint.size() &&
                connection.m_Endpoint[closeBracket + 1] == ':')
            {
                std::string const afterColon = connection.m_Endpoint.substr(closeBracket + 2);
                bool isPort = !afterColon.empty();
                for (char c : afterColon)
                {
                    if (c < '0' || c > '9')
                    {
                        isPort = false;
                        break;
                    }
                }
                if (isPort)
                {
                    port = afterColon;
                }
            }
            return;
        }

        size_t const colonPos = connection.m_Endpoint.rfind(':');
        if (colonPos != std::string::npos && colonPos > 0)
        {
            // Check if everything after the colon is digits (port)
            std::string const afterColon = connection.m_Endpoint.substr(colonPos + 1);
            bool isPort = !afterColon.empty();
            for (char c : afterColon)
            {
                if (c < '0' || c > '9')
                {
                    isPort = false;
                    break;
                }
            }

            if (isPort)
            {
                host = connection.m_Endpoint.substr(0, colonPos);
                port = afterColon;
            }
            else
            {
                host = connection.m_Endpoint;
            }
        }
        else
        {
            host = connection.m_Endpoint;
        }
    }

    bool PostgresConnector::ValidatePostgresParams(CloudConnection const& connection, std::string& errorMessage)
    {
        // Forbid any libpq param that would resolve to a local file path or
        // external file lookup.  See header docstring for the threat model.
        // The list is libpq's full set of file-path-bearing connection
        // parameters per https://www.postgresql.org/docs/current/libpq-connect.html;
        // any of these in m_Params is a configuration smell that has no
        // legitimate use in j9t (credentials live in KeyManager, not on disk).
        static char const* const kForbiddenKeys[] = {
            "sslcert",       // client certificate file
            "sslkey",        // client private key file
            "sslrootcert",   // root CA certificate file
            "sslcrl",        // certificate revocation list file
            "sslcrldir",     // CRL directory
            "sslpassword",   // passphrase for sslkey (would be a disk secret)
            "service",       // libpq service file lookup
            "passfile",      // libpq password file
        };
        for (auto const& [key, value] : connection.m_Params)
        {
            for (char const* forbidden : kForbiddenKeys)
            {
                if (key == forbidden)
                {
                    errorMessage = "Forbidden PostgreSQL connection param '" + key +
                                   "': libpq file-path / cert params have no legitimate use in j9t — "
                                   "credentials live in KeyManager, not on disk";
                    ConnectorHttp::IncrementPostgresForbiddenParamRejection();
                    return false;
                }
            }
        }
        return true;
    }

    bool PostgresConnector::IsValidSslMode(std::string const& host, std::string const& sslmode,
                                            std::string& errorMessage)
    {
        // Allowlist of libpq sslmode values.  Anything outside the list is a
        // typo or hostile input and would either be rejected by libpq with a
        // confusing error or fall through to its default.  Reject early.
        bool const isAllValid = (sslmode == "disable" || sslmode == "allow" || sslmode == "prefer" ||
                                  sslmode == "require" || sslmode == "verify-ca" ||
                                  sslmode == "verify-full");
        if (!isAllValid)
        {
            errorMessage = "Invalid PostgreSQL sslmode '" + sslmode +
                           "': must be one of disable, allow, prefer, require, verify-ca, verify-full";
            ConnectorHttp::IncrementPostgresInvalidSslmodeRejection();
            return false;
        }

        // Production posture: for non-localhost hosts, the three plaintext-
        // fallback modes are unsafe.  `disable` skips TLS entirely; `allow`
        // and `prefer` try plaintext / TLS first respectively but silently
        // fall back to the other if the server doesn't accept the preferred
        // protocol — both are MITM-vulnerable.  Mirrors email's
        // `allowLocal = !useSsl` posture.  For local-network hosts (loopback
        // / RFC 1918 / link-local), all 6 modes are accepted as a dev opt-out.
        bool const isPlaintextFallback = (sslmode == "disable" || sslmode == "allow" || sslmode == "prefer");
        if (isPlaintextFallback && !ConnectorHttp::IsLocalNetworkHost(host))
        {
            errorMessage = "PostgreSQL sslmode '" + sslmode +
                           "' permits plaintext fallback and is not safe for non-local host '" + host +
                           "'; use require, verify-ca, or verify-full";
            ConnectorHttp::IncrementPostgresInvalidSslmodeRejection();
            return false;
        }
        return true;
    }

    std::string PostgresConnector::BuildConnectionString(CloudConnection const& connection,
                                                          CloudCredentials const& credentials)
    {
        std::string host;
        std::string port;
        ParseHostPort(connection, host, port);

        auto paramOrDefault = [&connection](std::string const& key, std::string const& defaultValue) -> std::string
        {
            auto it = connection.m_Params.find(key);
            return (it != connection.m_Params.end() && !it->second.empty()) ? it->second : defaultValue;
        };

        std::string const database = paramOrDefault("database", "postgres");
        // Default sslmode is "require" — production posture mandates TLS.
        // libpq's own default is "prefer", which silently falls back to
        // plaintext if the server doesn't accept TLS — MITM-vulnerable.  For
        // a local non-TLS pg, set `sslmode=disable` explicitly (only allowed
        // when the host is localhost / RFC 1918 / link-local — see
        // IsValidSslMode).  TestConnection + dbQueryCloudTaskExecutor both
        // gate on IsValidSslMode before reaching BuildConnectionString, so
        // an invalid value never makes it into the connection string.
        std::string const sslmode = paramOrDefault("sslmode", "require");

        // Build connection string — libpq keyword/value format.
        // Use single-quote escaping for values that might contain special chars.
        auto escape = [](std::string const& val) -> std::string
        {
            std::string out = "'";
            for (char c : val)
            {
                if (c == '\\' || c == '\'')
                {
                    out += '\\';
                }
                out += c;
            }
            out += "'";
            return out;
        };

        std::string connStr;
        connStr += "host=" + escape(host);
        connStr += " port=" + escape(port);
        connStr += " dbname=" + escape(database);
        connStr += " user=" + escape(credentials.m_Username);
        if (!credentials.m_Password.empty())
        {
            connStr += " password=" + escape(credentials.m_Password);
        }
        connStr += " sslmode=" + escape(sslmode);
        connStr += " connect_timeout=10";

        return connStr;
    }

    bool PostgresConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        auto databaseIt = connection.m_Params.find("database");
        if (databaseIt == connection.m_Params.end() || databaseIt->second.empty())
        {
            errorMessage = "PostgreSQL connection requires 'database' parameter";
            return false;
        }

        // Tripwire: reject forbidden libpq cert/key/file-path params before
        // BuildConnectionString.  See ValidatePostgresParams docstring.
        if (!ValidatePostgresParams(connection, errorMessage))
        {
            LOG_SECURITY_WARN("[security] postgres_forbidden_param connection='{}' message='{}'",
                              connection.m_Name, errorMessage);
            return false;
        }

        // Gate on sslmode allowlist + non-localhost production posture before
        // any network I/O — a misconfigured `sslmode=prefer` against a
        // non-localhost host would otherwise silently fall back to plaintext.
        std::string host;
        std::string port;
        ParseHostPort(connection, host, port);
        auto const sslmodeIt = connection.m_Params.find("sslmode");
        std::string const sslmode = (sslmodeIt != connection.m_Params.end() && !sslmodeIt->second.empty())
                                         ? sslmodeIt->second
                                         : "require";
        if (!IsValidSslMode(host, sslmode, errorMessage))
        {
            LOG_SECURITY_WARN("[security] postgres_invalid_sslmode connection='{}' host='{}' sslmode='{}'",
                              connection.m_Name, host, sslmode);
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        std::string connStr = BuildConnectionString(connection, credentials);
        PGconn* conn = PQconnectdb(connStr.c_str());

        if (PQstatus(conn) != CONNECTION_OK)
        {
            errorMessage = "PostgreSQL connection failed: " + std::string(PQerrorMessage(conn));
            PQfinish(conn);
            return false;
        }

        // Run a trivial query to verify
        PGresult* res = PQexec(conn, "SELECT 1");
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            errorMessage = "PostgreSQL test query failed: " + std::string(PQresultErrorMessage(res));
            PQclear(res);
            PQfinish(conn);
            return false;
        }

        PQclear(res);
        PQfinish(conn);
        return true;
    }
} // namespace AIAssistant
