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

#include <fstream>
#include <chrono>
#include <iomanip>

#include "auxiliary/file.h"
#include "engine.h"
#include "session/fileWriter.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    FileWriter& FileWriter::Get()
    {
        static FileWriter instance;
        return instance;
    }

    void FileWriter::Write(fs::path const& filePath, std::string const& content)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        // Atomic write through the shared helper.  These files are the STNG /
        // CNTX / TASK / PROB inputs that AI dispatch reads as a completion
        // signal — a truncated partial would be parsed as malformed.  Helper
        // creates parent directories and renames the temp file in one step.
        std::string writeError;
        if (!EngineCore::AtomicWriteFile(filePath, content, writeError))
        {
            LOG_APP_ERROR("FileWriter::Write: {} (path='{}')", writeError, filePath.string());
            return;
        }
        LOG_APP_INFO("FileWriter: Wrote file '{}'", filePath.string());
    }

    void FileWriter::WriteWithHeader(fs::path const& filePath, std::string const& content, std::string const& model)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        std::string writeError;
        if (!EngineCore::AtomicWriteFile(filePath, content, writeError))
        {
            LOG_APP_ERROR("FileWriter::WriteWithHeader: {} (path='{}')", writeError, filePath.string());
            return;
        }
        LOG_APP_INFO("FileWriter: Wrote output file with header: {}", filePath.string());
    }
} // namespace AIAssistant
