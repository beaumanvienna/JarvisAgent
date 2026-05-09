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

#include "workflow/workflowRuntimeManager.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <optional>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <curl/curl.h>

#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "file/pathConfinement.h"
#include "jarvisAgent.h"
#include "json/jsonHelper.h"
#include "workflow/dataflowResolver.h"
#include "workflow/jcwfContainer.h"
#include "workflow/taskExecutorRegistry.h"
#include "workflow/taskFreshnessChecker.h"
#include "workflow/taskPathResolver.h"
#include "workflow/workflowRegistry.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {
        // Recursively flatten a simdjson DOM element into dotted-path key/value pairs.
        // Objects → "parent.child", arrays → "parent[0]", leaves → string repr.
        void FlattenJsonDom(simdjson::dom::element elem, std::string const& prefix,
                            std::unordered_map<std::string, std::string>& out)
        {
            if (elem.is_object())
            {
                simdjson::dom::object obj = elem;
                for (auto field : obj)
                {
                    std::string key(field.key);
                    std::string nested = prefix.empty() ? key : prefix + "." + key;
                    FlattenJsonDom(field.value, nested, out);
                }
            }
            else if (elem.is_array())
            {
                simdjson::dom::array arr = elem;
                size_t idx = 0;
                for (auto child : arr)
                {
                    std::string nested = prefix + "[" + std::to_string(idx++) + "]";
                    FlattenJsonDom(child, nested, out);
                }
            }
            else if (elem.is_string())
            {
                std::string_view sv = elem;
                out[prefix] = std::string(sv);
            }
            else if (elem.is_int64())
            {
                out[prefix] = std::to_string(int64_t(elem));
            }
            else if (elem.is_uint64())
            {
                out[prefix] = std::to_string(uint64_t(elem));
            }
            else if (elem.is_double())
            {
                std::ostringstream oss;
                oss << double(elem);
                out[prefix] = oss.str();
            }
            else if (elem.is_bool())
            {
                out[prefix] = bool(elem) ? "true" : "false";
            }
            else if (elem.is_null())
            {
                out[prefix] = "";
            }
        }

        // Forward declaration — defined below; used by InjectUpstreamOutputs.
        void InjectUpstreamAiStructuredOutput(std::string const& depIdPrefix,
                                              TaskInstanceState const& upstreamState,
                                              std::unordered_map<std::string, std::string>& targetInputValues);

        // Inject upstream task outputs into a downstream task's InputValues map.
        // Populates {{depIdPrefix.captured_stdout}}, {{depIdPrefix.output_file}},
        // {{depIdPrefix.SLOT}} for each declared output slot, and flattened
        // {{depIdPrefix.json.PATH}} entries parsed from response.json if present
        // in the upstream task's working directory.
        //
        // upstreamInstanceId is the full instance id of the source task state (e.g.
        // "createIssue" for regular tasks, "createIssue#3" for per-item child 3).
        // The suffix after '#' — if any — selects the matching per-child response
        // file (response_3.json) so per-item chains read their own index.
        void InjectUpstreamOutputs(WorkflowDefinition const& workflowDefinition, TaskDef const& upstreamDef,
                                   TaskInstanceState const& upstreamState, std::string const& depIdPrefix,
                                   std::string const& upstreamInstanceId,
                                   std::unordered_map<std::string, std::string>& targetInputValues)
        {
            if (!upstreamState.m_CapturedStdout.empty())
            {
                targetInputValues[depIdPrefix + ".captured_stdout"] = upstreamState.m_CapturedStdout;
            }

            if (!upstreamState.m_OutputValues.empty())
            {
                std::vector<std::pair<std::string, std::string>> sortedOutputs(
                    upstreamState.m_OutputValues.begin(), upstreamState.m_OutputValues.end());
                std::sort(sortedOutputs.begin(), sortedOutputs.end());

                targetInputValues[depIdPrefix + ".output_file"] = sortedOutputs.front().second;
                for (auto const& [slotName, slotValue] : sortedOutputs)
                {
                    targetInputValues[depIdPrefix + "." + slotName] = slotValue;
                }
            }

            // JSON-path expansion from upstream ai_call's structured output (schema-validated reply).
            InjectUpstreamAiStructuredOutput(depIdPrefix, upstreamState, targetInputValues);

            // JSON-path expansion from upstream's response.json (cloud tasks).
            std::filesystem::path const workflowBaseDir =
                TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);
            if (workflowBaseDir.empty() || upstreamDef.m_WorkingDirectory.empty())
            {
                return;
            }

            std::filesystem::path const taskDir =
                TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDir, upstreamDef.m_WorkingDirectory);

            // Pick response.json or response_<N>.json based on instance id.
            std::string responseFilename = "response.json";
            auto const hashPos = upstreamInstanceId.rfind('#');
            if (hashPos != std::string::npos)
            {
                std::string const suffix = upstreamInstanceId.substr(hashPos + 1);
                if (!suffix.empty())
                {
                    responseFilename = "response_" + suffix + ".json";
                }
            }
            std::filesystem::path const responseJsonPath = taskDir / responseFilename;

            std::error_code ec;
            if (!std::filesystem::exists(responseJsonPath, ec))
            {
                return;
            }

            try
            {
                simdjson::dom::parser parser;
                simdjson::dom::element root;
                auto err = parser.load(responseJsonPath.string()).get(root);
                if (err != simdjson::SUCCESS)
                {
                    LOG_APP_WARN("[upstream] failed to parse response.json for task '{}': {}", depIdPrefix,
                                 simdjson::error_message(err));
                    return;
                }

                std::unordered_map<std::string, std::string> flattened;
                FlattenJsonDom(root, "", flattened);

                for (auto const& [path, value] : flattened)
                {
                    targetInputValues[depIdPrefix + ".json." + path] = value;
                }

                LOG_APP_INFO("[upstream] injected {} json fields from task '{}' response.json", flattened.size(),
                             depIdPrefix);
            }
            catch (std::exception const& e)
            {
                LOG_APP_WARN("[upstream] exception reading response.json for task '{}': {}", depIdPrefix, e.what());
            }
        }

        // Flatten an upstream ai_call's structured-output JSON into {{A.json.PATH}}.
        // When an ai_call declares `output_schema`, the validated reply lands at a
        // `.json` file recorded in the task's m_OutputValues map (either a PROB-derived
        // `<stem>.output.json` or an explicit `file_outputs` path). Parsing it here
        // makes schema-validated fields first-class template sources — symmetric to
        // the cloud-task `response.json` handling above — so downstream tasks can
        // reference e.g. `{{classify.json.category}}` instead of parsing raw text
        // out of `captured_stdout`.
        void InjectUpstreamAiStructuredOutput(std::string const& depIdPrefix,
                                              TaskInstanceState const& upstreamState,
                                              std::unordered_map<std::string, std::string>& targetInputValues)
        {
            if (upstreamState.m_OutputValues.empty())
            {
                return;
            }

            std::string jsonFilePath;
            for (auto const& [slotName, slotValue] : upstreamState.m_OutputValues)
            {
                if (slotValue.size() >= 5 &&
                    slotValue.compare(slotValue.size() - 5, 5, ".json") == 0)
                {
                    jsonFilePath = slotValue;
                    break;
                }
            }

            if (jsonFilePath.empty())
            {
                return;
            }

            std::error_code ec;
            if (!std::filesystem::exists(jsonFilePath, ec) || ec)
            {
                return;
            }

            try
            {
                simdjson::dom::parser parser;
                simdjson::dom::element root;
                auto err = parser.load(jsonFilePath).get(root);
                if (err != simdjson::SUCCESS)
                {
                    LOG_APP_WARN("[upstream] failed to parse ai_call structured output for task '{}' at '{}': {}",
                                 depIdPrefix, jsonFilePath, simdjson::error_message(err));
                    return;
                }

                std::unordered_map<std::string, std::string> flattened;
                FlattenJsonDom(root, "", flattened);

                for (auto const& [path, value] : flattened)
                {
                    targetInputValues[depIdPrefix + ".json." + path] = value;
                }

                LOG_APP_INFO("[upstream] injected {} json fields from task '{}' structured output at '{}'",
                             flattened.size(), depIdPrefix, jsonFilePath);
            }
            catch (std::exception const& e)
            {
                LOG_APP_WARN("[upstream] exception reading structured output for task '{}' at '{}': {}",
                             depIdPrefix, jsonFilePath, e.what());
            }
        }

        // fnmatch-style matcher for the limited glob set used in JCWF
        // `file_outputs` patterns.  Supports `*` (zero or more characters,
        // anywhere — not just at start/end) and `?` (exactly one character).
        // No bracket expressions, no path-separator semantics — patterns are
        // applied to a single filename in a known directory, never to a
        // multi-segment path.  Iterative two-pointer match with backtracking;
        // O(name × pattern) worst case, no allocations.  Replaces an ad-hoc
        // matcher that only handled `*` at the very start or very end and
        // would silently fail on patterns like `PROB_*.json`.  Path-confinement
        // still gates the resulting paths via
        // ConfineUnderProjectRoot, so a too-permissive pattern can only ever
        // match files inside the project tree.
        bool GlobMatchesFilename(std::string_view pattern, std::string_view name)
        {
            size_t pi = 0;
            size_t ni = 0;
            size_t starPi = std::string_view::npos;
            size_t starNi = 0;

            while (ni < name.size())
            {
                if (pi < pattern.size() && pattern[pi] == '*')
                {
                    starPi = pi++;
                    starNi = ni;
                }
                else if (pi < pattern.size() &&
                         (pattern[pi] == '?' || pattern[pi] == name[ni]))
                {
                    ++pi;
                    ++ni;
                }
                else if (starPi != std::string_view::npos)
                {
                    pi = starPi + 1;
                    ni = ++starNi;
                }
                else
                {
                    return false;
                }
            }
            // Consume any trailing `*` chars in the pattern.
            while (pi < pattern.size() && pattern[pi] == '*')
            {
                ++pi;
            }
            return pi == pattern.size();
        }

        // Allowlist for run / workflow identifiers reaching the runtime layer.
        // Already enforced at the REST boundary (`IsValidWorkflowId` in webServer)
        // — this is defense-in-depth at the runtime entrypoints, so a future
        // direct caller (integration / test harness / new MCP tool) cannot
        // smuggle path-traversal segments or shell-metachar identifiers into
        // the on-disk paths the runtime later builds (queue/<id>, run-state
        // tracking maps, log lines).
        constexpr size_t kMaxRunIdLen = 256;
        bool IsValidRunOrWorkflowId(std::string const& id)
        {
            if (id.empty() || id.size() > kMaxRunIdLen)
            {
                return false;
            }
            for (char const c : id)
            {
                bool const ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') ||
                                c == '_' || c == '-' || c == '.';
                if (!ok)
                {
                    return false;
                }
            }
            // Refuse `..` and dot-prefixed identifiers — they survive the
            // charset check but still pose a containment hazard if joined
            // into a path.
            if (id == "." || id == ".." || id.front() == '.')
            {
                return false;
            }
            return true;
        }

        std::string GetIso8601NowUTC()
        {
            auto const now = std::chrono::system_clock::now();
            std::time_t const nowTimeT = std::chrono::system_clock::to_time_t(now);

            std::tm utcTime{};
#if defined(_WIN32)
            gmtime_s(&utcTime, &nowTimeT);
#else
            gmtime_r(&nowTimeT, &utcTime);
#endif

            std::ostringstream stream;
            stream << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
            return stream.str();
        }

        void PopulateSkippedTaskOutputsIfPossible(WorkflowDefinition const& workflowDefinition,
                                                  WorkflowRun const& workflowRun, TaskDef const& taskDefinition,
                                                  std::string const& taskId, TaskInstanceState& taskState)
        {
            std::vector<fs::path> unusedInputPaths;
            std::vector<fs::path> resolvedOutputPaths;

            if (!TaskPathResolver::ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                                unusedInputPaths, resolvedOutputPaths))
            {
                return;
            }

            if (taskDefinition.m_Outputs.empty() || resolvedOutputPaths.empty())
            {
                return;
            }

            std::vector<std::string> outputSlotNames;
            outputSlotNames.reserve(taskDefinition.m_Outputs.size());

            for (auto const& outputPair : taskDefinition.m_Outputs)
            {
                outputSlotNames.push_back(outputPair.first);
            }

            std::sort(outputSlotNames.begin(), outputSlotNames.end());

            if (outputSlotNames.size() == resolvedOutputPaths.size())
            {
                for (size_t index = 0; index < outputSlotNames.size(); ++index)
                {
                    taskState.m_OutputValues[outputSlotNames[index]] = resolvedOutputPaths[index].string();
                }
            }
            else if (resolvedOutputPaths.size() == 1)
            {
                std::string const onlyPath = resolvedOutputPaths[0].string();
                for (std::string const& slotName : outputSlotNames)
                {
                    taskState.m_OutputValues[slotName] = onlyPath;
                }
            }
            else if (outputSlotNames.size() == 1)
            {
                taskState.m_OutputValues[outputSlotNames[0]] = resolvedOutputPaths[0].string();
            }
            else
            {
                return;
            }

            {
                std::string summary;
                for (auto const& p : taskState.m_OutputValues)
                {
                    summary += p.first;
                    summary += "=";
                    summary += p.second;
                    summary += ";";
                }
                taskState.m_OutputsJson = summary;
            }
        }

        bool IsTerminal(TaskInstanceStateKind const state)
        {
            return (state == TaskInstanceStateKind::Succeeded || state == TaskInstanceStateKind::Skipped ||
                    state == TaskInstanceStateKind::Failed);
        }

        std::string WorkflowRunStateToString(WorkflowRunState const state)
        {
            // Closed enum — compiler -Wswitch catches missing arms when a variant is
            // added.  static_assert pins the count so a renumbering or new tail-variant
            // forces this site to be revisited.  No `default:` arm — fail-loud beats a
            // silent "unknown" fallback.
            static_assert(static_cast<int>(WorkflowRunState::Stopped) == 7,
                          "WorkflowRunState variant count changed — extend WorkflowRunStateToString");
            switch (state)
            {
                case WorkflowRunState::Pending:
                    return "pending";
                case WorkflowRunState::Running:
                    return "running";
                case WorkflowRunState::Paused:
                    return "paused";
                case WorkflowRunState::Stopping:
                    return "stopping";
                case WorkflowRunState::Succeeded:
                    return "succeeded";
                case WorkflowRunState::Failed:
                    return "failed";
                case WorkflowRunState::Cancelled:
                    return "cancelled";
                case WorkflowRunState::Stopped:
                    return "stopped";
            }
            return "unknown";
        }

        // ---- SSRF defense for the completion callback URL ----
        // The callbackUrl is workflow context, which is partially attacker-influenced
        // (a JCWF or webhook trigger payload can seed context).  Without this gate, a
        // malicious workflow could point the callback at internal services
        // (cloud-metadata endpoints like 169.254.169.254, intra-VPC databases, the
        // local control-plane) and exfiltrate task outputs to them.

        bool IsIp4InRejectedRange(uint32_t ip4HostOrder)
        {
            // Rejected ranges (host-byte order).  All loopback / private / link-local /
            // multicast / cloud-metadata / unspecified ranges are blocked.
            uint8_t const a = static_cast<uint8_t>((ip4HostOrder >> 24) & 0xFF);
            uint8_t const b = static_cast<uint8_t>((ip4HostOrder >> 16) & 0xFF);
            if (a == 0)        return true; // 0.0.0.0/8 unspecified
            if (a == 10)       return true; // RFC 1918
            if (a == 127)      return true; // loopback
            if (a == 169 && b == 254) return true; // link-local + cloud-metadata 169.254.169.254
            if (a == 172 && (b >= 16 && b <= 31)) return true; // RFC 1918
            if (a == 192 && b == 168) return true; // RFC 1918
            if (a == 100 && (b >= 64 && b <= 127)) return true; // CGNAT
            if (a >= 224)      return true; // multicast + reserved
            return false;
        }

        bool IsIp6Rejected(in6_addr const& addr)
        {
            // Loopback ::1
            static uint8_t const loopback[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
            if (std::memcmp(addr.s6_addr, loopback, 16) == 0) return true;
            // Unspecified ::
            static uint8_t const unspec[16] = {0};
            if (std::memcmp(addr.s6_addr, unspec, 16) == 0) return true;
            // Link-local fe80::/10
            if (addr.s6_addr[0] == 0xfe && (addr.s6_addr[1] & 0xc0) == 0x80) return true;
            // Unique-local fc00::/7
            if ((addr.s6_addr[0] & 0xfe) == 0xfc) return true;
            // Multicast ff00::/8
            if (addr.s6_addr[0] == 0xff) return true;
            // IPv4-mapped ::ffff:0:0/96 — extract the v4 and re-check
            static uint8_t const v4mapPrefix[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
            if (std::memcmp(addr.s6_addr, v4mapPrefix, 12) == 0)
            {
                uint32_t const v4 = (uint32_t(addr.s6_addr[12]) << 24) | (uint32_t(addr.s6_addr[13]) << 16) |
                                    (uint32_t(addr.s6_addr[14]) << 8)  |  uint32_t(addr.s6_addr[15]);
                return IsIp4InRejectedRange(v4);
            }
            return false;
        }

        // Returns true iff the URL is safe to use as a completion-callback target.
        // outReason is filled with a short human-readable reason on rejection.
        // Allowlist-style: scheme must be https, host must resolve only to public IPs.
        bool IsCallbackUrlAllowed(std::string const& url, std::string& outReason)
        {
            outReason.clear();

            // Require https:// prefix.  Plain http (or other schemes) leaks the
            // payload over the wire and removes peer-cert verification's protection.
            constexpr char const kHttpsPrefix[] = "https://";
            constexpr size_t kHttpsPrefixLen = sizeof(kHttpsPrefix) - 1;
            if (url.size() <= kHttpsPrefixLen ||
                url.compare(0, kHttpsPrefixLen, kHttpsPrefix) != 0)
            {
                outReason = "scheme not https";
                return false;
            }

            // Extract host (between "://" and the first '/', '?', '#', or end).
            size_t hostStart = kHttpsPrefixLen;
            // Skip optional userinfo "user:pass@"
            size_t const atPos = url.find('@', hostStart);
            size_t const pathPos = url.find_first_of("/?#", hostStart);
            if (atPos != std::string::npos && (pathPos == std::string::npos || atPos < pathPos))
            {
                hostStart = atPos + 1;
            }
            size_t hostEnd = url.find_first_of("/?#", hostStart);
            if (hostEnd == std::string::npos)
            {
                hostEnd = url.size();
            }
            std::string hostport = url.substr(hostStart, hostEnd - hostStart);
            if (hostport.empty())
            {
                outReason = "empty host";
                return false;
            }

            // Strip "[" "]" for bracketed IPv6 + the trailing ":port" if present.
            std::string host;
            if (!hostport.empty() && hostport.front() == '[')
            {
                size_t const closeBracket = hostport.find(']');
                if (closeBracket == std::string::npos)
                {
                    outReason = "malformed bracketed host";
                    return false;
                }
                host = hostport.substr(1, closeBracket - 1);
            }
            else
            {
                size_t const colonPos = hostport.find(':');
                host = (colonPos == std::string::npos) ? hostport : hostport.substr(0, colonPos);
            }

            if (host.empty())
            {
                outReason = "empty host after parsing";
                return false;
            }

            // Resolve host (IP literal OR DNS name).  Reject if ANY resolved address
            // is in a private / loopback / link-local / multicast range — closes the
            // DNS-rebinding-style attack where a hostname has both public and private
            // A records, or where an attacker-controlled DNS returns a private IP.
            struct addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICSERV;
            struct addrinfo* results = nullptr;
            int const gai = ::getaddrinfo(host.c_str(), "443", &hints, &results);
            if (gai != 0 || results == nullptr)
            {
                // gai_strerror's char width is platform-dependent: on Windows
                // when _UNICODE is defined it expands to gai_strerrorW which
                // returns WCHAR*, breaking the ternary against the narrow
                // string literal.  Pin to the ANSI variant on Windows.
#if defined(_WIN32)
                char const* const gaiMessage = (gai == 0) ? "no addresses" : ::gai_strerrorA(gai);
#else
                char const* const gaiMessage = (gai == 0) ? "no addresses" : ::gai_strerror(gai);
#endif
                outReason = std::string("DNS resolution failed: ") + gaiMessage;
                if (results) { ::freeaddrinfo(results); }
                return false;
            }
            for (struct addrinfo* it = results; it != nullptr; it = it->ai_next)
            {
                if (it->ai_family == AF_INET)
                {
                    auto const* sin = reinterpret_cast<struct sockaddr_in const*>(it->ai_addr);
                    uint32_t const ip4 = ntohl(sin->sin_addr.s_addr);
                    if (IsIp4InRejectedRange(ip4))
                    {
                        char buf[INET_ADDRSTRLEN] = {};
                        ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
                        outReason = std::string("resolves to internal IPv4 ") + buf;
                        ::freeaddrinfo(results);
                        return false;
                    }
                }
                else if (it->ai_family == AF_INET6)
                {
                    auto const* sin6 = reinterpret_cast<struct sockaddr_in6 const*>(it->ai_addr);
                    if (IsIp6Rejected(sin6->sin6_addr))
                    {
                        char buf[INET6_ADDRSTRLEN] = {};
                        ::inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
                        outReason = std::string("resolves to internal IPv6 ") + buf;
                        ::freeaddrinfo(results);
                        return false;
                    }
                }
            }
            ::freeaddrinfo(results);
            return true;
        }

        // Fire-and-forget POST to the callbackUrl with the run completion payload.
        // Runs on a detached thread to avoid blocking the main loop.
        void FireCompletionCallback(WorkflowRun const& workflowRun)
        {
            auto const callbackIterator = workflowRun.m_Context.find("callbackUrl");
            if (callbackIterator == workflowRun.m_Context.end() || callbackIterator->second.m_Value.empty())
            {
                return;
            }

            std::string const callbackUrl = callbackIterator->second.m_Value;
            std::string const workflowId = workflowRun.m_WorkflowId;
            std::string const runId = workflowRun.m_RunId;
            std::string const state = WorkflowRunStateToString(workflowRun.m_State);
            std::string const completedAt = workflowRun.m_CompletedAtIso8601;
            bool const hasFailed = workflowRun.m_HasFailed;

            // Output content is included by default for backwards compatibility.
            // Setting `callback_include_outputs` to "false" / "0" / "no" in the run
            // context strips per-task output values + file contents from the payload,
            // leaving only run-level state + per-task state + error messages.
            // Callers handling sensitive data (PII, secrets, output blobs that could
            // contain credentials) should opt out explicitly so a leaked callback URL
            // doesn't exfiltrate the content.
            bool includeOutputs = true;
            if (auto const it = workflowRun.m_Context.find("callback_include_outputs");
                it != workflowRun.m_Context.end())
            {
                std::string const v = it->second.m_Value;
                if (v == "false" || v == "0" || v == "no" || v == "False" || v == "FALSE")
                {
                    includeOutputs = false;
                }
            }

            // SSRF gate — refuse callbacks to internal/loopback/private endpoints.
            // The URL field of m_Context is partially attacker-influenced, so we
            // canonicalise + DNS-resolve it BEFORE building the payload (cheap
            // failure path; no body is rendered if the target is rejected).
            {
                std::string ssrfReason;
                if (!IsCallbackUrlAllowed(callbackUrl, ssrfReason))
                {
                    LOG_APP_ERROR("[callback] refused completion callback for run '{}' to '{}': {}",
                                  runId, callbackUrl, ssrfReason);
                    return;
                }
            }

            // Build per-task summary.
            std::string taskSummaryJson;
            {
                taskSummaryJson += "{";
                bool first = true;
                for (auto const& [taskId, taskState] : workflowRun.m_TaskStates)
                {
                    if (!first)
                    {
                        taskSummaryJson += ",";
                    }
                    first = false;

                    static_assert(static_cast<int>(TaskInstanceStateKind::WaitingExternal) == 6,
                                  "TaskInstanceStateKind variant count changed — extend this switch");
                    std::string taskStateStr;
                    switch (taskState.m_State)
                    {
                        case TaskInstanceStateKind::Pending:
                            taskStateStr = "pending";
                            break;
                        case TaskInstanceStateKind::Ready:
                            taskStateStr = "ready";
                            break;
                        case TaskInstanceStateKind::Running:
                            taskStateStr = "running";
                            break;
                        case TaskInstanceStateKind::Skipped:
                            taskStateStr = "skipped";
                            break;
                        case TaskInstanceStateKind::Succeeded:
                            taskStateStr = "succeeded";
                            break;
                        case TaskInstanceStateKind::Failed:
                            taskStateStr = "failed";
                            break;
                        case TaskInstanceStateKind::WaitingExternal:
                            taskStateStr = "waiting_external";
                            break;
                    }

                    taskSummaryJson += "\"" + JsonHelper::EscapeJsonString(taskId) + "\":{\"state\":\"" + taskStateStr + "\"";
                    if (!taskState.m_LastErrorMessage.empty())
                    {
                        taskSummaryJson += ",\"error\":\"" + JsonHelper::EscapeJsonString(taskState.m_LastErrorMessage) + "\"";
                    }

                    // Include task output content for succeeded tasks — only if the
                    // run context didn't opt out via callback_include_outputs=false.
                    if (includeOutputs && taskState.m_State == TaskInstanceStateKind::Succeeded &&
                        !taskState.m_OutputValues.empty())
                    {
                        static constexpr size_t kMaxOutputBytes = 65536;
                        taskSummaryJson += ",\"outputs\":{";
                        bool firstOutput = true;
                        for (auto const& [slotName, slotValue] : taskState.m_OutputValues)
                        {
                            if (!firstOutput)
                            {
                                taskSummaryJson += ",";
                            }
                            firstOutput = false;

                            // Try to read file content if the value looks like a path.
                            std::string content;
                            std::filesystem::path const filePath(slotValue);
                            std::error_code ec;
                            if (std::filesystem::is_regular_file(filePath, ec) && !ec)
                            {
                                std::ifstream ifs(filePath, std::ios::binary);
                                if (ifs.is_open())
                                {
                                    content.resize(kMaxOutputBytes);
                                    ifs.read(content.data(), static_cast<std::streamsize>(kMaxOutputBytes));
                                    content.resize(static_cast<size_t>(ifs.gcount()));
                                }
                            }

                            if (content.empty())
                            {
                                content = slotValue; // Fallback: use the raw value.
                            }

                            taskSummaryJson += "\"" + JsonHelper::EscapeJsonString(slotName) + "\":\"" + JsonHelper::EscapeJsonString(content) + "\"";
                        }
                        taskSummaryJson += "}";
                    }

                    taskSummaryJson += "}";
                }
                taskSummaryJson += "}";
            }

            // Build the full callback payload.
            std::string payload = "{";
            payload += "\"workflowId\":\"" + JsonHelper::EscapeJsonString(workflowId) + "\",";
            payload += "\"runId\":\"" + JsonHelper::EscapeJsonString(runId) + "\",";
            payload += "\"state\":\"" + state + "\",";
            payload += "\"ok\":" + std::string(hasFailed ? "false" : "true") + ",";
            payload += "\"completedAt\":\"" + JsonHelper::EscapeJsonString(completedAt) + "\",";
            payload += "\"tasks\":" + taskSummaryJson;
            payload += "}";

            LOG_APP_INFO("[callback] firing completion callback for run '{}' to '{}'", runId, callbackUrl);

            std::thread callbackThread(
                [callbackUrl, runId, payload = std::move(payload)]()
                {
                    CURL* curl = curl_easy_init();
                    if (curl == nullptr)
                    {
                        LOG_APP_WARN("[callback] curl_easy_init failed for run '{}' callback to '{}'", runId, callbackUrl);
                        return;
                    }

                    struct curl_slist* headers = nullptr;
                    headers = curl_slist_append(headers, "Content-Type: application/json");

                    curl_easy_setopt(curl, CURLOPT_URL, callbackUrl.c_str());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
                    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

                    // TLS hardening + SSRF defense in depth.  IsCallbackUrlAllowed
                    // already gates host range; these options ensure curl itself
                    // cannot silently downgrade or redirect the request to a worse
                    // destination.
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
                    // Restrict to HTTPS — guards against an LD_PRELOAD / curl-config
                    // that defaulted CURLOPT_PROTOCOLS to something broader.  Vendored
                    // curl is 8.x so CURLOPT_PROTOCOLS_STR is always available.
                    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
                    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");

                    CURLcode const res = curl_easy_perform(curl);
                    if (res == CURLE_OK)
                    {
                        long httpCode = 0;
                        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                        LOG_APP_INFO("[callback] completion callback for run '{}' succeeded (URL: '{}', HTTP {})", runId,
                                     callbackUrl, httpCode);
                    }
                    else
                    {
                        LOG_APP_WARN("[callback] completion callback for run '{}' failed (URL: '{}', curl error: {})", runId,
                                     callbackUrl, curl_easy_strerror(res));
                    }

                    curl_slist_free_all(headers);
                    curl_easy_cleanup(curl);
                });
            callbackThread.detach();
        }

        // Returns true if the task was rescheduled for retry (caller should NOT mark run as failed).
        // Returns false if retries are exhausted or the policy has no retries configured.
        bool TryScheduleRetry(TaskInstanceState& taskState, TaskDef const& taskDef, std::string const& taskId,
                              std::string const& runId)
        {
            RetryPolicy const& policy = taskDef.m_RetryPolicy;

            if (policy.m_MaxAttempts == 0)
            {
                return false;
            }

            if (taskState.m_AttemptCount >= policy.m_MaxAttempts)
            {
                LOG_APP_WARN("[retry] task '{}' in run '{}' exhausted all {} retries: {}", taskId, runId,
                             policy.m_MaxAttempts, taskState.m_LastErrorMessage);
                return false;
            }

            uint32_t const backoffMs = policy.m_BackoffMs * (taskState.m_AttemptCount + 1);

            taskState.m_State = TaskInstanceStateKind::Pending;
            taskState.m_RetryAfterTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoffMs);

            LOG_APP_INFO("[retry] task '{}' in run '{}' scheduled for retry (attempt {}/{}, backoff {}ms): {}", taskId,
                         runId, taskState.m_AttemptCount + 1, policy.m_MaxAttempts, backoffMs, taskState.m_LastErrorMessage);

            return true;
        }

        bool IsTaskReady(WorkflowRun const& workflowRun, TaskDef const& taskDefinition)
        {
            for (std::string const& dependencyId : taskDefinition.m_DependsOn)
            {
                auto iterator = workflowRun.m_TaskStates.find(dependencyId);
                if (iterator == workflowRun.m_TaskStates.end())
                {
                    return false;
                }

                TaskInstanceStateKind const dependencyState = iterator->second.m_State;
                if (dependencyState != TaskInstanceStateKind::Succeeded && dependencyState != TaskInstanceStateKind::Skipped)
                {
                    return false;
                }
            }

            return true;
        }

        std::unordered_map<std::string, TaskInstanceState>
        BuildInitialTaskStates(WorkflowDefinition const& workflowDefinition)
        {
            std::unordered_map<std::string, TaskInstanceState> taskStates;
            taskStates.reserve(workflowDefinition.m_Tasks.size());

            for (auto const& taskPair : workflowDefinition.m_Tasks)
            {
                TaskInstanceState state;
                state.m_State = TaskInstanceStateKind::Pending;
                state.m_AttemptCount = 0;
                state.m_LastErrorMessage.clear();
                state.m_InputValues.clear();
                state.m_OutputValues.clear();
                state.m_InputsJson.clear();
                state.m_OutputsJson.clear();

                taskStates.emplace(taskPair.first, std::move(state));
            }

            return taskStates;
        }

    } // namespace

    void WorkflowRuntimeManager::InitializeControlflowRuntime(ActiveRun& activeRun)
    {
        WorkflowDefinition const& workflowDefinition = activeRun.m_Definition;

        activeRun.m_ActivatedTasks.clear();
        activeRun.m_TasksWithIncomingControlflow.clear();
        activeRun.m_HandledFailureTasks.clear();
        activeRun.m_FiredBranches.clear();
        activeRun.m_BranchDrivingTask.clear();
        activeRun.m_BranchNormalTargets.clear();
        activeRun.m_BranchOnErrorTargets.clear();

        if (workflowDefinition.m_ControlNodes.empty() || workflowDefinition.m_ControlflowEdges.empty())
        {
            for (auto const& [taskId, _task] : workflowDefinition.m_Tasks)
            {
                activeRun.m_ActivatedTasks.insert(taskId);
            }
            return;
        }

        std::unordered_set<std::string> branchIds;
        branchIds.reserve(workflowDefinition.m_ControlNodes.size());
        for (auto const& cn : workflowDefinition.m_ControlNodes)
        {
            if (cn.m_Type == ControlNodeType::Branch && !cn.m_Id.empty())
            {
                branchIds.insert(cn.m_Id);
            }
        }

        for (auto const& edge : workflowDefinition.m_ControlflowEdges)
        {
            if (edge.m_From.empty() || edge.m_To.empty())
            {
                continue;
            }

            bool const fromIsBranch = (branchIds.find(edge.m_From) != branchIds.end());
            bool const toIsBranch = (branchIds.find(edge.m_To) != branchIds.end());

            if (edge.m_Kind == ControlflowKind::ErrorSignal)
            {
                if (toIsBranch && !fromIsBranch)
                {
                    activeRun.m_BranchDrivingTask[edge.m_To] = edge.m_From;
                    auto defIt = workflowDefinition.m_Tasks.find(edge.m_From);
                    if (defIt != workflowDefinition.m_Tasks.end() && defIt->second.m_ExposeErrorSignal)
                    {
                        activeRun.m_HandledFailureTasks.insert(edge.m_From);
                    }
                }
                continue;
            }

            if (edge.m_Kind == ControlflowKind::Normal)
            {
                if (toIsBranch && !fromIsBranch)
                {
                    if (activeRun.m_BranchDrivingTask.find(edge.m_To) == activeRun.m_BranchDrivingTask.end())
                    {
                        activeRun.m_BranchDrivingTask[edge.m_To] = edge.m_From;
                    }
                    continue;
                }

                if (fromIsBranch && !toIsBranch)
                {
                    activeRun.m_BranchNormalTargets[edge.m_From].push_back(edge.m_To);
                    activeRun.m_TasksWithIncomingControlflow.insert(edge.m_To);
                }
                continue;
            }

            if (edge.m_Kind == ControlflowKind::OnError)
            {
                if (fromIsBranch && !toIsBranch)
                {
                    activeRun.m_BranchOnErrorTargets[edge.m_From].push_back(edge.m_To);
                    activeRun.m_TasksWithIncomingControlflow.insert(edge.m_To);
                }
                continue;
            }
        }

        for (auto const& [taskId, _task] : workflowDefinition.m_Tasks)
        {
            if (activeRun.m_TasksWithIncomingControlflow.find(taskId) == activeRun.m_TasksWithIncomingControlflow.end())
            {
                activeRun.m_ActivatedTasks.insert(taskId);
            }
        }
    }

    void WorkflowRuntimeManager::SkipAllInstancesOfTask(WorkflowRun& workflowRun, std::string const& taskId,
                                                        std::string const& message)
    {
        for (auto& [instanceId, taskState] : workflowRun.m_TaskStates)
        {
            if (ParentTaskId(instanceId) != taskId)
            {
                continue;
            }

            if (taskState.m_State == TaskInstanceStateKind::Pending || taskState.m_State == TaskInstanceStateKind::Ready)
            {
                taskState.m_State = TaskInstanceStateKind::Skipped;
                taskState.m_LastErrorMessage = message;
                taskState.m_CompletedAtIso8601 = GetIso8601NowUTC();
            }
        }
    }

    void WorkflowRuntimeManager::FireBranchIfReady(ActiveRun& activeRun, std::string const& completedInstanceId,
                                                   TaskInstanceStateKind const completedState)
    {
        WorkflowRun& workflowRun = activeRun.m_Run;

        std::string const drivingParentId = ParentTaskId(completedInstanceId);

        LOG_APP_INFO("[controlflow debug] FireBranchIfReady: completedInstanceId='{}' drivingParentId='{}' "
                     "completedState={} branchDrivingTaskCount={}",
                     completedInstanceId, drivingParentId, static_cast<int>(completedState),
                     activeRun.m_BranchDrivingTask.size());
        for (auto const& [bid, dtid] : activeRun.m_BranchDrivingTask)
        {
            LOG_APP_INFO("[controlflow debug]   branch='{}' drivingTask='{}'", bid, dtid);
        }

        for (auto const& [branchId, drivingTaskId] : activeRun.m_BranchDrivingTask)
        {
            if (drivingTaskId != drivingParentId)
            {
                continue;
            }

            if (activeRun.m_FiredBranches.find(branchId) != activeRun.m_FiredBranches.end())
            {
                continue;
            }

            activeRun.m_FiredBranches.insert(branchId);

            bool const tookError = (completedState == TaskInstanceStateKind::Failed);
            std::vector<std::string> const& selectedTargets =
                tookError ? activeRun.m_BranchOnErrorTargets[branchId] : activeRun.m_BranchNormalTargets[branchId];
            std::vector<std::string> const& unselectedTargets =
                tookError ? activeRun.m_BranchNormalTargets[branchId] : activeRun.m_BranchOnErrorTargets[branchId];

            LOG_APP_INFO("[controlflow debug] firing branch '{}': tookError={} selectedCount={} unselectedCount={}",
                         branchId, tookError, selectedTargets.size(), unselectedTargets.size());

            for (std::string const& targetTaskId : selectedTargets)
            {
                if (!targetTaskId.empty())
                {
                    activeRun.m_ActivatedTasks.insert(targetTaskId);

                    // If a prior branch already skipped this task (e.g. shell_2 was on branch_1's
                    // normal path but is also on branch_2's normal path), reset it to Pending
                    // so the dispatch loop can pick it up.
                    for (auto& [instanceId, taskState] : workflowRun.m_TaskStates)
                    {
                        if (ParentTaskId(instanceId) == targetTaskId && taskState.m_State == TaskInstanceStateKind::Skipped)
                        {
                            taskState.m_State = TaskInstanceStateKind::Pending;
                            taskState.m_LastErrorMessage.clear();
                            taskState.m_CompletedAtIso8601.clear();
                            LOG_APP_INFO("[controlflow debug]   re-enabled previously-skipped task '{}'", instanceId);
                        }
                    }

                    LOG_APP_INFO("[controlflow debug]   activated task '{}'", targetTaskId);
                }
            }

            for (std::string const& targetTaskId : unselectedTargets)
            {
                if (targetTaskId.empty())
                {
                    continue;
                }
                SkipAllInstancesOfTask(workflowRun, targetTaskId, "skipped: branch '" + branchId + "' took other path");
            }
        }
    }

    WorkflowRuntimeManager::~WorkflowRuntimeManager() noexcept
    {
        // Stop() acquires m_Mutex and may interact with the thread pool / AiRequestPool.
        // A destructor that lets exceptions escape calls std::terminate; swallow + log
        // so a teardown failure on one subsystem doesn't take down the whole process.
        try
        {
            Stop();
        }
        catch (std::exception const& exception)
        {
            LOG_APP_ERROR("WorkflowRuntimeManager::~WorkflowRuntimeManager: Stop() threw: {}", exception.what());
        }
        catch (...)
        {
            LOG_APP_ERROR("WorkflowRuntimeManager::~WorkflowRuntimeManager: Stop() threw unknown exception");
        }
    }

    void WorkflowRuntimeManager::SetRegistry(WorkflowRegistry const* workflowRegistry)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_WorkflowRegistry = workflowRegistry;
    }

    void WorkflowRuntimeManager::SetRunTerminalObserver(RunTerminalObserver observer)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_RunTerminalObserver = std::move(observer);
    }

    void WorkflowRuntimeManager::Start()
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        m_IsRunning = true;
    }

    void WorkflowRuntimeManager::Stop()
    {
        SignalStop();
        WaitStop();
    }

    void WorkflowRuntimeManager::SignalStop()
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        m_IsRunning = false;

        std::queue<PendingRun> emptyQueue;
        m_PendingRuns.swap(emptyQueue);

        for (auto& activeRunPtr : m_ActiveRuns)
        {
            activeRunPtr->m_CancelRequested = true;
        }
    }

    void WorkflowRuntimeManager::WaitStop()
    {
        std::vector<std::shared_future<TaskExecutionResult>> taskFutures;
        std::vector<std::shared_future<FilterEvalResult>> filterFutures;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            for (auto& activeRunPtr : m_ActiveRuns)
            {
                ActiveRun& activeRun = *activeRunPtr;
                for (auto& [id, future] : activeRun.m_RunningTasks)
                {
                    taskFutures.push_back(future);
                }
                for (auto& [id, future] : activeRun.m_FilterEvalTasks)
                {
                    filterFutures.push_back(future);
                }
            }
        }

        for (auto& f : taskFutures)
        {
            if (f.valid())
            {
                f.wait_for(std::chrono::milliseconds(500));
            }
        }
        for (auto& f : filterFutures)
        {
            if (f.valid())
            {
                f.wait_for(std::chrono::milliseconds(500));
            }
        }

        // Clean up WaitingExternal tasks before clearing active runs.
        // During shutdown, Update() no longer runs, so these would never be resolved.
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            JarvisAgent* app = App::g_App;
            AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

            for (auto& activeRunPtr : m_ActiveRuns)
            {
                ActiveRun& activeRun = *activeRunPtr;
                for (auto& [taskId, taskState] : activeRun.m_Run.m_TaskStates)
                {
                    if (taskState.m_State != TaskInstanceStateKind::WaitingExternal)
                    {
                        continue;
                    }

                    if (requestPool != nullptr)
                    {
                        AiRequestHandle requestHandle{};
                        requestHandle.requestId = taskState.m_ExternalRequestId;
                        requestHandle.requestTimestampNs = taskState.m_ExternalRequestTimestampNs;

                        if (requestHandle.IsValid())
                        {
                            requestPool->Forget(requestHandle);
                        }
                    }

                    taskState.m_State = TaskInstanceStateKind::Failed;
                    taskState.m_LastErrorMessage = "shutdown: WaitingExternal task aborted";

                    LOG_APP_INFO("[shutdown] failed WaitingExternal task '{}' in run '{}'", taskId, activeRun.m_Run.m_RunId);
                }
            }

            m_ActiveRuns.clear();
            m_DeferredAiCompletions.clear();
        }
    }

    bool WorkflowRuntimeManager::Heartbeat(std::string const& taskInstanceId)
    {
        std::lock_guard<std::mutex> lock(m_WatchdogMutex);
        auto it = m_ActiveWatchdogs.find(taskInstanceId);
        if (it == m_ActiveWatchdogs.end())
        {
            return false;
        }
        it->second->Kick();
        return true;
    }

    void WorkflowRuntimeManager::RegisterWatchdog(std::string const& taskInstanceId,
                                                  std::shared_ptr<TaskWatchdog> const& watchdog)
    {
        std::lock_guard<std::mutex> lock(m_WatchdogMutex);
        m_ActiveWatchdogs[taskInstanceId] = watchdog;
    }

    void WorkflowRuntimeManager::UnregisterWatchdog(std::string const& taskInstanceId)
    {
        std::lock_guard<std::mutex> lock(m_WatchdogMutex);
        m_ActiveWatchdogs.erase(taskInstanceId);
    }

    void WorkflowRuntimeManager::EnqueueWorkflowRun(std::string const& workflowId)
    {
        (void)EnqueueWorkflowRunAndGetRunId(workflowId);
    }

    std::string WorkflowRuntimeManager::EnqueueWorkflowRunAndGetRunId(std::string const& workflowId)
    {
        if (!IsValidRunOrWorkflowId(workflowId))
        {
            LOG_APP_ERROR("WorkflowRuntimeManager::EnqueueWorkflowRunAndGetRunId: rejected invalid workflowId "
                          "(len={}, allowlist [A-Za-z0-9._-], no leading dot)",
                          workflowId.size());
            return std::string();
        }

        WorkflowRegistry const* workflowRegistry = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
        }

        std::string runId;
        if (workflowRegistry != nullptr)
        {
            std::optional<WorkflowDefinition> const workflowDefinition = workflowRegistry->GetWorkflow(workflowId);
            if (workflowDefinition.has_value())
            {
                if (!CheckAiProviderPrerequisites(workflowDefinition.value()))
                {
                    return std::string();
                }
                runId = GenerateRunId(workflowDefinition.value());
            }
        }

        if (runId.empty())
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            runId = workflowId + "_" + std::to_string(millis);
        }

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_PendingRuns.push(PendingRun{workflowId, runId, ContextMap{}});
        }

        return runId;
    }

    std::string WorkflowRuntimeManager::EnqueueWorkflowRunWithContextAndGetRunId(std::string const& workflowId,
                                                                                 std::string const& runId,
                                                                                 ContextMap const& context)
    {
        if (!IsValidRunOrWorkflowId(workflowId))
        {
            LOG_APP_ERROR("WorkflowRuntimeManager::EnqueueWorkflowRunWithContextAndGetRunId: rejected invalid "
                          "workflowId (len={})", workflowId.size());
            return std::string();
        }
        // runId is optional — empty triggers GenerateRunId below — but if
        // supplied it must satisfy the same allowlist.
        if (!runId.empty() && !IsValidRunOrWorkflowId(runId))
        {
            LOG_APP_ERROR("WorkflowRuntimeManager::EnqueueWorkflowRunWithContextAndGetRunId: rejected invalid runId "
                          "(len={}) for workflow '{}'", runId.size(), workflowId);
            return std::string();
        }

        WorkflowRegistry const* workflowRegistry = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
        }

        // Provider prerequisite check (must happen before enqueue).
        if (workflowRegistry != nullptr)
        {
            std::optional<WorkflowDefinition> const workflowDefinition = workflowRegistry->GetWorkflow(workflowId);
            if (workflowDefinition.has_value() && !CheckAiProviderPrerequisites(workflowDefinition.value()))
            {
                return std::string();
            }
        }

        std::string resolvedRunId = runId;
        if (resolvedRunId.empty())
        {
            if (workflowRegistry != nullptr)
            {
                std::optional<WorkflowDefinition> const workflowDefinition = workflowRegistry->GetWorkflow(workflowId);
                if (workflowDefinition.has_value())
                {
                    resolvedRunId = GenerateRunId(workflowDefinition.value());
                }
            }
        }

        if (resolvedRunId.empty())
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            resolvedRunId = workflowId + "_" + std::to_string(millis);
        }

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_PendingRuns.push(PendingRun{workflowId, resolvedRunId, context});
        }

        return resolvedRunId;
    }

    bool WorkflowRuntimeManager::TryGetLastRun(std::string const& workflowId, WorkflowRun& outRun) const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        auto iterator = m_LastRuns.find(workflowId);
        if (iterator == m_LastRuns.end())
        {
            return false;
        }

        outRun = iterator->second;
        return true;
    }

    // PRECONDITION: caller holds m_Mutex.  Calls TryApplyAiCompletion (which mutates
    // m_ActiveRuns task states) and pool->TryPopCompletion (no callback into this
    // manager — safe to hold the lock across).
    void WorkflowRuntimeManager::DrainAiRequestCompletions()
    {
        JarvisAgent* app = App::g_App;
        AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

        if (requestPool == nullptr)
        {
            return;
        }

        if (!m_DeferredAiCompletions.empty())
        {
            std::vector<AiRequestCompletion> stillDeferred;
            stillDeferred.reserve(m_DeferredAiCompletions.size());

            for (AiRequestCompletion const& completion : m_DeferredAiCompletions)
            {
                if (!TryApplyAiCompletion(completion))
                {
                    stillDeferred.push_back(completion);
                }
            }

            m_DeferredAiCompletions = std::move(stillDeferred);
        }

        AiRequestCompletion completion;

        while (requestPool->TryPopCompletion(completion))
        {
            if (!TryApplyAiCompletion(completion))
            {
                if (m_DeferredAiCompletions.size() < 256)
                {
                    m_DeferredAiCompletions.push_back(std::move(completion));
                }
                else
                {
                    LOG_APP_WARN("[WorkflowRuntimeManager] dropping deferred ai_call completion (queue full): wf='{}' "
                                 "run='{}' task='{}'",
                                 completion.m_WorkflowId, completion.m_RunId, completion.m_TaskId);
                }
            }
        }
    }

    // PRECONDITION: caller holds m_Mutex.  Mutates m_ActiveRuns entries (task states),
    // reads m_LastRuns, and may invoke external pool->Forget() calls (no callback into
    // this manager — safe to hold the lock across).
    bool WorkflowRuntimeManager::TryApplyAiCompletion(AiRequestCompletion const& completion)
    {
        for (auto& activeRunPtr : m_ActiveRuns)
        {
            ActiveRun& activeRun = *activeRunPtr;
            if (activeRun.m_Run.m_WorkflowId != completion.m_WorkflowId)
            {
                continue;
            }

            if (activeRun.m_Run.m_RunId != completion.m_RunId)
            {
                continue;
            }

            auto stateIterator = activeRun.m_Run.m_TaskStates.find(completion.m_TaskId);
            if (stateIterator == activeRun.m_Run.m_TaskStates.end())
            {
                return true;
            }

            TaskInstanceState& taskState = stateIterator->second;

            bool retryScheduled = false;

            if (completion.m_WasFailed)
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage =
                    completion.m_ErrorMessage.empty() ? "ai_call failed" : completion.m_ErrorMessage;

                std::string const parentId = ParentTaskId(completion.m_TaskId);
                auto defIt = activeRun.m_Definition.m_Tasks.find(parentId);
                if (defIt != activeRun.m_Definition.m_Tasks.end() &&
                    TryScheduleRetry(taskState, defIt->second, completion.m_TaskId, activeRun.m_Run.m_RunId))
                {
                    retryScheduled = true;
                }
                else
                {
                    activeRun.m_Run.m_HasFailed = true;
                    SkipDownstreamOfFailed(activeRun, completion.m_TaskId);
                }
            }
            else
            {
                taskState.m_State = TaskInstanceStateKind::Succeeded;
                taskState.m_LastErrorMessage.clear();
            }

            // Forget the old request handle before potentially clearing correlation IDs.
            {
                JarvisAgent* app = App::g_App;
                AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

                if (requestPool != nullptr)
                {
                    AiRequestHandle requestHandle{};
                    requestHandle.requestId = taskState.m_ExternalRequestId;
                    requestHandle.requestTimestampNs = taskState.m_ExternalRequestTimestampNs;

                    if (requestHandle.IsValid())
                    {
                        requestPool->Forget(requestHandle);
                    }
                }
            }

            if (retryScheduled)
            {
                // Clear external request correlation so the next attempt registers fresh.
                taskState.m_ExternalRequestId = 0;
                taskState.m_ExternalRequestTimestampNs = 0;
                taskState.m_OutputValues.clear();
                taskState.m_OutputsJson.clear();
            }
            else
            {
                taskState.m_OutputValues = completion.m_OutputValues;

                // Populate m_CapturedStdout from AI response text (enables per_item output piping).
                if (!completion.m_ResponseText.empty())
                {
                    static constexpr size_t kMaxCaptureChars = 1024;
                    taskState.m_CapturedStdout = TruncateUtf8Safe(completion.m_ResponseText, kMaxCaptureChars);
                }

                // Publish ai_call outputs into the run context.
                for (auto const& [outputName, outputValue] : taskState.m_OutputValues)
                {
                    std::string const contextKey = completion.m_TaskId + "." + outputName;
                    activeRun.m_Run.m_Context[contextKey] = ContextValue{outputValue};
                }

                std::string summary;
                for (auto const& p : taskState.m_OutputValues)
                {
                    summary += p.first;
                    summary += "=";
                    summary += p.second;
                    summary += ";";
                }
                taskState.m_OutputsJson = summary;
            }

            return true;
        }

        // Run not in m_ActiveRuns.  If it's in m_LastRuns (already completed/cancelled),
        // drop the completion instead of deferring forever.
        auto const lastIterator = m_LastRuns.find(completion.m_WorkflowId);
        if (lastIterator != m_LastRuns.end() && lastIterator->second.m_RunId == completion.m_RunId)
        {
            return true;
        }

        return false;
    }

    bool WorkflowRuntimeManager::Update()
    {
        bool stateChanged = false;
        std::vector<PendingRun> pendingToStart;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            if (!m_IsRunning)
            {
                return false;
            }

            while (!m_PendingRuns.empty())
            {
                pendingToStart.push_back(std::move(m_PendingRuns.front()));
                m_PendingRuns.pop();
            }
        }

        if (!pendingToStart.empty())
        {
            // StartPendingRuns acquires m_Mutex internally for the per-run push_back;
            // running it BEFORE the tick lock keeps the heavy per-run setup (container
            // extraction, BuildInitialTaskStates, controlflow init) out of the critical
            // section.
            StartPendingRuns(std::move(pendingToStart));
            stateChanged = true;
        }

        // External work deferred from the locked tick body.  Collected under the lock,
        // executed after release: keeps AiRequestPool / curl / callback observers off
        // the m_Mutex hot path, and guarantees no re-entrant lock acquisition if any of
        // those callees ever touch this manager.
        struct PostTickAction
        {
            std::string m_RunIdToCancel;            // empty = no cascade
            std::optional<WorkflowRun> m_TerminalRun; // present = fire callback + observer
        };
        std::vector<PostTickAction> postTickActions;
        RunTerminalObserver observerCopy;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            // Snapshot the observer under the lock.  SetRunTerminalObserver takes the
            // same lock, so swapping the observer mid-tick can't race against the call.
            observerCopy = m_RunTerminalObserver;

            // Fingerprint of task states + the run completion flag. The "before"
            // sample MUST happen before DrainAiRequestCompletions / PropagateSubWorkflowCompletions:
            // those functions mutate task states (AI replies flipping WaitingExternal → Succeeded,
            // sub-workflow completions propagating to parents). Sampling after the drain made
            // every AI completion invisible to the change detector, so the dashboard saw only
            // what TickActiveRun itself mutated and the task counter froze mid-run.
            auto fingerprint = [](ActiveRun const& a) -> uint64_t
            {
                uint64_t h = a.m_Run.m_IsCompleted ? 1ull : 0ull;
                for (auto const& [tid, ts] : a.m_Run.m_TaskStates)
                {
                    h = h * 1315423911ull + static_cast<uint64_t>(ts.m_State);
                    h = h * 2654435761ull + static_cast<uint64_t>(ts.m_AttemptCount);
                }
                return h;
            };

            std::unordered_map<std::string, uint64_t> fingerprintsBefore;
            fingerprintsBefore.reserve(m_ActiveRuns.size());
            for (auto const& runPtr : m_ActiveRuns)
            {
                fingerprintsBefore.emplace(runPtr->m_Run.m_RunId, fingerprint(*runPtr));
            }

            DrainAiRequestCompletions();
            PropagateSubWorkflowCompletions();

            for (size_t index = 0; index < m_ActiveRuns.size();)
            {
                ActiveRun& active = *m_ActiveRuns[index];

                TickActiveRun(active);

                // Compare to the pre-drain fingerprint. New runs added by StartPendingRuns
                // earlier in this Update() won't be in the map; treat that as a state change
                // (it is one — the run just started).
                auto const fpIt = fingerprintsBefore.find(active.m_Run.m_RunId);
                if (fpIt == fingerprintsBefore.end() || fingerprint(active) != fpIt->second)
                {
                    stateChanged = true;
                }

                // Cascade-cancel any in-flight HTTP requests bound to this run if
                // the run just transitioned to a failed/cancelled/stopped state.
                // Idempotent via m_CancelCascadeFired so retries / completion-hold
                // ticks don't re-fire.  Without this, after the workflow runtime
                // gives up on a run (watchdog, downstream failure, user cancel),
                // dispatched curl requests keep running against the AI provider
                // burning tokens for output that gets thrown away.
                bool const runTerminated =
                    active.m_Run.m_HasFailed || active.m_CancelRequested || active.m_StopRequested;
                if (runTerminated && !active.m_CancelCascadeFired)
                {
                    postTickActions.push_back(PostTickAction{active.m_Run.m_RunId, std::nullopt});
                    active.m_CancelCascadeFired = true;
                }

                if (active.m_Run.m_IsCompleted)
                {
                    // 2-second minimum-visibility hold: sub-second runs — common for adhoc
                    // submissions — would otherwise slip through m_ActiveRuns between snapshot
                    // broadcasts and never surface on the dashboard. Keep completed entries in the
                    // active list until at least 2 s have elapsed since the run started, then let
                    // the regular erase path run on a subsequent tick.
                    auto const elapsed = std::chrono::steady_clock::now() - active.m_StartedAtSteady;
                    if (elapsed < std::chrono::seconds(2))
                    {
                        ++index;
                        continue;
                    }

                    // Finalise the overall run state (was left at Pending/Running by TickActiveRun).
                    if (active.m_Run.m_State == WorkflowRunState::Cancelled)
                    {
                        // Keep Cancelled as-is.
                    }
                    else if (active.m_Run.m_State == WorkflowRunState::Stopping ||
                             active.m_Run.m_State == WorkflowRunState::Stopped)
                    {
                        active.m_Run.m_State =
                            active.m_Run.m_HasFailed ? WorkflowRunState::Failed : WorkflowRunState::Stopped;
                    }
                    else
                    {
                        active.m_Run.m_State =
                            active.m_Run.m_HasFailed ? WorkflowRunState::Failed : WorkflowRunState::Succeeded;
                    }

                    // Safety net: ensure completedAt is always set (some paths missed it).
                    if (active.m_Run.m_CompletedAtIso8601.empty())
                    {
                        active.m_Run.m_CompletedAtIso8601 = GetIso8601NowUTC();
                    }

                    // Only update lastRun if this run started later than the existing entry.
                    // Prevents an older stuck/timed-out run from overwriting a newer successful one.
                    {
                        auto const& workflowId = active.m_Run.m_WorkflowId;
                        auto existingIt = m_LastRuns.find(workflowId);
                        if (existingIt == m_LastRuns.end() ||
                            active.m_Run.m_StartedAtIso8601 >= existingIt->second.m_StartedAtIso8601)
                        {
                            m_LastRuns[workflowId] = active.m_Run;
                        }
                    }

                    if (active.m_Run.m_HasFailed)
                    {
                        ++m_TotalFailedRuns;
                    }
                    else
                    {
                        ++m_TotalCompletedRuns;
                    }

                    // Snapshot the WorkflowRun for callback + observer firing AFTER lock release.
                    postTickActions.push_back(PostTickAction{std::string{}, active.m_Run});

                    m_ActiveRuns.erase(m_ActiveRuns.begin() + static_cast<std::ptrdiff_t>(index));
                    stateChanged = true;
                    continue;
                }

                ++index;
            }
        }

        // ---- External calls outside the lock ----
        // m_Mutex is RELEASED here; the actions below may spawn detached threads (callback),
        // call into AiRequestPool (cancel cascade), or invoke a user-supplied observer that
        // could itself touch this manager via the public read APIs.
        if (!postTickActions.empty())
        {
            JarvisAgent* app = App::g_App;
            AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

            for (auto const& action : postTickActions)
            {
                if (!action.m_RunIdToCancel.empty() && requestPool != nullptr)
                {
                    requestPool->CancelRequestsForRun(action.m_RunIdToCancel);
                }

                if (action.m_TerminalRun.has_value())
                {
                    FireCompletionCallback(*action.m_TerminalRun);

                    if (observerCopy)
                    {
                        try
                        {
                            observerCopy(action.m_TerminalRun->m_RunId, action.m_TerminalRun->m_State);
                        }
                        catch (std::exception const& ex)
                        {
                            LOG_APP_ERROR("[runtime] RunTerminalObserver threw: {}", ex.what());
                        }
                        catch (...)
                        {
                            LOG_APP_ERROR("[runtime] RunTerminalObserver threw unknown exception");
                        }
                    }
                }
            }
        }

        return stateChanged;
    }

    void WorkflowRuntimeManager::StartPendingRuns(std::vector<PendingRun>&& pendingRuns)
    {
        WorkflowRegistry const* workflowRegistry = nullptr;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
        }

        if (workflowRegistry == nullptr)
        {
            return;
        }

        for (PendingRun const& pendingRun : pendingRuns)
        {
            if (pendingRun.m_WorkflowId.empty())
            {
                continue;
            }

            std::optional<WorkflowDefinition> workflowDefinition = workflowRegistry->GetWorkflow(pendingRun.m_WorkflowId);
            if (!workflowDefinition.has_value())
            {
                LOG_APP_WARN(
                    "WorkflowRuntimeManager::StartPendingRuns: workflow '{}' not found in registry, skipping run '{}'",
                    pendingRun.m_WorkflowId, pendingRun.m_RunId);
                continue;
            }

            // If this workflow was loaded from a .jcwf container, ensure it's extracted.
            if (!workflowDefinition->m_ContainerPath.empty())
            {
                std::filesystem::path const containerPath(workflowDefinition->m_ContainerPath);
                std::filesystem::path const extractedDir =
                    containerPath.parent_path() / containerPath.stem();

                if (JcwfContainer::IsExtractedStale(containerPath, extractedDir))
                {
                    std::string extractError;
                    if (!JcwfContainer::Extract(containerPath, extractedDir, extractError))
                    {
                        LOG_APP_ERROR("[workflow] failed to extract container '{}' before run: {}",
                                      containerPath.string(), extractError);
                        continue;
                    }
                }
            }

            std::string const runId =
                pendingRun.m_RunId.empty() ? GenerateRunId(workflowDefinition.value()) : pendingRun.m_RunId;

            ActiveRun activeRun;
            activeRun.m_Definition = workflowDefinition.value();
            activeRun.m_Run.m_RunId = runId;
            activeRun.m_Run.m_WorkflowId = pendingRun.m_WorkflowId;
            activeRun.m_Run.m_Context = pendingRun.m_Context;
            activeRun.m_Run.m_State = WorkflowRunState::Running;
            activeRun.m_Run.m_StartedAtIso8601 = GetIso8601NowUTC();
            activeRun.m_StartedAtSteady = std::chrono::steady_clock::now();
            activeRun.m_Run.m_CancellationToken = std::make_shared<TaskCancellationToken>();
            activeRun.m_Run.m_TaskStates = BuildInitialTaskStates(activeRun.m_Definition);

            InitializeControlflowRuntime(activeRun);

            LOG_APP_INFO("[workflow] run '{}' started (workflow '{}')", runId, pendingRun.m_WorkflowId);

            {
                std::scoped_lock<std::mutex> const lock(m_Mutex);
                m_ActiveRuns.push_back(std::make_unique<ActiveRun>(std::move(activeRun)));
            }
        }
    }

    void WorkflowRuntimeManager::TickActiveRun(ActiveRun& activeRun)
    {
        WorkflowDefinition const& workflowDefinition = activeRun.m_Definition;
        WorkflowRun& workflowRun = activeRun.m_Run;

        // The caller holds completed runs in m_ActiveRuns for a 2 s minimum-visibility
        // window. Re-entering the full tick (harvest → terminal-check → log) during
        // that hold fired the completion log every frame — ~120× per run.
        if (workflowRun.m_IsCompleted)
        {
            return;
        }

        // ---------------------------------------------------------
        // 0) Harvest filter evaluation completions + aggregate per-item results
        // ---------------------------------------------------------
        HarvestFilterEvalCompletions(activeRun);
        AggregatePerItemResults(activeRun);

        // ---------------------------------------------------------
        // 1) Harvest completed worker tasks (non-blocking)
        // ---------------------------------------------------------
        for (auto iterator = activeRun.m_RunningTasks.begin(); iterator != activeRun.m_RunningTasks.end();)
        {
            std::shared_future<TaskExecutionResult>& future = iterator->second;

            if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                ++iterator;
                continue;
            }

            TaskExecutionResult result;
            bool gotResult = false;

            try
            {
                result = future.get();
                gotResult = true;
            }
            catch (...)
            {
                gotResult = false;
            }

            if (!gotResult)
            {
                auto stateIterator = workflowRun.m_TaskStates.find(iterator->first);
                if (stateIterator != workflowRun.m_TaskStates.end())
                {
                    stateIterator->second.m_State = TaskInstanceStateKind::Failed;
                    stateIterator->second.m_LastErrorMessage = "task future threw";

                    std::string const parentId = ParentTaskId(iterator->first);
                    auto defIt = workflowDefinition.m_Tasks.find(parentId);
                    if (defIt == workflowDefinition.m_Tasks.end() ||
                        !TryScheduleRetry(stateIterator->second, defIt->second, iterator->first, workflowRun.m_RunId))
                    {
                        std::string const handledParentId = ParentTaskId(iterator->first);
                        bool const handled =
                            (activeRun.m_HandledFailureTasks.find(handledParentId) != activeRun.m_HandledFailureTasks.end());
                        if (!handled)
                        {
                            workflowRun.m_HasFailed = true;
                            SkipDownstreamOfFailed(activeRun, iterator->first);
                        }
                        FireBranchIfReady(activeRun, iterator->first, TaskInstanceStateKind::Failed);
                    }
                }
                else
                {
                    workflowRun.m_HasFailed = true;
                }
            }
            else
            {
                auto stateIterator = workflowRun.m_TaskStates.find(result.m_TaskId);
                if (stateIterator != workflowRun.m_TaskStates.end())
                {
                    // Guard against overwriting a terminal state that was already set
                    // by DrainAiRequestCompletions (race: completion arrived before
                    // the worker future was harvested).
                    auto const currentState = stateIterator->second.m_State;
                    if (currentState != TaskInstanceStateKind::Succeeded && currentState != TaskInstanceStateKind::Failed)
                    {
                        stateIterator->second = result.m_TaskState;

                        if (result.m_TaskState.m_State == TaskInstanceStateKind::WaitingExternal &&
                            stateIterator->second.m_WaitingExternalSince == std::chrono::steady_clock::time_point{})
                        {
                            stateIterator->second.m_WaitingExternalSince = std::chrono::steady_clock::now();
                        }
                    }
                }

                // Publish successful task outputs into the run context so downstream
                // tasks can resolve them via context lookup (without explicit dataflow wiring).
                if (result.m_ExecuteOk && stateIterator != workflowRun.m_TaskStates.end())
                {
                    for (auto const& [outputName, outputValue] : result.m_TaskState.m_OutputValues)
                    {
                        std::string const contextKey = result.m_TaskId + "." + outputName;
                        workflowRun.m_Context[contextKey] = ContextValue{outputValue};
                    }
                }

                if (!result.m_ExecuteOk)
                {
                    LOG_APP_ERROR("[workflow] task '{}' failed in run '{}': {}", result.m_TaskId, workflowRun.m_RunId,
                                  result.m_TaskState.m_LastErrorMessage);

                    std::string const parentId = ParentTaskId(result.m_TaskId);
                    auto defIt = workflowDefinition.m_Tasks.find(parentId);
                    if (stateIterator != workflowRun.m_TaskStates.end() && defIt != workflowDefinition.m_Tasks.end() &&
                        TryScheduleRetry(stateIterator->second, defIt->second, result.m_TaskId, workflowRun.m_RunId))
                    {
                        // Retry scheduled — do not fail the run.
                        LOG_APP_INFO("[controlflow debug] task '{}' retry scheduled", result.m_TaskId);
                    }
                    else
                    {
                        std::string const handledParentId = ParentTaskId(result.m_TaskId);
                        bool const handled =
                            (activeRun.m_HandledFailureTasks.find(handledParentId) != activeRun.m_HandledFailureTasks.end());
                        LOG_APP_INFO("[controlflow debug] task '{}' failed, parentId='{}' handled={} "
                                     "handledFailureTasksCount={} branchDrivingTaskCount={}",
                                     result.m_TaskId, handledParentId, handled, activeRun.m_HandledFailureTasks.size(),
                                     activeRun.m_BranchDrivingTask.size());
                        if (!handled)
                        {
                            workflowRun.m_HasFailed = true;
                            SkipDownstreamOfFailed(activeRun, result.m_TaskId);
                        }
                        FireBranchIfReady(activeRun, result.m_TaskId, TaskInstanceStateKind::Failed);
                    }
                }
                else
                {
                    // Successful completion may fire a branch's normal path.
                    FireBranchIfReady(activeRun, result.m_TaskId,
                                      stateIterator != workflowRun.m_TaskStates.end() ? stateIterator->second.m_State
                                                                                      : TaskInstanceStateKind::Succeeded);
                }
            }

            iterator = activeRun.m_RunningTasks.erase(iterator);
        }

        if (workflowRun.m_HasFailed && activeRun.m_RunningTasks.empty() && activeRun.m_FilterEvalTasks.empty())
        {
            // Ensure WaitingExternal tasks don't linger forever once the run is failed.
            JarvisAgent* app = App::g_App;
            AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

            for (auto& taskPair : workflowRun.m_TaskStates)
            {
                TaskInstanceState& taskState = taskPair.second;

                if (taskState.m_State != TaskInstanceStateKind::WaitingExternal)
                {
                    continue;
                }

                AiRequestHandle requestHandle{};
                requestHandle.requestId = taskState.m_ExternalRequestId;
                requestHandle.requestTimestampNs = taskState.m_ExternalRequestTimestampNs;

                if (requestPool != nullptr && requestHandle.IsValid())
                {
                    requestPool->Forget(requestHandle);
                }

                taskState.m_State = TaskInstanceStateKind::Failed;
                if (taskState.m_LastErrorMessage.empty())
                {
                    taskState.m_LastErrorMessage = "workflow failed while waiting for external completion";
                }
            }

            LOG_APP_ERROR("[workflow] run '{}' failed (workflow '{}')", workflowRun.m_RunId, workflowRun.m_WorkflowId);
            workflowRun.m_IsCompleted = true;
            return;
        }

        // ---------------------------------------------------------
        // Cancellation gate (best-effort, cooperative)
        // ---------------------------------------------------------
        if (activeRun.m_CancelRequested)
        {
            // Do not dispatch new work. Once all running tasks finish, mark the run cancelled.
            if (activeRun.m_RunningTasks.empty())
            {
                for (auto& taskPair : workflowRun.m_TaskStates)
                {
                    TaskInstanceState& taskState = taskPair.second;
                    if (taskState.m_State == TaskInstanceStateKind::Pending ||
                        taskState.m_State == TaskInstanceStateKind::Ready ||
                        taskState.m_State == TaskInstanceStateKind::WaitingExternal)
                    {
                        taskState.m_State = TaskInstanceStateKind::Skipped;
                        if (taskState.m_LastErrorMessage.empty())
                        {
                            taskState.m_LastErrorMessage = "cancelled";
                        }
                    }
                }

                workflowRun.m_State = WorkflowRunState::Cancelled;
                workflowRun.m_CompletedAtIso8601 = GetIso8601NowUTC();
                workflowRun.m_IsCompleted = true;
                LOG_APP_INFO("[workflow] run '{}' cancelled (workflow '{}')", workflowRun.m_RunId, workflowRun.m_WorkflowId);
                return;
            }
        }

        // ---------------------------------------------------------
        // Pause gate: skip dispatch entirely while paused.
        // In-flight tasks continue to run and are harvested above.
        // ---------------------------------------------------------
        if (activeRun.m_PauseRequested)
        {
            return;
        }

        // ---------------------------------------------------------
        // Stop gate: let in-flight tasks finish, then complete the run.
        // No new tasks are dispatched.
        // ---------------------------------------------------------
        if (activeRun.m_StopRequested)
        {
            if (activeRun.m_RunningTasks.empty() && activeRun.m_FilterEvalTasks.empty())
            {
                for (auto& taskPair : workflowRun.m_TaskStates)
                {
                    TaskInstanceState& taskState = taskPair.second;
                    if (taskState.m_State == TaskInstanceStateKind::Pending ||
                        taskState.m_State == TaskInstanceStateKind::Ready)
                    {
                        taskState.m_State = TaskInstanceStateKind::Skipped;
                        if (taskState.m_LastErrorMessage.empty())
                        {
                            taskState.m_LastErrorMessage = "stopped";
                        }
                    }
                }

                workflowRun.m_State = workflowRun.m_HasFailed ? WorkflowRunState::Failed : WorkflowRunState::Stopped;
                workflowRun.m_CompletedAtIso8601 = GetIso8601NowUTC();
                workflowRun.m_IsCompleted = true;
                LOG_APP_INFO("[workflow] run '{}' stopped (workflow '{}')", workflowRun.m_RunId, workflowRun.m_WorkflowId);
                return;
            }

            // Still have in-flight tasks — don't dispatch new ones, just return.
            return;
        }

        // Re-aggregate per-item results after harvesting worker completions.
        // Children that were dispatched and completed within the same tick
        // (e.g. fast Python tasks) are only in terminal state after harvest,
        // so the parent must be checked again before the dispatch phase.
        AggregatePerItemResults(activeRun);

        // ---------------------------------------------------------
        // 2) Dispatch newly-ready tasks (no waiting)
        // ---------------------------------------------------------
        if (Core::g_Core == nullptr)
        {
            workflowRun.m_HasFailed = true;
            workflowRun.m_IsCompleted = true;
            LOG_APP_ERROR("[workflow] run '{}' failed (workflow '{}') -- core unavailable", workflowRun.m_RunId,
                          workflowRun.m_WorkflowId);
            return;
        }

        ThreadPool& pool = Core::g_Core->GetThreadPool();

        bool dispatchedAny = false;

        for (auto& taskPair : workflowRun.m_TaskStates)
        {
            std::string const& taskId = taskPair.first;
            TaskInstanceState& taskState = taskPair.second;

            if (taskState.m_State != TaskInstanceStateKind::Pending && taskState.m_State != TaskInstanceStateKind::Ready)
            {
                continue;
            }

            // Respect retry backoff: skip if the retry-after time hasn't arrived yet.
            if (taskState.m_RetryAfterTime != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() < taskState.m_RetryAfterTime)
            {
                continue;
            }

            // For child instances (taskId#k), look up the parent's TaskDef
            std::string const parentId = ParentTaskId(taskId);
            bool const isChild = IsChildInstance(taskId);

            auto defIterator = workflowDefinition.m_Tasks.find(parentId);
            if (defIterator == workflowDefinition.m_Tasks.end())
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "task missing from workflow definition";
                workflowRun.m_HasFailed = true;
                continue;
            }

            TaskDef const& taskDefinition = defIterator->second;

            // Controlflow gate: tasks with incoming controlflow only run if activated by a branch.
            // Child instances are managed by the parent and bypass this gate.
            if (!isChild)
            {
                if (activeRun.m_TasksWithIncomingControlflow.find(parentId) !=
                    activeRun.m_TasksWithIncomingControlflow.end())
                {
                    if (activeRun.m_ActivatedTasks.find(parentId) == activeRun.m_ActivatedTasks.end())
                    {
                        continue;
                    }
                }
            }

            // Child instances skip DAG readiness (parent manages them)
            if (!isChild && !IsTaskReady(workflowRun, taskDefinition))
            {
                continue;
            }

            // Per-item parent tasks: dispatch filter evaluation, not normal execution
            if (!isChild && taskDefinition.m_Mode == TaskMode::PerItem && !taskDefinition.m_Filter.empty())
            {
                if (activeRun.m_FilterEvalTasks.find(taskId) == activeRun.m_FilterEvalTasks.end() &&
                    activeRun.m_PerItemChildren.find(taskId) == activeRun.m_PerItemChildren.end())
                {
                    DispatchFilterEvaluation(activeRun, taskId, taskDefinition);
                    dispatchedAny = true;
                }
                continue;
            }

            // Freshness check + skip (not for child instances — already handled during fan-out).
            // Tasks gated by controlflow bypass freshness: the branch decision is authoritative.
            bool const isControlflowGated =
                (activeRun.m_TasksWithIncomingControlflow.find(parentId) != activeRun.m_TasksWithIncomingControlflow.end());
            if (!isChild && !isControlflowGated)
            {
                TaskFreshnessChecker freshnessChecker;
                TaskFreshnessChecker::ResolvedPaths resolvedPaths;

                if (TaskPathResolver::ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                                   resolvedPaths.m_InputPaths, resolvedPaths.m_OutputPaths))
                {
                    LOG_APP_INFO("[paths debug] debug reason=resolveFreshnessPaths workflowId='{}' runId='{}' taskId='{}' "
                                 "inputCount={} outputCount={}",
                                 workflowRun.m_WorkflowId, workflowRun.m_RunId, taskId,
                                 static_cast<int>(resolvedPaths.m_InputPaths.size()),
                                 static_cast<int>(resolvedPaths.m_OutputPaths.size()));

                    for (fs::path const& inputPath : resolvedPaths.m_InputPaths)
                    {
                        LOG_APP_INFO("[paths debug] debug reason=resolveFreshnessPathsInput workflowId='{}' runId='{}' "
                                     "taskId='{}' inputPathAbsolute='{}'",
                                     workflowRun.m_WorkflowId, workflowRun.m_RunId, taskId, inputPath.string());
                    }

                    for (fs::path const& outputPath : resolvedPaths.m_OutputPaths)
                    {
                        LOG_APP_INFO("[paths debug] debug reason=resolveFreshnessPathsOutput workflowId='{}' runId='{}' "
                                     "taskId='{}' outputPathAbsolute='{}'",
                                     workflowRun.m_WorkflowId, workflowRun.m_RunId, taskId, outputPath.string());
                    }
                    auto resolveUpstreamOutputs = [&](std::string const& upstreamTaskId,
                                                      std::vector<fs::path>& outPaths) -> bool
                    {
                        auto upstreamIt = workflowDefinition.m_Tasks.find(upstreamTaskId);
                        if (upstreamIt == workflowDefinition.m_Tasks.end())
                        {
                            return false;
                        }

                        std::vector<fs::path> unusedInputs;
                        std::vector<fs::path> outputPaths;

                        if (!TaskPathResolver::ResolveFreshnessPathsForTask(workflowDefinition, workflowRun,
                                                                            upstreamIt->second, upstreamTaskId, unusedInputs,
                                                                            outputPaths))
                        {
                            return false;
                        }

                        outPaths = outputPaths;
                        return true;
                    };

                    if (freshnessChecker.IsTaskUpToDate(workflowDefinition, taskId, resolvedPaths, resolveUpstreamOutputs))
                    {
                        PopulateSkippedTaskOutputsIfPossible(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                             taskState);
                        taskState.m_State = TaskInstanceStateKind::Skipped;
                        LOG_APP_INFO("[paths debug] debug reason=freshnessSkip workflowId='{}' runId='{}' taskId='{}'",
                                     workflowRun.m_WorkflowId, workflowRun.m_RunId, taskId);
                        dispatchedAny = true;
                        continue;
                    }
                }
            }

            if (activeRun.m_RunningTasks.find(taskId) != activeRun.m_RunningTasks.end())
            {
                continue;
            }

            // Per-run AI call cap — "max_ai_calls_per_jcwf" (0 = no cap). Guards against
            // a single runaway JCWF (especially adhoc) consuming the entire AI budget.
            if (taskDefinition.m_Type == TaskType::AiCall && Core::g_Core != nullptr)
            {
                size_t const cap = Core::g_Core->GetConfig().m_MaxAiCallsPerJcwf;
                if (cap > 0 && activeRun.m_AiCallsDispatched >= cap)
                {
                    std::string const msg = "AI call cap exceeded (max_ai_calls_per_jcwf=" +
                                            std::to_string(cap) + ")";
                    LOG_APP_WARN("[runtime] {} — failing task '{}' in run '{}'",
                                 msg, taskId, workflowRun.m_RunId);
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    taskState.m_LastErrorMessage = msg;
                    dispatchedAny = true;
                    continue;
                }
                ++activeRun.m_AiCallsDispatched;
            }

            taskState.m_State = TaskInstanceStateKind::Running;

            WorkflowRun const workflowRunSnapshot = workflowRun;
            TaskInstanceState const taskStateSnapshot = taskState;

            // Capture by value, not by reference.  WaitStop()'s shutdown path may clear
            // m_ActiveRuns before all in-flight worker futures have completed; a captured
            // reference into ActiveRun::m_Definition would then dangle.  WorkflowDefinition
            // is heavyweight but the copy is per-dispatch (not per-tick) — never reach into
            // caller-stack data from an async work site.
            activeRun.m_RunningTasks[taskId] =
                pool.SubmitTask(
                        [this, workflowDefinitionCopy = workflowDefinition, workflowRunSnapshot,
                         taskDefinition, taskId, taskStateSnapshot]() -> TaskExecutionResult {
                            return ExecuteTaskOnWorker(workflowDefinitionCopy, workflowRunSnapshot, taskDefinition,
                                                       taskId, taskStateSnapshot);
                        })
                    .share();

            dispatchedAny = true;
        }

        // If no work is in-flight, mark non-activated controlflow-gated tasks as skipped so the run can terminate.
        // WaitingExternal tasks (e.g. ai_call awaiting AI response) are still active but not tracked in
        // m_RunningTasks, so we must also check for them before pruning controlflow-gated tasks.
        auto anyWaitingExternal = [&]()
        {
            for (auto const& [id, ts] : workflowRun.m_TaskStates)
            {
                if (ts.m_State == TaskInstanceStateKind::WaitingExternal)
                    return true;
            }
            return false;
        };
        if (activeRun.m_RunningTasks.empty() && activeRun.m_FilterEvalTasks.empty() && !anyWaitingExternal())
        {
            for (auto& [instanceId, taskState] : workflowRun.m_TaskStates)
            {
                if (taskState.m_State != TaskInstanceStateKind::Pending && taskState.m_State != TaskInstanceStateKind::Ready)
                {
                    continue;
                }

                std::string const parentId = ParentTaskId(instanceId);
                if (activeRun.m_TasksWithIncomingControlflow.find(parentId) ==
                    activeRun.m_TasksWithIncomingControlflow.end())
                {
                    continue;
                }

                if (activeRun.m_ActivatedTasks.find(parentId) != activeRun.m_ActivatedTasks.end())
                {
                    continue;
                }

                taskState.m_State = TaskInstanceStateKind::Skipped;
                if (taskState.m_LastErrorMessage.empty())
                {
                    taskState.m_LastErrorMessage = "skipped: controlflow not activated";
                }
                taskState.m_CompletedAtIso8601 = GetIso8601NowUTC();
                dispatchedAny = true;
            }
        }

        // ---------------------------------------------------------
        // 3) Completion / deadlock detection
        // ---------------------------------------------------------
        if (IsRunTerminal(activeRun))
        {
            // Rule A: terminal failure is based on the presence of unhandled failed tasks.
            bool hasUnhandledFailures = false;
            for (auto const& [instanceId, taskState] : workflowRun.m_TaskStates)
            {
                if (taskState.m_State != TaskInstanceStateKind::Failed)
                {
                    continue;
                }

                std::string const parentId = ParentTaskId(instanceId);
                if (activeRun.m_HandledFailureTasks.find(parentId) == activeRun.m_HandledFailureTasks.end())
                {
                    hasUnhandledFailures = true;
                    break;
                }
            }
            workflowRun.m_HasFailed = hasUnhandledFailures;

            if (workflowRun.m_HasFailed)
            {
                LOG_APP_ERROR("[workflow] run '{}' failed (workflow '{}')", workflowRun.m_RunId, workflowRun.m_WorkflowId);
            }
            else
            {
                LOG_APP_INFO("[workflow] run '{}' completed (workflow '{}')", workflowRun.m_RunId, workflowRun.m_WorkflowId);
            }
            workflowRun.m_IsCompleted = true;
            return;
        }

        // Safety-net: fail WaitingExternal tasks that exceeded their timeout.
        // This runs every tick, before the deadlock detector, so timed-out tasks
        // don't mask a real deadlock by keeping hasWaitingExternal == true.
        TimeoutWaitingExternalTasks(activeRun);

        if (!dispatchedAny && activeRun.m_RunningTasks.empty() && activeRun.m_FilterEvalTasks.empty())
        {
            bool hasWaitingExternal = false;
            bool hasPendingOrReady = false;
            bool hasRetryPending = false;

            for (auto const& taskPair : workflowRun.m_TaskStates)
            {
                TaskInstanceStateKind const state = taskPair.second.m_State;

                if (state == TaskInstanceStateKind::WaitingExternal)
                {
                    hasWaitingExternal = true;
                }
                else if (state == TaskInstanceStateKind::Pending || state == TaskInstanceStateKind::Ready)
                {
                    hasPendingOrReady = true;

                    // A task waiting for retry backoff is not a deadlock.
                    if (taskPair.second.m_RetryAfterTime != std::chrono::steady_clock::time_point{})
                    {
                        hasRetryPending = true;
                    }
                }
            }

            // WaitingExternal means we are legitimately waiting for filesystem-driven completion.
            // Retry-pending tasks are also legitimately waiting (for their backoff timer).
            if (!hasWaitingExternal && !hasRetryPending && hasPendingOrReady)
            {
                LOG_APP_CRITICAL("[workflow] run '{}' failed (workflow '{}') -- deadlock/cycle detected",
                                 workflowRun.m_RunId, workflowRun.m_WorkflowId);
                workflowRun.m_HasFailed = true;
                workflowRun.m_IsCompleted = true;
            }
        }
    }

    bool WorkflowRuntimeManager::IsRunTerminal(ActiveRun const& activeRun) const
    {
        for (auto const& taskPair : activeRun.m_Run.m_TaskStates)
        {
            if (!IsTerminal(taskPair.second.m_State))
            {
                return false;
            }
        }

        return true;
    }

    void WorkflowRuntimeManager::SkipDownstreamOfFailed(ActiveRun& activeRun, std::string const& failedTaskId)
    {
        WorkflowRun& workflowRun = activeRun.m_Run;
        WorkflowDefinition const& workflowDefinition = activeRun.m_Definition;

        // BFS: collect all tasks that transitively depend on the failed task.
        std::vector<std::string> queue;
        queue.push_back(failedTaskId);

        while (!queue.empty())
        {
            std::string const current = std::move(queue.back());
            queue.pop_back();

            // Find every task whose depends_on references `current` (by parent id).
            for (auto const& [taskId, taskDef] : workflowDefinition.m_Tasks)
            {
                for (std::string const& dep : taskDef.m_DependsOn)
                {
                    if (dep != ParentTaskId(current))
                    {
                        continue;
                    }

                    // Skip all instances of this task (single-mode: taskId, per-item: taskId#k).
                    for (auto& [instanceId, taskState] : workflowRun.m_TaskStates)
                    {
                        if (ParentTaskId(instanceId) != taskId)
                        {
                            continue;
                        }

                        if (taskState.m_State == TaskInstanceStateKind::Pending ||
                            taskState.m_State == TaskInstanceStateKind::Ready)
                        {
                            taskState.m_State = TaskInstanceStateKind::Skipped;
                            taskState.m_LastErrorMessage = "skipped: upstream task '" + failedTaskId + "' failed";
                            taskState.m_CompletedAtIso8601 = GetIso8601NowUTC();
                            queue.push_back(instanceId);
                            LOG_APP_INFO("[workflow] skipping '{}' in run '{}': upstream '{}' failed", instanceId,
                                         workflowRun.m_RunId, failedTaskId);
                        }
                    }
                }
            }
        }
    }

    void WorkflowRuntimeManager::TimeoutWaitingExternalTasks(ActiveRun& activeRun)
    {
        // Default timeout for non-ai_call WaitingExternal tasks (sub_workflow, etc.).
        // ai_call tasks are NOT subject to this safety net — their per-attempt timeout
        // is owned by curl (CURLOPT_TIMEOUT_MS, set from the per-interface size-aware
        // budget computed in AiRequestPool::Submit).  The runtime-level kill that used
        // to fire here at 5 min wall-clock killed legitimately-slow Sonnet/Opus calls
        // even when AiRequestPool's deadline was correctly extended past 5 min.
        static constexpr uint64_t kDefaultWaitingExternalTimeoutMs = 300000; // 5 minutes

        WorkflowRun& workflowRun = activeRun.m_Run;
        WorkflowDefinition const& workflowDefinition = activeRun.m_Definition;
        auto const now = std::chrono::steady_clock::now();

        JarvisAgent* app = App::g_App;
        AiRequestPool* requestPool = (app != nullptr) ? app->GetAiRequestPool() : nullptr;

        for (auto& [taskId, taskState] : workflowRun.m_TaskStates)
        {
            if (taskState.m_State != TaskInstanceStateKind::WaitingExternal)
            {
                continue;
            }

            if (taskState.m_WaitingExternalSince == std::chrono::steady_clock::time_point{})
            {
                continue;
            }

            // Skip ai_call tasks — curl owns their timeout via CURLOPT_TIMEOUT_MS.
            std::string const parentId = ParentTaskId(taskId);
            auto defIt = workflowDefinition.m_Tasks.find(parentId);
            if (defIt != workflowDefinition.m_Tasks.end() && defIt->second.m_Type == TaskType::AiCall)
            {
                continue;
            }

            // Other task types (sub_workflow, etc.): use the per-task explicit
            // timeout if set, else the 5-minute default.
            uint64_t const timeoutMs = (defIt != workflowDefinition.m_Tasks.end() && defIt->second.m_TimeoutMs > 0)
                                           ? defIt->second.m_TimeoutMs
                                           : kDefaultWaitingExternalTimeoutMs;

            auto const elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - taskState.m_WaitingExternalSince).count();

            if (static_cast<uint64_t>(elapsed) < timeoutMs)
            {
                continue;
            }

            LOG_APP_WARN("[workflow] WaitingExternal timeout for task '{}' in run '{}' ({}ms elapsed, limit {}ms)", taskId,
                         workflowRun.m_RunId, elapsed, timeoutMs);

            // Forget the AI request so the pool doesn't keep waiting.
            if (requestPool != nullptr)
            {
                AiRequestHandle requestHandle{};
                requestHandle.requestId = taskState.m_ExternalRequestId;
                requestHandle.requestTimestampNs = taskState.m_ExternalRequestTimestampNs;

                if (requestHandle.IsValid())
                {
                    requestPool->Forget(requestHandle);
                }
            }

            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "WaitingExternal timed out after " + std::to_string(elapsed) + "ms";
            taskState.m_CompletedAtIso8601 = GetIso8601NowUTC();

            workflowRun.m_HasFailed = true;

            // Propagate failure to downstream tasks.
            SkipDownstreamOfFailed(activeRun, taskId);
        }
    }

    WorkflowRuntimeManager::TaskExecutionResult
    WorkflowRuntimeManager::ExecuteTaskOnWorker(WorkflowDefinition const& workflowDefinition,
                                                WorkflowRun const& workflowRunSnapshot, TaskDef const& taskDefinition,
                                                std::string const& taskId, TaskInstanceState const& taskStateSnapshot) const
    {
        TaskExecutionResult result;
        result.m_TaskId = taskId;
        result.m_TaskState = taskStateSnapshot;

        LOG_APP_INFO("[paths debug] debug reason=dispatchTask workflowId='{}' runId='{}' taskId='{}'",
                     workflowRunSnapshot.m_WorkflowId, workflowRunSnapshot.m_RunId, taskId);

        result.m_TaskState.m_State = TaskInstanceStateKind::Running;
        result.m_TaskState.m_AttemptCount = taskStateSnapshot.m_AttemptCount + 1;

        WorkflowRun workerRun = workflowRunSnapshot;

        DataflowResolver dataflowResolver;

        std::optional<TaskResolvedInputs> optionalResolvedInputs =
            dataflowResolver.ResolveInputsForTask(workflowDefinition, workerRun, taskDefinition, taskId);

        if (!optionalResolvedInputs.has_value())
        {
            result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
            result.m_TaskState.m_LastErrorMessage = "Failed to resolve task inputs via dataflow / context";
            result.m_ExecuteOk = false;
            return result;
        }

        TaskResolvedInputs const& resolvedInputs = optionalResolvedInputs.value();

        if (!resolvedInputs.m_ErrorMessage.empty())
        {
            result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
            result.m_TaskState.m_LastErrorMessage = resolvedInputs.m_ErrorMessage;
            result.m_ExecuteOk = false;
            return result;
        }

        // Merge dataflow-resolved inputs into existing values (preserves per_item filter bindings)
        for (auto const& [key, value] : resolvedInputs.m_StringValues)
        {
            result.m_TaskState.m_InputValues[key] = value;
        }

        // Inject upstream task outputs for non-per-item chains.  Per-item children already
        // receive this injection at dispatch time (see per-item loop), so we only need to
        // handle regular task instances here.  The injection is idempotent — existing entries
        // from the per-item path are simply overwritten with the same values.
        if (taskId == taskDefinition.m_Id) // not a per-item child ("parentId#N")
        {
            for (std::string const& depId : taskDefinition.m_DependsOn)
            {
                auto upstreamStateIt = workerRun.m_TaskStates.find(depId);
                if (upstreamStateIt == workerRun.m_TaskStates.end())
                {
                    continue;
                }

                auto upstreamDefIt = workflowDefinition.m_Tasks.find(depId);
                if (upstreamDefIt == workflowDefinition.m_Tasks.end())
                {
                    continue;
                }

                InjectUpstreamOutputs(workflowDefinition, upstreamDefIt->second, upstreamStateIt->second, depId,
                                      depId, result.m_TaskState.m_InputValues);
            }
        }

        {
            std::string summary;
            for (auto const& p : resolvedInputs.m_StringValues)
            {
                summary += p.first;
                summary += "=";
                summary += p.second;
                summary += ";";
            }
            result.m_TaskState.m_InputsJson = summary;
        }

        TaskExecutorRegistry& executorRegistry = TaskExecutorRegistry::Get();

        // Propagate the actual task instance ID so executors can use it for request pool binding.
        // For per_item children this is e.g. "lookupDividend#0"; for single tasks it equals taskDefinition.m_Id.
        result.m_TaskState.m_TaskInstanceId = taskId;

        LOG_APP_INFO("[paths debug] debug reason=executeTask workflowId='{}' runId='{}' taskId='{}'", workerRun.m_WorkflowId,
                     workerRun.m_RunId, taskId);

        // Create inactivity watchdog for tasks with timeout_ms (excluding ai_call which has its own).
        std::shared_ptr<TaskWatchdog> watchdog;
        if (taskDefinition.m_TimeoutMs > 0 && taskDefinition.m_Type != TaskType::AiCall)
        {
            watchdog = std::make_shared<TaskWatchdog>();
            watchdog->Kick(); // initial heartbeat = now
            result.m_TaskState.m_Watchdog = watchdog;
            const_cast<WorkflowRuntimeManager*>(this)->RegisterWatchdog(taskId, watchdog);
        }

        bool const executedOk = executorRegistry.Execute(workflowDefinition, workerRun, taskDefinition, result.m_TaskState);

        // Unregister watchdog before returning.
        if (watchdog)
        {
            const_cast<WorkflowRuntimeManager*>(this)->UnregisterWatchdog(taskId);
        }

        if (!executedOk)
        {
            if (result.m_TaskState.m_State != TaskInstanceStateKind::Failed)
            {
                result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
            }
            result.m_ExecuteOk = false;
            return result;
        }

        // Post-execution inactivity check for synchronous tasks (python, internal).
        // Shell tasks enforce timeout inline via fork/exec/poll watchdog.
        if (watchdog && taskDefinition.m_Type != TaskType::Shell)
        {
            int64_t const inactiveMs = watchdog->ElapsedSinceLastKickMs();
            if (static_cast<uint64_t>(inactiveMs) > taskDefinition.m_TimeoutMs)
            {
                LOG_APP_WARN("Task '{}' exceeded inactivity timeout ({}ms inactive, {}ms limit)", taskId, inactiveMs,
                             taskDefinition.m_TimeoutMs);
                result.m_TaskState.m_State = TaskInstanceStateKind::Failed;
                result.m_TaskState.m_LastErrorMessage = "Task timed out (inactivity: " + std::to_string(inactiveMs) +
                                                        "ms, limit: " + std::to_string(taskDefinition.m_TimeoutMs) + "ms)";
                result.m_ExecuteOk = false;
                return result;
            }
        }

        {
            std::string summary;
            for (auto const& p : result.m_TaskState.m_OutputValues)
            {
                summary += p.first;
                summary += "=";
                summary += p.second;
                summary += ";";
            }
            result.m_TaskState.m_OutputsJson = summary;
        }

        // Preserve non-terminal executor-selected states (WaitingExternal).
        if (result.m_TaskState.m_State == TaskInstanceStateKind::Running)
        {
            result.m_TaskState.m_State = TaskInstanceStateKind::Succeeded;
        }

        result.m_ExecuteOk = (result.m_TaskState.m_State == TaskInstanceStateKind::Succeeded ||
                              result.m_TaskState.m_State == TaskInstanceStateKind::Skipped ||
                              result.m_TaskState.m_State == TaskInstanceStateKind::WaitingExternal);

        return result;
    }

    std::string WorkflowRuntimeManager::GenerateRunId(WorkflowDefinition const& workflowDefinition) const
    {
        auto now = std::chrono::system_clock::now();
        auto nowTimeT = std::chrono::system_clock::to_time_t(now);

        std::string runId = workflowDefinition.m_Id;
        runId += "_";
        runId += std::to_string(static_cast<long long>(nowTimeT));

        return runId;
    }

    bool WorkflowRuntimeManager::CheckAiProviderPrerequisites(WorkflowDefinition const& workflowDefinition) const
    {
        if (!workflowDefinition.m_HasAiCallTasks)
        {
            return true;
        }

        if (Core::g_Core == nullptr)
        {
            return true;
        }

        auto const& keyManager = Core::g_Core->GetKeyManager();

        if (!keyManager.HasProviders())
        {
            LOG_APP_WARN("Blocked workflow run '{}': contains ai_call tasks but no AI providers "
                         "are configured. Unlock the key store via POST /api/settings/keys/unlock "
                         "or configure providers via the Settings UI, then reload workflows.",
                         workflowDefinition.m_Id);
            return false;
        }

        // Check each required provider individually.
        // The providerName may be an interface name (e.g. "api.openai.com/gpt-4.1-mini/API1")
        // or a legacy key name (e.g. "openai").  Resolve interface name → key_name first.
        auto const& interfaces = Core::g_Core->GetConfig().m_ApiInterfaces;
        for (std::string const& providerName : workflowDefinition.m_RequiredAiProviders)
        {
            if (providerName.empty())
            {
                if (keyManager.GetDefaultCredential() == nullptr)
                {
                    LOG_APP_WARN("Blocked workflow run '{}': ai_call task requires system default "
                                 "provider, but no default provider is configured",
                                 workflowDefinition.m_Id);
                    return false;
                }
            }
            else
            {
                // Try interface name → key_name resolution first.
                std::string keyName;
                for (auto const& iface : interfaces)
                {
                    if (iface.m_Name == providerName)
                    {
                        keyName = iface.m_KeyName;
                        break;
                    }
                }

                // Fall back: treat providerName as a key name directly (legacy JCWFs).
                if (keyName.empty())
                {
                    keyName = providerName;
                }

                if (keyManager.GetCredential(keyName) == nullptr)
                {
                    LOG_APP_WARN("Blocked workflow run '{}': ai_call task requires provider '{}' "
                                 "(key '{}') which is not configured",
                                 workflowDefinition.m_Id, providerName, keyName);
                    return false;
                }
            }
        }

        return true;
    }

    bool WorkflowRuntimeManager::TryGetActiveRun(std::string const& runId, WorkflowRun& outRun) const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (auto const& activeRunPtr : m_ActiveRuns)
        {
            ActiveRun const& activeRun = *activeRunPtr;
            if (activeRun.m_Run.m_RunId == runId)
            {
                outRun = activeRun.m_Run;
                return true;
            }
        }

        return false;
    }

    bool WorkflowRuntimeManager::RequestCancelRun(std::string const& runId)
    {
        if (!IsValidRunOrWorkflowId(runId))
        {
            return false;
        }

        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (auto& activeRunPtr : m_ActiveRuns)
        {
            ActiveRun& activeRun = *activeRunPtr;
            if (activeRun.m_Run.m_RunId == runId)
            {
                activeRun.m_CancelRequested = true;
                if (activeRun.m_Run.m_CancellationToken)
                {
                    activeRun.m_Run.m_CancellationToken->Cancel();
                }
                CancelChildSubWorkflowRuns(runId);
                return true;
            }
        }

        return false;
    }

    bool WorkflowRuntimeManager::RequestPauseRun(std::string const& runId)
    {
        if (!IsValidRunOrWorkflowId(runId))
        {
            return false;
        }

        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (auto& activeRunPtr : m_ActiveRuns)
        {
            ActiveRun& activeRun = *activeRunPtr;
            if (activeRun.m_Run.m_RunId == runId)
            {
                if (activeRun.m_CancelRequested || activeRun.m_StopRequested)
                {
                    return false;
                }
                activeRun.m_PauseRequested = true;
                activeRun.m_Run.m_State = WorkflowRunState::Paused;
                LOG_APP_INFO("[workflow] pause requested for run '{}'", runId);
                return true;
            }
        }

        return false;
    }

    bool WorkflowRuntimeManager::RequestResumeRun(std::string const& runId)
    {
        if (!IsValidRunOrWorkflowId(runId))
        {
            return false;
        }

        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (auto& activeRunPtr : m_ActiveRuns)
        {
            ActiveRun& activeRun = *activeRunPtr;
            if (activeRun.m_Run.m_RunId == runId)
            {
                if (!activeRun.m_PauseRequested)
                {
                    return false;
                }
                activeRun.m_PauseRequested = false;
                activeRun.m_Run.m_State = WorkflowRunState::Running;
                LOG_APP_INFO("[workflow] resume requested for run '{}'", runId);
                return true;
            }
        }

        return false;
    }

    bool WorkflowRuntimeManager::RequestStopRun(std::string const& runId)
    {
        if (!IsValidRunOrWorkflowId(runId))
        {
            return false;
        }

        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (auto& activeRunPtr : m_ActiveRuns)
        {
            ActiveRun& activeRun = *activeRunPtr;
            if (activeRun.m_Run.m_RunId == runId)
            {
                if (activeRun.m_CancelRequested)
                {
                    return false;
                }
                activeRun.m_StopRequested = true;
                activeRun.m_PauseRequested = false;
                activeRun.m_Run.m_State = WorkflowRunState::Stopping;
                LOG_APP_INFO("[workflow] stop requested for run '{}'", runId);
                return true;
            }
        }

        return false;
    }

    bool WorkflowRuntimeManager::TryGetRunById(std::string const& runId, WorkflowRun& outRun) const
    {
        if (TryGetActiveRun(runId, outRun))
        {
            return true;
        }

        std::scoped_lock<std::mutex> const lock(m_Mutex);

        for (auto const& runPair : m_LastRuns)
        {
            WorkflowRun const& run = runPair.second;
            if (run.m_RunId == runId)
            {
                outRun = run;
                return true;
            }
        }

        return false;
    }

    std::vector<WorkflowRun> WorkflowRuntimeManager::GetActiveRunsSnapshot() const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);

        std::vector<WorkflowRun> runs;
        runs.reserve(m_ActiveRuns.size());
        for (auto const& activeRunPtr : m_ActiveRuns)
        {
            runs.emplace_back(activeRunPtr->m_Run);
        }

        return runs;
    }

    std::unordered_map<std::string, WorkflowRun> WorkflowRuntimeManager::GetLastRunsSnapshot() const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        return m_LastRuns; // copy
    }

    void WorkflowRuntimeManager::GetRunCounters(uint64_t& outCompleted, uint64_t& outFailed) const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        outCompleted = m_TotalCompletedRuns;
        outFailed = m_TotalFailedRuns;
    }

    // =================================================================
    // Clean command
    // =================================================================

    bool WorkflowRuntimeManager::CleanWorkflow(std::string const& workflowId, std::string& outErrorMessage)
    {
        outErrorMessage.clear();

        if (!IsValidRunOrWorkflowId(workflowId))
        {
            outErrorMessage = "workflow id is empty or contains characters outside [A-Za-z0-9._-] "
                              "(or starts with '.')";
            LOG_APP_ERROR("WorkflowRuntimeManager::CleanWorkflow: rejected invalid workflowId (len={})",
                          workflowId.size());
            return false;
        }

        // Reject if there is an active run for this workflow.
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            for (auto const& activeRunPtr : m_ActiveRuns)
            {
                ActiveRun const& activeRun = *activeRunPtr;
                if (activeRun.m_Run.m_WorkflowId == workflowId)
                {
                    outErrorMessage =
                        "cannot clean workflow '" + workflowId + "' while run '" + activeRun.m_Run.m_RunId + "' is active";
                    return false;
                }
            }
        }

        // Fetch workflow definition from registry.
        WorkflowRegistry const* workflowRegistry = nullptr;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            workflowRegistry = m_WorkflowRegistry;
        }

        if (workflowRegistry == nullptr)
        {
            outErrorMessage = "workflow registry is not available";
            return false;
        }

        std::optional<WorkflowDefinition> const workflowDefOpt = workflowRegistry->GetWorkflow(workflowId);
        if (!workflowDefOpt.has_value())
        {
            outErrorMessage = "workflow '" + workflowId + "' not found in registry";
            return false;
        }

        WorkflowDefinition const& workflowDef = workflowDefOpt.value();

        // Resolve the workflow base directory (same logic as ExecuteTaskOnWorker / DispatchFilterEvaluation).
        std::string workflowBaseDir = workflowDef.m_WorkflowBaseDirectoryAbsolute;
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowBaseDirectory;
        }
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowFileDirectoryAbsolute;
        }
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowFileDirectory;
        }

        fs::path const workflowBasePath = fs::absolute(fs::path(workflowBaseDir)).lexically_normal();

        size_t filesDeleted = 0;
        size_t dirsDeleted = 0;
        std::vector<std::string> errors;

        // Every path that reaches an fs::remove* call below must first pass
        // ConfineUnderProjectRoot.  An attacker-controlled file_outputs / working
        // directory entry — or a workflowId smuggled with `..` segments — could
        // otherwise resolve to a path outside the project tree (e.g. /etc/passwd,
        // /home/<user>/...).  Refuse to delete anything that does not
        // canonicalise inside the project root.  The helper also rejects
        // symlink targets that point out of tree, closing the symlink-attack
        // vector.
        //
        // The helper combines existence + type + delete in a single fs::remove*
        // syscall — fs::remove returns false (not error) for a non-existent path,
        // and ConfineUnderProjectRoot rejects symlink targets that point out of
        // tree.  This eliminates the TOCTOU window between fs::exists() and
        // fs::remove*() that the pre-fix call sites had (safety LOW 3).
        auto deleteIfConfined = [&errors](fs::path const& path, char const* siteLabel,
                                          bool recursive, size_t* outFilesDeleted, size_t* outDirsDeleted) -> bool
        {
            fs::path const confined = ConfineUnderProjectRoot(path);
            if (confined.empty())
            {
                std::string const msg =
                    std::string("[clean] refused to delete '") + path.string() +
                    "' at " + siteLabel + ": resolves outside project root or symlink-escapes it";
                LOG_APP_ERROR("{}", msg);
                errors.push_back(msg);
                return false;
            }

            std::error_code ec;
            if (recursive)
            {
                auto const removed = fs::remove_all(confined, ec);
                if (ec)
                {
                    errors.push_back("failed to remove directory '" + confined.string() + "': " + ec.message());
                    return false;
                }
                if (removed == 0)
                {
                    // Non-existent — silent skip; common during a second pass after
                    // a prior step removed the parent.  No log line, no stat bump.
                    return true;
                }
                if (outDirsDeleted) { ++(*outDirsDeleted); }
                if (outFilesDeleted) { *outFilesDeleted += (removed > 1) ? (removed - 1) : 0; }
                LOG_APP_INFO("[clean] removed {} '{}' ({} entries)", siteLabel, confined.string(), removed);
            }
            else
            {
                bool const removedOk = fs::remove(confined, ec);
                if (ec)
                {
                    errors.push_back("failed to remove '" + confined.string() + "': " + ec.message());
                    return false;
                }
                if (!removedOk)
                {
                    // Non-existent — silent skip.
                    return true;
                }
                if (outFilesDeleted) { ++(*outFilesDeleted); }
                LOG_APP_INFO("[clean] removed {} '{}'", siteLabel, confined.string());
            }
            return true;
        };

        // ---------------------------------------------------------------
        // 1) Delete queue/<workflowId>/ recursively
        // ---------------------------------------------------------------
        if (Core::g_Core != nullptr)
        {
            fs::path const queueRoot =
                fs::absolute(fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath)).lexically_normal();
            fs::path const queueWorkflowDir = queueRoot / workflowId;

            // No fs::exists pre-check — deleteIfConfined silent-skips on
            // non-existent paths (closes the TOCTOU window).
            deleteIfConfined(queueWorkflowDir, "queue directory", /*recursive=*/true,
                             &filesDeleted, &dirsDeleted);
        }

        // ---------------------------------------------------------------
        // 2) Delete declared file_outputs and working directories per task
        // ---------------------------------------------------------------
        // Collect working directories for recursive cleanup.
        std::vector<fs::path> workingDirsToClean;

        LOG_APP_INFO("[clean] workflowBasePath='{}' taskCount={}", workflowBasePath.string(), workflowDef.m_Tasks.size());

        for (auto const& [taskId, taskDef] : workflowDef.m_Tasks)
        {
            // Resolve task working directory.
            fs::path taskWorkDir;
            if (!taskDef.m_WorkingDirectory.empty())
            {
                taskWorkDir =
                    TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBasePath, taskDef.m_WorkingDirectory);
                LOG_APP_INFO("[clean] task '{}' workingDirectory='{}' resolved='{}'", taskId, taskDef.m_WorkingDirectory,
                             taskWorkDir.string());
            }

            // Delete declared file_outputs.
            for (std::string const& fileOutputTemplate : taskDef.m_FileOutputs)
            {
                if (fileOutputTemplate.empty())
                {
                    continue;
                }

                // file_outputs may contain glob-like patterns (e.g. "*.o").
                // Resolve relative to the task working directory.
                fs::path const outputPath(fileOutputTemplate);
                fs::path resolvedPath;

                if (outputPath.is_absolute())
                {
                    resolvedPath = outputPath.lexically_normal();
                }
                else if (!taskWorkDir.empty())
                {
                    resolvedPath = (taskWorkDir / outputPath).lexically_normal();
                }
                else
                {
                    resolvedPath = (workflowBasePath / outputPath).lexically_normal();
                }

                // If the path contains glob characters, expand and delete matching files.
                std::string const resolvedStr = resolvedPath.string();
                if (resolvedStr.find('*') != std::string::npos || resolvedStr.find('?') != std::string::npos)
                {
                    fs::path const parentDir = resolvedPath.parent_path();
                    std::string const pattern = resolvedPath.filename().string();

                    if (fs::exists(parentDir) && fs::is_directory(parentDir))
                    {
                        std::error_code ec;
                        for (auto const& entry : fs::directory_iterator(parentDir, ec))
                        {
                            if (!entry.is_regular_file())
                            {
                                continue;
                            }

                            std::string const filename = entry.path().filename().string();
                            if (GlobMatchesFilename(pattern, filename))
                            {
                                deleteIfConfined(entry.path(), "glob-matched file_output",
                                                 /*recursive=*/false, &filesDeleted, nullptr);
                            }
                        }
                    }
                }
                else
                {
                    // Literal file path.  No fs::exists / fs::is_regular_file
                    // pre-check — deleteIfConfined silent-skips on non-existent
                    // paths.  fs::remove on a directory fails with an error_code
                    // we surface to the errors vector, which is the right shape
                    // (a literal file_output that points at a directory is a
                    // workflow-author bug worth flagging).
                    deleteIfConfined(resolvedPath, "literal file_output",
                                     /*recursive=*/false, &filesDeleted, nullptr);
                }
            }

            // Remember working directories for empty-directory cleanup.
            if (!taskWorkDir.empty() && fs::exists(taskWorkDir) && fs::is_directory(taskWorkDir))
            {
                workingDirsToClean.push_back(taskWorkDir);
            }
        }

        // ---------------------------------------------------------------
        // 3) Clean up working directories (deepest first, recursive)
        // ---------------------------------------------------------------
        // Working directories contain entirely generated content (materialized
        // inputs, stdout/stderr captures, etc.), so remove them recursively.
        // Sort by path length descending so child dirs are removed before parents.
        std::sort(workingDirsToClean.begin(), workingDirsToClean.end(),
                  [](fs::path const& a, fs::path const& b) { return a.string().size() > b.string().size(); });

        for (fs::path const& dirPath : workingDirsToClean)
        {
            // No fs::exists pre-check — deleteIfConfined silent-skips.
            deleteIfConfined(dirPath, "working directory", /*recursive=*/true,
                             &filesDeleted, &dirsDeleted);
        }

        // ---------------------------------------------------------------
        // 4) Remove workflows/<workflowId>/ directory if now empty
        // ---------------------------------------------------------------
        {
            fs::path const workflowOutputDir = workflowBasePath / workflowId;
            // Keep the is_empty gate — semantic intent is "only delete an
            // empty leftover".  fs::remove on a non-empty directory fails
            // with "directory not empty" which we'd then surface as an error,
            // which is the wrong shape for the "stuff still there, leave it"
            // case.  TOCTOU window between is_empty and fs::remove is benign:
            // if a file appears, fs::remove returns false silently.
            std::error_code statEc;
            if (fs::is_directory(workflowOutputDir, statEc) && !statEc &&
                fs::is_empty(workflowOutputDir, statEc) && !statEc)
            {
                deleteIfConfined(workflowOutputDir, "empty workflow directory",
                                 /*recursive=*/false, nullptr, &dirsDeleted);
            }
        }

        // ---------------------------------------------------------------
        // Summary
        // ---------------------------------------------------------------
        if (!errors.empty())
        {
            outErrorMessage = "clean completed with errors:";
            for (std::string const& err : errors)
            {
                outErrorMessage += " [" + err + "]";
            }
            LOG_APP_WARN("[clean] workflow '{}': {} files, {} dirs deleted, {} errors", workflowId, filesDeleted,
                         dirsDeleted, errors.size());
            return false;
        }

        LOG_APP_INFO("[clean] workflow '{}': {} files, {} dirs deleted", workflowId, filesDeleted, dirsDeleted);
        return true;
    }

    // =================================================================
    // Per-item fan-out helpers
    // =================================================================

    FilterDef const* WorkflowRuntimeManager::FindFilterDef(WorkflowDefinition const& workflowDef,
                                                           std::string const& filterId) const
    {
        for (auto const& filter : workflowDef.m_Filters)
        {
            if (filter.m_Id == filterId)
            {
                return &filter;
            }
        }
        return nullptr;
    }

    std::string WorkflowRuntimeManager::ParentTaskId(std::string const& instanceId)
    {
        auto const pos = instanceId.find('#');
        if (pos == std::string::npos)
        {
            return instanceId;
        }
        return instanceId.substr(0, pos);
    }

    bool WorkflowRuntimeManager::IsChildInstance(std::string const& instanceId)
    {
        return instanceId.find('#') != std::string::npos;
    }

    void WorkflowRuntimeManager::DispatchFilterEvaluation(ActiveRun& activeRun, std::string const& taskId,
                                                          TaskDef const& taskDef)
    {
        WorkflowDefinition const& workflowDef = activeRun.m_Definition;
        WorkflowRun& workflowRun = activeRun.m_Run;

        FilterDef const* filterDef = FindFilterDef(workflowDef, taskDef.m_Filter);
        if (!filterDef)
        {
            auto& taskState = workflowRun.m_TaskStates[taskId];
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage =
                "per_item task '" + taskId + "' references unknown filter '" + taskDef.m_Filter + "'";
            workflowRun.m_HasFailed = true;
            return;
        }

        // Resolve workflow base directory (same logic as ExecuteTaskOnWorker)
        std::string workflowBaseDir = workflowDef.m_WorkflowBaseDirectoryAbsolute;
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowBaseDirectory;
        }
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowFileDirectoryAbsolute;
        }
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowFileDirectory;
        }

        FilterDef const filterDefCopy = *filterDef;
        std::string const parentTaskId = taskId;

        if (Core::g_Core == nullptr)
        {
            auto& taskState = workflowRun.m_TaskStates[taskId];
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "Core is null";
            workflowRun.m_HasFailed = true;
            return;
        }

        ThreadPool& pool = Core::g_Core->GetThreadPool();

        workflowRun.m_TaskStates[taskId].m_State = TaskInstanceStateKind::Running;

        activeRun.m_FilterEvalTasks[taskId] =
            pool.SubmitTask(
                    [filterDefCopy, workflowBaseDir, parentTaskId]() -> FilterEvalResult
                    {
                        FilterEvalResult result;
                        result.m_ParentTaskId = parentTaskId;

                        std::string errorMessage;
                        FilterEngine engine;
                        result.m_Items = engine.Evaluate(filterDefCopy, workflowBaseDir, errorMessage);

                        if (!errorMessage.empty())
                        {
                            result.m_Success = false;
                            result.m_ErrorMessage = errorMessage;
                            return result;
                        }

                        // Build and write manifest
                        FilterManifestManager manifestManager;
                        result.m_Manifest = manifestManager.BuildManifest(filterDefCopy.m_Id, result.m_Items, filterDefCopy);

                        std::string manifestError;
                        if (!manifestManager.WriteManifest(result.m_Manifest, workflowBaseDir, manifestError))
                        {
                            LOG_APP_WARN("[per_item] failed to write manifest for filter '{}': {}", filterDefCopy.m_Id,
                                         manifestError);
                        }

                        result.m_Success = true;
                        return result;
                    })
                .share();

        LOG_APP_INFO("[per_item] dispatched filter evaluation for task '{}' (filter '{}')", taskId, taskDef.m_Filter);
    }

    void WorkflowRuntimeManager::HarvestFilterEvalCompletions(ActiveRun& activeRun)
    {
        WorkflowRun& workflowRun = activeRun.m_Run;

        for (auto it = activeRun.m_FilterEvalTasks.begin(); it != activeRun.m_FilterEvalTasks.end();)
        {
            std::shared_future<FilterEvalResult>& future = it->second;

            if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                ++it;
                continue;
            }

            std::string const parentTaskId = it->first;
            FilterEvalResult evalResult;
            bool gotResult = false;

            try
            {
                evalResult = future.get();
                gotResult = true;
            }
            catch (std::exception const& e)
            {
                std::string const errorMsg = std::string("filter evaluation threw: ") + e.what();
                LOG_APP_ERROR("[per_item] task '{}' in run '{}': {}", parentTaskId, workflowRun.m_RunId, errorMsg);
                auto stateIt = workflowRun.m_TaskStates.find(parentTaskId);
                if (stateIt != workflowRun.m_TaskStates.end())
                {
                    stateIt->second.m_State = TaskInstanceStateKind::Failed;
                    stateIt->second.m_LastErrorMessage = errorMsg;
                }
                workflowRun.m_HasFailed = true;
            }
            catch (...)
            {
                LOG_APP_ERROR("[per_item] task '{}' in run '{}': filter evaluation threw unknown exception",
                              parentTaskId, workflowRun.m_RunId);
                auto stateIt = workflowRun.m_TaskStates.find(parentTaskId);
                if (stateIt != workflowRun.m_TaskStates.end())
                {
                    stateIt->second.m_State = TaskInstanceStateKind::Failed;
                    stateIt->second.m_LastErrorMessage = "filter evaluation threw unknown exception";
                }
                workflowRun.m_HasFailed = true;
            }

            if (gotResult)
            {
                if (!evalResult.m_Success)
                {
                    LOG_APP_WARN("[per_item] filter evaluation failed for task '{}' in run '{}': {}",
                                 parentTaskId, workflowRun.m_RunId, evalResult.m_ErrorMessage);
                    auto stateIt = workflowRun.m_TaskStates.find(parentTaskId);
                    if (stateIt != workflowRun.m_TaskStates.end())
                    {
                        stateIt->second.m_State = TaskInstanceStateKind::Failed;
                        stateIt->second.m_LastErrorMessage = evalResult.m_ErrorMessage;
                    }
                    workflowRun.m_HasFailed = true;
                }
                else
                {
                    // Look up the task definition to get the filter binding
                    auto defIt = activeRun.m_Definition.m_Tasks.find(parentTaskId);
                    if (defIt != activeRun.m_Definition.m_Tasks.end())
                    {
                        FanOutPerItemChildren(activeRun, parentTaskId, evalResult, defIt->second);
                    }
                    else
                    {
                        auto stateIt = workflowRun.m_TaskStates.find(parentTaskId);
                        if (stateIt != workflowRun.m_TaskStates.end())
                        {
                            stateIt->second.m_State = TaskInstanceStateKind::Failed;
                            stateIt->second.m_LastErrorMessage = "task definition not found after filter eval";
                        }
                        workflowRun.m_HasFailed = true;
                    }
                }
            }

            it = activeRun.m_FilterEvalTasks.erase(it);
        }
    }

    void WorkflowRuntimeManager::FanOutPerItemChildren(ActiveRun& activeRun, std::string const& parentTaskId,
                                                       FilterEvalResult const& evalResult, TaskDef const& taskDef)
    {
        WorkflowRun& workflowRun = activeRun.m_Run;
        WorkflowDefinition const& workflowDef = activeRun.m_Definition;

        // Resource cap: refuse the fan-out if the filter returned more items than
        // engineConfig.m_MaxPerItemFanOut.  An attacker-supplied filter (e.g. a
        // CSV with millions of rows, or a Polarion query that returns the entire
        // tracker) would otherwise spawn one task child + downstream dispatch
        // per item — exhausting threads, memory, and the AI provider quota.
        if (Core::g_Core != nullptr)
        {
            size_t const cap = Core::g_Core->GetConfig().m_MaxPerItemFanOut;
            if (cap > 0 && evalResult.m_Items.size() > cap)
            {
                std::string const msg = "per_item filter '" + taskDef.m_Filter + "' returned " +
                                        std::to_string(evalResult.m_Items.size()) +
                                        " items, exceeds max_per_item_fan_out=" + std::to_string(cap);
                LOG_APP_ERROR("[per_item] task '{}' in run '{}': {}", parentTaskId, workflowRun.m_RunId, msg);
                auto& parentState = workflowRun.m_TaskStates[parentTaskId];
                parentState.m_State = TaskInstanceStateKind::Failed;
                parentState.m_LastErrorMessage = msg;
                workflowRun.m_HasFailed = true;
                return;
            }
        }

        FilterDef const* filterDef = FindFilterDef(workflowDef, taskDef.m_Filter);
        std::string const binding = filterDef ? filterDef->m_Binding : "item";

        std::vector<std::string> childIds;
        childIds.reserve(evalResult.m_Items.size());

        // Read previous manifest for freshness comparison
        FilterManifestManager manifestManager;
        FilterManifest previousManifest;
        std::string prevManifestError;

        std::string workflowBaseDir = workflowDef.m_WorkflowBaseDirectoryAbsolute;
        if (workflowBaseDir.empty())
        {
            workflowBaseDir = workflowDef.m_WorkflowBaseDirectory;
        }

        bool const hasPreviousManifest =
            manifestManager.ReadManifest(taskDef.m_Filter, workflowBaseDir, previousManifest, prevManifestError);

        FilterManifestDiff diff;
        if (hasPreviousManifest)
        {
            diff = manifestManager.CompareManifests(previousManifest, evalResult.m_Manifest);
        }

        size_t skippedCount = 0;

        for (auto const& item : evalResult.m_Items)
        {
            std::string const childId = parentTaskId + "#" + std::to_string(item.m_Index);
            childIds.push_back(childId);

            TaskInstanceState childState;
            childState.m_State = TaskInstanceStateKind::Pending;
            childState.m_AttemptCount = 0;

            // Inject filter item values with binding prefix
            for (auto const& [fieldName, fieldValue] : item.m_Values)
            {
                childState.m_InputValues[binding + "." + fieldName] = fieldValue;
            }

            // Also inject the raw index and key
            childState.m_InputValues[binding + "._index"] = std::to_string(item.m_Index);
            childState.m_InputValues[binding + "._key"] = item.m_Key;
            childState.m_InputValues[binding + "._source_path"] = item.m_SourcePath;

            // Per-item output piping: inject upstream per_item task outputs for the matching item index.
            // When downstream per_item task B depends on per_item task A (same filter), A's child
            // outputs become available as {{A.captured_stdout}}, {{A.output_file}}, and
            // {{A.json.PATH}} (flattened from A's per-child response_<N>.json).
            for (std::string const& depId : taskDef.m_DependsOn)
            {
                auto upstreamChildrenIt = activeRun.m_PerItemChildren.find(depId);
                if (upstreamChildrenIt == activeRun.m_PerItemChildren.end())
                {
                    continue; // Not a per_item parent — no output piping needed
                }

                std::string const upstreamChildId = depId + "#" + std::to_string(item.m_Index);
                auto upstreamStateIt = workflowRun.m_TaskStates.find(upstreamChildId);
                if (upstreamStateIt == workflowRun.m_TaskStates.end())
                {
                    LOG_APP_WARN("[per_item] upstream child '{}' not found for downstream '{}' — "
                                 "skipping output piping",
                                 upstreamChildId, childId);
                    continue;
                }

                auto upstreamDefIt = workflowDef.m_Tasks.find(depId);
                if (upstreamDefIt == workflowDef.m_Tasks.end())
                {
                    continue;
                }

                InjectUpstreamOutputs(workflowDef, upstreamDefIt->second, upstreamStateIt->second, depId,
                                      upstreamChildId, childState.m_InputValues);
            }

            // Per-item freshness: skip if unchanged and outputs exist
            if (hasPreviousManifest && !diff.m_ExpressionChanged)
            {
                bool isUnchanged = false;
                for (size_t unchangedIdx : diff.m_UnchangedIndices)
                {
                    if (unchangedIdx == item.m_Index)
                    {
                        isUnchanged = true;
                        break;
                    }
                }

                if (isUnchanged)
                {
                    // Check if output files exist (simple existence check)
                    bool outputsExist = true;
                    for (auto const& fileOutput : taskDef.m_FileOutputs)
                    {
                        // Substitute binding variables in the output path
                        std::string resolvedOutput = fileOutput;
                        for (auto const& [k, v] : childState.m_InputValues)
                        {
                            std::string const placeholder = "{{" + k + "}}";
                            size_t pos = resolvedOutput.find(placeholder);
                            while (pos != std::string::npos)
                            {
                                resolvedOutput.replace(pos, placeholder.size(), v);
                                pos = resolvedOutput.find(placeholder, pos + v.size());
                            }
                        }

                        if (!resolvedOutput.empty() && !fs::exists(resolvedOutput))
                        {
                            outputsExist = false;
                            break;
                        }
                    }

                    if (outputsExist && !taskDef.m_FileOutputs.empty())
                    {
                        childState.m_State = TaskInstanceStateKind::Skipped;
                        ++skippedCount;
                    }
                }
            }

            workflowRun.m_TaskStates[childId] = std::move(childState);
        }

        activeRun.m_PerItemChildren[parentTaskId] = std::move(childIds);

        LOG_APP_INFO("[per_item] fan-out for task '{}': {} children ({} skipped as fresh)", parentTaskId,
                     evalResult.m_Items.size(), skippedCount);
    }

    void WorkflowRuntimeManager::AggregatePerItemResults(ActiveRun& activeRun)
    {
        WorkflowRun& workflowRun = activeRun.m_Run;

        for (auto const& [parentTaskId, childIds] : activeRun.m_PerItemChildren)
        {
            auto parentIt = workflowRun.m_TaskStates.find(parentTaskId);
            if (parentIt == workflowRun.m_TaskStates.end())
            {
                continue;
            }

            // Parent must be Running (set during DispatchFilterEvaluation)
            if (parentIt->second.m_State != TaskInstanceStateKind::Running)
            {
                continue;
            }

            bool allTerminal = true;
            bool anyFailed = false;
            size_t succeededCount = 0;
            size_t skippedCount = 0;

            for (std::string const& childId : childIds)
            {
                auto childIt = workflowRun.m_TaskStates.find(childId);
                if (childIt == workflowRun.m_TaskStates.end())
                {
                    allTerminal = false;
                    continue;
                }

                TaskInstanceStateKind const childState = childIt->second.m_State;

                if (!IsTerminal(childState))
                {
                    allTerminal = false;
                }

                if (childState == TaskInstanceStateKind::Failed)
                {
                    anyFailed = true;
                }
                else if (childState == TaskInstanceStateKind::Succeeded)
                {
                    ++succeededCount;
                }
                else if (childState == TaskInstanceStateKind::Skipped)
                {
                    ++skippedCount;
                }
            }

            if (!allTerminal)
            {
                continue;
            }

            if (anyFailed)
            {
                parentIt->second.m_State = TaskInstanceStateKind::Failed;
                parentIt->second.m_LastErrorMessage =
                    "per_item: one or more child instances failed (" + std::to_string(childIds.size()) + " total)";
                workflowRun.m_HasFailed = true;
            }
            else
            {
                parentIt->second.m_State = TaskInstanceStateKind::Succeeded;
            }

            parentIt->second.m_OutputsJson = "children=" + std::to_string(childIds.size()) +
                                             ";succeeded=" + std::to_string(succeededCount) +
                                             ";skipped=" + std::to_string(skippedCount);

            LOG_APP_INFO("[per_item] parent '{}' completed: {} children, {} succeeded, {} skipped", parentTaskId,
                         childIds.size(), succeededCount, skippedCount);
        }
    }

    // -----------------------------------------------------------------
    // Sub-workflow support
    // -----------------------------------------------------------------

    void WorkflowRuntimeManager::RegisterSubWorkflowLink(std::string const& childRunId, std::string const& parentRunId,
                                                         std::string const& parentTaskInstanceId)
    {
        if (!IsValidRunOrWorkflowId(childRunId) || !IsValidRunOrWorkflowId(parentRunId) ||
            parentTaskInstanceId.empty())
        {
            LOG_APP_ERROR("[sub-workflow] rejected invalid link registration "
                          "(childRunId.len={}, parentRunId.len={}, taskInstance.len={})",
                          childRunId.size(), parentRunId.size(), parentTaskInstanceId.size());
            return;
        }

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_SubWorkflowLinks[childRunId] = SubWorkflowLink{parentRunId, parentTaskInstanceId};
        }

        LOG_APP_INFO("[sub-workflow] registered link: child run '{}' → parent run '{}' task '{}'", childRunId,
                     parentRunId, parentTaskInstanceId);
    }

    // PRECONDITION: caller holds m_Mutex.  Iterates m_SubWorkflowLinks + m_ActiveRuns +
    // m_LastRuns; mutates parent ActiveRun task states + erases entries from
    // m_SubWorkflowLinks.
    void WorkflowRuntimeManager::PropagateSubWorkflowCompletions()
    {
        if (m_SubWorkflowLinks.empty())
        {
            return;
        }

        // Collect completed child runs that have a parent link.
        // We iterate the link map and check last-runs for completion.
        std::vector<std::string> completedChildRunIds;

        for (auto const& [childRunId, link] : m_SubWorkflowLinks)
        {
            // Check if the child run is still active (not yet completed).
            bool childStillActive = false;
            for (auto const& activeRunPtr : m_ActiveRuns)
            {
                if (activeRunPtr->m_Run.m_RunId == childRunId)
                {
                    childStillActive = true;
                    break;
                }
            }

            if (childStillActive)
            {
                continue;
            }

            // Child is no longer active — it must have completed.
            // Look it up in last-runs by run ID.
            WorkflowRun childRun;
            bool found = false;
            for (auto const& [workflowId, lastRun] : m_LastRuns)
            {
                if (lastRun.m_RunId == childRunId)
                {
                    childRun = lastRun;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                continue;
            }

            // Propagate to the parent task.
            for (auto& parentActiveRunPtr : m_ActiveRuns)
            {
                ActiveRun& parentActiveRun = *parentActiveRunPtr;
                if (parentActiveRun.m_Run.m_RunId != link.m_ParentRunId)
                {
                    continue;
                }

                auto taskIt = parentActiveRun.m_Run.m_TaskStates.find(link.m_ParentTaskInstanceId);
                if (taskIt == parentActiveRun.m_Run.m_TaskStates.end())
                {
                    continue;
                }

                if (taskIt->second.m_State != TaskInstanceStateKind::WaitingExternal)
                {
                    continue;
                }

                bool const childSucceeded = (childRun.m_State == WorkflowRunState::Succeeded);

                if (childSucceeded)
                {
                    taskIt->second.m_State = TaskInstanceStateKind::Succeeded;
                    taskIt->second.m_CompletedAtIso8601 = GetIso8601NowUTC();
                    taskIt->second.m_LastErrorMessage.clear();
                    LOG_APP_INFO("[sub-workflow] child run '{}' succeeded → parent task '{}' succeeded", childRunId,
                                 link.m_ParentTaskInstanceId);
                }
                else
                {
                    taskIt->second.m_State = TaskInstanceStateKind::Failed;
                    taskIt->second.m_CompletedAtIso8601 = GetIso8601NowUTC();
                    taskIt->second.m_LastErrorMessage =
                        "Child workflow run '" + childRunId + "' " +
                        (childRun.m_State == WorkflowRunState::Cancelled ? "was cancelled" : "failed");
                    LOG_APP_ERROR("[sub-workflow] child run '{}' {} → parent task '{}' failed", childRunId,
                                 (childRun.m_State == WorkflowRunState::Cancelled ? "cancelled" : "failed"),
                                 link.m_ParentTaskInstanceId);
                }

                break;
            }

            completedChildRunIds.push_back(childRunId);
        }

        // Clean up completed links.
        for (std::string const& childRunId : completedChildRunIds)
        {
            m_SubWorkflowLinks.erase(childRunId);
        }
    }

    void WorkflowRuntimeManager::CancelChildSubWorkflowRuns(std::string const& parentRunId)
    {
        for (auto const& [childRunId, link] : m_SubWorkflowLinks)
        {
            if (link.m_ParentRunId != parentRunId)
            {
                continue;
            }

            for (auto& activeRunPtr : m_ActiveRuns)
            {
                ActiveRun& activeRun = *activeRunPtr;
                if (activeRun.m_Run.m_RunId == childRunId)
                {
                    activeRun.m_CancelRequested = true;
                    LOG_APP_INFO("[sub-workflow] propagating cancellation to child run '{}'", childRunId);
                    break;
                }
            }
        }
    }

} // namespace AIAssistant
