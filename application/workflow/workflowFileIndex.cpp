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

#include "workflowFileIndex.h"

#include "engine.h"

#include <system_error>

namespace AIAssistant
{
    void WorkflowFileIndex::ScanDirectory(std::filesystem::path const& rootDir)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_BasenameIndex.clear();
        m_AllFiles.clear();

        std::error_code ec;
        std::filesystem::path const rootAbsolute = std::filesystem::absolute(rootDir, ec).lexically_normal();
        if (ec || rootAbsolute.empty())
        {
            LOG_APP_ERROR("WorkflowFileIndex::ScanDirectory: cannot resolve '{}'", rootDir.string());
            return;
        }

        m_RootDirectory = rootAbsolute;

        if (!std::filesystem::is_directory(rootAbsolute, ec))
        {
            LOG_APP_INFO("WorkflowFileIndex::ScanDirectory: '{}' is not a directory", rootAbsolute.string());
            return;
        }

        for (auto const& entry : std::filesystem::recursive_directory_iterator(rootAbsolute, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            // Skip hidden files/directories
            bool hidden = false;
            for (auto const& component : entry.path())
            {
                std::string const name = component.filename().string();
                if (!name.empty() && name[0] == '.')
                {
                    hidden = true;
                    break;
                }
            }
            if (hidden)
            {
                continue;
            }

            std::filesystem::path const absolutePath = entry.path().lexically_normal();
            std::string const basename = absolutePath.filename().string();

            m_BasenameIndex[basename].push_back(absolutePath);
            m_AllFiles.push_back(absolutePath);
        }

        LOG_APP_INFO("WorkflowFileIndex::ScanDirectory: indexed {} files in '{}'", m_AllFiles.size(), rootAbsolute.string());
    }

    std::vector<std::filesystem::path> WorkflowFileIndex::FindByBasename(std::string const& basename) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_BasenameIndex.find(basename);
        if (it != m_BasenameIndex.end())
        {
            return it->second;
        }
        return {};
    }

    std::filesystem::path WorkflowFileIndex::FindByRelativePath(std::string const& relativePath) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_RootDirectory.empty() || relativePath.empty())
        {
            return {};
        }

        std::filesystem::path const candidate = (m_RootDirectory / relativePath).lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
        {
            return candidate;
        }
        return {};
    }

    std::filesystem::path WorkflowFileIndex::GetRootDirectory() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_RootDirectory;
    }

    std::vector<std::filesystem::path> WorkflowFileIndex::GetAllFiles() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_AllFiles;
    }

    size_t WorkflowFileIndex::Size() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_AllFiles.size();
    }

    std::string WorkflowFileIndex::SerializeMarkdownListing() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_AllFiles.empty())
        {
            return "(no files indexed)";
        }

        std::string result;
        result.reserve(m_AllFiles.size() * 80);

        for (std::filesystem::path const& absPath : m_AllFiles)
        {
            std::filesystem::path const relPath = absPath.lexically_relative(m_RootDirectory);
            result += "- ";
            result += relPath.generic_string();
            result += '\n';
        }

        return result;
    }
} // namespace AIAssistant
