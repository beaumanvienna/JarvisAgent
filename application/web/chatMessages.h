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
#include <string>
#include <cstdint>
#include <chrono>
#include <queue>
#include <mutex>
#include <atomic>

namespace AIAssistant
{
    struct ChatMessageEntry
    {
        uint64_t id;
        std::string subsystem;
        std::string message;
        std::chrono::steady_clock::time_point timestamp;
        bool answered = false;
        bool expired = false;
    };

    class ChatMessagePool
    {
    public:
        ChatMessagePool(size_t initialSize = 100, double growThreshold = 0.7);

        uint64_t AddMessage(std::string const& subsystem, std::string const& message);
        void MarkAnswered(uint64_t id, std::string const& answerText);
        void RemoveExpired();
        void Update(); // called periodically to remove expired entries

        size_t Size() const { return m_Entries.size(); }
        size_t ActiveCount() const { return m_ActiveCount; }

    private:
        void GrowPool();

    private:
        std::vector<ChatMessageEntry> m_Entries;
        std::queue<size_t> m_FreeIndices;
        std::atomic<uint64_t> m_NextId{1};
        std::mutex m_Mutex;
        size_t m_ActiveCount{0};
        double m_GrowThreshold{0.7};
    };

} // namespace AIAssistant
