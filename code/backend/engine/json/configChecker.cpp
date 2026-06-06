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

#include <filesystem>

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
        // Use std::filesystem::create_directories directly, NOT EngineCore::CreateDirectory:
        // on Windows <windows.h> macro-expands the token `CreateDirectory` to `CreateDirectoryW`,
        // which would rewrite even the qualified call here and fail to link (`create_directories`
        // is not a Win32 macro).  It is recursive and a no-op when the dir already exists; we
        // re-check IsDirectory afterwards since the ec-overload returns void.
        auto ensureRuntimeDir = [](std::string const& dirPath) -> bool
        {
            if (EngineCore::IsDirectory(dirPath))
            {
                return true;
            }
            std::error_code directoryCreateError;
            std::filesystem::create_directories(dirPath, directoryCreateError);
            return EngineCore::IsDirectory(dirPath);
        };

        auto checkQueueFolderFilepath = ensureRuntimeDir;
        auto checkWorkflowsFolder = ensureRuntimeDir;

        // AI interfaces are no longer part of config.json — they live in the
        // encrypted API.json.enc and are validated by ApiInterfaceManager at
        // load/upsert time, then hydrated into m_ApiInterfaces after unlock.  So
        // there is nothing interface-related to validate here at config-load
        // (the table is legitimately empty until the master password is entered).

        // references for convenience
        auto& queueFolderFilepath = engineConfig.m_QueueFolderFilepath;
        auto& workflowsFolder = engineConfig.m_WorkflowsFolderFilepath;

        bool ok1 = checkQueueFolderFilepath(queueFolderFilepath); //
        bool ok2 = checkWorkflowsFolder(workflowsFolder);         //

        // conclusion
        m_ConfigIsOk = ok1 && ok2;

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
