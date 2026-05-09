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
#include "json/jsonHelper.h"

namespace AIAssistant
{
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

    std::optional<CloudConnection> CloudConnectionManager::GetConnection(std::string const& name) const
    {
        // Value-copy under shared_lock: the returned optional owns its bytes after this
        // function exits, so a concurrent writer that subsequently rehashes or erases
        // m_Connections cannot invalidate the caller's view.  This is the C++ idiom for
        // Rust's Option<&T> with the borrow checker enforcing that no reference outlives
        // the lock guard.  See header comment for the dangling-pointer hazard
        // this guards against.
        std::shared_lock lock(m_Mutex);
        auto const it = m_Connections.find(name);
        if (it == m_Connections.end())
        {
            return std::nullopt;
        }
        return it->second;
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
        // Input size cap: bounds the total memory the parse can allocate.  Hostile or
        // corrupt input that exceeds this is rejected before simdjson sees it.  The
        // padded_string + on-demand iteration each multiply the input size, so an
        // unbounded read here is the seed for a multi-megabyte allocation chain.
        static constexpr size_t kMaxConnectionsJsonBytes = 1 * 1024 * 1024;
        // Per-array, per-field, per-params caps: each individual element is bounded so
        // a single oversized field cannot push memory pressure past the input cap by a
        // large multiplicative factor (string copies into CloudConnection).
        static constexpr size_t kMaxConnections = 1024;
        static constexpr size_t kMaxFieldBytes = 4096;
        static constexpr size_t kMaxParamsPerConnection = 256;
        static constexpr size_t kMaxParamFieldBytes = 1024;

        if (json.size() > kMaxConnectionsJsonBytes)
        {
            LOG_CORE_ERROR("CloudConnectionManager::ParseConnectionsJson: input size {} bytes exceeds {} byte cap; "
                           "rejecting and leaving in-memory connections untouched",
                           json.size(), kMaxConnectionsJsonBytes);
            return false;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(json);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            LOG_CORE_ERROR("CloudConnectionManager::ParseConnectionsJson: parse error: {}; "
                           "leaving in-memory connections untouched",
                           simdjson::error_message(error));
            return false;
        }

        simdjson::ondemand::array connections;
        if (doc["connections"].get_array().get(connections) != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("CloudConnectionManager::ParseConnectionsJson: 'connections' array not found; "
                           "leaving in-memory connections untouched");
            return false;
        }

        // Parse into a local scratch map.  m_Connections is only replaced at the end on
        // full-parse success.  Failure mid-parse leaves the live state untouched (the
        // pre-fix behaviour wiped m_Connections at function entry, so any malformed
        // payload erased all connections — fail-open).
        std::unordered_map<std::string, CloudConnection> staging;

        size_t elementIndex = 0;
        for (auto element : connections)
        {
            size_t const idx = elementIndex++;

            if (staging.size() >= kMaxConnections)
            {
                LOG_CORE_ERROR("CloudConnectionManager::ParseConnectionsJson: connection count exceeds {}; "
                               "rejecting at element index {}; leaving in-memory connections untouched",
                               kMaxConnections, idx);
                return false;
            }

            simdjson::ondemand::object obj;
            if (element.get_object().get(obj) != simdjson::SUCCESS)
            {
                LOG_CORE_WARN("CloudConnectionManager::ParseConnectionsJson: skipping malformed element at index {}",
                              idx);
                continue;
            }

            CloudConnection conn;
            std::string_view sv;
            bool elementFieldTooLong = false;

            auto readBoundedField = [&](char const* fieldName, std::string& dst) {
                if (obj[fieldName].get_string().get(sv) == simdjson::SUCCESS)
                {
                    if (sv.size() > kMaxFieldBytes)
                    {
                        LOG_CORE_WARN("CloudConnectionManager::ParseConnectionsJson: skipping element at index {}: "
                                      "field '{}' size {} exceeds {} byte cap",
                                      idx, fieldName, sv.size(), kMaxFieldBytes);
                        elementFieldTooLong = true;
                        return;
                    }
                    dst.assign(sv);
                }
            };

            readBoundedField("name", conn.m_Name);
            if (elementFieldTooLong) continue;
            readBoundedField("type", conn.m_Type);
            if (elementFieldTooLong) continue;
            readBoundedField("endpoint", conn.m_Endpoint);
            if (elementFieldTooLong) continue;
            readBoundedField("key_name", conn.m_KeyName);
            if (elementFieldTooLong) continue;

            if (obj["auth_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                if (sv.size() > kMaxFieldBytes)
                {
                    LOG_CORE_WARN("CloudConnectionManager::ParseConnectionsJson: skipping element at index {}: "
                                  "field 'auth_type' size {} exceeds {} byte cap",
                                  idx, sv.size(), kMaxFieldBytes);
                    continue;
                }
                conn.m_AuthType = StringToAuthType(sv);
            }

            // Parse type-specific params with both per-entry-count and per-field-length bounds.
            simdjson::ondemand::object params;
            if (obj["params"].get_object().get(params) == simdjson::SUCCESS)
            {
                bool paramsOverflow = false;
                for (auto field : params)
                {
                    if (conn.m_Params.size() >= kMaxParamsPerConnection)
                    {
                        LOG_CORE_WARN("CloudConnectionManager::ParseConnectionsJson: skipping element at index {}: "
                                      "params count exceeds {}",
                                      idx, kMaxParamsPerConnection);
                        paramsOverflow = true;
                        break;
                    }
                    std::string_view key = field.unescaped_key();
                    std::string_view val;
                    if (field.value().get_string().get(val) != simdjson::SUCCESS)
                    {
                        continue;
                    }
                    if (key.size() > kMaxParamFieldBytes || val.size() > kMaxParamFieldBytes)
                    {
                        LOG_CORE_WARN("CloudConnectionManager::ParseConnectionsJson: skipping params entry at "
                                      "element index {}: key/value size exceeds {} byte cap",
                                      idx, kMaxParamFieldBytes);
                        continue;
                    }
                    conn.m_Params[std::string(key)] = std::string(val);
                }
                if (paramsOverflow)
                {
                    continue;
                }
            }

            if (conn.m_Name.empty())
            {
                LOG_CORE_WARN("CloudConnectionManager::ParseConnectionsJson: skipping element at index {}: "
                              "missing or empty 'name'",
                              idx);
                continue;
            }
            staging[conn.m_Name] = std::move(conn);
        }

        // Atomic swap: only here, after the entire parse has succeeded, do we replace
        // the live state.  Any earlier return left m_Connections untouched.
        std::unique_lock lock(m_Mutex);
        m_Connections = std::move(staging);
        m_Dirty = true;
        return true;
    }

    std::string CloudConnectionManager::SerializeToJson() const
    {
        // Hold shared_lock across the entire iteration: rehash from a concurrent writer
        // (AddConnection / RemoveConnection / ParseConnectionsJson) would invalidate the
        // range-for iterator and produce garbled JSON or a crash.
        std::shared_lock lock(m_Mutex);

        std::ostringstream oss;
        oss << "{\n";
        oss << "    \"connections\": [\n";

        size_t i = 0;
        for (auto const& [name, conn] : m_Connections)
        {
            oss << "        {\n";
            oss << "            \"name\": \"" << JsonHelper::EscapeJsonString(conn.m_Name) << "\",\n";
            oss << "            \"type\": \"" << JsonHelper::EscapeJsonString(conn.m_Type) << "\",\n";
            oss << "            \"endpoint\": \"" << JsonHelper::EscapeJsonString(conn.m_Endpoint) << "\",\n";
            oss << "            \"key_name\": \"" << JsonHelper::EscapeJsonString(conn.m_KeyName) << "\",\n";
            oss << "            \"auth_type\": \"" << AuthTypeToString(conn.m_AuthType) << "\"";

            if (!conn.m_Params.empty())
            {
                oss << ",\n            \"params\": {\n";
                size_t j = 0;
                for (auto const& [key, val] : conn.m_Params)
                {
                    oss << "                \"" << JsonHelper::EscapeJsonString(key) << "\": \""
                        << JsonHelper::EscapeJsonString(val) << "\"";
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
