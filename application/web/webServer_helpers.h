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

// Private internal helpers shared between webServer.cpp and webServer_studio.cpp.
// Not part of any public API.  Each helper is `inline` so the two translation
// units share a single definition (ODR-safe).  Do not include this from anywhere
// else — these are file-local plumbing.

#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include "core.h"
#include "engine.h"
#include "workflow/workflowJsonParser.h"
#include "workflow/workflowTypes.h"
#include "workflow/workflowValidator.h"

namespace AIAssistant
{
    namespace WebServerHelpers
    {
        // Resolve `relative` under `root` (using weakly_canonical) and assert
        // the result remains under root.  Returns empty path on rejection:
        // absolute `relative`, resolution error, or `..` escape after
        // canonicalisation.  Crow URL-decodes path parameters before handlers
        // run, so a request like `/dash-assets/../../etc/passwd` arrives at
        // the handler as `relative = "../../etc/passwd"`; without this gate,
        // a naive `root / relative` resolves outside the intended tree.
        inline std::filesystem::path ConfinePathUnder(std::filesystem::path const& root,
                                                      std::string const& relative)
        {
            if (relative.empty())
                return {};
            std::filesystem::path raw(relative);
            if (raw.is_absolute())
                return {};
            std::error_code ec;
            std::filesystem::path const canonicalRoot = std::filesystem::weakly_canonical(root, ec);
            if (ec || canonicalRoot.empty())
                return {};
            std::filesystem::path const resolved = std::filesystem::weakly_canonical(canonicalRoot / raw, ec);
            if (ec || resolved.empty())
                return {};
            std::filesystem::path const rel = resolved.lexically_relative(canonicalRoot);
            std::string const relGeneric = rel.generic_string();
            if (rel.empty() || relGeneric == ".." || relGeneric.rfind("../", 0) == 0)
                return {};
            return resolved;
        }

        inline std::string GenerateIntegrationRunId(std::string const& workflowId)
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            return workflowId + "_" + std::to_string(millis);
        }

        inline std::string ComputeHmacSha256Hex(std::string const& secret, std::string const& data)
        {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int digestLength = 0;

            HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                 reinterpret_cast<unsigned char const*>(data.data()), data.size(), digest, &digestLength);

            std::string hex;
            hex.reserve(digestLength * 2);
            static constexpr char hexDigits[] = "0123456789abcdef";
            for (unsigned int i = 0; i < digestLength; ++i)
            {
                hex.push_back(hexDigits[(digest[i] >> 4) & 0x0F]);
                hex.push_back(hexDigits[digest[i] & 0x0F]);
            }
            return hex;
        }

        inline bool VerifyHmacSignature(std::string const& secret, std::string const& body,
                                        std::string const& headerValue)
        {
            // Expected header format: "sha256=<hex>"
            static constexpr std::string_view kPrefix = "sha256=";
            if (headerValue.size() <= kPrefix.size() || headerValue.compare(0, kPrefix.size(), kPrefix) != 0)
            {
                return false;
            }

            std::string const providedHex = headerValue.substr(kPrefix.size());
            std::string const expectedHex = ComputeHmacSha256Hex(secret, body);

            // Constant-time comparison to prevent timing attacks.
            if (providedHex.size() != expectedHex.size())
            {
                return false;
            }
            unsigned char result = 0;
            for (size_t i = 0; i < providedHex.size(); ++i)
            {
                result |= static_cast<unsigned char>(providedHex[i]) ^ static_cast<unsigned char>(expectedHex[i]);
            }
            return result == 0;
        }

        inline void SetSecurityHeaders(crow::response& response)
        {
            response.set_header("X-Frame-Options", "DENY");
            response.set_header("X-Content-Type-Options", "nosniff");
            response.set_header("Referrer-Policy", "strict-origin-when-cross-origin");
            response.set_header("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
            response.set_header("Content-Security-Policy",
                                "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                                "connect-src 'self' ws: wss:; img-src 'self' data:");
        }

        inline void SetJsonHeaders(crow::response& response)
        {
            response.add_header("Content-Type", "application/json");
            response.add_header("Cache-Control", "no-store");
            SetSecurityHeaders(response);
        }

        inline bool IsBodyTooLarge(crow::request const& req, size_t maxMB)
        {
            return maxMB > 0 && req.body.size() > maxMB * 1024 * 1024;
        }

        inline crow::response MakePayloadTooLargeResponse(size_t maxMB)
        {
            crow::json::wvalue body;
            body["ok"] = false;
            body["error"] = "payload_too_large";
            body["message"] = "Request body exceeds " + std::to_string(maxMB) + " MB limit";
            crow::response resp(413, body.dump());
            SetJsonHeaders(resp);
            return resp;
        }

        inline crow::response MakeJsonResponse(int const httpStatus, crow::json::wvalue const& json)
        {
            crow::response response(httpStatus, json.dump());
            SetJsonHeaders(response);
            return response;
        }

        inline crow::response MakeAuthErrorResponse(std::string const& error)
        {
            crow::json::wvalue body;
            body["ok"] = false;
            if (error == "locked_out")
            {
                body["error"] = "locked_out";
                body["message"] = "Too many failed authentication attempts. Try again in 15 minutes.";
                crow::response resp(403, body.dump());
                SetJsonHeaders(resp);
                resp.add_header("Retry-After", "900");
                return resp;
            }
            else if (error == "rate_limited")
            {
                body["error"] = "rate_limited";
                body["message"] = "Too many requests. Try again later.";
                crow::response resp(429, body.dump());
                SetJsonHeaders(resp);
                resp.add_header("Retry-After", "5");
                return resp;
            }
            else if (error == "missing" || error == "malformed")
            {
                body["error"] = "unauthorized";
                body["message"] = "Authorization header required. Use: Authorization: Bearer <token>";
                crow::response resp(401, body.dump());
                SetJsonHeaders(resp);
                resp.add_header("WWW-Authenticate", "Bearer");
                return resp;
            }
            else if (error == "token_expired")
            {
                body["error"] = "token_expired";
                body["message"] = "API token has expired. A new token has been generated — check server logs.";
                crow::response resp(403, body.dump());
                SetJsonHeaders(resp);
                return resp;
            }
            else if (error == "insufficient_role")
            {
                body["error"] = "insufficient_role";
                body["message"] = "Your role does not have permission for this endpoint.";
                crow::response resp(403, body.dump());
                SetJsonHeaders(resp);
                return resp;
            }
            else
            {
                body["error"] = "forbidden";
                body["message"] = "Invalid API token.";
                crow::response resp(403, body.dump());
                SetJsonHeaders(resp);
                return resp;
            }
        }

        inline crow::response MakeJsonTextResponse(int const httpStatus, std::string const& jsonText)
        {
            crow::response response(httpStatus, jsonText);
            SetJsonHeaders(response);
            return response;
        }

        inline crow::response MakeWorkflowJsonError(int const httpStatus, std::string const& errorCode,
                                                    std::string const& message,
                                                    std::string const& endpoint,
                                                    std::optional<std::string> const& workflowId = std::nullopt)
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = false;
            responseJson["error"] = errorCode;
            responseJson["message"] = message;
            responseJson["endpoint"] = endpoint;
            if (workflowId.has_value())
            {
                responseJson["workflowId"] = workflowId.value();
            }

            return MakeJsonResponse(httpStatus, responseJson);
        }

        struct WorkflowValidationFinding
        {
            std::string m_Code;
            std::string m_Message;

            std::string m_Path;
            std::string m_TaskId;

            std::string m_Tier;
        };

        inline std::string ToTierString(WorkflowValidationTier const tier)
        {
            switch (tier)
            {
                case WorkflowValidationTier::A:
                    return "A";
                case WorkflowValidationTier::B:
                    return "B";
                case WorkflowValidationTier::C:
                    return "C";
                case WorkflowValidationTier::D:
                    return "D";
                default:
                    return "B";
            }
        }

        inline void AddFindingToList(crow::json::wvalue::list& list, WorkflowValidationFinding const& finding)
        {
            crow::json::wvalue item;
            item["code"] = finding.m_Code;
            item["message"] = finding.m_Message;
            if (!finding.m_Tier.empty())
            {
                item["tier"] = finding.m_Tier;
            }
            if (!finding.m_Path.empty())
            {
                item["path"] = finding.m_Path;
            }
            if (!finding.m_TaskId.empty())
            {
                item["taskId"] = finding.m_TaskId;
            }
            list.push_back(std::move(item));
        }

        inline crow::json::wvalue MakeWorkflowValidationResponse(bool const ok, std::string const& workflowId,
                                                                 std::vector<WorkflowValidationFinding> const& errors,
                                                                 std::vector<WorkflowValidationFinding> const& warnings,
                                                                 std::vector<WorkflowValidationFinding> const& infos)
        {
            crow::json::wvalue responseJson;
            responseJson["ok"] = ok;
            responseJson["id"] = workflowId;

            crow::json::wvalue::list errorsList;
            for (auto const& error : errors)
            {
                AddFindingToList(errorsList, error);
            }
            responseJson["errors"] = std::move(errorsList);

            crow::json::wvalue::list warningsList;
            for (auto const& warning : warnings)
            {
                AddFindingToList(warningsList, warning);
            }
            responseJson["warnings"] = std::move(warningsList);

            crow::json::wvalue::list infosList;
            for (auto const& info : infos)
            {
                AddFindingToList(infosList, info);
            }
            responseJson["infos"] = std::move(infosList);

            return responseJson;
        }

        inline void ValidateJcwfParsedWorkflow(WorkflowDefinition const& workflow,
                                               std::vector<WorkflowValidationFinding>& errors,
                                               std::vector<WorkflowValidationFinding>& warnings,
                                               std::vector<WorkflowValidationFinding>& infos)
        {
            std::vector<WorkflowValidationIssue> issues;
            WorkflowValidator::Validate(workflow, issues);

            for (WorkflowValidationIssue const& issue : issues)
            {
                WorkflowValidationFinding finding;
                finding.m_Code = issue.m_Code;
                finding.m_Message = issue.m_Message;
                finding.m_Path = issue.m_Path;
                finding.m_TaskId = issue.m_TaskId;
                finding.m_Tier = ToTierString(issue.m_Tier);

                if (issue.m_Severity == WorkflowValidationSeverity::Error)
                {
                    errors.push_back(std::move(finding));
                }
                else if (issue.m_Severity == WorkflowValidationSeverity::Info)
                {
                    infos.push_back(std::move(finding));
                }
                else
                {
                    warnings.push_back(std::move(finding));
                }
            }
        }

        inline void ValidateJcwfJsonText(std::string const& workflowJsonText,
                                         std::vector<WorkflowValidationFinding>& errors,
                                         std::vector<WorkflowValidationFinding>& warnings,
                                         std::vector<WorkflowValidationFinding>& infos,
                                         std::string& workflowIdOut,
                                         std::string& parseErrorMessageOut)
        {
            errors.clear();
            warnings.clear();
            infos.clear();
            workflowIdOut.clear();
            parseErrorMessageOut.clear();

            WorkflowJsonParser workflowJsonParser;
            WorkflowDefinition parsedWorkflow;
            if (!workflowJsonParser.ParseWorkflowJson(workflowJsonText, parsedWorkflow, parseErrorMessageOut))
            {
                return;
            }

            workflowIdOut = parsedWorkflow.m_Id;
            ValidateJcwfParsedWorkflow(parsedWorkflow, errors, warnings, infos);
        }

        inline bool IsValidWorkflowId(std::string const& workflowId)
        {
            if (workflowId.empty())
            {
                return false;
            }

            for (char const character : workflowId)
            {
                bool const isAlphaNumeric =
                    ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                     (character >= '0' && character <= '9'));
                bool const isAllowedSymbol = (character == '_' || character == '-');
                if (!isAlphaNumeric && !isAllowedSymbol)
                {
                    return false;
                }
            }

            return true;
        }

        inline bool IsValidTaskName(std::string const& taskName)
        {
            // Same restrictions as workflow ids: simple path-segment with no slashes.
            return IsValidWorkflowId(taskName);
        }

        inline std::filesystem::path GetWorkflowsDirectoryAbsolute(std::string& errorMessage)
        {
            errorMessage.clear();

            if (Core::g_Core == nullptr)
            {
                errorMessage = "Core::g_Core is null";
                return {};
            }

            auto const& config = Core::g_Core->GetConfig();

            std::filesystem::path workflowsPathFromConfig = std::filesystem::path(config.m_WorkflowsFolderFilepath);
            if (workflowsPathFromConfig.empty())
            {
                errorMessage = "Config m_WorkflowsFolderFilepath is empty";
                return {};
            }

            std::filesystem::path const& launchCwdAbsolute = Core::g_Core->GetLaunchCWDAbsolute();
            std::filesystem::path workflowsDirectoryAbsolute = workflowsPathFromConfig.is_absolute()
                                                                   ? workflowsPathFromConfig
                                                                   : (launchCwdAbsolute / workflowsPathFromConfig);

            return std::filesystem::absolute(workflowsDirectoryAbsolute).lexically_normal();
        }

        inline bool ReadTextFile(std::filesystem::path const& filePath, std::string& outContent)
        {
            std::ifstream fileStream(filePath, std::ios::in | std::ios::binary);
            if (!fileStream)
            {
                return false;
            }

            std::ostringstream stringStream;
            stringStream << fileStream.rdbuf();
            outContent = stringStream.str();
            return true;
        }

        inline bool WriteTextFileAtomic(std::filesystem::path const& filePath, std::string const& content,
                                        std::string& errorMessage)
        {
            errorMessage.clear();
            std::filesystem::path const parentDirectory = filePath.parent_path();
            std::error_code errorCode;
            std::filesystem::create_directories(parentDirectory, errorCode);
            if (errorCode)
            {
                errorMessage =
                    "Failed to create directories: " + parentDirectory.string() + " error=" + errorCode.message();
                return false;
            }

            std::filesystem::path const tempPath = filePath.string() + ".tmp";
            {
                std::ofstream fileStream(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!fileStream)
                {
                    errorMessage = "Failed to open temp file for writing: " + tempPath.string();
                    return false;
                }

                fileStream.write(content.data(), static_cast<std::streamsize>(content.size()));
                if (!fileStream.good())
                {
                    errorMessage = "Failed while writing temp file: " + tempPath.string();
                    return false;
                }
            }

            std::filesystem::rename(tempPath, filePath, errorCode);
            if (errorCode)
            {
                std::filesystem::remove(tempPath, errorCode);
                errorMessage =
                    "Failed to rename temp file to target: " + filePath.string() + " error=" + errorCode.message();
                return false;
            }

            return true;
        }

        inline char const* ToStringWorkflowRunState(WorkflowRunState const state)
        {
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
                default:
                    return "unknown";
            }
        }

        inline char const* ToStringTaskInstanceStateKind(TaskInstanceStateKind const state)
        {
            switch (state)
            {
                case TaskInstanceStateKind::Pending:
                    return "pending";
                case TaskInstanceStateKind::Ready:
                    return "ready";
                case TaskInstanceStateKind::Running:
                    return "running";
                case TaskInstanceStateKind::Skipped:
                    return "skipped";
                case TaskInstanceStateKind::Succeeded:
                    return "succeeded";
                case TaskInstanceStateKind::Failed:
                    return "failed";
                case TaskInstanceStateKind::WaitingExternal:
                    return "waiting_external";
                default:
                    return "unknown";
            }
        }

    } // namespace WebServerHelpers
} // namespace AIAssistant
