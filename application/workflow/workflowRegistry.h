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

#pragma once

#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "workflow/registryError.h"
#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    // Owns the in-memory map of registered workflows + their broken-on-parse list.
    //
    // --- Threading & lifetime contract -------------------------------------
    // All public methods are thread-safe — every entry point that reads or
    // mutates `m_Workflows` / `m_BrokenWorkflows` / `m_LastContainerError`
    // acquires `m_Mutex` for the duration of its work.  Private helpers
    // (`LoadWorkflowFile`, `LoadContainer`, `LoadContainerSubWorkflows`) assume
    // the lock is held by the public-method caller and must not re-acquire.
    //
    // Lock-acquire order across components: when a caller holds another
    // pool-level mutex and calls into WorkflowRegistry (e.g.
    // `AdhocWorkflowManager::Stage` → `SaveOrUpdateWorkflowFromJson`), the
    // outer mutex is acquired first.  WorkflowRegistry never calls back into
    // its callers, so there's no inversion risk.
    //
    // Path containment: JCWF write/read paths (the `workflowFilePathAbsolute`
    // arg to `SaveOrUpdateWorkflowFromJson`, the container paths consumed by
    // `LoadContainer` / `LoadContainerSubWorkflows`, and the file deletion in
    // `RemoveWorkflow`) all flow through `application/file/pathConfinement.h`'s
    // `ConfineUnderProjectRoot` helper.  A `..` traversal or symlinked escape
    // returns the empty path and the operation fails closed with an ERROR log.
    class WorkflowRegistry
    {
    public:
        [[nodiscard]] bool LoadDirectory(std::filesystem::path const& workflowsDirectoryPath);
        [[nodiscard]] bool ValidateAll() const;

        std::vector<std::string> GetWorkflowIds() const;
        std::optional<WorkflowDefinition> GetWorkflow(std::string const& workflowId) const;

        // Broken .jcwf files that failed to parse. Keyed by container path.
        struct BrokenWorkflow
        {
            std::string m_ContainerPath; // absolute path to the .jcwf file
            std::string m_Stem;          // filename without extension (display id)
            std::string m_Error;         // parse error message
        };
        // Returns a snapshot copy under the lock.  Returning `const&` would
        // expose the caller to a race: the underlying vector can be mutated by
        // a concurrent `LoadDirectory`/`SaveOrUpdateWorkflowFromJson` after
        // the caller drops the lock implicit in the call.  Copying is cheap
        // (the vector is small — bounded by the count of broken JCWFs in the
        // workflows tree).
        std::vector<BrokenWorkflow> GetBrokenWorkflows() const;

        // Returns the absolute (normalized) file path for the workflow if known.
        std::optional<std::string> TryGetWorkflowFilePathAbsolute(std::string const& workflowId) const;

        // Reverse lookup: find a workflow ID by its absolute file path.
        std::optional<std::string> TryGetWorkflowIdByFilePath(std::string const& absoluteFilePath) const;

        // Returns the sub-workflow dependency graph: workflowId → list of child workflowIds
        // referenced by sub_workflow tasks.
        std::unordered_map<std::string, std::vector<std::string>> GetSubWorkflowDependencyGraph() const;

        // Save or update a workflow by parsing provided JCWF JSON (canonical format) and storing it on disk.
        // The caller provides the target absolute path (including filename).
        // On success, the registry contains the parsed workflow keyed by its id.
        [[nodiscard]] bool SaveOrUpdateWorkflowFromJson(std::string const& workflowJson,
                                                        std::filesystem::path const& workflowFilePathAbsolute,
                                                        std::string& errorMessage);

        // Install a complete .jcwf zip blob at the given target path and register
        // it.  The caller hands us raw zip bytes (e.g. read from a version
        // snapshot under `.history/<id>/<ts>.jcwf`) and we atomically write them
        // to disk, wipe any stale extracted folder + evict prior entries that
        // share this container path, then re-extract and re-register via the
        // standard container ingestion path.  Validates the zip magic (`PK\x03\x04`)
        // before touching disk; path-confines `workflowFilePathAbsolute` under
        // the project root.  Fail-closed on any step.
        [[nodiscard]] bool UpsertJcwfFromZipBytes(std::string const& zipBytes,
                                                   std::filesystem::path const& workflowFilePathAbsolute,
                                                   std::string& errorMessage);

        // Remove a workflow from the registry. If deleteFile is true and the registry knows the file path,
        // the file will be deleted as well.  Returns `{}` on success; on failure carries a typed
        // RegistryError categorising the cause (NotFound / PathRefused / IoError).
        [[nodiscard]] std::expected<void, RegistryError>
        RemoveWorkflow(std::string const& workflowId, bool deleteFile);

        void Clear();

    private:
        // Lock-already-held variant of TryGetWorkflowIdByFilePath, called from
        // GetSubWorkflowDependencyGraph (which holds m_Mutex while iterating).
        // Public TryGetWorkflowIdByFilePath acquires the lock and delegates.
        std::optional<std::string>
        TryGetWorkflowIdByFilePathLocked(std::string const& absoluteFilePath) const;

        [[nodiscard]] bool LoadWorkflowFile(std::filesystem::path const& workflowFilePath);

        // Load a .jcwf zip container: extract, parse global.json + canvas JSONs,
        // register root workflow and all sub-workflows.  Caller holds m_Mutex.
        [[nodiscard]] bool LoadContainer(std::filesystem::path const& jcwfContainerPath);

        // Recursively discover and register sub-workflows within an extracted
        // container folder.  Caller holds m_Mutex.
        [[nodiscard]] bool LoadContainerSubWorkflows(std::filesystem::path const& folderPath,
                                                     std::string const& parentWorkflowId,
                                                     std::string const& containerPath,
                                                     std::string const& relativeFolderPath,
                                                     WorkflowDefinition const& globalMetadata);

        // Single mutex guarding all mutable state (m_Workflows / m_BrokenWorkflows /
        // m_LastContainerError).  Mutable so const accessors can take the lock.
        // Public methods acquire; private LoadContainer / LoadContainerSubWorkflows
        // assume the caller holds the lock.
        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, WorkflowDefinition> m_Workflows;
        std::vector<BrokenWorkflow> m_BrokenWorkflows;
        std::string m_LastContainerError; // set by LoadContainer on failure
    };
} // namespace AIAssistant
