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

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace AIAssistant
{
    // Generic connection pool for persistent-connection providers (PostgreSQL via libpq,
    // future IMAP keep-alive). HTTP-based connectors already benefit from libcurl connection
    // reuse (CURLOPT_TCP_KEEPALIVE), so this pool is for non-HTTP protocols.
    //
    // Usage pattern:
    //   auto handle = pool.Acquire("my-postgres", createFn);
    //   // use handle.m_Connection
    //   pool.Release("my-postgres", std::move(handle));
    //
    // Handles are validated on acquire (health check) and evicted if stale.
    class CloudConnectionPool
    {
    public:
        struct Config
        {
            int m_MaxConnectionsPerName{4};                           // Max pooled connections per connection name
            std::chrono::seconds m_MaxIdleTime{300};                  // Evict after 5 minutes idle
            std::chrono::seconds m_AcquireTimeout{10};                // Wait timeout when pool is full
        };

        // Opaque handle wrapping a pooled connection (e.g., PGconn*)
        struct PooledHandle
        {
            void* m_Connection{nullptr};
            std::chrono::steady_clock::time_point m_LastUsed{};
            bool m_Valid{false};

            explicit operator bool() const { return m_Valid && m_Connection != nullptr; }
        };

        // Callbacks for connection lifecycle
        using CreateFn = std::function<void*(std::string& errorMessage)>;
        using DestroyFn = std::function<void(void*)>;
        using HealthCheckFn = std::function<bool(void*)>;

        CloudConnectionPool() = default;
        explicit CloudConnectionPool(Config config);
        ~CloudConnectionPool();

        // Register lifecycle callbacks for a connection type (e.g., "postgres").
        void RegisterType(std::string const& type, DestroyFn destroyFn, HealthCheckFn healthCheckFn);

        // Acquire a connection handle. Creates a new connection via createFn if none available.
        // Blocks up to AcquireTimeout if the pool is full.
        PooledHandle Acquire(std::string const& connectionName, std::string const& type, CreateFn const& createFn,
                             std::string& errorMessage);

        // Return a connection handle to the pool.
        void Release(std::string const& connectionName, std::string const& type, PooledHandle handle);

        // Evict all idle connections that have exceeded MaxIdleTime.
        void EvictStale();

        // Drain all connections for a specific connection name (e.g., on connection config change).
        void Drain(std::string const& connectionName);

        // Drain all connections.
        void DrainAll();

    private:
        struct TypeCallbacks
        {
            DestroyFn m_Destroy;
            HealthCheckFn m_HealthCheck;
        };

        Config m_Config;
        std::mutex m_Mutex;
        std::condition_variable m_Cv;

        std::unordered_map<std::string, TypeCallbacks> m_TypeCallbacks;
        std::unordered_map<std::string, std::deque<PooledHandle>> m_Pool;   // connectionName -> idle handles
        std::unordered_map<std::string, int> m_ActiveCount;                 // connectionName -> in-use count
    };
} // namespace AIAssistant
