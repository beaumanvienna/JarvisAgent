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
#include <cstring>
#include <filesystem>
#include <string>

int engine(int argc, char* argv[])
{
    // create engine (including the logger)
    auto engine = std::make_unique<Core>();

    // parse JSON file to retrieve engine config
    std::filesystem::path const configFilePathAbsolute = Core::g_Core->GetLaunchCWDAbsolute() / "config.json";
    std::string const configFilePathAbsoluteString = configFilePathAbsolute.lexically_normal().string();
    ConfigParser configParser(configFilePathAbsoluteString.c_str());
    ConfigParser::EngineConfig engineConfig{};
    configParser.Parse(engineConfig);
    if (!configParser.ConfigParsed())
    {
        // exit with error = true
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
            // Try master password from environment variable
            char const* masterPasswordEnv = std::getenv("JARVIS_MASTER_PASSWORD");
            if (masterPasswordEnv && std::strlen(masterPasswordEnv) > 0)
            {
                keysLoaded = keyManager.Load(keysFilePathAbsolute, masterPasswordEnv);
                if (!keysLoaded)
                {
                    keyManager.SetKeyLoadStatus(KeyManager::KeyLoadStatus::WrongPassword);
                    LOG_CORE_WARN("KeyManager: failed to decrypt '{}' — wrong password?", keysFilePathAbsolute.string());
                }
                else
                {
                    keyManager.SetKeyLoadStatus(KeyManager::KeyLoadStatus::Ok);
                }
            }
            else
            {
                keyManager.SetKeyLoadStatus(KeyManager::KeyLoadStatus::NoPassword);
                LOG_CORE_WARN("KeyManager: '{}' exists but JARVIS_MASTER_PASSWORD is not set",
                              keysFilePathAbsolute.string());
            }
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

        // Fall back to OPENAI_API_KEY environment variable (backward compatibility)
        if (!keysLoaded)
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

        if (!keysLoaded)
        {
            LOG_CORE_WARN("KeyManager: no API keys configured — AI tasks will fail at dispatch time");
        }
    }

    engine->Start(engineConfig);

    // create application Jarvis
    std::unique_ptr<AIAssistant::Application> app = JarvisAgent::Create();

    // start Jarvis
    app->OnStart();

    engine->Run(app);

    // shutdown
    app->OnShutdown();
    engine->Shutdown();

    return EXIT_SUCCESS;
}
