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

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <ncursesw/ncurses.h>

#include "log/ITerminal.h"
#include "log/statusRenderer.h"

namespace AIAssistant
{
    class TerminalRenderer : public ITerminal
    {
    public:
        TerminalRenderer();
        virtual ~TerminalRenderer();

        void Initialize() override;
        void Shutdown() override;

        void Render() override;
        void RenderPaused(int counter) override;

        void EnqueueLogLine(std::string const& line) override;

        void UpdateSession(std::string const& name, std::string_view state, size_t outputs, size_t inflight,
                           size_t completed) override;

    private:
        void RenderLogMessage(std::string const& message);
        void RecreateWindowsIfNeeded();
        void ApplyTheme();
        void DrainQueuedLogLines();
        void HandleResize();

    private:
        WINDOW* m_LogWindow;
        WINDOW* m_StatusWindow;

        int m_LastRows;
        int m_LastCols;
        int m_LogPrintLine;

        StatusRenderer m_StatusRenderer;

        std::mutex m_LogMutex;
        std::vector<std::string> m_PendingLines;
    };
} // namespace AIAssistant
