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
    struct ScriptRegistryEntry
    {
        std::string m_FilePath;
        std::string m_Short;
        std::string m_Description;
        std::vector<std::string> m_Params;
        std::vector<std::string> m_Outputs;
        std::vector<std::string> m_ExportedFunctions;
    };

    class ScriptRegistry
    {
    public:
        void ScanDirectory(std::filesystem::path const& scriptsDir);

        void AddOrUpdate(std::filesystem::path const& filePath);
        void Remove(std::filesystem::path const& filePath);

        std::string SerializeMarkdownTable() const;
        std::vector<ScriptRegistryEntry> GetEntries() const;
        size_t Size() const;

        // Lookup by Python module path (e.g. "scripts.parseLog" -> "scripts/parseLog.py").
        ScriptRegistryEntry const* FindByModulePath(std::string const& modulePath) const;

        // Lookup by relative file path (e.g. "scripts/parseLog.py").
        ScriptRegistryEntry const* FindByFilePath(std::string const& filePath) const;

    private:
        static bool ParseHeader(std::filesystem::path const& filePath, ScriptRegistryEntry& outEntry);

        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, ScriptRegistryEntry> m_Entries;
    };
} // namespace AIAssistant
