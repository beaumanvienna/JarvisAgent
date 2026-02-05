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

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "workflow/workflowTypes.h"

namespace AIAssistant
{
    class WorkflowRegistry
    {
    public:
        bool LoadDirectory(std::filesystem::path const& workflowsDirectoryPath);
        bool ValidateAll() const;

        std::vector<std::string> GetWorkflowIds() const;
        std::optional<WorkflowDefinition> GetWorkflow(std::string const& workflowId) const;

        // Returns the absolute (normalized) file path for the workflow if known.
        std::optional<std::string> TryGetWorkflowFilePathAbsolute(std::string const& workflowId) const;

        // Save or update a workflow by parsing provided JCWF JSON (canonical format) and storing it on disk.
        // The caller provides the target absolute path (including filename).
        // On success, the registry contains the parsed workflow keyed by its id.
        bool SaveOrUpdateWorkflowFromJson(std::string const& workflowJson,
                                          std::filesystem::path const& workflowFilePathAbsolute, std::string& errorMessage);

        // Remove a workflow from the registry. If deleteFile is true and the registry knows the file path,
        // the file will be deleted as well.
        bool RemoveWorkflow(std::string const& workflowId, bool deleteFile, std::string& errorMessage);

        void Clear();

    private:
        bool LoadWorkflowFile(std::filesystem::path const& workflowFilePath);

        std::unordered_map<std::string, WorkflowDefinition> m_Workflows;
    };
} // namespace AIAssistant
