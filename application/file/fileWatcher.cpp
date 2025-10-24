
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

#include "file/fileWatcher.h"

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
            return;

        m_Running = true;
        m_WatcherThread = std::thread([this]() { Watch(); });
    }

    void FileWatcher::Stop()
    {
        m_Running = false;
        if (m_WatcherThread.joinable())
            m_WatcherThread.join();
    }

    void FileWatcher::Watch()
    {
        std::unordered_map<std::string, fs::file_time_type> files;

        // Initial snapshot
        for (auto& file : fs::recursive_directory_iterator(m_PathToWatch))
        {
            if (!fs::is_regular_file(file))
                continue;
            files[file.path().string()] = fs::last_write_time(file);
        }

        while (m_Running)
        {
            std::this_thread::sleep_for(m_Interval);

            // Detect added or modified
            for (auto& file : fs::recursive_directory_iterator(m_PathToWatch))
            {
                if (!fs::is_regular_file(file))
                    continue;

                const auto currentTime = fs::last_write_time(file);
                const auto pathStr = file.path().string();

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

            // Detect removed
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
