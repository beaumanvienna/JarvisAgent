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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
   OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#pragma once

#include <atomic>
#include <string>
#include "taskExecutor.h"

namespace AIAssistant
{
#if defined(_WIN32)
    enum class WindowsShell
    {
        PowerShell,
        Bash
    };
#endif

    // ------------------------------------------------------------------------
    // ShellTaskExecutor
    //
    // Executes a JCWF `shell` task by running a shell command (bash on POSIX,
    // PowerShell or bash on Windows) with captured stdout/stderr forwarded
    // through the JarvisAgent log pipeline.
    //
    // --- Trust model ---------------------------------------------------------
    // Shell tasks are operator-authored — the workflow author writes the
    // `command` field directly into the JCWF.  This executor cannot prevent
    // a malicious or buggy `command` from doing whatever the j9t process can
    // do (read files, mutate state, exec other tools).  Defense in depth
    // lives in three places:
    //
    //   1. **Operator gate at submission** — shell tasks only reach the
    //      runtime via JCWFs the operator authored or approved.  The adhoc
    //      submission path additionally requires `adhoc_enabled` + `operator`
    //      role minimum.
    //
    //   2. **`ValidateScriptPath`** — the `command` MUST resolve to a path
    //      under `<projectRoot>/scripts/` after `fs::weakly_canonical`.  This
    //      prevents an attacker-supplied `command` from referencing a binary
    //      outside the trusted scripts directory (also rejects symlink
    //      targets that point out of tree).
    //
    //   3. **`IsSafeArgument` + always-quote in `JoinArgumentsForSystem`** —
    //      every `args[]` element (after JCWF template expansion) is checked
    //      against an extended blocklist (`; & | > < ' " ` $ ( ) \`) and then
    //      single-quoted before being concatenated into the `sh -c` string.
    //      Every arg is single-quoted with embedded-quote escaping (`'\''`),
    //      so the shell treats the entire arg as a literal — globbing,
    //      variable expansion, and command substitution are all neutralised.
    //
    // Shell tasks legitimately need shell features (`cd`, redirects, pipes
    // for the `command` itself), so direct `execve`/`execvp` doesn't fit.
    // The argv-only execution model applies to surfaces like the assistant's
    // `run_shell` tool that don't need shell semantics — not here.
    // -----------------------------------------------------------------------
    class ShellTaskExecutor : public ITaskExecutor
    {
    public:
        virtual ~ShellTaskExecutor() = default;

        [[nodiscard]] bool Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                   TaskDef const& taskDefinition, TaskInstanceState& taskState) override;

#if defined(_WIN32)
        // Probe PATH for bash (if use_bash config is true) and cache result. Call once at startup.
        static void ProbeWindowsShell(bool useBashConfig);

        // Return the cached shell selection.
        static WindowsShell GetWindowsShell();
#endif

    private:
        // Restrict which scripts can be invoked (e.g., must live under "scripts/").
        [[nodiscard]] bool ValidateScriptPath(std::string const& path) const;

        // Conservative safety check: allow typical path / flag characters and spaces,
        // but reject characters commonly used for shell injection.
        [[nodiscard]] bool IsSafeArgument(std::string const& argument) const;

#if defined(_WIN32)
        // Atomic so concurrent `Execute()` reads don't race the
        // startup-time `ProbeWindowsShell` write.  WindowsShell is a
        // POD enum; the atomic specialisation is trivial.
        static std::atomic<WindowsShell> s_WindowsShell;
#endif
    };
} // namespace AIAssistant
