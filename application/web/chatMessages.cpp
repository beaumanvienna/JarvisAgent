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

#include "engine.h"
#include "jarvisAgent.h"
#include "chatMessages.h"
#include "webServer.h"

namespace AIAssistant
{

    ChatMessagePool::ChatMessagePool(size_t initialSize, double growThreshold) : m_GrowThreshold(growThreshold)
    {
        m_Entries.resize(initialSize);

        for (size_t i = 0; i < initialSize; ++i)
        {
            m_FreeIndices.push(i);
        }

        LOG_APP_INFO("ChatMessagePool initialized with {} entries", initialSize);
    }

    void ChatMessagePool::Update() { RemoveExpired(); }

    uint64_t ChatMessagePool::AddMessage(std::string const& subsystem, std::string const& message)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_FreeIndices.empty())
        {
            if (static_cast<double>(m_ActiveCount) / m_Entries.size() >= m_GrowThreshold)
            {
                GrowPool();
            }
        }

        size_t index = 0;
        if (!m_FreeIndices.empty())
        {
            index = m_FreeIndices.front();
            m_FreeIndices.pop();
        }

        ChatMessageEntry& entry = m_Entries[index];
        entry.id = m_NextId++;
        entry.subsystem = subsystem;
        entry.message = message;
        entry.timestamp = std::chrono::steady_clock::now();
        entry.answered = false;
        entry.expired = false;

        ++m_ActiveCount;
        return entry.id;
    }

    void ChatMessagePool::GrowPool()
    {
        size_t oldSize = m_Entries.size();
        size_t newSize = oldSize * 2;

        m_Entries.resize(newSize);
        for (size_t i = oldSize; i < newSize; ++i)
        {
            m_FreeIndices.push(i);
        }

        LOG_APP_INFO("ChatMessagePool expanded from {} to {} entries", oldSize, newSize);
    }

    void ChatMessagePool::MarkAnswered(uint64_t id, std::string const& answerText)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            ChatMessageEntry& entry = m_Entries[i];

            // CASE 1 — Normal answer
            if (entry.id == id && !entry.expired && !entry.answered)
            {
                entry.answered = true;

                crow::json::wvalue msg;
                msg["type"] = "output";
                msg["id"] = id;
                msg["text"] = answerText;
                App::g_App->GetWebServer()->BroadcastJSON(msg.dump());

                // Free entry
                entry = ChatMessageEntry{};
                m_FreeIndices.push(i);
                --m_ActiveCount;
                return;
            }
        }

        // CASE 2 — Late answer: message expired earlier
        LOG_APP_WARN("Late answer received for expired ChatMessage {}", id);

        crow::json::wvalue msg;
        msg["type"] = "late-answer";
        msg["id"] = id;
        msg["text"] = answerText;
        App::g_App->GetWebServer()->BroadcastJSON(msg.dump());
    }

    void ChatMessagePool::RemoveExpired()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(30);

        std::lock_guard<std::mutex> lock(m_Mutex);

        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            ChatMessageEntry& entry = m_Entries[i];
            if (entry.id != 0 && !entry.answered)
            {
                if (now - entry.timestamp > timeout)
                {
                    LOG_APP_WARN("ChatMessage {} expired", entry.id);

                    // inform browser about timeout
                    crow::json::wvalue msg;
                    msg["type"] = "timeout";
                    msg["id"] = entry.id;
                    msg["text"] = "Message expired after 30 seconds.";
                    App::g_App->GetWebServer()->BroadcastJSON(msg.dump());

                    // Reset entry
                    entry = ChatMessageEntry{};
                    m_FreeIndices.push(i);
                    --m_ActiveCount;
                }
            }
        }
    }

} // namespace AIAssistant