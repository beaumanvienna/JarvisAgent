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
#include <memory>

#include "log/log.h"
#include "application.h"
#include "event/eventQueue.h"
#include "json/configParser.h"
#include "auxiliary/threadPool.h"
#include "auxiliary/file.h"
#include "input/keyboardInput.h"

#include "log/terminalManager.h"
#include "log/terminalLogStreamBuf.h"

using namespace std::chrono_literals;
namespace AIAssistant
{
    class Core
    {
    public:
        Core();
        ~Core() = default;

        void Start(ConfigParser::EngineConfig const& engineConfig);
        void Run(std::unique_ptr<AIAssistant::Application>&);
        void Shutdown();
        bool Verbose() const;
        ConfigParser::EngineConfig const& GetConfig() const;
        ConfigParser::EngineConfig::InterfaceType const& GetInterfaceType() const;
        ThreadPool& GetThreadPool();
        TerminalManager* GetTerminalManager();

        // event API
        void PushEvent(EventQueue::EventPtr eventPtr);

    public:
        static std::unique_ptr<AIAssistant::Log> g_Logger;
        static Core* g_Core;

    private:
        static void SignalHandler(int signal);
        void DisableCtrlCOutput();

    private:
        // THREADS_REQUIRED_BY_APP:
        // file watcher, keyboard input, and web server need threads
        static constexpr uint THREADS_REQUIRED_BY_APP = 3;
        ThreadPool m_ThreadPool;
        EventQueue m_EventQueue;

        // core config
        ConfigParser::EngineConfig m_EngineConfig;

        // input
        std::unique_ptr<KeyboardInput> m_KeyboardInput;

        // terminal output and logging
        std::unique_ptr<TerminalManager> m_TerminalManager;
        std::unique_ptr<TerminalLogStreamBuf> m_TerminalBuf;
        std::shared_ptr<std::ofstream> m_LogFile;
        std::streambuf* m_OriginalCoutBuffer{nullptr};
    };
} // namespace AIAssistant