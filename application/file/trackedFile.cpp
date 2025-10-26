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

#include "trackedFile.h"
#include "log/log.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h> // if available, otherwise use std::hash fallback

#include "engine.h"

namespace AIAssistant
{
    TrackedFile::TrackedFile(fs::path const& path, FileCategory fileCategory) : m_Path(path), m_FileCategory{fileCategory}
    {
        m_LastHash = ComputeFileHash();
        m_Modified.store(true);
    }

    std::optional<std::string> TrackedFile::GetContentAndResetModified()
    {
        std::lock_guard lock(m_Mutex);
        std::ifstream file(m_Path);
        if (!file.is_open())
        {
            LOG_APP_WARN("Failed to open file for reading: {}", m_Path.string());
            return std::nullopt;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        m_Modified.store(false);
        return buffer.str();
    }

    bool TrackedFile::CheckIfContentChanged()
    {
        std::lock_guard lock(m_Mutex);

        std::string newHash = ComputeFileHash();
        if (newHash != m_LastHash)
        {
            m_LastHash = newHash;
            m_Modified.store(true);
            return true;
        }
        return false;
    }

    FileCategory TrackedFile::GetCategory() const { return m_FileCategory; }

    std::string TrackedFile::ComputeFileHash() const
    {
        std::ifstream file(m_Path, std::ios::binary);
        if (!file.is_open())
        {
            return {};
        }

        std::ostringstream oss;
        oss << file.rdbuf();
        std::string data = oss.str();

#if __has_include(<openssl/sha.h>)
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

        std::ostringstream hex;
        for (unsigned char c : hash)
        {
            hex << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }

        return hex.str();
#else
        // fallback: lightweight std::hash
        return std::to_string(std::hash<std::string>{}(data));
#endif
    }
} // namespace AIAssistant
