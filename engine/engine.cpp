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

    // check engine config
    ConfigChecker().Check(engineConfig);
    if (!engineConfig.IsValid())
    {
        // exit with error = true
        return EXIT_FAILURE;
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
