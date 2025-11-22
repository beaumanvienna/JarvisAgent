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

#include "log/terminalRenderer.h"

#include <algorithm>
#include <clocale> // std::setlocale

namespace AIAssistant
{
    TerminalRenderer::TerminalRenderer()
        : m_LogWindow(nullptr), m_StatusWindow(nullptr), m_LastRows(0), m_LastCols(0), m_LogPrintLine(0)
    {
    }

    TerminalRenderer::~TerminalRenderer() { Shutdown(); }

    void TerminalRenderer::ApplyTheme()
    {
        if (has_colors())
        {
            start_color();
            use_default_colors();

            // Log window: green text on default background (JarvisAgent style)
            init_pair(1, COLOR_GREEN, -1);
        }
    }

    void TerminalRenderer::Initialize()
    {
        // -----------------------------------------------------
        // Enable UTF-8 / locale so ncursesw handles wide glyphs
        // -----------------------------------------------------
        std::setlocale(LC_ALL, "");

        // -----------------------------------------------------
        // Global ncurses init
        // -----------------------------------------------------
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        nodelay(stdscr, TRUE);

        // -----------------------------------------------------
        // Determine terminal size
        // -----------------------------------------------------
        int rows = 0;
        int columns = 0;
        getmaxyx(stdscr, rows, columns);

        m_LastRows = rows;
        m_LastCols = columns;

        // -----------------------------------------------------
        // Apply theme & create windows
        // -----------------------------------------------------
        ApplyTheme();
        RecreateWindowsIfNeeded();
    }

    void TerminalRenderer::Shutdown()
    {
        if (m_LogWindow != nullptr)
        {
            delwin(m_LogWindow);
            m_LogWindow = nullptr;
        }

        if (m_StatusWindow != nullptr)
        {
            delwin(m_StatusWindow);
            m_StatusWindow = nullptr;
        }

        // Restore terminal state
        endwin();
    }

    void TerminalRenderer::RecreateWindowsIfNeeded()
    {
        int rows = 0;
        int cols = 0;
        getmaxyx(stdscr, rows, cols);

        if (rows <= 0 || cols <= 0)
        {
            return;
        }

        size_t sessionCount = m_StatusRenderer.GetSessionCount();
        if (sessionCount == 0)
        {
            sessionCount = 1;
        }

        int statusHeight = static_cast<int>(sessionCount);
        if (statusHeight >= rows)
        {
            statusHeight = std::max(1, rows - 1);
        }

        int logHeight = rows - statusHeight;
        if (logHeight < 1)
        {
            logHeight = 1;
        }

        bool recreate = false;

        if (m_LogWindow == nullptr || m_StatusWindow == nullptr)
        {
            recreate = true;
        }
        else
        {
            int currentLogRows = 0;
            int currentLogCols = 0;
            getmaxyx(m_LogWindow, currentLogRows, currentLogCols);

            int currentStatusRows = 0;
            int currentStatusCols = 0;
            getmaxyx(m_StatusWindow, currentStatusRows, currentStatusCols);

            if (currentLogRows != logHeight || currentLogCols != cols || currentStatusRows != statusHeight ||
                currentStatusCols != cols)
            {
                recreate = true;
            }
        }

        if (!recreate)
        {
            return;
        }

        if (m_LogWindow != nullptr)
        {
            delwin(m_LogWindow);
            m_LogWindow = nullptr;
        }

        if (m_StatusWindow != nullptr)
        {
            delwin(m_StatusWindow);
            m_StatusWindow = nullptr;
        }

        m_LogWindow = newwin(logHeight, cols, 0, 0);
        m_StatusWindow = newwin(statusHeight, cols, logHeight, 0);

        scrollok(m_LogWindow, TRUE);
        idlok(m_LogWindow, TRUE);

        werase(m_LogWindow);
        werase(m_StatusWindow);

        wrefresh(m_LogWindow);
        wrefresh(m_StatusWindow);

        m_LogPrintLine = 0;
    }

    void TerminalRenderer::HandleResize()
    {
        int rows = 0;
        int cols = 0;
        getmaxyx(stdscr, rows, cols);

        if (rows != m_LastRows || cols != m_LastCols)
        {
            m_LastRows = rows;
            m_LastCols = cols;
            RecreateWindowsIfNeeded();
        }
        else
        {
            RecreateWindowsIfNeeded();
        }
    }

    void TerminalRenderer::RenderLogMessage(std::string const& message)
    {
        if (m_LogWindow == nullptr)
        {
            return;
        }

        int rows = 0;
        int cols = 0;
        getmaxyx(m_LogWindow, rows, cols);

        if (rows <= 0 || cols <= 0)
        {
            return;
        }

        if (m_LogPrintLine >= rows)
        {
            wscrl(m_LogWindow, 1);
            m_LogPrintLine = rows - 1;

            wmove(m_LogWindow, m_LogPrintLine, 0);
            wclrtoeol(m_LogWindow);
        }

        wattron(m_LogWindow, COLOR_PAIR(1));
        mvwprintw(m_LogWindow, m_LogPrintLine, 0, "%s", message.c_str());
        wattroff(m_LogWindow, COLOR_PAIR(1));

        ++m_LogPrintLine;

        wrefresh(m_LogWindow);
    }

    void TerminalRenderer::EnqueueLogLine(std::string const& line)
    {
        std::lock_guard<std::mutex> guard(m_LogMutex);
        m_PendingLines.push_back(line);
    }

    void TerminalRenderer::UpdateSession(std::string const& name, std::string_view state, size_t outputs, size_t inflight,
                                         size_t completed)
    {
        m_StatusRenderer.UpdateSession(name, state, outputs, inflight, completed);
    }

    void TerminalRenderer::DrainQueuedLogLines()
    {
        std::vector<std::string> localLines;

        {
            std::lock_guard<std::mutex> guard(m_LogMutex);
            localLines.swap(m_PendingLines);
        }

        for (std::string const& line : localLines)
        {
            RenderLogMessage(line);
        }
    }

    void TerminalRenderer::Render()
    {
        HandleResize();

        DrainQueuedLogLines();

        if (m_StatusWindow != nullptr)
        {
            m_StatusRenderer.Render(m_StatusWindow);
        }

        if (m_LogWindow != nullptr)
        {
            wrefresh(m_LogWindow);
        }
    }

    void TerminalRenderer::RenderPaused(int counter)
    {
        HandleResize();

        if (m_LogWindow != nullptr)
        {
            werase(m_LogWindow);
            mvwprintw(m_LogWindow, 0, 0, "*** PAUSED (press 'p' to resume) ***");
            mvwprintw(m_LogWindow, 2, 0, "counter=%d", counter);
            wrefresh(m_LogWindow);
        }

        if (m_StatusWindow != nullptr)
        {
            m_StatusRenderer.Render(m_StatusWindow);
        }
    }
} // namespace AIAssistant
