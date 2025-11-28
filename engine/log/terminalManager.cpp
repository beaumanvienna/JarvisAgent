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

#include "log/terminalManager.h"
#include "engine.h"

#include <algorithm>
#include <clocale>
#include <mutex>

#include "pdcursesmod/curses.h"

namespace AIAssistant
{
    struct TerminalManager::Impl
    {
        WINDOW* m_LogWindow{nullptr};
        WINDOW* m_StatusWindow{nullptr};
        WINDOW* m_LogHeaderWindow{nullptr};
        WINDOW* m_StatusHeaderWindow{nullptr};

        int m_LastRows{0};
        int m_LastCols{0};
        int m_LogPrintLine{0};

        std::mutex m_LogMutex;
        std::vector<std::string> m_PendingLines;

        StatusLinesCallback m_StatusLinesCallback;
        StatusHeightCallback m_StatusHeightCallback;

        bool m_Initialized{false};

        void ApplyTheme()
        {
            if (has_colors())
            {
                start_color();
                use_default_colors();

                // Log window: JarvisAgent dashboard green (#02a414) if possible,
                // otherwise fall back to the default COLOR_GREEN.
                if (can_change_color())
                {
                    short const greenR = 149;
                    short const greenG = 635;
                    short const greenB = 412;
                    init_color(COLOR_GREEN, greenR, greenG, greenB);
                }

                init_pair(1, COLOR_GREEN, -1);
            }
        }

        void RecreateWindowsIfNeeded()
        {
            int rows = 0;
            int cols = 0;
            getmaxyx(stdscr, rows, cols);

            if (rows <= 0 || cols <= 0)
            {
                return;
            }

            // We have:
            //   - 1 row for LOG header
            //   - logContentHeight rows for log content
            //   - 1 row for STATUS header
            //   - statusContentHeight rows for status content
            //
            // rows = 1 + logContentHeight + 1 + statusContentHeight

            int statusContentHeight = 1;

            if (m_StatusHeightCallback)
            {
                statusContentHeight = m_StatusHeightCallback(rows);
                if (statusContentHeight < 1)
                {
                    statusContentHeight = 1;
                }

                // Leave at least 1 row for log content and 2 rows for headers
                int maxStatusContentHeight = std::max(1, rows - 3);
                if (statusContentHeight > maxStatusContentHeight)
                {
                    statusContentHeight = maxStatusContentHeight;
                }
            }

            int logContentHeight = rows - statusContentHeight - 2;
            if (logContentHeight < 1)
            {
                logContentHeight = 1;
            }

            int logHeaderY = 0;
            int logContentY = logHeaderY + 1;

            int statusHeaderY = logContentY + logContentHeight;
            int statusContentY = statusHeaderY + 1;

            bool recreate = false;

            if (m_LogWindow == nullptr || m_StatusWindow == nullptr || m_LogHeaderWindow == nullptr ||
                m_StatusHeaderWindow == nullptr)
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

                if (currentLogRows != logContentHeight || currentLogCols != cols ||
                    currentStatusRows != statusContentHeight || currentStatusCols != cols)
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
            if (m_LogHeaderWindow != nullptr)
            {
                delwin(m_LogHeaderWindow);
                m_LogHeaderWindow = nullptr;
            }
            if (m_StatusHeaderWindow != nullptr)
            {
                delwin(m_StatusHeaderWindow);
                m_StatusHeaderWindow = nullptr;
            }

            m_LogHeaderWindow = newwin(1, cols, logHeaderY, 0);
            m_LogWindow = newwin(logContentHeight, cols, logContentY, 0);

            m_StatusHeaderWindow = newwin(1, cols, statusHeaderY, 0);
            m_StatusWindow = newwin(statusContentHeight, cols, statusContentY, 0);

            scrollok(m_LogWindow, TRUE);
            idlok(m_LogWindow, TRUE);

            werase(m_LogHeaderWindow);
            werase(m_LogWindow);
            werase(m_StatusHeaderWindow);
            werase(m_StatusWindow);

            wattron(m_LogHeaderWindow, A_BOLD);
            mvwprintw(m_LogHeaderWindow, 0, 0, "[ LOG ]");
            wattroff(m_LogHeaderWindow, A_BOLD);

            for (int i = 6; i < getmaxx(m_LogHeaderWindow); ++i) // 6 = strlen("[ LOG ]")
            {
                mvwaddch(m_LogHeaderWindow, 0, i, '-');
            }

            wattron(m_StatusHeaderWindow, A_BOLD);
            mvwprintw(m_StatusHeaderWindow, 0, 0, "[ STATUS ]");
            wattroff(m_StatusHeaderWindow, A_BOLD);

            for (int i = 10; i < getmaxx(m_StatusHeaderWindow); ++i) // 10 = strlen("[ STATUS ]")
            {
                mvwaddch(m_StatusHeaderWindow, 0, i, '-');
            }

            wrefresh(m_LogHeaderWindow);
            wrefresh(m_LogWindow);
            wrefresh(m_StatusHeaderWindow);
            wrefresh(m_StatusWindow);

            m_LogPrintLine = 0;
        }

        void HandleResize()
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

        void RenderLogMessage(std::string const& message)
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

        void DrainQueuedLogLines()
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

        void RenderStatus()
        {
            if (m_StatusWindow == nullptr)
            {
                return;
            }

            werase(m_StatusWindow);

            if (!m_StatusLinesCallback)
            {
                wrefresh(m_StatusWindow);
                if (m_LogHeaderWindow)
                {
                    wrefresh(m_LogHeaderWindow);
                }
                if (m_StatusHeaderWindow)
                {
                    wrefresh(m_StatusHeaderWindow);
                }
                return;
            }

            int statusRows = 0;
            int statusCols = 0;
            getmaxyx(m_StatusWindow, statusRows, statusCols);

            if (statusRows <= 0 || statusCols <= 0)
            {
                return;
            }

            std::vector<std::string> lines;
            m_StatusLinesCallback(lines, statusCols);

            int lineIndex = 0;
            for (; lineIndex < statusRows && lineIndex < static_cast<int>(lines.size()); ++lineIndex)
            {
                mvwprintw(m_StatusWindow, lineIndex, 0, "%s", lines[static_cast<size_t>(lineIndex)].c_str());
            }

            while (lineIndex < statusRows)
            {
                wmove(m_StatusWindow, lineIndex, 0);
                wclrtoeol(m_StatusWindow);
                ++lineIndex;
            }

            wrefresh(m_StatusWindow);

            if (m_LogHeaderWindow)
            {
                wrefresh(m_LogHeaderWindow);
            }
            if (m_StatusHeaderWindow)
            {
                wrefresh(m_StatusHeaderWindow);
            }
        }
    };

    TerminalManager::TerminalManager() : m_Impl(std::make_unique<TerminalManager::Impl>()) {}

    TerminalManager::~TerminalManager() { Shutdown(); }

    void TerminalManager::Initialize()
    {
        if (m_Impl->m_Initialized)
        {
            return;
        }

        std::setlocale(LC_ALL, "");

        initscr();
        curs_set(0); // disable cursor
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        nodelay(stdscr, TRUE);

        int rows = 0;
        int columns = 0;
        getmaxyx(stdscr, rows, columns);

        m_Impl->m_LastRows = rows;
        m_Impl->m_LastCols = columns;

        m_Impl->ApplyTheme();
        m_Impl->RecreateWindowsIfNeeded();

        m_Impl->m_Initialized = true;
    }

    void TerminalManager::Shutdown()
    {
        if (!m_Impl->m_Initialized)
        {
            return;
        }

        if (m_Impl->m_LogWindow != nullptr)
        {
            delwin(m_Impl->m_LogWindow);
            m_Impl->m_LogWindow = nullptr;
        }
        if (m_Impl->m_StatusWindow != nullptr)
        {
            delwin(m_Impl->m_StatusWindow);
            m_Impl->m_StatusWindow = nullptr;
        }
        if (m_Impl->m_LogHeaderWindow != nullptr)
        {
            delwin(m_Impl->m_LogHeaderWindow);
            m_Impl->m_LogHeaderWindow = nullptr;
        }
        if (m_Impl->m_StatusHeaderWindow != nullptr)
        {
            delwin(m_Impl->m_StatusHeaderWindow);
            m_Impl->m_StatusHeaderWindow = nullptr;
        }

        endwin();

        m_Impl->m_Initialized = false;
    }

    void TerminalManager::Render()
    {
        if (!m_Impl->m_Initialized)
        {
            return;
        }

        m_Impl->HandleResize();
        m_Impl->DrainQueuedLogLines();
        m_Impl->RenderStatus();
    }

    void TerminalManager::RenderPaused(int counter)
    {
        if (!m_Impl->m_Initialized)
        {
            return;
        }

        m_Impl->HandleResize();

        if (m_Impl->m_LogWindow != nullptr)
        {
            werase(m_Impl->m_LogWindow);
            mvwprintw(m_Impl->m_LogWindow, 0, 0, "*** PAUSED (press 'p' to resume) ***");
            mvwprintw(m_Impl->m_LogWindow, 2, 0, "counter=%d", counter);
            wrefresh(m_Impl->m_LogWindow);
        }

        m_Impl->RenderStatus();
    }

    void TerminalManager::EnqueueLogLine(std::string const& line)
    {
        std::lock_guard<std::mutex> guard(m_Impl->m_LogMutex);
        m_Impl->m_PendingLines.push_back(line);
    }

    void TerminalManager::SetStatusCallbacks(StatusLinesCallback statusLinesCallback,
                                             StatusHeightCallback statusHeightCallback)
    {
        m_Impl->m_StatusLinesCallback = std::move(statusLinesCallback);
        m_Impl->m_StatusHeightCallback = std::move(statusHeightCallback);
        m_Impl->RecreateWindowsIfNeeded();
    }
} // namespace AIAssistant
