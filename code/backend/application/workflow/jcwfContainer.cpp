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
#include <string_view>

#include "auxiliary/file.h"
#include "engine.h"
#include "miniz.h"

namespace AIAssistant
{
    namespace
    {
        // Zip-bomb guards.  A .jcwf holds DAG/metadata JSON (the largest entry
        // across every shipped container is under 1 MiB); these caps sit far
        // above real usage but bound the heap a hostile archive can force us to
        // allocate — a tiny zip can otherwise decompress to gigabytes.
        constexpr mz_uint64 kMaxEntryUncompressedBytes = 128ull * 1024 * 1024; // 128 MiB / entry
        constexpr mz_uint64 kMaxTotalUncompressedBytes = 512ull * 1024 * 1024; // 512 MiB / archive
        constexpr mz_uint   kMaxEntryCount             = 8192;
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

        // Create `dirToEnsure` and every directory component between `rootReal`
        // (an existing, canonical, real directory) and it — one component at a
        // time — rejecting any existing component that is a symlink.  This is
        // the write-time half of the Zip-Slip defence: the upfront validation
        // pass proves the *archive* is benign, but a concurrent process can
        // plant a symlink into the extraction tree between validation and the
        // write (the symlink-race TOCTOU).  Walking + refusing a symlink
        // ancestor here means no write can be redirected out of the tree
        // through an ancestor link.  Returns empty on success, a short reason
        // otherwise.
        std::string EnsureSafeDirs(std::filesystem::path const& rootReal,
                                   std::filesystem::path const& dirToEnsure)
        {
            std::filesystem::path const rel = dirToEnsure.lexically_relative(rootReal);
            std::filesystem::path current = rootReal;
            for (auto const& comp : rel)
            {
                if (comp.empty() || comp == ".")
                    continue;
                if (comp == "..")
                    return "directory path escapes target root";
                current /= comp;

                std::error_code ec;
                std::filesystem::file_status const st = std::filesystem::symlink_status(current, ec);
                if (std::filesystem::is_symlink(st))
                    return "ancestor '" + comp.string() + "' is a symlink";
                if (std::filesystem::exists(st))
                {
                    if (!std::filesystem::is_directory(st))
                        return "ancestor '" + comp.string() + "' exists and is not a directory";
                    continue; // real directory — descend into it
                }
                if (!std::filesystem::create_directory(current, ec) && ec)
                    return "failed to create directory component '" + comp.string() + "': " + ec.message();
            }
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
        // An empty canonical root makes the std::mismatch containment check
        // below trivially pass (every path "starts with" the empty prefix), so
        // a resolution failure here must fail-closed, not silently disarm the
        // Zip-Slip defence.
        if (canonEc || canonicalTargetDir.empty())
        {
            mz_zip_reader_end(&zip);
            errorMessage = "Failed to resolve target directory for " + containerName;
            LOG_APP_ERROR("[jcwf] Extract: target dir resolution failed container='{}' path='{}' ec={}",
                          containerName, targetDir.string(), canonEc.message());
            return false;
        }

        mz_uint const numFiles = mz_zip_reader_get_num_files(&zip);

        if (numFiles > kMaxEntryCount)
        {
            mz_zip_reader_end(&zip);
            errorMessage = "Too many entries (" + std::to_string(numFiles) + ") in " + containerName;
            LOG_APP_ERROR("[jcwf] Extract: rejected over-count container='{}' entries={} cap={}",
                          containerName, numFiles, kMaxEntryCount);
            return false;
        }

        // ---- Validation pass: NO disk I/O.  Every entry is checked before
        // any byte is written.  A single hostile entry aborts the whole
        // extraction (fail-closed); partial-state on disk is the wrong
        // failure mode for a Zip-Slip defence.
        mz_uint64 totalUncompressed = 0;
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

            if (stat.m_is_encrypted)
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Rejected encrypted entry '" + std::string(name) + "' in " + containerName;
                LOG_APP_ERROR("[jcwf] Extract: rejected encrypted entry container='{}' entry='{}'",
                              containerName, std::string(name));
                return false;
            }

            if (!stat.m_is_directory)
            {
                if (stat.m_uncomp_size > kMaxEntryUncompressedBytes)
                {
                    mz_zip_reader_end(&zip);
                    errorMessage = "Entry '" + std::string(name) + "' exceeds size cap in " + containerName;
                    LOG_APP_ERROR("[jcwf] Extract: rejected oversized entry container='{}' entry='{}' "
                                  "uncomp={} cap={}",
                                  containerName, std::string(name), stat.m_uncomp_size,
                                  kMaxEntryUncompressedBytes);
                    return false;
                }
                totalUncompressed += stat.m_uncomp_size;
                if (totalUncompressed > kMaxTotalUncompressedBytes)
                {
                    mz_zip_reader_end(&zip);
                    errorMessage = "Total uncompressed size exceeds cap in " + containerName;
                    LOG_APP_ERROR("[jcwf] Extract: rejected over-total container='{}' total={} cap={}",
                                  containerName, totalUncompressed, kMaxTotalUncompressedBytes);
                    return false;
                }
            }

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

        // ---- Extraction pass: every entry has been validated above; what
        // remains are the write-time, filesystem-side checks the validation
        // pass structurally cannot make (it does no I/O).  A symlink planted
        // into the extraction tree by a concurrent process *after* validation
        // is defended here: EnsureSafeDirs refuses to descend through a symlink
        // ancestor, the write goes through AtomicWriteFile (tmp-file + rename,
        // which replaces a symlink at the destination rather than following
        // it), and a post-write canonical re-check fails the whole extraction
        // closed if anything still resolved out of the tree.
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

        // Resolve the *real* (existing) root now that it's on disk, so the
        // per-component walk and containment net compare against the resolved
        // directory rather than the pre-creation weakly_canonical form.
        std::filesystem::path const realTargetDir = std::filesystem::canonical(targetDir, ec);
        if (ec || realTargetDir.empty())
        {
            mz_zip_reader_end(&zip);
            errorMessage = "Failed to resolve extraction directory for " + containerName;
            LOG_APP_ERROR("[jcwf] Extract: canonical(targetDir) failed container='{}' path='{}' ec={}",
                          containerName, targetDir.string(), ec.message());
            return false;
        }

        for (mz_uint i = 0; i < numFiles; ++i)
        {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&zip, i, &stat))
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Failed to stat entry #" + std::to_string(i) + " in " + containerName;
                LOG_APP_ERROR("[jcwf] Extract: stat failed (write pass) container='{}' index={}", containerName, i);
                return false;
            }

            std::string const entryName(stat.m_filename);
            std::filesystem::path const destPath = realTargetDir / std::filesystem::path(entryName);

            if (stat.m_is_directory)
            {
                if (std::string const reason = EnsureSafeDirs(realTargetDir, destPath); !reason.empty())
                {
                    mz_zip_reader_end(&zip);
                    errorMessage = "Failed to create dir entry '" + entryName + "' in " + containerName + ": " + reason;
                    LOG_APP_ERROR("[jcwf] Extract: unsafe dir entry container='{}' entry='{}' reason='{}'",
                                  containerName, entryName, reason);
                    return false;
                }
                continue;
            }

            // Create the parent chain safely (rejects a symlink ancestor).
            if (std::string const reason = EnsureSafeDirs(realTargetDir, destPath.parent_path()); !reason.empty())
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Failed to create parent of '" + entryName + "' in " + containerName + ": " + reason;
                LOG_APP_ERROR("[jcwf] Extract: unsafe parent container='{}' entry='{}' reason='{}'",
                              containerName, entryName, reason);
                return false;
            }

            // Refuse to write onto a symlink at the destination itself.
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(destPath, ec)))
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Rejected symlink destination for '" + entryName + "' in " + containerName;
                LOG_APP_ERROR("[jcwf] Extract: destination is a symlink container='{}' entry='{}'",
                              containerName, entryName);
                return false;
            }

            // Decompress into a bounded heap buffer (size cap proven above),
            // then write via the atomic helper.  extract_to_heap is safe to
            // call by index without re-running the name lookup.
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (data == nullptr)
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Failed to extract '" + entryName + "' from " + containerName;
                LOG_APP_ERROR("[jcwf] Extract: heap extract failed container='{}' entry='{}'",
                              containerName, entryName);
                return false;
            }

            std::string writeError;
            bool const wrote = EngineCore::AtomicWriteFile(
                destPath, std::string_view(static_cast<char const*>(data), size), writeError);
            mz_free(data);
            if (!wrote)
            {
                mz_zip_reader_end(&zip);
                errorMessage = "Failed to write '" + entryName + "' from " + containerName + ": " + writeError;
                LOG_APP_ERROR("[jcwf] Extract: failed to write container='{}' entry='{}': {}",
                              containerName, entryName, writeError);
                return false;
            }

            // Final safety net: the freshly-written file must still resolve
            // under the real root.  If a symlink slipped in despite the checks
            // above, remove the escaped output (fs::remove on a symlink unlinks
            // the link, not its target) and fail the whole extraction closed.
            if (std::string const reason = ValidateContainment(destPath, realTargetDir); !reason.empty())
            {
                std::error_code rmEc;
                std::filesystem::remove(destPath, rmEc);
                mz_zip_reader_end(&zip);
                errorMessage = "Post-write containment check failed for '" + entryName + "' in " + containerName +
                               ": " + reason;
                LOG_APP_ERROR("[jcwf] Extract: post-write escape container='{}' entry='{}' reason='{}'",
                              containerName, entryName, reason);
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
        // Validate the requested entry name with the same gate the extraction
        // path uses — a NUL / absolute / '..' name has no legitimate meaning
        // for a zip-internal lookup and is rejected before it reaches miniz.
        if (std::string const reason = ValidateEntryName(internalPath); !reason.empty())
        {
            errorMessage = "Rejected entry '" + internalPath + "': " + reason;
            LOG_APP_ERROR("[jcwf] ReadFile: rejected entry name entry='{}' container='{}' reason='{}'",
                          internalPath, jcwfPath.string(), reason);
            return false;
        }

        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, jcwfPath.string().c_str(), 0))
        {
            errorMessage = "Failed to open zip: " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] ReadFile: failed to open zip path='{}'", jcwfPath.string());
            return false;
        }

        int const index = mz_zip_reader_locate_file(&zip, internalPath.c_str(), nullptr, 0);
        if (index < 0)
        {
            mz_zip_reader_end(&zip);
            errorMessage = "File '" + internalPath + "' not found in " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] ReadFile: entry not found entry='{}' container='{}'",
                          internalPath, jcwfPath.string());
            return false;
        }

        // Bound the heap allocation before decompressing — a zip bomb can
        // declare a multi-gigabyte uncompressed size from a few KB on disk.
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(index), &stat))
        {
            mz_zip_reader_end(&zip);
            errorMessage = "Failed to stat '" + internalPath + "' in " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] ReadFile: stat failed entry='{}' container='{}'", internalPath, jcwfPath.string());
            return false;
        }
        if (stat.m_uncomp_size > kMaxEntryUncompressedBytes)
        {
            mz_zip_reader_end(&zip);
            errorMessage = "Entry '" + internalPath + "' exceeds size cap in " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] ReadFile: rejected oversized entry entry='{}' container='{}' uncomp={} cap={}",
                          internalPath, jcwfPath.string(), stat.m_uncomp_size, kMaxEntryUncompressedBytes);
            return false;
        }

        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(index), &size, 0);
        if (data == nullptr)
        {
            mz_zip_reader_end(&zip);
            errorMessage = "Failed to read '" + internalPath + "' from " + jcwfPath.filename().string();
            LOG_APP_ERROR("[jcwf] ReadFile: heap extract failed entry='{}' container='{}'",
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
