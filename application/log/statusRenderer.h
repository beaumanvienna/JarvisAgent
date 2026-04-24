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

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace AIAssistant
{
    class StatusRenderer
    {
    public:
        // Mirrors the signals the dashboard consumes from /api/status +
        // /api/settings/keys/status so the TUI and web views stay in sync.
        struct RuntimeSnapshot
        {
            std::string keysStatus;        // "ok" | "no_password" | "wrong_password" | "no_keys_file"
            bool hasProviders{false};      // keyManager has at least one provider loaded
            size_t aiInflight{0};          // AiRequestPool::GetDirectDispatchInflight()
            size_t activeRuns{0};          // WorkflowRuntimeManager::GetActiveRunsSnapshot().size()
            bool mcpConnected{false};      // WebServer::IsMcpConnected()
            size_t cloudHealthy{0};
            size_t cloudRecovering{0};
            size_t cloudOpen{0};
            uint64_t totalCompleted{0};    // WorkflowRuntimeManager::GetRunCounters
            uint64_t totalFailed{0};
            bool pythonRunning{true};      // PythonEnginePool health
        };

        struct LastRunSummary
        {
            std::string displayId;
            std::string state;             // succeeded | failed | cancelled | running | pending | queued | stopped | paused
            std::string completedAtIso;    // ISO 8601 UTC; empty for still-running
            bool isAdhoc{false};
        };

        using RuntimeSnapshotProvider = std::function<RuntimeSnapshot()>;
        using LastRunsProvider = std::function<std::vector<LastRunSummary>(size_t maxCount)>;

        StatusRenderer() = default;

        // Providers are called once per redraw.  Keep them cheap.
        void SetRuntimeSnapshotProvider(RuntimeSnapshotProvider provider);
        void SetLastRunsProvider(LastRunsProvider provider);

        void Start();
        void Stop();

        // Builds two rows:
        //   Row 1 — status LEDs: Keys / AI in flight / Active runs / MCP / Cloud / succeeded / failed
        //   Row 2 — "Last runs:" + up to 3 entries laid out horizontally
        // When keys are locked, row 1 highlights that and row 2 carries a short
        // "Unlock in the dashboard" hint (the TUI itself has no unlock affordance).
        void BuildStatusLines(std::vector<std::string>& outLines, int maxColumns);

        // Always returns 2.
        size_t GetRowCount();

    private:
        std::mutex m_Mutex;
        size_t m_SpinnerIndex{0};
        std::chrono::steady_clock::time_point m_LastSpinnerUpdate{std::chrono::steady_clock::now()};
        RuntimeSnapshotProvider m_SnapshotProvider;
        LastRunsProvider m_LastRunsProvider;
    };
} // namespace AIAssistant
