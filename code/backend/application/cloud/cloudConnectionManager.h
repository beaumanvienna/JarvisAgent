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

#include <atomic>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cloudConnector.h"

namespace AIAssistant
{
    // In-memory store for CloudConnection configs. Persisted to config.json via
    // the REST API save endpoint. Thread-safe with shared_mutex (same pattern as KeyManager).
    class CloudConnectionManager
    {
    public:
        CloudConnectionManager() = default;

        // CRUD (thread-safe)
        bool AddConnection(CloudConnection connection);
        bool UpdateConnection(std::string const& name, CloudConnection connection);
        bool RemoveConnection(std::string const& name);

        // Read access (thread-safe).  GetConnection returns by-value (std::optional)
        // rather than a raw pointer so the caller receives a copy that has no lifetime
        // dependency on the live map.  Pre-fix the raw pointer outlived the shared_lock
        // guard inside GetConnection — any concurrent writer (Add/Update/Remove/Parse)
        // could rehash the map or erase the entry while the caller still dereferenced
        // the pointer (use-after-free).  The copy is performed while the lock is held;
        // after return the optional owns its bytes regardless of subsequent map state.
        [[nodiscard]] std::optional<CloudConnection> GetConnection(std::string const& name) const;
        std::vector<std::string> GetConnectionNames() const;
        std::vector<CloudConnection> GetAllConnections() const;
        bool HasConnections() const;

        // Dirty flag: true when in-memory state differs from on-disk state.  Atomic so
        // that IsDirty / ClearDirty are lock-free and cannot race with writers that
        // set m_Dirty = true under m_Mutex (the atomic store from inside the lock is
        // a well-ordered write that the lock-free loaders observe).
        bool IsDirty() const { return m_Dirty.load(std::memory_order_acquire); }
        void ClearDirty() { m_Dirty.store(false, std::memory_order_release); }

        // Serialization for persistence
        bool ParseConnectionsJson(std::string const& json);
        std::string SerializeToJson() const;

    private:
        std::unordered_map<std::string, CloudConnection> m_Connections;
        mutable std::shared_mutex m_Mutex;
        std::atomic<bool> m_Dirty{false};
    };
} // namespace AIAssistant
