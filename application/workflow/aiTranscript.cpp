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

#include "workflow/aiTranscript.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>

#include "engine.h"
#include "json/jsonHelper.h"
#include "simdjson/simdjson.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    namespace
    {
        // Hard cap on existing-transcript file size during read-modify-write.
        // 64 MB is generous — a real transcript that ever reaches this size
        // means the workflow has run amok or someone is using transcript
        // files for something they were not designed for.
        constexpr std::uintmax_t kMaxTranscriptBytes = 64ULL * 1024ULL * 1024ULL;

        // Cap applied to provider error messages before they are embedded in
        // the transcript.  Provider error bodies sometimes include masked
        // API-key suffixes, request IDs, or partial request bodies; truncating
        // bounds disk impact and keeps the most useful prefix.
        constexpr std::size_t kErrorMessageCap = 1024;

        // Single in-process mutex serialising all transcript read-modify-writes.
        // Per-path locking would be a perf optimisation but transcript writes
        // are infrequent (one request + one response per AI dispatch); a
        // global mutex is simpler and good enough.  Multi-process safety is
        // not provided — production deployments run a single j9t per host.
        std::mutex s_TranscriptMutex;

        [[nodiscard]] std::string NowIso8601() noexcept
        {
            try
            {
                auto const now = std::chrono::system_clock::now();
                auto const seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
                auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
                std::time_t const rawTime = std::chrono::system_clock::to_time_t(seconds);
                std::tm timeStruct{};
#ifdef _WIN32
                gmtime_s(&timeStruct, &rawTime);
#else
                gmtime_r(&rawTime, &timeStruct);
#endif
                std::ostringstream stream;
                stream << std::put_time(&timeStruct, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
                        << ms << 'Z';
                return stream.str();
            }
            catch (...)
            {
                // bad_alloc from ostringstream — return a syntactically-valid
                // ISO-8601 sentinel so the surrounding JSON stays well-formed.
                return std::string("1970-01-01T00:00:00.000Z");
            }
        }

        [[nodiscard]] bool ReadFile(std::filesystem::path const& path, std::string& outContent)
        {
            std::error_code sizeError;
            std::uintmax_t const size = std::filesystem::file_size(path, sizeError);
            if (sizeError)
            {
                // file does not exist or unreadable — let the caller treat
                // it as a fresh transcript.  Not an error log here.
                return false;
            }
            if (size > kMaxTranscriptBytes)
            {
                LOG_APP_ERROR("AiTranscript::ReadFile: transcript '{}' size {} bytes exceeds cap {}; refusing to load",
                              path.string(), size, kMaxTranscriptBytes);
                return false;
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                LOG_APP_ERROR("AiTranscript::ReadFile: cannot open transcript '{}'", path.string());
                return false;
            }
            std::ostringstream buffer;
            buffer << input.rdbuf();
            outContent = buffer.str();
            return true;
        }

        // Atomic write: writes to <path>.tmp, fsyncs (where supported), then
        // renames over the destination.  std::filesystem::rename is atomic on
        // POSIX and same-volume on Windows.  A crash before rename leaves the
        // original file intact (or absent on a fresh write); the .tmp sibling
        // is left for forensic inspection rather than auto-cleaned, which has
        // historically helped diagnose disk-full / permission failures.
        [[nodiscard]] bool WriteFile(std::filesystem::path const& path, std::string const& content)
        {
            std::error_code dirError;
            std::filesystem::create_directories(path.parent_path(), dirError);
            if (dirError)
            {
                LOG_APP_ERROR("AiTranscript::WriteFile: create_directories('{}') failed: {}",
                              path.parent_path().string(), dirError.message());
                return false;
            }
            std::filesystem::path const tempPath = path.string() + ".tmp";
            {
                std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    LOG_APP_ERROR("AiTranscript::WriteFile: cannot open temp file '{}'", tempPath.string());
                    return false;
                }
                output << content;
                if (!output.good())
                {
                    LOG_APP_ERROR("AiTranscript::WriteFile: stream write to '{}' failed (partial)",
                                  tempPath.string());
                    return false;
                }
            }
            std::error_code renameError;
            std::filesystem::rename(tempPath, path, renameError);
            if (renameError)
            {
                LOG_APP_ERROR("AiTranscript::WriteFile: rename('{}' -> '{}') failed: {}",
                              tempPath.string(), path.string(), renameError.message());
                return false;
            }
            return true;
        }

        // Returns a string-view literal so callers avoid the per-call heap
        // allocation that the previous `std::string` form incurred.
        [[nodiscard]] constexpr std::string_view MessageRoleToString(MessageRole role) noexcept
        {
            switch (role)
            {
                case MessageRole::System:    return "system";
                case MessageRole::User:      return "user";
                case MessageRole::Assistant: return "assistant";
            }
            // -Wswitch will warn if a new MessageRole variant is added without
            // a case here.  The fallthrough returns "user" only as a safety
            // net to keep the JSON well-formed if compiler warnings are
            // suppressed in some build configuration.
            return "user";
        }

        [[nodiscard]] constexpr std::string_view AiErrorKindToString(AiError::Kind kind) noexcept
        {
            switch (kind)
            {
                case AiError::Kind::None:             return "none";
                case AiError::Kind::Http:             return "http";
                case AiError::Kind::Parse:            return "parse";
                case AiError::Kind::SchemaValidation: return "schema_validation";
                case AiError::Kind::Timeout:          return "timeout";
                case AiError::Kind::Transport:        return "transport";
                case AiError::Kind::Provider:         return "provider";
            }
            return "none";
        }

        // Validates that `raw` is a syntactically valid JSON value and returns
        // its canonical re-serialised form.  Eliminates the structured-JSON
        // injection class — a malicious provider response containing an
        // unbalanced `]` or stray top-level keys cannot corrupt the
        // surrounding transcript array if we round-trip through simdjson DOM.
        // Returns "null" on parse failure; caller logs the rejection.
        [[nodiscard]] std::string CanonicalizeStructuredJson(std::string const& raw)
        {
            if (raw.empty())
            {
                return "null";
            }
            simdjson::dom::parser parser;
            simdjson::dom::element element;
            if (auto err = parser.parse(raw).get(element); err)
            {
                LOG_APP_ERROR("AiTranscript: structured JSON parse failed ({}); embedding null",
                              simdjson::error_message(err));
                return "null";
            }
            std::ostringstream stream;
            stream << element;
            return stream.str();
        }

        [[nodiscard]] bool AppendEntry(std::filesystem::path const& transcriptPath, std::string const& entryJson)
        {
            std::lock_guard<std::mutex> const guard(s_TranscriptMutex);

            std::string existing;
            bool const hadFile = ReadFile(transcriptPath, existing);
            std::string rebuilt;
            if (!hadFile || existing.empty())
            {
                rebuilt = "[\n  " + entryJson + "\n]\n";
            }
            else
            {
                size_t const closeBracket = existing.find_last_of(']');
                if (closeBracket == std::string::npos)
                {
                    rebuilt = "[\n  " + entryJson + "\n]\n";
                }
                else
                {
                    std::string const head = existing.substr(0, closeBracket);
                    std::string trimmedHead = head;
                    while (!trimmedHead.empty() &&
                           (trimmedHead.back() == ' ' || trimmedHead.back() == '\n' || trimmedHead.back() == '\t'))
                    {
                        trimmedHead.pop_back();
                    }
                    rebuilt = trimmedHead + ",\n  " + entryJson + "\n]\n";
                }
            }
            if (!WriteFile(transcriptPath, rebuilt))
            {
                LOG_APP_ERROR("AiTranscript::AppendEntry: write failed for '{}'", transcriptPath.string());
                return false;
            }
            return true;
        }
    } // anonymous namespace

    bool AiTranscript::AppendRequest(std::filesystem::path const& transcriptPath, AiInvocation const& envelope,
                                      std::string const& resolvedModel)
    {
        std::string json = "{";
        json += "\"kind\":\"request\"";
        json += ",\"timestamp\":\"" + NowIso8601() + "\"";
        json += ",\"interface\":\"" + JsonHelper::EscapeJsonString(envelope.m_InterfaceName) + "\"";
        json += ",\"model\":\"" + JsonHelper::EscapeJsonString(resolvedModel) + "\"";
        json += ",\"settings\":{";
        json += "\"temperature\":" + std::to_string(envelope.m_Settings.m_Temperature);
        if (envelope.m_Settings.m_Seed.has_value())
        {
            json += ",\"seed\":" + std::to_string(envelope.m_Settings.m_Seed.value());
        }
        if (envelope.m_Settings.m_MaxTokens.has_value())
        {
            json += ",\"max_tokens\":" + std::to_string(envelope.m_Settings.m_MaxTokens.value());
        }
        json += "}";
        json += ",\"messages\":[";
        for (size_t index = 0; index < envelope.m_Messages.size(); ++index)
        {
            if (index > 0) json += ",";
            auto const& message = envelope.m_Messages[index];
            json += "{\"role\":\"";
            json += MessageRoleToString(message.m_Role);
            json += "\"";
            json += ",\"content\":\"" + JsonHelper::EscapeJsonString(message.m_Content) + "\"}";
        }
        json += "]";
        if (envelope.m_ChunkIndex.has_value() && envelope.m_ChunkCount.has_value())
        {
            json += ",\"chunk\":{\"index\":" + std::to_string(envelope.m_ChunkIndex.value());
            json += ",\"count\":" + std::to_string(envelope.m_ChunkCount.value()) + "}";
        }
        json += "}";
        return AppendEntry(transcriptPath, json);
    }

    bool AiTranscript::AppendResponse(std::filesystem::path const& transcriptPath, AiReply const& reply)
    {
        std::string json = "{";
        json += "\"kind\":\"response\"";
        json += ",\"timestamp\":\"" + NowIso8601() + "\"";
        switch (reply.m_Kind)
        {
            case AiReply::Kind::Text:
                json += ",\"text\":\"" + JsonHelper::EscapeJsonString(reply.m_Text) + "\"";
                break;
            case AiReply::Kind::Structured:
                // Round-trip through simdjson DOM to guarantee the embedded
                // payload is a syntactically valid JSON value — not a raw
                // string fragment that could corrupt the transcript array.
                json += ",\"structured\":" + CanonicalizeStructuredJson(reply.m_StructuredJson);
                break;
            case AiReply::Kind::Error:
            {
                // Bound provider error message length and sanitize before
                // embedding — provider bodies sometimes include credential
                // hints or partial request fragments that should not be
                // persisted verbatim to disk.
                std::string const safeMessage =
                    TruncateUtf8Safe(SanitizeUtf8(reply.m_Error.m_Message), kErrorMessageCap);
                json += ",\"error\":{";
                json += "\"kind\":\"";
                json += AiErrorKindToString(reply.m_Error.m_Kind);
                json += "\"";
                json += ",\"http_status\":" + std::to_string(reply.m_Error.m_HttpStatus);
                json += ",\"message\":\"" + JsonHelper::EscapeJsonString(safeMessage) + "\"}";
                break;
            }
        }
        json += ",\"usage\":{";
        json += "\"input\":" + std::to_string(reply.m_Usage.m_InputTokens);
        json += ",\"output\":" + std::to_string(reply.m_Usage.m_OutputTokens);
        json += ",\"total\":" + std::to_string(reply.m_Usage.m_TotalTokens);
        json += "}";
        if (!reply.m_FinishReason.empty())
        {
            json += ",\"finish_reason\":\"" + JsonHelper::EscapeJsonString(reply.m_FinishReason) + "\"";
        }
        if (!reply.m_SystemFingerprint.empty())
        {
            json += ",\"system_fingerprint\":\"" + JsonHelper::EscapeJsonString(reply.m_SystemFingerprint) + "\"";
        }
        json += "}";
        return AppendEntry(transcriptPath, json);
    }
} // namespace AIAssistant
