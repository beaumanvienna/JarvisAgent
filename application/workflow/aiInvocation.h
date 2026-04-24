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

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace AIAssistant
{
    enum class MessageRole
    {
        System,
        User,
        Assistant
    };

    struct Message
    {
        MessageRole m_Role = MessageRole::User;
        std::string m_Content;
    };

    struct AiSettings
    {
        double m_Temperature = 0.0;
        std::optional<int64_t> m_Seed;
        std::optional<int32_t> m_MaxTokens;
    };

    struct AiRetryPolicy
    {
        int m_HttpMaxAttempts = 3;
        int m_OutputSchemaMaxAttempts = 3;
        int m_TotalMaxAttempts = 10;
        std::chrono::milliseconds m_BackoffMs{500};
    };

    enum class StructuredMode
    {
        None,
        NativeJsonSchema,
        ForcedToolShim
    };

    struct AiInvocation
    {
        std::string m_InterfaceName;
        std::optional<std::string> m_ModelOverride;
        AiSettings m_Settings;
        std::vector<Message> m_Messages;
        std::optional<std::string> m_OutputSchemaJson;
        StructuredMode m_StructuredMode = StructuredMode::None;
        std::chrono::milliseconds m_Timeout{120000};
        AiRetryPolicy m_Retry;
        std::filesystem::path m_QueueFolder;
        std::string m_ProbName;
        std::string m_ProvName;
        std::optional<int32_t> m_ChunkIndex;
        std::optional<int32_t> m_ChunkCount;

        // Transient: incremented by AiRequestPool::Submit when re-dispatching after a
        // schema validation failure.  Callers leave this at 0.
        int m_SchemaAttemptNumber = 0;
    };
} // namespace AIAssistant
