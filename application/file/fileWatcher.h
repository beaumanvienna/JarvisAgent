/* Copyright (c) 2025 JC Technolabs

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <filesystem>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include "event/eventQueue.h"
#include "event/filesystemEvent.h"
#include "core.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    class FileWatcher
    {
    public:
        explicit FileWatcher(const fs::path& pathToWatch, std::chrono::milliseconds interval = 100ms);
        ~FileWatcher();

        void Start();
        void Stop();
        void SignalStop();
        void WaitStop();

        // Dynamic watch set — register an additional root to observe. Thread-safe,
        // idempotent on duplicates. Returns false if the path is empty, does not
        // exist, or duplicates the primary / an already-added root. The new root
        // becomes observable on the next watch tick (<= m_Interval).
        bool AddPath(fs::path const& root);

        // Unregister a root previously added via AddPath. Thread-safe. Does NOT
        // fire FileRemovedEvent for files under the removed root — the caller is
        // responsible for any cleanup signaling. The primary root (ctor argument)
        // cannot be removed.
        bool RemovePath(fs::path const& root);

    private:
        void Watch();
        bool IsValidFile(fs::directory_entry const& entry);
        std::vector<fs::path> SnapshotAllRoots() const;
        std::vector<fs::path> DrainPendingRemovals();

        fs::path m_PrimaryRoot;
        std::chrono::milliseconds m_Interval;
        std::atomic<bool> m_Running{false};
        std::future<void> m_WatchTask;

        std::mutex m_StopMutex;
        std::condition_variable m_StopCV;

        // Additional roots registered at runtime. The primary root is NOT stored
        // here; it always watches for the watcher's lifetime. Removals are
        // deferred to the watch thread via m_PendingRemovals so files under the
        // dropped root can be pruned from the tracked-file map without firing
        // spurious FileRemovedEvent.
        mutable std::mutex m_RootsMutex;
        std::vector<fs::path> m_AdditionalRoots;
        std::vector<fs::path> m_PendingRemovals;
    };
} // namespace AIAssistant
