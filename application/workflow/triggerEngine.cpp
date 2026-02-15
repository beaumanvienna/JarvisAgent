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
#include <unordered_set>

// Bring in logging and core types.
#include "core.h"
#include "engine.h"

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

    TriggerEngine::TriggerEngine(TriggerCallback const& triggerCallback) : m_TriggerCallback{triggerCallback} {}

    void TriggerEngine::AddAutoTrigger(std::string const& workflowId, std::string const& triggerId, bool isEnabled)
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
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        std::string const normalizedPath = NormalizePath(path);

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

    void TriggerEngine::ClearWorkflowTriggers(std::string const& workflowId)
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        LOG_APP_INFO("TriggerEngine::ClearWorkflowTriggers: clearing triggers for workflow '{}'", workflowId);

        EraseWorkflowFromVector(m_CronTriggers, workflowId);
        EraseWorkflowFromVector(m_FileWatchTriggers, workflowId);
        EraseWorkflowFromVector(m_ManualTriggers, workflowId);

        // Rebuild file-watch index because indices may have changed.
        m_FileWatchIndex.clear();
        for (size_t index = 0; index < m_FileWatchTriggers.size(); ++index)
        {
            FileWatchTriggerInstance const& instance = m_FileWatchTriggers[index];
            m_FileWatchIndex[instance.m_WatchedPath].push_back(index);
        }
    }

    void TriggerEngine::Tick(std::chrono::system_clock::time_point const& now)
    {
        std::vector<TriggerFiredEvent> eventsToFire;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
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
        LOG_APP_INFO("[paths debug] debug TriggerEngine::NotifyFileEvent: reason=triggerEvent eventPathProvided='{}' "
                     "eventPathNormalized='{}' eventType='{}'",
                     path, normalizedEventPath, static_cast<int>(fileEventType));

        std::vector<TriggerFiredEvent> eventsToFire;

        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);

            std::unordered_set<size_t> processedIndices;

            auto processIndex = [&](size_t triggerIndex)
            {
                if (triggerIndex >= m_FileWatchTriggers.size())
                {
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
        std::string normalizedPath;
        normalizedPath.reserve(path.size());

        bool previousWasSlash = false;
        for (char character : path)
        {
            char const normalizedCharacter = (character == '\\') ? '/' : character;

            if (normalizedCharacter == '/')
            {
                if (previousWasSlash)
                {
                    continue;
                }
                previousWasSlash = true;
            }
            else
            {
                previousWasSlash = false;
            }

            normalizedPath.push_back(normalizedCharacter);
        }

        return normalizedPath;
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

} // namespace AIAssistant
