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

#include "jarvisAgent.h"
#include "file/probUtils.h"
#include "file/fileCategorizer.h"
#include "auxiliary/file.h"
#include <algorithm>
#include <iostream>

namespace AIAssistant
{
    FileCategorizer::~FileCategorizer() {}

    fs::path const FileCategorizer::AddFile(fs::path const& filePath)
    {
        FileCategory category = Categorize(filePath);
        auto& categoryMap = [&]() -> TrackedFiles&
        {
            switch (category)
            {
                case FileCategory::Settings:
                    return m_CategorizedFiles.m_Settings;
                case FileCategory::Context:
                    return m_CategorizedFiles.m_Context;
                case FileCategory::Task:
                    return m_CategorizedFiles.m_Tasks;
                case FileCategory::Requirement:
                    return m_CategorizedFiles.m_Requirements;
                case FileCategory::SubFolder:
                    return m_CategorizedFiles.m_Subfolders;
                case FileCategory::Ignored:
                    return m_CategorizedFiles.m_Ignored;
                default:
                    return m_CategorizedFiles.m_Requirements; // fallback
            }
        }();

        std::string key = filePath.string();
        categoryMap.Write()[key] = std::make_unique<TrackedFile>(filePath, category); // constructs and marks modified = true
        categoryMap.IncrementModifiedFiles();
        return filePath;
    }

    fs::path const FileCategorizer::RemoveFile(fs::path const& filePath)
    {
        RemoveFromFiles(m_CategorizedFiles.m_Settings, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Context, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Tasks, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Requirements, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Subfolders, filePath);
        RemoveFromFiles(m_CategorizedFiles.m_Ignored, filePath);
        return filePath;
    }

    fs::path const FileCategorizer::ModifyFile(fs::path const& filePath)
    {
        FileCategory category = Categorize(filePath);
        if (category == FileCategory::Ignored)
        {
            return {};
        }
        auto& categoryMap = [&]() -> TrackedFiles&
        {
            switch (category)
            {
                case FileCategory::Settings:
                    return m_CategorizedFiles.m_Settings;
                case FileCategory::Context:
                    return m_CategorizedFiles.m_Context;
                case FileCategory::Task:
                    return m_CategorizedFiles.m_Tasks;
                case FileCategory::Requirement:
                    return m_CategorizedFiles.m_Requirements;
                case FileCategory::SubFolder:
                    return m_CategorizedFiles.m_Subfolders;
                case FileCategory::Ignored:
                    return m_CategorizedFiles.m_Ignored;
                default:
                    return m_CategorizedFiles.m_Context; // fallback
            }
        }();

        auto& map = categoryMap.Get();
        auto it = map.find(filePath.string());
        if (it != map.end())
        {
            if (it->second->CheckIfContentChanged())
            {
                if (!it->second->IsModified())
                {
                    it->second->MarkModified();
                    categoryMap.IncrementModifiedFiles();
                }
                categoryMap.SetDirty();
                LOG_APP_INFO("FileCategorizer::ModifyFile: Modified file: {}", filePath.string());
            }
        }
        else
        {
            LOG_APP_CRITICAL("File not tracked yet (could be newly added)");
        }
        return filePath;
    }

    void FileCategorizer::RemoveFromFiles(TrackedFiles& files, fs::path const& path)
    {
        auto& map = files.Get();
        auto it = map.find(path.string());
        if (it != map.end())
        {
            if (it->second->IsModified())
            {
                files.DecrementModifiedFiles();
            }
            map.erase(it);
            files.SetDirty();
            LOG_APP_INFO("Removed file: {}", path.string());
        }
    }

    FileCategory FileCategorizer::Categorize(fs::path const& filePath) const
    {
        std::string filename = filePath.filename().string();

        if (EngineCore::IsDirectory(filePath))
        {
            return FileCategory::SubFolder;
        }

        if (filePath.stem().string().ends_with(".output"))
        {
            LOG_APP_INFO("Ignoring output file: {}", filePath.string());
            return FileCategory::Ignored;
        }

        if (filename.starts_with("STNG"))
        {
            return FileCategory::Settings;
        }

        if (filename.starts_with("CNTX"))
        {
            return FileCategory::Context;
        }

        if (filename.starts_with("TASK"))
        {
            return FileCategory::Task;
        }

        // --- Detect stale PROB files (input or output) ---
        {
            std::optional<ProbUtils::ProbFileInfo> probFileInfoOpt = ProbUtils::ParseProbFilename(filename);

            if (probFileInfoOpt.has_value())
            {
                // Stale PROB files should be ignored
                const ProbUtils::ProbFileInfo& probInfo = probFileInfoOpt.value();
                int64_t startupTimestamp = App::g_App->GetStartupTimestamp();

                if (probInfo.timestamp < startupTimestamp)
                {
                    // Silent ignore — PROB file created before app started
                    return FileCategory::Ignored;
                }

                // Non-stale PROB files should always be processed
                // They are treated as requirements (SessionManager gets them)
                return FileCategory::Requirement;
            }
        }

        // --- Quick magic-number check for common binary formats ---
        {
            std::ifstream f(filePath, std::ios::binary);
            if (!f)
            {
                LOG_APP_WARN("Could not open file for content check: {}", filePath.string());
                return FileCategory::Ignored;
            }

            std::array<unsigned char, 8> header{};
            f.read(reinterpret_cast<char*>(header.data()), header.size());
            size_t n = static_cast<size_t>(f.gcount());

            // Define a small lambda for header comparison
            auto match = [&](std::initializer_list<unsigned char> sig)
            { return n >= sig.size() && std::equal(sig.begin(), sig.end(), header.begin()); };

            if (match({0x50, 0x4B, 0x03, 0x04}) || // ZIP / DOCX / XLSX / ODT
                match({0x89, 0x50, 0x4E, 0x47}) || // PNG
                match({0x25, 0x50, 0x44, 0x46}) || // PDF
                match({0xFF, 0xD8, 0xFF}) ||       // JPEG
                match({0x47, 0x49, 0x46, 0x38}) || // GIF
                match({0x42, 0x4D}) ||             // BMP
                match({0x7F, 0x45, 0x4C, 0x46}) || // ELF binary
                match({0x4D, 0x5A})                // Windows PE (EXE/DLL)
            )
            {
                LOG_APP_INFO("Ignoring known binary type (magic number match): {}", filePath.string());
                return FileCategory::Ignored;
            }
        }

        // --- Check file readability (is it likely text?) ---
        {
            std::ifstream file(filePath, std::ios::binary);
            if (!file)
            {
                LOG_APP_WARN("Could not open file for content check: {}", filePath.string());
                return FileCategory::Ignored;
            }

            constexpr size_t kSampleSize = 256;
            char buffer[kSampleSize]{};
            file.read(buffer, kSampleSize);
            std::streamsize bytesRead = file.gcount();

            // If the file is empty, ignore it
            if (bytesRead == 0)
            {
                LOG_APP_INFO("Ignoring empty file: {}", filePath.string());
                return FileCategory::Ignored;
            }

            size_t nonTextCount = 0;
            for (std::streamsize i = 0; i < bytesRead; ++i)
            {
                unsigned char c = static_cast<unsigned char>(buffer[i]);
                // Allow printable ASCII + common whitespace (tab/newline/carriage return)
                if ((c < 0x09) || (c > 0x0D && c < 0x20) || // other control chars
                    (c == 0x7F))
                {
                    ++nonTextCount;
                }
            }

            double nonTextRatio = static_cast<double>(nonTextCount) / static_cast<double>(bytesRead);
            if (nonTextRatio > 0.1) // >10% non-printable = treat as binary
            {
                LOG_APP_INFO("Ignoring binary file (non-text ratio {:.1f}%): {}", nonTextRatio * 100.0, filePath.string());
                return FileCategory::Ignored;
            }
        }

        // --- Hard limit for oversized files ---
        {
            std::error_code errorCode;
            auto fileSize = fs::file_size(filePath, errorCode);

            size_t fileSizeLimit = Core::g_Core->GetConfig().m_MaxFileSizekB;

            if (!errorCode && fileSize > fileSizeLimit * 1024)
            {
                // Create .output.txt message
                fs::path outputPath = filePath;
                outputPath += ".output.txt";

                std::ofstream out(outputPath);
                if (out)
                {
                    out << "File '" << filePath.filename().string() << "' is too large (" << fileSize
                        << " bytes). Maximum allowed size is " << fileSizeLimit << " kB.\n"
                        << "Processing was skipped.\n";
                }
                else
                {
                    LOG_APP_ERROR("Failed to write oversized-file output: {}", outputPath.string());
                }

                LOG_APP_WARN("Ignoring oversized file: {} ({} bytes)", filePath.string(), fileSize);
                return FileCategory::Ignored;
            }
        }

        // anything else is considered a requirement
        return FileCategory::Requirement;
    }

    void FileCategorizer::PrintCategorizedFiles() const
    {
        auto printCategory = [](std::string const& category, TrackedFiles const& files)
        {
            auto const& map = files.m_Map;
            if (!map.empty())
            {
                std::cout << category << ":\n";
                for (auto const& file : map)
                {
                    std::cout << "  " << file.second->GetPath().string() << "\n";
                }
            }
        };

        std::cout << "=== FileCategorizer: Tracked Files ===\n";
        printCategory("Settings", m_CategorizedFiles.m_Settings);
        printCategory("Context", m_CategorizedFiles.m_Context);
        printCategory("Tasks", m_CategorizedFiles.m_Tasks);
        printCategory("Requirements", m_CategorizedFiles.m_Requirements);
        printCategory("Subfolders", m_CategorizedFiles.m_Subfolders);
        printCategory("Ignored", m_CategorizedFiles.m_Ignored);
        std::cout << "=== End of Tracked Files ===\n";
    }

    void TrackedFiles::IncrementModifiedFiles() { ++m_ModifiedFiles; }

    void TrackedFiles::DecrementModifiedFiles()
    {
        CORE_ASSERT(m_ModifiedFiles, "m_ModifiedFiles overflow");
        --m_ModifiedFiles;
    }

} // namespace AIAssistant
