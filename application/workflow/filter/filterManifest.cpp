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

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

#include "engine.h"
#include "simdjson/simdjson.h"

#if __has_include(<openssl/sha.h>)
#include <openssl/sha.h>
#define HAS_OPENSSL_SHA 1
#else
#define HAS_OPENSSL_SHA 0
#endif

namespace AIAssistant
{

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

        // Ensure the directory exists
        std::filesystem::path dirPath = std::filesystem::path(path).parent_path();
        std::error_code ec;
        std::filesystem::create_directories(dirPath, ec);
        if (ec)
        {
            errorMessage = "failed to create manifest directory '" + dirPath.string() + "': " + ec.message();
            return false;
        }

        // Build JSON manually (the structure is small and fixed)
        std::ostringstream json;
        json << "{\n";
        json << "  \"filter_id\": \"" << JsonEscape(manifest.m_FilterId) << "\",\n";
        json << "  \"evaluated_at\": \"" << JsonEscape(manifest.m_EvaluatedAt) << "\",\n";
        json << "  \"query_hash\": \"" << JsonEscape(manifest.m_QueryHash) << "\",\n";
        json << "  \"item_count\": " << manifest.m_ItemCount << ",\n";
        json << "  \"items\": [\n";

        for (size_t i = 0; i < manifest.m_Items.size(); ++i)
        {
            auto const& entry = manifest.m_Items[i];
            json << "    {\n";
            json << "      \"index\": " << entry.m_Index << ",\n";
            json << "      \"key\": \"" << JsonEscape(entry.m_Key) << "\",\n";
            json << "      \"source_path\": \"" << JsonEscape(entry.m_SourcePath) << "\",\n";
            json << "      \"source_mtime\": \"" << JsonEscape(entry.m_SourceMtime) << "\"\n";
            json << "    }";
            if (i + 1 < manifest.m_Items.size())
            {
                json << ",";
            }
            json << "\n";
        }

        json << "  ]\n";
        json << "}\n";

        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open())
        {
            errorMessage = "failed to write manifest to '" + path + "'";
            return false;
        }

        file << json.str();
        file.close();

        LOG_APP_INFO("[filter] wrote manifest for filter '{}' ({} items) to '{}'", manifest.m_FilterId, manifest.m_ItemCount,
                     path);

        return true;
    }

    // -----------------------------------------------------------------
    // ReadManifest — parse a previously written manifest JSON
    // -----------------------------------------------------------------

    bool FilterManifestManager::ReadManifest(std::string const& filterId, std::string const& workflowBaseDir,
                                             FilterManifest& manifestOut, std::string& errorMessage) const
    {
        std::string const path = ManifestPath(filterId, workflowBaseDir);

        if (!std::filesystem::exists(path))
        {
            errorMessage = "manifest file does not exist: " + path;
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            errorMessage = "cannot open manifest file: " + path;
            return false;
        }

        std::ostringstream oss;
        oss << file.rdbuf();
        std::string content = oss.str();

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(content);
        simdjson::ondemand::document document;

        simdjson::error_code ec = parser.iterate(paddedJson).get(document);
        if (ec)
        {
            errorMessage = "failed to parse manifest JSON: ";
            errorMessage += simdjson::error_message(ec);
            return false;
        }

        simdjson::ondemand::object root = document.get_object();

        // Read top-level fields
        {
            std::string_view sv;
            if (root["filter_id"].get_string().get(sv) == simdjson::SUCCESS)
            {
                manifestOut.m_FilterId = std::string(sv);
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
            if (root["item_count"].get_int64().get(count) == simdjson::SUCCESS)
            {
                manifestOut.m_ItemCount = static_cast<size_t>(count);
            }
        }

        // Read items array
        simdjson::ondemand::array itemsArray;
        if (root["items"].get_array().get(itemsArray) == simdjson::SUCCESS)
        {
            for (simdjson::ondemand::value itemValue : itemsArray)
            {
                simdjson::ondemand::object itemObj = itemValue.get_object();
                FilterManifestEntry entry;

                int64_t idx = 0;
                if (itemObj["index"].get_int64().get(idx) == simdjson::SUCCESS)
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
        canonical += "base_url=" + filterDef.m_Source.m_BaseUrl + ";";
        canonical += "project_id=" + filterDef.m_Source.m_ProjectId + ";";

        for (auto const& field : filterDef.m_Source.m_Fields)
        {
            canonical += "field=" + field + ";";
        }

#if HAS_OPENSSL_SHA
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<unsigned char const*>(canonical.data()), canonical.size(), hash);

        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (unsigned char c : hash)
        {
            hex << std::setw(2) << static_cast<int>(c);
        }

        return hex.str();
#else
        // Fallback: std::hash
        return std::to_string(std::hash<std::string>{}(canonical));
#endif
    }

    // -----------------------------------------------------------------
    // ManifestPath
    // -----------------------------------------------------------------

    std::string FilterManifestManager::ManifestPath(std::string const& filterId, std::string const& workflowBaseDir)
    {
        return (std::filesystem::path(workflowBaseDir) / filterId / (filterId + ".manifest.json")).string();
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

        // Convert file_time to system_clock time
        auto const sctp = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(ftime));
        auto const timeT = std::chrono::system_clock::to_time_t(sctp);

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
    // JsonEscape
    // -----------------------------------------------------------------

    std::string FilterManifestManager::JsonEscape(std::string const& s)
    {
        std::string result;
        result.reserve(s.size() + 8);

        for (char c : s)
        {
            switch (c)
            {
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    result += c;
                    break;
            }
        }

        return result;
    }

} // namespace AIAssistant
