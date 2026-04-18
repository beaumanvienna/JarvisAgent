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
#include "jarvisAgent.h"
#include "json/configParser.h"
#include "json/configChecker.h"
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
              << "JarvisAgent is a C++ backend that monitors a queue folder for input files,\n"
              << "dispatches AI requests in parallel, runs DAG-based workflows (AI, Python,\n"
              << "shell, C++), and serves a React dashboard plus a visual workflow editor.\n"
              << "\n"
              << "Usage:\n"
              << "  jarvisAgent              Start the agent (requires config.json in the working directory)\n"
              << "  jarvisAgent --help       Show this help message and exit\n"
              << "  jarvisAgent --version    Show version and exit\n"
              << "\n"
              << "Before starting:\n"
              << "  - Review and adjust config.json in the working directory\n"
              << "  - Install dependencies as described in README.md\n"
              << "  - Provide API keys via keys.json.enc, keys.json, or OPENAI_API_KEY env var\n"
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
              << "  - Dashboard:   http://localhost:8080\n"
              << "  - Workflow Editor: http://localhost:8080/editor\n"
              << "  - REST API:    see doc/api-endpoints.md\n"
              << "\n"
              << "Example:\n"
              << "  curl -s http://localhost:8080/api/status | python -m json.tool\n"
              << std::endl;
}

int engine(int argc, char* argv[])
{
    // Process CLI flags before anything else (clang-style: --help/--version
    // take priority and cause immediate exit, unknown options are rejected).
    bool wantsHelp = false;
    bool wantsVersion = false;
    std::string unknownOption;

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
        else if (argv[i][0] == '-' && unknownOption.empty())
        {
            unknownOption = argv[i];
        }
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
    if (!unknownOption.empty())
    {
        std::cerr << "jarvisAgent: unknown option '" << unknownOption << "'\n"
                  << "Try 'jarvisAgent --help' for more information." << std::endl;
        return EXIT_FAILURE;
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

    // Initialize key manager: try encrypted keys file, then plaintext, then env var fallback
    {
        auto& keyManager = Core::g_Core->GetKeyManager();
        std::filesystem::path const keysFilePathAbsolute =
            Core::g_Core->GetLaunchCWDAbsolute() / engineConfig.m_KeysFilePath;
        std::filesystem::path const plaintextKeysPathAbsolute = Core::g_Core->GetLaunchCWDAbsolute() / "keys.json";

        // Always store the encrypted keys file path for potential runtime unlock
        keyManager.SetKeysFilePath(keysFilePathAbsolute);

        bool keysLoaded = false;

        if (std::filesystem::exists(keysFilePathAbsolute))
        {
            // The master password is supplied at runtime via POST /api/settings/keys/unlock
            // (see doc/cyber security.md §"Master password after restart"). There is no
            // environment-variable auto-unlock path — that was removed so the password is
            // held exclusively in mlock()-protected memory (SecureString) and never leaks
            // through env vars, process listings, or docker inspect.
            keyManager.SetKeyLoadStatus(KeyManager::KeyLoadStatus::NoPassword);
            LOG_CORE_INFO("KeyManager: '{}' present — awaiting master password via "
                          "POST /api/settings/keys/unlock",
                          keysFilePathAbsolute.string());
        }

        // Try plaintext keys.json (development convenience)
        if (!keysLoaded && std::filesystem::exists(plaintextKeysPathAbsolute))
        {
            keysLoaded = keyManager.LoadPlaintext(plaintextKeysPathAbsolute);
            if (keysLoaded)
            {
                keyManager.SetKeyLoadStatus(KeyManager::KeyLoadStatus::Ok);
            }
        }

        // Fall back to OPENAI_API_KEY environment variable (bootstrap convenience).
        // We only consider this fallback when there is NO encrypted keys file at
        // all — otherwise the env-var-loaded provider would silently mask the
        // sealed-store state, and the dashboard's status endpoint would report
        // "ok" while the MCP key store is still locked (breaking the master-
        // password prompt flow and leaving every MCP auth request rejected).
        if (!keysLoaded && !std::filesystem::exists(keysFilePathAbsolute))
        {
            std::string endpoint;
            std::string model;
            std::string apiType;

            if (engineConfig.m_ApiIndex < engineConfig.m_ApiInterfaces.size())
            {
                auto const& iface = engineConfig.m_ApiInterfaces[engineConfig.m_ApiIndex];
                endpoint = iface.m_Url;
                model = iface.m_Model;
                apiType = (iface.m_InterfaceType == ConfigParser::EngineConfig::InterfaceType::API1) ? "API1" : "API2";
            }

            keysLoaded = keyManager.LoadFromEnvironment(endpoint, model, apiType);
            if (keysLoaded)
            {
                keyManager.SetKeyLoadStatus(KeyManager::KeyLoadStatus::Ok);
            }
        }

        if (!keysLoaded && keyManager.GetKeyLoadStatus() != KeyManager::KeyLoadStatus::NoPassword)
        {
            LOG_CORE_WARN("KeyManager: no API keys configured — AI tasks will fail at dispatch time");
        }
    }

    // Load cloud connections from connections.json if it exists
    {
        std::filesystem::path const connectionsPath =
            Core::g_Core->GetLaunchCWDAbsolute() / "connections.json";

        if (std::filesystem::exists(connectionsPath))
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

    engine->Start(engineConfig);

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
