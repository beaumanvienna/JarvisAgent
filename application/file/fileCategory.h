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
#include <vector>
#include <string>

namespace fs = std::filesystem;

namespace AIAssistant
{
    enum class FileCategory
    {
        Settings,
        Context,
        Task,
        Requirement,
        SubFolder,
        Unknown
    };

    struct CategorizedFiles
    {
        std::vector<fs::path> m_Settings;
        std::vector<fs::path> m_Context;
        std::vector<fs::path> m_Tasks;
        std::vector<fs::path> m_Requirements;
        std::vector<fs::path> m_Subfolders;
    };

    class FileCategorizer
    {
    public:
        FileCategorizer() = default;
        ~FileCategorizer();

        void AddFile(fs::path const& filePath);
        void RemoveFile(fs::path const& filePath);
        void ModifyFile(fs::path const& filePath);

        const CategorizedFiles& GetCategorizedFiles() const { return m_CategorizedFiles; }
        void PrintCategorizedFiles() const;

    private:
        FileCategory Categorize(fs::path const& filePath) const;
        void RemoveFromVector(std::vector<fs::path>& vec, fs::path const& path);

        CategorizedFiles m_CategorizedFiles;
    };
} // namespace AIAssistant
