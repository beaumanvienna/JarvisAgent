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

        // The queue + workflows roots are runtime-owned and untracked in git, so a
        // fresh checkout (or a cleaned tree) ships without them.  Create them on
        // demand here — the earliest validation point, before any subsystem scans
        // or loads them — and only treat a genuine failure (a non-directory file
        // sitting at the path, or a permissions error) as a fatal config error.
        // create_directories is recursive and a no-op when the directory already
        // exists; we re-check IsDirectory afterwards because its bool return is
        // false for an already-present directory on non-MSVC platforms.
        auto ensureRuntimeDir = [](std::string const& dirPath) -> bool
        {
            if (EngineCore::IsDirectory(dirPath))
            {
                return true;
            }
            EngineCore::CreateDirectory(dirPath);
            return EngineCore::IsDirectory(dirPath);
        };

        auto checkQueueFolderFilepath = ensureRuntimeDir;
        auto checkWorkflowsFolder = ensureRuntimeDir;

        auto checkApiInterface = [](std::vector<ConfigParser::EngineConfig::ApiInterface> const& apiInterfaces,
                                    size_t apiIndex) -> bool
        {
            if (apiInterfaces.empty())
            {
                return false;
            }

            if (apiIndex >= apiInterfaces.size())
            {
                return false;
            }

            auto checkUrl = [](std::string const& url) -> bool
            {
                // Scheme prefix check, not substring — guards against URLs
                // like "http://evil.com/?x=https://fake" passing the gate.
                // `http://` is accepted because `UrlPolicy::ValidateAiInterfaceUrl`
                // already gated it at parse time: any http:// URL that reaches
                // ConfigChecker is loopback-only and has no key_name (the local-
                // LLM case — ollama, llama.cpp, vLLM).  Non-loopback http://
                // is dropped from `m_ApiInterfaces` at parse, so it cannot
                // surface here.
                if (url.starts_with("https://") && url.size() > sizeof("https://") - 1)
                {
                    return true;
                }
                if (url.starts_with("http://") && url.size() > sizeof("http://") - 1)
                {
                    return true;
                }
                return false;
            };

            auto checkModel = [](std::string const& model) -> bool
            {
                return !model.empty();
            };

            bool hasUrl = checkUrl(apiInterfaces[apiIndex].m_Url);
            bool hasModel = checkModel(apiInterfaces[apiIndex].m_Model);
            bool hasType = apiInterfaces[apiIndex].m_InterfaceType != ConfigParser::EngineConfig::InterfaceType::InvalidAPI;
            return hasUrl && hasModel && hasType;
        };

        // references for convenience
        auto& queueFolderFilepath = engineConfig.m_QueueFolderFilepath;
        auto& workflowsFolder = engineConfig.m_WorkflowsFolderFilepath;

        bool ok1 = checkQueueFolderFilepath(queueFolderFilepath);                            //
        bool ok2 = checkWorkflowsFolder(workflowsFolder);                                    //
        bool ok3 = checkApiInterface(engineConfig.m_ApiInterfaces, engineConfig.m_ApiIndex); //

        // conclusion
        m_ConfigIsOk = ok1 && ok2 && ok3;

        // handling
        if (!m_ConfigIsOk)
        {
            if (!ok1)
            {
                LOG_CORE_ERROR("config error: queue folder is not a directory '{}' — see log/log.txt for details",
                               queueFolderFilepath);
            }
            if (!ok2)
            {
                LOG_CORE_ERROR("config error: workflows folder is not a directory '{}' — see log/log.txt for details",
                               workflowsFolder);
            }
            if (!ok3)
            {
                LOG_CORE_ERROR("config error: invalid API interface configuration (index {}, count {}) — see log/log.txt "
                               "for details",
                               engineConfig.m_ApiIndex, engineConfig.m_ApiInterfaces.size());
            }
        }
        else
        {
            // max threads not set: fix it
            if ((engineConfig.m_MaxThreads <= 0) || (engineConfig.m_MaxThreads > 256))
            {
                LOG_APP_ERROR("Max threads not set. Fixing max threads. The config file should have a field "
                              "similar to '\"max threads\": 20'");
                engineConfig.m_MaxThreads = 16;
            }

            // sleep time not set: fix it
            if ((engineConfig.m_SleepDuration <= 0ms) || (engineConfig.m_SleepDuration > 256ms))
            {
                LOG_APP_ERROR("Sleep time not set. Fixing sleep time. The config file should have a field "
                              "similar to '\"engine sleep time in run loop in ms\": 10'");
                engineConfig.m_SleepDuration = 10ms;
            }

            // max file size not set: fix it
            if ((engineConfig.m_MaxFileSizekB <= 0) || (engineConfig.m_MaxFileSizekB > 256))
            {
                LOG_APP_ERROR("Max file size not set. Fixing max file size. The config file should have a field "
                              "similar to '\"max file size in kB\": 20'");
                engineConfig.m_MaxFileSizekB = 20;
            }

            // max inflight ai calls: clamp to [1, 10000]. The reactive 429 handling in
            // CurlMultiDispatcher is the real backstop — this cap only prevents runaway
            // local resource use.
            if (engineConfig.m_MaxInflightAiCalls == 0 || engineConfig.m_MaxInflightAiCalls > 10000)
            {
                LOG_APP_ERROR("Max inflight AI calls out of range. Fixing to 1000. The config file should have a field "
                              "similar to '\"max inflight ai calls\": 1000'");
                engineConfig.m_MaxInflightAiCalls = 1000;
            }

            // port: range validation is done at parse time in configParser.cpp

            // python engines: clamp to [1, 16]
            if (engineConfig.m_PythonEngines == 0 || engineConfig.m_PythonEngines > 16)
            {
                LOG_APP_ERROR("Python engines out of range. Fixing to 4. The config file should have a field "
                              "similar to '\"python engines\": 4'");
                engineConfig.m_PythonEngines = 4;
            }
        }

        // all checks completed
        engineConfig.m_ConfigValid = m_ConfigIsOk;
        return m_ConfigIsOk;
    }

    bool ConfigChecker::ConfigIsOk() const { return m_ConfigIsOk; }

} // namespace AIAssistant
