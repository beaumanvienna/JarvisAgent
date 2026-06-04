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
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    // Scans a directory tree (typically workflows/) and indexes every file by
    // basename for fast lookup.  Thread-safe.
    class WorkflowFileIndex
    {
    public:
        // (Re-)scan the given directory recursively.
        void ScanDirectory(std::filesystem::path const& rootDir);

        // Lookup all absolute paths that share a given basename (case-sensitive).
        // Returns an empty vector when the basename is unknown.
        std::vector<std::filesystem::path> FindByBasename(std::string const& basename) const;

        // Lookup a single file by a relative path (e.g. "OpenSSH_Analysis/01_parse/OpenSSH_2k.log").
        // The relative path is resolved against the scanned root directory.
        // Returns empty path if not found on disk.
        std::filesystem::path FindByRelativePath(std::string const& relativePath) const;

        // Return the root directory that was scanned.
        std::filesystem::path GetRootDirectory() const;

        // Return all indexed file paths (absolute).
        std::vector<std::filesystem::path> GetAllFiles() const;

        // Number of indexed files.
        size_t Size() const;

        // Produce a concise markdown listing of all indexed files (relative to root).
        // Suitable for injecting into AI generation prompts.
        std::string SerializeMarkdownListing() const;

    private:
        mutable std::mutex m_Mutex;
        std::filesystem::path m_RootDirectory;
        // basename -> list of absolute paths (handles name collisions)
        std::unordered_map<std::string, std::vector<std::filesystem::path>> m_BasenameIndex;
        // All absolute paths (for GetAllFiles)
        std::vector<std::filesystem::path> m_AllFiles;
    };
} // namespace AIAssistant
