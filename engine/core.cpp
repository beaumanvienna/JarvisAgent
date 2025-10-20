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

namespace AIAssistant
{
    // global logger for the engine and application
    std::unique_ptr<AIAssistant::Log> Core::g_Logger;

    Core::Core() : m_SleepDuration(10ms)
    {
        // create the engine and application loggers
        g_Logger = std::make_unique<AIAssistant::Log>();
    }

    void Core::Start(ConfigParser::EngineConfig const& engineConfig)
    {
        m_MaxThreads = engineConfig.m_MaxThreads;
        m_QueueFolderFilepath = std::filesystem::path(engineConfig.m_QueueFolderFilepath);
        m_SleepDuration = std::chrono::milliseconds(engineConfig.m_SleepTime);
    }

    void Core::Run(std::unique_ptr<AIAssistant::Application>& app)
    {
        // run loop
        do
        {
            app->OnUpdate();

            // go easy on the CPU
            CORE_ASSERT((m_SleepDuration > 0ms) && (m_SleepDuration <= 256ms), "sleep duration incorrect");
            std::this_thread::sleep_for(std::chrono::milliseconds(m_SleepDuration));
        } while (!app->IsFinished());
    }

    void Core::Shutdown() {}
} // namespace AIAssistant