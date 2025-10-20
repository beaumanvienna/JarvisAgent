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
#include "json/configChecker.h"
#include "auxiliary/file.h"

namespace AIAssistant
{
    bool ConfigChecker::Check(ConfigParser::EngineConfig& engineConfig)
    {
        m_ConfigIsOk = true;

        auto& queueFolderFilepath = engineConfig.m_QueueFolderFilepath;
        if (!EngineCore::IsDirectory(queueFolderFilepath))
        {
            LOG_CORE_ERROR("config error: queue folder filepath is not a directory '{}'", queueFolderFilepath);
            m_ConfigIsOk = false;
        }
        else
        {
            // queue is a directory, but max threads not set: fix it
            if ((engineConfig.m_MaxThreads <= 0) || (engineConfig.m_MaxThreads > 256))
            {
                LOG_APP_ERROR("Max threads not set. Fixing max threads. The config file should have a field "
                              "similar to '\"max threads\": 20'");
                engineConfig.m_MaxThreads = 16;
            }

            // queue is a directory, but sleep time not set: fix it
            if ((engineConfig.m_SleepTime <= 0) || (engineConfig.m_SleepTime > 256))
            {
                LOG_APP_ERROR("Sleep time not set. Fixing sleep time. The config file should have a field "
                              "similar to '\"engine sleep time in run loop in ms\": 10'");
                engineConfig.m_SleepTime = 10;
            }
        }

        // all checks completed
        engineConfig.m_ConfigValid = m_ConfigIsOk;
        return m_ConfigIsOk;
    }

    bool ConfigChecker::ConfigIsOk() const { return m_ConfigIsOk; }

} // namespace AIAssistant
