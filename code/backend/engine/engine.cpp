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

#include "engine.h"
#include "core.h"
#include "curlWrapper/awsSigV4.h"
#include "jarvisAgent.h"
#include "json/configParser.h"
#include "json/configChecker.h"
#ifdef J9T_HEAPSCAN_BUILD
    #include "security/heapScan_test.h"
#endif
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

static void PrintVersion() { std::cout << "jarvisAgent version " << JARVIS_AGENT_VERSION << std::endl; }

static void PrintHelp()
{
    PrintVersion();
    std::cout << "\n"
              << "JarvisAgent is a C++ backend that runs DAG-based workflows (AI, Python,\n"
              << "shell, C++), dispatches AI requests in parallel over HTTP/2, and serves\n"
              << "a React dashboard plus a visual workflow editor.\n"
              << "\n"
              << "Usage:\n"
              << "  jarvisAgent              Start the agent (requires config.json in the working directory)\n"
              << "  jarvisAgent --help       Show this help message and exit\n"
              << "  jarvisAgent --version    Show version and exit\n"
              << "\n"
              << "Before starting:\n"
              << "  - Review and adjust config.json in the working directory\n"
              << "  - Install dependencies as described in README.md\n"
              << "  - Provide API keys in the encrypted keys.json.enc and unlock it at runtime via the dashboard or POST /api/settings/keys/unlock\n"
              << "\n"
              << "Python virtual environment:\n"
              << "  Packaged installs create a .venv/ in the working directory.\n"
              << "  JarvisAgent activates it automatically for workflow tasks.\n"
              << "  For manual use of markitdown, md2pdf, or playwright:\n"
#ifdef _WIN32
              << "    .venv\\Scripts\\activate\n"
#else
              << "    source .venv/bin/activate\n"
#endif
              << "\n"
              << "At runtime:\n"
              << "  - Terminal UI: press 'q' or Ctrl-C to quit\n"
              << "  - Dashboard:   https://localhost:8443\n"
              << "  - Workflow Editor: https://localhost:8443/editor\n"
              << "  - REST API:    see doc/api-endpoints.md\n"
              << "\n"
              << "Example:\n"
              << "  curl -sk https://localhost:8443/api/status | python -m json.tool\n"
              << std::endl;
}

int engine(int argc, char* argv[])
{
    // Process CLI flags before anything else. --help/--version take priority and
    // exit immediately. Any other argument is ignored rather than rejected: the
    // per-platform launchers (AppRun, jarvisagent-launcher.sh, the Windows .bat,
    // …) may forward launcher-only flags such as --no-browser or --home, and an
    // unrecognised argument must never prevent the server from starting.
    bool wantsHelp = false;
    bool wantsVersion = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            wantsHelp = true;
        }
        else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0)
        {
            wantsVersion = true;
        }
        // All other arguments are intentionally ignored (see comment above).
    }

    if (wantsHelp)
    {
        PrintHelp();
        return EXIT_SUCCESS;
    }
    if (wantsVersion)
    {
        PrintVersion();
        return EXIT_SUCCESS;
    }

    // Check for config.json before initializing the engine
    std::filesystem::path const configFilePathAbsolute = std::filesystem::current_path() / "config.json";
    if (!std::filesystem::exists(configFilePathAbsolute))
    {
        std::cerr << "Error: config.json not found in " << std::filesystem::current_path().string() << std::endl;
        return EXIT_FAILURE;
    }

    // create engine (including the logger)
    auto engine = std::make_unique<Core>();

    // parse JSON file to retrieve engine config
    std::string const configFilePathAbsoluteString = configFilePathAbsolute.lexically_normal().string();
    ConfigParser configParser(configFilePathAbsoluteString.c_str());
    ConfigParser::EngineConfig engineConfig{};
    configParser.Parse(engineConfig);
    if (!configParser.ConfigParsed())
    {
        fprintf(stderr, "Error: failed to parse config.json\nSee log/log.txt for details.\n");
        return EXIT_FAILURE;
    }

    // Store config file path for runtime access
    Core::g_Core->SetConfigFilePath(configFilePathAbsolute);

    // check engine config
    ConfigChecker().Check(engineConfig);
    if (!engineConfig.IsValid())
    {
        // exit with error = true
        return EXIT_FAILURE;
    }

    // Initialize key manager.  Credentials load exclusively from the AES-256-GCM
    // keys.json.enc, unlocked at runtime via POST /api/settings/keys/unlock.  There is
    // no plaintext-keystore path and no env-var credential path in any edition — every
    // on-disk credential is encrypted at rest (per doc/cyber security.md).
    {
        auto& keyManager = Core::g_Core->GetKeyManager();
        std::filesystem::path const keysFilePathAbsolute =
            Core::g_Core->GetLaunchCWDAbsolute() / engineConfig.m_KeysFilePath;

        keyManager.SetKeysFilePath(keysFilePathAbsolute);

        if (std::filesystem::exists(keysFilePathAbsolute))
        {
            // The master password is supplied at runtime via POST /api/settings/keys/unlock
            // (see doc/cyber security.md §"Master password after restart").  It is held
            // exclusively in mlock()-protected memory (SecureString) and never read from an
            // env var, so it never leaks through process listings or docker inspect.
            keyManager.SetKeyLoadStatus(KeyManager::KeyLoadStatus::NoPassword);
            LOG_CORE_INFO("KeyManager: '{}' present — awaiting master password via "
                          "POST /api/settings/keys/unlock",
                          keysFilePathAbsolute.string());
        }
        else
        {
            LOG_CORE_WARN("KeyManager: no '{}' present — AI tasks will fail at dispatch time until a "
                          "keystore is created and unlocked",
                          keysFilePathAbsolute.string());
        }
    }

    // Load cloud connections from connections.json if it exists
    {
        std::filesystem::path const connectionsPath =
            Core::g_Core->GetLaunchCWDAbsolute() / "connections.json";

        if (std::filesystem::exists(connectionsPath))
        {
            // Pre-read size cap: stat the file first so a multi-GB file can never
            // expand the in-process std::string before ParseConnectionsJson's own cap
            // sees it.  Same 1 MB threshold as the parser-side cap (defense in depth).
            static constexpr std::uintmax_t kMaxConnectionsFileBytes = 1 * 1024 * 1024;
            std::error_code sizeEc;
            auto const fileSize = std::filesystem::file_size(connectionsPath, sizeEc);
            if (sizeEc)
            {
                LOG_CORE_ERROR("CloudConnectionManager: failed to stat '{}': {}",
                               connectionsPath.string(), sizeEc.message());
            }
            else if (fileSize > kMaxConnectionsFileBytes)
            {
                LOG_CORE_ERROR("CloudConnectionManager: '{}' size {} bytes exceeds {} byte cap; refusing to load",
                               connectionsPath.string(), fileSize, kMaxConnectionsFileBytes);
            }
            else
            {
                std::ifstream file(connectionsPath);
                if (file)
                {
                    std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    file.close();

                    auto& connectionManager = engine->GetCloudConnectionManager();
                    if (connectionManager.ParseConnectionsJson(json))
                    {
                        auto names = connectionManager.GetConnectionNames();
                        LOG_CORE_INFO("CloudConnectionManager: loaded {} connection(s) from '{}'", names.size(),
                                      connectionsPath.string());
                        connectionManager.ClearDirty();
                    }
                    else
                    {
                        LOG_CORE_WARN("CloudConnectionManager: failed to parse '{}'", connectionsPath.string());
                    }
                }
            }
        }
    }

    engine->Start(engineConfig);

#ifndef NDEBUG
    SigV4Signer::RunSelfTest();
#endif

#ifdef J9T_HEAPSCAN_BUILD
    // Heap-scan audit: plant nonce secrets through each auth style, scan
    // /proc/self/mem for residue, exit with PASS/FAIL.  Binary never reaches
    // normal engine startup in audit builds.  See test/security/heapScan_test.cpp.
    std::exit(AIAssistant::RunHeapScanAudit());
#endif

    // create application Jarvis
    std::unique_ptr<AIAssistant::Application> app = JarvisAgent::Create();

    // start Jarvis
    app->OnStart();

    engine->Run(app);

    // shutdown — start watchdog that covers the entire sequence
    std::mutex watchdogMutex;
    std::condition_variable watchdogCV;
    bool watchdogDiffused = false;
    std::thread watchdog(
        [&]()
        {
            std::unique_lock<std::mutex> lock(watchdogMutex);
            if (!watchdogCV.wait_for(lock, 3s, [&] { return watchdogDiffused; }))
            {
                std::cerr << "[shutdown watchdog] timeout expired — forcing exit" << std::endl;
                _exit(EXIT_FAILURE);
            }
        });
    watchdog.detach();

    // Raw diagnostics — before OnShutdown, so we can see if we even get here.
#ifndef _WIN32
#define RAW_ENGINE(literal) do { [[maybe_unused]] auto rc_ = ::write(STDERR_FILENO, literal, sizeof(literal) - 1); } while (0)
#else
#define RAW_ENGINE(literal) _write(_fileno(stderr), literal, sizeof(literal) - 1)
#endif

    RAW_ENGINE("[engine] entering app->OnShutdown()...\n");
    app->OnShutdown();
    RAW_ENGINE("[engine] app->OnShutdown() done\n");
    engine->Shutdown();
    RAW_ENGINE("[engine] engine->Shutdown() done\n");

    // Print fatal startup message after curses teardown so it's visible on the terminal.
    if (!app->GetFatalStartupMessage().empty())
    {
        std::cerr << "\n" << app->GetFatalStartupMessage() << "\n" << std::endl;
    }

    RAW_ENGINE("[engine] diffusing watchdog\n");
    // diffuse the watchdog
    {
        std::lock_guard<std::mutex> lock(watchdogMutex);
        watchdogDiffused = true;
    }
    watchdogCV.notify_one();
    RAW_ENGINE("[engine] done\n");
#undef RAW_ENGINE

    return EXIT_SUCCESS;
}
