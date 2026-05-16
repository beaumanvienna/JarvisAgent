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

#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <streambuf>
#include <string>

#include "log/terminalManager.h"

namespace AIAssistant
{
    class TerminalLogStreamBuf : public std::streambuf
    {
    public:
        TerminalLogStreamBuf(TerminalManager* terminalManager, std::shared_ptr<std::ofstream> fileLogger)
            : m_TerminalManager(terminalManager), m_FileLogger(std::move(fileLogger))
        {
        }

        void SetLogBroadcastCallback(std::function<void(std::string const&)> callback)
        {
            std::lock_guard<std::mutex> lock(m_CallbackMutex);
            m_LogBroadcastCallback = std::move(callback);
        }

    protected:
        int sync() override
        {
            std::lock_guard<std::mutex> lock(m_BufferMutex);
            return syncLocked();
        }

        int overflow(int character) override
        {
            if (character == traits_type::eof())
                return traits_type::not_eof(character);

            std::lock_guard<std::mutex> lock(m_BufferMutex);
            char c = static_cast<char>(character);

            if (c == '\n')
            {
                syncLocked();
            }
            else
            {
                m_Buffer.push_back(c);
            }

            return character;
        }

        std::streamsize xsputn(char const* data, std::streamsize count) override
        {
            std::lock_guard<std::mutex> lock(m_BufferMutex);
            char const* start = data;
            char const* end = data + count;
            for (char const* p = start; p < end; ++p)
            {
                if (*p == '\n')
                {
                    m_Buffer.append(start, p);
                    syncLocked();
                    start = p + 1;
                }
            }
            if (start < end)
            {
                m_Buffer.append(start, end);
            }
            return count;
        }

    private:
        // Removes full ANSI escape sequences safely
        static std::string StripAnsi(std::string const& input)
        {
            std::string output;
            output.reserve(input.size());

            bool inEscape = false;
            for (unsigned char c : input)
            {
                if (!inEscape)
                {
                    if (c == 0x1B) // ESC
                    {
                        inEscape = true;
                        continue;
                    }
                    output.push_back(c);
                }
                else
                {
                    // end ESC if 'm' or reset
                    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                        inEscape = false;
                }
            }

            return output;
        }

        // Combining / format codepoint detection for CapCombiningRuns.
        // Curated list of Unicode ranges that visually attach to a preceding
        // base character (Mn / Mc / Me / a subset of Cf categories).  Not
        // exhaustive but covers everything the §19 TUI stress fixtures throw —
        // diacriticals, Hebrew/Arabic combining marks, ZWJ/ZWNJ, BiDi
        // controls, variation selectors (BMP + supplement), Mongolian VS,
        // BOM, combining marks for symbols, combining half marks.
        // Adding ICU as a dependency would give General_Category-perfect
        // detection but isn't worth the dependency weight for the §19 use
        // case — pathological input gets dropped, slight false positives
        // (rare-script combining marks not listed) just get capped sooner.
        static bool IsCombiningOrFormat(std::uint32_t cp)
        {
            if (cp >= 0x0300u && cp <= 0x036Fu) return true; // combining diacriticals
            if (cp >= 0x0483u && cp <= 0x0489u) return true; // Cyrillic combining
            if (cp >= 0x0591u && cp <= 0x05BDu) return true; // Hebrew points/cantillation
            if (cp == 0x05BFu) return true;
            if (cp == 0x05C1u || cp == 0x05C2u) return true;
            if (cp == 0x05C4u || cp == 0x05C5u) return true;
            if (cp == 0x05C7u) return true;
            if (cp >= 0x0610u && cp <= 0x061Au) return true; // Arabic small letter marks
            if (cp >= 0x064Bu && cp <= 0x065Fu) return true; // Arabic diacriticals
            if (cp == 0x0670u) return true;                  // Arabic superscript alef
            if (cp >= 0x06D6u && cp <= 0x06DCu) return true;
            if (cp >= 0x06DFu && cp <= 0x06E4u) return true;
            if (cp >= 0x06E7u && cp <= 0x06E8u) return true;
            if (cp >= 0x06EAu && cp <= 0x06EDu) return true;
            if (cp >= 0x180Bu && cp <= 0x180Du) return true; // Mongolian variation selectors
            if (cp >= 0x200Bu && cp <= 0x200Fu) return true; // ZWSP/ZWNJ/ZWJ/LRM/RLM
            if (cp >= 0x202Au && cp <= 0x202Eu) return true; // LRE/RLE/PDF/LRO/RLO
            if (cp >= 0x2060u && cp <= 0x206Fu) return true; // word joiner / function app / RLI/LRI/FSI/PDI
            if (cp >= 0x20D0u && cp <= 0x20FFu) return true; // combining marks for symbols
            if (cp == 0xFEFFu) return true;                  // BOM / zero-width no-break
            if (cp >= 0xFE00u && cp <= 0xFE0Fu) return true; // variation selectors (BMP)
            if (cp >= 0xFE20u && cp <= 0xFE2Fu) return true; // combining half marks
            if (cp >= 0xE0100u && cp <= 0xE01EFu) return true; // variation selectors supplement
            return false;
        }

        // CapCombiningRuns — limit consecutive combining/format codepoints
        // to N per base character before bytes reach the ncurses renderer or
        // any downstream consumer with a fixed-size grapheme-cluster buffer.
        // PDCursesMod's `_unpack_combined_character` uses a 10-wchar buffer
        // (vt/pdcdisp.c:338, wincon/pdcdisp.c:249) and either asserts/crashes
        // (Debug) or silently truncates / overflows (Release / Windows) when
        // fed a base + >9 combining marks.  Capping at 8 here gives a safe
        // 2-mark headroom under that buffer + matches what any real document
        // would contain (NFC rarely produces >2 combining marks per base).
        //
        // Pathological input (rendered via U+FFFD-substituted bytes from
        // SanitizeUtf8 + adversarial JSON fixtures like the §19 TUI stress
        // battery) can chain unlimited combining marks; this cap is the
        // boundary defense for the renderer.  Capped bytes are dropped
        // entirely (no replacement glyph — the visual effect at 9+ stacked
        // marks is already incomprehensible).
        //
        // Assumes well-formed UTF-8 input (which it is: callers run through
        // SanitizeUtf8 first OR are the LOG_* macros emitting our own
        // already-clean strings).  Malformed sequences would already have
        // been replaced with U+FFFD upstream; this path treats any continuation
        // byte without a valid lead as a single-byte passthrough.
        static std::string CapCombiningRuns(std::string const& input, std::size_t maxRun = 8)
        {
            std::string out;
            out.reserve(input.size());

            std::size_t consecutiveCombining = 0;
            std::size_t i = 0;
            while (i < input.size())
            {
                unsigned char const b = static_cast<unsigned char>(input[i]);

                // ASCII fast path — always a base char, resets the run.
                if (b < 0x80u)
                {
                    out.push_back(input[i]);
                    consecutiveCombining = 0;
                    ++i;
                    continue;
                }

                // Decode the multi-byte sequence (input is well-formed per the
                // contract — see header comment).  Lengths: 2/3/4.
                std::size_t needed = 0;
                std::uint32_t cp = 0;
                if      ((b & 0xE0u) == 0xC0u) { needed = 1; cp = b & 0x1Fu; }
                else if ((b & 0xF0u) == 0xE0u) { needed = 2; cp = b & 0x0Fu; }
                else if ((b & 0xF8u) == 0xF0u) { needed = 3; cp = b & 0x07u; }
                else
                {
                    // Defensive: shouldn't happen after SanitizeUtf8, but if a
                    // raw byte slips through, pass it once + reset the run.
                    out.push_back(input[i]);
                    consecutiveCombining = 0;
                    ++i;
                    continue;
                }

                if (i + needed >= input.size())
                {
                    // Truncated tail — pass + reset.
                    out.push_back(input[i]);
                    consecutiveCombining = 0;
                    ++i;
                    continue;
                }

                for (std::size_t k = 1; k <= needed; ++k)
                {
                    unsigned char const c = static_cast<unsigned char>(input[i + k]);
                    cp = (cp << 6) | (c & 0x3Fu);
                }

                if (IsCombiningOrFormat(cp))
                {
                    if (consecutiveCombining < maxRun)
                    {
                        out.append(input, i, needed + 1);
                        ++consecutiveCombining;
                    }
                    // else: drop this combining mark; the visual cluster is
                    // already saturated.  No replacement glyph.
                }
                else
                {
                    // Base character — emit + reset the run.
                    out.append(input, i, needed + 1);
                    consecutiveCombining = 0;
                }
                i += needed + 1;
            }
            return out;
        }

    private:
        int syncLocked()
        {
            if (!m_Buffer.empty())
            {
                // StripAnsi then CapCombiningRuns: each is independent — ANSI
                // escapes are byte-level, combining-mark runs are codepoint-level.
                // Running both unconditionally here means every downstream
                // consumer (ncurses TUI, log file, WS broadcast) gets the same
                // sanitized stream — no surface accidentally bypasses the cap.
                std::string clean = CapCombiningRuns(StripAnsi(m_Buffer));
                m_Buffer.clear();

                if (!clean.empty())
                {
                    if (m_TerminalManager != nullptr)
                    {
                        m_TerminalManager->EnqueueLogLine(clean);
                    }

                    if (m_FileLogger && m_FileLogger->is_open())
                    {
                        std::lock_guard<std::mutex> lock(m_FileMutex);
                        (*m_FileLogger) << clean << "\n";
                        m_FileLogger->flush();
                    }

                    {
                        std::lock_guard<std::mutex> lock(m_CallbackMutex);
                        if (m_LogBroadcastCallback)
                        {
                            m_LogBroadcastCallback(clean);
                        }
                    }
                }
            }

            return 0;
        }

    private:
        TerminalManager* m_TerminalManager;
        std::string m_Buffer;
        std::mutex m_BufferMutex;

        std::shared_ptr<std::ofstream> m_FileLogger;
        std::mutex m_FileMutex;

        std::function<void(std::string const&)> m_LogBroadcastCallback;
        std::mutex m_CallbackMutex;
    };
} // namespace AIAssistant
