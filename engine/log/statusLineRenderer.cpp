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

#include <unistd.h> // for isatty(STDOUT_FILENO)
#include "engine.h"
#include "log/statusLineRenderer.h"

namespace AIAssistant
{
    void StatusLineRenderer::Start()
    {
        if (m_Running)
        {
            return;
        }

        m_Running = true;
    }

    void StatusLineRenderer::Stop()
    {
        if (!m_Running)
        {
            return;
        }
        m_Running = false;
    }

    void StatusLineRenderer::UpdateSession(const std::string& name, std::string_view state, size_t outputs, size_t inflight,
                                           size_t completed)
    {
        if (!isatty(STDOUT_FILENO) || !m_Running)
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            auto& s = m_Sessions[name];
            s.name = name;
            s.state = state;
            s.outputs = outputs;
            s.inflight = inflight;
            s.completed = completed;
        }
    }

    void StatusLineRenderer::Render()
    {
        using namespace std::chrono_literals;

        if (!isatty(STDOUT_FILENO) || !m_Running)
        {
            return;
        }

        static auto lastDraw = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if ((now - lastDraw) < 33ms) // ~30 FPS cap
        {
            return;
        }
        lastDraw = now;

        static const auto spinner =
            std::to_array({"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷", "⠁", "⠂", "⠄", "⡀", "⢀", "⠠", "⠐", "⠈"});

        std::lock_guard<std::mutex> lock(m_Mutex);

        // Advance spinners
        for (auto& [_, s] : m_Sessions)
        {
            if (s.inflight > 0 && (now - s.lastSpinnerUpdate) >= 100ms)
            {
                s.spinnerIndex = (s.spinnerIndex + 1) % spinner.size();
                s.lastSpinnerUpdate = now;
            }
        }

        const size_t newHeight = m_Sessions.size();
        const size_t linesToPaint = std::max(m_LastHeight, newHeight);

        // Save cursor, hide cursor, go to bottom, move up to top of old panel
        std::cout << "\033[s"     // save cursor
                  << "\033[?25l"  // hide cursor (optional, looks nicer)
                  << "\033[999B"; // push to bottom

        if (m_LastHeight > 0)
        {
            std::cout << "\033[" << (m_LastHeight - 1) << "A"; // to top of old panel
        }

        // We’ll render exactly linesToPaint lines:
        //   - First render current sessions (sorted for stable order),
        //   - Then clear any leftover old lines if newHeight < m_LastHeight.
        std::vector<std::pair<std::string, SessionStatus>> rows;
        rows.reserve(m_Sessions.size());
        for (auto const& kv : m_Sessions)
        {
            rows.push_back(kv);
        }
        std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

        size_t printed = 0;
        for (; printed < rows.size(); ++printed)
        {
            auto const& name = rows[printed].first;
            auto const& s = rows[printed].second;

            std::string_view color = (s.state == "AllResponsesReceived")   ? "\033[32m"
                                     : (s.state == "SendingQueries")       ? "\033[33m"
                                     : (s.state == "CompilingEnvironment") ? "\033[36m"
                                                                           : "\033[35m";

            std::string spin = (s.inflight > 0) ? ("\033[33m" + std::string(spinner[s.spinnerIndex]) + "\033[0m") : " ";

            std::cout << "\033[2K\r" // clear line
                      << "[" << name << "] " << color << "STATE: " << s.state << "\033[0m"
                      << " | Outputs: " << s.outputs << " | In flight: " << s.inflight << " | Completed: " << s.completed
                      << " " << spin;

            if (printed + 1 < linesToPaint)
            {
                std::cout << "\n";
            }
        }

        // Clear any leftover old lines from a taller previous frame
        for (; printed < linesToPaint; ++printed)
        {
            std::cout << "\033[2K\r";
            if (printed + 1 < linesToPaint)
            {
                std::cout << "\n";
            }
        }

        // Update height and restore cursor
        m_LastHeight = newHeight;
        std::cout << "\033[u"    // restore cursor
                  << "\033[?25h" // show cursor
                  << std::flush;
    }

} // namespace AIAssistant
