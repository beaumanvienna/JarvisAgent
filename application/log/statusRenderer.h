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

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    class StatusRenderer
    {
    public:
        struct SessionStatus
        {
            std::string name;
            std::string state;
            size_t outputs{0};
            size_t inflight{0};
            size_t completed{0};

            size_t spinnerIndex{0};
            std::chrono::steady_clock::time_point lastSpinnerUpdate{std::chrono::steady_clock::now()};
        };

    public:
        StatusRenderer() = default;

        void UpdateSession(std::string const& name, std::string_view state, size_t outputs, size_t inflight,
                           size_t completed);

        void Start();
        void Stop();

        // Build human-readable status lines for the terminal status window.
        // maxColumns is the available width; implementation must be UTF-8 safe.
        void BuildStatusLines(std::vector<std::string>& outLines, int maxColumns);

        size_t GetSessionCount();

    private:
        std::mutex m_Mutex;
        std::unordered_map<std::string, SessionStatus> m_Sessions;
    };
} // namespace AIAssistant
