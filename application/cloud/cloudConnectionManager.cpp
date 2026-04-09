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

#include <sstream>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "cloud/cloudConnectionManager.h"

namespace AIAssistant
{
    // Escape a string for safe JSON output (handles quotes, backslashes, control chars).
    static std::string JsonEscape(std::string const& s)
    {
        std::string result;
        result.reserve(s.size());
        for (char c : s)
        {
            switch (c)
            {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }

    bool CloudConnectionManager::AddConnection(CloudConnection connection)
    {
        std::unique_lock lock(m_Mutex);
        if (connection.m_Name.empty())
        {
            LOG_CORE_WARN("CloudConnectionManager::AddConnection: empty name");
            return false;
        }
        if (m_Connections.count(connection.m_Name))
        {
            LOG_CORE_WARN("CloudConnectionManager::AddConnection: '{}' already exists", connection.m_Name);
            return false;
        }
        std::string name = connection.m_Name;
        m_Connections[name] = std::move(connection);
        m_Dirty = true;
        return true;
    }

    bool CloudConnectionManager::UpdateConnection(std::string const& name, CloudConnection connection)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Connections.find(name);
        if (it == m_Connections.end())
        {
            LOG_CORE_WARN("CloudConnectionManager::UpdateConnection: '{}' not found", name);
            return false;
        }
        connection.m_Name = name; // Ensure name consistency
        it->second = std::move(connection);
        m_Dirty = true;
        return true;
    }

    bool CloudConnectionManager::RemoveConnection(std::string const& name)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Connections.find(name);
        if (it == m_Connections.end())
        {
            LOG_CORE_WARN("CloudConnectionManager::RemoveConnection: '{}' not found", name);
            return false;
        }
        m_Connections.erase(it);
        m_Dirty = true;
        return true;
    }

    CloudConnection const* CloudConnectionManager::GetConnection(std::string const& name) const
    {
        std::shared_lock lock(m_Mutex);
        auto const it = m_Connections.find(name);
        if (it == m_Connections.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    std::vector<std::string> CloudConnectionManager::GetConnectionNames() const
    {
        std::shared_lock lock(m_Mutex);
        std::vector<std::string> names;
        names.reserve(m_Connections.size());
        for (auto const& [name, conn] : m_Connections)
        {
            names.push_back(name);
        }
        return names;
    }

    std::vector<CloudConnection> CloudConnectionManager::GetAllConnections() const
    {
        std::shared_lock lock(m_Mutex);
        std::vector<CloudConnection> connections;
        connections.reserve(m_Connections.size());
        for (auto const& [name, conn] : m_Connections)
        {
            connections.push_back(conn);
        }
        return connections;
    }

    bool CloudConnectionManager::HasConnections() const
    {
        std::shared_lock lock(m_Mutex);
        return !m_Connections.empty();
    }

    bool CloudConnectionManager::ParseConnectionsJson(std::string const& json)
    {
        std::unique_lock lock(m_Mutex);
        m_Connections.clear();

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(json);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            LOG_CORE_ERROR("CloudConnectionManager::ParseConnectionsJson: parse error: {}",
                           simdjson::error_message(error));
            return false;
        }

        simdjson::ondemand::array connections;
        if (doc["connections"].get_array().get(connections) != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("CloudConnectionManager::ParseConnectionsJson: 'connections' array not found");
            return false;
        }

        for (auto element : connections)
        {
            simdjson::ondemand::object obj;
            if (element.get_object().get(obj) != simdjson::SUCCESS)
            {
                continue;
            }

            CloudConnection conn;
            std::string_view sv;

            if (obj["name"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_Name = std::string(sv);
            }
            if (obj["type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_Type = std::string(sv);
            }
            if (obj["endpoint"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_Endpoint = std::string(sv);
            }
            if (obj["key_name"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_KeyName = std::string(sv);
            }
            if (obj["auth_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                conn.m_AuthType = StringToAuthType(sv);
            }

            // Parse type-specific params
            simdjson::ondemand::object params;
            if (obj["params"].get_object().get(params) == simdjson::SUCCESS)
            {
                for (auto field : params)
                {
                    std::string_view key = field.unescaped_key();
                    std::string_view val;
                    if (field.value().get_string().get(val) == simdjson::SUCCESS)
                    {
                        conn.m_Params[std::string(key)] = std::string(val);
                    }
                }
            }

            if (!conn.m_Name.empty())
            {
                m_Connections[conn.m_Name] = std::move(conn);
            }
        }

        return true;
    }

    std::string CloudConnectionManager::SerializeToJson() const
    {
        std::ostringstream oss;
        oss << "{\n";
        oss << "    \"connections\": [\n";

        size_t i = 0;
        for (auto const& [name, conn] : m_Connections)
        {
            oss << "        {\n";
            oss << "            \"name\": \"" << JsonEscape(conn.m_Name) << "\",\n";
            oss << "            \"type\": \"" << JsonEscape(conn.m_Type) << "\",\n";
            oss << "            \"endpoint\": \"" << JsonEscape(conn.m_Endpoint) << "\",\n";
            oss << "            \"key_name\": \"" << JsonEscape(conn.m_KeyName) << "\",\n";
            oss << "            \"auth_type\": \"" << AuthTypeToString(conn.m_AuthType) << "\"";

            if (!conn.m_Params.empty())
            {
                oss << ",\n            \"params\": {\n";
                size_t j = 0;
                for (auto const& [key, val] : conn.m_Params)
                {
                    oss << "                \"" << JsonEscape(key) << "\": \"" << JsonEscape(val) << "\"";
                    if (++j < conn.m_Params.size())
                    {
                        oss << ",";
                    }
                    oss << "\n";
                }
                oss << "            }\n";
            }
            else
            {
                oss << "\n";
            }

            oss << "        }";
            if (++i < m_Connections.size())
            {
                oss << ",";
            }
            oss << "\n";
        }

        oss << "    ]\n";
        oss << "}\n";
        return oss.str();
    }
} // namespace AIAssistant
