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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    class FileWatcher;

    // ------------------------------------------------------------------------
    // TriggerEngine
    //
    // Responsible for:
    //  - Evaluating cron expressions on a periodic Tick().
    //  - Reacting to file events from its own dedicated FileWatcher.
    //  - Handling manual trigger requests from CLI / Web UI.
    //
    // File-watch triggers observe arbitrary declared paths via the engine's own
    // `FileWatcher` (primary root = empty, per-trigger paths added via AddPath).
    // Events reach `NotifyFileEvent` through the standard JarvisAgent::OnEvent
    // subscription on the global event queue.
    //
    // It does NOT parse JCWF JSON. The WorkflowJsonParser turns JSON into
    // high-level trigger definitions, and Orchestrator then registers those
    // triggers here.
    // ------------------------------------------------------------------------
    class TriggerEngine
    {
    public:
        // File events understood by file-watch triggers.
        enum class FileEventType
        {
            Created = 0,
            Modified,
            Deleted
        };

        // Fired when a trigger wants to start a workflow run.
        struct TriggerFiredEvent
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
        };

        struct WebhookTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_Secret; // HMAC-SHA256 shared secret (empty = open)
            bool m_IsEnabled{true};
        };

        using TriggerCallback = std::function<void(TriggerFiredEvent const&)>;

    public:
        explicit TriggerEngine(TriggerCallback const& triggerCallback);
        ~TriggerEngine();

        TriggerEngine(TriggerEngine const&) = delete;
        TriggerEngine& operator=(TriggerEngine const&) = delete;
        TriggerEngine(TriggerEngine&&) = delete;
        TriggerEngine& operator=(TriggerEngine&&) = delete;

        // --------------------------------------------------------------------
        // Registration API (called by Orchestrator after parsing JCWF)
        // --------------------------------------------------------------------

        // Register an auto trigger.
        // Auto triggers fire once immediately when registered (if enabled and fireImmediately is true).
        // Set fireImmediately=false when re-registering triggers after a reload to avoid re-running all auto workflows.
        void AddAutoTrigger(std::string const& workflowId, std::string const& triggerId, bool isEnabled,
                            bool fireImmediately = true);

        // Register a cron trigger.
        // expression: 5-field cron string (minute hour day month weekday).
        // timezone: IANA timezone name (e.g. "America/Los_Angeles"). Empty = system local time.
        // enabled: if false, trigger is stored but never fires.
        void AddCronTrigger(std::string const& workflowId, std::string const& triggerId, std::string const& expression,
                            std::string const& timezone, bool isEnabled);

        // Register a file-watch trigger.
        // path: file or directory path the trigger is interested in.
        // events: vector of FileEventType (created/modified/deleted).
        // debounceMilliseconds: minimum time between firings.
        void AddFileWatchTrigger(std::string const& workflowId, std::string const& triggerId, std::string const& path,
                                 std::vector<FileEventType> const& events, uint32_t debounceMilliseconds, bool isEnabled);

        // Register a manual trigger.
        void AddManualTrigger(std::string const& workflowId, std::string const& triggerId, bool isEnabled);

        // Register a webhook trigger.
        // secret: shared secret for HMAC-SHA256 verification. Mandatory — the validator
        // rejects webhook triggers with an empty secret and the request handler rejects
        // requests with missing or invalid signatures.
        void AddWebhookTrigger(std::string const& workflowId, std::string const& triggerId, std::string const& secret,
                               bool isEnabled);

        // Register an S3-watch trigger (polls an S3 bucket for new objects).
        // connectionName: named CloudConnection for S3 access.
        // bucket: S3 bucket to watch (empty = use connection default).
        // prefix: key prefix to filter (empty = watch entire bucket).
        // pollIntervalSeconds: polling interval (minimum 60).
        void AddS3WatchTrigger(std::string const& workflowId, std::string const& triggerId,
                               std::string const& connectionName, std::string const& bucket,
                               std::string const& prefix, uint32_t pollIntervalSeconds, bool isEnabled);

        // Register a OneDrive-watch trigger (polls a OneDrive folder via delta query).
        // connectionName: named CloudConnection for OneDrive access (OAuth2).
        // folder: OneDrive folder path to watch (e.g. "Documents/reports").
        // pollIntervalSeconds: polling interval (minimum 60).
        void AddOneDriveWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                     std::string const& connectionName, std::string const& folder,
                                     uint32_t pollIntervalSeconds, bool isEnabled);

        // Register an email-watch trigger (polls IMAP folder for new messages).
        // connectionName: named CloudConnection for email access (IMAP).
        // folder: IMAP folder to watch (default: "INBOX").
        // subjectFilter: only trigger on messages matching this subject pattern (empty = all).
        // pollIntervalSeconds: polling interval (minimum 60).
        void AddEmailWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                  std::string const& connectionName, std::string const& folder,
                                  std::string const& subjectFilter, uint32_t pollIntervalSeconds, bool isEnabled);

        // Register an Azure Blob-watch trigger (polls a container for new blobs).
        // connectionName: named CloudConnection for Azure Blob access.
        // container: blob container to watch (empty = use connection default).
        // prefix: blob name prefix to filter (empty = watch entire container).
        // pollIntervalSeconds: polling interval (minimum 60).
        void AddAzureBlobWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                      std::string const& connectionName, std::string const& container,
                                      std::string const& prefix, uint32_t pollIntervalSeconds, bool isEnabled);

        // Register a GCS-watch trigger (polls a bucket for new objects).
        // connectionName: named CloudConnection for GCS access.
        // bucket: GCS bucket to watch (empty = use connection default).
        // prefix: object name prefix to filter (empty = watch entire bucket).
        // pollIntervalSeconds: polling interval (minimum 60).
        void AddGcsWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                std::string const& connectionName, std::string const& bucket,
                                std::string const& prefix, uint32_t pollIntervalSeconds, bool isEnabled);

        // Remove all triggers associated with a workflow (for reload).
        void ClearWorkflowTriggers(std::string const& workflowId);

        // Remove all triggers (for full reload).
        void ClearAll();

        // --------------------------------------------------------------------
        // Runtime API
        // --------------------------------------------------------------------

        // Called periodically from the main loop (for cron evaluation).
        void Tick(std::chrono::system_clock::time_point const& now);

        // Called by the FileWatcher when the given path has changed.
        void NotifyFileEvent(std::string const& path, FileEventType fileEventType,
                             std::chrono::system_clock::time_point const& now);

        // Called by CLI / Web UI when the user explicitly wants to run
        // a manual trigger.
        void FireManualTrigger(std::string const& workflowId, std::string const& triggerId);

        // Look up a registered webhook trigger for a given workflowId.
        // Returns nullptr if no webhook trigger is registered for that workflow.
        WebhookTriggerInstance const* GetWebhookTrigger(std::string const& workflowId) const;

    private:
        // Simple cron expression: supports either "*" or a single integer
        // for each field: minute, hour, day-of-month, month, weekday.
        //
        // This is intentionally minimal and can be extended later.
        class CronExpression
        {
        public:
            CronExpression() = default;

            // Attempt to parse "m h dom mon dow".
            // On failure, returns false and leaves the expression invalid.
            bool Parse(std::string const& expression);

            // Compute the next fire time strictly after "referenceTime".
            // timezone: IANA timezone name (e.g. "America/Los_Angeles"). Empty = system local time.
            // If no time is found within a reasonable window, returns
            // referenceTime (caller will then treat it as disabled).
            std::chrono::system_clock::time_point
            ComputeNextFireTime(std::chrono::system_clock::time_point const& referenceTime,
                                std::string const& timezone = {}) const;

            bool IsValid() const;

        private:
            // Each field: std::optional<int>-like via bool + value.
            bool m_HasMinute{false};
            int m_Minute{0};

            bool m_HasHour{false};
            int m_Hour{0};

            bool m_HasDayOfMonth{false};
            int m_DayOfMonth{1};

            bool m_HasMonth{false};
            int m_Month{1};

            bool m_HasDayOfWeek{false};
            int m_DayOfWeek{0}; // 0 = Sunday, like std::tm::tm_wday

            bool m_IsValid{false};
        };

        struct CronTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_Timezone; // IANA timezone (empty = system local)
            CronExpression m_Expression;
            std::chrono::system_clock::time_point m_NextFireTime{};
            bool m_IsEnabled{true};
        };

        struct FileWatchTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_WatchedPath;
            std::vector<FileEventType> m_Events;
            std::chrono::milliseconds m_DebounceInterval{0};
            std::chrono::system_clock::time_point m_LastFireTime{};
            bool m_HasFiredOnce{false};
            bool m_IsEnabled{true};
        };

        struct ManualTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            bool m_IsEnabled{true};
        };

        struct S3WatchTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_ConnectionName;
            std::string m_Bucket;
            std::string m_Prefix;
            std::chrono::seconds m_PollInterval{300};
            std::chrono::steady_clock::time_point m_NextPollTime{};
            std::string m_LastSeenKey; // highest key from last poll (change detection)
            bool m_IsEnabled{true};
        };

        struct OneDriveWatchTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_ConnectionName;
            std::string m_Folder;
            std::chrono::seconds m_PollInterval{300};
            std::chrono::steady_clock::time_point m_NextPollTime{};
            std::string m_DeltaToken; // Graph API delta token for efficient polling
            bool m_IsEnabled{true};
        };

        struct EmailWatchTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_ConnectionName;
            std::string m_Folder;         // IMAP folder (default: "INBOX")
            std::string m_SubjectFilter;  // Subject pattern filter (empty = all)
            std::chrono::seconds m_PollInterval{300};
            std::chrono::steady_clock::time_point m_NextPollTime{};
            std::string m_LastSeenUid;    // Last processed message UID
            bool m_IsEnabled{true};
        };

        struct AzureBlobWatchTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_ConnectionName;
            std::string m_Container;
            std::string m_Prefix;
            std::chrono::seconds m_PollInterval{300};
            std::chrono::steady_clock::time_point m_NextPollTime{};
            std::string m_LastSeenTimestamp; // Last-Modified of newest blob from last poll
            bool m_IsEnabled{true};
        };

        struct GcsWatchTriggerInstance
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_ConnectionName;
            std::string m_Bucket;
            std::string m_Prefix;
            std::chrono::seconds m_PollInterval{300};
            std::chrono::steady_clock::time_point m_NextPollTime{};
            std::string m_LastSeenTimestamp; // 'updated' timestamp of newest object from last poll
            bool m_IsEnabled{true};
        };

    private:
        void FireTrigger(std::string const& workflowId, std::string const& triggerId) const;

        static bool ContainsEvent(std::vector<FileEventType> const& events, FileEventType fileEventType);

        // Helper: erase-remove for workflowId from a vector.
        template <typename TriggerVector>
        void EraseWorkflowFromVector(TriggerVector& triggerVector, std::string const& workflowId);

        static std::string NormalizePath(std::string const& path);
        static bool IsPathMatch(std::string const& watchedPath, std::string const& eventPath);

    private:
        mutable std::mutex m_Mutex;
        TriggerCallback m_TriggerCallback;

        // Dedicated watcher for file_watch triggers.  Primary root is empty; each trigger's
        // path is registered via AddPath() on bind and RemovePath() on unbind.
        std::unique_ptr<FileWatcher> m_TriggerFileWatcher;

        std::vector<CronTriggerInstance> m_CronTriggers;
        std::vector<FileWatchTriggerInstance> m_FileWatchTriggers;
        std::vector<ManualTriggerInstance> m_ManualTriggers;
        std::vector<WebhookTriggerInstance> m_WebhookTriggers;
        std::vector<S3WatchTriggerInstance> m_S3WatchTriggers;
        std::vector<OneDriveWatchTriggerInstance> m_OneDriveWatchTriggers;
        std::vector<EmailWatchTriggerInstance> m_EmailWatchTriggers;
        std::vector<AzureBlobWatchTriggerInstance> m_AzureBlobWatchTriggers;
        std::vector<GcsWatchTriggerInstance> m_GcsWatchTriggers;

        // Optional acceleration structure for file-trigger lookups:
        // map path → indices into m_FileWatchTriggers.
        std::unordered_map<std::string, std::vector<size_t>> m_FileWatchIndex;

        // Acceleration structure for webhook lookups: workflowId → index into m_WebhookTriggers.
        std::unordered_map<std::string, size_t> m_WebhookIndex;
    };
} // namespace AIAssistant
