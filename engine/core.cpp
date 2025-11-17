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

#include <csignal>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#else
#include <windows.h>
#endif
#include "tracy/Tracy.hpp"

#include "core.h"
#include "engine.h"
#include "event/events.h"

namespace AIAssistant
{
    // global logger for the engine and application
    std::unique_ptr<AIAssistant::Log> Core::g_Logger;
    Core* Core::g_Core{nullptr};

    Core::Core()
    {
        g_Core = this;
        // signal handling
        signal(SIGINT, SignalHandler);
        DisableCtrlCOutput();

        // create the engine and application loggers
        g_Logger = std::make_unique<AIAssistant::Log>();
    }

    void Core::SignalHandler(int signal)
    {
        static bool sigIntReceived{false};
        if ((signal == SIGINT) && sigIntReceived)
        {
            LOG_CORE_INFO("force shudown");
            // force shudown
            exit(EXIT_FAILURE);
        }
        if (signal == SIGINT)
        {
            sigIntReceived = true;
            LOG_CORE_INFO("Received signal SIGINT, exiting");
            auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
            g_Core->PushEvent(event);
        }
    }

    void Core::DisableCtrlCOutput()
    {
#ifndef _WIN32
        termios term;
        if (tcgetattr(STDIN_FILENO, &term) == 0)
        {
            term.c_lflag &= ~ECHOCTL; // disable echoing of ^C etc.
            tcsetattr(STDIN_FILENO, TCSANOW, &term);
        }
#else
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        GetConsoleMode(hIn, &mode);
        mode &= ~ENABLE_ECHO_INPUT; // disable echoing
        SetConsoleMode(hIn, mode);
#endif
    }

    void Core::PushEvent(EventQueue::EventPtr eventPtr) { m_EventQueue.Push(std::move(eventPtr)); }

    void Core::Start(ConfigParser::EngineConfig const& engineConfig)
    {
        m_EngineConfig = engineConfig;

        m_ThreadPool.Reset(m_EngineConfig.m_MaxThreads + THREADS_REQUIRED_BY_APP);
        LOG_CORE_INFO("thread count: {}", m_ThreadPool.Size());

        m_KeyboardInput = std::make_unique<KeyboardInput>();
        m_KeyboardInput->Start();

        m_StatusLineRenderer.Start();
    }

    void Core::Run(std::unique_ptr<AIAssistant::Application>& app)
    {
        tracy::SetThreadName("main thread (run loop)");

        // run loop
        do
        {
            {
                ZoneScopedN("application->OnUpdate");
                app->OnUpdate();
            }

            m_StatusLineRenderer.Render();

            { // event handling
                const auto green = 0x00ff00;
                ZoneScopedNC("event handling", green);

                // pop all pending events from queue
                auto events = m_EventQueue.PopAll();

                for (auto& eventPtr : events)
                {
                    Event& event = *eventPtr;
                    EventDispatcher dispatcher(event);

                    // engine-level event handling
                    dispatcher.Dispatch<AppErrorEvent>(
                        [](AppErrorEvent& appErrorEvent)
                        {
                            LOG_CORE_CRITICAL("Engine handled AppErrorEvent, ID: {}", appErrorEvent.GetErrorCode());
                            return true;
                        });

                    // pass to app if not handled
                    if (!event.IsHandled())
                    {
                        app->OnEvent(eventPtr);
                    }
                }
            }

            { // go easy on the CPU
                const int cyan = 0x00ffff;
                ZoneScopedNC("sleep time (accuracy check for tracy)", cyan);
                CORE_ASSERT((m_EngineConfig.m_SleepDuration > 0ms) && (m_EngineConfig.m_SleepDuration <= 256ms),
                            "sleep duration incorrect");
                std::this_thread::sleep_for(std::chrono::milliseconds(m_EngineConfig.m_SleepDuration));
            }
        } while (!app->IsFinished());
    }

    void Core::Shutdown()
    {
        if (m_KeyboardInput)
        {
            m_KeyboardInput->Stop();
        }

        m_StatusLineRenderer.Stop();
        m_ThreadPool.Wait();
    }
} // namespace AIAssistant