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

#include "log/statusRenderer.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>

using namespace std::chrono_literals;

namespace AIAssistant
{
    namespace
    {
        // Bound `text` to at most `maxColumns` codepoints, landing on a
        // validated UTF-8 boundary.  One codepoint ≈ one column — the wide-char
        // width is intentionally NOT consulted; the approximation has been
        // good enough since the TUI shipped, and PDC_transform_line at refresh
        // time is what actually arbitrates wide-cell placement.
        //
        // Continuation-byte validation: a lead byte's *claimed* length is
        // honored only if every claimed continuation exists and starts with
        // 10xxxxxx.  Otherwise the lead is consumed as a single byte and the
        // malformed remnant survives into the output string — where
        // TerminalManager::SanitizeForCurses replaces it with '?' before it
        // reaches ncurses.  The earlier implementation trusted the lead byte
        // blindly, so a fake 4-byte lead near the end of a string could
        // over-skip past the actual character boundary, mis-counting columns
        // and potentially landing the cut mid-codepoint.
        std::string TruncateColumns(std::string_view text, int maxColumns)
        {
            if (maxColumns <= 0)
            {
                return {};
            }

            int columnCount = 0;
            size_t byteIndex = 0;
            size_t const totalBytes = text.size();

            while (byteIndex < totalBytes)
            {
                unsigned char const lead = static_cast<unsigned char>(text[byteIndex]);

                int const claimed = (lead & 0x80) == 0       ? 1
                                    : (lead & 0xE0) == 0xC0 ? 2
                                    : (lead & 0xF0) == 0xE0 ? 3
                                    : (lead & 0xF8) == 0xF0 ? 4
                                                            : 0;
                int charLength = 1;
                if (claimed >= 2 && byteIndex + static_cast<size_t>(claimed) <= totalBytes)
                {
                    bool valid = true;
                    for (int k = 1; k < claimed; ++k)
                    {
                        unsigned char const cont =
                            static_cast<unsigned char>(text[byteIndex + static_cast<size_t>(k)]);
                        if ((cont & 0xC0) != 0x80)
                        {
                            valid = false;
                            break;
                        }
                    }
                    if (valid) charLength = claimed;
                }

                if (columnCount + 1 > maxColumns)
                {
                    return std::string(text.substr(0, byteIndex));
                }

                byteIndex += static_cast<size_t>(charLength);
                columnCount += 1;
            }
            return std::string(text);
        }

        // Per-field caps for externally-sourced text dropped into the status
        // line.  The values are loose — well above any realistic legitimate
        // input — so well-formed displayIds and key-status labels render in
        // full; the cap only kicks in for pathological input (huge adhoc
        // suffix, malformed JCWF id, surprise upstream value).  Without these
        // bounds, a single long field would consume the row's column budget
        // and silently elide the rest of the status indicators.
        constexpr int kDisplayIdMaxColumns = 48;
        constexpr int kKeysStatusEchoMaxColumns = 24;

        std::string FormatRelative(std::string const& iso)
        {
            if (iso.empty()) return {};
            std::tm tm{};
            std::istringstream iss(iso);
            iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            if (iss.fail()) return {};
#ifdef _WIN32
            std::time_t t = _mkgmtime(&tm);
#else
            std::time_t t = timegm(&tm);
#endif
            if (t == static_cast<std::time_t>(-1)) return {};
            auto const tp = std::chrono::system_clock::from_time_t(t);
            auto const delta = std::chrono::system_clock::now() - tp;
            auto const secs = std::chrono::duration_cast<std::chrono::seconds>(delta).count();
            std::ostringstream oss;
            if (secs < 60)
            {
                oss << (secs < 0 ? 0 : secs) << "s ago";
            }
            else if (secs < 3600)
            {
                oss << (secs / 60) << "m ago";
            }
            else if (secs < 86400)
            {
                oss << (secs / 3600) << "h ago";
            }
            else
            {
                oss << (secs / 86400) << "d ago";
            }
            return oss.str();
        }

        std::string_view StateGlyph(std::string const& state)
        {
            if (state == "succeeded") return "✓";
            if (state == "failed") return "✗";
            if (state == "cancelled" || state == "stopped") return "⊘";
            if (state == "running" || state == "pending" || state == "queued" || state == "paused"
                || state == "stopping")
                return "…";
            return "·";
        }
    } // namespace

    void StatusRenderer::Start() {}
    void StatusRenderer::Stop() {}

    void StatusRenderer::SetRuntimeSnapshotProvider(RuntimeSnapshotProvider provider)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);
        m_SnapshotProvider = std::move(provider);
    }

    void StatusRenderer::SetLastRunsProvider(LastRunsProvider provider)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);
        m_LastRunsProvider = std::move(provider);
    }

    size_t StatusRenderer::GetRowCount()
    {
        return 2;
    }

    void StatusRenderer::BuildStatusLines(std::vector<std::string>& outLines, int maxColumns)
    {
        static std::array<char const*, 16> const spinnerChars{"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷",
                                                              "⠁", "⠂", "⠄", "⡀", "⢀", "⠠", "⠐", "⠈"};

        RuntimeSnapshotProvider snapshotProvider;
        LastRunsProvider lastRunsProvider;
        char const* spinnerGlyph = " ";

        {
            std::lock_guard<std::mutex> guard(m_Mutex);
            snapshotProvider = m_SnapshotProvider;
            lastRunsProvider = m_LastRunsProvider;
        }

        RuntimeSnapshot snap;
        if (snapshotProvider)
        {
            snap = snapshotProvider();
        }

        {
            std::lock_guard<std::mutex> guard(m_Mutex);
            if (snap.aiInflight > 0)
            {
                auto const now = std::chrono::steady_clock::now();
                if (now - m_LastSpinnerUpdate >= 100ms)
                {
                    m_SpinnerIndex = (m_SpinnerIndex + 1) % spinnerChars.size();
                    m_LastSpinnerUpdate = now;
                }
                spinnerGlyph = spinnerChars[m_SpinnerIndex % spinnerChars.size()];
            }
        }

        outLines.clear();

        // ---------- Row 1 — status LEDs (same signals as the dashboard) ----------
        {
            std::ostringstream oss;

            // Edition prefix — same compile-time define the /api/status `edition`
            // field uses, so the TUI label matches the dashboard exactly.
#ifdef J9T_STUDIO
            oss << "j9t Studio edition  |  ";
#else
            oss << "j9t Engine edition  |  ";
#endif

            // Keys state — the TUI's stand-in for the dashboard's "Connected" LED.
            if (snap.keysStatus == "ok")
            {
                oss << "● Keys unlocked";
            }
            else if (snap.keysStatus == "no_password")
            {
                oss << "● Keys LOCKED";
            }
            else if (snap.keysStatus == "wrong_password")
            {
                oss << "● Keys LOCKED (wrong password)";
            }
            else if (snap.keysStatus == "no_keys_file")
            {
                oss << "● Keys: no file (first run)";
            }
            else
            {
                // Externally-sourced echo path — bound the value so an
                // unexpected long status string can't crowd out the rest of
                // the row.  The four expected values above all fit in well
                // under the cap.
                oss << "● Keys: "
                    << (snap.keysStatus.empty() ? std::string("?")
                                                : TruncateColumns(snap.keysStatus, kKeysStatusEchoMaxColumns));
            }

            // AI in flight.
            if (snap.aiInflight > 0)
            {
                oss << "  ● AI queries in flight (" << snap.aiInflight << ") " << spinnerGlyph;
            }
            else
            {
                oss << "  ○ No queries";
            }

            // Active workflow runs.
            if (snap.activeRuns > 0)
            {
                oss << "  ● Workflow running (" << snap.activeRuns << ")";
            }
            else
            {
                oss << "  ○ No active runs";
            }

            // MCP sidecar.
            oss << (snap.mcpConnected ? "  ● MCP connected" : "  ○ MCP offline");

            // Cloud connector health.
            if (snap.cloudOpen > 0)
            {
                oss << "  ● Cloud: " << snap.cloudOpen << " circuit open";
            }
            else if (snap.cloudRecovering > 0)
            {
                oss << "  ● Cloud: " << snap.cloudRecovering << " recovering";
            }
            else if (snap.cloudHealthy > 0)
            {
                oss << "  ● Cloud: " << snap.cloudHealthy << " healthy";
            }
            else
            {
                oss << "  ○ Cloud: no connections";
            }

            // Workflow-run totals.
            oss << "  |  " << snap.totalCompleted << " succeeded";
            if (snap.totalFailed > 0)
            {
                oss << "  " << snap.totalFailed << " failed";
            }

            if (!snap.pythonRunning)
            {
                oss << "  !! Python offline";
            }

            outLines.push_back(TruncateColumns(oss.str(), maxColumns));
        }

        // ---------- Row 2 — last runs, plus a sealed-keys hint when applicable ----------
        {
            std::ostringstream oss;
            oss << "Last runs:";

            if (lastRunsProvider)
            {
                auto runs = lastRunsProvider(3);
                if (runs.size() > 3) runs.resize(3);
                if (runs.empty())
                {
                    oss << " (none yet)";
                }
                else
                {
                    for (auto const& run : runs)
                    {
                        // displayId carries workflow-id substrings and adhoc
                        // suffixes — both are externally controllable (JCWF
                        // upload, MCP run-with-id).  Bound here so a single
                        // pathological id can't fill the entire row.
                        oss << "  " << StateGlyph(run.state) << " "
                            << TruncateColumns(run.displayId, kDisplayIdMaxColumns);
                        if (run.isAdhoc)
                        {
                            oss << " [adhoc]";
                        }
                        std::string const ago = FormatRelative(run.completedAtIso);
                        if (!ago.empty())
                        {
                            oss << " " << ago;
                        }
                    }
                }
            }
            else
            {
                oss << " (n/a)";
            }

            // When keys are locked and providers aren't loaded, workflows with
            // ai_call tasks get skipped (same condition the dashboard banner
            // triggers on).  Surface it here since the TUI has no unlock UI.
            bool const keysBlocked = (snap.keysStatus != "ok") || !snap.hasProviders;
            if (keysBlocked)
            {
                oss << "  |  No AI providers configured — ai_call tasks are skipped. "
                       "Unlock the master password, or add providers in the Settings UI.";
            }

            outLines.push_back(TruncateColumns(oss.str(), maxColumns));
        }
    }
} // namespace AIAssistant
