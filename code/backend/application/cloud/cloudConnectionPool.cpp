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
        // Single loop with explicit re-evaluation on each iteration.  The prior
        // implementation held a long-lived `auto&` reference to the deque and
        // active-count map entries across the cv `wait_for` AND across the
        // `lock.unlock() / createFn(...) / lock.lock()` window.  Both windows
        // permit other threads to mutate the unordered_maps (RegisterType /
        // concurrent Acquire / Release), which can rehash and invalidate
        // those references plus the `callbacksIt` iterator.  Even when the
        // map's bucket layout was stable, the fall-through to `++m_ActiveCount`
        // after an unlocked createFn meant two concurrent waiters could both
        // create a connection and push the active count past
        // `m_MaxConnectionsPerName` — every assumption the cv predicate
        // relies on requires the map state to be re-read after each lock
        // re-acquisition.
        //
        // The loop below re-resolves the type callbacks, the deque, and the
        // active count each iteration, and reserves the active-count slot
        // BEFORE unlocking for `createFn` so a concurrent waiter sees the
        // slot taken and blocks on the cv (no over-create race).  Slot
        // rollback on createFn failure is paired with `notify_one` so any
        // waiter that would now succeed wakes up immediately.
        std::unique_lock lock(m_Mutex);
        auto const deadline = std::chrono::steady_clock::now() + m_Config.m_AcquireTimeout;

        for (;;)
        {
            // Re-resolve the type callbacks every iteration: `RegisterType`
            // could fire between the `wait_for` waking and us re-acquiring
            // the lock.  Lookup is O(1) and dwarfed by the connection cost.
            auto callbacksIt = m_TypeCallbacks.find(type);

            // 1. Try to reuse a healthy idle connection.
            auto& idle = m_Pool[connectionName];
            while (!idle.empty())
            {
                PooledHandle handle = std::move(idle.front());
                idle.pop_front();

                auto idleTime = std::chrono::steady_clock::now() - handle.m_LastUsed;
                if (idleTime > m_Config.m_MaxIdleTime)
                {
                    if (callbacksIt != m_TypeCallbacks.end() && callbacksIt->second.m_Destroy && handle.m_Connection)
                    {
                        callbacksIt->second.m_Destroy(handle.m_Connection);
                    }
                    LOG_APP_INFO("[connection-pool] evicted stale connection for '{}'", connectionName);
                    continue;
                }

                if (callbacksIt != m_TypeCallbacks.end() && callbacksIt->second.m_HealthCheck)
                {
                    if (!callbacksIt->second.m_HealthCheck(handle.m_Connection))
                    {
                        if (callbacksIt->second.m_Destroy && handle.m_Connection)
                        {
                            callbacksIt->second.m_Destroy(handle.m_Connection);
                        }
                        LOG_APP_INFO("[connection-pool] evicted unhealthy connection for '{}'", connectionName);
                        continue;
                    }
                }

                ++m_ActiveCount[connectionName];
                handle.m_LastUsed = std::chrono::steady_clock::now();
                return handle;
            }

            // 2. Idle pool empty — check if we can create a new connection.
            int const totalNow =
                m_ActiveCount[connectionName] + static_cast<int>(m_Pool[connectionName].size());
            if (totalNow < m_Config.m_MaxConnectionsPerName)
            {
                // 3. Reserve the slot BEFORE unlocking.  This prevents a second
                //    waiter from racing into the create path during our
                //    unlocked window and over-creating past
                //    m_MaxConnectionsPerName.  On createFn failure we roll
                //    back the slot AND notify one waiter that the budget is
                //    available again.
                ++m_ActiveCount[connectionName];

                std::string createError;
                void* conn = nullptr;
                lock.unlock();
                try
                {
                    conn = createFn(createError);
                }
                catch (std::exception const& e)
                {
                    createError = "createFn threw: " + std::string(e.what());
                    conn = nullptr;
                }
                catch (...)
                {
                    createError = "createFn threw: unknown exception";
                    conn = nullptr;
                }
                lock.lock();

                if (!conn)
                {
                    // Roll back the slot reservation.  Re-resolve the entry
                    // by key — the map's internal layout may have changed
                    // while we were unlocked, so the prior `auto&` references
                    // would be stale.
                    if (auto countIt = m_ActiveCount.find(connectionName);
                        countIt != m_ActiveCount.end() && countIt->second > 0)
                    {
                        --countIt->second;
                    }
                    errorMessage = createError.empty()
                                       ? "Failed to create connection for '" + connectionName + "'"
                                       : createError;
                    m_Cv.notify_one();
                    return {};
                }

                PooledHandle handle;
                handle.m_Connection = conn;
                handle.m_LastUsed = std::chrono::steady_clock::now();
                handle.m_Valid = true;
                return handle;
            }

            // 4. Pool at capacity — wait for a release or a slot opening.
            //    `wait_until` against a single deadline is correct under
            //    spurious wake-ups: each wake re-runs the loop from the top,
            //    re-reading map state with the lock re-acquired.
            bool const becameAvailable = m_Cv.wait_until(lock, deadline, [&]()
            {
                return !m_Pool[connectionName].empty() ||
                       m_ActiveCount[connectionName] < m_Config.m_MaxConnectionsPerName;
            });
            if (!becameAvailable)
            {
                errorMessage = "Connection pool exhausted for '" + connectionName + "' (timeout)";
                LOG_APP_WARN("[connection-pool] acquire timeout for '{}'", connectionName);
                return {};
            }
            // Loop and retry — re-resolve idle pool, callbacks, total count.
        }
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
