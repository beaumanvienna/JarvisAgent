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

#include "log/statusLineRenderer.h"

namespace AIAssistant
{
    void StatusLineRenderer::Update(std::string_view state, size_t outputs, size_t inflight, size_t completed)
    {
        if (!isatty(STDOUT_FILENO))
        {
            return; // disable dynamic terminal rendering when output is redirected
        }

        static const auto spinner =
            std::to_array({"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷", "⠁", "⠂", "⠄", "⡀", "⢀", "⠠", "⠐", "⠈"});

        std::string_view color = (state == "AllResponsesReceived")   ? "\033[32m" /*green*/
                                 : (state == "SendingQueries")       ? "\033[33m" /*yellow*/
                                 : (state == "CompilingEnvironment") ? "\033[36m" /*cyan*/
                                                                     : "\033[35m" /*magenta*/;

        using namespace std::chrono_literals;

        // Handle spinner timing — full cycle in 1.6 seconds (16 frames × 100 ms)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = now - m_LastSpinnerUpdate;

        if (elapsed >= 100ms) // advance every 100 ms
        {
            m_LastSpinnerUpdate = now;
            if (inflight > 0)
            {
                m_SpinnerIndex = (m_SpinnerIndex + 1) % spinner.size();
            }
        }

        // yellow spinner when queries are active
        std::string spinnerChar = inflight > 0 ? "\033[33m" + std::string(spinner[m_SpinnerIndex]) + "\033[0m   " : "    ";

        std::string line = std::string(color) + "STATE: " + std::string(state) + "\033[0m" +
                           " | Outputs: " + std::to_string(outputs) + " | In flight: " + std::to_string(inflight) +
                           " | Completed: " + std::to_string(completed) + " " + spinnerChar;

        std::lock_guard<std::mutex> lock(m_Mutex);
        std::cout << "\0337"                         // Save cursor position
                  << "\033[999B"                     // Move to bottom
                  << "\033[2K\r"                     // Clear line
                  << line << std::flush << "\033[u"; // Restore cursor
    }
} // namespace AIAssistant
