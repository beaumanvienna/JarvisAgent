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

#include "cloud/cloudConnectionPool.h"
#include "engine.h"

namespace AIAssistant
{
    CloudConnectionPool::CloudConnectionPool(Config config)
        : m_Config(std::move(config))
    {
    }

    CloudConnectionPool::~CloudConnectionPool()
    {
        DrainAll();
    }

    void CloudConnectionPool::RegisterType(std::string const& type, DestroyFn destroyFn,
                                           HealthCheckFn healthCheckFn)
    {
        std::lock_guard lock(m_Mutex);
        m_TypeCallbacks[type] = {std::move(destroyFn), std::move(healthCheckFn)};
    }

    CloudConnectionPool::PooledHandle CloudConnectionPool::Acquire(std::string const& connectionName,
                                                                    std::string const& type,
                                                                    CreateFn const& createFn,
                                                                    std::string& errorMessage)
    {
        std::unique_lock lock(m_Mutex);

        auto& idle = m_Pool[connectionName];
        auto& active = m_ActiveCount[connectionName];

        auto callbacksIt = m_TypeCallbacks.find(type);

        // Try to reuse an idle connection
        while (!idle.empty())
        {
            PooledHandle handle = std::move(idle.front());
            idle.pop_front();

            // Check if stale
            auto idleTime = std::chrono::steady_clock::now() - handle.m_LastUsed;
            if (idleTime > m_Config.m_MaxIdleTime)
            {
                // Evict stale connection
                if (callbacksIt != m_TypeCallbacks.end() && callbacksIt->second.m_Destroy && handle.m_Connection)
                {
                    callbacksIt->second.m_Destroy(handle.m_Connection);
                }
                LOG_APP_INFO("[connection-pool] evicted stale connection for '{}'", connectionName);
                continue;
            }

            // Health check
            if (callbacksIt != m_TypeCallbacks.end() && callbacksIt->second.m_HealthCheck)
            {
                if (!callbacksIt->second.m_HealthCheck(handle.m_Connection))
                {
                    // Connection is dead, destroy and try next
                    if (callbacksIt->second.m_Destroy && handle.m_Connection)
                    {
                        callbacksIt->second.m_Destroy(handle.m_Connection);
                    }
                    LOG_APP_INFO("[connection-pool] evicted unhealthy connection for '{}'", connectionName);
                    continue;
                }
            }

            ++active;
            handle.m_LastUsed = std::chrono::steady_clock::now();
            return handle;
        }

        // No idle connections — check if we can create a new one
        int total = active + static_cast<int>(idle.size());
        if (total >= m_Config.m_MaxConnectionsPerName)
        {
            // Wait for a release
            bool available = m_Cv.wait_for(lock, m_Config.m_AcquireTimeout, [&]()
            {
                return !m_Pool[connectionName].empty() || m_ActiveCount[connectionName] < m_Config.m_MaxConnectionsPerName;
            });

            if (!available)
            {
                errorMessage = "Connection pool exhausted for '" + connectionName + "' (timeout)";
                LOG_APP_WARN("[connection-pool] acquire timeout for '{}'", connectionName);
                return {};
            }

            // Retry — a connection may have been released
            if (!m_Pool[connectionName].empty())
            {
                PooledHandle handle = std::move(m_Pool[connectionName].front());
                m_Pool[connectionName].pop_front();
                ++active;
                handle.m_LastUsed = std::chrono::steady_clock::now();
                return handle;
            }
        }

        // Create a new connection
        lock.unlock();
        void* conn = createFn(errorMessage);
        lock.lock();

        if (!conn)
        {
            if (errorMessage.empty())
            {
                errorMessage = "Failed to create connection for '" + connectionName + "'";
            }
            return {};
        }

        ++m_ActiveCount[connectionName];

        PooledHandle handle;
        handle.m_Connection = conn;
        handle.m_LastUsed = std::chrono::steady_clock::now();
        handle.m_Valid = true;
        return handle;
    }

    void CloudConnectionPool::Release(std::string const& connectionName, std::string const& type,
                                      PooledHandle handle)
    {
        std::lock_guard lock(m_Mutex);

        auto& active = m_ActiveCount[connectionName];
        if (active > 0)
        {
            --active;
        }

        if (handle.m_Valid && handle.m_Connection)
        {
            handle.m_LastUsed = std::chrono::steady_clock::now();
            m_Pool[connectionName].push_back(std::move(handle));
        }

        m_Cv.notify_one();
    }

    void CloudConnectionPool::EvictStale()
    {
        std::lock_guard lock(m_Mutex);
        auto now = std::chrono::steady_clock::now();

        for (auto& [name, idle] : m_Pool)
        {
            auto it = idle.begin();
            while (it != idle.end())
            {
                if (now - it->m_LastUsed > m_Config.m_MaxIdleTime)
                {
                    // Find appropriate destroy callback
                    for (auto const& [typeName, callbacks] : m_TypeCallbacks)
                    {
                        if (callbacks.m_Destroy && it->m_Connection)
                        {
                            callbacks.m_Destroy(it->m_Connection);
                            break;
                        }
                    }
                    it = idle.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }

    void CloudConnectionPool::Drain(std::string const& connectionName)
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Pool.find(connectionName);
        if (it == m_Pool.end())
        {
            return;
        }

        for (auto& handle : it->second)
        {
            for (auto const& [typeName, callbacks] : m_TypeCallbacks)
            {
                if (callbacks.m_Destroy && handle.m_Connection)
                {
                    callbacks.m_Destroy(handle.m_Connection);
                    break;
                }
            }
        }
        it->second.clear();
        LOG_APP_INFO("[connection-pool] drained all idle connections for '{}'", connectionName);
    }

    void CloudConnectionPool::DrainAll()
    {
        std::lock_guard lock(m_Mutex);
        for (auto& [name, idle] : m_Pool)
        {
            for (auto& handle : idle)
            {
                for (auto const& [typeName, callbacks] : m_TypeCallbacks)
                {
                    if (callbacks.m_Destroy && handle.m_Connection)
                    {
                        callbacks.m_Destroy(handle.m_Connection);
                        break;
                    }
                }
            }
            idle.clear();
        }
        LOG_APP_INFO("[connection-pool] drained all connections");
    }
} // namespace AIAssistant
