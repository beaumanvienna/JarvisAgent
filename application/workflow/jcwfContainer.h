/* Copyright (c) 2026 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace AIAssistant
{
    // JCWF container (.jcwf) is a zip archive containing:
    //   - global.json          : workflow-level metadata (triggers, version, author, defaults)
    //   - <name>.json          : root canvas task DAG
    //   - <subfolder>/         : sub-workflow (folder name = sub-workflow display name)
    //     - <subfolder>.json   : sub-workflow canvas task DAG
    //     - <nested>/          : nested sub-workflows (recursive)

    class JcwfContainer
    {
    public:
        // Extract a .jcwf zip to a target directory.
        // Creates the target directory if it doesn't exist.
        // Returns true on success.
        static bool Extract(std::filesystem::path const& jcwfPath, std::filesystem::path const& targetDir,
                            std::string& errorMessage);

        // Pack a directory tree into a .jcwf zip file.
        // Recursively adds all files and folders from sourceDir.
        // Returns true on success.
        static bool Pack(std::filesystem::path const& sourceDir, std::filesystem::path const& jcwfPath,
                         std::string& errorMessage);

        // Read a single file from a .jcwf zip without full extraction.
        // internalPath uses forward slashes (e.g. "global.json" or "sub/sub.json").
        // Returns true on success; content is written to outContent.
        static bool ReadFile(std::filesystem::path const& jcwfPath, std::string const& internalPath,
                             std::string& outContent, std::string& errorMessage);

        // List all entries in a .jcwf zip (files and directories).
        // Returns empty vector on error.
        static std::vector<std::string> ListEntries(std::filesystem::path const& jcwfPath);

        // Check if a .jcwf file is a valid zip container.
        static bool IsValidContainer(std::filesystem::path const& jcwfPath);

        // Check if the extracted folder is stale (zip is newer than the folder).
        static bool IsExtractedStale(std::filesystem::path const& jcwfPath,
                                     std::filesystem::path const& extractedDir);
    };

} // namespace AIAssistant
