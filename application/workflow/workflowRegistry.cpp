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

#include "auxiliary/file.h"
#include "engine.h"
#include "core.h"
#include "file/pathConfinement.h"
#include "simdjson/simdjson.h"
#include "workflow/jcwfContainer.h"
#include "workflow/workflowJsonParser.h"

#include <algorithm>
#include <fstream>
#include <sstream>

static bool ReadFileToStringStatic(std::filesystem::path const& filePath, std::string& outText)
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
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Pull the resolved provider name from an ai_call task's `m_ParamsJson`.
    // Mirrors what AiCallTaskExecutor::TryExtractStringParam does for the same
    // field at dispatch time — kept inline (not extracted to a shared helper
    // yet — `feedback_cpp_discipline` "extract before the third site"; today
    // there are two read sites, executor + registry).  Returns the empty
    // string for tasks that don't specify a provider; that's the wire
    // contract — empty in `m_RequiredAiProviders` means "system default".
    std::string ExtractProviderFromParams(std::string const& paramsJson)
    {
        if (paramsJson.empty()) return {};
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(paramsJson);
        auto docResult = parser.iterate(padded);
        if (docResult.error()) return {};
        simdjson::ondemand::document doc = std::move(docResult.value());
        std::string_view sv;
        if (doc["provider"].get_string().get(sv) != simdjson::SUCCESS) return {};
        return std::string(sv);
    }
}

namespace AIAssistant
{
    namespace
    {
        // -----------------------------------------------------------------
        // AI provider availability helpers
        // -----------------------------------------------------------------
    } // namespace

    void WorkflowRegistry::Clear()
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_Workflows.clear();
        m_BrokenWorkflows.clear();
        m_LastContainerError.clear();
    }

    bool WorkflowRegistry::LoadDirectory(std::filesystem::path const& workflowsDirectoryPath)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        // Inline the Clear() body — calling Clear() here would re-acquire the
        // (non-recursive) mutex and deadlock.
        m_Workflows.clear();
        m_BrokenWorkflows.clear();
        m_LastContainerError.clear();

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

        // First pass: load .jcwf zip containers (non-recursive — containers are at the top level).
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
            std::string const extension = filePath.extension().string();

            if (extension == ".jcwf")
            {
                try
                {
                    if (!LoadContainer(filePath))
                    {
                        BrokenWorkflow broken;
                        broken.m_ContainerPath = filePath.string();
                        broken.m_Stem = filePath.stem().string();
                        broken.m_Error = m_LastContainerError.empty()
                            ? "Failed to parse .jcwf container (see application log for details)"
                            : m_LastContainerError;
                        m_BrokenWorkflows.push_back(std::move(broken));
                    }
                }
                catch (std::exception const& ex)
                {
                    LOG_APP_ERROR("WorkflowRegistry::LoadDirectory: uncaught exception loading '{}': {}",
                                  filePath.string(), ex.what());
                    BrokenWorkflow broken;
                    broken.m_ContainerPath = filePath.string();
                    broken.m_Stem = filePath.stem().string();
                    broken.m_Error = std::string("Exception: ") + ex.what();
                    m_BrokenWorkflows.push_back(std::move(broken));
                }
            }
        }

        // Empty directories are valid - no workflows is not an error
        return true;
    }

    std::vector<std::string> WorkflowRegistry::GetWorkflowIds() const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
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
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        auto const iterator = m_Workflows.find(workflowId);
        if (iterator == m_Workflows.end())
        {
            return std::nullopt;
        }

        return iterator->second; // copy (keeps call sites simple)
    }

    std::vector<WorkflowRegistry::BrokenWorkflow> WorkflowRegistry::GetBrokenWorkflows() const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        return m_BrokenWorkflows; // by-value snapshot
    }

    // LoadWorkflowFile is deprecated — .jcwf files are always zip containers now.
    // All loading goes through LoadContainer(). Kept as a stub for the header declaration.
    bool WorkflowRegistry::LoadWorkflowFile(std::filesystem::path const& workflowFilePath)
    {
        LOG_APP_WARN("WorkflowRegistry::LoadWorkflowFile is deprecated. "
                     ".jcwf files are zip containers — use LoadContainer(). File: '{}'",
                     workflowFilePath.string());
        return false;
    }

    bool WorkflowRegistry::ValidateAll() const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
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

std::optional<std::string> WorkflowRegistry::TryGetWorkflowFilePathAbsolute(std::string const& workflowId) const
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);
    auto const iterator = m_Workflows.find(workflowId);
    if (iterator == m_Workflows.end())
    {
        return std::nullopt;
    }

    std::string const& workflowFilePathAbsolute = iterator->second.m_WorkflowFilePathAbsolute;
    if (workflowFilePathAbsolute.empty())
    {
        return std::nullopt;
    }

    return workflowFilePathAbsolute;
}

std::optional<std::string> WorkflowRegistry::TryGetWorkflowIdByFilePath(std::string const& absoluteFilePath) const
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);
    return TryGetWorkflowIdByFilePathLocked(absoluteFilePath);
}

std::optional<std::string>
WorkflowRegistry::TryGetWorkflowIdByFilePathLocked(std::string const& absoluteFilePath) const
{
    std::filesystem::path const normalizedTarget = std::filesystem::weakly_canonical(absoluteFilePath);

    for (auto const& [workflowId, workflowDef] : m_Workflows)
    {
        if (workflowDef.m_WorkflowFilePathAbsolute.empty())
        {
            continue;
        }

        std::filesystem::path const normalizedCandidate =
            std::filesystem::weakly_canonical(workflowDef.m_WorkflowFilePathAbsolute);

        if (normalizedTarget == normalizedCandidate)
        {
            return workflowId;
        }

        // For sub-workflows inside containers, workflow_file points to the folder,
        // not the canvas JSON inside it. Match against the directory path as well.
        if (workflowDef.m_IsSubWorkflow && !workflowDef.m_WorkflowFileDirectoryAbsolute.empty())
        {
            std::filesystem::path const normalizedDir =
                std::filesystem::weakly_canonical(workflowDef.m_WorkflowFileDirectoryAbsolute);

            if (normalizedTarget == normalizedDir)
            {
                return workflowId;
            }
        }
    }

    return std::nullopt;
}

bool WorkflowRegistry::SaveOrUpdateWorkflowFromJson(std::string const& workflowJson,
                                                    std::filesystem::path const& workflowFilePathAbsolute,
                                                    std::string& errorMessage)
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);
    errorMessage.clear();

    if (workflowFilePathAbsolute.empty())
    {
        errorMessage = "workflowFilePathAbsolute is empty";
        return false;
    }

    // Path containment.  workflowFilePathAbsolute reaches us from the REST
    // editor surface (PUT /api/workflows/<id>) and from AdhocWorkflowManager::Stage.
    // A malicious caller could pass `..`-laced paths or absolute paths outside
    // the project root.  Gate via ConfineUnderProjectRoot — empty return means
    // the path doesn't resolve under project root; fail closed with ERROR.
    std::filesystem::path const jcwfPath = ConfineUnderProjectRoot(workflowFilePathAbsolute);
    if (jcwfPath.empty())
    {
        errorMessage = "workflowFilePathAbsolute does not resolve under project root: " +
                       workflowFilePathAbsolute.string();
        LOG_APP_ERROR("WorkflowRegistry::SaveOrUpdateWorkflowFromJson: rejected path '{}' — does not resolve "
                      "under project root",
                      workflowFilePathAbsolute.string());
        return false;
    }

    // Parse the incoming JSON to extract the workflow ID.
    // The editor sends a full JCWF JSON (tasks + metadata merged).
    // We split it: metadata → global.json, tasks/dataflow → canvas JSON.
    WorkflowJsonParser parser;
    WorkflowDefinition workflowDefinition;
    if (!parser.ParseWorkflowJson(workflowJson, workflowDefinition, errorMessage))
    {
        return false;
    }

    if (workflowDefinition.m_Id.empty())
    {
        errorMessage = "Workflow id is empty";
        return false;
    }

    std::string const& workflowId = workflowDefinition.m_Id;
    std::filesystem::path const extractedDir = jcwfPath.parent_path() / jcwfPath.stem();

    // Create the extracted directory.
    std::error_code ec;
    std::filesystem::create_directories(extractedDir, ec);
    if (ec)
    {
        errorMessage = "Failed to create extraction directory '" +
                       extractedDir.filename().string() + "' (" + ec.message() + ")";
        LOG_APP_ERROR("WorkflowRegistry: create_directories failed path='{}' ec={}",
                      extractedDir.string(), ec.message());
        return false;
    }

    // Multi-step file work (read existing global.json, write canvas JSON, pack
    // .jcwf zip, set path info).  Wrap in try/catch so an exception thrown by
    // the filesystem layer (permissions, disk full, weakly_canonical on a
    // pathological tree) doesn't leak partially-written state into the
    // registry — the m_Workflows insert is the LAST step and only happens on
    // a fully-successful save.
    try
    {
        // Read existing global.json to preserve metadata the editor may not send (label, doc, triggers).
        std::filesystem::path const globalJsonPath = extractedDir / "global.json";
        if (std::filesystem::exists(globalJsonPath))
        {
            std::string existingGlobalContent;
            if (ReadFileToStringStatic(globalJsonPath, existingGlobalContent))
            {
                WorkflowJsonParser globalParser;
                WorkflowDefinition existingGlobal;
                std::string globalParseError;
                if (globalParser.ParseGlobalJson(existingGlobalContent, existingGlobal, globalParseError))
                {
                    if (workflowDefinition.m_Label.empty() && !existingGlobal.m_Label.empty())
                    {
                        workflowDefinition.m_Label = existingGlobal.m_Label;
                    }
                    if (workflowDefinition.m_Doc.empty() && !existingGlobal.m_Doc.empty())
                    {
                        workflowDefinition.m_Doc = existingGlobal.m_Doc;
                    }
                }
            }
        }
        else
        {
            // Build a minimal global.json from the parsed metadata, then write
            // atomically through the shared helper.
            std::ostringstream globalBody;
            globalBody << "{\n"
                       << "  \"version\": \"" << workflowDefinition.m_Version << "\",\n"
                       << "  \"id\": \"" << workflowId << "\",\n"
                       << "  \"manual_start\": " << (workflowDefinition.m_ManualStart ? "true" : "false") << "\n"
                       << "}\n";
            std::string globalWriteError;
            if (!EngineCore::AtomicWriteFile(globalJsonPath, globalBody.str(), globalWriteError))
            {
                errorMessage = "Failed to write global.json: " + globalWriteError;
                LOG_APP_ERROR("WorkflowRegistry::SaveOrUpdateWorkflowFromJson: {} workflow='{}' path='{}'",
                              globalWriteError, workflowId, globalJsonPath.string());
                return false;
            }
        }

        // Write the canvas JSON (the full body — the editor sends tasks + metadata merged,
        // and ParseCanvasJson tolerates extra metadata fields).
        std::filesystem::path const canvasJsonPath = extractedDir / (workflowId + ".json");
        {
            std::string canvasWriteError;
            if (!EngineCore::AtomicWriteFile(canvasJsonPath, workflowJson, canvasWriteError))
            {
                errorMessage = "Failed to write canvas JSON: " + canvasWriteError;
                LOG_APP_ERROR("WorkflowRegistry::SaveOrUpdateWorkflowFromJson: {} workflow='{}' path='{}'",
                              canvasWriteError, workflowId, canvasJsonPath.string());
                return false;
            }
        }

        // Pack the extracted directory into a .jcwf zip container.
        if (!JcwfContainer::Pack(extractedDir, jcwfPath, errorMessage))
        {
            return false;
        }

        // Set path information on the definition.
        std::filesystem::path const absoluteExtracted = std::filesystem::weakly_canonical(extractedDir);
        workflowDefinition.m_WorkflowFilePath = canvasJsonPath.string();
        workflowDefinition.m_WorkflowFilePathAbsolute = std::filesystem::weakly_canonical(canvasJsonPath).string();
        workflowDefinition.m_WorkflowFileDirectory = extractedDir.string();
        workflowDefinition.m_WorkflowFileDirectoryAbsolute = absoluteExtracted.string();
        workflowDefinition.m_WorkflowBaseDirectoryAbsolute = absoluteExtracted.string();
        workflowDefinition.m_ContainerPath = jcwfPath.string();

        // Insert or replace in registry.
        m_Workflows[workflowId] = workflowDefinition;
    }
    catch (std::exception const& e)
    {
        errorMessage = std::string("Exception during workflow save: ") + e.what();
        LOG_APP_ERROR("WorkflowRegistry::SaveOrUpdateWorkflowFromJson: exception workflow='{}' path='{}': {}",
                      workflowId, jcwfPath.string(), e.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unknown exception during workflow save";
        LOG_APP_ERROR("WorkflowRegistry::SaveOrUpdateWorkflowFromJson: unknown exception workflow='{}' path='{}'",
                      workflowId, jcwfPath.string());
        return false;
    }

    LOG_APP_INFO("WorkflowRegistry: saved workflow '{}' as container '{}'", workflowId, jcwfPath.string());
    return true;
}

bool WorkflowRegistry::UpsertJcwfFromZipBytes(std::string const& zipBytes,
                                              std::filesystem::path const& workflowFilePathAbsolute,
                                              std::string& errorMessage)
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);
    errorMessage.clear();

    if (workflowFilePathAbsolute.empty())
    {
        errorMessage = "workflowFilePathAbsolute is empty";
        return false;
    }

    // Zip magic check before any disk I/O.  Catches the historical bug where a
    // plain-JSON body was fed here and silently written to the .jcwf path;
    // simdjson would then refuse to parse the resulting "zip" on next load.
    // PKZip local-file header is 'P' 'K' 0x03 0x04 (RFC1951 / APPNOTE).
    if (zipBytes.size() < 4 || zipBytes[0] != 'P' || zipBytes[1] != 'K' ||
        static_cast<unsigned char>(zipBytes[2]) != 0x03 ||
        static_cast<unsigned char>(zipBytes[3]) != 0x04)
    {
        errorMessage = "Provided bytes are not a valid .jcwf zip (missing PK\\x03\\x04 magic)";
        LOG_APP_ERROR("WorkflowRegistry::UpsertJcwfFromZipBytes: zip-magic check failed path='{}' size={}",
                      workflowFilePathAbsolute.string(), zipBytes.size());
        return false;
    }

    std::filesystem::path const jcwfPath = ConfineUnderProjectRoot(workflowFilePathAbsolute);
    if (jcwfPath.empty())
    {
        errorMessage = "workflowFilePathAbsolute does not resolve under project root: " +
                       workflowFilePathAbsolute.string();
        LOG_APP_ERROR("WorkflowRegistry::UpsertJcwfFromZipBytes: rejected path '{}' — does not resolve "
                      "under project root",
                      workflowFilePathAbsolute.string());
        return false;
    }

    std::filesystem::path const extractedDir = jcwfPath.parent_path() / jcwfPath.stem();
    std::string const canonicalContainerPath = std::filesystem::weakly_canonical(jcwfPath).string();

    // Evict stale registry entries for this container — root + any sub-workflows
    // from the previous version.  Without this, a restore that drops a sub-
    // workflow would leave the dropped sub lingering in `m_Workflows`.
    for (auto iterator = m_Workflows.begin(); iterator != m_Workflows.end();)
    {
        if (iterator->second.m_ContainerPath == canonicalContainerPath)
        {
            iterator = m_Workflows.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }

    std::string writeError;
    if (!EngineCore::AtomicWriteFile(jcwfPath, zipBytes, writeError))
    {
        errorMessage = "Failed to write .jcwf bytes: " + writeError;
        LOG_APP_ERROR("WorkflowRegistry::UpsertJcwfFromZipBytes: {} path='{}'", writeError, jcwfPath.string());
        return false;
    }

    // Wipe the stale extracted dir.  `JcwfContainer::Extract` overwrites
    // existing files but doesn't remove leftover entries — restoring an older
    // version with fewer files would leave stale ones, and stale sub-folders
    // would get loaded as if they belonged to the restored snapshot by
    // `LoadContainerSubWorkflows`.  remove_all is best-effort: a permission
    // failure is logged, and LoadContainer will still attempt extraction.
    std::error_code removeEc;
    std::filesystem::remove_all(extractedDir, removeEc);
    if (removeEc)
    {
        LOG_APP_WARN("WorkflowRegistry::UpsertJcwfFromZipBytes: failed to clean extracted dir '{}': {}",
                     extractedDir.string(), removeEc.message());
    }

    if (!LoadContainer(jcwfPath))
    {
        errorMessage = m_LastContainerError.empty()
                           ? std::string("Failed to load restored container '") + jcwfPath.filename().string() + "'"
                           : m_LastContainerError;
        LOG_APP_ERROR("WorkflowRegistry::UpsertJcwfFromZipBytes: LoadContainer failed path='{}': {}",
                      jcwfPath.string(), errorMessage);
        return false;
    }

    LOG_APP_INFO("WorkflowRegistry: installed restored container '{}'", jcwfPath.string());
    return true;
}

std::expected<void, RegistryError>
WorkflowRegistry::RemoveWorkflow(std::string const& workflowId, bool deleteFile)
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);

    auto const iterator = m_Workflows.find(workflowId);
    if (iterator == m_Workflows.end())
    {
        return std::unexpected(RegistryError::Make(RegistryErrorCode::NotFound,
                                                    "Workflow '" + workflowId + "' not found"));
    }

    std::string const workflowFilePathAbsolute = iterator->second.m_WorkflowFilePathAbsolute;

    m_Workflows.erase(iterator);

    if (deleteFile && !workflowFilePathAbsolute.empty())
    {
        // Path containment on the deletion site — defense in depth: the path
        // was canonicalised at insert time, but rejecting a tree-escape here
        // means a future bug that lets a non-confined path into the registry
        // can't trigger an arbitrary-file delete via RemoveWorkflow.
        std::filesystem::path const confinedPath = ConfineUnderProjectRoot(workflowFilePathAbsolute);
        if (confinedPath.empty())
        {
            LOG_APP_ERROR("WorkflowRegistry::RemoveWorkflow: refused delete workflow='{}' path='{}' — does not "
                          "resolve under project root",
                          workflowId, workflowFilePathAbsolute);
            return std::unexpected(RegistryError::Make(RegistryErrorCode::PathRefused,
                                                        "workflow file path does not resolve under project root: " +
                                                        workflowFilePathAbsolute));
        }
        std::error_code errorCode;
        std::filesystem::remove(confinedPath, errorCode);
        if (errorCode)
        {
            return std::unexpected(RegistryError::Make(RegistryErrorCode::IoError,
                                                        "Failed to delete workflow file '" + workflowFilePathAbsolute +
                                                        "': " + errorCode.message()));
        }
    }

    return {};
}

std::unordered_map<std::string, std::vector<std::string>> WorkflowRegistry::GetSubWorkflowDependencyGraph() const
{
    std::scoped_lock<std::mutex> const lock(m_Mutex);
    std::unordered_map<std::string, std::vector<std::string>> graph;

    for (auto const& [parentId, parentDef] : m_Workflows)
    {
        for (auto const& [taskId, taskDef] : parentDef.m_Tasks)
        {
            if (taskDef.m_Type != TaskType::SubWorkflow || taskDef.m_WorkflowFile.empty())
            {
                continue;
            }

            // Resolve the workflow file path to an absolute path.
            std::filesystem::path const resolvedPath = [&]()
            {
                std::filesystem::path const raw(taskDef.m_WorkflowFile);
                if (raw.is_absolute())
                {
                    return raw;
                }
                return std::filesystem::path(parentDef.m_WorkflowFileDirectoryAbsolute) / raw;
            }();

            std::string const absolutePath = std::filesystem::weakly_canonical(resolvedPath).string();
            // Use the locked variant — m_Mutex is already held by this method.
            std::optional<std::string> const childId = TryGetWorkflowIdByFilePathLocked(absolutePath);

            if (childId.has_value())
            {
                graph[parentId].push_back(childId.value());
            }
        }
    }

    return graph;
}

bool WorkflowRegistry::LoadContainer(std::filesystem::path const& jcwfContainerPath)
{
    m_LastContainerError.clear();
    std::string const stem = jcwfContainerPath.stem().string();
    std::filesystem::path const extractedDir = jcwfContainerPath.parent_path() / stem;

    // Extract if stale or missing.
    if (JcwfContainer::IsExtractedStale(jcwfContainerPath, extractedDir))
    {
        std::string extractError;
        if (!JcwfContainer::Extract(jcwfContainerPath, extractedDir, extractError))
        {
            m_LastContainerError = "Failed to extract: " + extractError;
            LOG_APP_ERROR("WorkflowRegistry: failed to extract container '{}': {}", jcwfContainerPath.string(),
                          extractError);
            return false;
        }
    }

    // Read global.json for metadata.
    std::filesystem::path const globalJsonPath = extractedDir / "global.json";
    WorkflowDefinition globalMetadata;

    if (std::filesystem::exists(globalJsonPath))
    {
        std::string globalContent;
        if (ReadFileToStringStatic(globalJsonPath, globalContent))
        {
            WorkflowJsonParser parser;
            std::string parseError;
            if (!parser.ParseGlobalJson(globalContent, globalMetadata, parseError))
            {
                LOG_APP_WARN("WorkflowRegistry: failed to parse global.json in '{}': {}", stem, parseError);
            }
        }
    }

    // Find the root canvas JSON (any .json that is NOT global.json).
    std::filesystem::path rootCanvasPath;
    std::error_code ec;

    for (auto const& entry : std::filesystem::directory_iterator(extractedDir, ec))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        if (entry.path().extension().string() != ".json")
        {
            continue;
        }
        if (entry.path().filename().string() == "global.json")
        {
            continue;
        }

        rootCanvasPath = entry.path();
        break;
    }

    if (rootCanvasPath.empty())
    {
        m_LastContainerError = "No root canvas JSON found in container";
        LOG_APP_ERROR("WorkflowRegistry: no root canvas JSON found in container '{}'", stem);
        return false;
    }

    // Parse the root canvas.
    std::string canvasContent;
    if (!ReadFileToStringStatic(rootCanvasPath, canvasContent))
    {
        m_LastContainerError = "Failed to read root canvas: " + rootCanvasPath.string();
        LOG_APP_ERROR("WorkflowRegistry: failed to read root canvas '{}'", rootCanvasPath.string());
        return false;
    }

    WorkflowDefinition rootDef = globalMetadata; // Start with global metadata.
    WorkflowJsonParser parser;
    std::string parseError;

    if (!parser.ParseCanvasJson(canvasContent, rootDef, parseError))
    {
        m_LastContainerError = parseError;
        LOG_APP_ERROR("WorkflowRegistry: failed to parse root canvas '{}': {}", rootCanvasPath.string(), parseError);
        return false;
    }

    // If global.json didn't provide an id, use the stem.
    if (rootDef.m_Id.empty())
    {
        rootDef.m_Id = stem;
    }

    // Set path information.
    std::filesystem::path const absoluteExtracted = std::filesystem::weakly_canonical(extractedDir);
    rootDef.m_WorkflowFilePath = rootCanvasPath.string();
    rootDef.m_WorkflowFileDirectory = extractedDir.string();
    rootDef.m_WorkflowFilePathAbsolute = std::filesystem::weakly_canonical(rootCanvasPath).string();
    rootDef.m_WorkflowFileDirectoryAbsolute = absoluteExtracted.string();
    rootDef.m_WorkflowBaseDirectoryAbsolute = absoluteExtracted.string();
    rootDef.m_ContainerPath = std::filesystem::weakly_canonical(jcwfContainerPath).string();
    rootDef.m_IsSubWorkflow = false;

    // Default trigger if none provided.
    if (rootDef.m_Triggers.empty())
    {
        WorkflowTrigger autoTrigger;
        autoTrigger.m_Type = WorkflowTriggerType::Auto;
        autoTrigger.m_Id = "auto";
        autoTrigger.m_IsEnabled = true;
        autoTrigger.m_ParamsJson = "{}";
        rootDef.m_Triggers.push_back(autoTrigger);
    }

    // Check for AI call tasks + collect every distinct provider name.  The
    // dashboard's per-row hazard glyph (Sitting-7 Workstream C) reads this list
    // via /api/workflows::interface_names to mark rows red when their providers
    // are degraded.  Distinct-set (not multiset) — UI cares which interfaces
    // are touched, not how many tasks each one feeds.
    {
        std::vector<std::string> seen;     // small-N (typical workflow has 1-3 providers); linear-scan beats a hash set.
        for (auto const& [taskId, task] : rootDef.m_Tasks)
        {
            if (task.m_Type != TaskType::AiCall) continue;
            rootDef.m_HasAiCallTasks = true;
            std::string const providerName = ExtractProviderFromParams(task.m_ParamsJson);
            if (std::find(seen.begin(), seen.end(), providerName) == seen.end())
            {
                seen.push_back(providerName);
            }
        }
        rootDef.m_RequiredAiProviders = std::move(seen);
    }

    std::string const rootId = rootDef.m_Id;
    m_Workflows[rootId] = std::move(rootDef);

    LOG_APP_INFO("WorkflowRegistry: loaded container '{}' as workflow '{}' from '{}'", stem, rootId,
                 jcwfContainerPath.string());

    // Recursively load sub-workflows from subfolders.  A sub-workflow load failure
    // is non-fatal for the container — the root + already-loaded subs stay registered;
    // the failure is logged inside the recursive helper.
    (void)LoadContainerSubWorkflows(extractedDir, rootId, m_Workflows[rootId].m_ContainerPath, "", globalMetadata);

    return true;
}

bool WorkflowRegistry::LoadContainerSubWorkflows(std::filesystem::path const& folderPath,
                                                  std::string const& parentWorkflowId,
                                                  std::string const& containerPath,
                                                  std::string const& relativeFolderPath,
                                                  WorkflowDefinition const& globalMetadata)
{
    std::error_code ec;

    for (auto const& entry : std::filesystem::directory_iterator(folderPath, ec))
    {
        if (!entry.is_directory())
        {
            continue;
        }

        std::string const folderName = entry.path().filename().string();
        std::string const subRelPath = relativeFolderPath.empty() ? folderName : (relativeFolderPath + "/" + folderName);

        // Look for a .json file inside this subfolder (named after the folder, or any non-global .json).
        std::filesystem::path canvasPath;
        std::filesystem::path preferredPath = entry.path() / (folderName + ".json");

        if (std::filesystem::exists(preferredPath))
        {
            canvasPath = preferredPath;
        }
        else
        {
            // Fall back to any .json file that isn't global.json.
            for (auto const& subEntry : std::filesystem::directory_iterator(entry.path(), ec))
            {
                if (!subEntry.is_regular_file())
                {
                    continue;
                }
                if (subEntry.path().extension().string() != ".json")
                {
                    continue;
                }
                if (subEntry.path().filename().string() == "global.json")
                {
                    continue;
                }

                canvasPath = subEntry.path();
                break;
            }
        }

        if (canvasPath.empty())
        {
            LOG_APP_WARN("WorkflowRegistry: sub-workflow folder '{}' has no canvas JSON, skipping", subRelPath);
            continue;
        }

        // Parse the sub-workflow canvas.
        std::string canvasContent;
        if (!ReadFileToStringStatic(canvasPath, canvasContent))
        {
            LOG_APP_WARN("WorkflowRegistry: failed to read sub-workflow canvas '{}'", canvasPath.string());
            continue;
        }

        // Start with global defaults (version, defaults) but NOT triggers/manual_start.
        WorkflowDefinition subDef;
        subDef.m_Version = globalMetadata.m_Version;
        subDef.m_Defaults = globalMetadata.m_Defaults;
        subDef.m_DefaultsJson = globalMetadata.m_DefaultsJson;

        WorkflowJsonParser parser;
        std::string parseError;

        bool parsedOk = false;
        try
        {
            parsedOk = parser.ParseCanvasJson(canvasContent, subDef, parseError);
        }
        catch (std::exception const& ex)
        {
            LOG_APP_WARN("WorkflowRegistry: exception parsing sub-workflow '{}': {}", subRelPath, ex.what());
            continue;
        }

        if (!parsedOk)
        {
            LOG_APP_WARN("WorkflowRegistry: failed to parse sub-workflow '{}': {}", subRelPath, parseError);
            continue;
        }

        // Generate a stable ID: parent__subfolder (double underscore separator).
        subDef.m_Id = parentWorkflowId + "__" + folderName;
        subDef.m_Label = folderName;

        // Set path information.
        std::filesystem::path const absoluteFolder = std::filesystem::weakly_canonical(entry.path());
        subDef.m_WorkflowFilePath = canvasPath.string();
        subDef.m_WorkflowFileDirectory = entry.path().string();
        subDef.m_WorkflowFilePathAbsolute = std::filesystem::weakly_canonical(canvasPath).string();
        subDef.m_WorkflowFileDirectoryAbsolute = absoluteFolder.string();
        subDef.m_WorkflowBaseDirectoryAbsolute = absoluteFolder.string();
        subDef.m_ContainerPath = containerPath;
        subDef.m_IsSubWorkflow = true;
        subDef.m_ParentWorkflowId = parentWorkflowId;
        subDef.m_ContainerFolderPath = subRelPath;

        // Sub-workflows have no triggers — they are invoked by parent.
        subDef.m_ManualStart = true;
        subDef.m_Triggers.clear();

        // Check for AI call tasks + collect distinct provider names — see the
        // matching root-load comment above for the rationale.
        {
            std::vector<std::string> seen;
            for (auto const& [taskId, task] : subDef.m_Tasks)
            {
                if (task.m_Type != TaskType::AiCall) continue;
                subDef.m_HasAiCallTasks = true;
                std::string const providerName = ExtractProviderFromParams(task.m_ParamsJson);
                if (std::find(seen.begin(), seen.end(), providerName) == seen.end())
                {
                    seen.push_back(providerName);
                }
            }
            subDef.m_RequiredAiProviders = std::move(seen);
        }

        std::string const subId = subDef.m_Id;
        m_Workflows[subId] = std::move(subDef);

        LOG_APP_INFO("WorkflowRegistry: loaded sub-workflow '{}' (folder '{}') from container", subId, subRelPath);

        // Recurse into nested sub-workflows — a nested-load failure is non-fatal
        // for the parent (already registered above); the failure is logged in the
        // deeper recursion.
        (void)LoadContainerSubWorkflows(entry.path(), subId, containerPath, subRelPath, globalMetadata);
    }

    return true;
}

// namespace AIAssistant