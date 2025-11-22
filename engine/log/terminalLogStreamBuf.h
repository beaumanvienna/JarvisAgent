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

#pragma once

#include <fstream>
#include <mutex>
#include <streambuf>
#include <string>

#include "log/terminalManager.h"

namespace AIAssistant
{
    class TerminalLogStreamBuf : public std::streambuf
    {
    public:
        TerminalLogStreamBuf(TerminalManager* terminalManager, std::shared_ptr<std::ofstream> fileLogger)
            : m_TerminalManager(terminalManager), m_FileLogger(std::move(fileLogger))
        {
        }

    protected:
        int sync() override
        {
            if (!m_Buffer.empty())
            {
                if (m_TerminalManager != nullptr)
                {
                    m_TerminalManager->EnqueueLogLine(m_Buffer);
                }

                if (m_FileLogger && m_FileLogger->is_open())
                {
                    std::lock_guard<std::mutex> lock(m_FileMutex);
                    (*m_FileLogger) << m_Buffer << "\n";
                    m_FileLogger->flush();
                }

                m_Buffer.clear();
            }

            return 0;
        }

        int overflow(int character) override
        {
            if (character == traits_type::eof())
            {
                return traits_type::not_eof(character);
            }

            char characterValue = static_cast<char>(character);

            if (characterValue == '\n')
            {
                sync();
            }
            else
            {
                m_Buffer.push_back(characterValue);
            }

            return character;
        }

        std::streamsize xsputn(char const* data, std::streamsize count) override
        {
            for (std::streamsize index = 0; index < count; ++index)
            {
                overflow(static_cast<unsigned char>(data[index]));
            }
            return count;
        }

    private:
        TerminalManager* m_TerminalManager;
        std::string m_Buffer;

        std::shared_ptr<std::ofstream> m_FileLogger;
        std::mutex m_FileMutex;
    };
} // namespace AIAssistant
