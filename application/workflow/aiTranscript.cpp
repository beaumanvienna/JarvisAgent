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
#include <sstream>
#include <system_error>

#include "engine.h"
#include "json/jsonHelper.h"

namespace AIAssistant
{
    namespace
    {
        std::string NowIso8601()
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
            stream << std::put_time(&timeStruct, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms
                    << 'Z';
            return stream.str();
        }

        bool ReadFile(std::filesystem::path const& path, std::string& outContent)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return false;
            }
            std::ostringstream buffer;
            buffer << input.rdbuf();
            outContent = buffer.str();
            return true;
        }

        bool WriteFile(std::filesystem::path const& path, std::string const& content)
        {
            std::error_code errorCode;
            std::filesystem::create_directories(path.parent_path(), errorCode);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return false;
            }
            output << content;
            return output.good();
        }

        std::string MessageRoleToString(MessageRole role)
        {
            switch (role)
            {
                case MessageRole::System:    return "system";
                case MessageRole::Assistant: return "assistant";
                case MessageRole::User:
                default:                     return "user";
            }
        }

        bool AppendEntry(std::filesystem::path const& transcriptPath, std::string const& entryJson)
        {
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
            return WriteFile(transcriptPath, rebuilt);
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
            json += "{\"role\":\"" + MessageRoleToString(message.m_Role) + "\"";
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
                json += ",\"structured\":" + (reply.m_StructuredJson.empty() ? std::string("null") : reply.m_StructuredJson);
                break;
            case AiReply::Kind::Error:
            default:
                json += ",\"error\":{";
                json += "\"kind\":\"" + JsonHelper::EscapeJsonString([&]()
                {
                    switch (reply.m_Error.m_Kind)
                    {
                        case AiError::Kind::Http:             return "http";
                        case AiError::Kind::Parse:            return "parse";
                        case AiError::Kind::SchemaValidation: return "schema_validation";
                        case AiError::Kind::Timeout:          return "timeout";
                        case AiError::Kind::Transport:        return "transport";
                        case AiError::Kind::Provider:         return "provider";
                        case AiError::Kind::None:
                        default:                              return "none";
                    }
                }()) + "\"";
                json += ",\"http_status\":" + std::to_string(reply.m_Error.m_HttpStatus);
                json += ",\"message\":\"" + JsonHelper::EscapeJsonString(reply.m_Error.m_Message) + "\"}";
                break;
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
