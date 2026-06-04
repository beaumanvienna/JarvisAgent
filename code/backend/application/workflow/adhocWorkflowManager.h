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

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

namespace AIAssistant
{
    class McpKeyManager;
    class WorkflowRegistry;

    // Staging, execution tracking, disk-quota bookkeeping, and reaper for
    // "adhoc" workflow runs — one-shot JCWFs submitted via
    // POST /api/workflows/run-adhoc.
    //
    // Each run lives in its own self-contained folder, nested under a
    // per-user slug so authorisation for artifact retrieval can be enforced
    // at the filesystem level as well as in the route handlers:
    //
    //   _adhoc/<user_slug>/<ISO8601>_<counter>_del-<ISO8601>/
    //     workflows/
    //       _adhoc_<ISO8601>_<counter>.jcwf          (zip container, built by WorkflowRegistry)
    //       _adhoc_<ISO8601>_<counter>/              (extracted — runtime reads from here)
    //         global.json
    //         _adhoc_<ISO8601>_<counter>.json        (canvas)
    //     queue/                                     (per-task outputs)
    //     meta.json                                  (user, role, policy, owner_slug — restart-safe attribution)
    //     manifest.json                              (output inventory written at completion)
    //
    // The folder name encodes the delete-at instant so the reaper is stateless.
    // `del-retain` means "never auto-delete"; `del-<ISO8601>` is a concrete UTC
    // target. For `on_completion`, OnRunCompleted() removes the folder inline.
    //
    // Legacy top-level run folders (from pre-namespace builds) are ignored by
    // Init()'s attribution scan; they'll be swept by the reaper once their TTL
    // expires so no migration is required.
    class AdhocWorkflowManager
    {
    public:
        // Direct envelope dispatch writes each run's .output.* files itself, so the adhoc
        // manager no longer needs to register/unregister queue folders with a FileWatcher.
        AdhocWorkflowManager(McpKeyManager& keyManager, WorkflowRegistry& registry);
        ~AdhocWorkflowManager();

        AdhocWorkflowManager(AdhocWorkflowManager const&) = delete;
        AdhocWorkflowManager& operator=(AdhocWorkflowManager const&) = delete;

        // Initialise with the adhoc base folder (typically <cwd>/_adhoc). Scans
        // existing folders to restore counter floor + per-user disk usage.
        void Init(std::filesystem::path const& adhocBaseFolder);

        // Submission payload — one staged run per call.
        struct StageRequest
        {
            std::string m_JcwfJson;                  // submitted canvas JSON
            std::string m_User;                      // from MCP key
            std::string m_Role;                      // from MCP key (for audit)
            std::string m_CleanupPolicy;             // "on_completion"|"ttl_1h"|"ttl_24h"|"ttl_48h"|"ttl_72h"|"retain"
            int m_DiskQuotaMb{1024};                 // quota from MCP key record
        };

        struct StageResult
        {
            std::string m_RunId;                     // e.g. "adhoc_20260416T143022_0001"
            std::string m_WorkflowId;                // e.g. "_adhoc_20260416T143022_0001"
            std::filesystem::path m_FolderPath;      // the per-run folder inside _adhoc/
        };

        // Stage the run: generate folder name, write JCWF, register with
        // WorkflowRegistry. Returns error string on failure.
        std::variant<StageResult, std::string> Stage(StageRequest const& req);

        // Called by the workflow runtime after a run reaches a terminal state.
        // For `on_completion` policy, removes the folder immediately.
        void OnRunCompleted(std::string const& runId);

        // Reaper entry points — StartReaperThread() spawns a background thread
        // that ticks every 60 s and calls ReapOnce() itself. Call Reap() directly
        // from tests or from a tick-driven loop.
        void Reap();
        void StartReaperThread();
        void StopReaperThread();

        // Quota queries.
        size_t GetUserDiskUsageBytes(std::string const& user) const;
        [[nodiscard]] bool WouldExceedQuota(std::string const& user, int diskQuotaMb, size_t additionalBytes) const;

        // Accessors for tests and status endpoints.
        size_t GetActiveRunCount() const;
        size_t GetTotalDiskUsageBytes() const;

        // Snapshot of the persistent attribution for a run — used by the
        // artifact-retrieval endpoints to enforce ownership and locate the
        // folder to serve from.
        struct RunInfo
        {
            std::string m_User;
            std::string m_Role;
            std::string m_OwnerSlug;
            std::string m_CleanupPolicy;
            std::filesystem::path m_FolderPath;
        };
        std::optional<RunInfo> GetRunInfo(std::string const& runId) const;

        // Pure function — pulled into the public API so unit tests can exercise it
        // without spinning up the full manager. Maps an MCP user string (e.g.
        // "alice@company.com") to a filesystem-safe slug used as the first
        // directory segment under _adhoc/. Shape: `<body>_<8 hex chars of
        // SHA-256(original_user)>`. Body: allowed characters [A-Za-z0-9._@-];
        // everything else collapsed to '_'; body capped at 55 bytes so the
        // total slug stays ≤ 64. The hash is over the ORIGINAL user (pre-
        // collapse) so distinct users that collapse to the same body still
        // get distinct slugs. Authorisation runs on the user identity, not
        // this slug — the slug is purely for filesystem naming.
        static std::string SanitizeUserSlug(std::string const& user);

    private:
        struct RunMeta
        {
            std::string m_User;
            std::string m_Role;
            std::string m_CleanupPolicy;
            std::string m_OwnerSlug;
            std::filesystem::path m_FolderPath;
            size_t m_DiskBytes{0};
        };

        static std::string Iso8601NowCompactUtc();
        static std::optional<std::chrono::system_clock::time_point>
            ParseDeleteAtFromFolderName(std::string const& folderName);
        static std::string ComputeDeleteAtForPolicy(std::string const& cleanupPolicy);

        std::string GenerateFolderName(std::string const& cleanupPolicy);
        std::string RewriteWorkflowId(std::string const& jcwfJson, std::string const& newId) const;
        size_t MeasureFolderBytes(std::filesystem::path const& folder) const;
        // Returns true on success, false if the meta file couldn't be opened
        // (already logged inside).  Callers in Stage roll back the partial
        // folder if this fails — meta.json is the artifact-attribution gate
        // and a missing one breaks downstream ownership checks.
        [[nodiscard]] bool WriteMeta(std::filesystem::path const& folder, RunMeta const& meta) const;
        std::optional<RunMeta> ReadMeta(std::filesystem::path const& folder) const;
        // Writes manifest.json describing the run's outputs — called from
        // OnRunCompleted so downstream listing endpoints can serve the same
        // snapshot without re-walking the folder on every request.
        void WriteManifest(std::filesystem::path const& folder,
                           RunMeta const& meta,
                           std::string const& runId) const;
        void RemoveFolder(std::filesystem::path const& folder);
        void ReaperLoop();

        McpKeyManager& m_KeyManager;
        WorkflowRegistry& m_Registry;

        mutable std::mutex m_Mutex;
        std::filesystem::path m_BasePath;
        std::atomic<uint32_t> m_Counter{0};
        std::unordered_map<std::string, RunMeta> m_RunIdToMeta;
        std::unordered_map<std::string, size_t> m_DiskUsageByUser;

        // Reaper-thread lifecycle.  m_ReaperRunning is the externally-visible
        // run flag; m_ReaperThread is the std::thread handle.  Both are
        // accessed from Start/StopReaperThread + the destructor — concurrent
        // Start/Stop on the same instance would race on the std::thread
        // assignment (atomic doesn't make a non-atomic member safe).  Both
        // are guarded by m_ReaperLifecycleMutex.  Inside ReaperLoop the
        // m_ReaperCv + m_ReaperCvMutex pair lets Stop() wake the loop
        // immediately instead of sleeping out the 60 s interval.  The CV uses
        // its OWN mutex (not the manager-wide m_Mutex shared with
        // Reap/Stage/OnRunCompleted) so a Stop notify never has to wait behind
        // a mid-flight Reap directory-walk that holds m_Mutex.  The predicate
        // reads only the atomic m_ReaperRunning, so the CV mutex guards just
        // the wait/notify handshake.
        std::thread m_ReaperThread;
        mutable std::mutex m_ReaperLifecycleMutex;
        std::atomic<bool> m_ReaperRunning{false};
        std::condition_variable m_ReaperCv;
        mutable std::mutex m_ReaperCvMutex;
    };
} // namespace AIAssistant
