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
#include <string>
#include <atomic>
#include <mutex>
#include <fstream>
#include <sstream>
#include <optional>

#include "file/fileCategory.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    class TrackedFile
    {
    public:
        TrackedFile(fs::path const& path, FileCategory fileCategory);

        bool IsModified() const { return m_Modified.load(); }
        fs::path const& GetPath() const { return m_Path; }

        // marks it as modified
        void MarkModified(bool modified = true);

        // retrieves content (and resets modified flag)
        std::string GetContent();
        FileCategory GetCategory() const;

        // called when file changes on disk
        // to make sure it really changed
        bool CheckIfContentChanged();

    private:
        std::string ComputeFileHash() const;

    private:
        fs::path m_Path;
        FileCategory m_FileCategory;
        std::atomic<bool> m_Modified{true}; // all new files start "modified"
        std::string m_LastHash;
        mutable std::mutex m_Mutex;
    };
} // namespace AIAssistant
