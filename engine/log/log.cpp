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

#include <iostream>
#include <vector>

#include "log/log.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace AIAssistant
{
    Log::Log()
    {
        std::vector<spdlog::sink_ptr> logSink;

        // ------------------------------------------------------------
        // IMPORTANT:
        // Use an ostream sink writing into std::cout.
        // Later, JarvisAgent::OnStart() redirects std::cout.rdbuf()
        // to TerminalLogStreamBuf, so ALL logs go through ncurses.
        // ------------------------------------------------------------
        auto ostreamSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(std::cout, /*force_flush=*/true);

        // Optional: adjust the pattern (remove color codes, because ncurses hates them)
        ostreamSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");

        logSink.emplace_back(ostreamSink);

        // ============================================================
        // ENGINE LOGGER
        // ============================================================
        m_Logger = std::make_shared<spdlog::logger>("Engine", begin(logSink), end(logSink));
        if (m_Logger)
        {
            spdlog::register_logger(m_Logger);
            m_Logger->set_level(spdlog::level::trace);
            m_Logger->flush_on(spdlog::level::trace);
        }

        // ============================================================
        // APPLICATION LOGGER
        // ============================================================
        m_AppLogger = std::make_shared<spdlog::logger>("Application", begin(logSink), end(logSink));
        if (m_AppLogger)
        {
            spdlog::register_logger(m_AppLogger);
            m_AppLogger->set_level(spdlog::level::trace);
            m_AppLogger->flush_on(spdlog::level::trace);
        }
    }
} // namespace AIAssistant
