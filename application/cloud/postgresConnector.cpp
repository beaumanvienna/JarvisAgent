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
#include "keys/keyManager.h"

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

        auto const* provider = Core::g_Core->GetKeyManager().GetProvider(connection.m_KeyName);
        if (!provider)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }

        credentials.m_AuthType = CloudAuthType::BasicAuth;
        credentials.m_Username = provider->m_Username;
        credentials.m_Password = provider->m_Password;

        if (credentials.m_Username.empty())
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' has no username for PostgreSQL";
            return false;
        }

        return true;
    }

    std::string PostgresConnector::BuildConnectionString(CloudConnection const& connection,
                                                          CloudCredentials const& credentials)
    {
        // Parse host:port from endpoint
        std::string host = "localhost";
        std::string port = "5432";

        if (!connection.m_Endpoint.empty())
        {
            size_t colonPos = connection.m_Endpoint.rfind(':');
            if (colonPos != std::string::npos && colonPos > 0)
            {
                // Check if everything after the colon is digits (port)
                std::string afterColon = connection.m_Endpoint.substr(colonPos + 1);
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

        auto paramOrDefault = [&connection](std::string const& key, std::string const& defaultValue) -> std::string
        {
            auto it = connection.m_Params.find(key);
            return (it != connection.m_Params.end() && !it->second.empty()) ? it->second : defaultValue;
        };

        std::string database = paramOrDefault("database", "postgres");
        std::string sslmode = paramOrDefault("sslmode", "prefer");

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
