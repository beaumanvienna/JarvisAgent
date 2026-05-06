/* Copyright (c) 2026 JC Technolabs
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

#include "workflow/adhocWorkflowManager.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

#include "engine.h"
#include "file/fileWatcher.h"
#include "json/jsonHelper.h"
#include "web/mcpKeyManager.h"
#include "workflow/workflowRegistry.h"

namespace AIAssistant
{
    namespace
    {
        constexpr char const* kMetaFileName = "meta.json";
        constexpr char const* kManifestFileName = "manifest.json";
        constexpr char const* kWorkflowIdPrefix = "_adhoc_";
        constexpr size_t kMaxUserSlugLength = 64;

        std::string FormatIsoCompact(std::chrono::system_clock::time_point tp)
        {
            std::time_t t = std::chrono::system_clock::to_time_t(tp);
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &t);
#else
            gmtime_r(&t, &utc);
#endif
            std::ostringstream oss;
            oss << std::put_time(&utc, "%Y%m%dT%H%M%S");
            return oss.str();
        }

        std::optional<std::chrono::system_clock::time_point> ParseIsoCompact(std::string const& s)
        {
            if (s.size() < 15) return std::nullopt; // YYYYMMDDTHHMMSS
            std::tm tm{};
            std::istringstream iss(s);
            iss >> std::get_time(&tm, "%Y%m%dT%H%M%S");
            if (iss.fail()) return std::nullopt;
#ifdef _WIN32
            std::time_t t = _mkgmtime(&tm);
#else
            std::time_t t = timegm(&tm);
#endif
            if (t == static_cast<std::time_t>(-1)) return std::nullopt;
            return std::chrono::system_clock::from_time_t(t);
        }

        std::chrono::seconds PolicyTtl(std::string const& policy)
        {
            if (policy == "ttl_1h")  return std::chrono::hours(1);
            if (policy == "ttl_24h") return std::chrono::hours(24);
            if (policy == "ttl_48h") return std::chrono::hours(48);
            if (policy == "ttl_72h") return std::chrono::hours(72);
            // on_completion and retain are handled separately
            return std::chrono::hours(72);
        }
    } // namespace

    AdhocWorkflowManager::AdhocWorkflowManager(McpKeyManager& keyManager, WorkflowRegistry& registry)
        : m_KeyManager(keyManager), m_Registry(registry)
    {
    }

    AdhocWorkflowManager::~AdhocWorkflowManager() { StopReaperThread(); }

    std::string AdhocWorkflowManager::SanitizeUserSlug(std::string const& user)
    {
        std::string slug;
        slug.reserve(std::min(user.size(), kMaxUserSlugLength));
        for (char ch : user)
        {
            if (slug.size() >= kMaxUserSlugLength) break;
            bool const allowed = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                 (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                                 ch == '@' || ch == '-';
            slug.push_back(allowed ? ch : '_');
        }
        return slug;
    }

    std::string AdhocWorkflowManager::Iso8601NowCompactUtc()
    {
        return FormatIsoCompact(std::chrono::system_clock::now());
    }

    std::optional<std::chrono::system_clock::time_point>
    AdhocWorkflowManager::ParseDeleteAtFromFolderName(std::string const& folderName)
    {
        auto pos = folderName.rfind("_del-");
        if (pos == std::string::npos) return std::nullopt;
        std::string tail = folderName.substr(pos + std::string("_del-").size());
        if (tail == "retain")
        {
            // "infinity" — no deletion.
            return std::chrono::system_clock::time_point::max();
        }
        return ParseIsoCompact(tail);
    }

    std::string AdhocWorkflowManager::ComputeDeleteAtForPolicy(std::string const& cleanupPolicy)
    {
        if (cleanupPolicy == "retain") return "retain";
        if (cleanupPolicy == "on_completion")
        {
            // Placeholder; OnRunCompleted() removes the folder immediately regardless.
            return "on_completion";
        }
        auto const delta = PolicyTtl(cleanupPolicy);
        return FormatIsoCompact(std::chrono::system_clock::now() + delta);
    }

    void AdhocWorkflowManager::Init(std::filesystem::path const& adhocBaseFolder)
    {
        std::lock_guard lock(m_Mutex);
        m_BasePath = adhocBaseFolder;

        std::error_code ec;
        std::filesystem::create_directories(m_BasePath, ec);
        if (ec)
        {
            LOG_APP_ERROR("[adhoc] Failed to create base folder '{}': {}", m_BasePath.string(), ec.message());
            return;
        }

        // Scan existing folders to restore counter floor and disk-usage attribution.
        // New layout: _adhoc/<user_slug>/<ts>_<counter>_del-<ts>/.
        // Legacy top-level folders (pre-namespace builds) are intentionally skipped —
        // they'll be swept by the reaper once their TTL expires.
        uint32_t maxCounter = 0;
        size_t legacySkipped = 0;
        auto const looksLikeRunFolder = [](std::string const& name) {
            return name.find("_del-") != std::string::npos;
        };

        auto scanRunFolder = [&](std::filesystem::path const& runFolder) {
            std::string const name = runFolder.filename().string();
            auto firstUnderscore = name.find('_');
            if (firstUnderscore == std::string::npos) return;
            auto secondUnderscore = name.find('_', firstUnderscore + 1);
            if (secondUnderscore == std::string::npos) return;

            try
            {
                uint32_t const counter = static_cast<uint32_t>(std::stoul(
                    name.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1)));
                maxCounter = std::max(maxCounter, counter);
            }
            catch (...)
            {
                return;
            }

            auto meta = ReadMeta(runFolder);
            if (!meta) return;
            meta->m_DiskBytes = MeasureFolderBytes(runFolder);
            m_DiskUsageByUser[meta->m_User] += meta->m_DiskBytes;

            std::string const runIdStem = name.substr(0, name.find("_del-"));
            std::string const runId = "adhoc_" + runIdStem;
            m_RunIdToMeta[runId] = *meta;

        };

        for (auto const& l1Entry : std::filesystem::directory_iterator(m_BasePath, ec))
        {
            if (!l1Entry.is_directory()) continue;
            std::string const l1Name = l1Entry.path().filename().string();

            if (looksLikeRunFolder(l1Name))
            {
                // Legacy top-level run folder — skip attribution; reaper will handle TTL.
                ++legacySkipped;
                continue;
            }

            // l1 is a user-slug directory — scan its children for run folders.
            std::error_code inner;
            for (auto const& runEntry : std::filesystem::directory_iterator(l1Entry.path(), inner))
            {
                if (!runEntry.is_directory()) continue;
                if (!looksLikeRunFolder(runEntry.path().filename().string())) continue;
                scanRunFolder(runEntry.path());
            }
        }

        m_Counter.store(maxCounter + 1);
        LOG_APP_INFO("[adhoc] Init: base='{}', scanned {} run(s) across user namespaces, "
                     "skipped {} legacy top-level folder(s), counter_floor={}",
                     m_BasePath.string(), m_RunIdToMeta.size(), legacySkipped, maxCounter + 1);
    }

    std::string AdhocWorkflowManager::GenerateFolderName(std::string const& cleanupPolicy)
    {
        std::string const ts = Iso8601NowCompactUtc();
        uint32_t const counter = m_Counter.fetch_add(1);
        char counterBuf[8];
        std::snprintf(counterBuf, sizeof(counterBuf), "%04u", counter);
        std::string const deleteAt = ComputeDeleteAtForPolicy(cleanupPolicy);
        return ts + "_" + counterBuf + "_del-" + deleteAt;
    }

    std::string AdhocWorkflowManager::RewriteWorkflowId(std::string const& jcwfJson,
                                                        std::string const& newId) const
    {
        // Replace the first occurrence of the top-level "id" field value with `newId`.
        // Canvas JSON always has "id" near the top; we only rewrite the first match so
        // sub-workflow nested ids stay untouched.
        std::regex const idPattern(R"("id"\s*:\s*"[^"]*")");
        return std::regex_replace(jcwfJson, idPattern,
                                  std::string("\"id\": \"") + newId + "\"",
                                  std::regex_constants::format_first_only);
    }

    size_t AdhocWorkflowManager::MeasureFolderBytes(std::filesystem::path const& folder) const
    {
        std::error_code ec;
        size_t total = 0;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(folder, ec))
        {
            if (ec) break;
            if (entry.is_regular_file())
            {
                auto size = entry.file_size(ec);
                if (!ec) total += size;
            }
        }
        return total;
    }

    void AdhocWorkflowManager::WriteMeta(std::filesystem::path const& folder, RunMeta const& meta) const
    {
        std::ofstream os(folder / kMetaFileName, std::ios::binary | std::ios::trunc);
        if (!os) return;
        // Minimal hand-built JSON; fields are validated server-side so no escaping
        // pass is needed. owner_slug is the filesystem-safe derivative of user.
        os << "{\n"
           << "  \"user\": \"" << JsonHelper::EscapeJsonString(meta.m_User) << "\",\n"
           << "  \"role\": \"" << JsonHelper::EscapeJsonString(meta.m_Role) << "\",\n"
           << "  \"cleanup_policy\": \"" << JsonHelper::EscapeJsonString(meta.m_CleanupPolicy) << "\",\n"
           << "  \"owner_slug\": \"" << JsonHelper::EscapeJsonString(meta.m_OwnerSlug) << "\"\n"
           << "}\n";
    }

    std::optional<AdhocWorkflowManager::RunMeta>
    AdhocWorkflowManager::ReadMeta(std::filesystem::path const& folder) const
    {
        std::ifstream is(folder / kMetaFileName);
        if (!is) return std::nullopt;
        std::stringstream ss;
        ss << is.rdbuf();
        std::string const content = ss.str();

        // Simple extract of "field": "value" — the file is written by us in a known shape.
        auto pluck = [&](char const* field) -> std::string {
            std::string needle = std::string("\"") + field + "\"";
            auto k = content.find(needle);
            if (k == std::string::npos) return {};
            auto colon = content.find(':', k);
            if (colon == std::string::npos) return {};
            auto quoteOpen = content.find('"', colon);
            if (quoteOpen == std::string::npos) return {};
            auto quoteClose = content.find('"', quoteOpen + 1);
            if (quoteClose == std::string::npos) return {};
            return content.substr(quoteOpen + 1, quoteClose - quoteOpen - 1);
        };

        RunMeta meta;
        meta.m_User = pluck("user");
        meta.m_Role = pluck("role");
        meta.m_CleanupPolicy = pluck("cleanup_policy");
        meta.m_OwnerSlug = pluck("owner_slug");
        // Backfill owner_slug for meta files written by earlier builds — keeps
        // existing runs readable without a migration step.
        if (meta.m_OwnerSlug.empty() && !meta.m_User.empty())
        {
            meta.m_OwnerSlug = SanitizeUserSlug(meta.m_User);
        }
        meta.m_FolderPath = folder;
        if (meta.m_User.empty()) return std::nullopt;
        return meta;
    }

    void AdhocWorkflowManager::RemoveFolder(std::filesystem::path const& folder)
    {
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        if (ec)
        {
            LOG_APP_ERROR("[adhoc] Failed to remove '{}': {}", folder.string(), ec.message());
        }
    }

    std::variant<AdhocWorkflowManager::StageResult, std::string>
    AdhocWorkflowManager::Stage(StageRequest const& req)
    {
        if (m_BasePath.empty())
        {
            return std::string("adhoc manager not initialised");
        }
        if (req.m_User.empty()) return std::string("user is required");

        // Policy validation happens on the REST handler side; default if empty.
        std::string const policy = req.m_CleanupPolicy.empty() ? std::string("ttl_72h") : req.m_CleanupPolicy;

        std::string const userSlug = SanitizeUserSlug(req.m_User);
        if (userSlug.empty())
        {
            return std::string("invalid_user_for_staging");
        }

        // Disk quota — rough pre-check against the JCWF source size. The post-stage
        // measurement (file on disk + extracted dir + task folders) will be more
        // accurate; the accounting map is updated once staging completes.
        if (WouldExceedQuota(req.m_User, req.m_DiskQuotaMb, req.m_JcwfJson.size() * 2))
        {
            return std::string("quota_exceeded");
        }

        std::lock_guard lock(m_Mutex);

        std::string const folderName = GenerateFolderName(policy);
        std::filesystem::path const folder = m_BasePath / userSlug / folderName;

        std::error_code ec;
        std::filesystem::create_directories(folder / "workflows", ec);
        if (ec) return std::string("failed to create folder: ") + ec.message();
        std::filesystem::create_directories(folder / "queue", ec);
        if (ec) return std::string("failed to create queue folder: ") + ec.message();

        // Workflow id is the folder stem (prefixed with "_adhoc_"): "<ts>_<counter>".
        std::string const folderStem = folderName.substr(0, folderName.find("_del-"));
        std::string const workflowId = kWorkflowIdPrefix + folderStem;
        std::string const runId = "adhoc_" + folderStem;

        // Rewrite the JCWF's top-level id so WorkflowRegistry keys it under our adhoc id.
        std::string const rewritten = RewriteWorkflowId(req.m_JcwfJson, workflowId);

        std::filesystem::path const jcwfPath = folder / "workflows" / (workflowId + ".jcwf");
        std::string saveErr;
        if (!m_Registry.SaveOrUpdateWorkflowFromJson(rewritten, jcwfPath, saveErr))
        {
            RemoveFolder(folder);
            return std::string("jcwf save failed: ") + saveErr;
        }

        RunMeta meta;
        meta.m_User = req.m_User;
        meta.m_Role = req.m_Role;
        meta.m_CleanupPolicy = policy;
        meta.m_OwnerSlug = userSlug;
        meta.m_FolderPath = folder;
        meta.m_DiskBytes = MeasureFolderBytes(folder);

        WriteMeta(folder, meta);
        m_RunIdToMeta[runId] = meta;
        m_DiskUsageByUser[req.m_User] += meta.m_DiskBytes;
        m_KeyManager.RecordDiskUsage(req.m_User, m_DiskUsageByUser[req.m_User]);

        LOG_APP_INFO("[adhoc] Staged runId={} workflowId={} user={} policy={} folder='{}' bytes={}",
                     runId, workflowId, req.m_User, policy, folder.string(), meta.m_DiskBytes);

        StageResult result;
        result.m_RunId = runId;
        result.m_WorkflowId = workflowId;
        result.m_FolderPath = folder;
        return result;
    }

    void AdhocWorkflowManager::OnRunCompleted(std::string const& runId)
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_RunIdToMeta.find(runId);
        if (it == m_RunIdToMeta.end()) return;

        // Recompute bytes at completion time — task outputs have accumulated.
        size_t const finalBytes = MeasureFolderBytes(it->second.m_FolderPath);
        auto& userTotal = m_DiskUsageByUser[it->second.m_User];
        userTotal = userTotal >= it->second.m_DiskBytes
                        ? userTotal - it->second.m_DiskBytes + finalBytes
                        : finalBytes;
        it->second.m_DiskBytes = finalBytes;
        m_KeyManager.RecordDiskUsage(it->second.m_User, userTotal);

        if (it->second.m_CleanupPolicy == "on_completion")
        {
            LOG_APP_INFO("[adhoc] on_completion cleanup for runId={}", runId);
            std::filesystem::path const parentDir = it->second.m_FolderPath.parent_path();
            RemoveFolder(it->second.m_FolderPath);
            userTotal = userTotal >= finalBytes ? userTotal - finalBytes : 0;
            m_KeyManager.RecordDiskUsage(it->second.m_User, userTotal);
            m_RunIdToMeta.erase(it);

            // Prune the user-slug parent folder if it's now empty — see Reap()
            // for rationale (keep _adhoc/ from accumulating stray namespace dirs).
            if (!parentDir.empty() && parentDir != m_BasePath)
            {
                std::error_code ec;
                if (std::filesystem::is_empty(parentDir, ec) && !ec)
                {
                    std::filesystem::remove(parentDir, ec);
                }
            }
            return;
        }

        // Retention keeps the folder; write a manifest so the discovery/download
        // endpoints can serve a consistent snapshot without re-walking the tree
        // on every request.
        WriteManifest(it->second.m_FolderPath, it->second, runId);

        LOG_APP_INFO("[adhoc] runId={} done, retention policy '{}' — folder retained",
                     runId, it->second.m_CleanupPolicy);
    }

    void AdhocWorkflowManager::WriteManifest(std::filesystem::path const& folder,
                                             RunMeta const& meta,
                                             std::string const& runId) const
    {
        // Structured output inventory — paths are relative to the run folder.
        // We intentionally skip meta.json and manifest.json itself so the
        // manifest only lists task-visible files.
        std::error_code ec;
        struct FileEntry
        {
            std::string m_RelPath;
            size_t m_SizeBytes;
            std::string m_ModifiedIso;
        };
        std::vector<FileEntry> entries;
        for (auto const& e : std::filesystem::recursive_directory_iterator(folder, ec))
        {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            std::string const name = e.path().filename().string();
            if (name == kMetaFileName || name == kManifestFileName) continue;

            FileEntry fe;
            auto rel = std::filesystem::relative(e.path(), folder, ec);
            if (ec) continue;
            fe.m_RelPath = rel.generic_string();
            fe.m_SizeBytes = e.file_size(ec);
            if (ec) fe.m_SizeBytes = 0;

            // mtime → ISO8601 UTC (best-effort).
            auto ftime = std::filesystem::last_write_time(e.path(), ec);
            if (!ec)
            {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                std::time_t t = std::chrono::system_clock::to_time_t(sctp);
                std::tm utc{};
#ifdef _WIN32
                gmtime_s(&utc, &t);
#else
                gmtime_r(&t, &utc);
#endif
                std::ostringstream iso;
                iso << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
                fe.m_ModifiedIso = iso.str();
            }
            entries.push_back(std::move(fe));
        }

        // Deterministic ordering keeps diffs between manifests readable.
        std::sort(entries.begin(), entries.end(),
                  [](auto const& a, auto const& b) { return a.m_RelPath < b.m_RelPath; });

        std::ofstream os(folder / kManifestFileName, std::ios::binary | std::ios::trunc);
        if (!os) return;

        // delete_at is encoded in the folder name — re-derive for the manifest.
        std::string deleteAtStr;
        {
            auto deleteAt = ParseDeleteAtFromFolderName(folder.filename().string());
            if (deleteAt)
            {
                if (*deleteAt == std::chrono::system_clock::time_point::max())
                {
                    deleteAtStr = "retain";
                }
                else
                {
                    std::time_t t = std::chrono::system_clock::to_time_t(*deleteAt);
                    std::tm utc{};
#ifdef _WIN32
                    gmtime_s(&utc, &t);
#else
                    gmtime_r(&t, &utc);
#endif
                    std::ostringstream iso;
                    iso << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
                    deleteAtStr = iso.str();
                }
            }
        }

        os << "{\n"
           << "  \"runId\": \"" << JsonHelper::EscapeJsonString(runId) << "\",\n"
           << "  \"owner\": \"" << JsonHelper::EscapeJsonString(meta.m_User) << "\",\n"
           << "  \"owner_slug\": \"" << JsonHelper::EscapeJsonString(meta.m_OwnerSlug) << "\",\n"
           << "  \"cleanup_policy\": \"" << JsonHelper::EscapeJsonString(meta.m_CleanupPolicy) << "\",\n"
           << "  \"delete_at\": \"" << JsonHelper::EscapeJsonString(deleteAtStr) << "\",\n"
           << "  \"files\": [";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i > 0) os << ",";
            os << "\n    {"
               << "\"path\": \"" << JsonHelper::EscapeJsonString(entries[i].m_RelPath) << "\", "
               << "\"size_bytes\": " << entries[i].m_SizeBytes << ", "
               << "\"modified_at\": \"" << JsonHelper::EscapeJsonString(entries[i].m_ModifiedIso) << "\""
               << "}";
        }
        if (!entries.empty()) os << "\n  ";
        os << "]\n}\n";
    }

    void AdhocWorkflowManager::Reap()
    {
        if (m_BasePath.empty()) return;

        auto const now = std::chrono::system_clock::now();
        std::vector<std::filesystem::path> toDelete;

        auto const folderIsRun = [](std::filesystem::path const& p) {
            return p.filename().string().find("_del-") != std::string::npos;
        };
        auto const considerForReap = [&](std::filesystem::path const& runFolder) {
            auto deleteAt = ParseDeleteAtFromFolderName(runFolder.filename().string());
            if (!deleteAt) return;
            if (*deleteAt == std::chrono::system_clock::time_point::max()) return; // retain
            if (now >= *deleteAt)
            {
                toDelete.push_back(runFolder);
            }
        };

        {
            std::lock_guard lock(m_Mutex);
            std::error_code ec;
            for (auto const& l1 : std::filesystem::directory_iterator(m_BasePath, ec))
            {
                if (!l1.is_directory()) continue;
                if (folderIsRun(l1.path()))
                {
                    // Legacy top-level run folder — reap on its own timestamp.
                    considerForReap(l1.path());
                    continue;
                }

                // User-slug directory — descend one level.
                std::error_code inner;
                for (auto const& runEntry : std::filesystem::directory_iterator(l1.path(), inner))
                {
                    if (!runEntry.is_directory()) continue;
                    if (!folderIsRun(runEntry.path())) continue;
                    considerForReap(runEntry.path());
                }
            }
        }

        for (auto const& folder : toDelete)
        {
            LOG_APP_INFO("[adhoc] Reaper removing '{}' (past delete-at)", folder.string());
            // Best-effort disk usage cleanup: read meta.json before rm-rf so we can
            // decrement the attribution for the user.
            auto meta = ReadMeta(folder);
            size_t const bytes = MeasureFolderBytes(folder);
            std::filesystem::path const parentDir = folder.parent_path();
            RemoveFolder(folder);

            if (meta)
            {
                std::lock_guard lock(m_Mutex);
                auto& userTotal = m_DiskUsageByUser[meta->m_User];
                userTotal = userTotal >= bytes ? userTotal - bytes : 0;
                m_KeyManager.RecordDiskUsage(meta->m_User, userTotal);

                std::string const folderName = folder.filename().string();
                std::string const runIdStem = folderName.substr(0, folderName.find("_del-"));
                m_RunIdToMeta.erase("adhoc_" + runIdStem);
            }

            // If the user-slug parent directory is now empty, remove it so the
            // _adhoc/ root doesn't accumulate orphan namespace folders. Guarded
            // so we never try to remove m_BasePath itself (legacy top-level runs
            // live directly at m_BasePath — their parent_path IS m_BasePath).
            if (parentDir != m_BasePath)
            {
                std::error_code ec;
                if (std::filesystem::is_empty(parentDir, ec) && !ec)
                {
                    std::filesystem::remove(parentDir, ec);
                }
            }
        }
    }

    void AdhocWorkflowManager::StartReaperThread()
    {
        if (m_ReaperRunning.exchange(true)) return; // already running
        m_ReaperThread = std::thread(&AdhocWorkflowManager::ReaperLoop, this);
    }

    void AdhocWorkflowManager::StopReaperThread()
    {
        if (!m_ReaperRunning.exchange(false)) return;
        if (m_ReaperThread.joinable()) m_ReaperThread.join();
    }

    void AdhocWorkflowManager::ReaperLoop()
    {
        while (m_ReaperRunning.load())
        {
            // Sleep in small chunks so Stop() doesn't have to wait 60 s.
            for (int i = 0; i < 60 && m_ReaperRunning.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!m_ReaperRunning.load()) break;
            try
            {
                Reap();
            }
            catch (std::exception const& e)
            {
                LOG_APP_ERROR("[adhoc] Reaper exception: {}", e.what());
            }
        }
    }

    size_t AdhocWorkflowManager::GetUserDiskUsageBytes(std::string const& user) const
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_DiskUsageByUser.find(user);
        return it == m_DiskUsageByUser.end() ? 0 : it->second;
    }

    bool AdhocWorkflowManager::WouldExceedQuota(std::string const& user, int diskQuotaMb,
                                                size_t additionalBytes) const
    {
        if (diskQuotaMb <= 0) return false;
        size_t const limit = static_cast<size_t>(diskQuotaMb) * 1024 * 1024;
        size_t const current = GetUserDiskUsageBytes(user);
        return (current + additionalBytes) > limit;
    }

    size_t AdhocWorkflowManager::GetActiveRunCount() const
    {
        std::lock_guard lock(m_Mutex);
        return m_RunIdToMeta.size();
    }

    std::optional<AdhocWorkflowManager::RunInfo>
    AdhocWorkflowManager::GetRunInfo(std::string const& runId) const
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_RunIdToMeta.find(runId);
        if (it == m_RunIdToMeta.end()) return std::nullopt;
        RunInfo info;
        info.m_User = it->second.m_User;
        info.m_Role = it->second.m_Role;
        info.m_OwnerSlug = it->second.m_OwnerSlug;
        info.m_CleanupPolicy = it->second.m_CleanupPolicy;
        info.m_FolderPath = it->second.m_FolderPath;
        return info;
    }

    size_t AdhocWorkflowManager::GetTotalDiskUsageBytes() const
    {
        std::lock_guard lock(m_Mutex);
        size_t total = 0;
        for (auto const& [_, bytes] : m_DiskUsageByUser) total += bytes;
        return total;
    }
} // namespace AIAssistant
