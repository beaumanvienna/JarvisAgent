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

#include "core.h"
#include "engine.h"
#include "input/keyboardInput.h"
#include "event/events.h"
#include <iostream>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#else
#include <conio.h>
#endif

using namespace AIAssistant;

void KeyboardInput::Start()
{
    if (m_Running)
    {
        return;
    }

    m_Running = true;
    m_ListenerTask = Core::g_Core->GetThreadPool().SubmitTask([this]() { Listen(); });
}

void KeyboardInput::Stop()
{
    if (!m_Running)
    {
        return;
    }
    m_Running = false;
    if (m_ListenerTask.valid())
    {
        m_ListenerTask.wait(); // wait for graceful exit
        LOG_CORE_INFO("Keyboard input stopped");
    }
}

void KeyboardInput::Listen()
{
#ifndef _WIN32
    // Check if stdin is a TTY (fails in Docker containers without -it)
    if (!isatty(STDIN_FILENO))
    {
        LOG_CORE_INFO("No TTY detected, keyboard input disabled (headless mode)");
        return;
    }

    // set terminal to raw mode (non-canonical, no echo)
    termios oldt{}, newt{};
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
#endif

    LOG_CORE_INFO("Keyboard input active. Press 'q' to quit.");

    while (m_Running)
    {
#ifndef _WIN32
        fd_set set;
        struct timeval timeout;
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms

        int rv = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout);
        if (rv > 0 && FD_ISSET(STDIN_FILENO, &set))
        {
            int ch = getchar();
            if (ch == 'q' || ch == 'Q')
            {
                LOG_CORE_INFO("Keyboard input: Quit requested");
                auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                Core::g_Core->PushEvent(event);
                break;
            }
            else if (ch != '\n' && ch != EOF)
            {
                auto event = std::make_shared<KeyPressedEvent>(static_cast<char>(ch));
                Core::g_Core->PushEvent(event);
            }
        }
#else
        if (_kbhit())
        {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q')
            {
                LOG_CORE_INFO("Keyboard: Quit requested");
                auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                Core::g_Core->PushEvent(event);
                break;
            }
            else
            {
                auto event = std::make_shared<KeyPressedEvent>(static_cast<char>(ch));
                Core::g_Core->PushEvent(event);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    }

#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif
}
