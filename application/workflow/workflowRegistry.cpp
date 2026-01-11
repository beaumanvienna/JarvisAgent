/* Copyright (c) 2025 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "workflow/workflowRegistry.h"

#include "engine.h"
#include "core.h"
#include "workflow/workflowJsonParser.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    namespace
    {
        bool ReadFileToString(std::filesystem::path const& filePath, std::string& outText)
        {
            std::ifstream fileStream(filePath, std::ios::binary);
            if (!fileStream.is_open())
            {
                return false;
            }

            fileStream.seekg(0, std::ios::end);
            std::streamoff const size = fileStream.tellg();
            if (size < 0)
            {
                return false;
            }

            outText.clear();
            outText.resize(static_cast<size_t>(size));

            fileStream.seekg(0, std::ios::beg);
            fileStream.read(outText.data(), static_cast<std::streamsize>(outText.size()));

            return fileStream.good() || fileStream.eof();
        }

        bool HasSupportedWorkflowExtension(std::filesystem::path const& filePath)
        {
            std::string const extension = filePath.extension().string();
            if (extension == ".jcwf")
            {
                return true;
            }

            // Many projects still store workflows as .json during transition.
            if (extension == ".json")
            {
                return true;
            }

            return false;
        }
    } // namespace

    void WorkflowRegistry::Clear() { m_Workflows.clear(); }

    bool WorkflowRegistry::LoadDirectory(std::filesystem::path const& workflowsDirectoryPath)
    {
        Clear();

        if (workflowsDirectoryPath.empty())
        {
            LOG_APP_WARN("WorkflowRegistry::LoadDirectory: empty workflows directory path");
            return false;
        }

        std::error_code errorCode;
        if (!std::filesystem::exists(workflowsDirectoryPath, errorCode))
        {
            LOG_APP_WARN("WorkflowRegistry::LoadDirectory: directory does not exist: '{}'", workflowsDirectoryPath.string());
            return false;
        }

        if (!std::filesystem::is_directory(workflowsDirectoryPath, errorCode))
        {
            LOG_APP_WARN("WorkflowRegistry::LoadDirectory: path is not a directory: '{}'", workflowsDirectoryPath.string());
            return false;
        }

        size_t loadedCount = 0;

        for (std::filesystem::directory_entry const& entry :
             std::filesystem::directory_iterator(workflowsDirectoryPath, errorCode))
        {
            if (errorCode)
            {
                LOG_APP_WARN("WorkflowRegistry::LoadDirectory: directory iteration error: '{}' ({})",
                             workflowsDirectoryPath.string(), errorCode.message());
                break;
            }

            if (!entry.is_regular_file())
            {
                continue;
            }

            std::filesystem::path const filePath = entry.path();
            if (!HasSupportedWorkflowExtension(filePath))
            {
                continue;
            }

            if (LoadWorkflowFile(filePath))
            {
                loadedCount++;
            }
        }

        if (loadedCount == 0)
        {
            return false;
        }

        return true;
    }

    std::vector<std::string> WorkflowRegistry::GetWorkflowIds() const
    {
        std::vector<std::string> workflowIds;
        workflowIds.reserve(m_Workflows.size());

        for (auto const& workflowPair : m_Workflows)
        {
            workflowIds.push_back(workflowPair.first);
        }

        std::sort(workflowIds.begin(), workflowIds.end());
        return workflowIds;
    }

    std::optional<WorkflowDefinition> WorkflowRegistry::GetWorkflow(std::string const& workflowId) const
    {
        auto const iterator = m_Workflows.find(workflowId);
        if (iterator == m_Workflows.end())
        {
            return std::nullopt;
        }

        return iterator->second; // copy (keeps call sites simple)
    }

    bool WorkflowRegistry::LoadWorkflowFile(std::filesystem::path const& workflowFilePath)
    {
        std::string const launchCWDAbsoluteText =
            (Core::g_Core != nullptr) ? Core::g_Core->GetLaunchCWDAbsolute().string() : "<null>";
        LOG_APP_INFO("[paths debug] WorkflowRegistry::LoadWorkflowFile debug: reason=loadWorkflow "
                     "workflowFilePathRelative='{}' isRelative={} launchCWDAbsolute='{}'",
                     workflowFilePath.string(), workflowFilePath.is_relative(), launchCWDAbsoluteText);

        std::filesystem::path workflowFilePathAbsolute = workflowFilePath;
        if (workflowFilePathAbsolute.is_relative())
        {
            if (Core::g_Core != nullptr)
            {
                workflowFilePathAbsolute =
                    std::filesystem::path(Core::g_Core->GetLaunchCWDAbsolute()) / workflowFilePathAbsolute;
            }
        }

        workflowFilePathAbsolute = std::filesystem::absolute(workflowFilePathAbsolute).lexically_normal();

        LOG_APP_INFO("[paths debug] WorkflowRegistry::LoadWorkflowFile debug: reason=resolveWorkflowFilePath "
                     "workflowFilePathRelative='{}' workflowFilePathAbsolute='{}'",
                     workflowFilePath.string(), workflowFilePathAbsolute.string());

        std::string fileText;
        if (!ReadFileToString(workflowFilePathAbsolute, fileText))
        {
            LOG_APP_WARN("WorkflowRegistry::LoadWorkflowFile: failed to read '{}'", workflowFilePathAbsolute.string());
            return false;
        }

        WorkflowJsonParser parser;
        WorkflowDefinition workflowDefinition;
        std::string errorMessage;

        if (!parser.ParseWorkflowJson(fileText, workflowDefinition, errorMessage))
        {
            LOG_APP_WARN("WorkflowRegistry::LoadWorkflowFile: parse failed for '{}': {}", workflowFilePathAbsolute.string(),
                         errorMessage);
            return false;
        }

        workflowDefinition.m_WorkflowFilePath = workflowFilePathAbsolute.lexically_normal().string();
        workflowDefinition.m_WorkflowFilePathAbsolute = workflowFilePathAbsolute.lexically_normal().string();

        std::filesystem::path const workflowFileDirectoryPathAbsolute =
            workflowFilePathAbsolute.parent_path().lexically_normal();
        workflowDefinition.m_WorkflowFileDirectory = workflowFileDirectoryPathAbsolute.string();
        workflowDefinition.m_WorkflowFileDirectoryAbsolute = workflowFileDirectoryPathAbsolute.string();

        std::string const workflowBaseDirectoryRaw = workflowDefinition.m_WorkflowBaseDirectory;
        bool workflowBaseDirectoryIsRelative = false;

        std::filesystem::path workflowBaseDirectoryPathAbsolute;
        if (workflowDefinition.m_WorkflowBaseDirectory.empty())
        {
            workflowBaseDirectoryPathAbsolute = workflowFileDirectoryPathAbsolute;
        }
        else
        {
            std::filesystem::path baseDirectoryPath(workflowDefinition.m_WorkflowBaseDirectory);
            workflowBaseDirectoryIsRelative = workflowBaseDirectoryIsRelative;
            if (workflowBaseDirectoryIsRelative)
            {
                baseDirectoryPath = workflowFileDirectoryPathAbsolute / baseDirectoryPath;
            }

            workflowBaseDirectoryPathAbsolute = std::filesystem::absolute(baseDirectoryPath).lexically_normal();
        }

        workflowDefinition.m_WorkflowBaseDirectory = workflowBaseDirectoryPathAbsolute.string();
        workflowDefinition.m_WorkflowBaseDirectoryAbsolute = workflowBaseDirectoryPathAbsolute.string();

        LOG_APP_INFO("[paths debug] WorkflowRegistry::LoadWorkf...DirectoryAbsolute='{}' workflowBaseDirectoryRelative='{}' "
                     "workflowBaseDirectoryIsRelative={} workflowBaseDirectoryAbsolute='{}'",
                     workflowDefinition.m_WorkflowFileDirectoryAbsolute, workflowBaseDirectoryRaw,
                     workflowBaseDirectoryIsRelative, workflowDefinition.m_WorkflowBaseDirectoryAbsolute);

        LOG_APP_WARN(
            "[paths debug] WorkflowRegistry::LoadWorkflowFile debug: reason=workflowParsed workflowId='{}' taskCount={}",
            workflowDefinition.m_Id, workflowDefinition.m_Tasks.size());

        if (workflowDefinition.m_Id.empty())
        {
            LOG_APP_WARN("WorkflowRegistry::LoadWorkflowFile: workflow in '{}' has empty id",
                         workflowFilePathAbsolute.string());
            return false;
        }
        auto const [iterator, inserted] = m_Workflows.emplace(workflowDefinition.m_Id, workflowDefinition);
        if (!inserted)
        {
            LOG_APP_WARN("WorkflowRegistry::LoadWorkflowFile: duplicate workflow id '{}' (file '{}')",
                         workflowDefinition.m_Id, workflowFilePathAbsolute.string());
            return false;
        }

        return true;
    }

    bool WorkflowRegistry::ValidateAll() const
    {
        bool allValid = true;

        for (auto const& workflowPair : m_Workflows)
        {
            std::string const& workflowId = workflowPair.first;
            WorkflowDefinition const& workflowDefinition = workflowPair.second;

            if (workflowDefinition.m_Id != workflowId)
            {
                LOG_APP_WARN("WorkflowRegistry::ValidateAll: workflow map key '{}' != definition id '{}'", workflowId,
                             workflowDefinition.m_Id);
                allValid = false;
            }

            // Validate tasks
            for (auto const& taskPair : workflowDefinition.m_Tasks)
            {
                std::string const& taskKey = taskPair.first;
                TaskDef const& taskDefinition = taskPair.second;

                if (taskDefinition.m_Id.empty())
                {
                    LOG_APP_WARN("WorkflowRegistry::ValidateAll: workflow '{}' task '{}' has empty id", workflowId, taskKey);
                    allValid = false;
                }

                if (!taskDefinition.m_Id.empty() && taskDefinition.m_Id != taskKey)
                {
                    LOG_APP_WARN("WorkflowRegistry::ValidateAll: workflow '{}' task key '{}' != task id '{}'", workflowId,
                                 taskKey, taskDefinition.m_Id);
                    allValid = false;
                }

                // Validate depends_on references exist
                for (std::string const& dependencyTaskId : taskDefinition.m_DependsOn)
                {
                    if (workflowDefinition.m_Tasks.find(dependencyTaskId) == workflowDefinition.m_Tasks.end())
                    {
                        LOG_APP_WARN(
                            "WorkflowRegistry::ValidateAll: workflow '{}' task '{}' depends_on '{}' which does not exist",
                            workflowId, taskKey, dependencyTaskId);
                        allValid = false;
                    }
                }
            }
        }

        return allValid;
    }

} // namespace AIAssistant
