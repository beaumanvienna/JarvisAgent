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
        switch (category)
        {
            case FileCategory::Settings:
            {
                m_CategorizedFiles.m_Settings.push_back(filePath);
                break;
            }
            case FileCategory::Context:
            {
                m_CategorizedFiles.m_Context.push_back(filePath);
                break;
            }
            case FileCategory::Task:
            {
                m_CategorizedFiles.m_Tasks.push_back(filePath);
                break;
            }
            case FileCategory::Requirement:
            {
                m_CategorizedFiles.m_Requirements.push_back(filePath);
                break;
            }
            case FileCategory::SubFolder:
            {
                m_CategorizedFiles.m_Subfolders.push_back(filePath);
                break;
            }
            default:
            {
                break;
            }
        }
    }

    void FileCategorizer::RemoveFile(fs::path const& filePath)
    {
        RemoveFromVector(m_CategorizedFiles.m_Settings, filePath);
        RemoveFromVector(m_CategorizedFiles.m_Context, filePath);
        RemoveFromVector(m_CategorizedFiles.m_Tasks, filePath);
        RemoveFromVector(m_CategorizedFiles.m_Requirements, filePath);
        RemoveFromVector(m_CategorizedFiles.m_Subfolders, filePath);
    }

    void FileCategorizer::ModifyFile(fs::path const& filePath)
    {
        // For now, we just remove and re-add to update its category if the filename changed
        RemoveFile(filePath);
        AddFile(filePath);
    }

    void FileCategorizer::RemoveFromVector(std::vector<fs::path>& vec, fs::path const& path)
    {
        vec.erase(std::remove(vec.begin(), vec.end(), path), vec.end());
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
        auto printCategory = [](std::string const& category, std::vector<fs::path> const& files)
        {
            if (!files.empty())
            {
                std::cout << category << ":\n";
                for (auto const& file : files)
                {
                    std::cout << "  " << file.string() << "\n";
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
