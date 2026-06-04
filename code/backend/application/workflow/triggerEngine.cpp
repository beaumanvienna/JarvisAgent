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

#include "workflow/triggerEngine.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <unordered_set>

// Bring in logging and core types.
#include "auxiliary/file.h"
#include "core.h"
#include "engine.h"
#include "file/fileWatcher.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "log/secretRedactor.h"
#include "cloud/emailConnector.h"
#include "simdjson/simdjson.h"

// MSVC natively supports C++20 chrono timezone (time_zone, zoned_time, etc.).
// Apple Clang's libc++ does not, so we use Howard Hinnant's date library on non-MSVC platforms.
// _MSC_VER targets MSVC specifically; MinGW/Clang-on-Windows use the date library like Linux/macOS.
#ifdef _MSC_VER
#include <chrono>
#else
#include "date/tz.h"
#endif

namespace AIAssistant
{
    // ========================================================================
    // TriggerEngine::CronExpression
    // ========================================================================

    bool TriggerEngine::CronExpression::Parse(std::string const& expression)
    {
        m_IsValid = false;

        // Expect 5 space-separated fields.
        // Format: minute hour day-of-month month day-of-week
        //
        // Each field:
        //   "*"  -> wildcard (matches any value)
        //   "N"  -> fixed integer value
        //
        // This is intentionally minimal and can be extended later.
        std::vector<std::string> tokens;
        tokens.reserve(5);

        std::string currentToken;
        for (char character : expression)
        {
            if (character == ' ')
            {
                if (!currentToken.empty())
                {
                    tokens.push_back(currentToken);
                    currentToken.clear();
                }
            }
            else
            {
                currentToken.push_back(character);
            }
        }

        if (!currentToken.empty())
        {
            tokens.push_back(currentToken);
        }

        if (tokens.size() != 5)
        {
            LOG_APP_ERROR("CronExpression::Parse: expected 5 fields, got {} in '{}'", tokens.size(), expression);
            return false;
        }

        auto parseField = [](std::string const& field, bool& hasValue, int& value, int minValue, int maxValue) -> bool
        {
            if (field == "*")
            {
                hasValue = false;
                value = minValue;
                return true;
            }

            try
            {
                int parsedValue = std::stoi(field);
                if (parsedValue < minValue || parsedValue > maxValue)
                {
                    return false;
                }

                hasValue = true;
                value = parsedValue;
                return true;
            }
            catch (...)
            {
                return false;
            }
        };

        bool parseOk = true;

        parseOk =
            parseOk && parseField(tokens[0], m_HasMinute, m_Minute, 0, 59) &&
            parseField(tokens[1], m_HasHour, m_Hour, 0, 23) && parseField(tokens[2], m_HasDayOfMonth, m_DayOfMonth, 1, 31) &&
            parseField(tokens[3], m_HasMonth, m_Month, 1, 12) && parseField(tokens[4], m_HasDayOfWeek, m_DayOfWeek, 0, 6);

        if (!parseOk)
        {
            LOG_APP_ERROR("CronExpression::Parse: invalid field in expression '{}'", expression);
            return false;
        }

        m_IsValid = true;
        return true;
    }

    std::chrono::system_clock::time_point
    TriggerEngine::CronExpression::ComputeNextFireTime(std::chrono::system_clock::time_point const& referenceTime,
                                                       std::string const& timezone) const
    {
        if (!m_IsValid)
        {
            return referenceTime;
        }

        // Resolve the timezone. Empty string = system local time (current_zone).
#ifdef _MSC_VER
        std::chrono::time_zone const* tz = nullptr;
        try
        {
            tz = timezone.empty() ? std::chrono::current_zone() : std::chrono::locate_zone(timezone);
        }
        catch (std::runtime_error const& e)
        {
            LOG_APP_ERROR("CronExpression::ComputeNextFireTime: invalid timezone '{}': {}; falling back to system local",
                          timezone, e.what());
            tz = std::chrono::current_zone();
        }
#else
        date::time_zone const* tz = nullptr;
        try
        {
            tz = timezone.empty() ? date::current_zone() : date::locate_zone(timezone);
        }
        catch (std::runtime_error const& e)
        {
            LOG_APP_ERROR("CronExpression::ComputeNextFireTime: invalid timezone '{}': {}; falling back to system local",
                          timezone, e.what());
            tz = date::current_zone();
        }
#endif

        // Step in 60-second increments, up to one year.
        using namespace std::chrono;

        auto candidateTime = referenceTime + minutes(1);

        constexpr int maxIterations = 60 * 24 * 366;
        for (int iterationIndex = 0; iterationIndex < maxIterations; ++iterationIndex)
        {
            // Convert UTC time_point to local time in the target timezone.
#ifdef _MSC_VER
            auto const zonedTime = std::chrono::zoned_time{tz, candidateTime};
            auto const localTime = zonedTime.get_local_time();
            auto const localDays = std::chrono::floor<std::chrono::days>(localTime);
            std::chrono::year_month_day const ymd{localDays};
            std::chrono::hh_mm_ss const hms{localTime - localDays};
            std::chrono::weekday const wd{localDays};
#else
            auto const zonedTime = date::make_zoned(tz, candidateTime);
            auto const localTime = zonedTime.get_local_time();
            auto const localDays = date::floor<date::days>(localTime);
            date::year_month_day const ymd{localDays};
            date::hh_mm_ss const hms{localTime - localDays};
            date::weekday const wd{localDays};
#endif

            int const minute = static_cast<int>(hms.minutes().count());
            int const hour = static_cast<int>(hms.hours().count());
            int const dayOfMonth = static_cast<int>(static_cast<unsigned>(ymd.day()));
            int const month = static_cast<int>(static_cast<unsigned>(ymd.month()));
            int const dayOfWeek = wd.c_encoding(); // 0 = Sunday

            bool const matches = (!m_HasMinute || minute == m_Minute) && (!m_HasHour || hour == m_Hour) &&
                                 (!m_HasDayOfMonth || dayOfMonth == m_DayOfMonth) && (!m_HasMonth || month == m_Month) &&
                                 (!m_HasDayOfWeek || dayOfWeek == m_DayOfWeek);

            if (matches)
            {
                return candidateTime;
            }

            candidateTime += minutes(1);
        }

        LOG_APP_WARN("CronExpression::ComputeNextFireTime: no match found within one year, treating as disabled");
        return referenceTime;
    }

    bool TriggerEngine::CronExpression::IsValid() const { return m_IsValid; }

    // ========================================================================
    // TriggerEngine
    // ========================================================================

    TriggerEngine::TriggerEngine(TriggerCallback const& triggerCallback)
        : m_TriggerCallback{triggerCallback}, m_TriggerFileWatcher{std::make_unique<FileWatcher>(std::filesystem::path{})}
    {
        m_TriggerFileWatcher->Start();
        // Load before any AddEmailWatchTrigger fires — otherwise the binder
        // would seed instances with empty UIDs and the next poll would
        // re-seed from current IMAP state, losing any message that arrived
        // during the restart window.
        LoadPersistedEmailWatermarks();
    }

    TriggerEngine::~TriggerEngine()
    {
        // Destructors are noexcept by default in C++11+; an exception escaping
        // here would call std::terminate.  FileWatcher::Stop() joins worker
        // threads and tears down inotify/ReadDirectoryChangesW state — any of
        // those can throw on hostile environments.  Swallow + log so a
        // teardown failure can't take the process down.
        try
        {
            if (m_TriggerFileWatcher)
            {
                m_TriggerFileWatcher->Stop();
            }
        }
        catch (std::exception const& e)
        {
            LOG_APP_ERROR("TriggerEngine::~TriggerEngine: FileWatcher::Stop() threw: {}", e.what());
        }
        catch (...)
        {
            LOG_APP_ERROR("TriggerEngine::~TriggerEngine: FileWatcher::Stop() threw non-std exception");
        }
    }

    void TriggerEngine::AddAutoTrigger(std::string const& workflowId, std::string const& triggerId, bool isEnabled,
                                       bool fireImmediately)
    {
        bool shouldFire = false;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            LOG_APP_INFO("TriggerEngine::AddAutoTrigger: registered auto trigger '{}' for workflow '{}'", triggerId,
                         workflowId);

            if (!isEnabled)
            {
                LOG_APP_INFO("TriggerEngine::AddAutoTrigger: trigger '{}' for workflow '{}' is disabled; not firing",
                             triggerId, workflowId);
            }
            else if (!fireImmediately)
            {
                LOG_APP_INFO(
                    "TriggerEngine::AddAutoTrigger: trigger '{}' for workflow '{}' registered (reload — not firing)",
                    triggerId, workflowId);
            }
            else
            {
                shouldFire = true;
            }
        }

        // Auto triggers start the workflow immediately upon registration.
        // Fire outside the lock to avoid potential deadlock with callback.
        if (shouldFire)
        {
            FireTrigger(workflowId, triggerId);
        }
    }

    void TriggerEngine::AddCronTrigger(std::string const& workflowId, std::string const& triggerId,
                                       std::string const& expression, std::string const& timezone, bool isEnabled)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        CronTriggerInstance cronTriggerInstance{};
        cronTriggerInstance.m_WorkflowId = workflowId;
        cronTriggerInstance.m_TriggerId = triggerId;
        cronTriggerInstance.m_Timezone = timezone;
        cronTriggerInstance.m_IsEnabled = isEnabled;

        if (!cronTriggerInstance.m_Expression.Parse(expression))
        {
            LOG_APP_ERROR("TriggerEngine::AddCronTrigger: failed to parse cron expression '{}' "
                          "for workflow '{}', trigger '{}'",
                          expression, workflowId, triggerId);
            cronTriggerInstance.m_IsEnabled = false;
        }
        else
        {
            auto const now = std::chrono::system_clock::now();
            cronTriggerInstance.m_NextFireTime = cronTriggerInstance.m_Expression.ComputeNextFireTime(now, timezone);
        }

        m_CronTriggers.push_back(std::move(cronTriggerInstance));

        LOG_APP_INFO("TriggerEngine::AddCronTrigger: registered cron trigger '{}' for workflow '{}' (timezone: '{}')",
                     triggerId, workflowId, timezone.empty() ? "system" : timezone);
    }

    void TriggerEngine::AddFileWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                            std::string const& path, std::vector<FileEventType> const& events,
                                            uint32_t debounceMilliseconds, bool isEnabled)
    {
        std::string const normalizedPath = NormalizePath(path);
        if (normalizedPath.empty())
        {
            // ConfineUnderProjectRoot rejected the path — empty input,
            // `..`-escape, absolute path outside the project root, or symlink
            // pointing out of tree.  Operator-config error; refuse to register.
            LOG_APP_ERROR("TriggerEngine::AddFileWatchTrigger: rejected path '{}' (does not resolve under "
                          "project root) workflow='{}' trigger='{}'",
                          path, workflowId, triggerId);
            return;
        }

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            FileWatchTriggerInstance fileTriggerInstance{};
            fileTriggerInstance.m_WorkflowId = workflowId;
            fileTriggerInstance.m_TriggerId = triggerId;
            fileTriggerInstance.m_WatchedPath = normalizedPath;
            fileTriggerInstance.m_Events = events;
            fileTriggerInstance.m_DebounceInterval = std::chrono::milliseconds(debounceMilliseconds);
            fileTriggerInstance.m_IsEnabled = isEnabled;

            size_t triggerIndex = m_FileWatchTriggers.size();
            m_FileWatchTriggers.push_back(std::move(fileTriggerInstance));

            // Update index map.
            auto& indexVector = m_FileWatchIndex[normalizedPath];
            indexVector.push_back(triggerIndex);
        }

        // Register the path with the owned watcher so file events flow to the global
        // event queue and back into NotifyFileEvent.  AddPath is idempotent — multiple
        // triggers on the same directory are fine, the watcher tracks it once.
        if (m_TriggerFileWatcher)
        {
            m_TriggerFileWatcher->AddPath(std::filesystem::path{normalizedPath});
        }

        LOG_APP_INFO("[paths debug] debug TriggerEngine::AddFileWatchTrigger: reason=bindTrigger workflowId='{}' "
                     "triggerId='{}' watchedPathProvided='{}' watchedPathNormalized='{}'",
                     workflowId, triggerId, path, normalizedPath);
    }

    void TriggerEngine::AddManualTrigger(std::string const& workflowId, std::string const& triggerId, bool isEnabled)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        ManualTriggerInstance manualTriggerInstance{};
        manualTriggerInstance.m_WorkflowId = workflowId;
        manualTriggerInstance.m_TriggerId = triggerId;
        manualTriggerInstance.m_IsEnabled = isEnabled;

        m_ManualTriggers.push_back(std::move(manualTriggerInstance));

        LOG_APP_INFO("TriggerEngine::AddManualTrigger: registered manual trigger '{}' for workflow '{}'", triggerId,
                     workflowId);
    }

    void TriggerEngine::AddWebhookTrigger(std::string const& workflowId, std::string const& triggerId,
                                          std::string const& secret, bool isEnabled)
    {
        // Register the HMAC shared secret with the redactor BEFORE any logging
        // path can see it (the LOG_APP_INFO below is shape-only, but defense-in-depth
        // protects against future fail-path logs and against any code that walks
        // m_WebhookTriggers and logs the field).
        if (!secret.empty())
        {
            SecretRedactor::Get().AddSecret(secret);
        }

        std::scoped_lock<std::mutex> const lock(m_Mutex);
        WebhookTriggerInstance webhookTriggerInstance{};
        webhookTriggerInstance.m_WorkflowId = workflowId;
        webhookTriggerInstance.m_TriggerId = triggerId;
        webhookTriggerInstance.m_Secret = secret;
        webhookTriggerInstance.m_IsEnabled = isEnabled;

        size_t const index = m_WebhookTriggers.size();
        m_WebhookTriggers.push_back(std::move(webhookTriggerInstance));
        m_WebhookIndex[workflowId] = index;

        LOG_APP_INFO("TriggerEngine::AddWebhookTrigger: registered webhook trigger '{}' for workflow '{}' (secret={})",
                     triggerId, workflowId, secret.empty() ? "<none>" : "<set>");
    }

    void TriggerEngine::AddS3WatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                        std::string const& connectionName, std::string const& bucket,
                                        std::string const& prefix, uint32_t pollIntervalSeconds, bool isEnabled)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        S3WatchTriggerInstance instance{};
        instance.m_WorkflowId = workflowId;
        instance.m_TriggerId = triggerId;
        instance.m_ConnectionName = connectionName;
        instance.m_Bucket = bucket;
        instance.m_Prefix = prefix;
        instance.m_PollInterval = std::chrono::seconds(std::max(pollIntervalSeconds, 60u));
        instance.m_NextPollTime = std::chrono::steady_clock::now() + instance.m_PollInterval;
        instance.m_IsEnabled = isEnabled;

        m_S3WatchTriggers.push_back(std::move(instance));

        LOG_APP_INFO(
            "TriggerEngine::AddS3WatchTrigger: registered s3_watch trigger '{}' for workflow '{}' "
            "(connection={}, bucket={}, prefix={}, interval={}s)",
            triggerId, workflowId, connectionName, bucket.empty() ? "<default>" : bucket,
            prefix.empty() ? "<all>" : prefix, pollIntervalSeconds);
    }

    void TriggerEngine::AddOneDriveWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                                std::string const& connectionName, std::string const& folder,
                                                uint32_t pollIntervalSeconds, bool isEnabled)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        OneDriveWatchTriggerInstance instance{};
        instance.m_WorkflowId = workflowId;
        instance.m_TriggerId = triggerId;
        instance.m_ConnectionName = connectionName;
        instance.m_Folder = folder;
        instance.m_PollInterval = std::chrono::seconds(std::max(pollIntervalSeconds, 60u));
        instance.m_NextPollTime = std::chrono::steady_clock::now() + instance.m_PollInterval;
        instance.m_IsEnabled = isEnabled;

        m_OneDriveWatchTriggers.push_back(std::move(instance));

        LOG_APP_INFO(
            "TriggerEngine::AddOneDriveWatchTrigger: registered onedrive_watch trigger '{}' for workflow '{}' "
            "(connection={}, folder={}, interval={}s)",
            triggerId, workflowId, connectionName, folder.empty() ? "<root>" : folder, pollIntervalSeconds);
    }

    void TriggerEngine::AddEmailWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                             std::string const& connectionName, std::string const& folder,
                                             std::string const& subjectFilter, uint32_t pollIntervalSeconds,
                                             bool isEnabled)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        EmailWatchTriggerInstance instance{};
        instance.m_WorkflowId = workflowId;
        instance.m_TriggerId = triggerId;
        instance.m_ConnectionName = connectionName;
        instance.m_Folder = folder.empty() ? "INBOX" : folder;
        instance.m_SubjectFilter = subjectFilter;
        instance.m_PollInterval = std::chrono::seconds(std::max(pollIntervalSeconds, 60u));
        instance.m_NextPollTime = std::chrono::steady_clock::now() + instance.m_PollInterval;
        instance.m_IsEnabled = isEnabled;

        // Restore persisted UID watermark so mail that arrived during the
        // restart window still fires the trigger on the next poll.  If no
        // entry is found, m_LastSeenUid stays empty and the first poll
        // seeds the watermark from current IMAP state (same as a fresh
        // install).
        std::string const persistKey = workflowId + "|" + triggerId;
        auto const persistedIt = m_PersistedEmailWatermarks.find(persistKey);
        if (persistedIt != m_PersistedEmailWatermarks.end() && !persistedIt->second.m_LastSeenUid.empty())
        {
            // Registration-time guard: connection + folder must match the trigger's
            // current targeting.  UIDs are only meaningful within (server, folder);
            // a mismatch here means the JCWF was edited to repoint after the last
            // save — discard the saved UID so the next poll seeds fresh from the
            // new target.  UIDVALIDITY can't be checked here (we haven't polled
            // yet); the poll-time guard at the dispatch site covers that.
            PersistedEmailWatermark const& wm = persistedIt->second;
            if (wm.m_ConnectionName != connectionName || wm.m_Folder != instance.m_Folder)
            {
                LOG_APP_WARN("[email_watch] discarding stale watermark for trigger '{}' workflow '{}' — "
                             "trigger now targets connection='{}' folder='{}' but persisted entry is "
                             "connection='{}' folder='{}' (UID '{}' applied to mismatched target would be "
                             "meaningless)",
                             triggerId, workflowId, connectionName, instance.m_Folder,
                             wm.m_ConnectionName, wm.m_Folder, wm.m_LastSeenUid);
            }
            else
            {
                instance.m_LastSeenUid = wm.m_LastSeenUid;
                instance.m_LastSeenUidValidity = wm.m_UidValidity;
                LOG_APP_INFO("[email_watch] restored persisted UID watermark '{}' (UIDVALIDITY {}) for "
                             "trigger '{}' workflow '{}'",
                             instance.m_LastSeenUid, instance.m_LastSeenUidValidity, triggerId, workflowId);
            }
        }

        // Capture the resolved folder before the move — instance is moved-from
        // after push_back, so reading instance.m_Folder in the log below would
        // print an empty string.
        std::string const resolvedFolder = instance.m_Folder;
        m_EmailWatchTriggers.push_back(std::move(instance));

        LOG_APP_INFO(
            "TriggerEngine::AddEmailWatchTrigger: registered email_watch trigger '{}' for workflow '{}' "
            "(connection={}, folder={}, subject_filter={}, interval={}s)",
            triggerId, workflowId, connectionName, resolvedFolder,
            subjectFilter.empty() ? "<all>" : subjectFilter, pollIntervalSeconds);
    }

    void TriggerEngine::AddAzureBlobWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                                std::string const& connectionName, std::string const& container,
                                                std::string const& prefix, uint32_t pollIntervalSeconds,
                                                bool isEnabled)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        AzureBlobWatchTriggerInstance instance{};
        instance.m_WorkflowId = workflowId;
        instance.m_TriggerId = triggerId;
        instance.m_ConnectionName = connectionName;
        instance.m_Container = container;
        instance.m_Prefix = prefix;
        instance.m_PollInterval = std::chrono::seconds(std::max(pollIntervalSeconds, 60u));
        instance.m_NextPollTime = std::chrono::steady_clock::now() + instance.m_PollInterval;
        instance.m_IsEnabled = isEnabled;

        m_AzureBlobWatchTriggers.push_back(std::move(instance));

        LOG_APP_INFO(
            "TriggerEngine::AddAzureBlobWatchTrigger: registered azure_blob_watch trigger '{}' for workflow '{}' "
            "(connection={}, container={}, prefix={}, interval={}s)",
            triggerId, workflowId, connectionName, container.empty() ? "<default>" : container,
            prefix.empty() ? "<all>" : prefix, pollIntervalSeconds);
    }

    void TriggerEngine::AddGcsWatchTrigger(std::string const& workflowId, std::string const& triggerId,
                                            std::string const& connectionName, std::string const& bucket,
                                            std::string const& prefix, uint32_t pollIntervalSeconds, bool isEnabled)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        GcsWatchTriggerInstance instance{};
        instance.m_WorkflowId = workflowId;
        instance.m_TriggerId = triggerId;
        instance.m_ConnectionName = connectionName;
        instance.m_Bucket = bucket;
        instance.m_Prefix = prefix;
        instance.m_PollInterval = std::chrono::seconds(std::max(pollIntervalSeconds, 60u));
        instance.m_NextPollTime = std::chrono::steady_clock::now() + instance.m_PollInterval;
        instance.m_IsEnabled = isEnabled;

        m_GcsWatchTriggers.push_back(std::move(instance));

        LOG_APP_INFO(
            "TriggerEngine::AddGcsWatchTrigger: registered gcs_watch trigger '{}' for workflow '{}' "
            "(connection={}, bucket={}, prefix={}, interval={}s)",
            triggerId, workflowId, connectionName, bucket.empty() ? "<default>" : bucket,
            prefix.empty() ? "<all>" : prefix, pollIntervalSeconds);
    }

    TriggerEngine::WebhookTriggerInstance const* TriggerEngine::GetWebhookTrigger(std::string const& workflowId) const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        auto const iterator = m_WebhookIndex.find(workflowId);
        if (iterator == m_WebhookIndex.end())
        {
            return nullptr;
        }
        size_t const index = iterator->second;
        if (index >= m_WebhookTriggers.size())
        {
            // The index map and the trigger vector must stay in sync — every
            // mutation site (AddWebhookTrigger, ClearWorkflowTriggers, ClearAll)
            // rebuilds or maintains the index under the same lock as the
            // vector.  Tripping this guard means a future change broke the
            // invariant; fail closed and surface it loudly.
            LOG_APP_ERROR("TriggerEngine::GetWebhookTrigger: m_WebhookIndex points at out-of-range slot {} "
                          "(size={}) for workflow '{}' — index/vector skew",
                          index, m_WebhookTriggers.size(), workflowId);
            return nullptr;
        }
        return &m_WebhookTriggers[index];
    }

    void TriggerEngine::ClearAll()
    {
        std::vector<std::string> pathsToDrop;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            pathsToDrop.reserve(m_FileWatchIndex.size());
            for (auto const& [path, indices] : m_FileWatchIndex) pathsToDrop.push_back(path);

            m_CronTriggers.clear();
            m_FileWatchTriggers.clear();
            m_ManualTriggers.clear();
            m_WebhookTriggers.clear();
            m_S3WatchTriggers.clear();
            m_OneDriveWatchTriggers.clear();
            m_EmailWatchTriggers.clear();
            m_AzureBlobWatchTriggers.clear();
            m_GcsWatchTriggers.clear();
            m_FileWatchIndex.clear();
            m_WebhookIndex.clear();
        }

        if (m_TriggerFileWatcher)
        {
            for (auto const& path : pathsToDrop)
            {
                m_TriggerFileWatcher->RemovePath(std::filesystem::path{path});
            }
        }

        LOG_APP_INFO("TriggerEngine::ClearAll: all triggers cleared");
    }

    void TriggerEngine::ClearWorkflowTriggers(std::string const& workflowId)
    {
        std::vector<std::string> pathsToDrop;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            LOG_APP_INFO("TriggerEngine::ClearWorkflowTriggers: clearing triggers for workflow '{}'", workflowId);

            // Capture the file-watch paths that are about to go away so we can
            // unregister them from the owned watcher after rebuilding the index.
            std::unordered_set<std::string> surviving;
            for (auto const& instance : m_FileWatchTriggers)
            {
                if (instance.m_WorkflowId != workflowId)
                {
                    surviving.insert(instance.m_WatchedPath);
                }
            }
            for (auto const& instance : m_FileWatchTriggers)
            {
                if (instance.m_WorkflowId == workflowId && !surviving.contains(instance.m_WatchedPath))
                {
                    pathsToDrop.push_back(instance.m_WatchedPath);
                }
            }

            EraseWorkflowFromVector(m_CronTriggers, workflowId);
            EraseWorkflowFromVector(m_FileWatchTriggers, workflowId);
            EraseWorkflowFromVector(m_ManualTriggers, workflowId);
            EraseWorkflowFromVector(m_WebhookTriggers, workflowId);
            EraseWorkflowFromVector(m_S3WatchTriggers, workflowId);
            EraseWorkflowFromVector(m_OneDriveWatchTriggers, workflowId);
            EraseWorkflowFromVector(m_EmailWatchTriggers, workflowId);
            EraseWorkflowFromVector(m_AzureBlobWatchTriggers, workflowId);
            EraseWorkflowFromVector(m_GcsWatchTriggers, workflowId);

            // Rebuild file-watch index because indices may have changed.
            m_FileWatchIndex.clear();
            for (size_t index = 0; index < m_FileWatchTriggers.size(); ++index)
            {
                FileWatchTriggerInstance const& instance = m_FileWatchTriggers[index];
                m_FileWatchIndex[instance.m_WatchedPath].push_back(index);
            }

            // Rebuild webhook index.
            m_WebhookIndex.clear();
            for (size_t index = 0; index < m_WebhookTriggers.size(); ++index)
            {
                m_WebhookIndex[m_WebhookTriggers[index].m_WorkflowId] = index;
            }
        }

        if (m_TriggerFileWatcher)
        {
            for (auto const& path : pathsToDrop)
            {
                m_TriggerFileWatcher->RemovePath(std::filesystem::path{path});
            }
        }
    }

    void TriggerEngine::Tick(std::chrono::system_clock::time_point const& now)
    {
        std::vector<TriggerFiredEvent> eventsToFire;

        // EmailPollJob carries trigger identity (workflowId + triggerId) — not an
        // index into m_EmailWatchTriggers — because the lock is dropped while
        // network I/O happens.  Between the under-lock collection pass and
        // re-acquiring the lock to update the watermark, another thread can
        // call ClearWorkflowTriggers / AddEmailWatchTrigger and shift the
        // vector.  An index-based lookup would either OOB (bounds check
        // catches that) or — worse — silently land on a different trigger and
        // overwrite its watermark.  Identity-based lookup post-network is
        // fail-safe: if the trigger is gone, the watermark update is dropped.
        struct EmailPollJob
        {
            std::string m_WorkflowId;
            std::string m_TriggerId;
            std::string m_ConnectionName;
            std::string m_Folder;
            std::string m_SubjectFilter;
            std::string m_LastSeenUid;
            uint32_t    m_LastSeenUidValidity{0};
        };
        std::vector<EmailPollJob> emailPollJobs;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            // Cron triggers
            for (CronTriggerInstance& cronTriggerInstance : m_CronTriggers)
            {
                if (!cronTriggerInstance.m_IsEnabled)
                {
                    continue;
                }

                if (!cronTriggerInstance.m_Expression.IsValid())
                {
                    continue;
                }

                // If next fire time is in the past or now, fire and schedule the next one.
                if (cronTriggerInstance.m_NextFireTime <= now)
                {
                    eventsToFire.push_back({cronTriggerInstance.m_WorkflowId, cronTriggerInstance.m_TriggerId});

                    cronTriggerInstance.m_NextFireTime =
                        cronTriggerInstance.m_Expression.ComputeNextFireTime(now, cronTriggerInstance.m_Timezone);
                }
            }

            // S3 watch triggers (polling)
            auto const steadyNow = std::chrono::steady_clock::now();
            for (S3WatchTriggerInstance& s3Instance : m_S3WatchTriggers)
            {
                if (!s3Instance.m_IsEnabled || steadyNow < s3Instance.m_NextPollTime)
                {
                    continue;
                }

                s3Instance.m_NextPollTime = steadyNow + s3Instance.m_PollInterval;

                // S3 polling is done outside the lock (see below)
                // For now, we just schedule the poll check — actual S3 list is done asynchronously
                // by the workflow run that the trigger fires. The trigger fires on every poll interval;
                // the workflow itself decides if there's new data worth processing.
                eventsToFire.push_back({s3Instance.m_WorkflowId, s3Instance.m_TriggerId});
            }

            // OneDrive watch triggers (polling)
            for (OneDriveWatchTriggerInstance& odInstance : m_OneDriveWatchTriggers)
            {
                if (!odInstance.m_IsEnabled || steadyNow < odInstance.m_NextPollTime)
                {
                    continue;
                }

                odInstance.m_NextPollTime = steadyNow + odInstance.m_PollInterval;

                // Same pattern as S3: fire on every poll interval.
                // The workflow run performs the actual delta query to detect changes.
                eventsToFire.push_back({odInstance.m_WorkflowId, odInstance.m_TriggerId});
            }

            // Email watch triggers — collect due triggers under lock, IMAP check happens below
            for (EmailWatchTriggerInstance& emailInstance : m_EmailWatchTriggers)
            {
                if (!emailInstance.m_IsEnabled || steadyNow < emailInstance.m_NextPollTime)
                {
                    continue;
                }

                emailInstance.m_NextPollTime = steadyNow + emailInstance.m_PollInterval;
                emailPollJobs.push_back({emailInstance.m_WorkflowId, emailInstance.m_TriggerId,
                                         emailInstance.m_ConnectionName, emailInstance.m_Folder,
                                         emailInstance.m_SubjectFilter, emailInstance.m_LastSeenUid,
                                         emailInstance.m_LastSeenUidValidity});
            }

            // Azure Blob watch triggers (polling)
            for (AzureBlobWatchTriggerInstance& azureInstance : m_AzureBlobWatchTriggers)
            {
                if (!azureInstance.m_IsEnabled || steadyNow < azureInstance.m_NextPollTime)
                {
                    continue;
                }

                azureInstance.m_NextPollTime = steadyNow + azureInstance.m_PollInterval;
                eventsToFire.push_back({azureInstance.m_WorkflowId, azureInstance.m_TriggerId});
            }

            // GCS watch triggers (polling)
            for (GcsWatchTriggerInstance& gcsInstance : m_GcsWatchTriggers)
            {
                if (!gcsInstance.m_IsEnabled || steadyNow < gcsInstance.m_NextPollTime)
                {
                    continue;
                }

                gcsInstance.m_NextPollTime = steadyNow + gcsInstance.m_PollInterval;
                eventsToFire.push_back({gcsInstance.m_WorkflowId, gcsInstance.m_TriggerId});
            }
        }

        // Email watch: perform IMAP UID checks outside the lock (network I/O).
        // Lookup-by-identity helper: returns the index of the trigger matching
        // (workflowId, triggerId) at call time, or m_EmailWatchTriggers.size()
        // if it's gone (cleared / replaced during the network window).  Caller
        // must hold m_Mutex.
        auto findEmailTriggerIndexLocked = [&](EmailPollJob const& job) -> size_t
        {
            for (size_t k = 0; k < m_EmailWatchTriggers.size(); ++k)
            {
                if (m_EmailWatchTriggers[k].m_WorkflowId == job.m_WorkflowId &&
                    m_EmailWatchTriggers[k].m_TriggerId == job.m_TriggerId)
                {
                    return k;
                }
            }
            return m_EmailWatchTriggers.size();
        };

        for (auto const& job : emailPollJobs)
        {
            if (Core::g_Core == nullptr)
            {
                LOG_APP_ERROR("[email_watch] Core::g_Core is null, skipping IMAP check workflow='{}' trigger='{}'",
                              job.m_WorkflowId, job.m_TriggerId);
                continue;
            }
            auto connection = Core::g_Core->GetCloudConnectionManager().GetConnection(job.m_ConnectionName);
            if (!connection)
            {
                // Operator-config error — workflow names a connection that
                // isn't registered.  Unrecoverable until config is fixed; ERROR
                // so the dashboard run analyzer surfaces it.
                LOG_APP_ERROR("[email_watch] connection '{}' not found, skipping IMAP check "
                              "workflow='{}' trigger='{}'",
                              job.m_ConnectionName, job.m_WorkflowId, job.m_TriggerId);
                continue;
            }

            EmailConnector emailConnector;
            CloudCredentials credentials;
            std::string errorMessage;
            if (!emailConnector.ResolveCredentials(*connection, credentials, errorMessage))
            {
                // Keystore unlock / credentials decode failure — operator must
                // intervene.  ERROR per fail-path discipline.
                LOG_APP_ERROR("[email_watch] failed to resolve credentials for '{}': {} "
                              "workflow='{}' trigger='{}'",
                              job.m_ConnectionName, errorMessage, job.m_WorkflowId, job.m_TriggerId);
                continue;
            }

            bool hasNewMail = false;
            uint32_t currentUidValidity = 0;
            std::string highestUid = EmailConnector::CheckForNewMail(
                *connection, credentials, job.m_Folder, job.m_SubjectFilter, job.m_LastSeenUid, hasNewMail,
                currentUidValidity, errorMessage);

            // UIDVALIDITY-change guard: if the mailbox renumbered UIDs since
            // we last saw it, our saved UID watermark is meaningless (RFC 3501
            // §2.3.1.1).  Treat current state as the new baseline — clear the
            // in-memory watermark so the upcoming hasNewMail check seeds fresh
            // and the next save records the new UIDVALIDITY.  Skip the firing
            // for this poll cycle (the current messages predate the trigger
            // re-registration from the operator's standpoint).
            if (currentUidValidity != 0 && job.m_LastSeenUidValidity != 0 &&
                currentUidValidity != job.m_LastSeenUidValidity)
            {
                LOG_APP_WARN("[email_watch] connection='{}' folder='{}' UIDVALIDITY changed "
                             "({} -> {}); discarding stale UID watermark '{}' and re-seeding from current state "
                             "workflow='{}' trigger='{}'",
                             job.m_ConnectionName, job.m_Folder, job.m_LastSeenUidValidity,
                             currentUidValidity, job.m_LastSeenUid, job.m_WorkflowId, job.m_TriggerId);
                hasNewMail = false;
                // highestUid is left as returned; the per-job state at the
                // update site below will record it as the new seed with the
                // new UIDVALIDITY so future polls compare against it.
            }

            if (highestUid.empty() && !errorMessage.empty())
            {
                // Transient network / IMAP error — recoverable on next poll.
                // WARN with full context so the dashboard can still spot
                // chronic failures.
                LOG_APP_WARN("[email_watch] IMAP check failed for '{}': {} workflow='{}' trigger='{}'",
                             job.m_ConnectionName, errorMessage, job.m_WorkflowId, job.m_TriggerId);
                continue;
            }

            if (!hasNewMail && job.m_LastSeenUid.empty() && !highestUid.empty())
            {
                LOG_APP_INFO("[email_watch] seeded UID watermark for '{}' folder '{}' (highest UID {}) "
                             "workflow='{}' trigger='{}'",
                             job.m_ConnectionName, job.m_Folder, highestUid, job.m_WorkflowId, job.m_TriggerId);
            }
            else if (!hasNewMail)
            {
                LOG_APP_INFO("[email_watch] polled '{}' folder '{}' — no new mail (watermark UID {}) "
                             "workflow='{}' trigger='{}'",
                             job.m_ConnectionName, job.m_Folder,
                             highestUid.empty() ? "<empty>" : highestUid, job.m_WorkflowId, job.m_TriggerId);
            }

            // Update the watermark under lock, looking up by identity.  If the
            // trigger was removed during the network call, drop the update —
            // there's nothing valid to write it to.  After updating, mirror
            // into the persisted-watermark map and write the file so a
            // restart doesn't lose ground.
            {
                std::scoped_lock<std::mutex> const lock(m_Mutex);
                size_t const triggerIndex = findEmailTriggerIndexLocked(job);
                if (triggerIndex < m_EmailWatchTriggers.size())
                {
                    m_EmailWatchTriggers[triggerIndex].m_LastSeenUid = highestUid;
                    m_EmailWatchTriggers[triggerIndex].m_LastSeenUidValidity = currentUidValidity;

                    std::string const persistKey = job.m_WorkflowId + "|" + job.m_TriggerId;
                    PersistedEmailWatermark& entry = m_PersistedEmailWatermarks[persistKey];
                    entry.m_ConnectionName = job.m_ConnectionName;
                    entry.m_Folder = job.m_Folder;
                    entry.m_UidValidity = currentUidValidity;
                    entry.m_LastSeenUid = highestUid;
                    {
                        std::time_t const t = std::time(nullptr);
                        std::tm utc{};
#ifdef _WIN32
                        gmtime_s(&utc, &t);
#else
                        gmtime_r(&t, &utc);
#endif
                        char buf[32];
                        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
                        entry.m_UpdatedAtIso = buf;
                    }
                    SavePersistedEmailWatermarksLocked();
                }
                else
                {
                    LOG_APP_INFO("[email_watch] trigger removed during IMAP check; dropping watermark update "
                                 "workflow='{}' trigger='{}'",
                                 job.m_WorkflowId, job.m_TriggerId);
                }
            }

            if (hasNewMail)
            {
                // Identity is locked in via the job; re-confirm under lock
                // that the trigger still exists before queuing the fire.
                bool stillActive = false;
                {
                    std::scoped_lock<std::mutex> const lock(m_Mutex);
                    size_t const triggerIndex = findEmailTriggerIndexLocked(job);
                    stillActive = triggerIndex < m_EmailWatchTriggers.size();
                }
                if (stillActive)
                {
                    LOG_APP_INFO("[email_watch] new mail detected in '{}' (UID {}), firing trigger "
                                 "workflow='{}' trigger='{}'",
                                 job.m_Folder, highestUid, job.m_WorkflowId, job.m_TriggerId);
                    eventsToFire.push_back({job.m_WorkflowId, job.m_TriggerId});
                }
            }
        }

        for (auto const& event : eventsToFire)
        {
            FireTrigger(event.m_WorkflowId, event.m_TriggerId);
        }
    }

    void TriggerEngine::NotifyFileEvent(std::string const& path, FileEventType fileEventType,
                                        std::chrono::system_clock::time_point const& now)
    {
        std::string const normalizedEventPath = NormalizePath(path);
        if (normalizedEventPath.empty())
        {
            // Event path doesn't resolve under the project root — drop it.
            // Not security-critical (no trigger fires), but worth visibility
            // since it usually means a misconfigured watch path or an OS-level
            // path race.  WARN rather than ERROR — recoverable.
            LOG_APP_WARN("TriggerEngine::NotifyFileEvent: event path '{}' does not resolve under project root, "
                         "dropping event (eventType={})",
                         path, static_cast<int>(fileEventType));
            return;
        }
        LOG_APP_INFO("[paths debug] debug TriggerEngine::NotifyFileEvent: reason=triggerEvent eventPathProvided='{}' "
                     "eventPathNormalized='{}' eventType='{}'",
                     path, normalizedEventPath, static_cast<int>(fileEventType));

        std::vector<TriggerFiredEvent> eventsToFire;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            std::unordered_set<size_t> processedIndices;

            // Bounds check on triggerIndex is the safety guard that lets
            // m_FileWatchIndex carry indices into m_FileWatchTriggers — both
            // are mutated only under m_Mutex (held throughout this lambda's
            // call sites), so the index is valid by construction here.  The
            // guard catches a future change that lets the two drift.
            auto processIndex = [&](size_t triggerIndex)
            {
                if (triggerIndex >= m_FileWatchTriggers.size())
                {
                    LOG_APP_ERROR("TriggerEngine::NotifyFileEvent: m_FileWatchIndex points at out-of-range slot "
                                  "{} (size={}) — index/vector skew",
                                  triggerIndex, m_FileWatchTriggers.size());
                    return;
                }

                processedIndices.insert(triggerIndex);

                FileWatchTriggerInstance& fileTriggerInstance = m_FileWatchTriggers[triggerIndex];

                if (!fileTriggerInstance.m_IsEnabled)
                {
                    return;
                }

                if (!ContainsEvent(fileTriggerInstance.m_Events, fileEventType))
                {
                    return;
                }

                bool canFire = false;
                if (!fileTriggerInstance.m_HasFiredOnce)
                {
                    canFire = true;
                }
                else
                {
                    auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - fileTriggerInstance.m_LastFireTime);
                    if (elapsed >= fileTriggerInstance.m_DebounceInterval)
                    {
                        canFire = true;
                    }
                }

                if (canFire)
                {
                    fileTriggerInstance.m_HasFiredOnce = true;
                    fileTriggerInstance.m_LastFireTime = now;

                    LOG_APP_INFO("[paths debug] debug TriggerEngine::NotifyFileEvent: reason=fireTrigger workflowId='{}' "
                                 "triggerId='{}' watchedPathNormalized='{}' eventPathNormalized='{}' eventType='{}'",
                                 fileTriggerInstance.m_WorkflowId, fileTriggerInstance.m_TriggerId,
                                 fileTriggerInstance.m_WatchedPath, normalizedEventPath, static_cast<int>(fileEventType));
                    eventsToFire.push_back({fileTriggerInstance.m_WorkflowId, fileTriggerInstance.m_TriggerId});
                }
            };

            // Fast path: exact match using index.
            {
                auto iterator = m_FileWatchIndex.find(normalizedEventPath);
                if (iterator != m_FileWatchIndex.end())
                {
                    std::vector<size_t> const& indices = iterator->second;
                    for (size_t triggerIndex : indices)
                    {
                        processIndex(triggerIndex);
                    }
                }
            }

            // Slow path: prefix (directory) matches.
            for (size_t triggerIndex = 0; triggerIndex < m_FileWatchTriggers.size(); ++triggerIndex)
            {
                if (processedIndices.contains(triggerIndex))
                {
                    continue;
                }

                FileWatchTriggerInstance const& fileTriggerInstance = m_FileWatchTriggers[triggerIndex];
                if (!IsPathMatch(fileTriggerInstance.m_WatchedPath, normalizedEventPath))
                {
                    continue;
                }

                processIndex(triggerIndex);
            }
        }

        for (auto const& event : eventsToFire)
        {
            FireTrigger(event.m_WorkflowId, event.m_TriggerId);
        }
    }

    void TriggerEngine::FireManualTrigger(std::string const& workflowId, std::string const& triggerId)
    {
        bool found = false;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            for (ManualTriggerInstance const& manualTriggerInstance : m_ManualTriggers)
            {
                if (!manualTriggerInstance.m_IsEnabled)
                {
                    continue;
                }

                if (manualTriggerInstance.m_WorkflowId == workflowId && manualTriggerInstance.m_TriggerId == triggerId)
                {
                    found = true;
                    break;
                }
            }
        }

        if (found)
        {
            FireTrigger(workflowId, triggerId);
        }
        else
        {
            LOG_APP_WARN("TriggerEngine::FireManualTrigger: manual trigger '{}' for workflow '{}' not found or disabled",
                         triggerId, workflowId);
        }
    }

    void TriggerEngine::FireTrigger(std::string const& workflowId, std::string const& triggerId) const
    {
        if (!m_TriggerCallback)
        {
            LOG_APP_WARN("TriggerEngine::FireTrigger: callback is not set (workflow '{}', trigger '{}')", workflowId,
                         triggerId);
            return;
        }

        TriggerFiredEvent event{};
        event.m_WorkflowId = workflowId;
        event.m_TriggerId = triggerId;

        LOG_APP_INFO("TriggerEngine::FireTrigger: firing trigger '{}' for workflow '{}'", triggerId, workflowId);

        m_TriggerCallback(event);
    }

    bool TriggerEngine::ContainsEvent(std::vector<FileEventType> const& events, FileEventType fileEventType)
    {
        return std::find(events.begin(), events.end(), fileEventType) != events.end();
    }

    template <typename TriggerVector>
    void TriggerEngine::EraseWorkflowFromVector(TriggerVector& triggerVector, std::string const& workflowId)
    {
        auto newEndIterator = std::remove_if(triggerVector.begin(), triggerVector.end(), [&workflowId](auto const& instance)
                                             { return instance.m_WorkflowId == workflowId; });

        triggerVector.erase(newEndIterator, triggerVector.end());
    }

    std::string TriggerEngine::NormalizePath(std::string const& path)
    {
        // Per JC_Workflow_Specification §3.2.2, file_watch paths are
        // project-root-relative (e.g. "data/report.xlsx").  The match key for
        // both watch-path registration and event-path lookup is the canonical
        // absolute path under the project root, produced via the shared
        // ConfineUnderProjectRoot helper (fs::weakly_canonical +
        // lexically_relative containment + symlink-escape rejection).  An
        // event path with embedded `..` cannot escape the watched tree because
        // both sides of the comparison are reduced to canonical form.
        //
        // Empty input → empty output (nothing to register or match).
        // Containment failure → empty output (caller rejects via the empty
        // sentinel).  Forward-slash form is enforced via .generic_string() so
        // Windows event paths normalize to the same key shape as the
        // JCWF-supplied path.
        if (path.empty())
        {
            return {};
        }
        std::filesystem::path const confined = ConfineUnderProjectRoot(path);
        if (confined.empty())
        {
            return {};
        }
        return confined.generic_string();
    }

    bool TriggerEngine::IsPathMatch(std::string const& watchedPath, std::string const& eventPath)
    {
        if (watchedPath.empty())
        {
            return false;
        }

        if (eventPath == watchedPath)
        {
            return true;
        }

        if (eventPath.size() < watchedPath.size())
        {
            return false;
        }

        if (eventPath.compare(0, watchedPath.size(), watchedPath) != 0)
        {
            return false;
        }

        // watchedPath is a prefix. It's a match if watchedPath ends with '/'
        // or the next char in eventPath is '/'.
        if (watchedPath.back() == '/')
        {
            return true;
        }

        if (eventPath.size() == watchedPath.size())
        {
            return true;
        }

        char const boundaryCharacter = eventPath[watchedPath.size()];
        return boundaryCharacter == '/';
    }

    // -----------------------------------------------------------------------
    // Persisted email_watch UID watermarks
    // -----------------------------------------------------------------------

    std::filesystem::path TriggerEngine::EmailWatermarksFilePath() const
    {
        std::filesystem::path const queueFolder = Core::g_Core != nullptr
            ? std::filesystem::path(Core::g_Core->GetConfig().m_QueueFolderFilepath)
            : std::filesystem::path{};
        if (queueFolder.empty()) return {};
        return std::filesystem::absolute(queueFolder).lexically_normal() / ".email_watermarks.json";
    }

    void TriggerEngine::LoadPersistedEmailWatermarks()
    {
        std::filesystem::path const path = EmailWatermarksFilePath();
        if (path.empty() || !std::filesystem::exists(path))
        {
            return;
        }

        using namespace simdjson;
        ondemand::parser parser;
        padded_string json;
        if (auto err = padded_string::load(path.string()).get(json); err != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("[email_watch] failed to load '{}': {}", path.string(), error_message(err));
            return;
        }

        ondemand::document doc;
        if (auto err = parser.iterate(json).get(doc); err != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("[email_watch] failed to parse '{}': {}", path.string(), error_message(err));
            return;
        }

        // Format-version gate.  Only version 2 is accepted — version 1 (the
        // pre-UIDVALIDITY shape) is rejected with a WARN and start-fresh
        // rather than partially-loaded with default UIDVALIDITY=0 (which would
        // appear correct but defeat the mailbox-renumber guard on the first
        // poll after upgrade).  Per `feedback_no_legacy`: no compat shims.
        uint64_t formatVersion = 0;
        if (auto err = doc["format_version"].get_uint64().get(formatVersion); err != simdjson::SUCCESS)
        {
            LOG_APP_WARN("[email_watch] '{}' missing 'format_version' — discarding (starting fresh)",
                         path.string());
            return;
        }
        if (formatVersion != 2)
        {
            LOG_APP_WARN("[email_watch] '{}' format_version={} (expected 2) — discarding (starting fresh; "
                         "next poll re-seeds watermarks)", path.string(), formatVersion);
            return;
        }

        std::unordered_map<std::string, PersistedEmailWatermark> loaded;
        ondemand::array watermarks;
        if (auto err = doc["watermarks"].get_array().get(watermarks); err != simdjson::SUCCESS)
        {
            LOG_APP_ERROR("[email_watch] '{}' missing 'watermarks' array: {}", path.string(), error_message(err));
            return;
        }

        for (auto entry : watermarks)
        {
            ondemand::object obj;
            if (entry.get_object().get(obj) != simdjson::SUCCESS) continue;
            std::string_view workflowId, triggerId, connectionName, folder, lastSeenUid, updatedAt;
            uint64_t uidValidity = 0;
            if (obj["workflow_id"].get_string().get(workflowId) != simdjson::SUCCESS) continue;
            if (obj["trigger_id"].get_string().get(triggerId) != simdjson::SUCCESS) continue;
            if (obj["last_seen_uid"].get_string().get(lastSeenUid) != simdjson::SUCCESS) continue;
            // Connection + folder + UIDVALIDITY are load-bearing in format v2 — skip
            // entries missing any of them rather than silently defaulting (a missing
            // uid_validity field with implicit 0 would skip the UIDVALIDITY guard).
            if (obj["connection_name"].get_string().get(connectionName) != simdjson::SUCCESS) continue;
            if (obj["folder"].get_string().get(folder) != simdjson::SUCCESS) continue;
            if (obj["uid_validity"].get_uint64().get(uidValidity) != simdjson::SUCCESS) continue;
            if (uidValidity > 0xFFFFFFFFull) continue; // out of 32-bit range
            // updated_at is informational — tolerate absence (leave it empty on miss).
            // Consume the [[nodiscard]] error_code via the condition: GCC's
            // -Wunused-result is not silenced by a (void) cast.
            if (obj["updated_at"].get_string().get(updatedAt) != simdjson::SUCCESS)
            {
                updatedAt = {};
            }

            PersistedEmailWatermark wm;
            wm.m_ConnectionName = std::string(connectionName);
            wm.m_Folder = std::string(folder);
            wm.m_UidValidity = static_cast<uint32_t>(uidValidity);
            wm.m_LastSeenUid = std::string(lastSeenUid);
            wm.m_UpdatedAtIso = std::string(updatedAt);
            loaded.emplace(std::string(workflowId) + "|" + std::string(triggerId), std::move(wm));
        }

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_PersistedEmailWatermarks = std::move(loaded);
        }
        LOG_APP_INFO("[email_watch] loaded {} persisted UID watermark(s) from '{}'",
                     m_PersistedEmailWatermarks.size(), path.string());
    }

    void TriggerEngine::SavePersistedEmailWatermarksLocked()
    {
        std::filesystem::path const path = EmailWatermarksFilePath();
        if (path.empty()) return;

        // Prune orphaned watermarks before writing.  Workflow removal goes through
        // ClearAll() + RegisterAll() (the reload path), which re-adds the surviving
        // workflows' triggers but silently drops the removed one's — leaving its
        // persisted entry with no live trigger.  Without this prune the map (and the
        // on-disk file) would grow unboundedly across removals.  By the time a save
        // fires (only after a successful poll), RegisterAll has fully repopulated
        // m_EmailWatchTriggers with the surviving triggers, so any persisted key with
        // no matching live trigger is genuinely orphaned and safe to drop.
        {
            std::unordered_set<std::string> liveKeys;
            liveKeys.reserve(m_EmailWatchTriggers.size());
            for (auto const& instance : m_EmailWatchTriggers)
            {
                liveKeys.insert(instance.m_WorkflowId + "|" + instance.m_TriggerId);
            }
            for (auto it = m_PersistedEmailWatermarks.begin(); it != m_PersistedEmailWatermarks.end();)
            {
                if (!liveKeys.contains(it->first))
                {
                    LOG_APP_INFO("[email_watch] pruning orphaned watermark '{}' (no live trigger)", it->first);
                    it = m_PersistedEmailWatermarks.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        std::ostringstream body;
        body << "{\n  \"format_version\": 2,\n  \"watermarks\": [";
        bool first = true;
        for (auto const& [key, wm] : m_PersistedEmailWatermarks)
        {
            auto const pipe = key.find('|');
            if (pipe == std::string::npos) continue;
            std::string const workflowId = key.substr(0, pipe);
            std::string const triggerId = key.substr(pipe + 1);
            if (!first) body << ",";
            first = false;
            body << "\n    {"
                 << "\"workflow_id\": \""    << JsonHelper::EscapeJsonString(workflowId)        << "\", "
                 << "\"trigger_id\": \""     << JsonHelper::EscapeJsonString(triggerId)         << "\", "
                 << "\"connection_name\": \""<< JsonHelper::EscapeJsonString(wm.m_ConnectionName) << "\", "
                 << "\"folder\": \""         << JsonHelper::EscapeJsonString(wm.m_Folder)         << "\", "
                 << "\"uid_validity\": "     << wm.m_UidValidity                                 << ", "
                 << "\"last_seen_uid\": \""  << JsonHelper::EscapeJsonString(wm.m_LastSeenUid)    << "\", "
                 << "\"updated_at\": \""     << JsonHelper::EscapeJsonString(wm.m_UpdatedAtIso)   << "\""
                 << "}";
        }
        if (!first) body << "\n  ";
        body << "]\n}\n";

        std::string writeError;
        if (!EngineCore::AtomicWriteFile(path, body.str(), writeError))
        {
            LOG_APP_ERROR("[email_watch] failed to write '{}': {}", path.string(), writeError);
        }
    }

} // namespace AIAssistant
