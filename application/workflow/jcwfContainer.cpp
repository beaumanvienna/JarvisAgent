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

#include <fstream>

#include "engine.h"
#include "miniz.h"

namespace AIAssistant
{
    bool JcwfContainer::Extract(std::filesystem::path const& jcwfPath, std::filesystem::path const& targetDir,
                                std::string& errorMessage)
    {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, jcwfPath.string().c_str(), 0))
        {
            errorMessage = "Failed to open zip: " + jcwfPath.string();
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(targetDir, ec);
        if (ec)
        {
            mz_zip_reader_end(&zip);
            errorMessage = "Failed to create directory: " + targetDir.string() + " (" + ec.message() + ")";
            return false;
        }

        mz_uint const numFiles = mz_zip_reader_get_num_files(&zip);

        for (mz_uint i = 0; i < numFiles; ++i)
        {
            char filename[1024];
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
                errorMessage = "Failed to extract '" + std::string(filename) + "' from " + jcwfPath.string();
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
            errorMessage = "Source is not a directory: " + sourceDir.string();
            return false;
        }

        mz_zip_archive zip{};
        if (!mz_zip_writer_init_file(&zip, jcwfPath.string().c_str(), 0))
        {
            errorMessage = "Failed to create zip: " + jcwfPath.string();
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
            errorMessage = "Failed to finalize zip: " + jcwfPath.string();
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
            errorMessage = "Failed to open zip: " + jcwfPath.string();
            return false;
        }

        size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, internalPath.c_str(), &size, 0);

        if (data == nullptr)
        {
            mz_zip_reader_end(&zip);
            errorMessage = "File '" + internalPath + "' not found in " + jcwfPath.string();
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
