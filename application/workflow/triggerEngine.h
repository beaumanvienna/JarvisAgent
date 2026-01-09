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
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    // ------------------------------------------------------------------------
    // TriggerEngine
    //
    // Responsible for:
    //  - Evaluating cron expressions on a periodic Tick().
    //  - Reacting to file events from FileWatcher.
    //  - Handling manual trigger requests from CLI / Web UI.
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

        using TriggerCallback = std::function<void(TriggerFiredEvent const&)>;

    public:
        explicit TriggerEngine(TriggerCallback const& triggerCallback);
        ~TriggerEngine() = default;

        TriggerEngine(TriggerEngine const&) = delete;
        TriggerEngine& operator=(TriggerEngine const&) = delete;
        TriggerEngine(TriggerEngine&&) = delete;
        TriggerEngine& operator=(TriggerEngine&&) = delete;

        // --------------------------------------------------------------------
        // Registration API (called by Orchestrator after parsing JCWF)
        // --------------------------------------------------------------------

        // Register an auto trigger.
        // Auto triggers fire once immediately when registered (if enabled).
        void AddAutoTrigger(std::string const& workflowId, std::string const& triggerId, bool isEnabled);

        // Register a cron trigger.
        // expression: 5-field cron string (minute hour day month weekday).
        // enabled: if false, trigger is stored but never fires.
        void AddCronTrigger(std::string const& workflowId, std::string const& triggerId, std::string const& expression,
                            bool isEnabled);

        // Register a file-watch trigger.
        // path: file or directory path the trigger is interested in.
        // events: vector of FileEventType (created/modified/deleted).
        // debounceMilliseconds: minimum time between firings.
        void AddFileWatchTrigger(std::string const& workflowId, std::string const& triggerId, std::string const& path,
                                 std::vector<FileEventType> const& events, uint32_t debounceMilliseconds, bool isEnabled);

        // Register a manual trigger.
        void AddManualTrigger(std::string const& workflowId, std::string const& triggerId, bool isEnabled);

        // Remove all triggers associated with a workflow (for reload).
        void ClearWorkflowTriggers(std::string const& workflowId);

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
            // If no time is found within a reasonable window, returns
            // referenceTime (caller will then treat it as disabled).
            std::chrono::system_clock::time_point
            ComputeNextFireTime(std::chrono::system_clock::time_point const& referenceTime) const;

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

    private:
        void FireTrigger(std::string const& workflowId, std::string const& triggerId) const;

        static bool ContainsEvent(std::vector<FileEventType> const& events, FileEventType fileEventType);

        // Helper: erase-remove for workflowId from a vector.
        template <typename TriggerVector>
        void EraseWorkflowFromVector(TriggerVector& triggerVector, std::string const& workflowId);

        static std::string NormalizePath(std::string const& path);
        static bool IsPathMatch(std::string const& watchedPath, std::string const& eventPath);

    private:
        TriggerCallback m_TriggerCallback;

        std::vector<CronTriggerInstance> m_CronTriggers;
        std::vector<FileWatchTriggerInstance> m_FileWatchTriggers;
        std::vector<ManualTriggerInstance> m_ManualTriggers;

        // Optional acceleration structure for file-trigger lookups:
        // map path → indices into m_FileWatchTriggers.
        std::unordered_map<std::string, std::vector<size_t>> m_FileWatchIndex;
    };
} // namespace AIAssistant
