
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

#include "core.h"
#include "engine.h"
#include "auxiliary/file.h"
#include "file/fileWatcher.h"
#include "event/events.h"

namespace AIAssistant
{
    FileWatcher::FileWatcher(const fs::path& pathToWatch, std::chrono::milliseconds interval)
        : m_PathToWatch(pathToWatch), m_Interval(interval)
    {
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

        // --- Initial scan ---
        for (auto& file : fs::recursive_directory_iterator(m_PathToWatch))
        {
            if (!IsValidFile(file))
            {
                continue;
            }

            std::string const pathStr = file.path().string();
            files[pathStr] = fs::last_write_time(file);

            // fire event for existing files at startup
            Core::g_Core->PushEvent(std::make_shared<FileAddedEvent>(pathStr));
        }

        while (m_Running)
        {
            {
                std::unique_lock<std::mutex> lock(m_StopMutex);
                m_StopCV.wait_for(lock, m_Interval, [this] { return !m_Running.load(); });
                if (!m_Running)
                {
                    break;
                }
            }

            if (!fs::is_directory(m_PathToWatch))
            {
                LOG_APP_INFO("folder '{}' no longer exists, requesting shutdown", m_PathToWatch.string());
                auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                Core::g_Core->PushEvent(event);
            }

            // Detect added or modified files
            try
            {
                std::error_code iterEc;
                for (auto it = fs::recursive_directory_iterator(m_PathToWatch, fs::directory_options::skip_permission_denied,
                                                                iterEc);
                     it != fs::recursive_directory_iterator(); it.increment(iterEc))
                {
                    if (iterEc)
                    {
                        iterEc.clear();
                        continue;
                    }

                    auto const& file = *it;
                    if (!IsValidFile(file))
                    {
                        continue;
                    }

                    fs::file_time_type const currentTime = fs::last_write_time(file, iterEc);
                    if (iterEc)
                    {
                        iterEc.clear();
                        continue;
                    }

                    std::string const pathStr = file.path().string();

                    if (!files.contains(pathStr))
                    {
                        Core::g_Core->PushEvent(std::make_shared<FileAddedEvent>(pathStr));
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
                // Directory was modified (e.g. clean) while iterating — safe to retry next cycle.
            }

            // Detect removed files
            for (auto it = files.begin(); it != files.end();)
            {
                if (!fs::exists(it->first))
                {
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
