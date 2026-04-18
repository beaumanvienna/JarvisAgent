
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

#include <algorithm>

#include "core.h"
#include "engine.h"
#include "auxiliary/file.h"
#include "file/fileWatcher.h"
#include "event/events.h"

namespace AIAssistant
{
    FileWatcher::FileWatcher(const fs::path& pathToWatch, std::chrono::milliseconds interval)
        : m_PrimaryRoot(pathToWatch), m_Interval(interval)
    {
    }

    bool FileWatcher::AddPath(fs::path const& root)
    {
        if (root.empty()) return false;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return false;
        fs::path const canonical = fs::canonical(root, ec);
        if (ec) return false;

        std::scoped_lock<std::mutex> const lock(m_RootsMutex);
        if (canonical == fs::canonical(m_PrimaryRoot, ec)) return false;
        for (auto const& existing : m_AdditionalRoots)
        {
            if (fs::canonical(existing, ec) == canonical) return false;
        }
        m_AdditionalRoots.push_back(canonical);
        LOG_APP_INFO("[FileWatcher] +path '{}'", canonical.string());
        return true;
    }

    bool FileWatcher::RemovePath(fs::path const& root)
    {
        if (root.empty()) return false;
        std::error_code ec;
        // Use weakly_canonical so removal works even after the folder has been
        // deleted from disk. Callers are allowed to delete the folder and then
        // call RemovePath, though the documented order is the opposite.
        fs::path const canonical = fs::weakly_canonical(root, ec);
        if (ec) return false;

        std::scoped_lock<std::mutex> const lock(m_RootsMutex);
        auto const it = std::find_if(m_AdditionalRoots.begin(), m_AdditionalRoots.end(),
                                     [&](fs::path const& p) {
                                         std::error_code inner;
                                         return fs::weakly_canonical(p, inner) == canonical;
                                     });
        if (it == m_AdditionalRoots.end()) return false;
        m_AdditionalRoots.erase(it);
        m_PendingRemovals.push_back(canonical);
        LOG_APP_INFO("[FileWatcher] -path '{}'", canonical.string());
        return true;
    }

    std::vector<fs::path> FileWatcher::SnapshotAllRoots() const
    {
        std::scoped_lock<std::mutex> const lock(m_RootsMutex);
        std::vector<fs::path> roots;
        roots.reserve(1 + m_AdditionalRoots.size());
        roots.push_back(m_PrimaryRoot);
        for (auto const& r : m_AdditionalRoots) roots.push_back(r);
        return roots;
    }

    std::vector<fs::path> FileWatcher::DrainPendingRemovals()
    {
        std::scoped_lock<std::mutex> const lock(m_RootsMutex);
        std::vector<fs::path> drained;
        drained.swap(m_PendingRemovals);
        return drained;
    }

    FileWatcher::~FileWatcher() { Stop(); }

    void FileWatcher::Start()
    {
        if (m_Running)
        {
            return;
        }

        m_Running = true;
        // Submit watcher to the thread pool
        m_WatchTask = Core::g_Core->GetThreadPool().SubmitTask([this]() { Watch(); });
    }

    bool FileWatcher::IsValidFile(fs::directory_entry const& entry)
    {
        if (!fs::is_regular_file(entry))
        {
            return false;
        }

        // exclude files that start with a dot
        // geany does that for temp files in the current folder
        auto filename = entry.path().filename().string();
        return !(!filename.empty() && filename[0] == '.');
    }

    void FileWatcher::Stop()
    {
        SignalStop();
        WaitStop();
    }

    void FileWatcher::SignalStop()
    {
        if (!m_Running)
        {
            return;
        }

        m_Running = false;
        m_StopCV.notify_one();
    }

    void FileWatcher::WaitStop()
    {
        if (m_WatchTask.valid())
        {
            m_WatchTask.wait();
#ifndef NDEBUG
            LOG_APP_INFO("File watcher stopped");
#endif
        }
    }

    void FileWatcher::Watch()
    {
        std::unordered_map<std::string, fs::file_time_type> files;

        auto const scanRoot = [&](fs::path const& root, bool fireAdd) {
            std::error_code existsEc;
            if (!fs::is_directory(root, existsEc)) return;
            try
            {
                std::error_code iterEc;
                for (auto it = fs::recursive_directory_iterator(
                         root, fs::directory_options::skip_permission_denied, iterEc);
                     it != fs::recursive_directory_iterator(); it.increment(iterEc))
                {
                    if (iterEc) { iterEc.clear(); continue; }
                    auto const& file = *it;
                    if (!IsValidFile(file)) continue;
                    fs::file_time_type const currentTime = fs::last_write_time(file, iterEc);
                    if (iterEc) { iterEc.clear(); continue; }
                    std::string const pathStr = file.path().string();
                    if (!files.contains(pathStr))
                    {
                        if (fireAdd)
                        {
                            Core::g_Core->PushEvent(std::make_shared<FileAddedEvent>(pathStr));
                        }
                        files[pathStr] = currentTime;
                    }
                    else if (files[pathStr] != currentTime)
                    {
                        Core::g_Core->PushEvent(std::make_shared<FileModifiedEvent>(pathStr));
                        files[pathStr] = currentTime;
                    }
                }
            }
            catch (fs::filesystem_error const&)
            {
                // Directory was modified (e.g. clean / reaper) while iterating —
                // safe to retry next cycle.
            }
        };

        // --- Initial scan of the primary root ---
        // Fires FileAddedEvent for pre-existing files so the file-drop queue is
        // processed even if they were dropped while j9t was stopped.
        scanRoot(m_PrimaryRoot, /*fireAdd=*/true);

        while (m_Running)
        {
            {
                std::unique_lock<std::mutex> lock(m_StopMutex);
                m_StopCV.wait_for(lock, m_Interval, [this] { return !m_Running.load(); });
                if (!m_Running) break;
            }

            // 1) Prune entries under any roots that were just unregistered.
            //    We intentionally do NOT fire FileRemovedEvent for those paths —
            //    the caller (e.g. AdhocWorkflowManager) is managing that lifecycle.
            auto const removedRoots = DrainPendingRemovals();
            for (auto const& removedRoot : removedRoots)
            {
                std::error_code canonEc;
                fs::path const canonRoot = fs::weakly_canonical(removedRoot, canonEc);
                std::string const canonRootStr = canonRoot.string();
                for (auto it = files.begin(); it != files.end();)
                {
                    if (it->first.rfind(canonRootStr, 0) == 0)
                    {
                        it = files.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            // 2) Primary-root existence check — if it's gone, shut down the
            //    engine. Additional roots disappearing is fine (runs get reaped).
            if (!fs::is_directory(m_PrimaryRoot))
            {
                LOG_APP_INFO("folder '{}' no longer exists, requesting shutdown", m_PrimaryRoot.string());
                auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                Core::g_Core->PushEvent(event);
            }

            // 3) Scan every registered root.
            auto const roots = SnapshotAllRoots();
            for (auto const& root : roots)
            {
                scanRoot(root, /*fireAdd=*/true);
            }

            // 4) Detect removed files across all roots.
            for (auto it = files.begin(); it != files.end();)
            {
                if (!fs::exists(it->first))
                {
                    LOG_APP_INFO("FileWatcher: file deleted from disk: {}", it->first);
                    Core::g_Core->PushEvent(std::make_shared<FileRemovedEvent>(it->first));
                    it = files.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
} // namespace AIAssistant
