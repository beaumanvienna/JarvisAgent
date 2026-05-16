# j9t patches to vendored PDCursesMod

This vendored copy of [PDCursesMod](https://github.com/Bill-Gray/PDCursesMod)
carries two local patches applied by j9t on **2026-05-16** during the
"AI Provider Error Visibility" dev plan's §19 SanitizeUtf8 verification
work.  Both fix grapheme-cluster-buffer overflow / assertion bugs in the
combining-character unpacking path that surface when a single base
character has more than 9 combining marks stacked on it.

## Trigger

j9t's TUI stress fixture `test/dispatch/fixtures/api1/ugly_real_world.json`
chains BOM + RTL/LTR overrides + ZWJ + LRI + RLI + FSI + Arabic letter
marks + Hebrew points + Zero-Width Joiner + Soft Hyphen + Mongolian
vowel separator + Unicode variation selector 16 (emoji-presentation)
into a single grapheme cluster.  PDCursesMod's combining-mark unpacker
uses a fixed 10-wchar buffer for that cluster.  Real provider responses
don't normally chain anywhere near 9+ combining marks; adversarial /
hostile input can.

## Patch 1 — `vt/pdcdisp.c` (Linux + macOS backend)

**Function:** `_unpack_combined_character` (around line 151).

**Pre-patch bug:** the post-loop assertion `assert(rval < buffsize)`
treats the legitimate "buffer fully utilized" state (`rval == buffsize`)
as overflow.  When fed a cluster with ≥ buffsize combining marks, the
loop exits with `rval == buffsize` AND `root` still set to a cluster
sentinel value (`> MAX_UNICODE`) rather than the actual base codepoint.
Writing that sentinel into `obuff[0]` and feeding it to
`PDC_wc_to_utf8` produces undefined output.

* In **Debug** builds the assertion fires SIGABRT and the entire ncurses
  application terminates (this is how j9t discovered the bug).
* In **Release** builds the assertion compiles out, the sentinel value
  silently corrupts the rendered glyph, and the cluster appears as
  garbage on screen.

**Fix:** drop the broken assertion; when the loop exits with `root`
still > `MAX_UNICODE` (cluster exceeded buffer capacity), substitute
`U+FFFD REPLACEMENT CHARACTER` for `obuff[0]` so downstream UTF-8
conversion produces a valid replacement glyph at the overflow site.
The combining marks already collected in `obuff[1..buffsize-1]` render
after.  The next mark we couldn't hold (still in `newchar`) is
intentionally dropped.

The patch is labeled `/* j9t fix (vt): ... */` for easy `git diff`
identification against upstream.

## Patch 2 — `wincon/pdcdisp.c` (Windows backend)

**Function:** the inline unpacking block inside the main render loop
(around line 246).

**Pre-patch bug:** the unpacking loop has NO bounds check on
`n_combined` against the size of the `added[10]` array.  Feeding it
a cluster with > 10 combining marks writes past `added[9]` into stack
memory — silent undefined behavior in **both** Debug and Release
(Windows backend has no equivalent of the vt assertion to catch it).
A second OOB read happens at `ch = (chtype)added[n_combined]` after
the loop.

**Fix:** add explicit bounds check `n_combined < J9T_MAX_COMBINED - 1`
to the loop condition so writes stay within the array.  When the loop
exits with `root` still a cluster sentinel, substitute `U+FFFD` for
both the base character AND the final dropped mark.  The marks already
collected in `added[0..n_combined-1]` render after.

The patch is labeled `/* j9t fix (wincon): ... */` for diff
identification.  Defines a small `J9T_MAX_COMBINED` macro scoped to
the block so it doesn't leak into the rest of the file.

## Defense in depth on the j9t side

These vendored patches are **belt** — the **braces** is a sibling
sanitizer in j9t itself: `engine/log/terminalLogStreamBuf.h` runs
every log line through a `CapCombiningRuns(...)` step that limits
consecutive combining / format codepoints to 8 per base character
before bytes reach the ncurses renderer.  That cap matches PDCursesMod's
10-wchar buffer with a 2-mark safety margin and produces visually
identical output for any real-world content (NFC normalization rarely
produces more than 2 combining marks per base).

The j9t-side cap is the primary defense — it stops pathological /
hostile input at the boundary where the codebase's bytes enter the
renderer pipeline.  The PDCursesMod patches are the secondary defense
for the case where some other code path bypasses the cap and feeds raw
clusters to ncurses.

## Rebasing onto a newer upstream PDCursesMod

If we ever pull a newer upstream PDCursesMod release, both patches
need to be re-applied:

1. After unpacking the new tree, `grep -n "j9t fix" vt/pdcdisp.c
   wincon/pdcdisp.c` — should return zero hits (the patches have been
   lost in the upgrade).
2. Re-apply by reading this file's two **Fix** sections and looking
   at the surrounding context in the new upstream code.  The pre-patch
   structures may have shifted but the bug shape should still be
   recognizable (a fixed-size combining buffer + an unguarded loop +
   either an off-by-one assertion or no bounds check at all).
3. Run `test/dispatch/test_tui_stress_malformed_utf8.py` against a
   TTY-active Debug Studio build (`./jarvisagent.sh --debug`, NOT
   `nohup ... > log/...log 2>&1 &` — see j9t's
   `doc/misc/hand-off.md` Sitting-8 entry for why).  If the test
   crashes with `vt/pdcdisp.c:NNN: _unpack_combined_character:
   Assertion ... failed`, the vt patch wasn't re-applied correctly.
4. On Windows, run the same test against a TTY-active Debug Studio
   build.  Stack-overflow symptoms are messier — typically a crash
   with an unhelpful stack trace, sometimes silent data corruption
   in surrounding stack frames.

## Why not upstream

The bug is real and reproducible; both fixes are small and obviously
correct.  An upstream PR is worth opening.  This vendored patch is
a short-term measure to unblock j9t shipping while that conversation
happens.  Once an upstream release contains the fixes, the
re-application step above goes away.
