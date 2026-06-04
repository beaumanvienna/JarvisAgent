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
#include <atomic>
#include <csignal>
#include <memory>
#include <filesystem>

#include "log/log.h"
#include "application.h"
#include "event/eventQueue.h"
#include "json/configParser.h"
#include "auxiliary/threadPool.h"
#include "input/keyboardInput.h"

#include "log/terminalManager.h"
#include "log/terminalLogStreamBuf.h"
#include "keys/keyManager.h"
#include "keys/oauthTokenManager.h"
#include "cloud/cloudConnectionManager.h"
#include "cloud/cloudConnectorRegistry.h"
#include "cloud/cloudCircuitBreaker.h"

using namespace std::chrono_literals;
namespace AIAssistant
{
    class Core
    {
    public:
        Core();
        ~Core();

        void Start(ConfigParser::EngineConfig const& engineConfig);
        void Run(std::unique_ptr<AIAssistant::Application>&);
        void Shutdown();
        void SignalShutdown();
        [[nodiscard]] bool IsShuttingDown() const { return m_ShuttingDown.load(); }
        void RequestQuit() { m_QuitRequested.store(true, std::memory_order_relaxed); }
        [[nodiscard]] bool IsQuitRequested() const { return m_QuitRequested.load(std::memory_order_relaxed); }
        bool Verbose() const;
        ConfigParser::EngineConfig const& GetConfig() const;
        ConfigParser::EngineConfig& GetMutableConfig() { return m_EngineConfig; }
        ConfigParser::EngineConfig::InterfaceType const& GetInterfaceType() const;
        void SetConfigFilePath(std::filesystem::path const& path) { m_ConfigFilePath = path; }
        std::filesystem::path const& GetConfigFilePath() const { return m_ConfigFilePath; }
        ThreadPool& GetThreadPool();
        TerminalManager* GetTerminalManager();
        TerminalLogStreamBuf* GetTerminalLogStreamBuf() { return m_TerminalBuf.get(); }
        KeyManager& GetKeyManager() { return m_KeyManager; }
        KeyManager const& GetKeyManager() const { return m_KeyManager; }
        CloudConnectionManager& GetCloudConnectionManager() { return m_CloudConnectionManager; }
        CloudConnectionManager const& GetCloudConnectionManager() const { return m_CloudConnectionManager; }
        CloudConnectorRegistry& GetCloudConnectorRegistry() { return m_CloudConnectorRegistry; }
        CloudConnectorRegistry const& GetCloudConnectorRegistry() const { return m_CloudConnectorRegistry; }
        CloudCircuitBreaker& GetCloudCircuitBreaker() { return m_CloudCircuitBreaker; }
        OAuthTokenManager& GetOAuthTokenManager() { return m_OAuthTokenManager; }
        OAuthTokenManager const& GetOAuthTokenManager() const { return m_OAuthTokenManager; }
        std::filesystem::path const& GetLaunchCWDAbsolute() const { return m_LaunchCWDAbsolute; };

        // event API.  `producer` tags the calling subsystem so the
        // per-producer breakdown emitted on EventQueue cap-hit names the
        // wedged caller.  See EventQueue::kMaxUnprocessedEvents.
        void PushEvent(EventQueue::EventPtr eventPtr, ProducerId producer);

    public:
        static std::unique_ptr<AIAssistant::Log> g_Logger;
        static Core* g_Core;

    private:
        static void SignalHandler(int sig);
        static void InstallSignalHandlers();
        void DisableCtrlCOutput();
        void CheckSignalFlags();

        // Signal-handler flags.  `volatile std::sig_atomic_t` is the only
        // POSIX-blessed way to communicate from a signal handler to the
        // main thread; std::atomic<bool> is lock-free in practice on every
        // platform we target but the C++ standard does NOT guarantee it
        // is async-signal-safe (only std::atomic_flag carries that
        // guarantee, and a flag-only API is too thin for the two-state
        // ack dance).  Loads / stores of sig_atomic_t are atomic per
        // POSIX; the polling pattern in Core::Run + Core::CheckSignalFlags
        // observes writes via the periodic re-read.
        //
        // s_ShutdownRequested  — set to 1 by the handler on first
        //                        SIGINT/SIGTERM; CheckSignalFlags consumes
        //                        it and pushes the shutdown event.
        // s_ShutdownAcked      — set to 1 by CheckSignalFlags after first
        //                        ack; the handler force-_exits if it sees
        //                        ack=1 (the user pressed Ctrl+C twice and
        //                        the orderly shutdown is taking too long).
        //                        Also force-exits if the request flag is
        //                        already set when a new signal arrives,
        //                        covering rapid double-press before the
        //                        main loop has had a chance to ack.
        static volatile std::sig_atomic_t s_ShutdownRequested;
        static volatile std::sig_atomic_t s_ShutdownAcked;

    private:
        // THREADS_REQUIRED_BY_APP:
        // file watcher and keyboard input need threads (web server uses its own thread)
        static constexpr size_t THREADS_REQUIRED_BY_APP = 2;
        std::atomic<bool> m_ShuttingDown{false};
        std::atomic<bool> m_QuitRequested{false};
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
        std::filesystem::path m_LaunchCWDAbsolute;
        std::filesystem::path m_ConfigFilePath;

        // key management
        KeyManager m_KeyManager;

        // cloud integration
        OAuthTokenManager m_OAuthTokenManager{m_KeyManager};
        CloudConnectionManager m_CloudConnectionManager;
        CloudConnectorRegistry m_CloudConnectorRegistry;
        CloudCircuitBreaker m_CloudCircuitBreaker;
    };
} // namespace AIAssistant
