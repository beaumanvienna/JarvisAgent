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

#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    class MemoryStore;
    class WorkflowRegistry;
    class WorkflowRuntimeManager;

    // -----------------------------------------------------------------
    // Tool definition
    // -----------------------------------------------------------------

    struct ToolDef
    {
        std::string name;
        std::string description;
        std::string argsDescription; // human-readable arg description for the system prompt
        bool requiresApproval{false};
    };

    // -----------------------------------------------------------------
    // Tool call (parsed from AI response)
    // -----------------------------------------------------------------

    struct ToolCall
    {
        std::string name;
        std::unordered_map<std::string, std::string> args;
    };

    // -----------------------------------------------------------------
    // Tool result
    // -----------------------------------------------------------------

    struct ToolResult
    {
        std::string toolName;
        bool ok{true};
        std::string output; // text output from tool execution
    };

    // -----------------------------------------------------------------
    // ToolRegistry — registers tools and executes them
    // -----------------------------------------------------------------

    class ToolRegistry
    {
    public:
        ToolRegistry();

        void SetWorkflowRegistry(WorkflowRegistry* registry) { m_WorkflowRegistry = registry; }
        void SetWorkflowRuntimeManager(WorkflowRuntimeManager* manager) { m_RuntimeManager = manager; }
        void SetMemoryStore(MemoryStore* store) { m_MemoryStore = store; }

        // Get all tool definitions (for system prompt injection).
        std::vector<ToolDef> const& GetToolDefs() const { return m_ToolDefs; }

        // Generate the tool descriptions block for the system prompt.
        std::string BuildToolDescriptions() const;

        // Execute a tool call. Returns the result.
        ToolResult Execute(ToolCall const& call);

        // Parse <tool_call> blocks from AI response text.
        // Returns the tool calls found and the response text with tool_call blocks removed.
        static std::vector<ToolCall> ParseToolCalls(std::string const& responseText, std::string& outCleanText);

    private:
        // Tool implementations
        ToolResult ExecGetSystemStatus(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecListWorkflows(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecGetRunStatus(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecListRecentRuns(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecGetTaskOutput(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecRunWorkflow(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecReadFile(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecSearchFiles(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecListFiles(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecGetLogTail(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecSaveMemory(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecRecallMemory(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecListMemories(std::unordered_map<std::string, std::string> const& args);
        ToolResult ExecDeleteMemory(std::unordered_map<std::string, std::string> const& args);

        using ToolFn = std::function<ToolResult(std::unordered_map<std::string, std::string> const&)>;

        std::vector<ToolDef> m_ToolDefs;
        std::unordered_map<std::string, ToolFn> m_ToolFns;

        WorkflowRegistry* m_WorkflowRegistry{nullptr};
        WorkflowRuntimeManager* m_RuntimeManager{nullptr};
        MemoryStore* m_MemoryStore{nullptr};

        // Deny-list for read_file (security)
        static bool IsPathDenied(std::string const& path);

        // Max output size for tool results (4 KB)
        static constexpr size_t kMaxToolOutputSize = 4096;

        // Truncate output if too long
        static std::string TruncateOutput(std::string const& output, size_t maxLen = kMaxToolOutputSize);
    };

} // namespace AIAssistant
