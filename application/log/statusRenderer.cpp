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

#include "log/statusRenderer.h"

#include <algorithm>
#include <array>
#include <sstream>

using namespace std::chrono_literals;

namespace AIAssistant
{
    // ---------------------------------------------------------
    // Helper: safely truncate UTF-8 string to maxColumns chars
    // ---------------------------------------------------------
    static void SafeTruncateUtf8(std::string& text, int maxColumns)
    {
        if (maxColumns <= 0)
        {
            text.clear();
            return;
        }

        int columnCount = 0;
        size_t byteIndex = 0;
        size_t const totalBytes = text.size();

        while (byteIndex < totalBytes)
        {
            unsigned char c = static_cast<unsigned char>(text[byteIndex]);

            int charLength = (c & 0x80) == 0      ? 1
                             : (c & 0xE0) == 0xC0 ? 2
                             : (c & 0xF0) == 0xE0 ? 3
                             : (c & 0xF8) == 0xF0 ? 4
                                                  : 1;

            if (columnCount + 1 > maxColumns)
            {
                text.resize(byteIndex);
                return;
            }

            byteIndex += static_cast<size_t>(charLength);
            columnCount += 1;
        }
    }

    void StatusRenderer::Start()
    {
        // No-op (kept for compatibility)
    }

    void StatusRenderer::Stop()
    {
        // No-op (kept for compatibility)
    }

    void StatusRenderer::UpdateSession(std::string const& name, std::string_view state, size_t outputs, size_t inflight,
                                       size_t completed)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        SessionStatus& sessionStatus = m_Sessions[name];
        sessionStatus.name = name;
        sessionStatus.state = std::string(state);
        sessionStatus.outputs = outputs;
        sessionStatus.inflight = inflight;
        sessionStatus.completed = completed;
    }

    size_t StatusRenderer::GetSessionCount()
    {
        std::lock_guard<std::mutex> guard(m_Mutex);
        return m_Sessions.size();
    }

    void StatusRenderer::BuildStatusLines(std::vector<std::string>& outLines, int maxColumns)
    {
        static std::array<char const*, 16> const spinnerChars{"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷",
                                                              "⠁", "⠂", "⠄", "⡀", "⢀", "⠠", "⠐", "⠈"};

        auto const now = std::chrono::steady_clock::now();

        std::vector<std::pair<std::string, SessionStatus>> rows;

        {
            std::lock_guard<std::mutex> guard(m_Mutex);

            for (auto& entry : m_Sessions)
            {
                SessionStatus& sessionStatus = entry.second;

                if (sessionStatus.inflight > 0 && (now - sessionStatus.lastSpinnerUpdate) >= 100ms)
                {
                    sessionStatus.spinnerIndex = (sessionStatus.spinnerIndex + 1) % spinnerChars.size();
                    sessionStatus.lastSpinnerUpdate = now;
                }
            }

            rows.reserve(m_Sessions.size());
            for (auto const& entry : m_Sessions)
            {
                rows.push_back(entry);
            }
        }

        std::sort(rows.begin(), rows.end(),
                  [](std::pair<std::string, SessionStatus> const& left, std::pair<std::string, SessionStatus> const& right)
                  { return left.first < right.first; });

        outLines.clear();

        for (auto const& row : rows)
        {
            std::string const& name = row.first;
            SessionStatus const& sessionStatus = row.second;

            char const* spinnerGlyph = " ";
            if (sessionStatus.inflight > 0)
            {
                spinnerGlyph = spinnerChars[sessionStatus.spinnerIndex % spinnerChars.size()];
            }

            std::ostringstream textStream;
            textStream << "[" << name << "] "
                       << "STATE: " << sessionStatus.state << " | Outputs: " << sessionStatus.outputs
                       << " | In flight: " << sessionStatus.inflight << " | Completed: " << sessionStatus.completed << " "
                       << spinnerGlyph;

            std::string lineText = textStream.str();
            SafeTruncateUtf8(lineText, maxColumns);

            outLines.push_back(std::move(lineText));
        }
    }
} // namespace AIAssistant
