
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
        if (!m_Running)
        {
            return;
        }

        m_Running = false;
        if (m_WatchTask.valid())
        {
            m_WatchTask.wait(); // wait for graceful exit
            LOG_APP_INFO("File watcher stopped");
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
            std::this_thread::sleep_for(m_Interval);

            if (!EngineCore::FileExists(m_PathToWatch))
            {
                LOG_APP_INFO("folder '{}' no longer exists, requesting shutdown", m_PathToWatch.string());
                auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                Core::g_Core->PushEvent(event);
            }

            // Detect added or modified files
            for (auto& file : fs::recursive_directory_iterator(m_PathToWatch))
            {
                if (!IsValidFile(file))
                {
                    continue;
                }

                fs::file_time_type const currentTime = fs::last_write_time(file);
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
