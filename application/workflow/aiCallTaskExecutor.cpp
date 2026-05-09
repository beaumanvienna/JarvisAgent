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

#include "workflow/aiCallTaskExecutor.h"
#include "workflow/templateEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "jarvisAgent.h"
#include "content/chunkPlanner.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "session/fileWriter.h"
#include "workflow/aiInvocation.h"
#include "workflow/aiRequestPool.h"
#include "workflow/taskPathResolver.h"

#include "simdjson/simdjson.h"

namespace AIAssistant
{
    namespace
    {
        // Hard cap on file reads — prevents a malicious or runaway path
        // (`/dev/zero`, an in-progress 100 GB log, a fifo) from exhausting
        // the parsing thread's heap.  100 MB is generous for prompt/context
        // material that's about to be sent to an AI provider.
        constexpr std::uintmax_t kMaxReadBytes = 100ULL * 1024ULL * 1024ULL;

        // Cap on glob match count.  Defends against a directory path that
        // resolves to a tree containing millions of small files (which can
        // happen with broken symlink loops or attacker-planted file farms).
        constexpr std::size_t kMaxGlobMatches = 10'000;

        // CNTX/PROB source paths are confined under the project root
        // (`ConfineUnderProjectRoot` from `application/file/pathConfinement.h`)
        // at every materialise site.  Cross-task data flow inside the
        // project (e.g. `ai_call` reading another task's output via
        // `../../../workflows/<id>/<other_task>/output.json`) is the
        // documented JCWF pattern, so the gate is project-root-wide rather
        // than task-folder-wide.  Defends against `/etc/shadow`-style
        // absolute paths and against `..`-laden relative paths that escape
        // the project tree.

        // Allowlist for paths handed to the markitdown shell command.  popen()
        // executes through /bin/sh so anything that could be parsed as a
        // metacharacter (including `;`, `|`, `&`, `(`, `)`, `<`, `>`, `$`, `\``,
        // `\n`, `\r`, etc.) must be rejected.  The previous blocklist missed
        // most of these.  Allowlist matches typical filesystem paths only.
        [[nodiscard]] static bool IsAllowedMarkitdownPath(std::string const& path) noexcept
        {
            if (path.empty() || path.size() > 4096)
            {
                return false;
            }
            for (char const c : path)
            {
                bool const ok =
                    (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '/' || c == '.' || c == '_' || c == '-' || c == '+' || c == '=' || c == ':';
                if (!ok)
                {
                    return false;
                }
            }
            // Reject any `..` segment defensively.
            if (path.find("..") != std::string::npos)
            {
                return false;
            }
            return true;
        }

        static int64_t NowTimestampNs()
        {
            auto const now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        }

        static bool ResolveTemplateString(std::string const& value,
                                          std::unordered_map<std::string, std::string> const& inputValues,
                                          std::string& outResolved)
        {
            outResolved.clear();
            outResolved.reserve(value.size());

            size_t pos = 0;

            while (pos < value.size())
            {
                size_t const dollar = value.find("${", pos);
                if (dollar == std::string::npos)
                {
                    outResolved.append(value.substr(pos));
                    break;
                }

                outResolved.append(value.substr(pos, dollar - pos));

                size_t const close = value.find('}', dollar + 2);
                if (close == std::string::npos)
                {
                    return false;
                }

                std::string const token = value.substr(dollar + 2, close - (dollar + 2));

                if (token.rfind("inputs.", 0) == 0)
                {
                    std::string const key = token.substr(7);
                    auto iterator = inputValues.find(key);
                    if (iterator == inputValues.end())
                    {
                        return false;
                    }
                    outResolved.append(iterator->second);
                }
                else
                {
                    return false;
                }

                pos = close + 1;
            }

            if (outResolved.find("${") != std::string::npos)
            {
                return false;
            }

            return true;
        }

        static bool ResolveTemplatePathList(std::vector<std::string> const& templates,
                                            std::unordered_map<std::string, std::string> const& inputValues,
                                            std::vector<std::string>& outResolvedPaths)
        {
            outResolvedPaths.clear();
            outResolvedPaths.reserve(templates.size());

            for (std::string const& t : templates)
            {
                if (t.find("${") == std::string::npos)
                {
                    outResolvedPaths.push_back(t);
                    continue;
                }

                std::string resolved;
                if (!ResolveTemplateString(t, inputValues, resolved))
                {
                    return false;
                }

                if (resolved.empty())
                {
                    return false;
                }

                outResolvedPaths.push_back(std::move(resolved));
            }

            return true;
        }

        // Build a flat key-value map from the workflow "defaults" JSON, prefixed with "defaults.".
        // E.g. {"ai":{"provider":"openai","model":"gpt-4.1-mini"}} becomes:
        //   "defaults.ai.provider" -> "openai"
        //   "defaults.ai.model"    -> "gpt-4.1-mini"
        static std::unordered_map<std::string, std::string> BuildDefaultsMap(std::string const& defaultsJson)
        {
            std::unordered_map<std::string, std::string> result;
            if (defaultsJson.empty())
            {
                return result;
            }

            try
            {
                simdjson::ondemand::parser parser;
                simdjson::padded_string padded(defaultsJson);
                simdjson::ondemand::document doc = parser.iterate(padded);
                simdjson::ondemand::object root = doc.get_object().value();

                for (auto field : root)
                {
                    std::string_view const key = field.unescaped_key().value();
                    simdjson::ondemand::value val = field.value();
                    simdjson::ondemand::json_type const type = val.type().value();

                    if (type == simdjson::ondemand::json_type::object)
                    {
                        simdjson::ondemand::object nested = val.get_object().value();
                        for (auto nestedField : nested)
                        {
                            std::string_view const nk = nestedField.unescaped_key().value();
                            simdjson::ondemand::value nv = nestedField.value();
                            simdjson::ondemand::json_type const nt = nv.type().value();

                            std::string fullKey = "defaults." + std::string(key) + "." + std::string(nk);

                            if (nt == simdjson::ondemand::json_type::string)
                            {
                                result[fullKey] = std::string(nv.get_string().value());
                            }
                            else if (nt == simdjson::ondemand::json_type::number)
                            {
                                result[fullKey] = std::to_string(nv.get_int64().value());
                            }
                        }
                    }
                    else if (type == simdjson::ondemand::json_type::string)
                    {
                        result["defaults." + std::string(key)] = std::string(val.get_string().value());
                    }
                    else if (type == simdjson::ondemand::json_type::number)
                    {
                        result["defaults." + std::string(key)] = std::to_string(val.get_int64().value());
                    }
                }
            }
            catch (std::exception const& e)
            {
                LOG_APP_WARN("BuildDefaultsMap: simdjson parse exception; defaults map left empty/partial: {}",
                             e.what());
            }
            catch (...)
            {
                LOG_APP_WARN("BuildDefaultsMap: non-std exception during parse; defaults map left empty/partial");
            }

            return result;
        }

        static std::string ExpandWithDefaults(std::string const& raw,
                                              std::unordered_map<std::string, std::string> const& defaultsMap)
        {
            if (raw.find("{{") == std::string::npos || defaultsMap.empty())
            {
                return raw;
            }

            TemplateContext ctx{};
            ctx.m_InputValues = &defaultsMap;

            std::string expanded;
            std::string error;
            if (ExpandTemplate(raw, ctx, TemplateMode::Lenient, expanded, error))
            {
                return expanded;
            }
            return raw;
        }

        static bool WriteInlineQueueFileRefs(std::vector<QueueFileRef> const& fileRefs, std::string& outErrorMessage)
        {
            for (QueueFileRef const& fileRef : fileRefs)
            {
                if (!fileRef.m_HasInlineContent)
                {
                    continue;
                }

                if (fileRef.m_Path.empty())
                {
                    outErrorMessage = "queue_binding contains inline file with empty 'path'";
                    return false;
                }

                LOG_APP_INFO("[paths debug] debug reason=writeInlineQueueBindingFile filePathRelative='{}' wasRelative='{}' "
                             "bytes={} ",
                             fileRef.m_Path, std::filesystem::path(fileRef.m_Path).is_relative(), fileRef.m_Content.size());

                if (!AiCallTaskExecutor::WriteTextFile(fileRef.m_Path, fileRef.m_Content, outErrorMessage))
                {
                    return false;
                }
            }

            return true;
        }

        static bool ReadTextFile(std::filesystem::path const& filePath, std::string& outText, std::string& outErrorMessage)
        {
            std::error_code sizeErrorCode;
            std::uintmax_t const fileBytes = std::filesystem::file_size(filePath, sizeErrorCode);
            if (sizeErrorCode)
            {
                outErrorMessage = std::string("Failed to stat file: ") + sizeErrorCode.message();
                return false;
            }
            if (fileBytes > kMaxReadBytes)
            {
                outErrorMessage = "Refusing to read file: size " + std::to_string(fileBytes) +
                                  " bytes exceeds cap " + std::to_string(kMaxReadBytes);
                return false;
            }
            std::ifstream inputStream(filePath, std::ios::binary);
            if (!inputStream.is_open())
            {
                outErrorMessage = std::string("Failed to open file for reading: ") + filePath.filename().string();
                return false;
            }

            std::ostringstream textStream;
            textStream << inputStream.rdbuf();
            outText = textStream.str();
            return true;
        }

        // Office document extensions that markitdown handles.  Lowercased before compare.
        static bool IsOfficeExtension(std::string extension)
        {
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return extension == ".pdf" || extension == ".docx" || extension == ".xlsx" ||
                   extension == ".pptx" || extension == ".odt";
        }

        // Runs `markitdown <input>` synchronously and captures stdout.  Returns
        // true on success with markdown in outMarkdown; false on any error with a
        // human-readable explanation in outErrorMessage.  Guards against:
        //  - markitdown CLI not installed (exit != 0 or popen failure)
        //  - empty output (the tool printed nothing)
        //  - explicit "# ERROR:" markers (matches the Python helper's convention)
        static bool ConvertWithMarkitdown(std::filesystem::path const& inputPath, std::string& outMarkdown,
                                          std::string& outErrorMessage)
        {
            outMarkdown.clear();
            outErrorMessage.clear();

            // Allowlist gate (replaces the previous narrow blocklist).  popen
            // routes through /bin/sh, so the path string MUST be limited to
            // characters that cannot be misinterpreted as shell metacharacters.
            // Future migration to argv-based execution (posix_spawn / fork+
            // execvp) would lift this restriction.
            std::string const rawPath = inputPath.string();
            if (!IsAllowedMarkitdownPath(rawPath))
            {
                outErrorMessage = "markitdown: refusing unsafe input path '" + rawPath + "'";
                return false;
            }

            std::string const command = "markitdown \"" + rawPath + "\" 2>/dev/null";
#if defined(_WIN32)
            FILE* pipe = _popen(command.c_str(), "r");
#else
            FILE* pipe = popen(command.c_str(), "r");
#endif
            if (pipe == nullptr)
            {
                outErrorMessage = "markitdown: popen failed for '" + rawPath + "'";
                return false;
            }

            constexpr size_t kChunkBytes = 16 * 1024;
            std::array<char, kChunkBytes> buffer{};
            while (true)
            {
                size_t const bytesRead = std::fread(buffer.data(), 1, buffer.size(), pipe);
                if (bytesRead > 0)
                {
                    outMarkdown.append(buffer.data(), bytesRead);
                }
                if (bytesRead < buffer.size())
                {
                    break;
                }
            }

#if defined(_WIN32)
            int const closeStatus = _pclose(pipe);
#else
            int const closeStatus = pclose(pipe);
#endif
            if (closeStatus != 0)
            {
                outErrorMessage = "markitdown: non-zero exit (" + std::to_string(closeStatus) + ") for '" +
                                  rawPath + "'";
                return false;
            }

            if (outMarkdown.empty())
            {
                outErrorMessage = "markitdown: empty output for '" + rawPath + "' — conversion failed silently";
                return false;
            }

            // Trim leading whitespace for the error-marker check.
            size_t const firstNonSpace = outMarkdown.find_first_not_of(" \t\r\n");
            if (firstNonSpace != std::string::npos && outMarkdown.compare(firstNonSpace, 8, "# ERROR:") == 0)
            {
                outErrorMessage = "markitdown: output starts with '# ERROR:' marker for '" + rawPath + "'";
                return false;
            }

            return true;
        }

        static bool StartsWith(std::string const& value, std::string const& prefix) { return value.rfind(prefix, 0) == 0; }

        static bool ContainsGlobChars(std::string const& path)
        {
            return path.find('*') != std::string::npos || path.find('?') != std::string::npos;
        }

        // Simple glob matcher supporting '*' (any sequence) and '?' (single char).
        static bool MatchGlob(std::string const& pattern, std::string const& text)
        {
            size_t pIdx = 0;
            size_t tIdx = 0;
            size_t starP = std::string::npos;
            size_t starT = 0;

            while (tIdx < text.size())
            {
                if (pIdx < pattern.size() && (pattern[pIdx] == text[tIdx] || pattern[pIdx] == '?'))
                {
                    ++pIdx;
                    ++tIdx;
                }
                else if (pIdx < pattern.size() && pattern[pIdx] == '*')
                {
                    starP = pIdx;
                    starT = tIdx;
                    ++pIdx;
                }
                else if (starP != std::string::npos)
                {
                    pIdx = starP + 1;
                    ++starT;
                    tIdx = starT;
                }
                else
                {
                    return false;
                }
            }

            while (pIdx < pattern.size() && pattern[pIdx] == '*')
            {
                ++pIdx;
            }

            return pIdx == pattern.size();
        }

        // Expand glob patterns in cntx_files into individual QueueFileRef entries.
        // Non-glob entries are passed through unchanged.
        static bool ExpandCntxFileGlobs(std::filesystem::path const& taskWorkingDirectoryPath,
                                        std::vector<AIAssistant::QueueFileRef> const& cntxFiles,
                                        std::vector<AIAssistant::QueueFileRef>& outExpanded, std::string& outErrorMessage)
        {
            outExpanded.clear();

            for (AIAssistant::QueueFileRef const& fileRef : cntxFiles)
            {
                if (fileRef.m_HasInlineContent || !ContainsGlobChars(fileRef.m_Path))
                {
                    outExpanded.push_back(fileRef);
                    continue;
                }

                // Resolve the glob path relative to the task working directory.
                std::filesystem::path globPath(fileRef.m_Path);
                if (!globPath.is_absolute())
                {
                    globPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, globPath);
                }
                else
                {
                    globPath = globPath.lexically_normal();
                }

                std::filesystem::path const parentDir = globPath.parent_path();
                std::string const filenamePattern = globPath.filename().string();

                if (!std::filesystem::is_directory(parentDir))
                {
                    outErrorMessage = "cntx_files glob directory does not exist: " + parentDir.string();
                    return false;
                }

                // Collect matching entries, then sort for deterministic ordering.
                std::vector<std::filesystem::path> matches;
                for (auto const& entry : std::filesystem::directory_iterator(parentDir))
                {
                    if (!entry.is_regular_file())
                    {
                        continue;
                    }

                    if (MatchGlob(filenamePattern, entry.path().filename().string()))
                    {
                        if (matches.size() >= kMaxGlobMatches)
                        {
                            outErrorMessage = "cntx_files glob match count exceeds cap " +
                                              std::to_string(kMaxGlobMatches) + " for pattern '" +
                                              fileRef.m_Path + "'";
                            return false;
                        }
                        matches.push_back(entry.path());
                    }
                }

                std::sort(matches.begin(), matches.end());

                LOG_APP_INFO("[paths debug] debug reason=expandCntxGlob pattern='{}' parentDir='{}' matchCount={}",
                             fileRef.m_Path, parentDir.string(), matches.size());

                if (matches.empty())
                {
                    LOG_APP_WARN("cntx_files glob matched zero files: {}", fileRef.m_Path);
                }

                for (std::filesystem::path const& matchPath : matches)
                {
                    AIAssistant::QueueFileRef expanded{};
                    expanded.m_Path = matchPath.lexically_normal().string();
                    expanded.m_HasInlineContent = false;
                    outExpanded.push_back(std::move(expanded));
                }
            }

            return true;
        }

        static bool MaterializeCntxFilesFromQueueBinding(std::filesystem::path const& taskWorkingDirectoryPath,
                                                         std::vector<AIAssistant::QueueFileRef>& cntxFiles,
                                                         std::string& outErrorMessage)
        {
            std::unordered_set<std::string> usedFilenames;

            for (size_t index = 0; index < cntxFiles.size(); ++index)
            {
                AIAssistant::QueueFileRef& fileRef = cntxFiles[index];

                // Inline CNTX files are handled by WriteInlineQueueBindingFiles().
                if (fileRef.m_HasInlineContent)
                {
                    continue;
                }

                if (fileRef.m_Path.empty())
                {
                    outErrorMessage = "queue_binding contains CNTX file with empty 'path'";
                    return false;
                }

                std::filesystem::path sourcePath(fileRef.m_Path);
                if (!sourcePath.is_absolute())
                {
                    sourcePath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, sourcePath);
                }
                else
                {
                    sourcePath = sourcePath.lexically_normal();
                }

                // Containment gate — refuse CNTX paths that resolve outside
                // the project root (e.g. `/etc/shadow`, or a `..`-laden
                // path that escapes the project tree).  Cross-task data
                // flow inside the project is allowed.
                std::filesystem::path const confinedSource = ConfineUnderProjectRoot(sourcePath);
                if (confinedSource.empty())
                {
                    outErrorMessage = "CNTX source path resolves outside project root: '" +
                                      fileRef.m_Path + "'";
                    LOG_APP_ERROR("[ai_call] {}", outErrorMessage);
                    return false;
                }
                sourcePath = confinedSource;

                // Office formats (.pdf/.docx/.xlsx/.pptx/.odt) can't be sent raw to
                // an AI — convert synchronously via markitdown and substitute the
                // resulting markdown for the file's text.  Failures fail the task
                // with a clear message rather than silently dispatching a binary blob.
                bool const isOffice = IsOfficeExtension(sourcePath.extension().string());
                std::string sourceText;
                if (isOffice)
                {
                    auto const markitdownStart = std::chrono::steady_clock::now();
                    std::error_code sizeEc;
                    uintmax_t const bytesOnDisk = std::filesystem::file_size(sourcePath, sizeEc);
                    LOG_APP_INFO("[ai_call] markitdown START source='{}' bytes-on-disk={}",
                                 sourcePath.string(), sizeEc ? 0ULL : static_cast<uint64_t>(bytesOnDisk));
                    std::string convertError;
                    bool const converted = ConvertWithMarkitdown(sourcePath, sourceText, convertError);
                    auto const markitdownEnd = std::chrono::steady_clock::now();
                    auto const elapsedMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(markitdownEnd - markitdownStart).count();
                    if (!converted)
                    {
                        LOG_APP_WARN("[ai_call] markitdown FAIL source='{}' elapsed={}ms error='{}'",
                                     sourcePath.string(), elapsedMs, convertError);
                        outErrorMessage = convertError;
                        return false;
                    }
                    LOG_APP_INFO("[ai_call] markitdown END   source='{}' elapsed={}ms markdown-bytes={}",
                                 sourcePath.string(), elapsedMs, sourceText.size());
                }
                else if (!ReadTextFile(sourcePath, sourceText, outErrorMessage))
                {
                    std::ostringstream errorStream;
                    errorStream << "Missing CNTX source '" << sourcePath.string() << "': " << outErrorMessage;
                    outErrorMessage = errorStream.str();
                    return false;
                }

                // Strip ".output" from the stem so the materialized CNTX file name reads
                // naturally on disk (e.g. PROB_NVDA.output.txt → CNTX_PROB_NVDA.txt).
                // For office-source conversions, rewrite the extension to .md so the
                // on-disk artifact reflects the converted content.
                std::string baseName;
                {
                    std::filesystem::path const srcFilename = sourcePath.filename();
                    std::string stem = srcFilename.stem().string();
                    std::string ext = srcFilename.extension().string();

                    if (stem.size() > 7 && stem.ends_with(".output"))
                    {
                        stem.erase(stem.size() - 7); // remove ".output"
                    }

                    if (isOffice)
                    {
                        ext = ".md";
                    }

                    baseName = stem + ext;
                }

                if (!StartsWith(baseName, "CNTX_"))
                {
                    baseName = "CNTX_" + baseName;
                }

                if (usedFilenames.find(baseName) != usedFilenames.end())
                {
                    std::ostringstream renamed;
                    renamed << "CNTX_" << index << "_" << sourcePath.filename().string();
                    baseName = renamed.str();
                }
                usedFilenames.insert(baseName);

                std::filesystem::path const destPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, baseName);

                LOG_APP_INFO("[paths debug] debug reason=materializeCntxFile sourcePathRelative='{}' "
                             "sourcePathAbsolute='{}' destPathAbsolute='{}'",
                             fileRef.m_Path, sourcePath.lexically_normal().generic_string(),
                             destPath.lexically_normal().generic_string());
                if (!AiCallTaskExecutor::WriteTextFile(destPath.string(), sourceText, outErrorMessage))
                {
                    return false;
                }

                // Promote to inline so downstream envelope construction can feed the content
                // into the user message.  Without this the envelope would send an empty CNTX.
                fileRef.m_Path = destPath.string();
                fileRef.m_Content = std::move(sourceText);
                fileRef.m_HasInlineContent = true;
            }

            return true;
        }

        // Materialize non-inline PROB file references into the working directory.
        // After materialization the QueueFileRef entries are updated in-place so that
        // the downstream expectedOutputPath logic (which checks m_HasInlineContent)
        // can find them.
        static bool MaterializeProbFilesFromQueueBinding(std::filesystem::path const& taskWorkingDirectoryPath,
                                                         std::vector<AIAssistant::QueueFileRef>& probFiles,
                                                         std::string& outErrorMessage)
        {
            std::unordered_set<std::string> usedFilenames;

            for (size_t index = 0; index < probFiles.size(); ++index)
            {
                AIAssistant::QueueFileRef& fileRef = probFiles[index];

                // Inline PROB files are handled by WriteInlineQueueBindingFiles().
                if (fileRef.m_HasInlineContent)
                {
                    continue;
                }

                if (fileRef.m_Path.empty())
                {
                    outErrorMessage = "queue_binding contains PROB file with empty 'path'";
                    return false;
                }

                std::filesystem::path sourcePath(fileRef.m_Path);
                if (!sourcePath.is_absolute())
                {
                    sourcePath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, sourcePath);
                }
                else
                {
                    sourcePath = sourcePath.lexically_normal();
                }

                std::filesystem::path const confinedProbSource = ConfineUnderProjectRoot(sourcePath);
                if (confinedProbSource.empty())
                {
                    outErrorMessage = "PROB source path resolves outside project root: '" +
                                      fileRef.m_Path + "'";
                    LOG_APP_ERROR("[ai_call] {}", outErrorMessage);
                    return false;
                }
                sourcePath = confinedProbSource;

                std::string sourceText;
                if (!ReadTextFile(sourcePath, sourceText, outErrorMessage))
                {
                    std::ostringstream errorStream;
                    errorStream << "Missing PROB source '" << sourcePath.filename().string() << "': "
                                << outErrorMessage;
                    outErrorMessage = errorStream.str();
                    return false;
                }

                // Build destination filename with PROB_ prefix.
                std::string baseName;
                {
                    std::filesystem::path const srcFilename = sourcePath.filename();
                    baseName = srcFilename.string();
                }

                if (!StartsWith(baseName, "PROB_"))
                {
                    baseName = "PROB_" + baseName;
                }

                if (usedFilenames.find(baseName) != usedFilenames.end())
                {
                    std::ostringstream renamed;
                    renamed << "PROB_" << index << "_" << sourcePath.filename().string();
                    baseName = renamed.str();
                }
                usedFilenames.insert(baseName);

                std::filesystem::path const destPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, baseName);

                LOG_APP_INFO("[paths debug] debug reason=materializeProbFile sourcePathRelative='{}' "
                             "sourcePathAbsolute='{}' destPathAbsolute='{}'",
                             fileRef.m_Path, sourcePath.lexically_normal().generic_string(),
                             destPath.lexically_normal().generic_string());
                if (!AiCallTaskExecutor::WriteTextFile(destPath.string(), sourceText, outErrorMessage))
                {
                    return false;
                }

                // Update the ref so downstream logic treats it as inline.
                fileRef.m_Path = destPath.string();
                fileRef.m_Content = sourceText;
                fileRef.m_HasInlineContent = true;
            }

            return true;
        }

    } // namespace

    std::string AiCallTaskExecutor::BuildProbFilename(int64_t const requestId, int64_t const timestampNs)
    {
        // Format: PROB_<id>_<timestampNs>.txt or PROB_<id>_<timestampNs>.txt
        std::ostringstream stringStream;
        stringStream << "PROB_" << requestId << "_" << timestampNs << ".txt";
        return stringStream.str();
    }

    bool AiCallTaskExecutor::WriteTextFile(std::string const& filePath, std::string const& fileContent,
                                           std::string& outErrorMessage)
    {
        outErrorMessage.clear();

        std::filesystem::path const filesystemPath(filePath);

        std::filesystem::path const filesystemPathAbsolute = std::filesystem::absolute(filesystemPath).lexically_normal();
        LOG_APP_INFO("[paths debug] debug reason=writeTextFile filePathRelative='{}' filePathAbsolute='{}' wasRelative='{}'",
                     filePath, filesystemPathAbsolute.generic_string(), filesystemPath.is_relative());
        std::error_code errorCode;

        std::filesystem::path const parentPath = filesystemPath.parent_path();
        if (!parentPath.empty())
        {
            std::error_code existsBeforeErrorCode;
            bool const existedBefore = std::filesystem::exists(parentPath, existsBeforeErrorCode);
            std::filesystem::path const parentPathAbsolute = std::filesystem::absolute(parentPath).lexically_normal();
            LOG_APP_INFO("[folder creation debug] debug create_directories attempt path='{}' reason='aiCallTaskExecutor "
                         "writeTextFile parent' existedBefore='{}' existsBeforeEc='{}' existsBeforeMsg='{}'",
                         parentPathAbsolute.generic_string(), existedBefore, existsBeforeErrorCode.value(),
                         existsBeforeErrorCode.message());
            std::filesystem::create_directories(parentPath, errorCode);
            if (errorCode)
            {
                LOG_APP_INFO("[folder creation debug] debug create_directories failed path='{}' ec='{}' message='{}' "
                             "reason='aiCallTaskExecutor writeTextFile parent'",
                             parentPathAbsolute.generic_string(), errorCode.value(), errorCode.message());
                outErrorMessage = "failed to create directories for: " + filePath + " (" + errorCode.message() + ")";
                return false;
            }
            if (!errorCode)
            {
                std::error_code existsAfterErrorCode;
                bool const existsAfter = std::filesystem::exists(parentPath, existsAfterErrorCode);
                bool const created = (!existedBefore && existsAfter && !existsAfterErrorCode);
                LOG_APP_INFO("[folder creation debug] debug create_directories ok path='{}' created='{}' existsAfter='{}' "
                             "existsAfterEc='{}' existsAfterMsg='{}'",
                             parentPathAbsolute.generic_string(), created, existsAfter, existsAfterErrorCode.value(),
                             existsAfterErrorCode.message());
            }
        }

        // Atomic write: open <final>.tmp.<counter> → write → close → rename.
        // A SIGKILL or disk-full mid-write leaves the previous version of the
        // final file intact, instead of a truncated partial that downstream
        // consumers parse as malformed.  The counter (atomic) makes the temp
        // name unique under concurrent writers targeting the same final path.
        static std::atomic<uint64_t> s_TempCounter{0};
        uint64_t const counter = s_TempCounter.fetch_add(1, std::memory_order_relaxed);
        std::filesystem::path const tempPath =
            filesystemPath.parent_path() /
            (filesystemPath.filename().string() + ".tmp." + std::to_string(counter));
        std::string const tempPathString = tempPath.string();

        {
            std::ofstream outputStream(tempPathString, std::ios::binary | std::ios::trunc);
            if (!outputStream.is_open())
            {
                outErrorMessage = "failed to open for writing: " + filesystemPath.filename().string();
                LOG_APP_ERROR("[ai_call] WriteTextFile open failed path='{}'", filePath);
                return false;
            }

            outputStream.write(fileContent.data(), static_cast<std::streamsize>(fileContent.size()));
            if (!outputStream.good())
            {
                outErrorMessage = "failed while writing: " + filesystemPath.filename().string();
                LOG_APP_ERROR("[ai_call] WriteTextFile write failed path='{}'", filePath);
                std::error_code rmEc;
                std::filesystem::remove(tempPathString, rmEc); // best-effort cleanup
                return false;
            }
        } // ofstream destructor closes the stream BEFORE rename

        std::error_code renameEc;
        std::filesystem::rename(tempPathString, filePath, renameEc);
        if (renameEc)
        {
            outErrorMessage = "failed to finalize write: " + filesystemPath.filename().string() +
                              " (" + renameEc.message() + ")";
            LOG_APP_ERROR("[ai_call] WriteTextFile rename failed temp='{}' final='{}' ec={}",
                          tempPathString, filePath, renameEc.message());
            std::error_code rmEc;
            std::filesystem::remove(tempPathString, rmEc); // best-effort cleanup
            return false;
        }

        return true;
    }

    std::optional<std::string> AiCallTaskExecutor::TryExtractStringParam(std::string const& rawParamsJson,
                                                                         std::string const& fieldName,
                                                                         std::string& outErrorMessage)
    {
        outErrorMessage.clear();

        if (rawParamsJson.empty())
        {
            return std::nullopt;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string const paddedJson(rawParamsJson);

        auto document = parser.iterate(paddedJson);
        if (document.error() != simdjson::SUCCESS)
        {
            outErrorMessage = "invalid params JSON: " + std::string(simdjson::error_message(document.error()));
            return std::nullopt;
        }

        std::string_view const fieldNameView(fieldName);

        auto field = document[fieldNameView];

        if (field.error() == simdjson::NO_SUCH_FIELD)
        {
            return std::nullopt;
        }

        if (field.error() != simdjson::SUCCESS)
        {
            outErrorMessage = "error accessing params JSON field '" + fieldName +
                              "': " + std::string(simdjson::error_message(field.error()));
            return std::nullopt;
        }

        std::string_view fieldText;
        auto const stringError = field.get(fieldText);
        if (stringError != simdjson::SUCCESS)
        {
            return std::nullopt;
        }

        return std::string(fieldText);
    }

    std::string AiCallTaskExecutor::ApplySimpleTemplate(std::string const& templateText, TaskInstanceState const& taskState)
    {
        TemplateContext context;
        context.m_InputValues = &taskState.m_InputValues;

        std::string expandedOut;
        std::string errorMessage;
        ExpandTemplate(templateText, context, TemplateMode::Lenient, expandedOut, errorMessage);
        return expandedOut;
    }

    std::string AiCallTaskExecutor::TryBuildPromptFromParams(TaskDef const& taskDefinition,
                                                             TaskInstanceState const& taskState)
    {
        std::string errorMessage;

        std::optional<std::string> const promptTemplate =
            TryExtractStringParam(taskDefinition.m_ParamsJson, "prompt_template", errorMessage);

        if (promptTemplate.has_value())
        {
            return ApplySimpleTemplate(promptTemplate.value(), taskState);
        }

        // Fallback: deterministic prompt that includes the raw params JSON and current inputs.
        std::ostringstream stringStream;

        stringStream << "[ai_call]\n";

        if (!taskDefinition.m_Label.empty())
        {
            stringStream << "task_label: " << taskDefinition.m_Label << "\n";
        }

        if (!taskDefinition.m_ParamsJson.empty())
        {
            stringStream << "params_json: " << taskDefinition.m_ParamsJson << "\n";
        }

        stringStream << "inputs:\n";
        for (auto const& pair : taskState.m_InputValues)
        {
            stringStream << "  " << pair.first << ": " << pair.second << "\n";
        }

        return stringStream.str();
    }

    bool AiCallTaskExecutor::WriteInlineQueueBindingFiles(QueueBinding const& queueBinding, std::string& outErrorMessage)
    {
        // Write all environment artifacts (STNG/TASK/CNTX) and PROB requirement files
        // to disk for replay/debug.  The envelope construction below carries the same
        // content, so these files are no longer load-bearing for dispatch.
        if (!WriteInlineQueueFileRefs(queueBinding.m_StngFiles, outErrorMessage))
        {
            return false;
        }

        if (!WriteInlineQueueFileRefs(queueBinding.m_TaskFiles, outErrorMessage))
        {
            return false;
        }

        if (!WriteInlineQueueFileRefs(queueBinding.m_CntxFiles, outErrorMessage))
        {
            return false;
        }

        if (!WriteInlineQueueFileRefs(queueBinding.m_ProbFiles, outErrorMessage))
        {
            return false;
        }

        return true;
    }

    bool AiCallTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                     TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        std::string errorMessage;

        std::string launchCWDAbsolute;
        if (Core::g_Core != nullptr)
        {
            launchCWDAbsolute = Core::g_Core->GetLaunchCWDAbsolute().string();
        }

        LOG_APP_INFO(
            "[paths debug] debug reason=spawnAiCallTask workflowId='{}' runId='{}' taskId='{}' "
            "workflowFilePathRelative='{}' workflowFilePathAbsolute='{}' workflowFileDirectoryRelative='{}' "
            "workflowFileDirectoryAbsolute='{}' workflowBaseDirectoryRelative='{}' workflowBaseDirectoryAbsolute='{}' "
            "launchCWDAbsolute='{}'",
            workflowDefinition.m_Id, workflowRun.m_RunId, taskDefinition.m_Id, workflowDefinition.m_WorkflowFilePath,
            workflowDefinition.m_WorkflowFilePathAbsolute, workflowDefinition.m_WorkflowFileDirectory,
            workflowDefinition.m_WorkflowFileDirectoryAbsolute, workflowDefinition.m_WorkflowBaseDirectory,
            workflowDefinition.m_WorkflowBaseDirectoryAbsolute, launchCWDAbsolute);

        // ------------------------------------------------------------
        // Resolve workflow base directory (directory containing the loaded .jcwf file)
        // ------------------------------------------------------------
        std::filesystem::path const workflowBaseDirectoryPath =
            TaskPathResolver::ResolveWorkflowBaseDirectory(workflowDefinition);

        LOG_APP_INFO("[paths debug] debug reason=resolveWorkflowBaseDirectory workflowId='{}' runId='{}' "
                     "selectedWorkflowBaseDirectory='{}'",
                     workflowDefinition.m_Id, workflowRun.m_RunId,
                     workflowBaseDirectoryPath.lexically_normal().generic_string());
        if (workflowBaseDirectoryPath.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "workflow base directory is empty (WorkflowDefinition not populated by loader)";
            return false;
        }

        std::filesystem::path const taskWorkingDirectoryPath =
            TaskPathResolver::ResolveTaskWorkingDirectoryPath(workflowBaseDirectoryPath, taskDefinition.m_WorkingDirectory);

        LOG_APP_INFO("[paths debug] debug reason=resolveTaskWorkingDirectory workflowId='{}' runId='{}' taskId='{}' "
                     "taskWorkingDirectoryRelative='{}' taskWorkingDirectoryAbsolute='{}'",
                     workflowDefinition.m_Id, workflowRun.m_RunId, taskDefinition.m_Id, taskDefinition.m_WorkingDirectory,
                     taskWorkingDirectoryPath.lexically_normal().generic_string());

        JarvisAgent* app = App::g_App;
        if (app == nullptr)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "App::g_App is null";
            return false;
        }

        AiRequestPool* requestPool = app->GetAiRequestPool();
        if (requestPool == nullptr)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "AiRequestPool is null";
            return false;
        }

        std::string const taskIdForBinding =
            taskState.m_TaskInstanceId.empty() ? taskDefinition.m_Id : taskState.m_TaskInstanceId;
        if (taskIdForBinding.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ai_call cannot bind request to workflow task: TaskDef.m_Id is empty";
            return false;
        }

        // ------------------------------------------------------------
        // Write inline queue binding files (STNG/TASK/CNTX static artifacts)
        // ------------------------------------------------------------
        QueueBinding localizedQueueBinding = taskDefinition.m_QueueBinding;

        auto const localizeInlineFileRefs = [&](std::vector<QueueFileRef>& fileRefs)
        {
            for (QueueFileRef& fileRef : fileRefs)
            {
                if (!fileRef.m_HasInlineContent)
                {
                    continue;
                }

                if (fileRef.m_Path.empty())
                {
                    continue;
                }

                std::filesystem::path const filePath(fileRef.m_Path);
                if (!filePath.is_absolute())
                {
                    std::filesystem::path const rewritten =
                        TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, filePath);
                    fileRef.m_Path = rewritten.string();
                }
            }
        };

        // Per-item tasks: substitute {{binding.field}} placeholders in inline paths BEFORE localization
        if (!taskState.m_InputValues.empty())
        {
            auto const substituteInlinePaths = [&](std::vector<QueueFileRef>& fileRefs)
            {
                for (QueueFileRef& fileRef : fileRefs)
                {
                    if (fileRef.m_HasInlineContent && !fileRef.m_Path.empty())
                    {
                        fileRef.m_Path = ApplySimpleTemplate(fileRef.m_Path, taskState);
                    }
                }
            };

            substituteInlinePaths(localizedQueueBinding.m_StngFiles);
            substituteInlinePaths(localizedQueueBinding.m_TaskFiles);
            substituteInlinePaths(localizedQueueBinding.m_CntxFiles);
            substituteInlinePaths(localizedQueueBinding.m_ProbFiles);
        }

        localizeInlineFileRefs(localizedQueueBinding.m_StngFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_TaskFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_CntxFiles);
        localizeInlineFileRefs(localizedQueueBinding.m_ProbFiles);

        // Per-item tasks: substitute {{binding.field}} placeholders in inline content
        if (!taskState.m_InputValues.empty())
        {
            auto const substituteInlineContent = [&](std::vector<QueueFileRef>& fileRefs)
            {
                for (QueueFileRef& fileRef : fileRefs)
                {
                    if (fileRef.m_HasInlineContent && !fileRef.m_Content.empty())
                    {
                        fileRef.m_Content = ApplySimpleTemplate(fileRef.m_Content, taskState);
                    }
                }
            };

            substituteInlineContent(localizedQueueBinding.m_StngFiles);
            substituteInlineContent(localizedQueueBinding.m_TaskFiles);
            substituteInlineContent(localizedQueueBinding.m_CntxFiles);
            substituteInlineContent(localizedQueueBinding.m_ProbFiles);
        }

        // ------------------------------------------------------------
        // Determine expected output path from the first PROB file.
        // Submit writes the reply as <stem>.output.txt (text/chunked-reduce) or
        // <stem>.output.json (structured output via output_schema).
        // Must match Submit's filename convention exactly so OnOutputFileCreated
        // matches the pending-path map.
        // ------------------------------------------------------------
        std::string const outputExtension = taskDefinition.m_OutputSchemaJson.empty() ? ".txt" : ".json";
        std::string expectedOutputPath;
        for (auto const& probFile : localizedQueueBinding.m_ProbFiles)
        {
            if (!probFile.m_Path.empty())
            {
                std::filesystem::path probPath(probFile.m_Path);

                // For non-inline (path-reference) PROB files, resolve relative to
                // the task working directory — they will be materialized into
                // the working directory later by MaterializeProbFilesFromQueueBinding.
                if (!probFile.m_HasInlineContent)
                {
                    if (!probPath.is_absolute())
                    {
                        probPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, probPath);
                    }
                    // Build the destination filename the same way MaterializeProbFilesFromQueueBinding does.
                    std::string baseName = probPath.filename().string();
                    if (baseName.rfind("PROB_", 0) != 0)
                    {
                        baseName = "PROB_" + baseName;
                    }
                    probPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, baseName);
                }

                std::filesystem::path outputPath = probPath;
                outputPath.replace_filename(probPath.stem().string() + ".output" + outputExtension);
                expectedOutputPath = outputPath.lexically_normal().generic_string();
                break;
            }
        }

        if (expectedOutputPath.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ai_call has no prob_files — cannot determine expected output path";
            return false;
        }

        LOG_APP_INFO("[paths debug] debug reason=resolveExpectedOutput workflowId='{}' runId='{}' taskId='{}' "
                     "expectedOutputPath='{}'",
                     workflowDefinition.m_Id, workflowRun.m_RunId, taskIdForBinding, expectedOutputPath);

        // ------------------------------------------------------------
        // Determine output mapping for completion (deterministic)
        // ------------------------------------------------------------
        std::vector<std::string> outputSlotNames;
        outputSlotNames.reserve(taskDefinition.m_Outputs.size());

        for (auto const& pair : taskDefinition.m_Outputs)
        {
            outputSlotNames.push_back(pair.first);
        }

        std::sort(outputSlotNames.begin(), outputSlotNames.end());

        std::vector<std::string> resolvedFileOutputs;
        if (!ResolveTemplatePathList(taskDefinition.m_FileOutputs, taskState.m_InputValues, resolvedFileOutputs))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "failed to resolve file_outputs template paths for ai_call";
            return false;
        }

        for (std::string& outputPathText : resolvedFileOutputs)
        {
            std::filesystem::path outputPath(outputPathText);
            if (!outputPath.is_absolute())
            {
                outputPath = TaskPathResolver::ResolvePath(taskWorkingDirectoryPath, outputPath);
            }
            else
            {
                outputPath = outputPath.lexically_normal();
            }
            outputPathText = outputPath.lexically_normal().generic_string();
        }

        // Allocate the request handle up front but DEFER registration with
        // AiRequestPool until after the slow materialization steps (markitdown
        // conversion of PDFs/DOCX/... can legitimately take tens of seconds
        // and would otherwise trip the 5-second file-activity watchdog).
        int64_t const requestId = requestPool->AllocateRequestId();
        int64_t const timestampNs = NowTimestampNs();

        AiRequestHandle requestHandle{};
        requestHandle.requestId = requestId;
        requestHandle.requestTimestampNs = timestampNs;

        // ------------------------------------------------------------
        // Per-subfolder provider settings sidecar (optional, write-only).
        // Replay tooling reads it; the envelope is authoritative for dispatch.
        // ------------------------------------------------------------
        {
            std::string providerOverrideError;
            std::optional<std::string> rawProviderOpt =
                TryExtractStringParam(taskDefinition.m_ParamsJson, "provider", providerOverrideError);
            std::optional<std::string> rawModelOpt =
                TryExtractStringParam(taskDefinition.m_ParamsJson, "model", providerOverrideError);
            std::optional<std::string> const temperatureOpt =
                TryExtractStringParam(taskDefinition.m_ParamsJson, "temperature", providerOverrideError);

            // Expand {{defaults.*}} templates in provider/model params.
            auto const defaultsMap = BuildDefaultsMap(workflowDefinition.m_DefaultsJson);
            std::optional<std::string> const providerOpt =
                rawProviderOpt ? std::optional(ExpandWithDefaults(*rawProviderOpt, defaultsMap)) : std::nullopt;
            std::optional<std::string> const modelOpt =
                rawModelOpt ? std::optional(ExpandWithDefaults(*rawModelOpt, defaultsMap)) : std::nullopt;

            if (providerOpt.has_value())
            {
                std::filesystem::path const providerSettingsPath = taskWorkingDirectoryPath / "PROV_provider.json";

                if (!std::filesystem::exists(providerSettingsPath))
                {
                    // Warn if the folder already has files — PROV must come first.
                    if (std::filesystem::exists(taskWorkingDirectoryPath) &&
                        !std::filesystem::is_empty(taskWorkingDirectoryPath))
                    {
                        LOG_APP_WARN("[ai_call] PROV file for task '{}' is being placed after other files "
                                     "already exist in '{}'. PROV should always be the first file in the queue folder.",
                                     taskIdForBinding, taskWorkingDirectoryPath.string());
                    }

                    // The JCWF "provider" field is an interface name from config.json
                    // (e.g. "api.openai.com/gpt-4.1-mini/API1"). Look it up by name to get
                    // URL, model, API type, and key_name. The PROV sidecar stores the
                    // key_name under "provider" for replay tooling.
                    std::string resolvedUrl;
                    std::string resolvedApiType;
                    std::string effectiveModel;
                    std::string keyNameForProv; // key_name from interface → goes into PROV "provider"
                    {
                        if (Core::g_Core == nullptr)
                        {
                            taskState.m_LastErrorMessage = "AiCallTaskExecutor: Core::g_Core is null";
                            taskState.m_State = TaskInstanceStateKind::Failed;
                            LOG_APP_ERROR("[ai_call] Core::g_Core is null run='{}' workflow='{}' task='{}'",
                                          workflowRun.m_RunId, workflowRun.m_WorkflowId, taskDefinition.m_Id);
                            return false;
                        }
                        auto const& interfaces = Core::g_Core->GetConfig().m_ApiInterfaces;

                        // Primary lookup: match by interface name.
                        for (auto const& iface : interfaces)
                        {
                            if (iface.m_Name == providerOpt.value())
                            {
                                resolvedUrl = iface.m_Url;
                                effectiveModel = modelOpt.value_or(iface.m_Model);
                                keyNameForProv = iface.m_KeyName;
                                switch (iface.m_InterfaceType)
                                {
                                    case ConfigParser::EngineConfig::InterfaceType::API1:
                                        resolvedApiType = "API1";
                                        break;
                                    case ConfigParser::EngineConfig::InterfaceType::API2:
                                        resolvedApiType = "API2";
                                        break;
                                    case ConfigParser::EngineConfig::InterfaceType::API3:
                                        resolvedApiType = "API3";
                                        break;
                                    default:
                                        break;
                                }
                                break;
                            }
                        }

                        if (resolvedUrl.empty())
                        {
                            LOG_APP_WARN("[ai_call] PROV: provider '{}' does not match any "
                                         "interface name in config.json for task '{}'",
                                         providerOpt.value(), taskIdForBinding);
                        }
                    }

                    // Every string value is JSON-escaped before embedding.
                    // Previous direct concatenation could produce malformed
                    // (or attacker-shaped) JSON if any field contained `"`,
                    // `\`, or control characters.
                    std::string sidecarJson = "{";
                    // Store key_name (not interface name) so replay tooling can resolve the API key.
                    std::string const& provForSidecar = keyNameForProv.empty() ? providerOpt.value() : keyNameForProv;
                    sidecarJson += "\"provider\":\"" + JsonHelper::EscapeJsonString(provForSidecar) + "\"";

                    if (!resolvedUrl.empty())
                    {
                        sidecarJson += ",\"url\":\"" + JsonHelper::EscapeJsonString(resolvedUrl) + "\"";
                    }

                    if (!resolvedApiType.empty())
                    {
                        sidecarJson += ",\"api_type\":\"" + JsonHelper::EscapeJsonString(resolvedApiType) + "\"";
                    }

                    if (!effectiveModel.empty())
                    {
                        sidecarJson += ",\"model\":\"" + JsonHelper::EscapeJsonString(effectiveModel) + "\"";
                    }

                    if (temperatureOpt.has_value())
                    {
                        // Validate as a finite double in a sane range before
                        // emitting as a JSON number — drops attacker-shaped
                        // strings (e.g. `1},"injected":true`) at the gate.
                        try
                        {
                            std::size_t consumed = 0;
                            double const temperatureValue = std::stod(temperatureOpt.value(), &consumed);
                            if (consumed == temperatureOpt.value().size() &&
                                std::isfinite(temperatureValue) &&
                                temperatureValue >= 0.0 && temperatureValue <= 2.0)
                            {
                                sidecarJson += ",\"temperature\":" + std::to_string(temperatureValue);
                            }
                            else
                            {
                                LOG_APP_WARN("Sidecar: refusing temperature out of range [0,2] or non-finite: '{}'",
                                             temperatureOpt.value());
                            }
                        }
                        catch (std::exception const&)
                        {
                            LOG_APP_WARN("Sidecar: refusing non-numeric temperature value '{}'",
                                         temperatureOpt.value());
                        }
                    }

                    sidecarJson += "}";

                    std::string sidecarError;
                    if (!WriteTextFile(providerSettingsPath.string(), sidecarJson, sidecarError))
                    {
                        LOG_APP_WARN("Failed to write provider settings '{}': {}", providerSettingsPath.string(),
                                     sidecarError);
                    }
                }
            }
        }

        // ------------------------------------------------------------
        // Write queue files and run synchronous materialization (including
        // markitdown conversion of any office-format cntx_files).  None of
        // these steps hold a registered pending entry, so failures just set
        // Failed and return — no Forget needed.
        // ------------------------------------------------------------
        if (!WriteInlineQueueBindingFiles(localizedQueueBinding, errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        // Expand glob patterns (e.g. "../01_lookupDividend/PROB_*.output.txt") into
        // individual file references before materialization.
        std::vector<QueueFileRef> expandedCntxFiles;
        if (!ExpandCntxFileGlobs(taskWorkingDirectoryPath, localizedQueueBinding.m_CntxFiles, expandedCntxFiles,
                                 errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        if (!MaterializeCntxFilesFromQueueBinding(taskWorkingDirectoryPath, expandedCntxFiles, errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        if (!MaterializeProbFilesFromQueueBinding(taskWorkingDirectoryPath, localizedQueueBinding.m_ProbFiles, errorMessage))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        // All slow I/O (inline writes, glob expansion, markitdown conversion,
        // prob-file materialization) is done.  Register the pending entry
        // now so the 5 s file-activity watchdog only covers the fast phase
        // between registration and Submit.
        AiRequestHandle const registered = requestPool->RegisterPendingWorkflowTask(
            requestHandle, workflowRun.m_WorkflowId, workflowRun.m_RunId, taskIdForBinding, resolvedFileOutputs,
            outputSlotNames, expectedOutputPath);

        if (!registered.IsValid())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "AiRequestPool::RegisterPendingWorkflowTask failed";
            return false;
        }
        requestPool->KickFileActivityWatchdog(requestHandle);

        // ------------------------------------------------------------
        // Direct envelope dispatch.  Concatenates STNG + CNTX + TASK + first PROB
        // content into a single user message and submits via AiRequestPool.  The
        // callback writes <prob>.output.{txt,json} and calls OnOutputFileCreated,
        // which releases the workflow-bound pending entry for the runtime tick.
        // ------------------------------------------------------------
        {
            // Build the message in three logical parts so chunking can split only
            // the CNTX (the typically-oversized part) while keeping STNG/TASK/PROB
            // intact in every chunk envelope.
            auto const joinContent = [](std::vector<QueueFileRef> const& fileRefs)
            {
                std::string result;
                for (auto const& fileRef : fileRefs)
                {
                    if (!fileRef.m_Content.empty())
                    {
                        if (!result.empty()) result += "\n";
                        result += fileRef.m_Content;
                    }
                }
                return result;
            };
            std::string const stngText = joinContent(localizedQueueBinding.m_StngFiles);
            std::string const taskText = joinContent(localizedQueueBinding.m_TaskFiles);
            std::string const cntxText = joinContent(expandedCntxFiles);
            // Build prefix = stng + task joined with newlines, skipping empty parts.
            std::string stngTaskPrefix;
            if (!stngText.empty()) stngTaskPrefix += stngText;
            if (!taskText.empty())
            {
                if (!stngTaskPrefix.empty()) stngTaskPrefix += "\n";
                stngTaskPrefix += taskText;
            }

            bool const hasStng = !stngText.empty();
            bool const hasTask = !taskText.empty();
            bool const hasCntx = !cntxText.empty();
            if (!hasStng)
            {
                LOG_APP_INFO("[ai_call] no STNG content for task '{}' — default settings apply", taskIdForBinding);
            }
            if (!hasTask)
            {
                LOG_APP_INFO("[ai_call] no TASK content for task '{}' — relying on PROB self-description",
                             taskIdForBinding);
            }
            if (!hasCntx)
            {
                LOG_APP_INFO("[ai_call] no CNTX content for task '{}' — results may be less precise",
                             taskIdForBinding);
            }

            std::string probRelativeName;
            std::string probContent;
            for (auto const& probFile : localizedQueueBinding.m_ProbFiles)
            {
                std::filesystem::path const probPath(probFile.m_Path);
                std::string baseName = probPath.filename().string();
                if (!probFile.m_HasInlineContent && baseName.rfind("PROB_", 0) != 0)
                {
                    baseName = "PROB_" + baseName;
                }
                probRelativeName = baseName;
                probContent = probFile.m_Content;
                break;
            }

            // Full single-envelope message: STNG + TASK + CNTX + PROB.
            auto const joinWithNewline = [](std::initializer_list<std::string const*> parts)
            {
                std::string result;
                for (auto const* part : parts)
                {
                    if (!part->empty())
                    {
                        if (!result.empty()) result += "\n";
                        result += *part;
                    }
                }
                return result;
            };
            std::string const combinedMessage = joinWithNewline({&stngTaskPrefix, &cntxText, &probContent});

            bool hasNonWhitespace = false;
            for (char const character : combinedMessage)
            {
                if (!std::isspace(static_cast<unsigned char>(character)))
                {
                    hasNonWhitespace = true;
                    break;
                }
            }

            if (hasNonWhitespace && !probRelativeName.empty())
            {
                // Resolve per-task api_interface override (JCWF "provider" param), if any.
                std::string interfaceOverrideError;
                std::optional<std::string> const rawProviderOpt =
                    TryExtractStringParam(taskDefinition.m_ParamsJson, "provider", interfaceOverrideError);
                auto const defaultsMapEnvelope = BuildDefaultsMap(workflowDefinition.m_DefaultsJson);
                std::string envelopeInterfaceName;
                if (rawProviderOpt.has_value())
                {
                    envelopeInterfaceName = ExpandWithDefaults(*rawProviderOpt, defaultsMapEnvelope);
                }

                AiInvocation envelope;
                envelope.m_InterfaceName = envelopeInterfaceName;
                envelope.m_QueueFolder = taskWorkingDirectoryPath;
                envelope.m_ProbName = probRelativeName;
                // Forward the JCWF-declared per-task timeout (if any) so it
                // overrides the size-aware budget in AiRequestPool::Submit.
                // Workflow authors who set timeout_ms in the task definition
                // beat the auto-computed budget.
                if (taskDefinition.m_TimeoutMs > 0)
                {
                    envelope.m_Timeout = std::chrono::milliseconds(taskDefinition.m_TimeoutMs);
                }
                if (!taskDefinition.m_OutputSchemaJson.empty())
                {
                    envelope.m_OutputSchemaJson = taskDefinition.m_OutputSchemaJson;
                }
                if (taskDefinition.m_OutputSchemaMaxAttempts > 0)
                {
                    envelope.m_Retry.m_OutputSchemaMaxAttempts =
                        static_cast<int>(taskDefinition.m_OutputSchemaMaxAttempts);
                }

                Message userMessage;
                userMessage.m_Role = MessageRole::User;
                userMessage.m_Content = combinedMessage;
                envelope.m_Messages.push_back(std::move(userMessage));

                // Structure-aware chunking.  When the combined prompt overruns the
                // interface's max_context_tokens, split it at markdown section boundaries
                // and fan out one envelope per chunk.  Each chunk writes a per-chunk
                // .output.chunk<i>-of-<N>.txt; once all chunks arrive, the aggregator
                // concatenates them into the final <prob>.output.txt and signals
                // completion via AiRequestPool::OnOutputFileCreated.
                //
                // Chunking + output_schema are mutually exclusive — a chunked response
                // can't satisfy a whole-object schema.  If both are set, schema wins:
                // single-envelope dispatch proceeds (possibly oversized) and the
                // provider may reject it.
                uint64_t maxContextTokens = 0;
                if (Core::g_Core != nullptr)
                {
                    auto const& configForChunk = Core::g_Core->GetConfig();
                    for (auto const& apiCandidate : configForChunk.m_ApiInterfaces)
                    {
                        bool const matchesRequested =
                            envelopeInterfaceName.empty()
                                ? (&apiCandidate == &configForChunk.m_ApiInterfaces[configForChunk.m_ApiIndex])
                                : (apiCandidate.m_Name == envelopeInterfaceName);
                        if (matchesRequested && apiCandidate.m_MaxContextTokens > 0)
                        {
                            maxContextTokens = apiCandidate.m_MaxContextTokens;
                            break;
                        }
                    }
                }

                std::vector<std::string> chunks;
                if (maxContextTokens > 0 && !envelope.m_OutputSchemaJson.has_value())
                {
                    uint64_t const estimatedTokens = ChunkPlanner::EstimateTokens(combinedMessage);
                    if (estimatedTokens > maxContextTokens)
                    {
                        // Reserve room for STNG/TASK/PROB prefix/suffix (repeated in every
                        // chunk) plus ~20% overhead for the model response itself.
                        uint64_t const fixedOverheadTokens =
                            ChunkPlanner::EstimateTokens(stngTaskPrefix) + ChunkPlanner::EstimateTokens(probContent);
                        uint64_t const responseOverhead = maxContextTokens / 5;
                        uint64_t const overhead = fixedOverheadTokens + responseOverhead;
                        // Chunk only the CNTX text so each chunk envelope keeps the full
                        // STNG/TASK instructions and trailing PROB intact.
                        chunks = ChunkPlanner::Plan(cntxText, maxContextTokens, overhead);
                        LOG_APP_INFO("[ai_call] task '{}' chunking: ~{} tokens > max_context_tokens={} → {} chunks",
                                     taskIdForBinding, estimatedTokens, maxContextTokens, chunks.size());
                    }
                }
                if (envelope.m_OutputSchemaJson.has_value() && maxContextTokens > 0)
                {
                    uint64_t const estimatedTokens = ChunkPlanner::EstimateTokens(combinedMessage);
                    if (estimatedTokens > maxContextTokens)
                    {
                        LOG_APP_WARN("[ai_call] task '{}' prompt est ~{} tokens exceeds max_context_tokens={} but "
                                     "output_schema is set — chunking is skipped (schema wins), request may be rejected",
                                     taskIdForBinding, estimatedTokens, maxContextTokens);
                    }
                }

                if (chunks.size() <= 1)
                {
                    // Single-envelope dispatch (the common case — no chunking needed).
                    // Fire-and-forget: Submit's callback writes <prob>.output.txt and
                    // calls OnOutputFileCreated itself.
                    if (!requestPool->Submit(envelope, nullptr))
                    {
                        requestPool->Forget(requestHandle);
                        taskState.m_State = TaskInstanceStateKind::Failed;
                        taskState.m_LastErrorMessage =
                            "ai_call Submit rejected envelope (no interface, no API key, or empty body)";
                        return false;
                    }
                }
                else
                {
                    // Chunked fan-out with reduce pass.
                    //
                    //   1. Each chunk envelope carries `stngTaskPrefix + <chunk_i_of_cntx> + probContent`,
                    //      so every chunk sees the full instructions and the original question.
                    //   2. Aggregator collects N replies.
                    //   3. Once all N arrive, submit a single "reduce" envelope that consumes
                    //      the N partial answers and produces ONE unified reply satisfying the
                    //      original PROB.  This avoids the "5×N bullets" concat-artefact.
                    //   4. If the reduce envelope is rejected at Submit time, fall back to
                    //      plain concat of partial replies.
                    //
                    // The per-chunk .output.chunk<i>-of-<N>.txt files remain on disk for
                    // replay; the reduced reply becomes <prob>.output.txt.
                    struct ChunkAggregator
                    {
                        std::mutex m_Mutex;
                        std::vector<std::string> m_Parts;
                        size_t m_Remaining;
                        bool m_AnyFailed{false};
                        std::string m_FirstErrorMessage;
                        std::filesystem::path m_QueueFolder;
                        std::string m_ProbName;
                        // Borrowed pointer to the AiRequestPool that owns the curl callbacks
                        // dispatching to this aggregator.  The pool is owned by
                        // `JarvisAgent::m_AiRequestPool` as `std::unique_ptr<AiRequestPool>`;
                        // its `Shutdown()` drains all in-flight curl callbacks BEFORE the pool
                        // destructs, so the raw pointer is stable for the entire aggregator
                        // lifetime under correct shutdown ordering.  Every consumer site
                        // ALSO null-checks before deref as defense in depth.
                        AiRequestPool* m_RequestPool;
                        std::string m_TaskIdForLog;
                        // Reduce-pass context: carries instruction prefix/suffix so the
                        // aggregator can build the reduce envelope without re-reading files.
                        std::string m_StngTaskPrefix;
                        std::string m_ProbContent;
                        AiInvocation m_EnvelopeTemplate;
                    };

                    auto aggregator = std::make_shared<ChunkAggregator>();
                    aggregator->m_Parts.assign(chunks.size(), std::string{});
                    aggregator->m_Remaining = chunks.size();
                    aggregator->m_QueueFolder = taskWorkingDirectoryPath;
                    aggregator->m_ProbName = probRelativeName;
                    aggregator->m_RequestPool = requestPool;
                    aggregator->m_TaskIdForLog = taskIdForBinding;
                    aggregator->m_StngTaskPrefix = stngTaskPrefix;
                    aggregator->m_ProbContent = probContent;
                    aggregator->m_EnvelopeTemplate = envelope;

                    // Helper: write the plain concatenation fallback when reduce can't run.
                    auto const writeConcatFallback = [](std::shared_ptr<ChunkAggregator> const& agg,
                                                         size_t totalChunks, std::string const& reason)
                    {
                        std::string combined;
                        for (size_t i = 0; i < agg->m_Parts.size(); ++i)
                        {
                            if (i > 0)
                            {
                                combined += "\n\n<!-- chunk " + std::to_string(i + 1) + "/" +
                                            std::to_string(totalChunks) + " -->\n\n";
                            }
                            combined += agg->m_Parts[i];
                        }
                        if (!agg->m_QueueFolder.empty() && !agg->m_ProbName.empty())
                        {
                            std::filesystem::path const outputPath =
                                agg->m_QueueFolder /
                                (std::filesystem::path(agg->m_ProbName).stem().string() + ".output.txt");
                            FileWriter::Get().WriteWithHeader(outputPath, combined, std::string{});
                            std::string const normalizedPath =
                                std::filesystem::absolute(outputPath).lexically_normal().generic_string();
                            if (agg->m_RequestPool != nullptr)
                            {
                                // Aggregator's reduce-pass fallback writes the final output;
                                // the per-path lookup signal is observability only — a missing
                                // pending entry here just means the binding already completed.
                                (void)agg->m_RequestPool->OnOutputFileCreated(normalizedPath);
                            }
                        }
                        LOG_APP_WARN("[ai_call] task '{}' reduce pass fallback to plain concat ({})",
                                     agg->m_TaskIdForLog, reason);
                    };

                    size_t const totalChunks = chunks.size();
                    bool allSubmitted = true;
                    for (size_t chunkIndex = 0; chunkIndex < totalChunks; ++chunkIndex)
                    {
                        AiInvocation chunkEnvelope = envelope;
                        chunkEnvelope.m_ChunkIndex = static_cast<int32_t>(chunkIndex);
                        chunkEnvelope.m_ChunkCount = static_cast<int32_t>(totalChunks);
                        chunkEnvelope.m_Messages.clear();

                        // Build chunk user message: stngTaskPrefix + chunk-of-cntx + probContent.
                        // Each chunk is a self-contained request: full instructions, a slice
                        // of the context, and the same question.
                        std::string chunkBody;
                        auto const appendPart = [&chunkBody](std::string const& s)
                        {
                            if (s.empty()) return;
                            if (!chunkBody.empty()) chunkBody += "\n";
                            chunkBody += s;
                        };
                        appendPart(stngTaskPrefix);
                        std::string const chunkLabel =
                            "[context slice " + std::to_string(chunkIndex + 1) + "/" +
                            std::to_string(totalChunks) +
                            " — the full input was split; this is one section]\n";
                        appendPart(chunkLabel + chunks[chunkIndex]);
                        appendPart(probContent);

                        Message chunkMessage;
                        chunkMessage.m_Role = MessageRole::User;
                        chunkMessage.m_Content = std::move(chunkBody);
                        chunkEnvelope.m_Messages.push_back(std::move(chunkMessage));

                        auto onReply = [aggregator, chunkIndex, totalChunks, writeConcatFallback](
                                           AiReply const& reply)
                        {
                            bool shouldFinalize = false;
                            {
                                std::scoped_lock<std::mutex> const lock(aggregator->m_Mutex);
                                if (reply.m_Kind == AiReply::Kind::Text)
                                {
                                    aggregator->m_Parts[chunkIndex] = reply.m_Text;
                                }
                                else
                                {
                                    aggregator->m_AnyFailed = true;
                                    if (aggregator->m_FirstErrorMessage.empty())
                                    {
                                        aggregator->m_FirstErrorMessage =
                                            reply.m_Error.m_Message.empty()
                                                ? "chunk " + std::to_string(chunkIndex) + " failed"
                                                : reply.m_Error.m_Message;
                                    }
                                }
                                --aggregator->m_Remaining;
                                shouldFinalize = (aggregator->m_Remaining == 0);
                            }
                            if (!shouldFinalize) return;

                            // If any chunk failed, skip the reduce pass — concat what we have.
                            if (aggregator->m_AnyFailed)
                            {
                                LOG_APP_WARN("[ai_call] task '{}' chunked dispatch had errors: {} — "
                                             "writing plain concat and signalling completion",
                                             aggregator->m_TaskIdForLog, aggregator->m_FirstErrorMessage);
                                writeConcatFallback(aggregator, totalChunks, "one or more chunks failed");
                                return;
                            }

                            LOG_APP_INFO("[ai_call] task '{}' all {} chunks returned — submitting reduce pass",
                                         aggregator->m_TaskIdForLog, totalChunks);

                            // Build reduce envelope.  User message shape:
                            //   [STNG + TASK]
                            //   [intro framing the partial answers]
                            //   --- partial 1 --- <reply1>
                            //   --- partial 2 --- <reply2>
                            //   ...
                            //   [reduce instruction]
                            //   [PROB]
                            std::string reduceBody;
                            auto const appendR = [&reduceBody](std::string const& s)
                            {
                                if (s.empty()) return;
                                if (!reduceBody.empty()) reduceBody += "\n";
                                reduceBody += s;
                            };
                            appendR(aggregator->m_StngTaskPrefix);
                            appendR("The full input was too large for the model's context window, so it "
                                    "was split into " + std::to_string(totalChunks) + " slices and the "
                                    "same question was asked of each slice separately.  Here are the "
                                    "resulting partial answers:");
                            for (size_t i = 0; i < aggregator->m_Parts.size(); ++i)
                            {
                                std::string const header = "\n--- partial answer " + std::to_string(i + 1) +
                                                           " of " + std::to_string(totalChunks) + " ---\n";
                                appendR(header + aggregator->m_Parts[i]);
                            }
                            appendR("Using ALL the partial answers above as evidence, produce a SINGLE "
                                    "unified response to the original request.  Do not repeat bullets or "
                                    "sections across partials; consolidate and deduplicate.  Do not "
                                    "mention that the input was chunked.  The original request follows:");
                            appendR(aggregator->m_ProbContent);

                            AiInvocation reduceEnvelope = aggregator->m_EnvelopeTemplate;
                            reduceEnvelope.m_Messages.clear();
                            reduceEnvelope.m_ChunkIndex.reset();
                            reduceEnvelope.m_ChunkCount.reset();
                            Message reduceMessage;
                            reduceMessage.m_Role = MessageRole::User;
                            reduceMessage.m_Content = std::move(reduceBody);
                            reduceEnvelope.m_Messages.push_back(std::move(reduceMessage));

                            // On successful submit, the reduce envelope's own Submit callback
                            // will write <prob>.output.txt and signal OnOutputFileCreated for us
                            // (because m_ChunkIndex is unset).  If Submit rejects the envelope
                            // synchronously (e.g. interface changed, key missing), fall back.
                            if (aggregator->m_RequestPool == nullptr ||
                                !aggregator->m_RequestPool->Submit(reduceEnvelope, nullptr))
                            {
                                writeConcatFallback(aggregator, totalChunks,
                                                    aggregator->m_RequestPool == nullptr
                                                        ? "request pool unavailable (shutdown?)"
                                                        : "reduce Submit rejected");
                                return;
                            }

                            LOG_APP_INFO("[ai_call] task '{}' reduce pass submitted", aggregator->m_TaskIdForLog);
                        };

                        if (!requestPool->Submit(chunkEnvelope, onReply))
                        {
                            allSubmitted = false;
                            break;
                        }
                    }

                    if (!allSubmitted)
                    {
                        requestPool->Forget(requestHandle);
                        taskState.m_State = TaskInstanceStateKind::Failed;
                        taskState.m_LastErrorMessage =
                            "ai_call chunked Submit rejected envelope (no interface, no API key, or empty body)";
                        return false;
                    }
                }
            }
            else
            {
                // No dispatchable content.  Fail the task immediately rather than
                // leaving it in WaitingExternal until the 120 s safety-net fires —
                // nothing is ever going to produce an output file for this task.
                requestPool->Forget(requestHandle);
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage =
                    "ai_call has no dispatchable prompt (STNG/TASK/CNTX/PROB combined body "
                    "is empty or whitespace-only, or no prob file was declared)";
                return false;
            }
        }

        // ------------------------------------------------------------
        // Asynchronous completion (event-driven)
        // ------------------------------------------------------------
        taskState.m_ExternalRequestId = requestId;
        taskState.m_ExternalRequestTimestampNs = timestampNs;

        taskState.m_State = TaskInstanceStateKind::WaitingExternal;
        taskState.m_LastErrorMessage.clear();

        return true;
    }
} // namespace AIAssistant
