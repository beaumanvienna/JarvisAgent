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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#include "web/aiJcwfService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <set>
#include <sstream>

#include "engine.h"
#include "jarvisAgent.h"
#include "curlWrapper/curlManager.h"
#include "curlWrapper/curlWrapper.h"
#include "file/scriptRegistry.h"
#include "json/jcwfGenerationGuide.generated.h"
#include "json/jcwfSchema.generated.h"
#include "json/jsonHelper.h"
#include "json/replyParser.h"
#include "json/requestBuilder.h"
#include "keys/credential.h"
#include "keys/keyManager.h"
#include "simdjson/simdjson.h"
#include "workflow/aiCallTaskExecutor.h"
#include "workflow/aiInvocation.h"
#include "workflow/aiRequestPool.h"
#include "workflow/workflowJsonParser.h"
#include "workflow/workflowFileIndex.h"
#include "workflow/workflowValidator.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {
        static constexpr uint64_t AI_CALL_TIMEOUT_MS = 120000; // 2 minutes per AI call

        // Workflow IDs used for logging (visible in Run Analyser).
        static constexpr char const* WF_ID_EXPLAIN = "_ai_explain";
        static constexpr char const* WF_ID_GENERATE = "_ai_generate";

        static std::string GenerateRunId(std::string const& workflowId)
        {
            auto const now = std::chrono::system_clock::now();
            auto const nowTimeT = std::chrono::system_clock::to_time_t(now);
            return workflowId + "_" + std::to_string(static_cast<long long>(nowTimeT));
        }

        static fs::path GetQueueBasePath()
        {
            if (Core::g_Core == nullptr)
            {
                return {};
            }
            return fs::absolute(fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath)).lexically_normal();
        }

        static bool WriteFile(fs::path const& filePath, std::string const& content, std::string& outError)
        {
            return AiCallTaskExecutor::WriteTextFile(filePath.string(), content, outError);
        }

        // Resolve the JCWF-configured AI interface name from config.json.
        // Returns empty when -1 (use global default) or index is out of range — in which case
        // AiRequestPool::Submit falls back to the default interface.
        static std::string ResolveJcwfInterfaceName()
        {
            auto const& config = Core::g_Core->GetConfig();
            int const idx = config.m_JcwfAiInterfaceIndex;
            if (idx < 0 || static_cast<size_t>(idx) >= config.m_ApiInterfaces.size())
            {
                return {};
            }
            return config.m_ApiInterfaces[static_cast<size_t>(idx)].m_Name;
        }

        // Detect the host OS/distro for shell script generation prompts.
        static std::string GetHostOsDescription()
        {
#if defined(__APPLE__)
            // macOS: run sw_vers
            FILE* pipe = popen("sw_vers 2>/dev/null", "r");
            if (pipe)
            {
                char buf[256];
                std::string result;
                while (fgets(buf, sizeof(buf), pipe))
                {
                    result += buf;
                }
                pclose(pipe);
                if (!result.empty())
                {
                    // Trim trailing whitespace
                    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                    {
                        result.pop_back();
                    }
                    return result;
                }
            }
            return "macOS (version unknown)";
#elif defined(_WIN32)
            return "Windows — generate PowerShell (.ps1) scripts. "
                   "Use PowerShell conventions: param([string]$Arg1, ...) for positional args, "
                   "Set-StrictMode -Version Latest, $ErrorActionPreference = 'Stop', "
                   "Write-Output / Copy-Item / Remove-Item cmdlets, Join-Path for paths. "
                   "No shebang, no POSIX awk/sed/grep, no single-quote shell escaping.";
#else
            // Linux: read first 5 lines of /etc/os-release
            std::ifstream osRelease("/etc/os-release");
            if (osRelease)
            {
                std::string result;
                std::string line;
                int count = 0;
                while (count < 5 && std::getline(osRelease, line))
                {
                    if (!result.empty())
                    {
                        result += '\n';
                    }
                    result += line;
                    ++count;
                }
                if (!result.empty())
                {
                    return result;
                }
            }
            return "Linux (distro unknown)";
#endif
        }

        // GeneratedScript is defined in workflow/workflowValidator.h

        // Extract script paths referenced by shell/python tasks in a JCWF JSON string.
        static std::vector<std::string> ExtractScriptPaths(std::string const& jcwfJson)
        {
            std::vector<std::string> paths;

            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(jcwfJson);
            simdjson::ondemand::document doc;
            if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
            {
                return paths;
            }

            simdjson::ondemand::object tasksObj;
            if (doc["tasks"].get_object().get(tasksObj) != simdjson::SUCCESS)
            {
                return paths;
            }

            for (auto field : tasksObj)
            {
                simdjson::ondemand::object taskObj;
                if (field.value().get_object().get(taskObj) != simdjson::SUCCESS)
                {
                    continue;
                }

                std::string_view typeView;
                if (taskObj["type"].get_string().get(typeView) != simdjson::SUCCESS)
                {
                    continue;
                }

                if (typeView != "shell" && typeView != "python")
                {
                    continue;
                }

                // Shell tasks: extract script path from "params.command" field.
                if (typeView == "shell")
                {
                    simdjson::ondemand::object shellParamsObj;
                    if (taskObj["params"].get_object().get(shellParamsObj) == simdjson::SUCCESS)
                    {
                        std::string_view commandView;
                        if (shellParamsObj["command"].get_string().get(commandView) == simdjson::SUCCESS)
                        {
                            std::string command(commandView);
                            if (command.rfind("scripts/", 0) == 0)
                            {
                                // Strip arguments after the script path (first space-separated token)
                                size_t spacePos = command.find(' ');
                                std::string scriptPath =
                                    (spacePos != std::string::npos) ? command.substr(0, spacePos) : command;
                                paths.push_back(std::move(scriptPath));
                            }
                        }
                    }
                }

                // Python tasks: extract script path from "params.module" field.
                // Module "scripts.parseSSHLog" → file "scripts/parseSSHLog.py".
                if (typeView == "python")
                {
                    simdjson::ondemand::object paramsObj;
                    if (taskObj["params"].get_object().get(paramsObj) == simdjson::SUCCESS)
                    {
                        std::string_view moduleView;
                        if (paramsObj["module"].get_string().get(moduleView) == simdjson::SUCCESS)
                        {
                            std::string modulePath(moduleView);
                            // Convert dots to slashes and append .py
                            for (char& c : modulePath)
                            {
                                if (c == '.')
                                {
                                    c = '/';
                                }
                            }
                            modulePath += ".py";
                            if (modulePath.rfind("scripts/", 0) == 0)
                            {
                                paths.push_back(std::move(modulePath));
                            }
                        }
                    }
                }
            }

            return paths;
        }

        // ----------------------------------------------------------------
        // Shell task I/O info extraction
        // ----------------------------------------------------------------

        struct ShellTaskIoInfo
        {
            std::vector<std::string> fileInputs;
            std::vector<std::string> fileOutputs;
        };

        // Extract file_inputs/file_outputs for each shell script path in the JCWF.
        static std::unordered_map<std::string, ShellTaskIoInfo> ExtractShellTaskIoMap(std::string const& jcwfJson)
        {
            std::unordered_map<std::string, ShellTaskIoInfo> result;

            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(jcwfJson);
            simdjson::ondemand::document doc;
            if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
            {
                return result;
            }

            simdjson::ondemand::object tasksObj;
            if (doc["tasks"].get_object().get(tasksObj) != simdjson::SUCCESS)
            {
                return result;
            }

            for (auto field : tasksObj)
            {
                simdjson::ondemand::object taskObj;
                if (field.value().get_object().get(taskObj) != simdjson::SUCCESS)
                {
                    continue;
                }

                std::string_view typeView;
                if (taskObj["type"].get_string().get(typeView) != simdjson::SUCCESS || typeView != "shell")
                {
                    continue;
                }

                // Extract command path
                std::string scriptPath;
                {
                    simdjson::ondemand::object paramsObj;
                    if (taskObj["params"].get_object().get(paramsObj) != simdjson::SUCCESS)
                    {
                        continue;
                    }
                    std::string_view cmdView;
                    if (paramsObj["command"].get_string().get(cmdView) != simdjson::SUCCESS)
                    {
                        continue;
                    }
                    scriptPath = std::string(cmdView);
                    size_t sp = scriptPath.find(' ');
                    if (sp != std::string::npos)
                    {
                        scriptPath = scriptPath.substr(0, sp);
                    }
                }

                ShellTaskIoInfo info;

                // Extract file_inputs
                simdjson::ondemand::array fiArr;
                if (taskObj["file_inputs"].get_array().get(fiArr) == simdjson::SUCCESS)
                {
                    for (auto entry : fiArr)
                    {
                        std::string_view sv;
                        if (entry.get_string().get(sv) == simdjson::SUCCESS)
                        {
                            info.fileInputs.emplace_back(sv);
                        }
                    }
                }

                // Extract file_outputs
                simdjson::ondemand::array foArr;
                if (taskObj["file_outputs"].get_array().get(foArr) == simdjson::SUCCESS)
                {
                    for (auto entry : foArr)
                    {
                        std::string_view sv;
                        if (entry.get_string().get(sv) == simdjson::SUCCESS)
                        {
                            info.fileOutputs.emplace_back(sv);
                        }
                    }
                }

                result[scriptPath] = std::move(info);
            }

            return result;
        }

        // ----------------------------------------------------------------
        // Script validation helpers
        // ----------------------------------------------------------------

        // Extract a mapping of script file path → expected function name from the JCWF.
        static std::unordered_map<std::string, std::string> ExtractScriptFunctionMap(std::string const& jcwfJson)
        {
            std::unordered_map<std::string, std::string> result;

            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(jcwfJson);
            simdjson::ondemand::document doc;
            if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
            {
                return result;
            }

            simdjson::ondemand::object tasksObj;
            if (doc["tasks"].get_object().get(tasksObj) != simdjson::SUCCESS)
            {
                return result;
            }

            for (auto field : tasksObj)
            {
                simdjson::ondemand::object taskObj;
                if (field.value().get_object().get(taskObj) != simdjson::SUCCESS)
                {
                    continue;
                }

                std::string_view typeView;
                if (taskObj["type"].get_string().get(typeView) != simdjson::SUCCESS)
                {
                    continue;
                }

                if (typeView != "python")
                {
                    continue;
                }

                simdjson::ondemand::object paramsObj;
                if (taskObj["params"].get_object().get(paramsObj) != simdjson::SUCCESS)
                {
                    continue;
                }

                std::string_view moduleView;
                if (paramsObj["module"].get_string().get(moduleView) != simdjson::SUCCESS)
                {
                    continue;
                }

                std::string modulePath(moduleView);
                for (char& c : modulePath)
                {
                    if (c == '.')
                    {
                        c = '/';
                    }
                }
                modulePath += ".py";

                std::string_view funcView;
                if (paramsObj["function"].get_string().get(funcView) == simdjson::SUCCESS)
                {
                    result[modulePath] = std::string(funcView);
                }
            }

            return result;
        }

        struct ScriptValidationResult
        {
            bool passed{true};
            std::vector<std::string> issues;
        };

        // Validate a generated script for structural correctness.
        static ScriptValidationResult ValidateGeneratedScript(std::string const& scriptContent,
                                                              std::string const& expectedFunctionName, bool isPython,
                                                              bool isPowerShell = false)
        {
            ScriptValidationResult vr;

            if (scriptContent.empty())
            {
                vr.passed = false;
                vr.issues.push_back("Script content is empty.");
                return vr;
            }

            std::vector<std::string> lines;
            {
                std::istringstream stream(scriptContent);
                std::string line;
                while (std::getline(stream, line))
                {
                    lines.push_back(line);
                }
            }

            if (lines.empty())
            {
                vr.passed = false;
                vr.issues.push_back("Script has no lines.");
                return vr;
            }

            if (isPython)
            {
                // Check 1: Shebang
                if (lines[0].find("#!/usr/bin/env python3") == std::string::npos &&
                    lines[0].find("#!/usr/bin/python3") == std::string::npos)
                {
                    vr.issues.push_back("Missing Python shebang line. First line should be: #!/usr/bin/env python3");
                }

                // Check 2: @jarvis-script marker (within first 20 lines)
                bool hasMarker = false;
                for (size_t i = 0; i < std::min(lines.size(), size_t(20)); ++i)
                {
                    if (lines[i].find("@jarvis-script") != std::string::npos)
                    {
                        hasMarker = true;
                        break;
                    }
                }
                if (!hasMarker)
                {
                    vr.issues.push_back("Missing '# @jarvis-script' metadata marker in the first 20 lines.");
                }

                // Check 3: @short metadata
                bool hasShort = false;
                for (size_t i = 0; i < std::min(lines.size(), size_t(50)); ++i)
                {
                    if (lines[i].find("@short") != std::string::npos)
                    {
                        hasShort = true;
                        break;
                    }
                }
                if (!hasShort)
                {
                    vr.issues.push_back("Missing '# @short: ...' metadata line.");
                }

                // Check 4: Expected function definition
                if (!expectedFunctionName.empty())
                {
                    std::string const defPattern = "def " + expectedFunctionName + "(";
                    bool hasFuncDef = false;
                    std::string funcDefLine;
                    for (auto const& l : lines)
                    {
                        if (l.find(defPattern) != std::string::npos)
                        {
                            hasFuncDef = true;
                            funcDefLine = l;
                            break;
                        }
                    }
                    if (!hasFuncDef)
                    {
                        vr.issues.push_back("Expected function definition 'def " + expectedFunctionName +
                                            "(...)' not found.");
                    }
                    else if (funcDefLine.find("context") == std::string::npos)
                    {
                        // Check 5: Function should accept context parameter
                        vr.issues.push_back("Function '" + expectedFunctionName +
                                            "' should accept 'context=None' as a parameter.");
                    }
                }
            }
            else if (isPowerShell)
            {
                // PowerShell script checks: no shebang, but require @jarvis-script and Set-StrictMode.
                bool hasMarker = false;
                for (size_t i = 0; i < std::min(lines.size(), size_t(20)); ++i)
                {
                    if (lines[i].find("@jarvis-script") != std::string::npos)
                    {
                        hasMarker = true;
                        break;
                    }
                }
                if (!hasMarker)
                {
                    vr.issues.push_back("Missing '# @jarvis-script' metadata marker in the first 20 lines.");
                }

                bool hasShort = false;
                for (size_t i = 0; i < std::min(lines.size(), size_t(50)); ++i)
                {
                    if (lines[i].find("@short") != std::string::npos)
                    {
                        hasShort = true;
                        break;
                    }
                }
                if (!hasShort)
                {
                    vr.issues.push_back("Missing '# @short: ...' metadata line.");
                }

                bool hasStrictMode = false;
                for (size_t i = 0; i < std::min(lines.size(), size_t(20)); ++i)
                {
                    if (lines[i].find("Set-StrictMode") != std::string::npos)
                    {
                        hasStrictMode = true;
                        break;
                    }
                }
                if (!hasStrictMode)
                {
                    vr.issues.push_back(
                        "Missing 'Set-StrictMode -Version Latest' (PowerShell equivalent of set -euo pipefail).");
                }
            }
            else
            {
                // Shell script checks
                if (lines[0].find("#!/usr/bin/env bash") == std::string::npos &&
                    lines[0].find("#!/bin/bash") == std::string::npos)
                {
                    vr.issues.push_back("Missing bash shebang line. First line should be: #!/usr/bin/env bash");
                }

                bool hasMarker = false;
                for (size_t i = 0; i < std::min(lines.size(), size_t(20)); ++i)
                {
                    if (lines[i].find("@jarvis-script") != std::string::npos)
                    {
                        hasMarker = true;
                        break;
                    }
                }
                if (!hasMarker)
                {
                    vr.issues.push_back("Missing '# @jarvis-script' metadata marker in the first 20 lines.");
                }

                // Check 3: @short metadata
                bool hasShortShell = false;
                for (size_t i = 0; i < std::min(lines.size(), size_t(50)); ++i)
                {
                    if (lines[i].find("@short") != std::string::npos)
                    {
                        hasShortShell = true;
                        break;
                    }
                }
                if (!hasShortShell)
                {
                    vr.issues.push_back("Missing '# @short: ...' metadata line.");
                }

                // Check 4: set -euo pipefail (within first 20 lines, after shebang/metadata)
                bool hasPipefail = false;
                for (size_t i = 0; i < std::min(lines.size(), size_t(20)); ++i)
                {
                    if (lines[i].find("set -euo pipefail") != std::string::npos ||
                        lines[i].find("set -euxo pipefail") != std::string::npos)
                    {
                        hasPipefail = true;
                        break;
                    }
                }
                if (!hasPipefail)
                {
                    vr.issues.push_back("Missing 'set -euo pipefail' (or 'set -euxo pipefail') after the metadata header.");
                }
            }

            vr.passed = vr.issues.empty();
            return vr;
        }

        // ----------------------------------------------------------------
        // Chunked generation helpers
        // ----------------------------------------------------------------

        static constexpr size_t DEFAULT_BATCH_SIZE = 10;

        static std::string StripMarkdownFences(std::string const& input)
        {
            std::string trimmed = input;
            size_t start = trimmed.find_first_not_of(" \t\n\r");
            if (start != std::string::npos)
            {
                trimmed = trimmed.substr(start);
            }
            if (trimmed.rfind("```", 0) == 0)
            {
                size_t firstNewline = trimmed.find('\n');
                if (firstNewline != std::string::npos)
                {
                    trimmed = trimmed.substr(firstNewline + 1);
                }
            }
            size_t lastFence = trimmed.rfind("```");
            if (lastFence != std::string::npos && lastFence > 0)
            {
                trimmed = trimmed.substr(0, lastFence);
            }
            size_t end = trimmed.find_last_not_of(" \t\n\r");
            if (end != std::string::npos)
            {
                trimmed = trimmed.substr(0, end + 1);
            }
            return trimmed;
        }

        // Extract task IDs from the decomposition JSON output.
        // The AI outputs { "tasks": { "id1": {...}, "id2": {...} } }.
        static std::vector<std::string> ExtractTaskIdsFromDecomposition(std::string const& decomposition)
        {
            std::vector<std::string> taskIds;

            std::string const cleaned = StripMarkdownFences(decomposition);

            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(cleaned);
            simdjson::ondemand::document doc;
            if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
            {
                return taskIds;
            }

            // Try doc["tasks"] first (wrapped format), then root-level keys (unwrapped).
            // The AI sometimes returns { "id1": {...}, "id2": {...} } without a "tasks" wrapper.
            simdjson::ondemand::object tasksObj;
            bool foundTasks = (doc["tasks"].get_object().get(tasksObj) == simdjson::SUCCESS);

            if (foundTasks)
            {
                for (auto field : tasksObj)
                {
                    std::string_view key;
                    if (field.unescaped_key().get(key) == simdjson::SUCCESS)
                    {
                        taskIds.emplace_back(key);
                    }
                    [[maybe_unused]] auto val = field.value();
                }
            }
            else
            {
                // Fallback: re-parse and iterate root-level keys
                simdjson::ondemand::parser parser2;
                simdjson::padded_string padded2(cleaned);
                simdjson::ondemand::document doc2;
                if (parser2.iterate(padded2).get(doc2) == simdjson::SUCCESS)
                {
                    simdjson::ondemand::object rootObj;
                    if (doc2.get_object().get(rootObj) == simdjson::SUCCESS)
                    {
                        for (auto field : rootObj)
                        {
                            std::string_view key;
                            if (field.unescaped_key().get(key) == simdjson::SUCCESS)
                            {
                                // Skip known workflow-level fields
                                if (key != "version" && key != "id" && key != "label" && key != "doc" && key != "triggers" &&
                                    key != "defaults" && key != "base_directory" && key != "manual_start" &&
                                    key != "control_nodes" && key != "controlflow")
                                {
                                    taskIds.emplace_back(key);
                                }
                            }
                            [[maybe_unused]] auto val = field.value();
                        }
                    }
                }
            }

            return taskIds;
        }

        // Find the matching closing brace for an opening brace at startPos.
        // Respects JSON string escaping. Returns npos if not found.
        static size_t FindMatchingBrace(std::string const& json, size_t startPos)
        {
            if (startPos >= json.size() || json[startPos] != '{')
            {
                return std::string::npos;
            }
            int depth = 1;
            bool inStr = false;
            for (size_t i = startPos + 1; i < json.size(); ++i)
            {
                char c = json[i];
                if (c == '\\' && inStr)
                {
                    ++i;
                    continue;
                }
                if (c == '"')
                {
                    inStr = !inStr;
                    continue;
                }
                if (inStr)
                {
                    continue;
                }
                if (c == '{')
                {
                    ++depth;
                }
                else if (c == '}')
                {
                    if (--depth == 0)
                    {
                        return i;
                    }
                }
            }
            return std::string::npos;
        }

        // Find the opening and closing brace positions of the "tasks" object.
        static std::pair<size_t, size_t> FindTasksObject(std::string const& json)
        {
            size_t keyPos = json.find("\"tasks\"");
            if (keyPos == std::string::npos)
            {
                return {std::string::npos, std::string::npos};
            }
            size_t colonPos = json.find(':', keyPos + 7);
            if (colonPos == std::string::npos)
            {
                return {std::string::npos, std::string::npos};
            }
            size_t openPos = json.find('{', colonPos + 1);
            if (openPos == std::string::npos)
            {
                return {std::string::npos, std::string::npos};
            }
            size_t closePos = FindMatchingBrace(json, openPos);
            return {openPos, closePos};
        }

        // Extract the inner content of the "tasks" object (between { and }).
        static std::string ExtractTasksInnerContent(std::string const& json)
        {
            auto [openPos, closePos] = FindTasksObject(json);
            if (openPos == std::string::npos || closePos == std::string::npos)
            {
                return "";
            }
            return json.substr(openPos + 1, closePos - openPos - 1);
        }

        // Merge multiple JCWF JSON fragments into a single JCWF.
        // fragments[0] must be a complete JCWF (workflow-level fields + first batch of tasks).
        // fragments[1..N] contain { "tasks": { ... } } with additional task entries.
        static std::string MergeJcwfFragments(std::vector<std::string> const& fragments)
        {
            if (fragments.empty())
            {
                return "{}";
            }
            if (fragments.size() == 1)
            {
                return fragments[0];
            }

            std::string result = fragments[0];

            for (size_t i = 1; i < fragments.size(); ++i)
            {
                std::string additionalTasks = ExtractTasksInnerContent(fragments[i]);

                // Trim whitespace from the extracted content
                size_t first = additionalTasks.find_first_not_of(" \t\n\r");
                size_t last = additionalTasks.find_last_not_of(" \t\n\r");
                if (first == std::string::npos || last == std::string::npos)
                {
                    continue;
                }
                additionalTasks = additionalTasks.substr(first, last - first + 1);
                if (additionalTasks.empty())
                {
                    continue;
                }

                // Remove trailing comma if present
                if (!additionalTasks.empty() && additionalTasks.back() == ',')
                {
                    additionalTasks.pop_back();
                }

                // Find where to insert in the result's tasks object
                auto [openPos, closePos] = FindTasksObject(result);
                if (closePos == std::string::npos)
                {
                    LOG_APP_WARN("[AiJcwfService] MergeJcwfFragments: cannot find tasks object in base for fragment {}", i);
                    continue;
                }

                // Check if the tasks object already has content
                std::string existingInner = result.substr(openPos + 1, closePos - openPos - 1);
                size_t existingFirst = existingInner.find_first_not_of(" \t\n\r");
                bool hasExisting = (existingFirst != std::string::npos);

                std::string insertion = (hasExisting ? ",\n" : "\n") + additionalTasks;
                result.insert(closePos, insertion);
            }

            return result;
        }

        // Find the position range of a task block in the "tasks" object.
        // Returns {keyStart, blockEnd} where keyStart is the position of the opening quote
        // of the key, and blockEnd is the position of the closing brace.
        static std::pair<size_t, size_t> FindTaskBlockBounds(std::string const& jcwf, std::string const& taskId)
        {
            auto [tasksOpen, tasksClose] = FindTasksObject(jcwf);
            if (tasksOpen == std::string::npos)
            {
                return {std::string::npos, std::string::npos};
            }

            std::string const searchKey = "\"" + taskId + "\"";
            size_t searchStart = tasksOpen + 1;

            while (searchStart < tasksClose)
            {
                size_t keyPos = jcwf.find(searchKey, searchStart);
                if (keyPos == std::string::npos || keyPos >= tasksClose)
                {
                    return {std::string::npos, std::string::npos};
                }

                // Verify a colon follows (skipping whitespace)
                size_t afterKey = keyPos + searchKey.size();
                size_t colonPos = std::string::npos;
                for (size_t i = afterKey; i < tasksClose; ++i)
                {
                    if (jcwf[i] == ':')
                    {
                        colonPos = i;
                        break;
                    }
                    if (jcwf[i] != ' ' && jcwf[i] != '\t' && jcwf[i] != '\n' && jcwf[i] != '\r')
                    {
                        break;
                    }
                }

                if (colonPos == std::string::npos)
                {
                    searchStart = afterKey;
                    continue;
                }

                // Find opening brace of the task object
                size_t taskOpen = std::string::npos;
                for (size_t i = colonPos + 1; i < tasksClose; ++i)
                {
                    if (jcwf[i] == '{')
                    {
                        taskOpen = i;
                        break;
                    }
                    if (jcwf[i] != ' ' && jcwf[i] != '\t' && jcwf[i] != '\n' && jcwf[i] != '\r')
                    {
                        break;
                    }
                }

                if (taskOpen == std::string::npos)
                {
                    searchStart = afterKey;
                    continue;
                }

                size_t taskClose = FindMatchingBrace(jcwf, taskOpen);
                if (taskClose == std::string::npos)
                {
                    return {std::string::npos, std::string::npos};
                }

                return {taskOpen, taskClose};
            }

            return {std::string::npos, std::string::npos};
        }

        // Extract the raw JSON block for a specific task from a JCWF string.
        static std::string ExtractTaskBlock(std::string const& jcwf, std::string const& taskId)
        {
            auto [blockStart, blockEnd] = FindTaskBlockBounds(jcwf, taskId);
            if (blockStart == std::string::npos || blockEnd == std::string::npos)
            {
                return "";
            }
            return jcwf.substr(blockStart, blockEnd - blockStart + 1);
        }

        // Replace task blocks in the base JCWF with those from a patch JSON.
        // The patch should be { "tasks": { "id1": {...}, "id2": {...} } }.
        // Tasks not in the patch are left unchanged.
        static std::string PatchTasksIntoJcwf(std::string const& baseJcwf, std::string const& patchJson)
        {
            std::string const cleanPatch = StripMarkdownFences(patchJson);

            // Extract task IDs from the patch
            std::vector<std::string> patchTaskIds = ExtractTaskIdsFromDecomposition(cleanPatch);
            if (patchTaskIds.empty())
            {
                LOG_APP_WARN("[AiJcwfService] PatchTasksIntoJcwf: no task IDs found in patch");
                return baseJcwf;
            }

            std::string result = baseJcwf;

            for (auto const& taskId : patchTaskIds)
            {
                std::string newBlock = ExtractTaskBlock(cleanPatch, taskId);
                if (newBlock.empty())
                {
                    LOG_APP_WARN("[AiJcwfService] PatchTasksIntoJcwf: could not extract block for task '{}'", taskId);
                    continue;
                }

                auto [blockStart, blockEnd] = FindTaskBlockBounds(result, taskId);
                if (blockStart == std::string::npos)
                {
                    LOG_APP_WARN("[AiJcwfService] PatchTasksIntoJcwf: task '{}' not found in base — skipping", taskId);
                    continue;
                }

                // Replace the old block with the new one
                result.replace(blockStart, blockEnd - blockStart + 1, newBlock);
                LOG_APP_INFO("[AiJcwfService] PatchTasksIntoJcwf: patched task '{}'", taskId);
            }

            return result;
        }

    } // namespace

    AiJcwfService::~AiJcwfService() { Shutdown(); }

    void AiJcwfService::Shutdown()
    {
        // Two-phase shutdown to avoid joining-under-lock deadlock.  A background
        // thread that calls back into a public method (e.g. a future change adds
        // `JoinFinishedThreads` mid-lambda) would re-acquire `m_ThreadsMutex` and
        // deadlock against this join.  Holding the lock only while we move-out
        // the vector keeps the public-method ↔ Shutdown contract safe even if
        // future code calls Shutdown from a path that ALSO touches m_ThreadsMutex.
        //
        // Idempotency: this method may be called multiple times (~AiJcwfService
        // is the canonical caller; an external explicit Shutdown is also
        // permitted).  Second-call observes an already-empty vector, no-ops the
        // join loop, and re-sets m_ShuttingDown (already true).  No double-join
        // because the threads were already moved-out and joined on the first
        // call.
        m_ShuttingDown.store(true);
        std::vector<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> lock(m_ThreadsMutex);
            toJoin = std::move(m_BackgroundThreads);
            m_BackgroundThreads.clear(); // moved-from vector is in valid-unspecified state; clear to canonicalize.
        }
        for (auto& thread : toJoin)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    }

    void AiJcwfService::SetBroadcastFn(BroadcastFn broadcastFn)
    {
        std::lock_guard<std::mutex> lock(m_BroadcastMutex);
        m_BroadcastFn = std::move(broadcastFn);
    }

    void AiJcwfService::Broadcast(std::string const& jsonString)
    {
        std::lock_guard<std::mutex> lock(m_BroadcastMutex);
        if (m_BroadcastFn)
        {
            LOG_APP_INFO("[AiJcwfService] Broadcast: queuing message (len={}, preview='{}')", jsonString.size(),
                         jsonString.substr(0, 120));
            m_BroadcastFn(jsonString);
        }
        else
        {
            LOG_APP_WARN("[AiJcwfService] Broadcast: m_BroadcastFn is null, message dropped (len={})", jsonString.size());
        }
    }

    void AiJcwfService::JoinFinishedThreads()
    {
        // Thread count is bounded by user-initiated requests (one per button click).
        // Threads are joined in Shutdown(). No active cleanup needed here.
    }

    std::string AiJcwfService::LoadGenerationGuide()
    {
        // Compiled-in via Phase 4 prebuild step (tools/generateEmbeddedHeaders.py reads
        // doc/jcwf_generation_guide.md at build time).  No more disk lookups, no more
        // silent-fallback-to-placeholder path.
        return std::string{kJcwfGenerationGuide};
    }

    bool AiJcwfService::ValidateJcwf(std::string const& jcwfJsonText, std::string& outValidationSummary,
                                     ScriptRegistry const* scriptRegistry,
                                     std::vector<GeneratedScript> const* pendingScripts,
                                     std::vector<WorkflowValidationIssue>* outIssues)
    {
        outValidationSummary.clear();

        WorkflowJsonParser parser;
        WorkflowDefinition parsedWorkflow;
        if (auto r = parser.ParseWorkflowJson(jcwfJsonText, parsedWorkflow); !r)
        {
            outValidationSummary = "Parse error: " + r.error().m_Details;
            return false;
        }

        WorkflowFileIndex const* fileIndex = nullptr;
        if (JarvisAgent* app = App::g_App.load(std::memory_order_acquire); app != nullptr)
        {
            fileIndex = app->GetWorkflowFileIndex();
        }

        std::vector<WorkflowValidationIssue> issues;
        WorkflowValidator::Validate(parsedWorkflow, scriptRegistry, pendingScripts, issues, fileIndex);

        if (outIssues != nullptr)
        {
            *outIssues = issues;
        }

        bool hasErrors = false;
        bool hasWarnings = false;

        std::ostringstream ss;
        for (auto const& issue : issues)
        {
            if (issue.m_Severity == WorkflowValidationSeverity::Error)
            {
                ss << "ERROR [" << issue.m_Code << "]: " << issue.m_Message;
                hasErrors = true;
            }
            else if (issue.m_Severity == WorkflowValidationSeverity::Warning)
            {
                ss << "WARNING [" << issue.m_Code << "]: " << issue.m_Message;
                hasWarnings = true;
            }
            else
            {
                continue;
            }

            if (!issue.m_Path.empty())
            {
                ss << " (path: " << issue.m_Path << ")";
            }
            if (!issue.m_TaskId.empty())
            {
                ss << " (task: " << issue.m_TaskId << ")";
            }
            ss << "\n";
            if (!issue.m_SuggestedFix.empty())
            {
                ss << "  FIX: " << issue.m_SuggestedFix << "\n";
            }
            if (!issue.m_Context.empty())
            {
                ss << "  CONTEXT: " << issue.m_Context << "\n";
            }
        }

        if (!hasErrors && !hasWarnings)
        {
            return true;
        }

        outValidationSummary = ss.str();
        return !hasErrors;
    }

    bool AiJcwfService::RunSingleAiCall(std::string const& subfolderName, std::string const& stngContent,
                                        std::string const& taskContent, std::string const& cntxContent,
                                        std::string const& probContent, std::string& outResponseText,
                                        std::string& outError, std::string const& outputSchemaJson)
    {
        outResponseText.clear();
        outError.clear();

        JarvisAgent* app = App::g_App.load(std::memory_order_acquire);
        if (app == nullptr)
        {
            outError = "Application not available";
            return false;
        }

        AiRequestPool* requestPool = app->GetAiRequestPool();
        if (requestPool == nullptr)
        {
            outError = "AiRequestPool not available";
            return false;
        }

        if (Core::g_Core == nullptr)
        {
            outError = "Core not available";
            return false;
        }

        fs::path const queueBase = GetQueueBasePath();
        if (queueBase.empty())
        {
            outError = "Queue base path is not configured";
            return false;
        }

        // Create a disk subfolder for transcripts + .output.* files.  Disk-first philosophy
        // preserved; no longer load-bearing for dispatch — the envelope carries the content.
        fs::path const queueDir = queueBase / "_ai_jcwf_service" / subfolderName;
        std::error_code ec;
        fs::create_directories(queueDir, ec);
        if (ec)
        {
            outError = "Failed to create queue directory: " + queueDir.string() + " (" + ec.message() + ")";
            return false;
        }

        // Clean any previous files in the directory.
        for (auto const& entry : fs::directory_iterator(queueDir, ec))
        {
            fs::remove(entry.path(), ec);
        }

        // Write the environment files to disk for replay/debug.  Empty sections are skipped.
        std::string writeError;
        if (!stngContent.empty())
        {
            if (!WriteFile(queueDir / "STNG_settings.txt", stngContent, writeError))
            {
                outError = "Failed to write STNG file: " + writeError;
                return false;
            }
        }
        if (!taskContent.empty())
        {
            if (!WriteFile(queueDir / "TASK_instructions.txt", taskContent, writeError))
            {
                outError = "Failed to write TASK file: " + writeError;
                return false;
            }
        }
        if (!cntxContent.empty())
        {
            if (!WriteFile(queueDir / "CNTX_context.txt", cntxContent, writeError))
            {
                outError = "Failed to write CNTX file: " + writeError;
                return false;
            }
        }
        std::string const probFilename = "prob.txt";
        if (!WriteFile(queueDir / probFilename, probContent, writeError))
        {
            outError = "Failed to write PROB file: " + writeError;
            return false;
        }

        // Build the envelope.  Concatenate STNG + TASK + CNTX + PROB into a single user message.
        AiInvocation envelope;
        envelope.m_InterfaceName = ResolveJcwfInterfaceName();
        envelope.m_QueueFolder = queueDir;
        envelope.m_ProbName = probFilename;
        envelope.m_Timeout = std::chrono::milliseconds(AI_CALL_TIMEOUT_MS);
        if (!outputSchemaJson.empty())
        {
            envelope.m_OutputSchemaJson = outputSchemaJson;
        }

        std::string combined;
        auto const appendSection = [&combined](std::string const& section)
        {
            if (section.empty()) return;
            if (!combined.empty()) combined += "\n";
            combined += section;
        };
        appendSection(stngContent);
        appendSection(taskContent);
        appendSection(cntxContent);
        appendSection(probContent);

        Message userMessage;
        userMessage.m_Role = MessageRole::User;
        userMessage.m_Content = std::move(combined);
        envelope.m_Messages.push_back(std::move(userMessage));

        // Blocking wait via std::promise — this runs on a background thread, and Submit's
        // callback fires on the dispatcher's I/O thread (LiveTransport curl path or
        // MockTransport fixture-replay path; both deliver via Pump()).
        //
        // Two safety properties on the callback path:
        //
        //   (1) Both `promise` and `fulfilled` are captured by value as
        //       shared_ptr<...>, so the callback owns its own references to
        //       both shared states.  If `RunSingleAiCall` returns on the
        //       timeout branch below before the network reply arrives, the
        //       caller's `future` is destroyed during stack unwind — but
        //       the promise's shared state stays alive (held by the callback's
        //       shared_ptr), so a LATE `set_value` is well-defined per the
        //       C++ standard: it stores the value into the shared state with
        //       no waiters, which is harmless.
        //
        //   (2) The atomic-CAS guard on `fulfilled` ensures that even if
        //       `AiRequestPool::Submit` were to invoke the callback more than
        //       once (today's contract is single-callback, but defense-in-
        //       depth against a future cancel-after-success race in the
        //       dispatcher), only the first `set_value` lands.  Without this
        //       guard a second `set_value` would throw
        //       `future_error(promise_already_satisfied)`, and an uncaught
        //       exception inside the curl thread's lambda would terminate.
        auto promise = std::make_shared<std::promise<AiReply>>();
        auto fulfilled = std::make_shared<std::atomic<bool>>(false);
        std::future<AiReply> future = promise->get_future();

        LOG_APP_INFO("[AiJcwfService] AI call dispatched: subfolder='{}' probFile='{}' interface='{}'", subfolderName,
                     (queueDir / probFilename).string(),
                     envelope.m_InterfaceName.empty() ? "<default>" : envelope.m_InterfaceName);

        bool const submitted = requestPool->Submit(envelope,
            [promise, fulfilled](AiReply const& reply) mutable
            {
                bool expected = false;
                if (!fulfilled->compare_exchange_strong(expected, true))
                {
                    return; // already fulfilled — second callback is a no-op
                }
                try
                {
                    promise->set_value(reply);
                }
                catch (std::future_error const&)
                {
                    // Spec-wise set_value should not throw here (CAS above
                    // guards against the only realistic throw class,
                    // promise_already_satisfied; the future being destroyed
                    // does NOT invalidate the shared state).  Catch as
                    // belt-and-suspenders so any future toolchain quirk
                    // doesn't take down the curl thread.
                }
            });

        if (!submitted)
        {
            outError = "AiRequestPool::Submit rejected envelope (no interface, no API key, or empty body)";
            return false;
        }

        std::future_status const status = future.wait_for(std::chrono::milliseconds(AI_CALL_TIMEOUT_MS));
        if (status != std::future_status::ready)
        {
            outError = "AI call timed out after " + std::to_string(AI_CALL_TIMEOUT_MS) + "ms";
            return false;
        }

        AiReply const reply = future.get();

        if (reply.m_Kind == AiReply::Kind::Error)
        {
            outError = reply.m_Error.m_Message.empty() ? "AI call failed" : reply.m_Error.m_Message;
            return false;
        }

        outResponseText = (reply.m_Kind == AiReply::Kind::Structured) ? reply.m_StructuredJson : reply.m_Text;

        if (outResponseText.empty())
        {
            outError = "AI returned empty response";
            return false;
        }

        LOG_APP_INFO("[AiJcwfService] AI call completed: subfolder='{}' responseLen={}", subfolderName,
                     outResponseText.size());

        return true;
    }

    // ----------------------------------------------------------------
    // Explain: JCWF → natural language
    // ----------------------------------------------------------------

    void AiJcwfService::ExplainAsync(std::string const& jcwfJsonText)
    {
        JoinFinishedThreads();

        std::string jcwfCopy = jcwfJsonText;

        std::lock_guard<std::mutex> lock(m_ThreadsMutex);
        m_BackgroundThreads.emplace_back(
            [this, jcwfText = std::move(jcwfCopy)]()
            {
                std::string const workflowId = WF_ID_EXPLAIN;
                std::string const runId = GenerateRunId(workflowId);
                int const seq = m_NextRequestSeq.fetch_add(1);
                std::string const seqStr = std::to_string(seq);

                LOG_APP_INFO("[workflow] run '{}' started (workflow '{}')", runId, workflowId);

                // ----------------------------------------------------------
                // Stage 1: High-level explanation
                // ----------------------------------------------------------
                Broadcast(R"({"type":"ai-explain-progress","message":"Generating explanation..."})");

                std::string const stng1 = "Be succinct. No embellishments. No preamble. No closing remarks. "
                                          "Output ONLY the structured explanation — nothing else.";

                std::string const task1 =
                    "Produce a brief, structured explanation of this JCWF workflow.\n"
                    "Use these sections ONLY:\n"
                    "1) Overview (2-3 sentences max)\n"
                    "2) Tasks — for each task you MUST state: id, type, working_directory, "
                    "queue_binding content (exact STNG/TASK/CNTX/PROB text), file_inputs, "
                    "materialize mappings, expose_error_signal value, depends_on list.\n"
                    "3) Dependencies and controlflow — MUST list every controlflow edge "
                    "(from, to, kind, from_port, to_port). MUST list every depends_on.\n"
                    "4) Error handling — MUST state which tasks expose error signals and how branches route.\n"
                    "Rules:\n"
                    "- MUST reproduce every queue_binding file content verbatim — do NOT truncate.\n"
                    "- MUST reproduce every file_inputs path verbatim.\n"
                    "- MUST reproduce every materialize mapping verbatim.\n"
                    "- MUST note shared working directories.\n"
                    "- Use SHORT sentences. No filler. No commentary. No examples.";

                std::string const cntx1 = "--- JCWF Workflow JSON ---\n" + jcwfText;
                std::string const prob1 = "Explain this JCWF workflow. Be brief.";

                LOG_APP_INFO("[workflow] task 'explain_stage1' executing in run '{}' (workflow '{}')", runId, workflowId);

                std::string stage1Response;
                std::string stage1Error;
                bool const stage1Ok = RunSingleAiCall("explain_" + seqStr + "_stage1", stng1, task1, cntx1, prob1,
                                                      stage1Response, stage1Error);

                if (!stage1Ok)
                {
                    LOG_APP_ERROR("[workflow] task 'explain_stage1' failed in run '{}': {}", runId, stage1Error);
                    Broadcast(R"({"type":"ai-explain-result","ok":false,"error":")" + JsonHelper::EscapeJsonString(stage1Error) + R"("})");
                    LOG_APP_ERROR("[workflow] run '{}' failed (workflow '{}')", runId, workflowId);
                    return;
                }
                LOG_APP_INFO("[workflow] task 'explain_stage1' completed in run '{}' (workflow '{}')", runId, workflowId);

                if (m_ShuttingDown.load())
                {
                    Broadcast(R"({"type":"ai-explain-result","ok":false,"error":"Service is shutting down"})");
                    return;
                }

                // ----------------------------------------------------------
                // Stage 2: Review / refine / enrich with spec awareness
                // ----------------------------------------------------------
                Broadcast(R"({"type":"ai-explain-progress","message":"Reviewing and enriching explanation..."})");

                std::string const generationGuide = LoadGenerationGuide();

                std::string const stng2 = "Be succinct. No embellishments. No preamble. No closing remarks. "
                                          "Output ONLY the corrected explanation — nothing else.";

                std::string const task2 =
                    "Review the explanation against the JCWF JSON and the JCWF specification.\n"
                    "Rules:\n"
                    "- MUST fix any inaccuracy.\n"
                    "- MUST verify every queue_binding content string matches the JSON verbatim.\n"
                    "- MUST verify every file_inputs path matches the JSON verbatim.\n"
                    "- MUST verify every materialize mapping matches the JSON verbatim.\n"
                    "- MUST verify every controlflow edge (from, to, kind, ports) matches the JSON.\n"
                    "- MUST verify expose_error_signal values match the JSON.\n"
                    "- MUST verify shared vs unique working directories.\n"
                    "- MUST verify depends_on lists match the JSON.\n"
                    "- MUST keep same structure: Overview, Tasks, Dependencies, Error handling.\n"
                    "- MUST keep output brief — short sentences, no filler, no commentary.\n"
                    "- The result MUST be precise enough to recreate the JCWF from the explanation alone.";

                std::string const cntx2 = "--- JCWF Workflow JSON ---\n" + jcwfText +
                                          "\n\n--- JCWF Specification (condensed) ---\n" + generationGuide +
                                          "\n\n--- Explanation to review and enrich ---\n" + stage1Response;

                std::string const prob2 = "Review and correct this explanation. Keep it brief.";

                LOG_APP_INFO("[workflow] task 'explain_stage2' executing in run '{}' (workflow '{}')", runId, workflowId);

                std::string stage2Response;
                std::string stage2Error;
                bool const stage2Ok = RunSingleAiCall("explain_" + seqStr + "_stage2", stng2, task2, cntx2, prob2,
                                                      stage2Response, stage2Error);

                if (stage2Ok)
                {
                    LOG_APP_INFO("[workflow] task 'explain_stage2' completed in run '{}' (workflow '{}')", runId,
                                 workflowId);
                    Broadcast(R"({"type":"ai-explain-result","ok":true,"summary":")" + JsonHelper::EscapeJsonString(stage2Response) + R"("})");
                    LOG_APP_INFO("[workflow] run '{}' completed (workflow '{}')", runId, workflowId);
                }
                else
                {
                    // Stage 2 failed — fall back to Stage 1 result (still useful).
                    LOG_APP_WARN("[workflow] task 'explain_stage2' failed in run '{}': {} — returning stage 1 result", runId,
                                 stage2Error);
                    Broadcast(R"({"type":"ai-explain-result","ok":true,"summary":")" + JsonHelper::EscapeJsonString(stage1Response) + R"("})");
                    LOG_APP_INFO("[workflow] run '{}' completed with stage 1 fallback (workflow '{}')", runId, workflowId);
                }
            });
    }

    // ----------------------------------------------------------------
    // Generate: natural language → JCWF
    // ----------------------------------------------------------------

    void AiJcwfService::GenerateAsync(std::string const& prompt, std::string const& currentJcwfJson)
    {
        JoinFinishedThreads();

        std::string promptCopy = prompt;
        std::string currentJcwfCopy = currentJcwfJson;

        std::lock_guard<std::mutex> lock(m_ThreadsMutex);
        m_BackgroundThreads.emplace_back(
            [this, userPrompt = std::move(promptCopy), currentJcwf = std::move(currentJcwfCopy)]()
            {
                std::string const workflowId = WF_ID_GENERATE;
                std::string const runId = GenerateRunId(workflowId);
                int const seq = m_NextRequestSeq.fetch_add(1);
                std::string const seqStr = std::to_string(seq);
                int totalStages = 5;

                LOG_APP_INFO("[workflow] run '{}' started (workflow '{}')", runId, workflowId);

                auto broadcastProgress = [&](int stage, std::string const& message)
                {
                    std::ostringstream ss;
                    ss << R"({"type":"ai-generate-progress","stage":)" << stage << R"(,"totalStages":)" << totalStages
                       << R"(,"message":")" << JsonHelper::EscapeJsonString(message) << R"("})";
                    Broadcast(ss.str());
                };

                std::vector<GeneratedScript> generatedScripts;

                auto broadcastResult = [&](bool ok, std::string const& jcwfOrError, int retries)
                {
                    if (ok)
                    {
                        // jcwfOrError is raw JSON — embed directly (not string-escaped).
                        std::ostringstream ss;
                        ss << R"({"type":"ai-generate-result","ok":true,"jcwf":)" << jcwfOrError << R"(,"retries":)"
                           << retries;

                        if (!generatedScripts.empty())
                        {
                            ss << R"(,"scripts":[)";
                            for (size_t i = 0; i < generatedScripts.size(); ++i)
                            {
                                if (i > 0)
                                    ss << ",";
                                ss << R"({"path":")" << JsonHelper::EscapeJsonString(generatedScripts[i].path) << R"(","content":")"
                                   << JsonHelper::EscapeJsonString(generatedScripts[i].content) << R"(","executable":)"
                                   << (generatedScripts[i].executable ? "true" : "false") << "}";
                            }
                            ss << "]";
                        }

                        ss << "}";
                        Broadcast(ss.str());
                        LOG_APP_INFO("[workflow] run '{}' completed (workflow '{}')", runId, workflowId);
                    }
                    else
                    {
                        Broadcast(R"({"type":"ai-generate-result","ok":false,"error":")" + JsonHelper::EscapeJsonString(jcwfOrError) +
                                  R"("})");
                        LOG_APP_ERROR("[workflow] run '{}' failed (workflow '{}')", runId, workflowId);
                    }
                };

                if (m_ShuttingDown.load())
                {
                    broadcastResult(false, "Service is shutting down", 0);
                    return;
                }

                std::string const generationGuide = LoadGenerationGuide();

                // ----------------------------------------------------------
                // Stage 1: Decompose the prompt
                // ----------------------------------------------------------
                broadcastProgress(1, "Analyzing prompt...");
                LOG_APP_INFO("[workflow] task 'decompose' executing in run '{}' (workflow '{}')", runId, workflowId);

                std::string const decomposeStng = "Be succinct. No embellishments. No preamble. No closing remarks. "
                                                  "Output ONLY the structured task breakdown — nothing else.";

                std::string decomposeTask =
                    "Produce a structured task breakdown from the user's request.\n"
                    "Host OS: " +
                    GetHostOsDescription() +
                    "\n"
                    "For each task you MUST specify:\n"
                    "- task_id (short slug)\n"
                    "- type (shell | ai_call | python | internal)\n"
                    "- label\n"
                    "- working_directory (ai_call: '../queue/<wfId>/<NN>_<taskId>', shell: '<wfId>/<NN>_<taskId>')\n"
                    "- depends_on list\n"
                    "- expose_error_signal (true/false)\n"
                    "- For ai_call: exact STNG, TASK, CNTX, PROB file content. "
                    "Use cntx_files string paths (not inline objects) to feed upstream outputs to the AI.\n"
                    "- For shell: command (MUST start with 'scripts/'), args, file_inputs paths, materialize mappings\n"
                    "- For python: module (MUST start with 'scripts.'), function name, file_inputs, file_outputs. "
                    "The runtime calls function(**kwargs, context=dict) — NOT via CLI.\n"
                    "- If error handling needed: which branch node, which controlflow edges (from, to, kind, ports)\n"
                    "Rules:\n"
                    "- Every ai_call STNG content MUST include 'No markdown fences, no explanations.' "
                    "because AI output is consumed directly by compilers/tools, not humans.\n"
                    "- Branch nodes MUST appear ONLY in control_nodes, NOT in tasks.\n"
                    "- Every controlflow edge MUST specify from, to, kind, from_port, to_port.\n"
                    "- Use MUST and SHALL for hard constraints. Leave no ambiguity.\n";

                if (!currentJcwf.empty())
                {
                    decomposeTask += "\nThe user wants to MODIFY an existing workflow. Here is the current JCWF:\n"
                                     "--- Current JCWF ---\n" +
                                     currentJcwf + "\n--- End Current JCWF ---\n";
                }

                std::string scriptRegistryTable;
                if (JarvisAgent* app = App::g_App.load(std::memory_order_acquire); app && app->GetScriptRegistry())
                {
                    scriptRegistryTable = app->GetScriptRegistry()->SerializeMarkdownTable();
                }

                std::string workflowFileListing;
                if (JarvisAgent* app = App::g_App.load(std::memory_order_acquire); app && app->GetWorkflowFileIndex())
                {
                    // Re-scan so the listing is fresh
                    app->GetWorkflowFileIndex()->ScanDirectory(app->GetWorkflowFileIndex()->GetRootDirectory());
                    workflowFileListing = app->GetWorkflowFileIndex()->SerializeMarkdownListing();
                }

                std::string decomposeCntx = "--- JCWF Generation Guide (condensed spec) ---\n" + generationGuide;
                if (!scriptRegistryTable.empty())
                {
                    decomposeCntx += "\n\n--- Script Registry ---\n" + scriptRegistryTable;
                }
                if (!workflowFileListing.empty())
                {
                    decomposeCntx += "\n\n--- Workflow File Inventory (paths relative to workflows/) ---\n"
                                     "These files already exist on disk. Use them in file_inputs when appropriate.\n" +
                                     workflowFileListing;
                }
                std::string const decomposeProb = "User request: " + userPrompt;

                std::string decomposition;
                std::string decomposeError;
                if (!RunSingleAiCall("gen_" + seqStr + "_decompose", decomposeStng, decomposeTask, decomposeCntx,
                                     decomposeProb, decomposition, decomposeError))
                {
                    LOG_APP_ERROR("[workflow] task 'decompose' failed in run '{}': {}", runId, decomposeError);
                    broadcastResult(false, "Decomposition failed: " + decomposeError, 0);
                    return;
                }
                LOG_APP_INFO("[workflow] task 'decompose' completed in run '{}' (workflow '{}')", runId, workflowId);

                if (m_ShuttingDown.load())
                {
                    broadcastResult(false, "Service is shutting down", 0);
                    return;
                }

                // ----------------------------------------------------------
                // Stage 2: Generate JCWF JSON
                // ----------------------------------------------------------
                broadcastProgress(2, "Generating JCWF...");
                LOG_APP_INFO("[workflow] task 'generate' executing in run '{}' (workflow '{}')", runId, workflowId);

                // Shared MUST rules for all generate calls (single-call and batched).
                std::string const generateMustRules =
                    "Host OS: " + GetHostOsDescription() +
                    "\n"
                    "MUST rules:\n"
                    "- Every task 'id' field MUST match its key in the 'tasks' map.\n"
                    "- ai_call working_directory MUST be '../queue/<workflowId>/<NN>_<taskId>'.\n"
                    "- shell working_directory MUST be '<workflowId>/<NN>_<taskId>'.\n"
                    "- shell command MUST start with 'scripts/'.\n"
                    "- python params.module MUST start with 'scripts.' (e.g. 'scripts.parseLog').\n"
                    "- python params.function MUST name the actual callable in the script.\n"
                    "- ai_call cntx_files: use string paths to upstream outputs, NOT inline objects with placeholders.\n"
                    "- ai_call cntx_files crossing from queue to workflows: use '../../../workflows/<pythonWorkDir>/<file>' "
                    "(3 levels up from queue/X/Y to root, then into workflows/). NEVER use only '../../'.\n"
                    "- file_inputs values are bare filenames relative to working_directory (e.g. 'input.log'). "
                    "NEVER prefix with the working_directory path — that doubles the path at runtime.\n"
                    "- Prefer a SINGLE combined JSON output file over splitting into many files.\n"
                    "- version MUST be '1.1' if using control_nodes or controlflow.\n"
                    "- depends_on MUST form a DAG (no cycles).\n"
                    "- Branch nodes MUST appear ONLY in control_nodes, NEVER in tasks.\n"
                    "- expose_error_signal + controlflow edges for error branches.\n"
                    "- Every controlflow edge MUST have from, to, kind, from_port, to_port.\n"
                    "- Port names: dep-source, error-signal, cf-in-normal, cf-in-error, "
                    "cf-out-normal, cf-out-error, dep-target.\n"
                    "- Every ai_call stng_files content MUST include 'No markdown fences, no explanations.' "
                    "because AI output is consumed directly by compilers/tools.\n"
                    "- ai_call tasks MUST NOT declare 'file_outputs'. Instead, declare an 'outputs' slot: "
                    "\"outputs\": { \"<slotName>\": { \"type\": \"string\" } }. "
                    "The slot auto-maps to the task's <stem>.output.txt artifact, and downstream "
                    "tasks reference it as {{taskId.output_file}} or {{taskId.<slotName>}}.\n";

                std::string const generateStng = "Output ONLY valid JSON. No markdown fences. No explanations. No comments. "
                                                 "The output MUST parse as a complete JCWF file.";

                // Build shared context used by both single-call and batched paths.
                std::string generateCntxBase =
                    "--- Task Breakdown ---\n" + decomposition + "\n\n--- JCWF Generation Guide ---\n" + generationGuide;
                if (!scriptRegistryTable.empty())
                {
                    generateCntxBase += "\n\n--- Script Registry ---\n" + scriptRegistryTable;
                }
                if (!workflowFileListing.empty())
                {
                    generateCntxBase += "\n\n--- Workflow File Inventory (paths relative to workflows/) ---\n"
                                        "These files already exist on disk. Use them in file_inputs when appropriate.\n" +
                                        workflowFileListing;
                }

                // Decide: single-call vs fan-out based on task count.
                // Read batch size from config (fallback to DEFAULT_BATCH_SIZE).
                size_t configBatchSize = DEFAULT_BATCH_SIZE;
                if (auto const& cfg = Core::g_Core->GetConfig(); cfg.m_JcwfBatchSize > 0)
                {
                    configBatchSize = cfg.m_JcwfBatchSize;
                }

                std::vector<std::string> taskIds = ExtractTaskIdsFromDecomposition(decomposition);
                bool const useFanOut = !taskIds.empty() && taskIds.size() > configBatchSize && currentJcwf.empty();

                std::string generatedJcwf;

                if (!useFanOut)
                {
                    // ---- Single-call path (original behavior) ----
                    std::string const generateTask = "Generate a complete JCWF JSON file from the task breakdown below.\n" +
                                                     generateMustRules + "Output ONLY the JSON. Nothing else.";

                    std::string generateCntx = generateCntxBase;
                    if (!currentJcwf.empty())
                    {
                        generateCntx += "\n\n--- Current JCWF (modify this) ---\n" + currentJcwf;
                    }

                    std::string const generateProb = "Generate the JCWF JSON.";

                    std::string generateError;
                    if (!RunSingleAiCall("gen_" + seqStr + "_generate", generateStng, generateTask, generateCntx,
                                         generateProb, generatedJcwf, generateError, kJcwfSchemaJson))
                    {
                        LOG_APP_ERROR("[workflow] task 'generate' failed in run '{}': {}", runId, generateError);
                        broadcastResult(false, "Generation failed: " + generateError, 0);
                        return;
                    }
                    LOG_APP_INFO("[workflow] task 'generate' completed in run '{}' (workflow '{}')", runId, workflowId);
                }
                else
                {
                    // ---- Fan-out path: partition tasks into batches ----
                    size_t const batchSize = configBatchSize;
                    std::vector<std::vector<std::string>> batches;
                    for (size_t i = 0; i < taskIds.size(); i += batchSize)
                    {
                        size_t end = std::min(i + batchSize, taskIds.size());
                        batches.emplace_back(taskIds.begin() + i, taskIds.begin() + end);
                    }

                    LOG_APP_INFO("[AiJcwfService] Fan-out: {} tasks in {} batches (batchSize={})", taskIds.size(),
                                 batches.size(), batchSize);
                    totalStages += static_cast<int>(batches.size()) - 1; // extra sub-stages

                    std::vector<std::string> fragments;
                    fragments.reserve(batches.size());

                    for (size_t batchIdx = 0; batchIdx < batches.size(); ++batchIdx)
                    {
                        if (m_ShuttingDown.load())
                        {
                            broadcastResult(false, "Service is shutting down", 0);
                            return;
                        }

                        // Build comma-separated task ID list for this batch
                        std::string batchList;
                        for (size_t j = 0; j < batches[batchIdx].size(); ++j)
                        {
                            if (j > 0)
                            {
                                batchList += ", ";
                            }
                            batchList += batches[batchIdx][j];
                        }

                        broadcastProgress(2, "Generating JCWF (batch " + std::to_string(batchIdx + 1) + "/" +
                                                 std::to_string(batches.size()) + ")...");

                        std::string batchTask;
                        std::string batchStng = generateStng;

                        if (batchIdx == 0)
                        {
                            // First batch: complete JCWF with workflow-level fields + first batch of tasks.
                            batchTask = "Generate a complete JCWF JSON file from the task breakdown below.\n"
                                        "Include ALL workflow-level fields (id, label, doc, version, triggers, defaults, "
                                        "base_directory, control_nodes, controlflow).\n"
                                        "Generate ONLY these tasks in the \"tasks\" map: " +
                                        batchList +
                                        "\n"
                                        "The remaining tasks will be generated separately and merged in later.\n"
                                        "Do NOT generate task entries for tasks not in the list above.\n" +
                                        generateMustRules + "Output ONLY the JSON. Nothing else.";
                        }
                        else
                        {
                            // Subsequent batches: tasks-only fragment.
                            batchTask = "Generate ONLY the task entries for these tasks: " + batchList +
                                        "\n"
                                        "Output format: { \"tasks\": { \"taskId\": {...}, ... } }\n"
                                        "Do NOT include workflow-level fields (id, label, version, triggers, etc.) — "
                                        "only the \"tasks\" map with the listed tasks.\n" +
                                        generateMustRules + "Output ONLY the JSON. Nothing else.";

                            batchStng = "Output ONLY valid JSON. No markdown fences. No explanations. No comments. "
                                        "The output MUST be a JSON object with a single \"tasks\" key.";
                        }

                        std::string batchFragment;
                        std::string batchError;
                        if (!RunSingleAiCall("gen_" + seqStr + "_generate_batch_" + std::to_string(batchIdx), batchStng,
                                             batchTask, generateCntxBase, "Generate the JCWF JSON.", batchFragment,
                                             batchError))
                        {
                            LOG_APP_WARN("[workflow] batch {} generate failed in run '{}': {}", batchIdx, runId, batchError);
                            broadcastResult(false,
                                            "Generation failed (batch " + std::to_string(batchIdx) + "): " + batchError, 0);
                            return;
                        }

                        fragments.push_back(StripMarkdownFences(batchFragment));
                        LOG_APP_INFO("[AiJcwfService] Batch {}/{} completed (tasks: {})", batchIdx + 1, batches.size(),
                                     batchList);
                    }

                    // Merge all fragments into a single JCWF
                    generatedJcwf = MergeJcwfFragments(fragments);
                    LOG_APP_INFO("[workflow] task 'generate' completed in run '{}' ({} batches merged)", runId,
                                 fragments.size());
                }

                // Strip markdown fences if the AI wrapped the output.
                generatedJcwf = StripMarkdownFences(generatedJcwf);

                // ----------------------------------------------------------
                // Early validate+fix (fan-out only): catch path issues before script generation
                // ----------------------------------------------------------
                if (useFanOut)
                {
                    broadcastProgress(2, "Validating merged JCWF...");
                    LOG_APP_INFO("[workflow] early validation in run '{}' (pre-script, fan-out)", runId);

                    std::string earlyValidationSummary;
                    std::vector<WorkflowValidationIssue> earlyIssues;
                    // Pass nullptr for scriptRegistry and pendingScripts — skip script checks.
                    ValidateJcwf(generatedJcwf, earlyValidationSummary, nullptr, nullptr, &earlyIssues);

                    if (!earlyValidationSummary.empty())
                    {
                        LOG_APP_WARN("[workflow] early validation issues in run '{}':\n{}", runId, earlyValidationSummary);

                        // Use targeted fix for task-specific issues
                        bool const earlyAllTaskSpecific =
                            !earlyIssues.empty() && std::all_of(earlyIssues.begin(), earlyIssues.end(),
                                                                [](auto const& i) { return !i.m_TaskId.empty(); });

                        if (earlyAllTaskSpecific)
                        {
                            std::string const earlyFixStng =
                                "You are a JCWF code fixer. Output ONLY valid JSON — no markdown fences, "
                                "no explanations. Fix all validation errors AND warnings while preserving "
                                "the workflow's intended behavior.";

                            std::set<std::string> earlyAffectedIds;
                            for (auto const& issue : earlyIssues)
                            {
                                earlyAffectedIds.insert(issue.m_TaskId);
                            }

                            std::string earlyAffectedBlocks;
                            for (auto const& tid : earlyAffectedIds)
                            {
                                std::string block = ExtractTaskBlock(generatedJcwf, tid);
                                if (!block.empty())
                                {
                                    earlyAffectedBlocks += "\n--- Task: " + tid + " ---\n" + block + "\n";
                                }
                            }

                            std::string const earlyFixTask =
                                "The workflow has validation issues in specific tasks. Fix ONLY the listed "
                                "tasks.\n"
                                "Output format: { \"tasks\": { \"taskId\": {...}, ... } }\n"
                                "Include ONLY the fixed tasks in the output — no workflow-level fields.\n\n"
                                "Validation issues:\n" +
                                earlyValidationSummary;

                            std::string const earlyFixCntx =
                                "--- Task Breakdown (all tasks, for reference) ---\n" + decomposition +
                                "\n\n--- Affected Task Blocks (current, to be fixed) ---\n" + earlyAffectedBlocks +
                                "\n\n--- JCWF Generation Guide ---\n" + generationGuide;

                            std::string const earlyFixProb = "Fix the listed tasks.";

                            LOG_APP_INFO("[AiJcwfService] Early fix: {} affected task(s)", earlyAffectedIds.size());
                            broadcastProgress(2, "Fixing merged JCWF...");

                            std::string earlyFixedJcwf;
                            std::string earlyFixError;
                            if (RunSingleAiCall("gen_" + seqStr + "_early_fix", earlyFixStng, earlyFixTask, earlyFixCntx,
                                                earlyFixProb, earlyFixedJcwf, earlyFixError))
                            {
                                generatedJcwf = PatchTasksIntoJcwf(generatedJcwf, StripMarkdownFences(earlyFixedJcwf));
                                LOG_APP_INFO("[workflow] early fix completed in run '{}'", runId);
                            }
                            else
                            {
                                LOG_APP_WARN("[workflow] early fix failed in run '{}': {}", runId, earlyFixError);
                            }
                        }
                    }
                    else
                    {
                        LOG_APP_INFO("[workflow] early validation passed — no issues in run '{}'", runId);
                    }
                }

                // ----------------------------------------------------------
                // Stage 3: Generate companion scripts
                // ----------------------------------------------------------
                if (m_ShuttingDown.load())
                {
                    broadcastResult(false, "Service is shutting down", 0);
                    return;
                }

                {
                    broadcastProgress(3, "Checking for new scripts...");
                    LOG_APP_INFO("[workflow] task 'generate_scripts' executing in run '{}' (workflow '{}')", runId,
                                 workflowId);

                    std::vector<std::string> scriptPaths = ExtractScriptPaths(generatedJcwf);
                    auto const scriptFunctionMap = ExtractScriptFunctionMap(generatedJcwf);
                    auto const shellIoMap = ExtractShellTaskIoMap(generatedJcwf);

                    // Filter to only scripts that don't exist on disk
                    std::vector<std::string> newScripts;
                    for (auto const& sp : scriptPaths)
                    {
                        if (!fs::exists(sp))
                        {
                            newScripts.push_back(sp);
                        }
                    }

                    if (!newScripts.empty())
                    {
                        LOG_APP_INFO("[workflow] generating {} new script(s) in run '{}'", newScripts.size(), runId);

                        for (size_t i = 0; i < newScripts.size(); ++i)
                        {
                            if (m_ShuttingDown.load())
                            {
                                broadcastResult(false, "Service is shutting down", 0);
                                return;
                            }

                            std::string const& scriptPath = newScripts[i];
                            bool const isShell = scriptPath.ends_with(".sh");
                            bool const isPs1 = scriptPath.ends_with(".ps1");

                            broadcastProgress(3, "Generating " + scriptPath + " (" + std::to_string(i + 1) + "/" +
                                                     std::to_string(newScripts.size()) + ")...");

                            std::string const scriptStng =
                                "Output ONLY the raw script file content. No markdown fences. No explanations. "
                                "No introductory or closing commentary. The output must be a valid, runnable script.";

                            std::string scriptTask;
                            if (isShell)
                            {
                                // Build explicit positional-arg mapping from JCWF file_inputs/file_outputs
                                std::string argMapping;
                                auto ioIt = shellIoMap.find(scriptPath);
                                if (ioIt != shellIoMap.end())
                                {
                                    int argNum = 1;
                                    for (auto const& fi : ioIt->second.fileInputs)
                                    {
                                        argMapping += "  $" + std::to_string(argNum) + " = file_inputs (\"" + fi + "\")\n";
                                        argNum++;
                                    }
                                    for (auto const& fo : ioIt->second.fileOutputs)
                                    {
                                        argMapping += "  $" + std::to_string(argNum) + " = file_outputs (\"" + fo + "\")\n";
                                        argNum++;
                                    }
                                }

                                scriptTask = "Generate a bash script for '" + scriptPath +
                                             "'.\n"
                                             "Host OS: " +
                                             GetHostOsDescription() +
                                             "\n"
                                             "Rules:\n"
                                             "- First line MUST be: #!/usr/bin/env bash\n"
                                             "- Second line MUST be: # @jarvis-script\n"
                                             "- Include metadata with COLON format: # @short: ..., # @params: ..., "
                                             "# @description: ..., # @outputs: ... (if any).\n"
                                             "- After the metadata header: set -euo pipefail\n"
                                             "- CRITICAL: The executor passes file_inputs as the first positional "
                                             "args, then file_outputs. Your script receives:\n" +
                                             argMapping +
                                             "  The script MUST use $1, $2, etc. to access these files. "
                                             "NEVER hardcode file paths. NEVER use literal paths from the JCWF. "
                                             "Always assign positional args to named variables at the top "
                                             "(e.g. infile=\"$1\"; outfile=\"$2\").\n"
                                             "- POSIX portability: when using awk, use ONLY POSIX-compatible syntax. "
                                             "Do NOT use gawk extensions: no multidimensional arrays (arr[k1][k2]), "
                                             "no 3-argument match() (match(s,r,arr)), no asort()/asorti(), "
                                             "no nextfile, no PROCINFO, no @include, no gensub(). "
                                             "Use SUBSEP-based keys: arr[k1,k2] with split(key, parts, SUBSEP). "
                                             "For sorting, pipe to external 'sort' command instead of asort() or "
                                             "PROCINFO[\"sorted_in\"]. Use gsub() instead of gensub(). "
                                             "If GNU awk is truly required, call 'gawk' explicitly.\n"
                                             "- Output ONLY the script. Nothing else.";
                            }
                            else if (isPs1)
                            {
                                // Build explicit positional-arg mapping from JCWF file_inputs/file_outputs
                                std::string argMapping;
                                auto ioIt = shellIoMap.find(scriptPath);
                                int argNum = 1;
                                if (ioIt != shellIoMap.end())
                                {
                                    for (auto const& fi : ioIt->second.fileInputs)
                                    {
                                        argMapping +=
                                            "  $Arg" + std::to_string(argNum) + " = file_inputs (\"" + fi + "\")\n";
                                        argNum++;
                                    }
                                    for (auto const& fo : ioIt->second.fileOutputs)
                                    {
                                        argMapping +=
                                            "  $Arg" + std::to_string(argNum) + " = file_outputs (\"" + fo + "\")\n";
                                        argNum++;
                                    }
                                }

                                scriptTask = "Generate a PowerShell script for '" + scriptPath +
                                             "'.\n"
                                             "Host OS: " +
                                             GetHostOsDescription() +
                                             "\n"
                                             "Rules:\n"
                                             "- First line MUST be: # @jarvis-script\n"
                                             "- No shebang line (PowerShell does not use one).\n"
                                             "- Include metadata comments: # @short: ..., # @params: ..., "
                                             "# @description: ..., # @outputs: ... (if any).\n"
                                             "- After metadata, declare a param() block with named parameters:\n"
                                             "    param([string]$Arg1, [string]$Arg2, ...)\n"
                                             "- After param(): Set-StrictMode -Version Latest\n"
                                             "- After Set-StrictMode: $ErrorActionPreference = 'Stop'\n"
                                             "- CRITICAL: The executor passes file_inputs as the first positional "
                                             "args, then file_outputs. Your script receives:\n" +
                                             argMapping +
                                             "  Use $Arg1, $Arg2, etc. (matching your param() declaration). "
                                             "NEVER hardcode file paths. NEVER use literal paths from the JCWF.\n"
                                             "- Use PowerShell cmdlets: Write-Output, Copy-Item, Remove-Item, "
                                             "Join-Path, Get-Content, Set-Content, etc.\n"
                                             "- Use & operator to call external executables: & g++ $Source -o $Output\n"
                                             "- No POSIX awk/sed/grep. No bash syntax.\n"
                                             "- Output ONLY the script. Nothing else.";
                            }
                            else
                            {
                                scriptTask = "Generate a Python script for '" + scriptPath +
                                             "'.\n"
                                             "Rules:\n"
                                             "- First line MUST be: #!/usr/bin/env python3\n"
                                             "- Second line MUST be: # @jarvis-script\n"
                                             "- Include metadata with COLON format: # @short: ..., # @description: ..., "
                                             "# @outputs: ... (if any).\n"
                                             "- The runtime calls the function programmatically: "
                                             "module.function(**kwargs, context=dict). Do NOT use sys.argv, argparse, "
                                             "or main().\n"
                                             "- The function name MUST match the 'function' field in the JCWF params.\n"
                                             "- Accept `context=None` and `**kwargs` as parameters.\n"
                                             "- Read file inputs via context['_file_input_0'], context['_file_input_1'], "
                                             "etc. (absolute resolved paths from file_inputs).\n"
                                             "- Get working directory via context['_task_working_directory'].\n"
                                             "- Write output files to the working directory using os.path.join().\n"
                                             "- Output ONLY the script. Nothing else.";
                            }

                            std::string const scriptCntx =
                                "--- JCWF Workflow ---\n" + generatedJcwf + "\n\n--- User Request ---\n" + userPrompt;

                            std::string const scriptProb = "Generate the script: " + scriptPath;

                            std::string scriptContent;
                            std::string scriptError;
                            if (!RunSingleAiCall("gen_" + seqStr + "_script_" + std::to_string(i), scriptStng, scriptTask,
                                                 scriptCntx, scriptProb, scriptContent, scriptError))
                            {
                                LOG_APP_WARN("[workflow] script generation failed for '{}' in run '{}': {}", scriptPath,
                                             runId, scriptError);
                                // Non-fatal — continue with remaining scripts
                                continue;
                            }

                            // Strip markdown fences if the AI wrapped the output
                            scriptContent = StripMarkdownFences(scriptContent);

                            // ---- Script validation + fix + review cycle ----

                            // Look up expected function name for this script
                            std::string expectedFunc;
                            {
                                auto funcIt = scriptFunctionMap.find(scriptPath);
                                if (funcIt != scriptFunctionMap.end())
                                {
                                    expectedFunc = funcIt->second;
                                }
                            }

                            // Layer 1: Structural validation
                            bool const isPythonScript = !isShell && !isPs1;
                            auto scriptVr = ValidateGeneratedScript(scriptContent, expectedFunc, isPythonScript, isPs1);

                            if (!scriptVr.passed)
                            {
                                LOG_APP_WARN("[workflow] script '{}' has {} structural issue(s) in "
                                             "run '{}'",
                                             scriptPath, scriptVr.issues.size(), runId);

                                std::string issueSummary;
                                for (auto const& issue : scriptVr.issues)
                                {
                                    issueSummary += "- " + issue + "\n";
                                }

                                // Layer 2: Fix AI call
                                broadcastProgress(3, "Fixing " + scriptPath + "...");

                                std::string const sFixStng = "You are a script fixer. Output ONLY the corrected script. "
                                                             "No markdown fences. No explanations. No commentary.";

                                std::string sFixTask = "The generated script has structural issues. Fix ALL of "
                                                       "them:\n\n" +
                                                       issueSummary + "\nRules:\n";
                                if (isShell)
                                {
                                    // Build arg mapping for the fix prompt too
                                    std::string fixArgMapping;
                                    auto fixIoIt = shellIoMap.find(scriptPath);
                                    if (fixIoIt != shellIoMap.end())
                                    {
                                        int fixArgNum = 1;
                                        for (auto const& fi : fixIoIt->second.fileInputs)
                                        {
                                            fixArgMapping +=
                                                "  $" + std::to_string(fixArgNum) + " = file_inputs (\"" + fi + "\")\n";
                                            fixArgNum++;
                                        }
                                        for (auto const& fo : fixIoIt->second.fileOutputs)
                                        {
                                            fixArgMapping +=
                                                "  $" + std::to_string(fixArgNum) + " = file_outputs (\"" + fo + "\")\n";
                                            fixArgNum++;
                                        }
                                    }

                                    sFixTask += "- First line: #!/usr/bin/env bash\n"
                                                "- Second line: # @jarvis-script\n"
                                                "- Include # @short: ... and # @description: ... metadata\n"
                                                "- After the metadata header: set -euo pipefail\n"
                                                "- The executor passes file_inputs then file_outputs as "
                                                "positional args:\n" +
                                                fixArgMapping +
                                                "  MUST use $1, $2 etc. NEVER hardcode file paths.\n"
                                                "- POSIX awk only: no arr[k1][k2], no 3-arg match(), "
                                                "no asort()/asorti(), no PROCINFO, no gensub(). "
                                                "Use SUBSEP keys, external sort, and gsub().\n"
                                                "- Output ONLY the fixed script.";
                                }
                                else if (isPs1)
                                {
                                    std::string fixArgMapping;
                                    auto fixIoIt = shellIoMap.find(scriptPath);
                                    if (fixIoIt != shellIoMap.end())
                                    {
                                        int fixArgNum = 1;
                                        for (auto const& fi : fixIoIt->second.fileInputs)
                                        {
                                            fixArgMapping +=
                                                "  $Arg" + std::to_string(fixArgNum) + " = file_inputs (\"" + fi + "\")\n";
                                            fixArgNum++;
                                        }
                                        for (auto const& fo : fixIoIt->second.fileOutputs)
                                        {
                                            fixArgMapping +=
                                                "  $Arg" + std::to_string(fixArgNum) +
                                                " = file_outputs (\"" + fo + "\")\n";
                                            fixArgNum++;
                                        }
                                    }

                                    sFixTask += "- First line: # @jarvis-script (no shebang)\n"
                                                "- Include # @short: ... and # @description: ... metadata\n"
                                                "- param([string]$Arg1, ...) block after metadata\n"
                                                "- Set-StrictMode -Version Latest and $ErrorActionPreference = 'Stop' "
                                                "after param()\n"
                                                "- The executor passes file_inputs then file_outputs as positional args:\n" +
                                                fixArgMapping +
                                                "  MUST use $Arg1, $Arg2 etc. NEVER hardcode file paths.\n"
                                                "- Use PowerShell cmdlets. No POSIX awk/sed/grep.\n"
                                                "- Output ONLY the fixed script.";
                                }
                                else
                                {
                                    sFixTask += "- First line: #!/usr/bin/env python3\n"
                                                "- Second line: # @jarvis-script\n"
                                                "- Include # @short: ... and # @description: ... metadata\n"
                                                "- Output ONLY the fixed script.";
                                    if (!expectedFunc.empty())
                                    {
                                        sFixTask += "\n- Function name MUST be: " + expectedFunc +
                                                    "\n- Function must accept (context=None, **kwargs)";
                                    }
                                }

                                std::string const sFixCntx = "--- Current Script ---\n" + scriptContent +
                                                             "\n\n--- JCWF Workflow ---\n" + generatedJcwf;
                                std::string const sFixProb = "Fix the script: " + scriptPath;

                                std::string fixedScript;
                                std::string fixScriptError;
                                if (RunSingleAiCall("gen_" + seqStr + "_script_" + std::to_string(i) + "_fix", sFixStng,
                                                    sFixTask, sFixCntx, sFixProb, fixedScript, fixScriptError))
                                {
                                    scriptContent = StripMarkdownFences(fixedScript);
                                    LOG_APP_INFO("[workflow] script '{}' fixed in run '{}'", scriptPath, runId);
                                }
                                else
                                {
                                    LOG_APP_WARN("[workflow] script fix failed for '{}': {}", scriptPath, fixScriptError);
                                }
                            }

                            // Layer 3: AI review for logical correctness (always)
                            {
                                broadcastProgress(3, "Reviewing " + scriptPath + "...");

                                std::string const sRevStng = "You are a code reviewer. Output ONLY the final script. "
                                                             "No markdown fences. No explanations. No commentary.";

                                std::string sRevTask = "Review this script for correctness and fix any issues.\n"
                                                       "Check for:\n";
                                if (isShell)
                                {
                                    sRevTask += "1. Correct shebang: #!/usr/bin/env bash\n"
                                                "2. set -euo pipefail present after metadata header\n"
                                                "3. Proper quoting of variables (\"$var\" not $var)\n"
                                                "4. Correct use of positional args ($1, $2, ...)\n"
                                                "5. Proper error handling (exit codes, error messages to stderr)\n"
                                                "6. No hardcoded absolute paths — use relative paths\n"
                                                "7. Output files written to the correct working directory\n";
                                }
                                else if (isPs1)
                                {
                                    sRevTask += "1. No shebang line (PowerShell does not use one)\n"
                                                "2. Set-StrictMode -Version Latest present after param() block\n"
                                                "3. $ErrorActionPreference = 'Stop' present\n"
                                                "4. Correct use of param() block for positional args\n"
                                                "5. No POSIX awk/sed/grep; use PowerShell cmdlets\n"
                                                "6. No hardcoded absolute paths\n"
                                                "7. Output files written to the correct working directory\n";
                                }
                                else
                                {
                                    sRevTask += "1. Type safety: no operations comparing incompatible types "
                                                "(e.g. datetime vs string, int vs None)\n"
                                                "2. Correct context usage: file inputs from "
                                                "context['_file_input_0'], working dir from "
                                                "context['_task_working_directory']\n"
                                                "3. Output files written to working directory via "
                                                "os.path.join()\n"
                                                "4. Proper error handling for file I/O\n"
                                                "5. All imports at top of file\n"
                                                "6. Consistent data types throughout (don't store a value as "
                                                "a string then compare it as a different type later)\n"
                                                "7. No use of sys.argv, argparse, or if __name__ == "
                                                "'__main__'\n";
                                }
                                sRevTask += "\nIf you find issues, fix them. If it's correct, output it "
                                            "unchanged.\n"
                                            "Output ONLY the script. Nothing else.";

                                std::string const sRevCntx = "--- Script to Review ---\n" + scriptContent +
                                                             "\n\n--- JCWF Workflow ---\n" + generatedJcwf +
                                                             "\n\n--- User Request ---\n" + userPrompt;
                                std::string const sRevProb = "Review and fix if needed: " + scriptPath;

                                std::string reviewedScript;
                                std::string reviewError;
                                if (RunSingleAiCall("gen_" + seqStr + "_script_" + std::to_string(i) + "_review", sRevStng,
                                                    sRevTask, sRevCntx, sRevProb, reviewedScript, reviewError))
                                {
                                    scriptContent = StripMarkdownFences(reviewedScript);
                                    LOG_APP_INFO("[workflow] script '{}' reviewed in run '{}'", scriptPath, runId);
                                }
                                else
                                {
                                    LOG_APP_WARN("[workflow] script review failed for '{}': {}", scriptPath, reviewError);
                                }
                            }

                            GeneratedScript gs;
                            gs.path = scriptPath;
                            gs.content = scriptContent;
                            gs.executable = isShell;
                            generatedScripts.push_back(std::move(gs));

                            LOG_APP_INFO("[workflow] generated script '{}' in run '{}'", scriptPath, runId);
                        }
                    }

                    LOG_APP_INFO("[workflow] task 'generate_scripts' completed in run '{}' ({} scripts generated)", runId,
                                 generatedScripts.size());
                }

                // Minimum annunciation time for stages 4 and 5 (so the user can see them).
                static constexpr int64_t MIN_STAGE_DISPLAY_MS = 500;

                auto ensureMinDisplay = [](std::chrono::steady_clock::time_point const& start)
                {
                    auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                            .count();
                    if (elapsed < MIN_STAGE_DISPLAY_MS)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(MIN_STAGE_DISPLAY_MS - elapsed));
                    }
                };

                // Validation result bundle: valid flag, summary string, raw issues.
                struct ValidationResult
                {
                    bool valid;
                    std::string summary;
                    std::vector<WorkflowValidationIssue> issues;
                };

                // Helper: run validation and return {valid, summary, issues}
                auto runValidation = [&](std::string const& taskLabel) -> ValidationResult
                {
                    ScriptRegistry const* scriptRegistry = nullptr;
                    if (JarvisAgent* app = App::g_App.load(std::memory_order_acquire); app != nullptr)
                    {
                        scriptRegistry = app->GetScriptRegistry();
                    }
                    ValidationResult vr;
                    ValidateJcwf(generatedJcwf, vr.summary, scriptRegistry, &generatedScripts, &vr.issues);
                    vr.valid =
                        vr.summary.empty() || !std::any_of(vr.issues.begin(), vr.issues.end(), [](auto const& i)
                                                           { return i.m_Severity == WorkflowValidationSeverity::Error; });
                    if (!vr.summary.empty())
                    {
                        LOG_APP_WARN("[workflow] task '{}' in run '{}':\n{}", taskLabel, runId, vr.summary);
                    }
                    return vr;
                };

                // stripMarkdownFences is now a static function in the anonymous namespace above.

                // ----------------------------------------------------------
                // Stage 4: Validate (first pass)
                // ----------------------------------------------------------
                {
                    if (m_ShuttingDown.load())
                    {
                        broadcastResult(false, "Service is shutting down", 0);
                        return;
                    }

                    auto stageStart = std::chrono::steady_clock::now();
                    broadcastProgress(4, "Validating...");
                    LOG_APP_INFO("[workflow] task 'validate' executing in run '{}' (workflow '{}')", runId, workflowId);

                    auto vr = runValidation("validate");

                    LOG_APP_INFO("[workflow] task 'validate' completed in run '{}' (workflow '{}') — {} errors, {} warnings",
                                 runId, workflowId, vr.valid ? "no" : "has", vr.summary.empty() ? "no" : "has");
                    ensureMinDisplay(stageStart);

                    // ----------------------------------------------------------
                    // Stage 5: Fix-It (if there are any errors or warnings)
                    // ----------------------------------------------------------
                    if (m_ShuttingDown.load())
                    {
                        broadcastResult(false, "Service is shutting down", 0);
                        return;
                    }

                    if (vr.summary.empty())
                    {
                        // No errors, no warnings — announce and finish
                        auto fixStart = std::chrono::steady_clock::now();
                        broadcastProgress(5, "No errors, no warnings to fix");
                        LOG_APP_INFO("[workflow] task 'fix' skipped in run '{}' — nothing to fix", runId);
                        ensureMinDisplay(fixStart);
                        broadcastResult(true, generatedJcwf, 0);
                        return;
                    }

                    // There are issues (errors and/or warnings) — ask AI to fix them
                    auto fixStart = std::chrono::steady_clock::now();
                    broadcastProgress(5, "Fixing errors and warnings...");
                    LOG_APP_INFO("[workflow] task 'fix' executing in run '{}' (workflow '{}')", runId, workflowId);

                    std::string const fixStng =
                        "You are a JCWF code fixer. Output ONLY valid JSON — no markdown fences, no explanations, "
                        "no introductory or closing commentary. Fix all validation errors AND warnings while "
                        "preserving the workflow's intended behavior.";

                    // Check if we can use targeted patching:
                    // All issues must be task-specific AND the workflow must be large.
                    bool allTaskSpecific =
                        !vr.issues.empty() && std::all_of(vr.issues.begin(), vr.issues.end(),
                                                          [](auto const& i)
                                                          {
                                                              return !i.m_TaskId.empty() &&
                                                                     (i.m_Severity == WorkflowValidationSeverity::Error ||
                                                                      i.m_Severity == WorkflowValidationSeverity::Warning);
                                                          });
                    bool useTargetedFix = useFanOut && allTaskSpecific;

                    std::string fixedJcwf;
                    std::string fixError;

                    if (useTargetedFix)
                    {
                        // ---- Targeted fix: send only affected tasks + skeleton ----
                        // Collect unique affected task IDs
                        std::set<std::string> affectedIds;
                        for (auto const& issue : vr.issues)
                        {
                            affectedIds.insert(issue.m_TaskId);
                        }

                        // Build context: decomposition (compact skeleton) + affected task blocks
                        std::string affectedBlocks;
                        for (auto const& tid : affectedIds)
                        {
                            std::string block = ExtractTaskBlock(generatedJcwf, tid);
                            if (!block.empty())
                            {
                                affectedBlocks += "\n--- Task: " + tid + " ---\n" + block + "\n";
                            }
                        }

                        std::string const fixTask =
                            "The workflow has validation issues in specific tasks. Fix ONLY the listed tasks.\n"
                            "Output format: { \"tasks\": { \"taskId\": {...}, ... } }\n"
                            "Include ONLY the fixed tasks in the output — no workflow-level fields.\n\n"
                            "Validation issues:\n" +
                            vr.summary;

                        std::string const fixCntx = "--- Task Breakdown (all tasks, for reference) ---\n" + decomposition +
                                                    "\n\n--- Affected Task Blocks (current, to be fixed) ---\n" +
                                                    affectedBlocks + "\n\n--- JCWF Generation Guide ---\n" + generationGuide;

                        std::string const fixProb = "Fix the listed tasks.";

                        LOG_APP_INFO("[AiJcwfService] Using targeted fix for {} affected task(s)", affectedIds.size());

                        if (!RunSingleAiCall("gen_" + seqStr + "_fix", fixStng, fixTask, fixCntx, fixProb, fixedJcwf,
                                             fixError))
                        {
                            LOG_APP_ERROR("[workflow] task 'fix' failed in run '{}': {}", runId, fixError);
                            ensureMinDisplay(fixStart);
                            broadcastResult(!WorkflowValidator::HasErrors(vr.issues), generatedJcwf, 1);
                            return;
                        }

                        // Patch the fixed tasks back into the full JCWF
                        generatedJcwf = PatchTasksIntoJcwf(generatedJcwf, StripMarkdownFences(fixedJcwf));
                    }
                    else
                    {
                        // ---- Full-JCWF fix (original behavior) ----
                        std::string const fixTask =
                            "The JCWF JSON below has validation issues. Fix ALL errors AND warnings, then output the "
                            "corrected JCWF JSON. Output ONLY the fixed JSON, nothing else.\n\n"
                            "Validation issues:\n" +
                            vr.summary;

                        std::string const fixCntx = "--- Current JCWF ---\n" + generatedJcwf +
                                                    "\n\n--- JCWF Generation Guide ---\n" + generationGuide;

                        std::string const fixProb = "Fix the JCWF JSON.";

                        if (!RunSingleAiCall("gen_" + seqStr + "_fix", fixStng, fixTask, fixCntx, fixProb, fixedJcwf,
                                             fixError, kJcwfSchemaJson))
                        {
                            LOG_APP_ERROR("[workflow] task 'fix' failed in run '{}': {}", runId, fixError);
                            ensureMinDisplay(fixStart);
                            broadcastResult(!WorkflowValidator::HasErrors(vr.issues), generatedJcwf, 1);
                            return;
                        }

                        generatedJcwf = StripMarkdownFences(fixedJcwf);
                    }

                    LOG_APP_INFO("[workflow] task 'fix' completed in run '{}' (workflow '{}') [targeted={}]", runId,
                                 workflowId, useTargetedFix);
                    ensureMinDisplay(fixStart);
                }

                // ----------------------------------------------------------
                // Stage 4 (second pass): Re-validate after fix
                // ----------------------------------------------------------
                {
                    if (m_ShuttingDown.load())
                    {
                        broadcastResult(false, "Service is shutting down", 1);
                        return;
                    }

                    auto stageStart = std::chrono::steady_clock::now();
                    broadcastProgress(4, "Re-validating...");
                    LOG_APP_INFO("[workflow] task 'revalidate' executing in run '{}' (workflow '{}')", runId, workflowId);

                    auto vr2 = runValidation("revalidate");

                    if (!vr2.summary.empty())
                    {
                        LOG_APP_WARN("[workflow] task 'revalidate' still has issues in run '{}':\n{}", runId, vr2.summary);
                    }
                    LOG_APP_INFO("[workflow] task 'revalidate' completed in run '{}' (workflow '{}')", runId, workflowId);
                    ensureMinDisplay(stageStart);

                    // Accept result regardless — we only do one fix iteration
                    broadcastResult(true, generatedJcwf, 1);
                }
            });
    }

    void AiJcwfService::FixFailedScriptAsync(std::string const& scriptPath, std::string const& stderrContent,
                                             std::string const& taskType)
    {
        JoinFinishedThreads();

        std::lock_guard<std::mutex> lock(m_ThreadsMutex);
        m_BackgroundThreads.emplace_back(
            [this, scriptPath, stderrContent, taskType]()
            {
                if (m_ShuttingDown.load())
                {
                    Broadcast(R"({"type":"ai-fix-script-result","ok":false,"error":"Service is shutting down"})");
                    return;
                }

                Broadcast(R"({"type":"ai-fix-script-progress","message":"Reading script..."})");

                // Read the current script from disk
                std::string scriptContent;
                {
                    std::ifstream ifs(scriptPath);
                    if (!ifs)
                    {
                        Broadcast(R"({"type":"ai-fix-script-result","ok":false,"error":"Cannot read script: )" +
                                  JsonHelper::EscapeJsonString(scriptPath) + R"("})");
                        return;
                    }
                    std::ostringstream ss;
                    ss << ifs.rdbuf();
                    scriptContent = ss.str();
                }

                if (m_ShuttingDown.load())
                {
                    Broadcast(R"({"type":"ai-fix-script-result","ok":false,"error":"Service is shutting down"})");
                    return;
                }

                Broadcast(R"({"type":"ai-fix-script-progress","message":"Sending to AI for fix..."})");

                bool const isShell = scriptPath.ends_with(".sh");
                bool const isPs1Fix = scriptPath.ends_with(".ps1");

                // Build the fix prompt
                std::string const stng = "You are a script fixer. Output ONLY the corrected script. "
                                         "No markdown fences. No explanations. No commentary.";

                std::string task = "The script failed at runtime with the following error output:\n\n"
                                   "--- stderr ---\n" +
                                   stderrContent +
                                   "\n--- end stderr ---\n\n"
                                   "Host OS: " +
                                   GetHostOsDescription() +
                                   "\n\n"
                                   "Fix the script so it runs without errors. Preserve its original purpose and logic.\n"
                                   "Rules:\n";

                if (isShell)
                {
                    task += "- First line: #!/usr/bin/env bash\n"
                            "- Second line: # @jarvis-script\n"
                            "- Include # @short: ... and # @description: ... metadata\n"
                            "- After the metadata header: set -euo pipefail\n"
                            "- Use positional args ($1, $2, ...) for file parameters. NEVER hardcode paths.\n"
                            "- POSIX awk only: no arr[k1][k2], no 3-arg match(), "
                            "no asort()/asorti(), no nextfile, no PROCINFO, no gensub(). "
                            "Use SUBSEP keys, external sort, and gsub().\n"
                            "- Output ONLY the fixed script.";
                }
                else if (isPs1Fix)
                {
                    task += "- First line: # @jarvis-script (no shebang for PowerShell)\n"
                            "- Include # @short: ... and # @description: ... metadata\n"
                            "- param([string]$Arg1, ...) block after metadata\n"
                            "- Set-StrictMode -Version Latest and $ErrorActionPreference = 'Stop' after param()\n"
                            "- Use $Arg1, $Arg2, ... for file parameters. NEVER hardcode paths.\n"
                            "- Use PowerShell cmdlets. No POSIX awk/sed/grep. No bash syntax.\n"
                            "- Output ONLY the fixed script.";
                }
                else
                {
                    task += "- First line: #!/usr/bin/env python3\n"
                            "- Second line: # @jarvis-script\n"
                            "- Include # @short: ... and # @description: ... metadata\n"
                            "- Output ONLY the fixed script.";
                }

                std::string const cntx = "--- Current Script (" + scriptPath + ") ---\n" + scriptContent;
                std::string const prob = "Fix the runtime error in " + scriptPath;

                std::string fixedContent;
                std::string fixError;

                std::string const subfolder = "fix_script_" + std::to_string(m_NextRequestSeq.fetch_add(1));

                bool const ok = RunSingleAiCall(subfolder, stng, task, cntx, prob, fixedContent, fixError);

                if (!ok)
                {
                    Broadcast(R"({"type":"ai-fix-script-result","ok":false,"error":")" + JsonHelper::EscapeJsonString(fixError) + R"("})");
                    return;
                }

                // Strip markdown fences if the AI wrapped them
                fixedContent = StripMarkdownFences(fixedContent);

                // Broadcast result with the fixed script for review
                std::ostringstream ss;
                ss << R"({"type":"ai-fix-script-result","ok":true,"scripts":[{"path":")" << JsonHelper::EscapeJsonString(scriptPath)
                   << R"(","content":")" << JsonHelper::EscapeJsonString(fixedContent) << R"(","executable":)" << (isShell ? "true" : "false")
                   << R"(}]})";
                Broadcast(ss.str());

                LOG_APP_INFO("[ai-fix-script] Fixed script '{}' — sending to frontend for review", scriptPath);
            });
    }

} // namespace AIAssistant
