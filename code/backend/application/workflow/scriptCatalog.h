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
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace AIAssistant
{
    // Catalog of scripts under scripts/ available to JCWF runs. Built by
    // scanning the directory and parsing the `@jarvis-script` metadata
    // header each script carries in its comment block. Served to MCP agents
    // via GET /api/scripts so they can pick pre-deployed scripts when
    // composing adhoc workflows.
    //
    // Shell metadata format (first ~50 lines, any comment style):
    //     # @jarvis-script
    //     # @short: One-line description
    //     # @params: arg1 arg2 arg3
    //     # @description: Longer description, can wrap onto
    //     #   indented continuation lines.
    //     # @outputs: Free-text description of the produced artifacts
    //
    // Python metadata format: same tags in `#` comments at the top of the
    // module, plus the module name is derived from the path — a script at
    // `scripts/extractChapters.py` exposes module `scripts.extractChapters`
    // which JCWFs reference via `params.module`.
    class ScriptCatalog
    {
    public:
        struct Entry
        {
            std::string m_Path;           // "scripts/parseOpenSshLog.sh" (relative to launch CWD)
            std::string m_Type;           // "shell" or "python"
            std::string m_Module;         // python only — e.g. "scripts.extractChapters"
            std::string m_Short;
            std::string m_Description;
            std::vector<std::string> m_Params;
            std::string m_Outputs;
            bool m_HasShebang{false};
            bool m_HasJarvisMarker{false};
            bool m_Executable{false};
        };

        // (Re-)scan the scripts folder and rebuild the catalog. Thread-safe.
        // Symlinked files are skipped — a malicious symlink under scripts/
        // pointing at /etc/shadow or similar must not produce a catalog entry
        // whose `m_Path` lets a downstream consumer escape the scripts root.
        // Entries are also capped at kMaxEntries to bound heap pressure
        // against directories that have grown unexpectedly.
        void Refresh(std::filesystem::path const& scriptsBaseFolder);

        // Snapshot of all known scripts. Filter by type ("shell" / "python") or
        // pass an empty string to list everything.
        [[nodiscard]] std::vector<Entry> List(std::string const& typeFilter = {}) const;

        // Lookup by path (shell) or module (python). Returns nullopt on miss.
        [[nodiscard]] std::optional<Entry> GetByPath(std::string const& relativePath) const;
        [[nodiscard]] std::optional<Entry> GetByModule(std::string const& moduleName) const;

        [[nodiscard]] std::filesystem::path GetRootDirectory() const;
        [[nodiscard]] size_t Size() const;

    private:
        // Parse a single file's @jarvis-script metadata. Opens and reads the
        // first kMaxMetadataLines lines — cheap, scales to thousands of scripts.
        static Entry ParseFile(std::filesystem::path const& absolutePath,
                               std::filesystem::path const& rootDir);

        mutable std::mutex m_Mutex;
        std::filesystem::path m_RootDirectory;
        std::vector<Entry> m_Entries;
    };
} // namespace AIAssistant
