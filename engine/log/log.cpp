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

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "log/log.h"
#include "log/secretRedactor.h"

#include <spdlog/formatter.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace AIAssistant
{
    namespace
    {
        // Wraps an inner spdlog formatter and scrubs registered secrets from
        // every message payload before the inner formatter sees it.  Without
        // this wrapper, SecretRedactor::AddSecret() registrations are inert —
        // the redactor's Redact() method is otherwise never called.
        //
        // Hot-path optimisation: if no secrets are registered, skip the copy
        // and forward straight to the inner formatter.  Mutex inside Redact()
        // serialises but spdlog already serialises per-sink, so this adds
        // bounded contention only when secrets are actually present.
        class RedactingFormatter : public spdlog::formatter
        {
        public:
            explicit RedactingFormatter(std::unique_ptr<spdlog::formatter> inner) : m_Inner(std::move(inner)) {}

            void format(spdlog::details::log_msg const& msg, spdlog::memory_buf_t& dest) override
            {
                auto& redactor = SecretRedactor::Get();
                if (!redactor.HasSecrets())
                {
                    m_Inner->format(msg, dest);
                    return;
                }

                // Redact the payload, then forward a copy of the log_msg
                // pointing at the redacted text.  The redacted std::string
                // lives until the end of this call; the inner formatter
                // writes to dest synchronously, so no lifetime issue.
                std::string const redacted = redactor.Redact(
                    std::string(msg.payload.data(), msg.payload.size()));
                spdlog::details::log_msg redactedMsg = msg;
                redactedMsg.payload = spdlog::string_view_t(redacted.data(), redacted.size());
                m_Inner->format(redactedMsg, dest);
            }

            std::unique_ptr<spdlog::formatter> clone() const override
            {
                return std::make_unique<RedactingFormatter>(m_Inner->clone());
            }

        private:
            std::unique_ptr<spdlog::formatter> m_Inner;
        };

        std::unique_ptr<spdlog::formatter> MakeRedactingFormatter(std::string pattern)
        {
            auto inner = std::make_unique<spdlog::pattern_formatter>(std::move(pattern));
            return std::make_unique<RedactingFormatter>(std::move(inner));
        }
    } // namespace

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

        // Use a redacting formatter (wraps the pattern formatter) so registered
        // secrets are scrubbed from message bodies before pattern substitution.
        // Pattern: "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v"
        // (no color codes — ncurses hates them.)
        ostreamSink->set_formatter(MakeRedactingFormatter("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v"));

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

        // ============================================================
        // SECURITY LOGGER
        // Writes to both the ostream (TUI/console) and a dedicated
        // rotating file: log/security.txt (10 MB x 5 files).
        // ============================================================
        std::filesystem::create_directories("log");

        std::vector<spdlog::sink_ptr> securitySinks;
        securitySinks.emplace_back(ostreamSink);
        securitySinks.emplace_back(
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>("log/security.txt", 10 * 1024 * 1024, 5));
        securitySinks.back()->set_formatter(
            MakeRedactingFormatter("[%Y-%m-%d %H:%M:%S.%e] [Security] [%l] %v"));

        m_SecurityLogger = std::make_shared<spdlog::logger>("Security", begin(securitySinks), end(securitySinks));
        if (m_SecurityLogger)
        {
            spdlog::register_logger(m_SecurityLogger);
            m_SecurityLogger->set_level(spdlog::level::trace);
            m_SecurityLogger->flush_on(spdlog::level::trace);
        }
    }
} // namespace AIAssistant
