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

        // check functions
        auto checkQueueFolderFilepath = [](std::string const& queueFolderFilepath) -> bool
        {
            bool isDir = EngineCore::IsDirectory(queueFolderFilepath);
            CORE_ASSERT(isDir, "queueFolderFilepath is not a directory");
            return isDir;
        };

        auto checkApiInterface = [](std::vector<ConfigParser::EngineConfig::ApiInterface> const& apiInterfaces,
                                    size_t apiIndex) -> bool
        {
            if (apiInterfaces.empty())
            {
                CORE_ASSERT(false, "no APIs provided");
                return false;
            }

            if (apiInterfaces.size() < apiIndex)
            {
                CORE_ASSERT(false, "invalid API index");
                return false;
            }

            auto checkUrl = [](std::string const& url) -> bool
            {
                std::string https("https://");
                bool notEmpty = url.size() > https.size();
                bool hasHttps = url.find(https) != std::string::npos;
                CORE_ASSERT(notEmpty && hasHttps, "provided url invalid");
                return notEmpty && hasHttps;
            };

            auto checkModel = [](std::string const& model) -> bool
            {
                bool notEmpty = !model.empty();
                CORE_ASSERT(notEmpty, "no model provided");
                return notEmpty;
            };

            bool hasUrl = checkUrl(apiInterfaces[apiIndex].m_Url);
            bool hasModel = checkModel(apiInterfaces[apiIndex].m_Model);
            bool hasType = apiInterfaces[apiIndex].m_InterfaceType != ConfigParser::EngineConfig::InterfaceType::InvalidAPI;
            return hasUrl && hasModel && hasType;
        };

        // references for convenience
        auto& queueFolderFilepath = engineConfig.m_QueueFolderFilepath;

        // conclusion
        m_ConfigIsOk = checkQueueFolderFilepath(queueFolderFilepath) &&                          //
                       checkApiInterface(engineConfig.m_ApiInterfaces, engineConfig.m_ApiIndex); //

        // handling
        if (!m_ConfigIsOk)
        {
            LOG_CORE_ERROR("config error: queue folder filepath is not a directory '{}'", queueFolderFilepath);
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
            if ((engineConfig.m_SleepDuration <= 0ms) || (engineConfig.m_SleepDuration > 256ms))
            {
                LOG_APP_ERROR("Sleep time not set. Fixing sleep time. The config file should have a field "
                              "similar to '\"engine sleep time in run loop in ms\": 10'");
                engineConfig.m_SleepDuration = 10ms;
            }
        }

        // all checks completed
        engineConfig.m_ConfigValid = m_ConfigIsOk;
        return m_ConfigIsOk;
    }

    bool ConfigChecker::ConfigIsOk() const { return m_ConfigIsOk; }

} // namespace AIAssistant
