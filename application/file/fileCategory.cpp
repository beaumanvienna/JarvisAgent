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

#include "file/fileCategory.h"
#include "auxiliary/file.h"
#include <algorithm>
#include <iostream>

namespace AIAssistant
{
    FileCategorizer::~FileCategorizer() {}

    void FileCategorizer::AddFile(fs::path const& filePath)
    {
        FileCategory category = Categorize(filePath);
        auto& categoryMap = [&]() -> TrackedFiles&
        {
            switch (category)
            {
                case FileCategory::Settings:
                    return m_CategorizedFiles.m_Settings;
                case FileCategory::Context:
                    return m_CategorizedFiles.m_Context;
                case FileCategory::Task:
                    return m_CategorizedFiles.m_Tasks;
                case FileCategory::Requirement:
                    return m_CategorizedFiles.m_Requirements;
                case FileCategory::SubFolder:
                    return m_CategorizedFiles.m_Subfolders;
                default:
                    return m_CategorizedFiles.m_Requirements; // fallback
            }
        }();

        std::string key = filePath.string();
        categoryMap[key] = std::make_unique<TrackedFile>(filePath); // constructs and marks modified = true
    }

    void FileCategorizer::RemoveFile(fs::path const& filePath)
    {
        RemoveFromFiles(m_CategorizedFiles.m_Settings, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Context, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Tasks, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Requirements, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Subfolders, filePath);
    }

    void FileCategorizer::ModifyFile(fs::path const& filePath)
    {
        FileCategory category = Categorize(filePath);
        auto& categoryMap = [&]() -> TrackedFiles&
        {
            switch (category)
            {
                case FileCategory::Settings:
                    return m_CategorizedFiles.m_Settings;
                case FileCategory::Context:
                    return m_CategorizedFiles.m_Context;
                case FileCategory::Task:
                    return m_CategorizedFiles.m_Tasks;
                case FileCategory::Requirement:
                    return m_CategorizedFiles.m_Requirements;
                case FileCategory::SubFolder:
                    return m_CategorizedFiles.m_Subfolders;
                default:
                    return m_CategorizedFiles.m_Context; // fallback
            }
        }();

        auto it = categoryMap.find(filePath.string());
        if (it != categoryMap.end())
        {
            if (it->second->CheckIfContentChanged())
            {
                it->second->MarkModified();
                LOG_APP_INFO("FileCategorizer::ModifyFile: Modified file: {}", filePath.string());
            }
        }
        else
        {
            LOG_APP_CRITICAL("File not tracked yet (could be newly added)");
        }
    }

    void FileCategorizer::RemoveFromFiles(TrackedFiles& files, fs::path const& path)
    {
        auto it = files.find(path.string());
        if (it != files.end())
        {
            files.erase(it);
            LOG_APP_INFO("Removed file: {}", path.string());
        }
    }

    FileCategory FileCategorizer::Categorize(fs::path const& filePath) const
    {
        std::string filename = filePath.filename().string();

        if (EngineCore::IsDirectory(filePath))
        {
            return FileCategory::SubFolder;
        }

        if (filename.starts_with("STNG"))
        {
            return FileCategory::Settings;
        }

        if (filename.starts_with("CNXT"))
        {
            return FileCategory::Context;
        }

        if (filename.starts_with("TASK"))
        {
            return FileCategory::Task;
        }

        // anything else is considered a requirement
        return FileCategory::Requirement;
    }

    void FileCategorizer::PrintCategorizedFiles() const
    {
        auto printCategory = [](std::string const& category, TrackedFiles const& files)
        {
            if (!files.empty())
            {
                std::cout << category << ":\n";
                for (auto const& file : files)
                {
                    std::cout << "  " << file.second->GetPath().string() << "\n";
                }
            }
        };

        std::cout << "=== FileCategorizer: Tracked Files ===\n";
        printCategory("Settings", m_CategorizedFiles.m_Settings);
        printCategory("Context", m_CategorizedFiles.m_Context);
        printCategory("Tasks", m_CategorizedFiles.m_Tasks);
        printCategory("Requirements", m_CategorizedFiles.m_Requirements);
        printCategory("Subfolders", m_CategorizedFiles.m_Subfolders);
        std::cout << "=== End of Tracked Files ===\n";
    }
} // namespace AIAssistant
