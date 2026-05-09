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

#include <string>
#include <vector>

#include "workflow/filter/filterEngine.h"

namespace AIAssistant
{

    // Per-item entry stored in the manifest.
    struct FilterManifestEntry
    {
        size_t m_Index{0};
        std::string m_Key; // stable identity
        std::string m_SourcePath;
        std::string m_SourceMtime; // ISO 8601 or filesystem time as string
    };

    // Full manifest for one filter evaluation.
    struct FilterManifest
    {
        std::string m_FilterId;
        std::string m_EvaluatedAt; // ISO 8601
        std::string m_QueryHash;   // SHA-256 of normalized filter expression
        size_t m_ItemCount{0};
        std::vector<FilterManifestEntry> m_Items;
    };

    // Result of comparing a new evaluation against a previous manifest.
    struct FilterManifestDiff
    {
        bool m_ExpressionChanged{false};        // query_hash differs → all items stale
        std::vector<size_t> m_NewItemIndices;   // indices in new items that didn't exist before
        std::vector<std::string> m_RemovedKeys; // keys that were in old manifest but not in new
        std::vector<size_t> m_ChangedIndices;   // indices in new items whose source_mtime changed
        std::vector<size_t> m_UnchangedIndices; // indices in new items that are unchanged
    };

    // Manages reading, writing, and comparing filter manifests.
    //
    // Trust model: filterId comes from the workflow definition (operator-
    // trusted) and ends up in a filesystem path.  ManifestPath validates
    // filterId against an allowlist AND routes the resulting path through
    // ConfineUnderProjectRoot so a malformed id (containing `..`, slashes,
    // or absolute prefixes) cannot escape the workflow directory.
    class FilterManifestManager
    {
    public:
        // Build a manifest from filter evaluation results.
        [[nodiscard]] FilterManifest BuildManifest(std::string const& filterId, std::vector<FilterItem> const& items,
                                                    FilterDef const& filterDef) const;

        // Write manifest JSON to <workflowBaseDir>/<filterID>/<filterID>.manifest.json.
        // Creates the directory if it does not exist.  Atomic via temp-then-rename.
        [[nodiscard]] bool WriteManifest(FilterManifest const& manifest, std::string const& workflowBaseDir,
                                          std::string& errorMessage) const;

        // Read a previously written manifest.  Returns false if the file doesn't
        // exist, is malformed, or exceeds the size cap.
        [[nodiscard]] bool ReadManifest(std::string const& filterId, std::string const& workflowBaseDir,
                                         FilterManifest& manifestOut, std::string& errorMessage) const;

        // Compare a newly built manifest against a previous one.
        [[nodiscard]] FilterManifestDiff CompareManifests(FilterManifest const& previous,
                                                           FilterManifest const& current) const;

        // Compute SHA-256 hash of the normalized filter expression (for change detection).
        [[nodiscard]] static std::string ComputeQueryHash(FilterDef const& filterDef);

        // Get the manifest file path for a filter.  Returns empty string if
        // filterId fails the allowlist check or the resulting path escapes
        // the project root.
        [[nodiscard]] static std::string ManifestPath(std::string const& filterId, std::string const& workflowBaseDir);

    private:
        // Get current time as ISO 8601 string.
        [[nodiscard]] static std::string NowIso8601();

        // Get file modification time as string.
        [[nodiscard]] static std::string FileMtimeString(std::string const& path);
    };

} // namespace AIAssistant
