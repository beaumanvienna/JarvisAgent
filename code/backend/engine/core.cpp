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
#include <condition_variable>
#include <mutex>
#include <thread>
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
#include "curlWrapper/curlWrapper.h"

volatile std::sig_atomic_t AIAssistant::Core::s_ShutdownRequested{0};
volatile std::sig_atomic_t AIAssistant::Core::s_ShutdownAcked{0};

extern "C" void JarvisRedirect(const char* message)
{
    if (!message)
    {
        return;
    }

    // Forward into the C++ logging system, which handles:
    //  - ncurses terminal log window via TerminalLogStreamBuf
    //  - /tmp/log.txt
    std::cout << message << std::endl;
}

namespace AIAssistant
{
    // global logger for the engine and application
    std::unique_ptr<AIAssistant::Log> Core::g_Logger;
    Core* Core::g_Core{nullptr};

    Core::Core() : m_LaunchCWDAbsolute(std::filesystem::current_path())
    {
        g_Core = this;
        InstallSignalHandlers();
        DisableCtrlCOutput();

        // -----------------------------------------------------------------
        // Create terminal manager and redirect std::cout / std::cerr
        // -----------------------------------------------------------------
        m_TerminalManager = std::make_unique<TerminalManager>();

        m_LogFile = std::make_shared<std::ofstream>();
        std::string filename = "log/log.txt";
        m_LogFile->open(filename, std::ios::out | std::ios::trunc);

        m_OriginalCoutBuffer = std::cout.rdbuf();
        m_TerminalBuf = std::make_unique<TerminalLogStreamBuf>(m_TerminalManager.get(), m_LogFile);
        std::cout.rdbuf(m_TerminalBuf.get());
        std::cerr.rdbuf(m_TerminalBuf.get());

        // create the engine and application loggers (logs go through terminal)
        g_Logger = std::make_unique<AIAssistant::Log>();

        if (m_LogFile->is_open())
        {
            LOG_CORE_INFO("Logging to {}", filename);
        }
        else
        {
            LOG_CORE_WARN("Failed to open log file {}", filename);
        }
        LOG_CORE_INFO("Launch CWD (Absolute) {}", m_LaunchCWDAbsolute.string());
    }

    Core::~Core()
    {
        if (m_OriginalCoutBuffer != nullptr)
        {
            std::cout.rdbuf(m_OriginalCoutBuffer);
            std::cerr.rdbuf(m_OriginalCoutBuffer);
            m_OriginalCoutBuffer = nullptr;
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Async-signal-safety contract
    // ─────────────────────────────
    // SignalHandler runs in signal context, which means the C++ standard
    // library is almost entirely off-limits.  POSIX defines a small set
    // of operations that are async-signal-safe; we constrain the handler
    // to those only:
    //
    //   PERMITTED:  _exit, write, raise, kill, sigaction, signal,
    //               atomic load/store of `volatile sig_atomic_t`,
    //               std::atomic_flag::test_and_set.
    //   FORBIDDEN:  LOG_APP_* / LOG_CORE_* / LOG_SECURITY_* (spdlog locks
    //               + heap), malloc/new/delete, std::mutex, std::cout /
    //               std::cerr, std::string operations, std::filesystem,
    //               std::shared_ptr / std::make_shared, anything that may
    //               throw, anything that allocates.
    //
    // The two-flag dance (s_ShutdownRequested + s_ShutdownAcked) keeps
    // the handler trivial while preserving the "Ctrl+C twice = force
    // exit" escape hatch:
    //
    //   • First signal: handler sets s_ShutdownRequested=1.  Main loop's
    //     CheckSignalFlags consumes it, sets s_ShutdownAcked=1, emits the
    //     log line, requests quit, pushes the engine shutdown event.
    //   • Second signal arrives AFTER the main loop ack'd: handler sees
    //     s_ShutdownAcked=1 → _exit(EXIT_FAILURE).  No graceful path.
    //   • Rapid double-press BEFORE the main loop ack'd: handler sees
    //     s_ShutdownRequested already 1 → _exit(EXIT_FAILURE).  Same
    //     outcome, different race.
    //
    // The handler is registered for SIGINT (Ctrl+C) and SIGTERM (kill,
    // pkill, systemd `stop`).  SIGPIPE is process-wide ignored because
    // libcurl is configured with CURLOPT_NOSIGNAL=1
    // (workflowRuntimeManager.cpp), which disables curl's internal
    // SIGPIPE handler — without our explicit ignore a remote peer
    // closing a socket mid-write would terminate the process.  Crash
    // signals (SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGABRT) are
    // intentionally NOT handled: any non-trivial work in their handlers
    // is undefined behaviour because the program state is already
    // corrupt, and our log path is async-signal-unsafe regardless.  Let
    // the OS produce a core dump.
    // ─────────────────────────────────────────────────────────────────────
    void Core::SignalHandler(int sig)
    {
        if (sig != SIGINT && sig != SIGTERM)
        {
            return;
        }
        // Either flag set means "already in shutdown" — second arrival is
        // the user asking us to give up gracefully and die now.
        if (s_ShutdownAcked != 0 || s_ShutdownRequested != 0)
        {
            _exit(EXIT_FAILURE);
        }
        s_ShutdownRequested = 1;
    }

    void Core::InstallSignalHandlers()
    {
#ifdef _WIN32
        // Windows lacks sigaction; the CRT's signal() supports SIGINT and
        // SIGTERM in a limited fashion.  No SIGPIPE on Windows (no Unix
        // sockets in the POSIX sense).
        std::signal(SIGINT, &Core::SignalHandler);
        std::signal(SIGTERM, &Core::SignalHandler);
#else
        // sigaction over signal(): predictable semantics across Linux/macOS
        // (signal() inherits BSD vs SysV ambiguity), explicit mask, no
        // SA_RESTART so a slow syscall (e.g. sleep_for in the engine main
        // loop) wakes promptly when the signal arrives.
        struct sigaction sa{};
        sa.sa_handler = &Core::SignalHandler;
        sigemptyset(&sa.sa_mask);
        // Block both SIGINT and SIGTERM during handler execution so a
        // SIGTERM mid-SIGINT-handler (or vice versa) cannot interleave the
        // flag updates.  The default mask only blocks the delivered
        // signal; we widen to both since they share the same handler.
        sigaddset(&sa.sa_mask, SIGINT);
        sigaddset(&sa.sa_mask, SIGTERM);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        // SIGPIPE: ignore process-wide.  EPIPE surfaces on the originating
        // syscall instead of killing the process; libcurl translates it
        // into CURLE_SEND_ERROR / CURLE_RECV_ERROR for the caller to
        // handle.  See header comment block on s_ShutdownRequested for
        // the libcurl CURLOPT_NOSIGNAL background.
        struct sigaction saIgnore{};
        saIgnore.sa_handler = SIG_IGN;
        sigemptyset(&saIgnore.sa_mask);
        saIgnore.sa_flags = 0;
        sigaction(SIGPIPE, &saIgnore, nullptr);
#endif
    }

    void Core::CheckSignalFlags()
    {
        if (s_ShutdownRequested != 0)
        {
            // Order matters: set the ack BEFORE clearing the request.  If
            // a second signal arrives between these two stores, the
            // handler still sees s_ShutdownAcked=1 and force-exits — the
            // narrow window is safe in either ordering.  Also, never reset
            // s_ShutdownAcked: the handler must remain able to escalate
            // to force-exit until the process actually terminates.
            s_ShutdownAcked = 1;
            s_ShutdownRequested = 0;
            LOG_CORE_INFO("Received signal SIGINT/SIGTERM, exiting");
            g_Core->RequestQuit();
            auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
            g_Core->PushEvent(event, ProducerId::SignalHandler);
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

    void Core::PushEvent(EventQueue::EventPtr eventPtr, ProducerId producer)
    {
        m_EventQueue.Push(std::move(eventPtr), producer);
    }

    void Core::Start(ConfigParser::EngineConfig const& engineConfig)
    {
        m_EngineConfig = engineConfig;

        m_ThreadPool.Reset(m_EngineConfig.m_MaxThreads + THREADS_REQUIRED_BY_APP);
        LOG_CORE_INFO("thread count: {}", m_ThreadPool.Size());

        if (m_TerminalManager)
        {
            m_TerminalManager->Initialize();
        }

        m_KeyboardInput = std::make_unique<KeyboardInput>();
        m_KeyboardInput->Start();

        m_OAuthTokenManager.Start();
    }

    void Core::Run(std::unique_ptr<AIAssistant::Application>& app)
    {
        tracy::SetThreadName("main thread (run loop)");

        // run loop
        do
        {
            { // event handling (before OnUpdate so quit/SIGINT is processed promptly)
#ifdef TRACY_ENABLE
                const auto green = 0x00ff00;
                ZoneScopedNC("event handling", green);
#endif

                // Check if SIGINT was received (async-signal-safe polling)
                CheckSignalFlags();

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

            {
                ZoneScopedN("application->OnUpdate");
                app->OnUpdate();
            }

            if (m_TerminalManager)
            {
                m_TerminalManager->Render();
            }

            { // go easy on the CPU
#ifdef TRACY_ENABLE
                const int cyan = 0x00ffff;
                ZoneScopedNC("sleep time (accuracy check for tracy)", cyan);
#endif
                CORE_ASSERT((m_EngineConfig.m_SleepDuration > 0ms) && (m_EngineConfig.m_SleepDuration <= 256ms),
                            "sleep duration incorrect");
                std::this_thread::sleep_for(std::chrono::milliseconds(m_EngineConfig.m_SleepDuration));
            }
        } while (!app->IsFinished());
    }

    void Core::SignalShutdown()
    {
        m_ShuttingDown = true;
        m_ThreadPool.RequestStop();
        LOG_CORE_INFO("[shutdown] global shutdown signal set");
    }

    void Core::Shutdown()
    {
        auto const shutdownStart = std::chrono::steady_clock::now();
        auto elapsed = [&]()
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - shutdownStart)
                          .count();
            return ms;
        };

        LOG_CORE_INFO("[shutdown +{}ms] stopping OAuthTokenManager...", elapsed());
        m_OAuthTokenManager.Stop();
        LOG_CORE_INFO("[shutdown +{}ms] OAuthTokenManager stopped", elapsed());

        LOG_CORE_INFO("[shutdown +{}ms] stopping KeyboardInput...", elapsed());
        if (m_KeyboardInput)
        {
            m_KeyboardInput->Stop();
        }
        LOG_CORE_INFO("[shutdown +{}ms] KeyboardInput stopped", elapsed());

        // Shut down thread pool BEFORE curl global cleanup and TerminalManager.
        // Thread pool tasks use curl handles and may still be logging.
        LOG_CORE_INFO("[shutdown +{}ms] shutting down thread pool ({} threads)...", elapsed(), m_ThreadPool.Size());
        m_ThreadPool.Shutdown();
        LOG_CORE_INFO("[shutdown +{}ms] thread pool done", elapsed());

        LOG_CORE_INFO("[shutdown +{}ms] CurlWrapper::GlobalCleanup...", elapsed());
        CurlWrapper::GlobalCleanup();
        LOG_CORE_INFO("[shutdown +{}ms] CurlWrapper cleaned up", elapsed());

        LOG_CORE_INFO("[shutdown +{}ms] stopping TerminalManager...", elapsed());
        if (m_TerminalManager)
        {
            m_TerminalManager->Shutdown();
        }
        LOG_CORE_INFO("[shutdown +{}ms] TerminalManager stopped", elapsed());

        // --- Raw diagnostics: bypass spdlog/streambuf to catch post-log hangs ---
#ifndef _WIN32
#define RAW_SHUTDOWN(literal)                                                             \
    do                                                                                    \
    {                                                                                     \
        [[maybe_unused]] auto rc_ = ::write(STDERR_FILENO, literal, sizeof(literal) - 1); \
    } while (0)
#else
#define RAW_SHUTDOWN(literal) _write(_fileno(stderr), literal, sizeof(literal) - 1)
#endif

        RAW_SHUTDOWN("[shutdown] flushing cout...\n");
        // Ensure all pending log output is flushed ---
        std::cout << std::flush;
        RAW_SHUTDOWN("[shutdown] flushing cerr...\n");
        std::cerr << std::flush;

        RAW_SHUTDOWN("[shutdown] restoring rdbuf...\n");
        if (m_OriginalCoutBuffer != nullptr)
        {
            std::cout.rdbuf(m_OriginalCoutBuffer);
            std::cerr.rdbuf(m_OriginalCoutBuffer);
            m_OriginalCoutBuffer = nullptr;
        }

        RAW_SHUTDOWN("[shutdown] resetting TerminalBuf...\n");
        m_TerminalBuf.reset();

        RAW_SHUTDOWN("[shutdown] closing log file...\n");
        if (m_LogFile && m_LogFile->is_open())
        {
            m_LogFile->close();
        }

        RAW_SHUTDOWN("[shutdown] Core::Shutdown() done\n");
#undef RAW_SHUTDOWN
    }

    bool Core::Verbose() const { return m_EngineConfig.m_Verbose; }

    ConfigParser::EngineConfig const& Core::GetConfig() const { return m_EngineConfig; }

    ConfigParser::EngineConfig::InterfaceType const& Core::GetInterfaceType() const
    {
        // Interfaces are hydrated from the encrypted store only after unlock, so
        // the table is legitimately empty before then — guard the index.
        static constexpr ConfigParser::EngineConfig::InterfaceType invalid =
            ConfigParser::EngineConfig::InterfaceType::InvalidAPI;
        if (m_EngineConfig.m_ApiIndex >= m_EngineConfig.m_ApiInterfaces.size())
        {
            return invalid;
        }
        return m_EngineConfig.m_ApiInterfaces[m_EngineConfig.m_ApiIndex].m_InterfaceType;
    }

    ThreadPool& Core::GetThreadPool() { return m_ThreadPool; }

    TerminalManager* Core::GetTerminalManager() { return m_TerminalManager.get(); }
} // namespace AIAssistant