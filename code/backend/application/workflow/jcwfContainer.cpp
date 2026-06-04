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

#include "jcwfContainer.h"

#include <algorithm>
#include <fstream>
#include <string_view>

#include "engine.h"
#include "miniz.h"

namespace AIAssistant
{
    namespace
    {
        // The high 16 bits of the central-directory `external file attributes`
        // word hold the Unix mode bits when the zip was authored on Unix.  The
        // S_IFMT field occupies the top 4 of those 16 (so bits 28..31 of the
        // 32-bit external_attr).  S_IFLNK = 0o120000 → 0xA000 in the high 16
        // bits → 0xA0000000 here.
        constexpr mz_uint32 kUnixModeMask = 0xF0000000u;
        constexpr mz_uint32 kUnixModeLnk  = 0xA0000000u;

        // Reasons a zip entry name is rejected at validation.  Returns empty
        // on success, a short reason string otherwise — the caller embeds it
        // into both the user-facing errorMessage and the LOG_APP_ERROR line.
        std::string ValidateEntryName(std::string_view name)
        {
            if (name.empty())
                return "empty filename";
            if (name.find('\0') != std::string_view::npos)
                return "embedded NUL byte";
            // Reject absolute paths regardless of host.  Zip standard mandates
            // forward-slash-only relative names; absolute paths are by spec
            // already malformed and have only ever meant "try to write here".
            if (name.front() == '/' || name.front() == '\\')
                return "absolute path";
            // Windows drive-letter form (e.g. "C:\\Users\\...") — also rejected.
            if (name.size() >= 2 && name[1] == ':')
                return "Windows drive-letter path";
            // Reject any '..' path segment.  Tokenise on '/' and '\\' both —
            // zip standard says forward-slash only, but we defend against an
            // archive that snuck in backslash separators.
            size_t pos = 0;
            while (pos < name.size())
            {
                size_t const sep = name.find_first_of("/\\", pos);
                std::string_view const segment = (sep == std::string_view::npos)
                                                     ? name.substr(pos)
                                                     : name.substr(pos, sep - pos);
                if (segment == "..")
                    return "parent-directory ('..') segment";
                if (sep == std::string_view::npos)
                    break;
                pos = sep + 1;
            }
            return {};
        }

        bool IsSymlinkEntry(mz_zip_archive_file_stat const& stat)
        {
            return (stat.m_external_attr & kUnixModeMask) == kUnixModeLnk;
        }

        // Defense-in-depth: after the name-shape check passes, confirm the
        // resolved destination still lies under the resolved target dir.
        // weakly_canonical (not canonical) because the entry doesn't exist on
        // disk yet — it resolves the existing prefix (including symlinks in
        // ancestor directories) and leaves the rest normalized.  Lexicographic
        // prefix comparison on canonical absolute paths is the standard
        // zip-slip containment check.
        std::string ValidateContainment(std::filesystem::path const& destPath,
                                        std::filesystem::path const& canonicalTargetDir)
        {
            std::error_code ec;
            std::filesystem::path const canonicalDest = std::filesystem::weakly_canonical(destPath, ec);
            if (ec)
                return "weakly_canonical failed: " + ec.message();
            auto const [targetIt, destIt] = std::mismatch(canonicalTargetDir.begin(), canonicalTargetDir.end(),
                                                          canonicalDest.begin(), canonicalDest.end());
            if (targetIt != canonicalTargetDir.end())
                return "resolves outside target directory";
            return {};
        }
    } // namespace

    bool JcwfContainer::Extract(std::filesystem::path const& jcwfPath, std::filesystem::path const& targetDir,
                                std::string& errorMessage)
    {
        std::string const containerName = jcwfPath.filename().string();

        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, jcwfPath.string().c_str(), 0))
        {
            // Surface only the basename to API consumers — full install path
            // belongs in the operator's local log, not the response body.
            errorMessage = "Failed to open zip: " + containerName;
            LOG_APP_ERROR("[jcwf] Extract: failed to open zip path='{}'", jcwfPath.string());
            return false;
        }

        // Resolve the target dir's canonical form once for the containment
        // check.  weakly_canonical handles the case where targetDir doesn't
        // exist yet (it will be created below) — it normalises the path and
        // resolves any existing ancestor symlinks.
        std::error_code canonEc;
        std::filesystem::path const absTargetDir = std::filesystem::absolute(targetDir, canonEc);
        std::filesystem::path const canonicalTargetDir =
            std::filesystem::weakly_canonical(absTargetDir, canonEc);

        mz_uint const numFiles = mz_zip_reader_get_num_files(&zip);

        // ---- Validation pass: NO disk I/O.  Every entry is checked before
        // any byte is written.  A single hostile entry aborts the whole
        // extraction (fail-closed); partial-state on disk is the wrong
        // failure mode for a Zip-Slip defence.
        for (mz_uint i = 0; i < numFiles; ++i)
        {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&zip, i, &stat))
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Failed to stat entry #" + std::to_string(i) + " in " + containerName;
                LOG_APP_ERROR("[jcwf] Extract: stat failed container='{}' index={}", containerName, i);
                return false;
            }

            std::string_view const name(stat.m_filename);

            if (std::string const reason = ValidateEntryName(name); !reason.empty())
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Rejected entry '" + std::string(name) + "' in " + containerName +
                               ": " + reason;
                LOG_APP_ERROR("[jcwf] Extract: rejected malformed entry container='{}' entry='{}' reason='{}'",
                              containerName, std::string(name), reason);
                return false;
            }

            if (IsSymlinkEntry(stat))
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Rejected symlink entry '" + std::string(name) + "' in " + containerName;
                LOG_APP_ERROR("[jcwf] Extract: rejected symlink container='{}' entry='{}' external_attr=0x{:08x}",
                              containerName, std::string(name), stat.m_external_attr);
                return false;
            }

            std::filesystem::path const destPath = targetDir / std::filesystem::path(name);
            if (std::string const reason = ValidateContainment(destPath, canonicalTargetDir); !reason.empty())
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Rejected escaping entry '" + std::string(name) + "' in " + containerName +
                               ": " + reason;
                LOG_APP_ERROR("[jcwf] Extract: rejected escaping entry container='{}' entry='{}' reason='{}'",
                              containerName, std::string(name), reason);
                return false;
            }
        }

        // ---- Extraction pass: every entry has been validated above.
        std::error_code ec;
        std::filesystem::create_directories(targetDir, ec);
        if (ec)
        {
            mz_zip_reader_end(&zip);
            errorMessage = "Failed to create extraction directory '" +
                           targetDir.filename().string() + "' (" + ec.message() + ")";
            LOG_APP_ERROR("[jcwf] Extract: failed to create directory path='{}' ec={}",
                          targetDir.string(), ec.message());
            return false;
        }

        for (mz_uint i = 0; i < numFiles; ++i)
        {
            char filename[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
            mz_zip_reader_get_filename(&zip, i, filename, sizeof(filename));

            std::filesystem::path const destPath = targetDir / filename;

            if (mz_zip_reader_is_file_a_directory(&zip, i))
            {
                std::filesystem::create_directories(destPath, ec);
                continue;
            }

            // Ensure parent directory exists.
            std::filesystem::path const parentDir = destPath.parent_path();
            if (!parentDir.empty())
            {
                std::filesystem::create_directories(parentDir, ec);
            }

            if (!mz_zip_reader_extract_to_file(&zip, i, destPath.string().c_str(), 0))
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Failed to extract '" + std::string(filename) + "' from " + containerName;
                LOG_APP_ERROR("[jcwf] Extract: failed to write container='{}' entry='{}'",
                              containerName, std::string(filename));
                return false;
            }
        }

        mz_zip_reader_end(&zip);

        LOG_APP_INFO("[JcwfContainer] extracted '{}' to '{}' ({} entries)", jcwfPath.string(), targetDir.string(),
                     numFiles);
        return true;
    }

    bool JcwfContainer::Pack(std::filesystem::path const& sourceDir, std::filesystem::path const& jcwfPath,
                             std::string& errorMessage)
    {
        if (!std::filesystem::is_directory(sourceDir))
        {
            errorMessage = "Source is not a directory: " + sourceDir.filename().string();
            LOG_APP_ERROR("[jcwf] Pack: source is not a directory path='{}'", sourceDir.string());
            return false;
        }

        mz_zip_archive zip{};
        if (!mz_zip_writer_init_file(&zip, jcwfPath.string().c_str(), 0))
        {
            errorMessage = "Failed to create zip: " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] Pack: failed to create zip path='{}'", jcwfPath.string());
            return false;
        }

        std::error_code ec;
        uint32_t fileCount = 0;

        for (auto const& entry : std::filesystem::recursive_directory_iterator(sourceDir, ec))
        {
            std::filesystem::path const relativePath = std::filesystem::relative(entry.path(), sourceDir, ec);
            if (ec)
            {
                continue;
            }

            // Use forward slashes in zip entries.
            std::string archiveName = relativePath.generic_string();

            if (entry.is_directory())
            {
                // Add directory entry (trailing slash).
                archiveName += "/";
                mz_zip_writer_add_mem(&zip, archiveName.c_str(), nullptr, 0, 0);
                continue;
            }

            if (entry.is_regular_file())
            {
                if (!mz_zip_writer_add_file(&zip, archiveName.c_str(), entry.path().string().c_str(), nullptr, 0,
                                            MZ_BEST_COMPRESSION))
                {
                    mz_zip_writer_end(&zip);
                    errorMessage = "Failed to add '" + archiveName + "' to zip";
                    return false;
                }
                ++fileCount;
            }
        }

        if (!mz_zip_writer_finalize_archive(&zip))
        {
            mz_zip_writer_end(&zip);
            errorMessage = "Failed to finalize zip: " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] Pack: failed to finalize zip path='{}'", jcwfPath.string());
            return false;
        }

        mz_zip_writer_end(&zip);

        LOG_APP_INFO("[JcwfContainer] packed '{}' into '{}' ({} files)", sourceDir.string(), jcwfPath.string(),
                     fileCount);
        return true;
    }

    bool JcwfContainer::ReadFile(std::filesystem::path const& jcwfPath, std::string const& internalPath,
                                 std::string& outContent, std::string& errorMessage)
    {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, jcwfPath.string().c_str(), 0))
        {
            errorMessage = "Failed to open zip: " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] ReadFile: failed to open zip path='{}'", jcwfPath.string());
            return false;
        }

        size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, internalPath.c_str(), &size, 0);

        if (data == nullptr)
        {
            mz_zip_reader_end(&zip);
            errorMessage = "File '" + internalPath + "' not found in " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] ReadFile: entry not found entry='{}' container='{}'",
                          internalPath, jcwfPath.string());
            return false;
        }

        outContent.assign(static_cast<char const*>(data), size);
        mz_free(data);
        mz_zip_reader_end(&zip);
        return true;
    }

    std::vector<std::string> JcwfContainer::ListEntries(std::filesystem::path const& jcwfPath)
    {
        std::vector<std::string> entries;

        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, jcwfPath.string().c_str(), 0))
        {
            return entries;
        }

        mz_uint const numFiles = mz_zip_reader_get_num_files(&zip);
        entries.reserve(numFiles);

        for (mz_uint i = 0; i < numFiles; ++i)
        {
            char filename[1024];
            mz_zip_reader_get_filename(&zip, i, filename, sizeof(filename));
            entries.emplace_back(filename);
        }

        mz_zip_reader_end(&zip);
        return entries;
    }

    bool JcwfContainer::IsValidContainer(std::filesystem::path const& jcwfPath)
    {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, jcwfPath.string().c_str(), 0))
        {
            return false;
        }

        mz_zip_reader_end(&zip);
        return true;
    }

    bool JcwfContainer::IsExtractedStale(std::filesystem::path const& jcwfPath,
                                         std::filesystem::path const& extractedDir)
    {
        std::error_code ec;

        if (!std::filesystem::exists(extractedDir, ec))
        {
            return true; // Not extracted yet.
        }

        if (!std::filesystem::exists(jcwfPath, ec))
        {
            return false; // No zip to compare against.
        }

        auto const zipTime = std::filesystem::last_write_time(jcwfPath, ec);
        if (ec)
        {
            return true;
        }

        // Check if any file in the extracted dir is older than the zip.
        // Simple heuristic: compare zip mtime against the extracted directory's mtime.
        auto const dirTime = std::filesystem::last_write_time(extractedDir, ec);
        if (ec)
        {
            return true;
        }

        return zipTime > dirTime;
    }

} // namespace AIAssistant
