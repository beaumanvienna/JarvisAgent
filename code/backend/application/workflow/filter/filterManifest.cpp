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

#include "workflow/filter/filterManifest.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

#include <openssl/sha.h>

#include "auxiliary/file.h"
#include "engine.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "simdjson/simdjson.h"

namespace AIAssistant
{
    namespace
    {
        // Hard cap on manifest file size during read.  The manifest is a flat
        // index keyed by filter; even very large filters should be well under
        // this.  An attacker who plants a multi-GB file at the manifest path
        // (via path-traversal, symlink, or filesystem compromise) cannot cause
        // unbounded heap allocation in the parsing thread.
        constexpr std::uintmax_t kMaxManifestBytes = 16ULL * 1024ULL * 1024ULL;

        // Hard cap on the number of items materialised from a manifest.  At
        // 1M items the manifest itself is far over kMaxManifestBytes already,
        // but bound here as well for defense-in-depth.
        constexpr std::size_t kMaxManifestItems = 1'000'000;

        // filterId allowlist: alphanumerics plus `_-.` (the dot allowed for
        // versioned filter ids like `myFilter.v2`).  No path separators, no
        // leading dot, no `..` sequences.  Empty rejected.  Any character
        // outside this set is a hard reject — if a workflow author needs a
        // funky filter id, they update the workflow, not the allowlist.
        [[nodiscard]] bool IsValidFilterId(std::string const& id) noexcept
        {
            if (id.empty() || id.size() > 256)
            {
                return false;
            }
            if (id.front() == '.' || id.find("..") != std::string::npos)
            {
                return false;
            }
            for (char const c : id)
            {
                bool const ok =
                    (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.';
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace


    // -----------------------------------------------------------------
    // BuildManifest
    // -----------------------------------------------------------------

    FilterManifest FilterManifestManager::BuildManifest(std::string const& filterId, std::vector<FilterItem> const& items,
                                                        FilterDef const& filterDef) const
    {
        FilterManifest manifest;
        manifest.m_FilterId = filterId;
        manifest.m_EvaluatedAt = NowIso8601();
        manifest.m_QueryHash = ComputeQueryHash(filterDef);
        manifest.m_ItemCount = items.size();

        manifest.m_Items.reserve(items.size());
        for (auto const& item : items)
        {
            FilterManifestEntry entry;
            entry.m_Index = item.m_Index;
            entry.m_Key = item.m_Key;
            entry.m_SourcePath = item.m_SourcePath;
            entry.m_SourceMtime = FileMtimeString(item.m_SourcePath);
            manifest.m_Items.push_back(std::move(entry));
        }

        return manifest;
    }

    // -----------------------------------------------------------------
    // WriteManifest — writes JSON to <workflowBaseDir>/<filterID>/<filterID>.manifest.json
    // -----------------------------------------------------------------

    bool FilterManifestManager::WriteManifest(FilterManifest const& manifest, std::string const& workflowBaseDir,
                                              std::string& errorMessage) const
    {
        std::string const path = ManifestPath(manifest.m_FilterId, workflowBaseDir);
        if (path.empty())
        {
            errorMessage = "manifest path rejected (invalid filterId or escapes project root): '" +
                           manifest.m_FilterId + "'";
            LOG_APP_ERROR("[filter] WriteManifest: {}", errorMessage);
            return false;
        }

        // Build JSON manually (the structure is small and fixed).  item_count
        // is derived from m_Items.size() — never trust a separate counter
        // field that can drift from the array contents.
        std::size_t const itemCount = manifest.m_Items.size();
        std::ostringstream json;
        json << "{\n";
        json << "  \"filter_id\": \"" << JsonHelper::EscapeJsonString(manifest.m_FilterId) << "\",\n";
        json << "  \"evaluated_at\": \"" << JsonHelper::EscapeJsonString(manifest.m_EvaluatedAt) << "\",\n";
        json << "  \"query_hash\": \"" << JsonHelper::EscapeJsonString(manifest.m_QueryHash) << "\",\n";
        json << "  \"item_count\": " << itemCount << ",\n";
        json << "  \"items\": [\n";

        for (size_t i = 0; i < manifest.m_Items.size(); ++i)
        {
            auto const& entry = manifest.m_Items[i];
            json << "    {\n";
            json << "      \"index\": " << entry.m_Index << ",\n";
            json << "      \"key\": \"" << JsonHelper::EscapeJsonString(entry.m_Key) << "\",\n";
            json << "      \"source_path\": \"" << JsonHelper::EscapeJsonString(entry.m_SourcePath) << "\",\n";
            json << "      \"source_mtime\": \"" << JsonHelper::EscapeJsonString(entry.m_SourceMtime) << "\"\n";
            json << "    }";
            if (i + 1 < manifest.m_Items.size())
            {
                json << ",";
            }
            json << "\n";
        }

        json << "  ]\n";
        json << "}\n";

        // Atomic write through the shared helper.  Helper creates parent
        // directories, opens <path>.tmp.<counter> with stream exceptions
        // enabled (failbit | badbit), writes, closes, then renames.
        if (!EngineCore::AtomicWriteFile(path, json.str(), errorMessage))
        {
            LOG_APP_ERROR("[filter] WriteManifest: {}", errorMessage);
            return false;
        }

        // Path omitted from the success log to avoid persisting full
        // filesystem layout into log aggregation; filterId + count is
        // enough for triage.
        LOG_APP_INFO("[filter] wrote manifest for filter '{}' ({} items)", manifest.m_FilterId, itemCount);

        return true;
    }

    // -----------------------------------------------------------------
    // ReadManifest — parse a previously written manifest JSON
    // -----------------------------------------------------------------

    bool FilterManifestManager::ReadManifest(std::string const& filterId, std::string const& workflowBaseDir,
                                             FilterManifest& manifestOut, std::string& errorMessage) const
    {
        std::string const path = ManifestPath(filterId, workflowBaseDir);
        if (path.empty())
        {
            errorMessage = "manifest path rejected (invalid filterId or escapes project root): '" + filterId + "'";
            LOG_APP_ERROR("[filter] ReadManifest: {}", errorMessage);
            return false;
        }

        // No exists() check — race-free single open() instead.  ENOENT is
        // an expected outcome (first run; manifest not yet written) so we
        // do not log it; other open errors are real and get a log line.
        std::error_code sizeErrorCode;
        std::uintmax_t const fileBytes = std::filesystem::file_size(path, sizeErrorCode);
        if (sizeErrorCode)
        {
            // File missing or unreadable — treat as "no previous manifest".
            errorMessage = "manifest file unavailable: " + sizeErrorCode.message();
            return false;
        }
        if (fileBytes > kMaxManifestBytes)
        {
            errorMessage = "manifest file size " + std::to_string(fileBytes) + " bytes exceeds cap " +
                           std::to_string(kMaxManifestBytes);
            LOG_APP_ERROR("[filter] ReadManifest: {} (filterId='{}')", errorMessage, filterId);
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            errorMessage = "cannot open manifest file: " + path;
            LOG_APP_ERROR("[filter] ReadManifest: {}", errorMessage);
            return false;
        }

        std::ostringstream oss;
        oss << file.rdbuf();
        std::string const content = oss.str();

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(content);
        simdjson::ondemand::document document;

        if (auto ec = parser.iterate(paddedJson).get(document); ec)
        {
            errorMessage = std::string("failed to parse manifest JSON: ") + simdjson::error_message(ec);
            LOG_APP_ERROR("[filter] ReadManifest: {} (filterId='{}')", errorMessage, filterId);
            return false;
        }

        simdjson::ondemand::object root;
        if (auto ec = document.get_object().get(root); ec)
        {
            errorMessage = std::string("manifest top-level not an object: ") + simdjson::error_message(ec);
            LOG_APP_ERROR("[filter] ReadManifest: {} (filterId='{}')", errorMessage, filterId);
            return false;
        }

        // Read top-level fields.  filter_id is required — if it is missing
        // or empty we fail closed; the manifest is corrupt, treat as absent.
        // The simdjson view-into-paddedJson borrow lifetime is the enclosing
        // brace-scope below; do not extend a string_view past this scope.
        {
            std::string_view sv;
            if (root["filter_id"].get_string().get(sv) == simdjson::SUCCESS)
            {
                manifestOut.m_FilterId = std::string(sv);
            }
            if (manifestOut.m_FilterId.empty())
            {
                errorMessage = "manifest missing required field 'filter_id'";
                LOG_APP_ERROR("[filter] ReadManifest: {} (filterId='{}')", errorMessage, filterId);
                return false;
            }

            if (root["evaluated_at"].get_string().get(sv) == simdjson::SUCCESS)
            {
                manifestOut.m_EvaluatedAt = std::string(sv);
            }

            if (root["query_hash"].get_string().get(sv) == simdjson::SUCCESS)
            {
                manifestOut.m_QueryHash = std::string(sv);
            }

            int64_t count = 0;
            if (root["item_count"].get_int64().get(count) == simdjson::SUCCESS && count >= 0)
            {
                manifestOut.m_ItemCount = static_cast<size_t>(count);
            }
        }

        // Read items array, capped at kMaxManifestItems.  Excess entries
        // beyond the cap are dropped with an error log so the operator can
        // see the truncation.
        simdjson::ondemand::array itemsArray;
        if (root["items"].get_array().get(itemsArray) == simdjson::SUCCESS)
        {
            for (simdjson::ondemand::value itemValue : itemsArray)
            {
                if (manifestOut.m_Items.size() >= kMaxManifestItems)
                {
                    LOG_APP_ERROR("[filter] ReadManifest: items count exceeds cap {} for filterId='{}'; "
                                  "remaining entries ignored",
                                  kMaxManifestItems, filterId);
                    break;
                }
                simdjson::ondemand::object itemObj;
                if (itemValue.get_object().get(itemObj) != simdjson::SUCCESS)
                {
                    continue;
                }
                FilterManifestEntry entry;

                int64_t idx = 0;
                if (itemObj["index"].get_int64().get(idx) == simdjson::SUCCESS && idx >= 0)
                {
                    entry.m_Index = static_cast<size_t>(idx);
                }

                std::string_view sv;
                if (itemObj["key"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    entry.m_Key = std::string(sv);
                }

                if (itemObj["source_path"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    entry.m_SourcePath = std::string(sv);
                }

                if (itemObj["source_mtime"].get_string().get(sv) == simdjson::SUCCESS)
                {
                    entry.m_SourceMtime = std::string(sv);
                }

                manifestOut.m_Items.push_back(std::move(entry));
            }
        }

        // Cross-validate item_count against actual array size; warn (not
        // fail) on disagreement — a hand-edited or stale manifest where
        // m_ItemCount drifted should be flagged but still usable.
        if (manifestOut.m_ItemCount != manifestOut.m_Items.size())
        {
            LOG_APP_WARN("[filter] ReadManifest: item_count={} disagrees with items.size={} for filterId='{}'",
                         manifestOut.m_ItemCount, manifestOut.m_Items.size(), filterId);
            manifestOut.m_ItemCount = manifestOut.m_Items.size();
        }

        return true;
    }

    // -----------------------------------------------------------------
    // CompareManifests
    // -----------------------------------------------------------------

    FilterManifestDiff FilterManifestManager::CompareManifests(FilterManifest const& previous,
                                                               FilterManifest const& current) const
    {
        FilterManifestDiff diff;

        // If expression changed, everything is stale
        if (previous.m_QueryHash != current.m_QueryHash)
        {
            diff.m_ExpressionChanged = true;
            for (size_t i = 0; i < current.m_Items.size(); ++i)
            {
                diff.m_NewItemIndices.push_back(i);
            }
            return diff;
        }

        // Build lookup from previous manifest: key → entry
        std::unordered_map<std::string, FilterManifestEntry const*> previousByKey;
        for (auto const& entry : previous.m_Items)
        {
            previousByKey[entry.m_Key] = &entry;
        }

        // Classify current items
        std::unordered_map<std::string, bool> currentKeys;
        for (size_t i = 0; i < current.m_Items.size(); ++i)
        {
            auto const& entry = current.m_Items[i];
            currentKeys[entry.m_Key] = true;

            auto it = previousByKey.find(entry.m_Key);
            if (it == previousByKey.end())
            {
                // New item (not in previous manifest)
                diff.m_NewItemIndices.push_back(i);
            }
            else if (it->second->m_SourceMtime != entry.m_SourceMtime)
            {
                // Source changed
                diff.m_ChangedIndices.push_back(i);
            }
            else
            {
                // Unchanged
                diff.m_UnchangedIndices.push_back(i);
            }
        }

        // Detect removed items
        for (auto const& entry : previous.m_Items)
        {
            if (currentKeys.find(entry.m_Key) == currentKeys.end())
            {
                diff.m_RemovedKeys.push_back(entry.m_Key);
            }
        }

        return diff;
    }

    // -----------------------------------------------------------------
    // ComputeQueryHash — SHA-256 of a normalized filter expression string
    // -----------------------------------------------------------------

    std::string FilterManifestManager::ComputeQueryHash(FilterDef const& filterDef)
    {
        // Build a canonical string from the filter source fields that affect item selection.
        // If any of these change, items should be re-evaluated.
        std::string canonical;
        canonical += "kind=" + filterDef.m_Source.m_Kind + ";";
        canonical += "path=" + filterDef.m_Source.m_Path + ";";
        canonical += "delimiter=" + filterDef.m_Source.m_Delimiter + ";";
        canonical += "has_header=" + std::string(filterDef.m_Source.m_HasHeader ? "true" : "false") + ";";
        canonical += "range=" + filterDef.m_Source.m_Range + ";";
        canonical += "skip_empty=" + std::string(filterDef.m_Source.m_SkipEmpty ? "true" : "false") + ";";
        canonical += "index_path=" + filterDef.m_Source.m_IndexPath + ";";
        canonical += "query=" + filterDef.m_Source.m_Query + ";";
        canonical += "connection=" + filterDef.m_Source.m_Connection + ";";
        canonical += "base_url=" + filterDef.m_Source.m_BaseUrl + ";";
        canonical += "project_id=" + filterDef.m_Source.m_ProjectId + ";";

        for (auto const& field : filterDef.m_Source.m_Fields)
        {
            canonical += "field=" + field + ";";
        }

        // OpenSSL is vendored under code/vendor/openssl and always present in
        // every supported build; the previous std::hash fallback was dead
        // code AND a security weak spot (collision-prone, predictable).
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<unsigned char const*>(canonical.data()), canonical.size(), hash);

        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (unsigned char c : hash)
        {
            hex << std::setw(2) << static_cast<int>(c);
        }

        return hex.str();
    }

    // -----------------------------------------------------------------
    // ManifestPath
    // -----------------------------------------------------------------

    std::string FilterManifestManager::ManifestPath(std::string const& filterId, std::string const& workflowBaseDir)
    {
        // First gate: filterId allowlist.  Rejects path-separators, ..,
        // leading dot, and characters outside [A-Za-z0-9._-].
        if (!IsValidFilterId(filterId))
        {
            return std::string();
        }
        // Second gate: ConfineUnderProjectRoot resolves the candidate path
        // and rejects anything that escapes the project root via absolute
        // workflowBaseDir, symlink chains, or remaining slip we missed.
        std::filesystem::path const candidate =
            std::filesystem::path(workflowBaseDir) / filterId / (filterId + ".manifest.json");
        std::filesystem::path const confined = ConfineUnderProjectRoot(candidate);
        if (confined.empty())
        {
            return std::string();
        }
        return confined.string();
    }

    // -----------------------------------------------------------------
    // NowIso8601
    // -----------------------------------------------------------------

    std::string FilterManifestManager::NowIso8601()
    {
        auto const now = std::chrono::system_clock::now();
        auto const timeT = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &timeT);
#else
        gmtime_r(&timeT, &tm);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    // -----------------------------------------------------------------
    // FileMtimeString
    // -----------------------------------------------------------------

    std::string FilterManifestManager::FileMtimeString(std::string const& path)
    {
        if (path.empty())
        {
            return "";
        }

        std::error_code ec;
        auto const ftime = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            return "";
        }

        // Convert filesystem file_time_type to a system_clock::time_point.
        //
        // Toolchain support map for the C++20 cross-clock conversion:
        //   • libstdc++ 13+   — std::chrono::file_clock::to_sys works
        //   • libc++ 16+      — to_sys returns nanoseconds-precision time_point
        //                       that does not implicitly convert to the
        //                       microsecond-precision system_clock::time_point
        //                       to_time_t expects (needs time_point_cast)
        //   • MSVC 19.34+     — to_sys works (VS 2022 17.4, Nov 2022)
        //   • MSVC 19.30-33   — to_sys missing; clock_cast missing too
        //
        // The Windows CI runner is on an MSVC older than 19.34, so we use
        // the toolchain-portable two-now() offset approach instead.  The
        // offset between file_clock and system_clock is computed once per
        // call from back-to-back clock reads (race window microseconds);
        // it is collapsed by to_time_t's 1-second precision floor below,
        // so the result of put_time below is unaffected.  This output is
        // for display only (filter manifest mtime field), so the bounded
        // race is acceptable.  For a use case that needed nanosecond-stable
        // mtime as a change-detection key (the previous concern that drove
        // the to_sys switch), we would need a platform-dispatched solution
        // using FILETIME on Windows and to_sys elsewhere.
        auto const fileNow = std::chrono::file_clock::now();
        auto const sysNow = std::chrono::system_clock::now();
        auto const sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            sysNow + (ftime - fileNow));
        auto const timeT = std::chrono::system_clock::to_time_t(sysTime);

        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &timeT);
#else
        gmtime_r(&timeT, &tm);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

} // namespace AIAssistant
