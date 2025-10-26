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
#include <unordered_map>
#include <string>

#include "engine.h"
#include "file/trackedFile.h"
#include "file/fileCategory.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    using TrackedFileMap = std::unordered_map<std::string, std::unique_ptr<TrackedFile>>;

    struct TrackedFiles
    {
        TrackedFileMap m_Map;
        bool m_Dirty{true};
        TrackedFileMap& Write()
        {
            m_Dirty = true;
            return m_Map;
        }
        uint m_ModifiedFiles{0};

        TrackedFileMap& Get() { return m_Map; }
        void SetDirty(bool dirty = true) { m_Dirty = dirty; }
        bool GetDirty() const { return m_Dirty; }
        void IncrementModifiedFiles();
        void DecrementModifiedFiles();
        uint GetModifiedFiles() const { return m_ModifiedFiles; }
    };

    struct CategorizedFiles
    {
        TrackedFiles m_Settings;
        TrackedFiles m_Context;
        TrackedFiles m_Tasks;
        TrackedFiles m_Requirements;
        TrackedFiles m_Subfolders;
    };

    class FileCategorizer
    {
    public:
        FileCategorizer() = default;
        ~FileCategorizer();

        fs::path const AddFile(fs::path const& filePath);
        fs::path const RemoveFile(fs::path const& filePath);
        fs::path const ModifyFile(fs::path const& filePath);

        CategorizedFiles& GetCategorizedFiles() { return m_CategorizedFiles; }
        void PrintCategorizedFiles() const;

    private:
        FileCategory Categorize(fs::path const& filePath) const;
        void RemoveFromFiles(TrackedFiles& files, fs::path const& path);

        CategorizedFiles m_CategorizedFiles;
    };
} // namespace AIAssistant
