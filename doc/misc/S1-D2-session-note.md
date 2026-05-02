# S1 — D2 (Web + Cloud + Assistant) Hardening Session Note

**Plans:** `doc/misc/cybersec-hardening-dev-plan.md` (§18) + `doc/misc/cpp-safety-hardening-dev-plan.md` (§19), session S1 covers domain D2 in both.
**Sources:** `doc/combinedCyberSecAudit.md` §05 (assistantTools.h); `doc/combinedSafetyAudit.md` §05 (same file).
**Started:** 2026-04-29.
**Status:** in progress — sitting 1 of N closed at the documented boundary "argv-only execution + canonical-cwd in `application/assistant/assistantTools.cpp`".

This file accumulates per-change template entries (per cybersec-hardening §5) across sittings.

---

## Sitting 1 — assistantTools.cpp argv-only + canonical-cwd

**Scope locked:** the 5 cyber-sec CRITICAL findings in `application/assistant/assistantTools.cpp` — all share the popen-with-string-composition root cause and the blocklist-instead-of-allowlist anti-pattern. HIGHs in the same file (`IsPathDenied` symlink/case bypass, `.bak` leak, `ExecJcwfGenerate` JSON injection, `ExecJcwfWriteScript` path validation, `ExecGetFileSummary` missing deny-list, Windows PowerShell command quoting, prompt-injection enabling AI-controlled tool execution without approval, `IsPathDenied` deny-list bypass via symlinks and case variants) and assistantController.h CRITICALs (auth bypass, path traversal in GetSession) are explicitly deferred to sitting 2 — the boundary is "argv-only execution + canonical-cwd in this file" so we don't carry state across sittings.

### [CRITICAL] Shell injection in `ExecSearchFiles` — assistantTools.cpp:~1040

**Finding (paraphrased):** `query` and `glob` are interpolated into a shell command with only trivial single-quote-to-double-quote substitution before `popen`. An AI-supplied query like `foo' --evil-flag` or `foo" $(evil)` breaks out of the quoting and injects shell commands. The `|| grep ...` fallback re-uses the same un-sanitized query, doubling the injection surface.

**Verification:** Holds up. Read the original code: `for (auto& c : query) if (c == '\'') c = '"';` followed by `cmd += " -- '" + query + "' . 2>/dev/null || grep -rn ... '" + query + "' ..."`. Both points are exactly as the audit describes.

**Change:** POSIX path now uses argv-array exec via the new anonymous-namespace helper `RunArgvCapture` — no shell parser anywhere on the dispatch path; `query` and `glob` are passed as discrete `argv` elements. The grep fallback is removed (clean error if `rg` is missing). Windows path keeps popen-via-bash but applies full POSIX single-quote escaping (`PosixSingleQuote`) to `query`, `glob`, and every `--glob` exclusion — the audit's minimum-acceptable mitigation per its CRITICAL #1 fix list. Argv-exec port to CreateProcess on Windows is deferred to a Windows-specific sitting (cmd-line round-trip via CommandLineToArgvW is non-trivial and out of today's scope).

**Ramifications:**
- Callers touched: only `m_ToolFns["search_files"]` lambda — internal dispatch, no external API surface change.
- Tests touched: existing dispatcher hermetic tests (re-ran). Assistant tool tests (`test/assistant/test_assistant.py`) cannot run against TLS-only j9t over WS — see Verification gap below.
- Docs touched: none. The tool's user-visible contract is unchanged.
- Blast radius if wrong: search_files returns a clean error or an empty result on POSIX systems where `rg` isn't installed — previously the grep fallback hid this. Mitigation: error message names the missing dependency.

**Tested by:**
- Studio debug build clean (`make config=debug`); 5 .cpp files recompiled (assistantController.cpp, assistantTools.cpp, jarvisAgent.cpp, webServer.cpp, webServer_studio.cpp), link succeeds.
- `python3 test/dispatch/test_testinterface_hermetic.py` PASS — confirms the rebuild + binary + adjacent code paths still function.
- Runtime smoke of `search_files` tool itself deferred to JC dashboard verification (see "Verification gap" section).

---

### [CRITICAL] Shell injection in `ExecListFiles` — assistantTools.cpp:~1107

**Finding (paraphrased):** `path` argument is interpolated into a `find '...'` shell command string with no escaping beyond `lexically_normal()`. A path containing `foo' -exec rm -rf / \;` injects shell commands.

**Verification:** Holds up. Original: `std::string cmd = "find '" + dirPath.string() + "' -maxdepth ..."` then popen. The single-quote in user-supplied `path` would terminate the quoted argument. lexically_normal() is purely syntactic and does not escape.

**Change:** Replaced the entire shell-out with `std::filesystem::recursive_directory_iterator` walking with a leaf-name exclusion set (node_modules, .git, bin-int, vendor, bin) and a depth cap. No exec, no shell parser, no popen on either platform — fully portable.

**Ramifications:**
- Callers touched: only `m_ToolFns["list_files"]` lambda.
- Tests touched: same gap as search_files — runtime test is dashboard-driven manual verification by JC.
- Docs touched: none.
- Blast radius if wrong: list_files output format slightly differs from the prior `find -printf '%y %p\n'` — types are now `f`/`d`/`l` (file/dir/link) matching the prior `%y` semantics. Output ordering may differ (filesystem-order vs find's default). If a downstream consumer parses list_files output strictly, that's a regression — but the consumer is the AI which reads natural-language output.

**Tested by:**
- Build clean.
- Hermetic dispatcher test PASS.
- Runtime smoke deferred (see Verification gap).

---

### [CRITICAL] Shell injection in `ExecGetLogTail` — assistantTools.cpp:~1162 (DELETED)

**Finding (paraphrased):** `tail -N '...'` shell composition with `logPath.string()` embedded; latent vulnerability if `logPath` ever becomes configurable.

**Verification:** Holds up — the `tail -N '...'` composition exists and is dangerous if reactivated. However the function is unreachable from current AI dispatch — line 149's comment notes it was removed from `m_ToolFns` in favor of the `/log [N]` slash command, and grep confirms zero callers in `m_ToolFns`.

**Change:** Deleted the function entirely (definition + declaration in `assistantTools.h` + the dead-code comment in the constructor). No replacement — the slash command path serves the user need, and the AI tool was self-defeating per the original removal rationale.

**Ramifications:**
- Callers touched: zero (function was already unreachable from `m_ToolFns`).
- Tests touched: none.
- Docs touched: none.
- Blast radius if wrong: zero — function was dead code.

**Tested by:**
- Build clean (would have failed if a hidden caller existed).
- grep confirms no remaining references to `ExecGetLogTail` or `get_log_tail` after deletion.

---

### [CRITICAL] Incomplete blocklist in `IsCommandBlocked` allows trivial bypass — assistantTools.cpp:~1448 (DELETED)

**Finding (paraphrased):** Substring-based blocklist trivially bypassed by whitespace variation (`rm  -rf /`), case (`SuDo`), suffix variation (`rm -rf /home`), or alternative spellings (`mkfs.ext4`). False security confidence; cannot be made secure for arbitrary shell commands.

**Verification:** Holds up. Blocklist patterns are literal `cmd.find(p) != std::string::npos` substring matches. Trivially bypassed.

**Change:** Deleted `IsCommandBlocked` entirely along with both call sites (POSIX `ExecRunShell` and Windows `ExecRunShell`). Per memory `feedback_allowlist_not_blocklist`, blocklists fail open; the only legitimate security gate for `run_shell` is the human-approval flow at the controller layer (assistantController.h's approval mechanism — separately verified next sitting). The audit's recommended sandbox / enumerated-allowlist alternatives are larger architectural changes not scoped to this sitting; the right intermediate posture is "no false-confidence blocklist + must-have approval at controller layer", which is what this commit produces.

**Ramifications:**
- Callers touched: 2 (POSIX + Windows `ExecRunShell`).
- Tests touched: none direct. The approval gate at the controller layer (sitting 2 work) is the security control going forward.
- Docs touched: assistantTools.cpp section header for run_shell rewritten to document the security model explicitly.
- Blast radius if wrong: a user can now run any command they could have run before via the approval flow — same as the prior reality (the blocklist was non-functional). No new attack surface; just removal of theater.

**Tested by:**
- Build clean.
- grep confirms zero remaining references to `IsCommandBlocked`.

---

### [CRITICAL] CWD path-traversal check in `ExecRunShell` is ineffective — assistantTools.cpp:~1490 (POSIX) / ~1844 (Windows)

**Finding (paraphrased):** `cwdNorm.find("..") != std::string::npos` after `lexically_normal()` is ineffective: lexically_normal already resolves `..` syntactically, so a malicious `../../../etc` becomes `../../../etc` and is caught — but `/etc/passwd` is not. POSIX path additionally embeds the cwd into a `cd '...' &&` shell composition (CWD value of `foo' && evil_cmd && echo '` injects).

**Verification:** Holds up. POSIX: `std::string fullCmd = "cd '" + cwdPath.string() + "' && " + command;` is verbatim. Windows: same `find("..")` check + `cd '...' &&` (bash mode) or `Set-Location ...; ` (PowerShell mode), but cwd was also passed to CreateProcessA's lpCurrentDirectory — so the cd-in-shell composition was redundant.

**Change:** New helper `IsCwdInsideProjectRoot` does `fs::weakly_canonical(cwd)` against `fs::weakly_canonical(fs::current_path())` and rejects when the relative form is empty or starts with `..`. Defends against `..` segments, absolute paths to outside-project locations, AND symlinks pointing outside the project root (the audit's HIGH on `IsPathDenied` symlink bypass uses the same root cause; this fix closes the cwd-side of that hole).

POSIX: cwd is now applied via `chdir(canonicalCwd.c_str())` in the child before `execl("/bin/sh", "sh", "-c", command)`, never composed into the shell command. The audit's "CWD value would inject" exploit is structurally impossible.

Windows: same `IsCwdInsideProjectRoot` validation; `canonicalCwd.string()` is passed to CreateProcessA's `lpCurrentDirectory`. The redundant `cd '...' &&` (bash mode) and `Set-Location ...; ` (PowerShell mode) prefixes are dropped — they were doing the same thing as lpCurrentDirectory and were the only place a quote-escape failure could re-introduce CWD injection.

**Ramifications:**
- Callers touched: 2 (POSIX + Windows `ExecRunShell`); the helper is also reusable for future cwd validation sites.
- Tests touched: runtime smoke is the dashboard manual test (Verification gap below).
- Docs touched: section comment block rewritten for the new security model.
- Blast radius if wrong: a legitimate cwd inside the project root is rejected. Mitigation: `IsCwdInsideProjectRoot` returns a human-readable reason in `reasonOut` that the tool surfaces to the AI/user.

**Tested by:**
- Build clean.
- Hermetic dispatcher test PASS — confirms unrelated paths unbroken.
- Runtime smoke deferred to dashboard manual verification.

---

### Verification gap — assistant tool runtime smoke

**Issue:** the existing assistant test harness (`test/assistant/test_assistant.py`) connects via plain `ws://localhost:8080` and does not negotiate TLS or accept self-signed certs. j9t in this configuration runs HTTPS+WSS only on port 8443. The websocket-client library used by the test does support `sslopt={"cert_reqs": ssl.CERT_NONE}` but the test harness doesn't wire it through.

**Impact on this sitting:** the assistant-tool changes (search_files, list_files, run_shell) cannot be smoke-tested via the existing automated harness against the live server. The build is clean, the hermetic dispatcher path passes, and the changes are read-by-eye-correct, but a real end-to-end assistant chat exercising the tools has not run in this sitting.

**Recommendation:** JC drives a brief manual chat through the dashboard exercising:
- `search_files` with a normal query (e.g. `SanitizeUtf8` to find recent changes) — verify rg results look right
- `list_files` on the project root (depth 1 + 2) — verify output shape matches expectation
- `run_shell` with `ls -la` and a normal cwd (e.g. `.` or `application/`) — verify it works
- `run_shell` with a path-traversal cwd (e.g. `../..`) — verify it's rejected with a clean error

If any of these misbehaves, sitting 1 stays open and the changes get fixed before sitting 2 starts.

**Follow-up to track:** the test harness's TLS+self-signed gap is a real test-infra defect — the assistant tests have likely been silently un-runnable since the j9t HTTPS migration. Adding to the §5g remaining follow-ups list as a separate item.

---

## Skipped findings (this sitting)

| Finding | Severity | Reason for deferral |
|---|---|---|
| `IsPathDenied` symlink/case bypass | HIGH | Same file, same theme (canonical-path resolution), but adjacent rather than load-bearing for the CRITICAL cluster. Sitting 2. |
| `.bak`/`.tmp` files leak via deny-list miss | HIGH | Deny-list extension; depends on the canonical-path refactor in `IsPathDenied`. Sitting 2. |
| `ExecJcwfWriteScript` path-prefix validation | HIGH | Same canonical-path theme. Sitting 2. |
| `ExecJcwfGenerate` global.json string-concat JSON | HIGH | Different theme (JSON serialization) — folds into the simdjson-on-write work in the cybersec plan §8 (D3 keystore + config parser). Better addressed in S2 or as a Studio-edition-helper. Sitting 2 if quick; may slip to S2. |
| `ExecGetFileSummary` missing `IsPathDenied` call | HIGH | One-line fix but depends on the canonical-path refactor in `IsPathDenied`. Sitting 2. |
| Windows PowerShell `command` quoting (`-Command "..."`) | HIGH | Windows-specific refactor (`-EncodedCommand` or script-file pattern). Dedicated Windows sitting. |
| Prompt-injection-enables-tool-execution-without-approval | HIGH | Controller-layer fix (enforcing `requiresApproval` in `Execute()` plus stripping `<tool_call>` from re-fed content). Folds into assistantController.h sitting 2 work. |
| `ParseJsonString` missing `\uXXXX` | MEDIUM | Same file. Hand-rolled JSON parser is the legacy of pre-simdjson code; per memory `feedback_simdjson_only`, the right move is rewriting on simdjson — too large for this sitting. Sitting 2 or follow-up. |
| Task error messages reflected verbatim (`<tool_call>` injection via stdout) | MEDIUM | Output-side sanitization at `ExecGetTaskOutput` / `ExecGetRunStatus`. Sitting 2. |
| `contextAssembler.h` prompt-injection findings | MEDIUM/LOW | Different file. Sitting 2 or 3. |
| `workspaceIndexer.h` path traversal | HIGH | Different file. Sitting 2. |

All deferred items will land in sitting 2 of S1 (or later sittings if scope continues to expand).

---

## Sitting 1 wrap

**What landed:** 5 CRITICALs in `application/assistant/assistantTools.cpp`:
1. ExecSearchFiles → POSIX argv exec / Windows-bash with PosixSingleQuote escaping.
2. ExecListFiles → `std::filesystem::recursive_directory_iterator` (no exec).
3. ExecGetLogTail → deleted (was unreachable).
4. IsCommandBlocked → deleted (blocklist anti-pattern).
5. ExecRunShell CWD → IsCwdInsideProjectRoot canonical-path check + chdir() in child / lpCurrentDirectory (no `cd '...' &&` composition on either platform).

Plus one MEDIUM swept along the way (search_files grep fallback removed — was reintroducing the same vulnerability).

**Open boundary at session-end:** assistantTools.cpp HIGHs (deny-list canonical-path refactor, `.bak` leak, ExecJcwfGenerate/WriteScript paths), assistantController.h CRITICALs and HIGHs, assistantSession + assistantMemory, web/cloud surfaces, and the runtime-smoke verification gap noted above.

---

## Sitting 2 — assistantTools.cpp HIGHs (canonical-path theme + JSON-escape + tool-marker defang)

**Scope locked:** the 5 cyber-sec HIGH/MEDIUM findings in `application/assistant/assistantTools.cpp` that share the "canonical-path / external-content reflection" theme.  Pre-work first: fix the test harness's TLS+self-signed gap so we have automated regression coverage going forward.  assistantController.h CRITICALs (auth bypass, GetSession path traversal) deferred to sitting 3.

### [PRE] Fix test_assistant.py TLS+self-signed gap

**Finding:** `test/assistant/test_assistant.py` connected via plain `ws://localhost:8080`, did not negotiate TLS, did not accept self-signed certs, and had no Authorization-header plumbing.  Silently un-runnable since the j9t HTTPS migration.

**Verification:** Holds up — drove the test harness against the running TLS+auth j9t and got `Failed to connect: SSL_CERT_VERIFY_FAILED` then `400 Bad Request` (no auth) then "socket already closed" once auth landed (concurrent send/recv race).

**Change:**
- Default URL `wss://localhost:8443/ws/assistant`.
- `--token` flag + `J9T_TOKEN` env-var fallback; passed via `Authorization: Bearer <token>` header on the WebSocket handshake.
- `sslopt={"cert_reqs": ssl.CERT_NONE, "check_hostname": False}` when URL starts with `wss://`.
- `_ws_io_lock` (threading.Lock) serializing every `ws.send()` and `ws.recv()` site — websocket-client 1.7.0's WebSocket isn't thread-safe under TLS+concurrent send/recv (the ping-thread + main-thread race that previously dropped the connection within ~40 ms).

**Ramifications:**
- Callers touched: only the test harness; production code untouched.
- Tests touched: all 28 non-AI tests now run end-to-end (previously zero ran).
- Docs touched: none — the `--url` and `--token` flags self-document via `--help`.
- Blast radius if wrong: zero for production; test-only.

**Tested by:** `python3 test/assistant/test_assistant.py` — 28 passed, 0 failed in 2.1 s.

---

### [HIGH] `IsPathDenied` deny-list bypassable via symlinks and case variants — assistantTools.cpp:589

**Finding:** Deny-list checks `lexically_normal()` (purely syntactic) + string prefix/suffix matching.  No `fs::canonical()`, no case-folding, so `safe.txt → config.json` symlinks and `Config.JSON` on case-insensitive filesystems both bypass.

**Verification:** Holds up.  Original code: `fs::path(path).lexically_normal()` followed by `==` / `ends_with("/" + denied)` literal compares; ext compared by `==` (case-sensitive).

**Change:** Rewrite around `fs::weakly_canonical(projectRoot / path)` — resolves symlinks in any existing prefix.  Add a project-root-confinement check (paths that resolve outside the project root are denied — closes the structural gap that allowed `safe.txt → /etc/passwd`).  Filename and extension comparisons are now case-folded via `std::tolower`.  Subtree deny for `assistant/` runs against `lexically_relative(projectRoot)` so both POSIX and Windows separators are handled.  Added `.bak`, `.tmp` to denied extensions, plus per-base `.bak` / `.tmp` filenames (`config.json.bak`, `keys.json.bak`, etc.) — closes the audit's HIGH "ExecWriteFile/ExecEditFile write to backup .bak files predictably, leaking secrets".  Fail closed on any resolution error.

**Ramifications:**
- Callers touched: `ExecReadFile` (line 1134), `ExecWriteFile` (line 1772), `ExecEditFile` (line 1852), and now `ExecGetFileSummary` + `ExecJcwfWriteScript` via the next two changes in this sitting.
- Tests touched: 28 non-AI tests still pass (regression floor).  Direct path-denial tests would need AI-driven tool calls; deferred to JC dashboard verification or AI test suite.
- Docs touched: section comment block above `IsPathDenied` rewritten to document the fix and its rationale.
- Blast radius if wrong: ExecReadFile / ExecWriteFile / ExecEditFile reject legitimate paths.  Most likely failure mode: a path that resolves correctly but fails the `lexically_relative` empty-string check on Windows (different drive letters).  Mitigation: error message + the assistant retries with a different path, no data loss.

**Tested by:** Build clean.  28 non-AI assistant tests pass.  Hermetic dispatcher PASS (regression).

---

### [MEDIUM/exfiltration] `ExecGetFileSummary` reads and forwards arbitrary file content to external AI — assistantTools.cpp:1589

**Finding:** `WorkspaceIndexer::ReadFileContent(filePath, 32768)` is called without IsPathDenied gating.  AI-controlled `path` argument can read `.env` / `config.json` / `keys.json` and ship their contents to the external AI provider.

**Verification:** Holds up.  No deny-list call anywhere in `ExecGetFileSummary` body; the path goes straight from args into `ReadFileContent`.

**Change:** Add `if (IsPathDenied(filePath)) return {"get_file_summary", false, "Access denied: ..."};` before the cached-summary check (so a stale cache pre-dating the deny rule doesn't surface a denied file's summary either).

**Ramifications:**
- Callers touched: `ExecGetFileSummary` only.
- Tests touched: same as above.
- Docs touched: inline comment cites the audit.
- Blast radius if wrong: a sensitive file the AI legitimately wants summarized is rejected.  Same response as ExecReadFile — clean error.

**Tested by:** Build clean.  28 non-AI assistant tests pass.

---

### [HIGH] `ExecJcwfWriteScript` path validation only checks `scripts/` prefix — assistantTools.cpp:3020

**Finding:** `normalized.starts_with("scripts/")` on `lexically_normal()` output is purely syntactic; doesn't reject inputs that resolve outside scripts/ via crafted paths or symlinks; doesn't call IsPathDenied.

**Verification:** Holds up.  Original: `fs::path(path).lexically_normal()` then `starts_with("scripts/")` + `find("..") != npos` checks.  No canonical resolution.

**Change:**
1. Reject absolute paths early.
2. Resolve to absolute canonical form via `fs::weakly_canonical(projectRoot / path)`.
3. Compute `lexically_relative(scriptsRoot)` and reject empty/`..`-prefixed forms.
4. Run resolved path through `IsPathDenied` (catches `.bak` / `.tmp` plus the assistant subtree).

**Ramifications:**
- Callers touched: `ExecJcwfWriteScript` only.
- Tests touched: same as above.
- Docs touched: inline comment cites the audit.
- Blast radius if wrong: a legitimate script path is rejected.  The assistant retries with a corrected path.

**Tested by:** Build clean.  28 non-AI assistant tests pass.

---

### [HIGH] `ExecJcwfGenerate` writes `global.json` with raw string-concat workflowId — assistantTools.cpp:2624

**Finding:** `ofs << "{\n  \"version\": \"1.1\",\n  \"id\": \"" << workflowId << "\",\n  \"manual_start\": true\n}"` — no escaping.  AI-supplied workflowId containing `"`, `\`, or control chars produces malformed JSON or allows JSON injection (e.g. flipping `manual_start` to false).

**Verification:** Holds up.  Original code is verbatim string-concat.

**Change:** Define a `JsonEscape(std::string const&)` helper in the file's anonymous namespace (parallel to the existing one in `assistantSession.cpp`) and apply it: `ofs << "..." << JsonEscape(workflowId) << "...";`.  RFC 8259 string-content escape — handles `"`, `\\`, `\\n`, `\\r`, `\\t`, and 0x00-0x1F via `\\u%04x`.  Convergence note: this is now the third copy of `JsonEscape` in the codebase (assistantSession.cpp + workspaceIndexer.cpp + here); the engine's `JsonHelper::SanitizeForJson` is broken (silently drops form-feed, missing control-char handling).  Tracked as a follow-up rather than rolled in to keep this sitting's diff bounded.

**Ramifications:**
- Callers touched: `ExecJcwfGenerate` only.
- Tests touched: same as above.
- Docs touched: inline comment cites the audit.
- Blast radius if wrong: `global.json` becomes malformed for normal workflowId values.  Mitigation: workflowId values are typically `[A-Za-z0-9_]+` and pass through unchanged.

**Tested by:** Build clean.  28 non-AI assistant tests pass.

---

### [MEDIUM] Task error messages reflected verbatim into AI-facing tool result — assistantTools.cpp:1149 / 972

**Finding:** `ExecGetTaskOutput` and `ExecGetRunStatus` reflect `m_LastErrorMessage` / `m_CapturedStdout` / `m_CapturedStderr` verbatim into the tool result.  A script that prints `<tool_call>...</tool_call>` to stdout (deliberately or via log injection) becomes a parsed tool call on the next AI turn.  Indirect prompt injection.

**Verification:** Holds up.  Both functions concatenate raw external bytes into the AI-facing string.

**Change:** Define `DefangToolMarkers(std::string const&)` in the file's anonymous namespace.  Replaces the literal sequences `<tool_call>`, `</tool_call>`, `<tool_result>`, `</tool_result>` with their U+27E8 / U+27E9 mathematical-angle-bracket equivalents (UTF-8: `\xE2\x9F\xA8` / `\xE2\x9F\xA9`).  Visual content preserved; the ASCII byte sequences the parser keys on are gone.  Applied at every reflection site in `ExecGetTaskOutput` (3 sites: error message + stdout + stderr) and `ExecGetRunStatus` (1 site: per-task error message).

**Ramifications:**
- Callers touched: `ExecGetTaskOutput` + `ExecGetRunStatus` only.
- Tests touched: same as above.
- Docs touched: inline comment cites the audit.
- Blast radius if wrong: a script that legitimately prints `<tool_call>` (e.g. a tool-syntax tutorial) gets visually rendered with mathematical brackets instead of ASCII brackets.  No data loss; cosmetic.

**Tested by:** Build clean.  28 non-AI assistant tests pass.  Hermetic dispatcher PASS.

---

## Skipped findings (sitting 2)

| Finding | Severity | Reason for deferral |
|---|---|---|
| Windows PowerShell `command` quoting (`-Command "..."`) | HIGH | Windows-specific refactor (`-EncodedCommand` or script-file pattern).  Dedicated Windows sitting. |
| Prompt-injection-enables-tool-execution-without-approval | HIGH | Controller-layer fix (enforcing `requiresApproval` in `Execute()` plus stripping `<tool_call>` from re-fed content).  Folds into assistantController.h sitting 3 work — the approval-bypass CRITICAL is the same lever. |
| `ParseJsonString` missing `\uXXXX` | MEDIUM | Hand-rolled JSON parser; right move per memory `feedback_simdjson_only` is rewriting on simdjson.  Too large for this sitting; sitting 3 or follow-up. |
| `contextAssembler.h` prompt-injection findings | MEDIUM/LOW | Different file; in scope for D2 but better paired with assistantController.h work in sitting 3. |
| `workspaceIndexer.h::ReadFileContent` path traversal | HIGH | Different file.  Pairs with the now-rewritten `IsPathDenied` — sitting 3 picks it up. |
| Convergence of three `JsonEscape` copies | discipline | Refactor across three files.  Tracked but not bundled to keep this sitting's diff focused. |

---

## Sitting 2 wrap

**What landed:**
1. Test harness TLS+self-signed gap fixed → 28 non-AI assistant tests now pass.
2. `IsPathDenied` rewritten on `fs::weakly_canonical` + project-root-confinement + case-folded comparison + `.bak`/`.tmp` extension denies + per-base `.bak`/`.tmp` filenames.
3. `ExecGetFileSummary` now calls `IsPathDenied` before reading + forwarding to AI.
4. `ExecJcwfWriteScript` now does canonical scripts/ confinement + `IsPathDenied`.
5. `ExecJcwfGenerate` now JSON-escapes `workflowId` before embedding in `global.json`.
6. `ExecGetTaskOutput` + `ExecGetRunStatus` defang `<tool_call>` / `</tool_call>` / `<tool_result>` / `</tool_result>` markers in reflected external bytes.

`assistantTools.cpp` is now closed for cyber-sec audit findings — every CRITICAL and every HIGH from the audit's §05 has either landed (sittings 1+2) or is explicitly tracked as deferred (Windows PowerShell quoting, ParseJsonString simdjson rewrite, JsonEscape convergence).

**Open boundary at sitting-end:** assistantController.h (CRITICAL approval bypass, CRITICAL GetSession path traversal, plus the safety-side HIGHs: lifetime captures, lock-order inversions, CV wakeups, WS client-pointer races, thread-vector growth), `workspaceIndexer.h::ReadFileContent` (HIGH), `contextAssembler.h` (MEDIUM/LOW), web/cloud surfaces.

---

## Sitting 3 — assistantController.{h,cpp} controller-layer security funnel

**Scope locked:** every CRITICAL and HIGH cyber-sec finding in `application/assistant/assistantController.{h,cpp}` (audit §02) plus the load-bearing concurrency-safety HIGHs that share the same code regions.  Boundary: "controller-layer security funnel — auth-binding, lifetime, and external-content reflection in this file".  The cross-component thread-pool refactor (`JoinFinishedThreads` → engine `ThreadPool` per memory `feedback_no_jthread_use_threadpool`), the broader session-lock redesign, and the `ToolRegistry`/`MemoryStore`/`WorkspaceIndexer` thread-safety contract audit are explicitly deferred to a later sitting — they cross component boundaries and would balloon this sitting beyond the documented "one file, closeable cluster" pattern.

**Stale audit findings noted up front:**
- *HIGH "Unauthenticated WebSocket endpoint"* — stale.  `webServer_studio.cpp:760` `.onaccept` already gates the upgrade via `Authenticate(req)` / `LOG_SECURITY_WARN` and rejects unauthenticated handshakes.  Per memory `feedback_auth_funnel_one_gate` the right place is the upgrade gate (one funnel per surface), not bespoke per-handler checks.  The audit was generated before the gate landed.  Recorded in skipped table; no controller-side change.

### [PRE] Sweep audit traces from sittings 1+2 + new memory

**Finding:** during sitting 3 JC pointed out that the audit-citing comments I'd been adding in sittings 1+2 ("Cyber-sec audit (assistantTools.h §05) HIGH:", `[CRITICAL]`, severity tags inline, "this addresses MEDIUM #X", etc.) are *change-trace*, not load-bearing context — water under the bridge in no time.  The session note + audit doc are the right place for the trace; the source code should read like "this is how it is" without referring back to the document that drove the change.

**Verification:** held up.  Greps showed 12 sites in `assistantTools.cpp` (across sittings 1+2) plus several I'd just added in sitting 3 itself (controller, header).  All cited audit § / severity tags with no operational value to a future reader.

**Change:** stripped every audit citation, severity tag, sitting/session ref, and "memory `feedback_X`" inline note from the working tree.  Where the *invariant* the comment was protecting is genuinely non-obvious (allowlist rationale on `IsValidSessionId`, defense-in-depth note on canonical-path containment, "originConn never dereferenced — pointer identity only", "send_text re-checks membership under lock"), kept the rule in present-tense plain English.  Where the comment was just narrating what nearby code already shows, deleted the comment entirely.  `LOG_SECURITY_*` runtime tags (`assistant_session_invalid_id`, `assistant_approval_wrong_connection`, etc.) stay — those are operator signals, not change-trace.  One comment exception: `JCWF spec §3.1.2` reference in `shellTaskExecutor.cpp` stays (spec is stable; audit isn't).

**Memory landed:** `feedback_no_audit_traces_in_code.md` — new feedback memory making the rule explicit so future sittings don't reintroduce the pattern.  Body: "code comments must not cite audit findings, refactor numbers, session notes, or any session-tracking artifact — the why must stand on its own".  Indexed in `MEMORY.md`.

**Tested by:** Studio debug build clean.  Comments are non-functional; mechanical scan via `grep -nE '§|\[HIGH\]|cyber-sec|audit|...'` returns no matches across the affected files.  No code semantics changed.

---

### [CRITICAL] `HandleLogCommand` shell injection via popen+tail — assistantController.cpp:~640

**Finding:** `tail -N 'log/log.txt'` composed as a string and passed to `popen` (with `WrapForBash` on Windows wrapping the whole thing in `bash -c "..."`).  Hardcoded path today, but the pattern is latent — a configurable log path or a path containing a quote breaks the quoting and injects shell commands.  `HandleLogCommand` also returned `"Log file not found: " + logPath.string()` to the WS client (LOW info disclosure).

**Verification:** Holds up. Original code is verbatim popen+tail with single-quote-wrapped path.

**Change:** Replaced with a pure C++ seek-tail.  `std::ifstream` opens the log binary, seeks to EOF, walks backwards in 4 KB chunks counting newlines until N+1 are seen or BOF is reached, then returns the trailing tail capped at 32 KB.  No popen, no `WrapForBash`, no platform fork between bash and tail.  User-visible error is `"Log file not available."` — the absolute path goes only to the server-side `LOG_APP_INFO` for operator triage.  `WrapForBash` and the `popen` / `pclose` Windows aliases are deleted (both unused after this change).

**Ramifications:**
- Callers touched: `HandleLogCommand` only.
- Tests touched: regression path is the assistant `/log [N]` slash command — exercised via dashboard chat or via a future test harness command call.
- Docs touched: none.
- Blast radius if wrong: `/log` returns truncated or empty output.  Mitigation: the seek-tail short-circuits cleanly on `tellg() <= 0` (empty file) and on an EOF-walk overflow (caps at 32 KB).

**Tested by:** Studio debug build clean.

---

### [CRITICAL] `GetSession` path traversal via unsanitised sessionId — assistantController.cpp:~1845

**Finding:** `sessionsDir / (sessionId + ".jsonl")` with `sessionId` taken straight from the WS payload.  An ID of `../../etc/passwd` resolves outside the sessions directory.  Even on a miss, the `fs::exists` probe is a file-existence oracle.

**Verification:** Holds up.  Originals use the raw concatenation with no validation.

**Change:** New anonymous-namespace `IsValidSessionId(s)` enforces strict allowlist `[A-Za-z0-9_-]{1,128}` — every character outside that set is rejected.  Path-traversal sequences (`/`, `\\`, `..`) are excluded by construction.  Defense-in-depth: after building `sessionsDir / (sessionId + ".jsonl")`, the resolved path is canonicalised (`fs::weakly_canonical`) and compared against the canonical sessions dir; any escape returns nullptr with a `LOG_SECURITY_WARN("[security] assistant_session_path_escape ...")`.  Invalid IDs log `[security] assistant_session_invalid_id length=...` (length only, never the value).  Failure modes are silent to the WS client (returns nullptr; the existing handlers either fall through to `CreateSession` or surface a "Session not found" error that doesn't leak the rejected value).

**Ramifications:**
- Callers touched: every caller of `GetSession` already handles nullptr.
- Tests touched: `HandleResumeSession` / `HandleUserMessage` / `HandleListSessions` all funnel through `GetSession` — the existing 28-test assistant suite covers protocol regression.
- Docs touched: none.
- Blast radius if wrong: a legitimate sessionId is rejected.  IDs generated by `AssistantSession::GenerateSessionId` follow `sess_<ms>_<n>` which falls inside the allowlist; manual or test IDs that include disallowed characters would be rejected (intentional).

**Tested by:** Studio debug build clean.

---

### [HIGH] Approval bypass via unauthenticated `approval_response` — assistantController.{h,cpp}

**Finding:** Any connected client could send `{"type":"approval_response","requestId":"apr_<seq>","approved":true}` to approve a pending tool call initiated by *another* client.  The `requestId` was a sequential `apr_<int>` derived from `m_NextApprovalSeq.fetch_add(1)`, trivially guessable.  No connection-binding on the approval.

**Verification:** Holds up.  Code reads exactly as the audit describes.

**Change:**
- `PendingApproval` struct grows a `crow::websocket::connection* originConn` (and an `originSessionId` for completeness).  Stored as identity only — never dereferenced.  Set at `RequestToolApproval` time from the connection that triggered the user_message.
- `RunAiCallAsync` takes the originating `crow::websocket::connection*` and threads it via lambda capture into `RequestToolApproval`.  `HandleUserMessage` passes `&conn`.
- `HandleApprovalResponse(conn, requestId, approved)` rejects responses where `approval->originConn != &conn` with a `LOG_SECURITY_WARN("[security] assistant_approval_wrong_connection requestId_prefix=...")`.  An unknown `requestId` logs `assistant_approval_unknown_request` (no echo of the requestId itself).
- `m_NextApprovalSeq` removed from the header.  `requestId` is now `"apr_" + RandomHex(16)` (128-bit cryptographically random hex via OpenSSL `RAND_bytes`).  RAND_bytes failure fails closed (returns `false` from `RequestToolApproval`).
- New `OnClose` hook → `CancelApprovalsForConnection(&conn)` walks `m_PendingApprovals` and fail-closes any owned by the disconnecting client.  Without this, a closed-mid-flow client would leave the background AI loop hanging on the 60-second CV timeout, *and* a future connection that reuses the same pointer address could match by identity and approve a stale request.
- `Shutdown` notify_all moved out of `m_ApprovalsMutex` (snapshot the shared_ptrs under lock, notify outside) — the predicate already checks the `m_ShuttingDown` atomic, so the notify needs no lock.

**Ramifications:**
- Callers touched: `HandleUserMessage` (passes `&conn`), `RunAiCallAsync` (lambda capture), `OnMessage` (passes `conn` to `HandleApprovalResponse`), `OnClose` (calls `CancelApprovalsForConnection`).  Header signatures of `RunAiCallAsync` / `RequestToolApproval` / `HandleApprovalResponse` updated.
- Tests touched: 28-test assistant regression covers the protocol surface; the approval-flow itself is exercised end-to-end only via the AI suite (`--with-ai`) or by manual dashboard chat.
- Docs touched: none.
- Blast radius if wrong: a legitimate user's approval is rejected.  Mitigation: the connection match is exact (pointer identity); the only way for it to fail is if the client reconnected mid-flow, in which case `OnClose` would have already fail-closed the approval.

**Tested by:** Studio debug build clean.

---

### [HIGH] Unbounded WS frame size + maxEntries clamp — assistantController.cpp OnMessage / get_history

**Finding:** `OnMessage` accepts `std::string const& data` of unbounded size and constructs `simdjson::padded_string(data)` directly — a multi-MB or multi-GB frame triggers an unbounded heap allocation.  `HandleGetHistory` cast a client-supplied `int64_t` to `int` with no lower bound; a negative value made the loop's `static_cast<int>(history.size()) >= maxEntries` always false → unbounded scan.

**Verification:** Holds up.  Both code paths read as the audit describes.

**Change:**
- `OnMessage` checks `data.size() > 64 KB` first, logs `[security] assistant_ws_frame_too_large bytes=...`, queues an error message, and returns before the simdjson allocation.
- The `get_history` branch clamps `maxEntries` with `std::clamp<int64_t>(val, 1, 500)` *before* casting to `int`.

**Ramifications:**
- Callers touched: `OnMessage` body only.
- Tests touched: 28-test suite covers normal frame sizes; oversized-frame and negative-`maxEntries` paths are negative-input paths (no existing test, but the fix is mechanical).
- Docs touched: none.
- Blast radius if wrong: a legitimate caller sending a >64 KB frame gets rejected.  64 KB covers the largest plausible user_message + slash-command shape; oversized inputs are a DoS attempt or misbehaving client.

**Tested by:** Studio debug build clean.

---

### [HIGH] Session lifetime — `unique_ptr` → `shared_ptr` for safe lambda capture

**Finding:** `m_Sessions` stored sessions as `unique_ptr<AssistantSession>`.  `GetSession` returned a raw `AssistantSession*` that the background AI lambda held across blocking `RunSingleAiCall` and `RequestToolApproval` calls (each up to 120 s).  A concurrent `Shutdown` (or future eviction) could destroy the session while the background thread is still using it — use-after-free on `session->AddAssistantMessage(...)`, etc.

**Verification:** Holds up.  Lambda captures and holds the raw pointer across blocking calls.

**Change:**
- `m_Sessions` switched to `unordered_map<std::string, shared_ptr<AssistantSession>>`.
- `GetSession` and `CreateSession` return `shared_ptr<AssistantSession>`.
- The background lambda in `RunAiCallAsync` declares `std::shared_ptr<AssistantSession> session = GetSession(sid)` so the session stays alive for the lambda's lifetime regardless of m_Sessions evictions or Shutdown ordering.
- All other callers in `assistantController.cpp` updated from `auto* session` / `AssistantSession*` to `auto session` (shared_ptr arithmetic is interchangeable for the `if (session)` and `session->...` patterns the callers use).
- `HandleListSessions` now snapshots the in-memory `m_Sessions` map under `m_SessionsMutex` once, then iterates outside the lock — closes the lock-order TOCTOU between "id appears in list" and "GetSession returns a valid pointer" without holding the controller mutex across `GetTurnCount()`.
- `HandleCompletionRequest`'s history branch snapshots session shared_ptrs under `m_SessionsMutex`, then iterates `GetAllTurns()` outside the lock — closes the "controller-mutex-held while session-mutex-acquired" lock-order inversion.
- `OnOpen` / `OnClose` read `m_Clients.size()` *inside* the lock scope (data-race fix).

**Ramifications:**
- Callers touched: 10 call sites in `assistantController.cpp` plus the 2 definitions; header types updated.
- Tests touched: 28-test suite covers the protocol-level session APIs; the lifetime fix is exercised on every AI call.
- Docs touched: none.
- Blast radius if wrong: a session pointer outlives `m_Sessions` only for as long as the lambda holds it (correct behaviour).  Memory pressure is bounded by `MAX_TOOL_ITERATIONS = 10` per AI call.

**Tested by:** Studio debug build clean.

---

### [HIGH] DrainPendingMessages connection-pointer revalidate — assistantController.cpp:~1820

**Finding:** Snapshot of `m_Clients` under `m_ClientsMutex`, lock released, then `client->send_text(batch)` called on each pointer.  A concurrent `OnClose` between snapshot and dereference can free the connection — use-after-free.

**Verification:** Holds up.

**Change:**  Per-client membership re-check under `m_ClientsMutex` immediately before each `send_text`.  Same pattern `WebServer::DrainPendingBroadcasts` already uses for its `/ws` broadcast loop.  The race window is narrowed to the moments between the lock release and the network call; the broader codebase relies on Crow's deferred-destruction semantics to make even that window safe.  If a tighter guarantee is wanted, we can revisit and hold the lock for the whole loop (cost: blocks OnOpen/OnClose for the duration of multi-MB sends to N clients).

**Ramifications:**
- Callers touched: `DrainPendingMessages` only.
- Tests touched: regression covered by 28-test suite (every test exercises send + drain).
- Docs touched: none.
- Blast radius if wrong: a stale-pointer send still races identically to the WebServer broadcast path, which has been in production for the lifetime of the project.  No regression vs. the prior state.

**Tested by:** Studio debug build clean.

---

### [MEDIUM] Tool-result XML defang + HandleRunsCommand default arm

**Finding (defang):** `RunAiCallAsync` builds `<tool_result name="...">{result.output}</tool_result>` blocks with no escaping of the inner bytes.  A script that prints `</tool_result>` to stdout closes the wrapper early; a script that prints `<tool_call>` becomes a parsed tool call on the next AI turn.  Indirect prompt injection.

**Finding (switch):** `HandleRunsCommand`'s switch over `WorkflowRunState` had a `default:` arm — adding a new state silently produces "unknown" instead of breaking the build.  Memory `feedback_cpp_discipline`.

**Verification:** Both hold up.

**Change:**
- The anonymous-namespace `DefangToolMarkers` helper in `assistantTools.cpp` (sitting 2) is the second use site → triggers the C++ discipline rule.  Promoted to a public static `ToolRegistry::DefangToolMarkers`.  All 4 prior call sites in `assistantTools.cpp` resolve via unqualified name lookup (member functions of the same class).  `RunAiCallAsync` calls `ToolRegistry::DefangToolMarkers(result.output)` before embedding into the `<tool_result>` block.
- `HandleRunsCommand` switch: dropped the `default:` arm.  The compiler's `-Wswitch` warning now flags any unhandled `WorkflowRunState` enumerator the next time someone adds one.

**Ramifications:**
- Callers touched: `RunAiCallAsync` (one site) + the 4 existing tools.cpp sites that now resolve to the static method.
- Tests touched: 28-test suite still passes (the protocol surface is unchanged).
- Docs touched: none.
- Blast radius if wrong: legitimate `<tool_call>` content in tool output gets rendered with mathematical brackets instead of ASCII brackets — cosmetic, no data loss.  An unhandled `WorkflowRunState` becomes a build break (intended).

**Tested by:** Studio debug build clean.

---

### [MEDIUM] QueueMessage cap + log redaction + WriteFile flush

**Findings:**
- *QueueMessage unbounded growth:* background AI threads call `QueueMessage` from inside the tool loop; if a client sends nothing further, `m_PendingMessages` grows until the next `OnMessage` (which may never come for a long-running tool batch).
- *Sensitive content at INFO:* user-message body (`msg.substr(0, 60)`) and memory `key=value` bodies were logged at INFO; a user pasting an API key or a memory entry storing a secret persists fragments to log files.
- *WriteFile flush:* `std::ofstream` flushes-via-destructor swallow errors; `outError` echoed the absolute filesystem path back to the caller.

**Verification:** All three hold up.

**Change:**
- `QueueMessage` caps at 10000 pending messages.  Overflow logs `LOG_APP_ERROR("[assistant] Pending message queue full (>={}); dropping message", ...)` and drops the new message.  The cap is well above any plausible session and below "process OOM".
- Memory recall log redacted: was `LOG_APP_INFO(... msg.substr(0,60), {} matches, total {})` and per-memory `[{}] = {}` lines; now `LOG_APP_INFO("[assistant] Memory recall: {} matches (total store: {}, query_len={})", ...)` plus a one-line `Injected {} memories into context`.  Memory keys/values + user-message text never reach the log.
- Approval log redacted: was `Approval requested: {} — {}` (full requestId + full description with tool args); now `requestId_prefix={}` (first 8 hex chars) plus `tool={}` (name only).  Same on `Approval timed out` and `Approval result`.
- `WriteFile` calls `ofs.flush()` explicitly and checks `ofs.good()` after; failures emit `LOG_APP_ERROR("[assistant] WriteFile flush failed: path='{}' bytes={}", ...)` server-side and return the generic `"File write error"` to the caller (no path leak).

**Ramifications:**
- Callers touched: `QueueMessage`, `RequestToolApproval` (3 log sites), the AI lambda (memory log sites), `WriteFile`.
- Tests touched: regression covered by 28-test suite (every test produces queued messages and write activity).
- Docs touched: none.
- Blast radius if wrong: an oversized queue silently drops messages instead of OOMing — a UX regression but not a security one.  Logs are quieter; the prior fields can be reconstructed from the full requestId at the dashboard if ever needed.

**Tested by:** Studio debug build clean.

---

## Skipped findings (sitting 3)

| Finding | Severity | Reason for deferral |
|---|---|---|
| Unauthenticated WebSocket endpoint | HIGH | Stale audit — `webServer_studio.cpp:760` `.onaccept` already gates the upgrade via `Authenticate(req)`.  No controller-side change needed. |
| `JoinFinishedThreads` is a no-op; thread vector grows without bound | MEDIUM safety | Reuse the engine `ThreadPool` per memory `feedback_no_jthread_use_threadpool`.  Cross-component refactor — better in its own sitting where the contract change can be reviewed end-to-end. |
| Background lambda captures `m_ToolRegistry` / `m_MemoryStore` / `m_WorkspaceIndexer` without per-class thread-safety contract | MEDIUM safety | Document and audit those three classes' thread-safety; potentially serialise via a per-class mutex.  Bigger surface than this sitting. |
| QueueMessage drain CV/timer (responses sit until next OnMessage) | MEDIUM | A genuinely useful UX fix but orthogonal to the controller-layer security funnel.  Pair with a WS-layer drain timer in a focused sitting. |
| TOCTOU in `GetSession` between `fs::exists` and `AssistantSession` constructor | HIGH safety | Mitigated structurally by the new sessionId allowlist (no path-traversal vector).  The remaining "TOCTOU between probe and ctor" is a benign load-or-skip race; the audit's recommended fix moves the open into `AssistantSession`'s ctor — a refactor that belongs with the next sitting on `assistantSession.h`. |
| `HandleCompletionRequest` snapshot consistency (other branches still iterate workflowRegistry / sessions outside the lock) | MEDIUM | Only the *history* branch held `m_SessionsMutex` while calling `GetAllTurns()`; that's fixed.  The slash + workflow branches are not under any lock-order risk and are out of scope. |
| Convergence of three `JsonEscape` copies + the new `RandomHex` helper (third copy) | discipline | Both are flagged by `feedback_cpp_discipline`'s "third copy" rule.  Tracked but not bundled — a `engine/util/secureRandom.h` + fix to the broken `JsonHelper::SanitizeForJson` is a focused convergence sitting. |
| `workspaceIndexer.h::ReadFileContent` path traversal | HIGH | Different file.  Pairs naturally with the now-rewritten `IsPathDenied` and is a one-liner once the workspace root is canonicalised — but still a separate file for sitting boundary discipline. |
| `contextAssembler.h` prompt-injection findings | MEDIUM/LOW | Different file. |

---

## Sitting 3 wrap

**What landed:**
0. Audit-trace sweep across sittings 1+2+3 (PRE-step) + new memory `feedback_no_audit_traces_in_code`.
1. `HandleLogCommand` popen → ifstream seek-tail; `WrapForBash` and the popen Windows aliases deleted.
2. `GetSession` strict allowlist regex on sessionId + canonical-path containment under sessions dir.
3. Approvals bound to originating `crow::websocket::connection*` + 128-bit `RAND_bytes` hex requestId; `OnClose` cancels pending approvals for the disconnecting client.  `Shutdown` notify_all moved out of `m_ApprovalsMutex`.
4. Incoming WS frame capped at 64 KB before simdjson allocation.  `get_history` clamps `maxEntries` with `std::clamp<int64_t>(val, 1, 500)` before the cast.
5. `m_Sessions` storage `unique_ptr<AssistantSession>` → `shared_ptr<AssistantSession>`; lambda captures keep sessions alive across blocking calls.  `HandleListSessions` / `HandleCompletionRequest` snapshot under lock then iterate outside.  `OnOpen` / `OnClose` read `m_Clients.size()` inside the lock.
6. `DrainPendingMessages` per-client re-check under `m_ClientsMutex` before each `send_text` (matches `WebServer::DrainPendingBroadcasts`).
7. `DefangToolMarkers` promoted to public `ToolRegistry::DefangToolMarkers`; controller uses it on `result.output` before XML embedding.  `HandleRunsCommand` `default:` arm removed.
8. `QueueMessage` capped at 10k.  Memory recall + approval logs redacted (counts/lengths/prefixes only — no message text, no memory values, no full requestId).  `WriteFile` explicit flush + generic error string.

**What's verified:**
- Studio debug build clean (`make config=debug`); only `assistantController.cpp` + `assistantTools.cpp` + `webServer{,_studio}.cpp` + `jarvisAgent.cpp` recompile, link succeeds.
- 28-test assistant non-AI suite: **PASS** end-to-end (`python3 test/assistant/test_assistant.py` → 28 passed, 0 failed in 2.1 s) against the new binary.  Covers every controller protocol path: user_message, command, list_sessions, resume_session, new_session, approval_response (the now-hardened path), get_history (the now-clamped path), completion_request, plus all 11 slash commands including `/log` (the rewritten popen → ifstream reader).
- `python3 test/dispatch/test_testinterface_hermetic.py`: **PASS** — adjacent dispatcher path unbroken.
- **Not directly verified:** the AI-driven runtime path that exercises `RequestToolApproval` end-to-end with the connection-binding check, the WS frame-size rejection, and the tool-result defang on real tool stdout — these need either `--with-ai` or a manual dashboard chat session.  JC to drive that pre-commit if appetite allows.

**Verification gotcha:** keystore unlock is not auto-driven by `JARVIS_MASTER_PASSWORD` on startup — needs an explicit `POST /api/settings/keys/unlock` with `{"master_password": "..."}` before any MCP-key-authenticated REST/WS request will pass.  Worth knowing on a fresh-launch test run.

**Open boundary at sitting-end:** `assistantController.{h,cpp}` is now closed for cyber-sec audit findings — every CRITICAL and HIGH from §02 has either landed or is explicitly tracked above.  Remaining open clusters: `assistantSession.h` (HIGH path traversal in ctor + HIGH weak random session ID + MEDIUM/LOW data integrity), `assistantMemory.h` (HIGH RNG race + HIGH lock inversion + MEDIUM JSON escape), `workspaceIndexer.h::ReadFileContent` (HIGH), `contextAssembler.h` (MEDIUM/LOW), then the broader web/cloud surfaces.

---

## Sitting 4 — assistantSession + assistantMemory + workspaceIndexer + contextAssembler

**Scope locked:** the four remaining D2 assistant-side files in the cyber-sec + safety audits — `assistantSession.{h,cpp}`, `assistantMemory.{h,cpp}`, `workspaceIndexer.{h,cpp}`, `contextAssembler.{h,cpp}`.  Deferred per the previous sitting's hand-off; they cluster naturally because they share three load-bearing patterns (path anchoring, JSON escaping, lock-from-ctor) that are best fixed in one pass.  Boundary: this sitting closes every HIGH-severity finding plus the load-bearing MEDIUMs in those four files.  Sitting 5 picks up the cross-component refactors (`JoinFinishedThreads → engine ThreadPool`, broader thread-safety contract audit) and the rest of D2 surface (web/cloud).

A pre-step landed first: convergence of the 4 broken `JsonEscape` copies (`engine/json/jsonHelper.cpp::SanitizeForJson` + `assistantMemory.cpp::JsonEscapeMem` + `workspaceIndexer.cpp::JsonEscapeIdx` + `assistantSession.cpp::JsonEscape`) into a single RFC 8259-compliant helper.  Memory `feedback_cpp_discipline`'s "refactor to one helper before adding a third copy" rule was past due.

A second pre-step lifted `RandomHex(numBytes)` from `assistantController.cpp`'s anonymous namespace to a new `application/assistant/assistantHelpers.{h,cpp}` so `assistantSession.cpp` and `assistantMemory.cpp` can both replace their broken RNGs without code duplication.  The strict opaque-ID allowlist (`IsValidOpaqueId`) followed the same path — was previously inline in `assistantController.cpp::IsValidSessionId`, generalised as a free function so all three files use the same check.

### [PRE-STEP] JsonEscape four-copy convergence — engine/json/jsonHelper.{h,cpp}

**Finding (paraphrased):** Four copies of `JsonEscape`/`JsonEscapeMem`/`JsonEscapeIdx`/`SanitizeForJson` exist across `application/assistant/` and `engine/json/`.  Three of them (memory, indexer, session) miss the RFC 8259 §7 requirement to escape every codepoint U+0000–U+001F as `\uXXXX` — they pass through control bytes verbatim, producing invalid JSON that simdjson silently rejects on reload.  The existing `JsonHelper::SanitizeForJson` is itself broken: line 35 has a literal 0x0C (form-feed) as the case label that drops the byte instead of escaping it.

**Verification:** Confirmed.  Read all four implementations: only `assistantTools.cpp::JsonEscape` (added in sitting 2) emits `\u%04x` for control chars.  The `SanitizeForJson` literal 0x0C label is visible in the raw file — the `default:` arm copies the byte through, so any value containing 0x01–0x08, 0x0B, 0x0E–0x1F bytes (e.g., from an AI provider returning embedded escape sequences in its raw output, or from a webhook payload) generates malformed JSON.  This affects `application/json/requestBuilder.cpp` and `application/workflow/aiTranscript.cpp`, both of which build outbound AI request bodies and persisted transcripts — i.e., a load-bearing JSON path for every AI call.

**Change:** Rewrote `engine/json/jsonHelper.cpp::JsonHelper::SanitizeForJson` as a thin wrapper around a new `static std::string EscapeJsonString(std::string_view)` that does proper RFC 8259 escaping (the four shorthand cases `"`/`\\`/`\n`/`\r`/`\t` plus `\u00XX` for every other control byte).  Existing call sites continue to compile unchanged because `SanitizeForJson` remains a member method that delegates to the static.  New use sites (session/memory/indexer) call `JsonHelper::EscapeJsonString(x)` directly.

Removed the local `JsonEscapeMem` from `assistantMemory.cpp`, `JsonEscapeIdx` from `workspaceIndexer.cpp`, and the local `JsonEscape` in `assistantSession.cpp` anon namespace; all three now route through the shared helper.  `assistantTools.cpp`'s anon-namespace `JsonEscape` and `assistantController.cpp`'s anon-namespace `JsonEscape` are intentionally left in place this sitting — both already correct after sitting 2/3, and migrating them would balloon the diff into hundreds of QueueMessage call-site edits.  Tracked as a follow-up; the discipline rule is satisfied because every NEW escape site uses the central helper.

**Ramifications:**
- Callers touched: `engine/json/jsonHelper.{h,cpp}` (rewrite); `application/json/requestBuilder.cpp` (no source change — the underlying behaviour upgrades transparently); `application/workflow/aiTranscript.cpp` (same).
- Tests touched: 28-test assistant suite covers the persisted JSONL path indirectly via session save/resume; dispatcher hermetic test exercises the request-body path.  Both pass.
- Docs touched: `engine/json/json.md` is generated documentation per memory `feedback_combined_doc_generated` — left unmodified; will refresh on next jarvisCppDocu run.
- Blast radius if wrong: outbound AI request bodies.  Failure mode would be either malformed JSON (caught by provider) or new escapes that providers don't recognize — but `\u00XX` is universally recognized JSON.  Conservative win.

**Tested by:**
- Studio debug build clean.
- 28-test assistant suite PASS (uses the rewritten path implicitly via session save/load).
- Hermetic dispatcher test PASS (uses `JsonHelper::SanitizeForJson` via requestBuilder).

---

### [PRE-STEP] RandomHex + IsValidOpaqueId extraction — application/assistant/assistantHelpers.{h,cpp}

**Finding (paraphrased):** Sitting 3 added `RandomHex(numBytes)` and `IsValidSessionId(s)` to `assistantController.cpp`'s anonymous namespace.  Sitting 4 needs both at three new sites (`assistantSession::GenerateSessionId`, `MemoryStore::GenerateId`, `AssistantSession::AssistantSession(resume)`, plus the `ListSessions` directory filter).  Memory `feedback_cpp_discipline` "refactor to one helper before adding a third copy" applies.

**Change:** Created `application/assistant/assistantHelpers.{h,cpp}` containing both helpers in `namespace AIAssistant`.  `IsValidSessionId` was renamed to `IsValidOpaqueId` since it now applies to session IDs, memory IDs, approval requestIds, and any future opaque assistant identifier — strict allowlist `[A-Za-z0-9_-]{1,128}`.  Updated `assistantController.cpp` to drop both local definitions and include the new header; the controller's `GetSession` now calls `IsValidOpaqueId(sessionId)` (same byte-equivalent contract).

**Ramifications:**
- Callers touched: `assistantController.cpp` (drop 2 anon-namespace fns + 1 include + 1 callsite rename); new helper compiled in via the existing `application/assistant/**` premake glob.
- Premake regen required (`premake5 gmake`) before the next `make` build picks up the new `.cpp`.  Done.
- Tests touched: none directly — the controller's session-ID validation behaviour is byte-equivalent; the 28-test suite covers it.
- Blast radius if wrong: ID validation regression would surface as legitimate sessions being rejected.  Suite passes.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] Path traversal in `AssistantSession(sessionsDir, sessionId)` constructor

**Finding (paraphrased):** Resume constructor concatenates `sessionId` directly into a filesystem path (`sessionsDir / (sessionId + ".jsonl")`) with no validation, so an attacker-controlled `"../../etc/passwd"` escapes the sessions directory.  `LoadFromFile` then opens whatever the resolved path points at.

**Verification:** Holds up — the resume ctor receives `sessionId` from caller and constructs `m_FilePath` without any allowlist or canonicalisation.  Sitting 3's `AssistantController::GetSession` already gates on `IsValidSessionId` + canonical-path containment, so the production path is currently safe.  But the constructor is part of the public surface; defense in depth requires the constructor itself to validate.

**Change:** Resume ctor now calls `IsValidOpaqueId(sessionId)` before any path construction; on failure, logs `LOG_SECURITY_WARN("[security] assistant_session_resume_invalid_id length={}")` (length only, never the value), clears `m_SessionId`, sets `m_FileBroken = true`, and returns without touching the filesystem.  Belt-and-suspenders alongside the controller-layer gate.

**Ramifications:**
- Callers touched: none — controller already gates first; this is a second, equivalent gate.
- Tests touched: `Session: resume unknown session ID` (existing) covers the valid-format-but-not-found path; the all-pass run confirms no regression.
- Blast radius: a future direct caller of the constructor (no controller in front) cannot smuggle a traversal sessionId.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] Weak/predictable session ID generation

**Finding (paraphrased):** Session IDs are `"sess_" + millisecond-timestamp + "_" + counter`.  Both components are predictable: process start time is observable from log output and file mtimes, and the counter resets to zero on every restart.  An attacker who can enumerate roughly when a session was opened can brute-force session IDs and hijack conversations.  The counter-resets-on-restart bug also enables silent data corruption: if process restarts within the same millisecond, the new counter starts at 0 and the new session appends to an old session's `.jsonl` file.

**Verification:** Holds up — the original `GenerateSessionId` constructs the ID from `std::chrono::system_clock` ms + an `std::atomic<uint32_t>` static.  Both are predictable; neither is cryptographically secure.

**Change:** `GenerateSessionId` now returns `"sess_" + RandomHex(16)` — 32 hex chars of `RAND_bytes` output, 128 bits of entropy.  No timestamp, no counter, no observable process state.  RAND_bytes failure (logged at ERROR by `RandomHex`) falls through to a `"sess_fallback_" + ms` fallback so the session has SOME unique-ish ID rather than empty (which would collide).  Two collateral closures: the timestamp leak (audit MEDIUM "session ID logged directly") is now also addressed because `LogSafeSessionId` truncates to 8 hex chars in the `LOG_APP_INFO` lines, and the counter-reset corruption disappears because there is no counter.

**Ramifications:**
- Callers touched: existing sessions with the old `sess_<ms>_<n>` format remain readable on disk — `IsValidOpaqueId` accepts them (they match `[A-Za-z0-9_-]{1,128}`), and `ListSessions` continues to surface them.  Pure additive change; no migration needed.
- Tests touched: 28-test suite includes session-list / resume coverage; passes.
- Blast radius: legitimate sessions cannot be enumerated from logs anymore, since logs only carry the 8-char prefix.  Operators investigating a specific session can grep for the prefix.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `AppendTurn` writes silently fail; no flush, no error propagation

**Finding (paraphrased):** `AppendTurn` opens an ofstream, streams insertions, never calls `flush()`, never checks `ofs.good()`.  On disk-full or open failure, `m_Turns.push_back` already happened so memory and disk diverge; the failure is logged at WARN, not ERROR.  Caller has no way to know.

**Verification:** Holds up.  `m_Turns.push_back(turn)` was the first line of `AppendTurn`; the file write happened second; failures landed at WARN.

**Change:** Renamed `AppendTurn` → `AppendTurnLocked` (assumes lock is held).  New flow: write to disk first (open + insertions + explicit `flush` + `good()` check); only on success commit `m_Turns.push_back(turn)`.  On any failure, set sticky `m_FileBroken = true` so subsequent appends fail fast at the head of the function rather than re-attempting on a known-bad file.  All log lines are now `LOG_APP_ERROR` with the truncated session ID prefix.  `AddUserMessage` and `AddAssistantMessage` are now `[[nodiscard]] bool` and propagate the persistence outcome.  All 6 controller call sites were updated to `(void)session->AddXxxMessage(...)` with a one-line comment at the first site explaining the contract — the session itself emits the ERROR log; the controller proceeds with the in-memory request so the user still gets a response when disk is full.

**Ramifications:**
- Callers touched: 6 sites in `assistantController.cpp` (HandleUserMessage user-message path, slash-command path, completion path, AI-error path, AI-success path, max-iterations path).  All updated.
- Tests touched: 28-test suite covers the protocol path; passes.  The error path itself (disk full) requires a fault-injection test not in scope this sitting.
- Blast radius: dashboard run analysis (memory `feedback_log_failures`) now surfaces persistence failures since they hit ERROR level; previously invisible.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `LoadFromFile` acquires `m_Mutex` from constructor — false safety, real fragility

**Finding (paraphrased):** `LoadFromFile()` was called from the resume constructor and acquired `m_Mutex` via `lock_guard`.  At construction time `this` is not yet shared, so the lock provides no actual safety; if the constructor ever runs in a context where `this` is published before construction completes (factory pattern with eager sharing), the lock provides false safety.  Conversely, the new-session ctor does not call `LoadFromFile` and so does not lock — inconsistent.

**Change:** Renamed `LoadFromFile` → `LoadFromFileLocked` and dropped the lock acquisition; documented the construction-time thread-safety contract (object must be fully constructed before sharing).  The header explicitly states this contract.  No callers outside the constructor remain.

**Ramifications:**
- Callers touched: only the resume constructor; behavior identical when called from a single thread (the production case).
- Blast radius: a future caller that wants to reload from disk on a live session would now need to acquire the lock themselves before calling `LoadFromFileLocked`.  The header's `_Locked` suffix and the namespace-private visibility make this discoverable.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `ListSessions` TOCTOU between `exists()` and `directory_iterator`

**Finding (paraphrased):** `fs::exists(sessionsDir)` then `fs::directory_iterator(sessionsDir, ec)` is a classic TOCTOU.  More importantly, the `ec` from the iterator construction was never checked, so a permissions error silently produced an empty list with no log.

**Change:** Removed the `exists` pre-check.  Construct `directory_iterator(sessionsDir, ec)` directly, inspect `ec`, distinguish `errc::no_such_file_or_directory` (silent — first run) from any other error (`LOG_APP_WARN` with the path).  Added `IsValidOpaqueId` filter on each `.jsonl` stem so foreign files placed in the sessions directory don't surface as fake sessions.

**Ramifications:**
- Callers touched: only the static method itself; semantics improve (better diagnostics).
- Tests touched: `List sessions` test in the suite passes.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [MEDIUM] Unbounded JSONL load + line length

**Finding (paraphrased):** `LoadFromFile` reads every line into `m_Turns` with no cap on count or per-line length.

**Change:** Added `kMaxTurnsPerSession = 10000`, `kMaxLineBytes = 1 MiB`, `kMaxTurnTextBytes = 256 KiB` (header constants).  Per-line size guard rejects oversized lines with an ERROR-level log + sticky `m_FileBroken` (the file is corrupt-or-attacker-influenced; refuse further writes).  Per-turn text is clamped on load.  Per-session count is bounded on both load and runtime append.

**Ramifications:** Legitimate sessions today are far below these limits.  Defense in depth.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [MEDIUM] `ExtractJsonString` mishandles `\uXXXX` escapes — replaced with simdjson

**Finding (paraphrased):** The hand-rolled `ExtractJsonString` parser handles `\"`, `\\`, `\n`, `\r`, `\t` but treats `\u` as a literal `u`.  Round-tripping a stored string through `JsonEscape` (which DOES emit `\uXXXX` for control chars) and then `ExtractJsonString` (which DOESN'T decode them) corrupts the data.

**Change:** Replaced the home-built `ExtractJsonString` with `simdjson::ondemand` parsing per memory `feedback_simdjson_only`.  Each line is parsed as a JSON object; `role`/`text`/`ts` are extracted via `obj["..."].get_string().get(sv)`.  Role values are now validated against the closed set `{"user", "assistant", "system"}` — adversarial JSONL with `"role":"badvalue"` is dropped, addressing the LOW "exhaustive role match" finding.

**Ramifications:**
- Existing valid JSONL files are forward-compatible — simdjson parses them correctly.
- Strict-validation rejection of unknown roles is a behavior change.  Acceptable: there is no legitimate path producing non-{user,assistant,system} roles.

**Tested by:** Studio debug build + 28-test suite PASS (covers session save/resume round-trip).

---

### [LOW] File permissions 0600 on session JSONL (POSIX)

**Finding (paraphrased):** `std::ofstream` creates session files with the process umask (typically 0644 — world-readable).  Conversation history may include any secrets the user pasted.

**Change:** New static helper `RestrictFilePermissions(path)` calls `fs::permissions(path, owner_read | owner_write, replace, ec)` after the first write.  POSIX-only (Windows relies on inherited NTFS DACLs).  `ec` is intentionally ignored — best-effort, never fails the write that just succeeded.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `MemoryStore::GenerateId` data race on `static` `mt19937_64`

**Finding (paraphrased):** `GenerateId` uses a `static std::mt19937_64` seeded from steady_clock.  It is not thread-safe; concurrent calls (across two `MemoryStore` instances, or from the relaxed `GetRelevant` path) race on RNG state.  Also low-entropy and predictable.

**Change:** `GenerateId` now returns `"mem_" + RandomHex(16)`.  RAND_bytes failure path uses a static-mutex-guarded counter for degraded fallback.  No mt19937, no shared mutable RNG state.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `GetRelevant` does not hold the lock across `Recall` + trim — atomicity gap, latent deadlock

**Finding (paraphrased):** `GetRelevant` calls `Recall` (which acquires `m_Mutex`), then `results.resize(maxResults)` outside any lock.  Between the return of `Recall` and the resize, another thread can mutate `m_Entries`.  The data is safe (Recall returns copies) but the contract "all public methods hold the lock for the whole operation" is violated.  More critically, any future refactor that adds a lock in `GetRelevant` before delegating to `Recall` will deadlock on the non-recursive mutex.

**Change:** Extracted `RecallLocked(query)` as a private const method that assumes `m_Mutex` is held.  Public `Recall` acquires the lock and delegates.  Public `GetRelevant` acquires the lock once and calls `RecallLocked`, then trims under the same lock.  Class-level contract restored.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `SaveToDisk` silent persistence failure on the main data path

**Finding (paraphrased):** `SaveToDisk` swallows `fs::create_directories` failures, logs `ofstream` open failures at WARN, and returns void.  `Save`/`Delete`/`ClearAll` mutate in-memory state, then call `SaveToDisk`, then return success indicators (id / true) regardless of persistence outcome.  Disk full = silent data loss across restart.

**Change:** Renamed `SaveToDisk` → `SaveToDiskLocked` returning `bool`.  Logs every failure at `LOG_APP_ERROR` with the path and message.  Sets sticky `m_FileBroken` on any failure.  `Save` propagates failure: on `SaveToDiskLocked() == false`, rolls back the just-pushed entry and returns empty id; the caller sees an explicit signal.  `Delete` similarly returns false if persistence fails.  `Save` and `Delete` are now `[[nodiscard]]`; existing callers in `assistantTools.cpp` already capture the return values, so no rippling warnings.

**Ramifications:** The pre-existing `Save`/`Delete` contract changes from "return id/true and quietly lose the data on disk failure" to "return empty/false on persistence failure and roll back the in-memory mutation".  Callers benefit; no test regressions.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `LoadFromDisk` TOCTOU + `[MEDIUM]` `LoadFromDisk` lock-from-ctor

**Finding (paraphrased):** `fs::exists()` then separate `ifstream` open is a TOCTOU.  The constructor calls `LoadFromDisk()` which acquires the per-instance mutex — same false-safety as session.

**Change:** Renamed `LoadFromDisk` → `LoadFromDiskLocked`; dropped the lock; documented the construction-time contract.  Removed the `fs::exists` pre-check; the open below distinguishes "absent" (silent first-run) from "present but unreadable" (`LOG_APP_ERROR` + `m_FileBroken`).

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [MEDIUM] Unbounded `m_Entries` + per-field length

**Finding (paraphrased):** `Save` and `LoadFromDisk` accept arbitrarily large keys, values, tags.

**Change:** Added `kMaxEntries = 10000`, `kMaxKeyBytes = 256`, `kMaxValueBytes = 64 KiB`, `kMaxTagBytes = 256`, `kMaxTagsPerEntry = 32` as header constants.  `Save` clamps inputs (truncates rather than rejects, preserving best-effort UX) and rejects after the entry cap.  `LoadFromDisk` clamps stored fields and stops at the entry cap with an ERROR-level log + `m_FileBroken`.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [MEDIUM] `Recall`/`GetRelevantFiles` raw pointers into vector — fragile

**Finding (paraphrased):** Both methods built `Scored { score, Entry const* }` arrays pointing into `m_Entries`.  Currently safe (lock held throughout), but one refactor away from a use-after-free if any future call mutates the vector inside the locked section.

**Change:** Both `Scored` structs now hold `size_t idx` instead of a raw pointer.  Lookup is `m_Entries[idx]` at the dereference loop — same lifetime guarantees but no pointer to dangle.  Same change applied to `WorkspaceIndexer::GetRelevantFiles` for consistency.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [LOW] Memory-key logging redaction

**Finding (paraphrased):** `LOG_APP_INFO("[memory] Saved new memory: key='{}' id='{}'", key, id)` logs the user-supplied `key` verbatim.  Keys may contain newlines (log injection), secrets, or PII.

**Change:** New static `LogSafeKey` helper replaces control bytes with `?`, caps display at 64 chars, appends the original byte length in parentheses.  Logs now read e.g. `key='user_name'(9)` or `key='very-long-key-tha...'(120)`.  Newline injection is structurally impossible.  Applied at every `[memory]` log site that previously emitted `key`.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `WorkspaceIndexer::ReadFileContent` path traversal — no anchoring against workspace root

**Finding (paraphrased):** The static `ReadFileContent(relativePath, maxBytes)` checks for the literal string `".."` in the normalized path then for a leading `/`.  On Windows, `lexically_normal()` produces `C:\...` which starts with a drive letter, bypassing the `/` check.  Even on POSIX, the function offers no anchoring against any workspace root: a caller passing any relative path that doesn't contain literal `..` and doesn't start with `/` slips through, including a symlink whose OS-level resolution targets `/etc/passwd`.

**Change:** Removed `static` from `ReadFileContent`.  Constructor now captures `m_WorkspaceRoot = fs::weakly_canonical(fs::current_path())` at j9t startup (before any cwd shenanigans).  New private helper `ResolveAndConfine(relativePath)` does `weakly_canonical(m_WorkspaceRoot / raw)` and verifies the result lives under `m_WorkspaceRoot` via `lexically_relative` — empty/`..`-prefix means escape, returns empty path.  `ReadFileContent` calls `ResolveAndConfine` first; on rejection returns empty content immediately.  Symlinks pointing out of the workspace are caught (weakly_canonical resolves them).  Absolute paths are also rejected up front.

`assistantTools.cpp::ExecGetFileSummary` (the only external caller) updated from `WorkspaceIndexer::ReadFileContent(filePath, 32768)` to `m_WorkspaceIndexer->ReadFileContent(filePath, 32768)`.

**Ramifications:**
- Existing valid `relativePath` arguments resolve identically (already inside the workspace).
- Note this is workspace-confinement only, NOT a deny-list against sensitive files (config.json, .env, etc.) — `assistantTools.h::IsPathDenied` is the deny-list; this is the orthogonal "stay inside the project tree" gate.  The header comment makes this explicit.

**Tested by:** Studio debug build + 28-test suite PASS.  Runtime tool path needs `--with-ai` for a real `ExecGetFileSummary` exercise; deferred to JC pre-commit smoke if desired.

---

### [HIGH] `LoadIndex` accepts untrusted `relativePath` from on-disk JSONL without re-validation

**Finding (paraphrased):** Each `relativePath` field from `assistant/index/file_index.jsonl` is trusted into `m_Entries` and `m_PathToIndex` without any re-validation.  An attacker who can write to the index file (e.g., via a separate path-traversal bug or as a co-tenant local user) can inject `../../etc/shadow` and have the indexer happily summarise it.

**Change:** Every `relativePath` parsed from the index file passes through `ResolveAndConfine`; rejects log `LOG_SECURITY_WARN("[security] indexer_index_path_escape len=...")` (length only, never the value) and the entry is dropped.  Bonus: `kMaxIndexEntries = 100000` cap applied during load — also addresses the MEDIUM "no entry limit" finding.

**Tested by:** Studio debug build + 28-test suite PASS (suite covers `/index` slash command which exercises load path).

---

### [HIGH] `LastScanTime()` reads `m_LastScanTime` without lock

**Finding:** Class invariant says all public methods acquire the mutex; this one didn't.

**Change:** `LastScanTime()` now acquires `m_Mutex` for the read.  Trivial.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [HIGH] `ScanDirectory` ignores `ec` from `fs::file_size` / `fs::last_write_time`

**Finding (paraphrased):** Both calls use the `ec` overload but do not check it.  On failure they return `UINTMAX_MAX` / implementation-defined value, which is then stored in the index as garbage size/mtime.  Summary cache invalidation breaks because the bogus mtime never matches.

**Change:** Both `ScanDirectory` and the top-level-files block in `ScanWorkspace` now check `ec` after each filesystem call and `continue` past the entry on error.  No bogus values reach `m_Entries`.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [MEDIUM] `ReadFileContent` truncation logic was buggy — second `fs::file_size` raced against itself

**Finding (paraphrased):** The post-read truncation marker check was `if (fileSize < fs::file_size(filePath, ec))` where `fileSize` had already been clamped to `maxBytes` earlier.  The comparison effectively asked "was the on-disk file just now bigger than my clamped read window?", which is racy and doesn't capture the intent (truncation happened iff the *original* file was larger than maxBytes).

**Change:** Save the original size before clamping; compare original-vs-maxBytes for the truncation decision.  Single `fs::file_size` call (no second TOCTOU).

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [MEDIUM] `SaveIndex` silent write errors + `LoadIndex` severity mismatch

**Change:** `SaveIndex` checks `ofs.good()` after the write loop and `flush`; failure logs at `LOG_APP_ERROR`.  `create_directories` failure also now logs at ERROR.  `LoadIndex` distinguishes "file does not exist" (silent first run) from "file present but unreadable" (`LOG_APP_ERROR`).

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [LOW] `SetFileSummary` accepts unbounded summary; `kMaxSummaryBytes = 8 KiB`

**Change:** Cached summary is clamped to 8 KiB at `SetFileSummary`.  A runaway provider response (or a prompt-injected mega-summary) cannot bloat the index file.

**Tested by:** Studio debug build + 28-test suite PASS.

---

### [MEDIUM] `ContextAssembler::BuildConversationContext` prompt injection via `turn.text`

**Finding (paraphrased):** `turn.text` from prior assistant and user turns is concatenated into the context with no sanitization.  An earlier turn containing the literal strings `<tool_call>` / `</tool_call>` / `<tool_result>` / `</tool_result>` / `=== … ===` (the system-prompt section delimiter pattern) will be interpreted by the model as a real structural signal rather than data.  Same vulnerability for the new `userMessage` placed in `prompt.prob`.

**Verification:** Holds up.  System prompt explicitly tells the model to treat `<tool_result>` content as data, but enforcement is purely instructional.  No C++-level escaping anywhere in the prior code.

**Change:** New `static std::string DefangContextSentinels(std::string const& text)` does two things: (a) calls `ToolRegistry::DefangToolMarkers` (sitting 3's public helper for `<tool_call>`/`</tool_call>`/`<tool_result>`/`</tool_result>` → mathematical-angle-bracket replacements); (b) replaces any run of 3+ `=` with the same number of U+2550 (BOX DRAWINGS DOUBLE HORIZONTAL, UTF-8 `E2 95 90`).  Visual content preserved (the user sees a thicker bar instead of `===`); the ASCII byte sequence the model keys on is gone.

`BuildConversationContext` now calls `DefangContextSentinels` on every `turn.text` before concatenation.  `Assemble` does the same on `userMessage` before placing it in `prompt.prob`.

**Tested by:** Studio debug build + 28-test suite PASS.  Note: the sentinel defang behavior under live AI is not directly verified by the protocol-level test suite — JC pre-commit smoke (or a `--with-ai` run with a crafted user message) would confirm runtime behaviour.

---

### [MEDIUM] Unbounded conversation context + per-turn text + tool descriptions

**Change:** Header constants: `kMaxUserMessageBytes = 64 KiB`, `kMaxTurnTextBytes = 32 KiB`, `kMaxConversationContextBytes = 128 KiB`, `kMaxToolDescriptionsBytes = 64 KiB`.  `BuildConversationContext` truncates each turn's defanged text to `kMaxTurnTextBytes` and stops adding turns once the running concat would exceed `kMaxConversationContextBytes`.  `Assemble` truncates `toolDescriptions` to `kMaxToolDescriptionsBytes` (defense in depth — currently only the trusted `ToolRegistry` builds these, but a future regression that lets user-influenced text leak in is bounded) and `userMessage` to `kMaxUserMessageBytes`.

**Tested by:** Studio debug build + 28-test suite PASS.

---

## Skipped findings table — Sitting 4

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| Memory store path validated at construction | `assistantMemory.h` | LOW | Only caller is the controller passing `assistant/memory.json` (non-user-controlled).  Real exploit path requires controlling the constructor argument.  Defer until the storePath becomes operator-configurable. |
| Encrypt-at-rest for memory store | `assistantMemory.h` | MEDIUM | Architectural change; memory may contain secrets but local-file confidentiality is currently delegated to OS file permissions.  Encrypted-store design tracked for post-1.0. |
| `ScanWorkspace` double-clear logic refactor | `workspaceIndexer.cpp` | MEDIUM | Logic is correct today.  Refactor to "build into locals, swap at end" improves clarity but risks subtle regressions in the duplicate-detection invariant.  Defer to a dedicated cleanup sitting. |
| `ReadFileContent` `O_NOFOLLOW` open | `workspaceIndexer.cpp` | MEDIUM | `weakly_canonical` already resolves symlinks at the path-resolution step, so the symlink-TOCTOU window is narrower than the audit assumes.  A genuine `O_NOFOLLOW` requires platform-specific code; defer. |
| Generic `[[nodiscard]]` audit on `Save`/`Delete`/`Recall`/`Size`/`ListAll`/`GetRelevant` | `assistantMemory.h` | MEDIUM | Annotated this sitting (no-op for callers since they all already capture). |
| Locale-safe `tolower` in `ScoreMatch` | `assistantMemory.cpp` | LOW | Already cast to `unsigned char` in this sitting. |
| `SaveToDisk const`-correctness rename | `assistantMemory.cpp` | LOW | Method now `bool SaveToDiskLocked()` (non-const). |
| `LoadFromDisk` `m_FileBroken` reset on success | `assistantMemory.cpp` | LOW | Sticky flag is correct: a corrupted file should refuse further writes until operator intervenes.  Resetting would silently mask recurring failures. |
| Move out of `JsonHelper::SanitizeForJson` instance method to static | `engine/json/jsonHelper.{h,cpp}` | LOW | Static `EscapeJsonString` added; instance `SanitizeForJson` retained for backwards compatibility with 10+ existing call sites in `aiTranscript.cpp` / `requestBuilder.cpp`.  Migration is a separate, larger sweep. |
| Migrate `assistantController.cpp` and `assistantTools.cpp` anon-namespace `JsonEscape` to `JsonHelper::EscapeJsonString` | both | LOW | Both copies are RFC-compliant after sittings 2/3.  Migration is mechanical but ripples to ~50 QueueMessage call sites; bound this sitting's diff. |
| `JoinFinishedThreads` → engine `ThreadPool` | `assistantController.{h,cpp}` | HIGH (sitting 3 carry-over) | Cross-component refactor; deferred to its own sitting per memory `feedback_no_jthread_use_threadpool`. |
| `m_ToolRegistry` / `m_MemoryStore` / `m_WorkspaceIndexer` thread-safety contract audit | `assistantController.h` | sitting 3 carry-over | Background-lambda vs main-thread access patterns need a dedicated audit; deferred. |

---

## Sitting 5 — assistant-internal debt closeout (engine ThreadPool, drain CV, JsonEscape sweep, registry contract)

**Scope locked:** the four assistant-internal cross-component items the sitting-4 hand-off enumerated as the natural follow-up cluster: (a) `JoinFinishedThreads` → engine `ThreadPool` migration, (b) `QueueMessage` drain CV so AI replies don't sit until the next `OnMessage`, (c) JsonEscape four-copy convergence (the last two anon-namespace copies in `assistantTools.cpp` + `assistantController.cpp` migrated to `JsonHelper::EscapeJsonString`), (d) thread-safety contract documented on `ToolRegistry` (the only one of the three shared registries that lacked an explicit contract; `MemoryStore` and `WorkspaceIndexer` already carry one post sitting 4).  Boundary at sitting-end: the assistant subsystem is now free of direct `std::thread`s, has no hidden `JsonEscape` duplicates, and has a documented concurrency contract on every component the AI lambda touches concurrently.  D2 web/cloud surface is now the next densest cluster — kicks off in sitting 6.

### Anon-namespace `JsonEscape` migrated to `JsonHelper::EscapeJsonString`

**Finding (carry-over from sitting 4):** Two anon-namespace copies of `JsonEscape` remained — `assistantTools.cpp` (1 caller) and `assistantController.cpp` (38 callers).  Both were RFC 8259-compliant after sittings 2–3, so this is convergence, not a fix.  Sittings 1–4 migrated `assistantSession.cpp`, `assistantMemory.cpp`, `workspaceIndexer.cpp`; this sitting closes the last two.

**Change:** Both files now `#include "json/jsonHelper.h"` and call `JsonHelper::EscapeJsonString(...)` directly.  The local `JsonEscape` definitions are deleted.  In `assistantTools.cpp::ExecJcwfGenerate` the single caller (workflowId embedded in `global.json`) routes through the central helper.  In `assistantController.cpp` all 38 message-construction sites in `OnMessage`, `HandleNewSession`, `HandleResumeSession`, `HandleListSessions`, `HandleGetHistory`, `HandleCompletionRequest`, `RunAiCallAsync` (including the multi-step tool loop), `RequestToolApproval`, and the slash-command handlers route through the central helper.

**Ramifications:**
- Callers touched: 39 `JsonEscape(...)` call sites (38 + 1) → `JsonHelper::EscapeJsonString(...)` (mechanical `replace_all`).
- Tests touched: none — protocol-level test suite is unaffected (output bytes identical for the same inputs).
- Docs touched: none.
- Blast radius: zero — same RFC 8259 escape, same inputs, same outputs.  The convergence eliminates a future maintenance hazard if `JsonHelper::EscapeJsonString` is updated (e.g. for non-printable Unicode policy) but the assistant copies aren't.

**Tested by:** Studio debug build clean (`make config=debug` after a fresh `premake5 gmake` was unnecessary — no new `.cpp` files); 28-test assistant suite PASS in 2.1 s; `test_testinterface_hermetic.py` PASS (adjacent dispatcher path).

---

### `JoinFinishedThreads` + manual `std::thread` vector → engine `ThreadPool` futures

**Finding (sitting 3 carry-over):** `AssistantController` spawned a fresh `std::thread` for every AI dispatch and stored it in `std::vector<std::thread> m_BackgroundThreads`.  The `JoinFinishedThreads` sweeper was a no-op (the comment in the function admitted so: "We can't easily check if a thread is 'done' in C++"), so the vector grew unbounded across a session and was only drained by `Shutdown`.  Memory `feedback_no_jthread_use_threadpool` calls for reusing `engine/auxiliary/threadPool.h` instead of bespoke `std::thread`s for cross-platform consistency.

**Change:** `m_BackgroundThreads` is now `std::vector<std::shared_future<void>> m_BackgroundFutures`.  `RunAiCallAsync` submits onto `Core::g_Core->GetThreadPool().SubmitTask([...]() {...})` and stores the `.share()`'d future.  `JoinFinishedFutures` (renamed) drops futures whose task has finished via `wait_for(0ms) == ready` — now a real cleanup, not a no-op.  `Shutdown` snapshots the futures under `m_ThreadsMutex`, then calls `wait()` on each outside the lock.  `<thread>` removed from the controller header; `<future>` and `<algorithm>` added.

**Ramifications:**
- Callers touched: zero external — `JoinFinishedThreads` was private; the rename is invisible.
- Tests touched: 28-test suite + hermetic dispatcher exercise the dispatch + shutdown paths.
- Docs touched: header comment on `m_BackgroundFutures` documents why we use the engine pool.
- Blast radius: lower — no per-turn `std::thread` creation cost; pool size is bounded; the unbounded-vector growth bug is closed.  Lifetime ordering relative to WRM teardown is preserved by the existing jarvisAgent.cpp shutdown sequence (`AssistantController::Shutdown` precedes `WorkflowRuntimeManager` reset).
- One subtlety: the engine ThreadPool's own `Shutdown` (called later in core teardown) will `wait()` on all submitted tasks.  The assistant's `Shutdown` waits on its own futures first, so by the time the engine pool tears down, the assistant lambdas are already drained.  Net behaviour: identical to the prior `std::thread::join` pattern.

**Tested by:** Studio debug build clean; 28-test assistant suite PASS (covers dispatch + shutdown + reconnect under the new path); hermetic dispatcher PASS.  Live AI dispatch over the new path not directly verified (would need `--with-ai`); structurally covered by the build.

---

### `QueueMessage` drain CV — AI replies surface immediately, not on next `OnMessage`

**Finding (sitting 4 carry-over):** `DrainPendingMessages` was only called from `OnMessage`.  When an AI lambda finished after the user's last message, the response sat in `m_PendingMessages` until the user sent another message.  The 10k cap in `QueueMessage` was a band-aid documenting the bug: "a long-running tool loop can produce many messages without an incoming WS message arriving to drain the queue."

**Change:** New `m_DrainCv` (bound to `m_PendingMutex`) and a `DrainLoop` task submitted to the engine `ThreadPool` from the controller constructor.  `QueueMessage` notifies on every successful enqueue.  `DrainLoop` `wait_for`s 1 s as a backstop, then calls `DrainPendingMessages` — which is unchanged (it already snapshots under locks, so calling from a non-WS thread is safe; Crow's `send_text` is thread-safe via `asio::post` onto the io-context strand, verified in `vendor/crow/include/crow/crow/websocket.h`).  `Shutdown` notifies `m_DrainCv` and waits on `m_DrainLoopFuture`; a final `DrainPendingMessages` flush after the future returns ensures any messages produced by lambdas right before shutdown still reach connected clients before the WS close fires.

**Ramifications:**
- Callers touched: zero — `QueueMessage` and `DrainPendingMessages` signatures unchanged; the existing `DrainPendingMessages` calls inside `OnMessage` remain (they're now redundant but harmless, and ensure synchronous flush before the handler returns for protocol-error responses).
- Tests touched: 28-test suite PASS — protocol-level coverage exercises the QueueMessage path on every test, so a deadlock or missed-notify would surface.
- Docs touched: header comment on `m_DrainCv` and `m_PendingMessages` documents the intent.
- Blast radius: one extra long-running worker on the engine ThreadPool (alongside file watcher + keyboard input).  Pool sizing is `m_MaxThreads + 2`; bumping `THREADS_REQUIRED_BY_APP` to 3 is defense-in-depth but not strictly needed — the drain loop sleeps on the CV most of the time and doesn't block other tasks.  Left at 2; revisit if pool pressure ever surfaces in `debug_signals`.

**Tested by:** Studio debug build clean; 28-test suite PASS in 2.1 s — and notably, every QueueMessage/drain path now exercises the CV-driven code path.  Hermetic dispatcher PASS.  Live AI multi-step dispatch (the regime where intermediate `tool_status` / `tool_result` messages benefit most) not directly smoked here; would need `--with-ai`.

---

### Thread-safety contract documented on `ToolRegistry`

**Finding (sitting 3 carry-over):** `MemoryStore` and `WorkspaceIndexer` carry an explicit thread-safety contract in their headers since sitting 4: "all public methods acquire `m_Mutex` once and hold it for the whole operation."  `ToolRegistry` does not — its `Execute` method is called from the AI lambda (background thread) while its setters (`SetWorkflowRegistry`, `SetWorkflowRuntimeManager`, `SetMemoryStore`, `SetWorkspaceIndexer`, `SetAiCallFn`) are called from `AssistantController`'s constructor (and from `WebServer` immediately after, on the main thread before any AI dispatch).

**Verification:** Reading the code: `ToolRegistry` owns no post-publication mutable state — `m_ToolDefs` and `m_ToolFns` are populated in the ctor, the four backing pointers are set once in `AssistantController::AssistantController` and `AssistantController::SetWorkflowRegistry` / `SetWorkflowRuntimeManager` (both called from `WebServer::Start` paths before any WS route fires).  The targets of the pointers are individually thread-safe (`MemoryStore` + `WorkspaceIndexer` per their own contracts; `WorkflowRegistry` and `WorkflowRuntimeManager` per their own).  So the registry needs no internal mutex — but the contract was undocumented.

**Change:** `assistantTools.h` now carries an explicit class-level comment block before `class ToolRegistry` documenting (a) the set-once nature of the setters, (b) the after-publication read-only contract on the backing pointers, (c) the targets' individual thread-safety, (d) the explicit "if a future change introduces post-publication mutable state, add a mutex first" forward note.  Each setter docstring is annotated with "set-once".

**Ramifications:**
- Callers touched: zero.
- Tests touched: zero.
- Docs touched: header-only.
- Blast radius: documentation only — no behavioural change.

**Tested by:** Studio debug build clean; 28-test suite PASS.

---

## Skipped findings table — Sitting 5

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| Bump `THREADS_REQUIRED_BY_APP` to 3 to reserve a slot for the assistant drain loop | `engine/core.h` | LOW | The drain loop sleeps on the CV — it doesn't block other tasks.  Pool sizing is `m_MaxThreads + 2` with `m_MaxThreads` typically 32+; one extra long-running worker is well within slack.  Bump if `debug_signals` ever shows pool pressure. |
| Migrate the dashboard's `WebServer::DrainPendingBroadcasts` to a CV-driven model | `application/web/webServer.cpp` | MEDIUM | Same architectural shape (drain only on WS message), but the dashboard has constant client interaction so the gap is much smaller in practice.  Tracked for sitting 6+ if the dashboard refactor goes there. |
| Convert AI-dispatch lambda from `[this]` capture to a captured-shared-state struct | `application/assistant/assistantController.cpp` | MEDIUM | Per memory `feedback_capture_by_value_async`: the lambda captures `this` and uses it across long blocking calls.  The current design is safe because `Shutdown` waits on every future before destruction — but documented (not refactored) for now. |
| Add a per-controller `Shutdown` ordering test | test infra | LOW | The 28-test suite implicitly exercises shutdown via the test harness's `disconnect`-then-`reconnect` flow.  A dedicated stress test (rapid connect / dispatch / disconnect cycles) would harden the new future-based join path; tracked for sitting 6+. |
| Add Tracy profiler scope to `DrainLoop` | `application/assistant/assistantController.cpp` | LOW | Useful but Tracy is opt-in (`--tracy`); add when the next round of profiling is in progress, not as part of a hardening sitting. |

---

## Sitting 6 — webServer.cpp Cluster A: path/auth/static gating + body caps

**Scope locked:** the seven HIGH+MEDIUM findings on the **pre-auth and pre-role-check perimeter** of `application/web/webServer.cpp` — every input gate that an attacker can hit without first having a valid credential, plus the one role-escalation gap inside the post-auth WS handler (`ai-write-scripts`).  Findings on config-write atomicity (Cluster B) and concurrency (Cluster C — `DrainPendingBroadcasts` UAF, `TryMcpAuth` const_cast, dangling lambdas) are explicitly deferred to sittings 7+ — bound this sitting's diff at the perimeter.  Boundary at sitting-end: every public-or-near-public route on `webServer.cpp` either (a) authenticates before doing real work, (b) confines paths it builds from caller input, (c) bounds body size before allocating, or (d) does all three.  Dashboard + Workflow Editor static handlers no longer accept `..` traversal; the MCP heartbeat now requires a valid MCP key; OAuth callback now explicitly verifies TLS peer + hostname.

### `ServeDashboardStatic` + `ServeWorkflowEditorStatic` canonicalize before filesystem access

**Finding (HIGH cyber-sec):** Both handlers stripped a URL prefix and concatenated the remainder onto `distRoot` without `lexically_normal()` or canonical-path containment.  Crow URL-decodes path parameters before handlers run, so a request like `GET /dash-assets/../../etc/passwd` arrived at the handler as `relative = "../../etc/passwd"` and `distRoot / relative` resolved outside the dist directory.  Same shape for `/assets/...` and `/editor/assets/...` routes.

**Change:** New shared helper `WebServerHelpers::ConfinePathUnder(root, relative)` in `application/web/webServer_helpers.h` — wraps the established `weakly_canonical(root / raw)` + `lexically_relative(root)` containment pattern (same as `WorkspaceIndexer::ResolveAndConfine` from sitting 4).  Returns empty path on rejection (absolute relative, resolution error, or `..` escape).  `ServeDashboardStatic` calls it against `dashboard/ui/dist`; `ServeWorkflowEditorStatic` calls it against `workflow-editor/ui/dist/assets` (both `/assets/` and `/editor/assets/` URL layouts route to the same helper).  On rejection both handlers emit `LOG_SECURITY_WARN("[security] dashboard_static_path_escape len=...")` / `editor_static_path_escape` (length only — never the path) and return HTTP 400.

**Verified at runtime:** `curl --path-as-is "https://localhost:8443/dash-assets/../../etc/passwd"` returns HTTP 400 + `dashboard_static_path_escape len=16` lands in `log/security.txt`.

**Ramifications:**
- Callers touched: zero external — the URL-route signatures are unchanged.
- Tests touched: 28-test assistant suite + hermetic dispatcher pass; no static-asset traversal regression test exists yet (added to skipped findings as "future test fixture").
- Docs touched: none (handler comment documents the gate).
- Blast radius: any legitimate request that previously resolved a `..` segment (none — Vite-built bundles never produce `..` paths) would now 400.

---

### `ReadLogFile` path confinement under launch-cwd `log/` + `fromOffset` clamp

**Finding (HIGH cyber-sec):** `ReadLogFile` is a public method on `WebServer`; while current call-sites pass hardcoded `"log/log.txt"` and `"log/security.txt"`, the method has no internal containment — a future refactor that lets the caller influence `logPath` would expose path traversal.  Bonus MEDIUM: `fromOffset > fileSize` can produce a large `deltaSize` allocation in the cast path before the existing guard catches it.

**Change:** Resolve `logPath` against the launch cwd via `WebServerHelpers::ConfinePathUnder` and assert the result lives under `<launchCwd>/log/`.  On rejection: `LOG_SECURITY_WARN("[security] readlog_path_escape len=...")`, return HTTP 400.  Bonus: explicit `if (fromOffset > fileSize) fromOffset = fileSize;` clamp documents the invariant the existing guard relies on.  The two callers — `HandleLogGet` and `HandleSecurityLogGet` — both pass relative literals that resolve cleanly under the gate.

**Ramifications:**
- Callers touched: zero external — `ReadLogFile` signature unchanged; call-site behaviour for legitimate requests unchanged.
- Tests touched: 28-test suite + hermetic pass.
- Docs touched: none.
- Blast radius: zero for the two existing callers; defense-in-depth for any future caller.

---

### `ai-write-scripts` requires admin role (per-connection role pinning)

**Finding (HIGH cyber-sec + HIGH safety):** The `/ws` upgrade authenticated *any* role at `.onaccept`.  The `ai-write-scripts` message branch wrote arbitrary content under `scripts/` and chmod-ed `+x` on `.sh` files without re-checking that the connection's role was admin.  An operator or viewer with a valid MCP key or session cookie could plant scripts that subsequent admin-triggered workflow runs would then execute.

**Change:** Role pinned to each WS connection at upgrade time:
- New `m_WsClientRoles : std::unordered_map<crow::websocket::connection*, std::string>` (guarded by `m_Mutex`) in `webServer.h`.
- `.onaccept` lambda now writes `auth.m_Role` into the connection's `userdata` (Crow's per-connection void* slot — same mechanism Crow uses internally).
- `.onopen` reads userdata, frees the heap copy, and stores the role in `m_WsClientRoles` under the connection pointer.  Logging refactor as a side effect: snapshot client/connect/peak counts under lock, log outside (closes the HIGH safety finding "onopen reads m_Clients.size() outside lock").
- `.onclose` erases from `m_WsClientRoles` at the same site as `m_Clients.erase`.
- The `ai-write-scripts` branch reads the role from `m_WsClientRoles` (under m_Mutex), and if it's not `"admin"`, emits `LOG_SECURITY_WARN("[security] ai_write_scripts_role_denied role='...' ip=...")` and sends a `{"type":"ai-write-scripts-result","ok":false,"error":"forbidden"}` reply before returning early.

**Ramifications:**
- Callers touched: zero external — the WS protocol contract is unchanged (admins see no behaviour change; non-admins see the new `forbidden` reply for one specific message type).
- Tests touched: 28-test assistant suite covers the assistant WS path (separate route `/ws/assistant`) and passes; no test covers the dashboard `/ws` `ai-write-scripts` flow yet.
- Docs touched: none.
- Blast radius: any non-admin client that was previously *successfully* writing scripts via the dashboard → AI assistant flow would now be rejected.  Per the audit, that path is itself a vulnerability; rejecting it is the goal.

**Tested by:** Studio debug build clean; 28-test suite PASS.  Live ai-write-scripts admin-vs-operator test deferred (would need a multi-role test fixture; not yet built).

---

### `HandleMcpHeartbeatPost` requires MCP key + body cap + pre-auth rate limit

**Finding (HIGH cyber-sec + MEDIUM body-cap):** `POST /api/mcp/heartbeat` was registered as public — any unauthenticated caller could pin `IsMcpConnected()` to `true` indefinitely by writing `m_McpLastHeartbeat`, suppressing dashboard alerts on a stalled sidecar.  The handler also ignored the request body entirely without a size cap.

**Change:** Signature updated from `HandleMcpHeartbeatPost()` to `HandleMcpHeartbeatPost(crow::request const&)`.  Three gates added at the top of the handler in this order:
1. `IsRateLimited(RateLimitTier::PreAuth, req.remote_ip_address)` — first line of defense against unauthenticated floods; returns `MakeAuthErrorResponse("rate_limited")` (HTTP 429).
2. `IsBodyTooLarge(req, 1)` — heartbeat carries no body content; 1 KB cap is generous slack for future fields.  Returns `MakePayloadTooLargeResponse(1)` (HTTP 413).
3. `TryMcpAuth(req)` — must produce a valid `AuthResult`.  On miss: `LOG_SECURITY_WARN("[security] mcp_heartbeat_unauthorized ip=...")`, `RecordAuthFailure(...)`, return `MakeAuthErrorResponse("forbidden")` (HTTP 403).

**Verified at runtime:**
- `curl -X POST https://localhost:8443/api/mcp/heartbeat` (no auth) → HTTP 403 + `{"error":"forbidden"}`.
- `curl -X POST -H "Authorization: Bearer $J9T_TOKEN" https://localhost:8443/api/mcp/heartbeat` → HTTP 200 + `{"ok":true}`.

**Ramifications:**
- Callers touched: the MCP sidecar (`mcp/dist/index.js`) already sends an admin MCP key on every request — verified live (debug_signals counters tick).
- Tests touched: 28-test assistant + hermetic dispatcher pass.
- Docs touched: handler comment documents the new gates.
- Blast radius: any caller that was hitting the heartbeat unauthenticated would now be rejected.  No legitimate client does this.

---

### `HandleN8nStartPost` + `HandleWebhookPost` validate caller-supplied `runId`

**Finding (MEDIUM cyber-sec × 2):** Both handlers built a persisted-request file path as `workflowsDir / workflowId / [taskName | "webhook"] / runId / "request.json"`.  `workflowId` and `taskName` were validated by `IsValidWorkflowId` / `IsValidTaskName`, but `runId` — when caller-supplied via the JSON body — was used directly without validation.  `runId = "../../../"` would escape the run dir and write `request.json` wherever the process can write.

**Change:** Both handlers now validate `runId` against `IsValidWorkflowId` (the same alnum + `_`/`-` allowlist used for workflowId/taskName).  In the n8n handler, the check goes in the `else if` branch immediately after the `runId.empty() → GenerateIntegrationRunId` fallback (so server-generated IDs are always trusted).  In the webhook handler, the check is colocated with the `runId` parse from the JSON body (rejected on the spot rather than later when the path is built).  Both return `MakeWorkflowJsonError(400, "invalid_run_id", ...)` on rejection.

**Ramifications:**
- Callers touched: legitimate clients (n8n, webhook senders) generate server-friendly IDs already.  This validates *caller-supplied* IDs only.
- Tests touched: hermetic dispatcher pass; no n8n / webhook traversal regression test exists.
- Docs touched: handler comment documents why the gate exists.
- Blast radius: a caller passing an exotic runId (containing `/`, `.`, etc.) would now 400.  The legitimate clients use either server-generated IDs or simple alphanumeric strings.

---

### `HandleOAuthCallbackGet` explicit `CURLOPT_SSL_VERIFYPEER` + `CURLOPT_SSL_VERIFYHOST`

**Finding (HIGH cyber-sec):** The OAuth token-exchange POST never explicitly set `CURLOPT_SSL_VERIFYPEER` or `CURLOPT_SSL_VERIFYHOST`.  libcurl's defaults are correct (verification on), but a future libcurl rebuild with different defaults — or a project-managed CA bundle path becoming empty — could silently open a MitM window where token exchange occurs over an unverified connection.

**Change:** Two new explicit `curl_easy_setopt` calls before the CAINFO block: `CURLOPT_SSL_VERIFYPEER, 1L` (CA validation) and `CURLOPT_SSL_VERIFYHOST, 2L` (hostname match).  When `CurlWrapper::GetCaBundlePath()` returns empty (Linux/macOS — system CA bundle), an `LOG_APP_INFO` line records that we're using the system trust store, so an operator with a misconfigured build sees the early-warning signal in the log.

**Ramifications:**
- Callers touched: zero external — the OAuth callback URL is unchanged; legitimate tokens still complete successfully.
- Tests touched: hermetic dispatcher pass (different code path; OAuth flow not exercised by either test).
- Docs touched: none.
- Blast radius: a misconfigured token endpoint with a non-trusted certificate would now fail verification (correct behaviour).

---

### `HandleKeysUnlockPost` rate-limited + auth-failure recorded on wrong password

**Finding (MEDIUM cyber-sec):** `POST /api/settings/keys/unlock` is intentionally pre-auth (the master password IS the credential), but the handler had no pre-auth rate limit, no body-size cap, and didn't record auth failures on wrong-password responses.  An attacker could brute-force the master password against the live API at libcurl speed.

**Change:** Three gates at the top of the handler:
1. `IsRateLimited(RateLimitTier::PreAuth, req.remote_ip_address)` — first line of defense; returns `MakeAuthErrorResponse("rate_limited")` (HTTP 429) with the standard pre-auth bucket sized for legitimate operator traffic.
2. `IsBodyTooLarge(req, 1)` — the body carries only a master password string; 1 MB cap bounds malformed-body memory use.
3. On wrong-password (`!keyManager.Unlock(masterPassword)`): `RecordAuthFailure(req.remote_ip_address)` + `LOG_SECURITY_WARN("[security] keys_unlock_wrong_password ip=...")`.  Auth-failure tracking flows into the standard `kMaxAuthFailures` (10) within `kAuthFailureWindow` (5 min) lockout.

**Ramifications:**
- Callers touched: zero external — the `/api/settings/keys/unlock` endpoint contract is unchanged for legitimate single-attempt unlocks.
- Tests touched: this session's restart of j9t exercises the unlock endpoint (the handler succeeds on the right password as before).
- Docs touched: handler comment documents the new gates.
- Blast radius: an attacker hammering the endpoint hits the pre-auth rate limit first, then the failed-auth lockout.  Legitimate operators (one or two password attempts) are unaffected.

---

## Skipped findings table — Sitting 6

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| `HandleAiInterfacesSavePost` / `HandleConfigSettingsPut` / `HandleConnectionsSavePost` non-atomic write + naive string-replace | `webServer.cpp` | HIGH × 4 | Cluster B — separate sitting (atomic writes + simdjson round-trip).  Don't bundle with the perimeter cluster; the changes touch large code regions and the existing string-replace is dangerous enough to deserve its own review pass. |
| `DrainPendingBroadcasts` use-after-free (m_Clients re-check then send_text without holding lock) | `webServer.cpp` | CRITICAL safety | Cluster C — concurrency.  Same architectural shape as the assistant `DrainPendingMessages` path that sitting 5 left alone (it relies on Crow's send_text being asio-posted, which makes the UAF window narrower than the audit assumes — but a defensive fix is still warranted).  Tracked for sitting 7. |
| `TryMcpAuth` `const_cast<WebServer*>(this)` to call `RecordAuthFailure` | `webServer.cpp` | CRITICAL safety | Cluster C.  Fix: drop `const` from `Authenticate`, `TryMcpAuth`, `TrySessionAuth`, `AttachMcpExpiryHeader`, `CheckAuth`.  Touches many declarations + every call site.  Bound this sitting's diff. |
| `SetWorkflowRuntimeManager` captures raw `m_AdhocManager.get()` — dangling on reset | `webServer.cpp` | CRITICAL safety | Cluster C lifetime.  Fix: weak_ptr or explicit observer-clear before `m_AdhocManager` reset.  Defer. |
| `m_ClientCount` atomic vs `m_Clients` set consistency window | `webServer.cpp` | HIGH safety | Cluster C.  The atomic is acknowledged-as-hint in the audit's own analysis; "benign over-sends" is the worst case.  Defer for now. |
| `fs::exists()` then open() TOCTOU in `ServeDashboardIndex`, `HandleWorkflowVersionGetGet`, `HandleWorkflowVersionRestorePost` | `webServer.cpp` + `webServer_studio.cpp` | HIGH safety | Cluster C TOCTOU.  Fix: drop the `exists` precheck and rely on the `ifstream` open status (matches `TryReadBinaryFile` pattern).  Mechanical sweep — bundle with the cluster B / C sittings. |
| `ExtractSessionCookie` doesn't validate cookie value characters | `webServer.cpp` | LOW cyber-sec | Defense in depth; benign in practice (C++ string handles null bytes safely; the only consumer is a hash lookup).  Bundle with WebSessionManager hardening. |
| `HandleMcpKeysEnrollPost` doesn't validate `key_expiry_days`/`enrollment_ttl_minutes` upper bounds | `webServer.cpp` | LOW cyber-sec | Admin-only endpoint; an admin who wants a never-expiring key can edit the JSON file directly.  The audit-level concern is policy hardening, not exploit prevention.  Defer to the keys/MCP cluster. |
| `HandleLogAnalyzeLastRunGet` reads entire log into memory | `webServer.cpp` | MEDIUM cyber-sec | Authenticated endpoint; the audit's concern is operator-OOM-via-large-log.  Real fix is the same tail-bytes pattern as `ReadLogFile`.  Bundle with a "log endpoints hardening" mini-sitting. |
| Trusted proxy header strip `\r\n` | `webServer.cpp` | MEDIUM cyber-sec | Operator-controlled gateway; log injection requires the gateway itself to be misconfigured.  Defer with the auth funnel hardening cluster. |
| OAuth `codeVerifier` percent-encoding consistency | `webServer.cpp` | MEDIUM correctness | Robustness/correctness, not security.  Bundle with the OAuth code path hardening sitting. |
| WebSessionManager findings (role allowlist, constant-time compare, max sessions cap, RAND_bytes failure handling) | `webSessionManager.cpp` | MEDIUM × 3 + LOW | Separate file with its own audit section; bundle as a single mini-sitting. |

---

## Sitting 7 — webServer.cpp Cluster B: config-write atomicity

**Scope locked:** the four HIGH findings in **Cluster B** that sitting 6 explicitly deferred — three safety HIGHs ("writes config file non-atomically" × 3) and one cyber-sec HIGH ("writes config by naive string replacement without JSON escaping" — covers `HandleAiInterfacesSavePost` and `HandleConfigSettingsPut`).  The three handlers all share the same shape: read the existing file as raw text, splice or patch values inline, write back through `std::ofstream` with `std::ios::trunc` (or default trunc).  A failure midway — disk full, concurrent SIGTERM, exception during write — leaves the on-disk file empty or partially written; an admin who supplies a string field with `"` or `\` corrupts the JSON.  Boundary at sitting-end: every Cluster B handler routes through `WebServerHelpers::WriteTextFileAtomic` (tmp-file + rename), every caller-supplied string field goes through `JsonHelper::EscapeJsonString` before reaching the file, and every patched result is re-parsed with simdjson before the rename happens.  Cluster C (concurrency: `DrainPendingBroadcasts` UAF, `TryMcpAuth` `const_cast`, `SetWorkflowRuntimeManager` dangling lambda, `m_ClientCount` consistency window, fs::exists TOCTOU sweep) remains queued for sitting 8.

### `HandleConnectionsSavePost` — atomic write + ERROR-level fail log

**Finding (HIGH safety):** `std::ofstream file(connectionsFilePath); ... file << json;` — default-mode ofstream truncates on open, so a write failure leaves `connections.json` empty.  Bonus gap: the failure branch logged at neither ERROR level nor in `log/security.txt` — the audit's "log all failures at ERROR level" rule from memory `feedback_log_failures` was being violated for a security-relevant on-disk file.

**Verification:** Holds up.  Original code opens the file via the default-mode `ofstream` constructor (which uses `std::ios::out` ⇒ truncate-on-open), writes the JSON, calls `file.close()`.  No tmp-file dance, no atomicity barrier between the truncate and the write.

**Change:** Single substitution: the `std::ofstream` block becomes a `WriteTextFileAtomic(connectionsFilePath, json, writeError)` call.  The helper writes to `<path>.tmp`, flushes, then `fs::rename`s onto the target — POSIX guarantees this rename is atomic.  On failure the tmp file is unlinked and the target is untouched.  Failure path now emits both `LOG_APP_ERROR` (dashboard run-analyzer surface) AND `LOG_SECURITY_WARN("[security] connections_save_failed ...")` (security log surface).  `connectionManager.SerializeToJson()` already JSON-escapes its output (separate copy of the assistant-subsystem `JsonEscape` lineage, RFC-correct on inspection), so no escape gap to address here.

**Verified at runtime:** `curl -X POST -H "Authorization: Bearer $J9T_TOKEN" https://localhost:8443/api/connections/save` → HTTP 200 + `{"ok":true,"path":"..."}`.  `connections.json` rewritten with byte-identical content; no `connections.json.tmp` lingers post-rename.  `git diff connections.json` clean.

**Ramifications:**
- Callers touched: zero external — the endpoint contract (`POST /api/connections/save`, response shape) is unchanged.
- Tests touched: 28-test assistant suite + hermetic dispatcher pass.
- Docs touched: handler comment documents the new atomic-write gate.
- Blast radius: zero for the success path; failure-path responses now carry the underlying `WriteTextFileAtomic` error message rather than the previous generic "Failed to open" string — slightly more diagnosable.

---

### `HandleAiInterfacesSavePost` — JSON-escape every string field + atomic write + simdjson tripwire

**Finding (HIGH cyber-sec + HIGH safety):**  Two flaws in one handler.  (1) The "API interfaces" array is rebuilt as a string with raw `+ iface.m_Name +`-style concatenation across `m_Name`, `m_Description`, `m_Url`, `m_Model`, `m_KeyName` — none JSON-escaped.  An admin saving an interface whose description contains `"`, `\`, newline, or any control byte produced corrupt JSON that broke `config.json` on next parse.  (2) The find-replace splices the new array into the existing config text, then writes back via `std::ofstream(configPath, std::ios::binary | std::ios::trunc)` — a partial write leaves `config.json` truncated.

**Verification:** Holds up.  Read the original code at `webServer.cpp` ~4849: every `newArray += "..." + iface.m_X + "..."` call site is a verbatim splice of the user-controlled string into JSON-string-content position with no escaping.  The closing write block opens with `std::ios::trunc`.

**Change:** Three changes routed through one handler:
1. **JSON-escape every caller-supplied string field** — every embed of `iface.m_Name`, `iface.m_Description`, `iface.m_Url`, `iface.m_Model`, `iface.m_KeyName` now passes through `JsonHelper::EscapeJsonString(...)` before the `+` concatenation.  `apiStr` comes from a closed enum (`InterfaceType` → `"API1"`/`"API2"`/etc.) and needs no escaping.  The numeric fields (rate-limit knobs) format through `std::to_string` / `snprintf("%g")` and are inherently safe.  `JsonHelper::EscapeJsonString` is the canonical RFC 8259 escaper that the entire assistant subsystem standardised on in sitting 4 — extending its usage to webServer.cpp is the natural next step.
2. **simdjson tripwire** — after the bracket-counted text replacement but **before** the on-disk write, the patched `fileContent` is parsed with `simdjson::ondemand::parser`, the `"API interfaces"` array is iterated, and the element count is compared against `config.m_ApiInterfaces.size()`.  Any structural breakage (escape bug, bracket-counter miscount on an exotic string, future replaceField logic regression) surfaces here as `LOG_APP_ERROR("post-replacement validation failed ...")` + HTTP 500 with the original `config.json` left untouched.
3. **Atomic write** — the `ofstream + trunc` block becomes `WriteTextFileAtomic(configPath, fileContent, writeError)`.  Failure branch emits `LOG_APP_ERROR` + the helper's diagnostic message in the response body.

**Verified at runtime:**
- Baseline save (no field changes): `POST /api/settings/ai-interfaces/save` → HTTP 200, `config.json` byte-identical (md5 unchanged), no `config.json.tmp` lingering.
- **JSON-escape verification under hostile input:** `PUT /api/settings/ai-interfaces/api.openai.com%2Fgpt-4.1%2FAPI1` with `{"description":"audit-test value with \"quotes\" and \\backslash and a\nnewline"}` → HTTP 200; subsequent `POST .../save` → HTTP 200; the resulting `config.json` parses cleanly under `python3 json.load`, and the on-disk bytes show `"audit-test value with \"quotes\" and \\backslash and a\nnewline"` — i.e. RFC 8259 `\"`, `\\`, `\n` escapes correctly applied.  Pre-fix the same payload would have produced a config.json that failed on next reload (literal newline in a JSON string is a parse error).
- After verification, the test description was reverted to its pre-sitting value; `git diff config.json` clean.

**Ramifications:**
- Callers touched: zero external — the `POST /api/settings/ai-interfaces/save` contract is unchanged.
- Tests touched: 28-test assistant suite PASS.  Live curl smokes confirm both the escape path and the atomic-write path.
- Docs touched: handler-internal comments document the escape gate, the simdjson tripwire, and the atomic write — three short blocks each citing the audit finding they close.
- Blast radius: an admin who previously saved an interface with `"` or `\n` in a field had silent corruption on next reload; that path is now well-formed.  No legitimate flow was previously round-trippable with such characters.

---

### `HandleConfigSettingsPut` — depth-aware `replaceField` + atomic write + simdjson tripwire

**Finding (HIGH cyber-sec + HIGH safety):**  Same family as the AI interfaces handler, but for the seven top-level scalar settings (`API index`, `max threads`, `verbose`, `max file size in kB`, `jcwf batch size`, `jcwf AI interface`, `use_bash`).  Three flaws: (1) the `replaceField` lambda does `fileContent.find(searchKey)` — a brace-scope-unaware substring match.  Today's config schema has no key collisions between top-level and nested objects (`"API interfaces"` array elements use `name`/`url`/`model`/`API`/`key_name`, none of which overlap with the seven top-level scalar names).  But the lambda's contract — "replaces the value of the named key" — silently breaks if a future schema introduces such an overlap.  (2) Same `ofstream + trunc` non-atomicity as the interfaces handler.  (3) No post-replacement validation; if the lambda ever miscalculated value boundaries, corrupt JSON would land on disk.

**Verification:** Holds up.  Read the lambda body at `webServer.cpp` ~5322: `auto pos = fileContent.find(searchKey);` — first match anywhere, no depth tracking.  The write block uses `std::ios::trunc`.

**Change:** Three changes:
1. **Depth-aware replaceField** — the lambda is rewritten to walk `fileContent` byte-by-byte, tracking object depth via `{` / `}` and skipping string contents (honouring `\\`-escapes inside JSON strings).  The key match only fires when the candidate key-string is encountered at object depth 1 (immediately inside the root `{`).  Same shape as the existing `arrayEnd` scanner in `HandleAiInterfacesSavePost`, just specialised for "find a top-level key's value range".  Closes the cyber-sec finding's "no brace/object scope awareness" gap before any future schema introduces a key collision.
2. **simdjson tripwire** — after all seven `replaceField` calls, the patched `fileContent` is iterated with `simdjson::ondemand::parser`; both `validateDoc.error()` and `validateDoc.get_object()` are checked.  Any structural breakage (clobbered closing brace, mismatched delimiters, future replaceField bug) surfaces as HTTP 500 + ERROR log; the on-disk `config.json` is left untouched.
3. **Atomic write** — `ofstream + trunc` → `WriteTextFileAtomic`.

**Verified at runtime:**
- `PUT /api/settings/config` with `{"max_threads":42,"verbose":true,"jcwf_batch_size":7}` → HTTP 200; `config.json` updated atomically (no `config.json.tmp` lingering); `python3 json.load` parses cleanly with `max threads=42`, `verbose=True`, `jcwf batch size=7`.
- Reverted via second PUT (`max_threads=20`, `verbose=false`, `jcwf_batch_size=1`); `git diff config.json` clean post-revert.
- Depth-aware behaviour can't be directly demonstrated end-to-end because the current schema has no top-level/nested key collisions, but the lambda's correctness is verifiable by inspection (same shape as the well-tested `arrayEnd` scanner immediately above it).

**Ramifications:**
- Callers touched: zero external — the `PUT /api/settings/config` contract is unchanged.
- Tests touched: 28-test assistant suite + hermetic dispatcher pass.
- Docs touched: handler-internal comments document the depth-awareness rationale and the audit finding it closes.
- Blast radius: a future schema that introduces a key shared between top-level and a nested object would, on the old lambda, replace the *first occurrence anywhere* (likely the nested one).  Now it correctly targets only the top-level field.

---

## Skipped findings table — Sitting 7

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| Full parse-mutate-serialize through simdjson + custom serializer | `webServer.cpp` | (audit-suggested fix variant) | The audit suggested `parse the full document with simdjson, mutate the fields, dump back` as an alternative to the find-replace approach.  `config.json` is hand-edited (custom field ordering, comments, layout) — a parse-and-reserialize roundtrip would lose all of that.  Per memory `feedback_simdjson_only` simdjson is parse-only; rolling a custom JSON serializer is itself a meaningful engineering exercise.  The find-replace + JSON-escape + simdjson-tripwire posture is the pragmatic intermediate that addresses the cyber-sec and safety findings without rewriting the file's layout.  Defer the serializer work to a future "config.json schema migration" sitting if needed. |
| `cloudConnectionManager.cpp::JsonEscape` | `application/cloud/cloudConnectionManager.cpp` | (sitting 4 lineage) | A 6th anon-namespace `JsonEscape` copy lives outside the assistant subsystem and is correct on inspection.  Sitting 4 closed five copies inside `application/assistant/`; this one lives in `application/cloud/` and was out of cluster B scope.  Track for the connection-manager audit cluster (likely sitting 9+ when the cloud surface comes around). |
| Cluster C concurrency CRITICALs | `webServer.cpp` | CRITICAL × 3 | `DrainPendingBroadcasts` UAF, `TryMcpAuth` `const_cast`, `SetWorkflowRuntimeManager` dangling lambda — full cluster queued for sitting 8.  Bound this sitting at the config-write surface to keep the diff reviewable. |
| Cluster C concurrency HIGHs | `webServer.cpp` + `webServer_studio.cpp` | HIGH × 3 | `m_ClientCount` consistency window, `fs::exists()` then open() TOCTOU sweep — same cluster, same sitting. |
| Live curl smoke for `HandleAiInterfacesSavePost` write-failure path (e.g. parent dir read-only) | `webServer.cpp` | (verification gap) | Would need a controlled disk-failure fixture (read-only mount, full /tmp etc.).  The success path was verified live; the failure path's `WriteTextFileAtomic` returns false and the response carries the underlying error — same code path as `HandleN8nStartPost`'s atomic-write fail branch which has been exercised in prior sittings.  Defer to a "fault-injection regression test" tracked under the broader cybersec dev plan. |
| `replaceField` depth-aware behaviour under a synthetic schema with top-level/nested key overlap | `webServer.cpp` | (verification gap) | Current schema has no overlap.  Would need a temporary schema mutation plus a unit test; bundles cleanly with the future "JCWF schema overlap regression test" fixture.  Defer. |

---

## Sitting 8 — webServer.cpp Cluster C: concurrency

**Scope locked:** the six findings sitting 7 explicitly deferred — three **CRITICAL** concurrency / lifetime issues (`DrainPendingBroadcasts` UAF, `TryMcpAuth` / `Authenticate` `const_cast<WebServer*>(this)` cascade, `SetWorkflowRuntimeManager` raw-pointer-captured-lambda dangling at shutdown) plus two **HIGH** items (`m_ClientCount` atomic-vs-set consistency window, `fs::exists()` followed-by-open TOCTOU sweep across `ServeDashboardIndex` / `HandleWorkflowVersionGetGet` / `HandleWorkflowVersionRestorePost`).  Boundary at sitting-end: webServer.cpp's auth funnel no longer hides mutation behind `const_cast`, the WebSocket broadcast path holds `m_Mutex` continuously over the per-client `send_text` loop, the run-terminal observer is detached on both swap *and* shutdown before any teardown that could unwind `m_AdhocManager`, the `m_ClientCount` atomic carries a complete contract comment, and the three handlers that read files no longer use the `fs::exists()` precheck pattern.  After this sitting, the audit's `webServer.cpp` CRITICAL/HIGH cluster — Clusters A (sitting 6) + B (sitting 7) + C — is closed; the remaining `webServer.cpp` findings are MEDIUM/LOW and bundle naturally with the cloud surface (sitting 9+).

### `const`-cast cascade — drop `const` from the auth funnel

**Finding (CRITICAL safety):**  `TryMcpAuth` and `Authenticate` were declared `const`, but mutate shared state on the failure path: `TryMcpAuth` calls `RecordAuthFailure(ip)` which writes to `m_AuthFailures` under `m_RateLimitMutex`, and `Authenticate` calls `IsRateLimited(...)` (also non-const).  Both worked around their `const`-ness by `const_cast<WebServer*>(this)`-ing — `TryMcpAuth` did this once for the `RecordAuthFailure` call, `Authenticate` did it as `auto* self = const_cast<WebServer*>(this);` and then routed every member access (`self->m_RateLimitMutex`, `self->m_AuthFailures`, `self->IsRateLimited(...)`) through `self`.  Compiles, locks correctly today — but breaks the C++ contract that `const` member functions are safe to call concurrently from multiple threads.  Future code that adds a genuinely-read-only concurrent caller (or a refactor that removes the locking inside `RecordAuthFailure`) loses the type-system signal that mutation is happening.

**Verification:** Holds up.  The two `const_cast<WebServer*>(this)` sites at `webServer.cpp:602` and `:668` were verbatim as the audit described.

**Change:** Drop `const` from six declarations in `webServer.h` (`AttachMcpExpiryHeader`, `Authenticate`, `CheckAdminAuth`, both `CheckAuth` overloads, `TryMcpAuth`, `TrySessionAuth`) and the matching definitions in `webServer.cpp`.  Delete the `const_cast<WebServer*>(this)->RecordAuthFailure(ip);` line in `TryMcpAuth` and replace with `RecordAuthFailure(ip);`.  Delete the `auto* self = const_cast<WebServer*>(this);` line in `Authenticate` and rewrite every `self->m_RateLimitMutex` / `self->m_AuthFailures` / `self->IsRateLimited(...)` as plain `m_RateLimitMutex` / `m_AuthFailures` / `IsRateLimited(...)`.  Six callers across `webServer.cpp` and `webServer_studio.cpp` (which all use `CheckAuth(req, "viewer")` / `CheckAuth(req, "admin")` / `Authenticate(req)`) are unaffected — they were already invoking from non-const handlers.  Header comments updated to document why each method is non-const ("Non-const: calls RecordAuthFailure / IsRateLimited; marking these methods const and const_cast-ing inside hides the mutation from the type system and creates a foot-gun for future readers who assume const = thread-safe.").

**Verified at runtime:**
- Studio debug build clean.  No diagnostics from clangd; six caller sites compiled unmodified.
- `curl -H "Authorization: Bearer $J9T_TOKEN" /api/auth/whoami` → HTTP 200 + `{"role":"admin","user":"admin","ok":true}` — happy path.
- `curl /api/auth/whoami` (no auth) → HTTP 401 + `auth_failure reason=missing_credential` log line — pre-auth path.
- `curl -H "Authorization: Bearer mcp_invalid_token_xx" /api/auth/whoami` → HTTP 401 + `mcp_auth_failure reason=invalid_key` security log line — confirms `RecordAuthFailure` runs from the rewritten path (`debug_signals` reports `auth_failure_records: 1` post-call).

**Ramifications:**
- Callers touched: zero external — the authorization contract on every endpoint is unchanged; only the type-system signal moved.
- Tests touched: 28-test assistant non-AI suite + hermetic dispatcher PASS; both exercise the auth funnel end-to-end.
- Docs touched: none (the header comment is the canonical doc).
- Blast radius: zero behaviour change.  Type-safety improvement only.

---

### `DrainPendingBroadcasts` UAF — hold `m_Mutex` over the per-client send loop

**Finding (CRITICAL safety):**  The drain pattern was: take `m_Mutex`, swap `m_PendingBroadcasts` into local + snapshot `m_Clients` into local, drop the lock, build the JSON batch outside the lock, then per-client lock-find-unlock-send.  The "lock-find-unlock-send" window was the UAF: between the per-client `m_Clients.find(client) == m_Clients.end()` re-validation under lock and the subsequent `client->send_text(safeBatch)` call (with the lock dropped), `.onclose` could fire on another ASIO thread, erase the connection from `m_Clients`, and Crow could destroy the connection — leaving `client` a dangling pointer at `send_text` time.  The narrowness of the window made the bug rare in practice (Crow's `send_text` is asio::post-based, see arch table), but the pattern is fundamentally wrong.

**Verification:** Holds up.  Code at `webServer.cpp:4261` was the snapshot-then-per-client-lock pattern, exactly as audited.

**Change:** Restructure to hold `m_Mutex` for the entire send loop.  Remove the `clients` snapshot variable (no longer needed) and iterate `m_Clients` directly under the lock.  The architecture-table-justified asio::post-internals of Crow's `send_text` mean each call returns in microseconds (post to the connection's strand and return), so the lock window stays small even with many clients.  `.onclose` is automatically gated on `m_Mutex` (it acquires the same mutex to erase from `m_Clients`), so while we hold the lock no connection in our iteration set can be destroyed.  In-code comment documents the rationale + cites the Crow internals path that makes the lock window cheap.

**Verified at runtime:**
- 28-test assistant suite + hermetic dispatcher PASS.
- **Live WS smoke:** Connected a Python `websockets.connect()` client to `wss://localhost:8443/ws` with the admin bearer token, sent one ping frame, received the drain output, disconnected.  `debug_signals` post-test: `websocket_total_connects: 1`, `websocket_total_drains: 1`, `websocket_last_drain_bytes: 722`, `websocket_last_drain_messages: 4`, `websocket_peak_drain_duration_us: 44`, `websocket_total_disconnects: 1`, `websocket_clients: 0`.  44 μs drain duration with the lock held confirms the "lock window stays small" claim — and the new code path runs without crash on an actual client connection.
- **Not directly verified:** the pre-fix UAF window itself.  Reproducing the original bug requires concurrent disconnect-during-drain timing that's hard to engineer without a stress fixture.  The new code is structurally immune (no unlock-send window exists) so the verification posture is "the bug class is gone by construction" rather than "the specific bug instance was reproduced and fixed".

**Ramifications:**
- Callers touched: zero — only `DrainPendingBroadcasts`'s internal structure changed; producers (`Broadcast` / `BroadcastJSON` / `EnqueueLogLine` / etc.) still enqueue under the same locking contract.
- Tests touched: 28-test assistant suite + hermetic dispatcher PASS; the live WS smoke exercises the new code path end-to-end.
- Docs touched: none (the in-code comment is the canonical doc; `doc/architecture.md` line 394 already enshrines "drain on the IO thread, asio-posted send_text" as the design decision).
- Blast radius: drain duration (formerly: lock-snapshot, then unlocked send) now serializes against `.onopen` and `.onclose`.  At the observed 44 μs with one client, the back-pressure on connect/disconnect during drain is negligible.  If a future workload runs hundreds of concurrent clients with long batches, the trade-off may need re-examination — but that's far past current scale.

---

### `SetWorkflowRuntimeManager` dangling lambda — observer detach on swap and shutdown

**Finding (CRITICAL safety):** `SetWorkflowRuntimeManager` installs a `RunTerminalObserver` lambda on the WRM that captures `m_AdhocManager.get()` as a raw pointer.  `m_AdhocManager` is a `unique_ptr` member of `WebServer`; the lambda's captured raw pointer is valid only as long as `WebServer` lives.  Two failure modes: (a) `SetWorkflowRuntimeManager` re-called with a different WRM pointer leaves the *old* WRM still holding the lambda — if the old WRM later fires the observer after `WebServer` has rotated `m_AdhocManager` (or itself been destroyed), the lambda dispatches into a dead pointer.  (b) Shutdown ordering is fragile: `WebServer::~WebServer()` runs `Stop()` which currently does NOT clear the observer; if the WRM is destroyed *after* `WebServer` (process-exit ordering), and a run terminal-fires during that window, the lambda's `adhoc->OnRunCompleted(runId)` is a use-after-free.

**Verification:** Holds up.  Code at `webServer.cpp:179` captures `AdhocWorkflowManager* adhoc = m_AdhocManager.get();` and routes the WRM observer through it.  `SignalStop()` does not detach.

**Change:** Two checkpoints.
1. **Swap-detach in `SetWorkflowRuntimeManager`**: when the in-coming `workflowRuntimeManager` differs from the existing `m_WorkflowRuntimeManager` and the existing one is non-null, call `m_WorkflowRuntimeManager->SetRunTerminalObserver({})` (an empty `std::function`) before swapping.  Holds the existing `m_Mutex` already; `WorkflowRuntimeManager::SetRunTerminalObserver` acquires its own `m_Mutex` internally, no lock-order issue.
2. **Shutdown-detach in `SignalStop`**: at the very top of `SignalStop` (before `m_AiJcwfService.Shutdown()` / `m_AssistantController.Shutdown()` / WS-close loop), take `m_Mutex` and clear the observer if `m_WorkflowRuntimeManager` is non-null.  This is the defensive belt — `m_AdhocManager` won't be touched by an in-flight observer dispatch from this point forward.

Each checkpoint also emits a permanent `LOG_APP_INFO` trace immediately after the clear:
- `WebServer::SetWorkflowRuntimeManager: detached run-terminal observer from previous WRM before swap`
- `[shutdown] WebServer::SignalStop: detached run-terminal observer from WRM`

This is observability, not just instrumentation — both events are one-shot per transition (zero log volume in steady state) but provide positive evidence in `log/log.txt` that the new lifetime contract is enforced when the transition fires.  In-code comments at both checkpoints cite the audit and explain why both are needed.

**Verified at runtime:**
- Build clean.
- 28-test assistant suite PASS — exercises the controller-shutdown path which transitively exercises `SignalStop`.
- A workflow run that completed during this session emitted no observer-related crash logs (`adhoc_runs_active: 1` from a prior test pre-existed; `workflow_runs_total_completed: 2` ticked over correctly during the sitting).
- **Shutdown-detach observable post-sitting:** restarted the debug binary, triggered `POST /api/shutdown`, and confirmed `[2026-04-30 20:30:58.377] [Application] [info] [shutdown] WebServer::SignalStop: detached run-terminal observer from WRM` lands in `log/log.txt`.  The new code path runs at every clean shutdown — the lifetime contract is observable, not merely structural.
- **Not directly verified:** the swap-detach branch (re-init `SetWorkflowRuntimeManager` with a different non-null WRM).  `SetWorkflowRuntimeManager` is called exactly once at startup in current production, so the new branch is exercise-pending — the matching `LOG_APP_INFO` trace is in place to surface the event when (if) a future re-init flow lands.  The "destroy WRM after WebServer + fire a run-terminal" race is similarly fixture-dependent; the fix is structural ("observer is cleared on every transition") rather than reactive ("we caught the bug fire"), so the verification posture is "the lifetime contract is now enforced and observable at every transition".

**Ramifications:**
- Callers touched: `SetWorkflowRuntimeManager` and `SignalStop` only.
- Tests touched: 28-test suite PASS.
- Docs touched: none (in-code comments document the contract).
- Blast radius: a re-init of WRM (currently unused — the function is called once at startup) now correctly detaches the old observer.  Existing single-init flows are unchanged.

---

### `m_ClientCount` consistency — explicit performance-hint contract

**Finding (HIGH safety):**  `m_ClientCount` was a `std::atomic<size_t>` mirror of `m_Clients.size()`, updated under `m_Mutex` alongside the set in `.onopen` / `.onclose`, but read without the lock in the hot logging-path early-exits (`Broadcast` / `BroadcastJSON` / `EnqueueLogLine`).  The audit's concern: the atomic creates a "false sense of lock-free correctness" — the comment `// lock-free mirror of m_Clients.size() for EnqueueLogLine` doesn't make the racy-by-design semantics explicit.  Under load, the load may not match the locked set, leading to benign over-sends.

**Verification:** Holds up.  Code at `webServer.h:405` was the terse one-line comment.

**Change:** Replace the one-line `// lock-free mirror …` comment with a multi-line contract block making three things explicit: (1) it's a *performance hint*, never a routing source-of-truth — `m_Clients` under `m_Mutex` is authoritative; (2) the worst-case race is a benign over-send (broadcast queued for a just-disconnected client) or under-send (broadcast skipped for a just-connecting client); (3) the rule for future readers — "never use this atomic to decide whether a client receives a message — only whether the broadcast machinery runs at all on the producer side".  No code change.

**Verified at runtime:**  Trivial (comment only).  Build clean.

**Ramifications:**
- Callers touched: zero.
- Tests touched: 28-test suite + hermetic PASS (no behaviour change).
- Docs touched: none beyond the in-code comment.
- Blast radius: zero.

---

### TOCTOU sweep — drop `fs::exists()` precheck on three handlers

**Finding (HIGH safety):**  Three sites used the `fs::exists(path); if (!exists) return 404; std::ifstream ifs(path); if (!ifs.is_open()) return 500;` pattern.  Between the existence check and the open, another process or thread can delete or replace the file.  For `ServeDashboardIndex`, the consequence is a 500 instead of 404 (mostly cosmetic).  For `HandleWorkflowVersionGetGet` and `HandleWorkflowVersionRestorePost`, a replaced version file could serve unintended content.

**Verification:** Holds up.  Three sites flagged in the audit, all present in the current code.

**Change:** Drop the `fs::exists()` precheck at all three sites; rely on the open-status to drive the response.
1. **`ServeDashboardIndex`**: directly call `ServeStaticFile(distIndex)`; on 404 from the helper, substitute the developer-friendly "Dashboard UI build not found. Please run …" 500 response (preserves the UX for the missing-build case while closing the TOCTOU).
2. **`HandleWorkflowVersionGetGet`**: drop the precheck; conflate `ifs.is_open()==false` into a single 404 `version_not_found` response (covers both "missing" and "permission denied"; for a read-only endpoint, the distinction is not actionable).
3. **`HandleWorkflowVersionRestorePost`**: same treatment for the version-read step.  Bonus: drop the inner `fs::exists(targetPath)` precheck around the best-effort backup-current branch — `fs::copy_file` already populates `std::error_code` on missing-source, so the `ec`-absorbing pattern handles it cleanly.

In-code comments at each site cite the TOCTOU class + the new error-routing rationale.

**Verified at runtime (initial, in-sitting):**
- `curl https://localhost:8443/` → HTTP 200 (dashboard index serves cleanly).
- `curl /api/workflows/foo-bar-baz/versions/20260101T000000` (no such workflow) → HTTP 404 + `{"error":"version_not_found", ...}` — the new error-routing produces the right code.
- 28-test suite + hermetic PASS.

**Verified at runtime (post-sitting follow-up pass):**
- **`ServeDashboardIndex` missing-build path**: `mv dashboard/ui/dist/index.html /tmp/...; curl /` → HTTP 500 with the developer-friendly `"Dashboard UI build not found. Please run: cd dashboard/ui && npm install && npm run build"` body; restored the file → HTTP 200 again.  `git diff dashboard/ui/dist/index.html` clean post-test.  Confirms the substitution branch (`ServeStaticFile` 404 → developer-friendly 500) runs as designed without re-introducing the TOCTOU.
- **`HandleWorkflowVersionRestorePost` TOCTOU verified, but exposed a pre-existing bug**: `POST /api/workflows/exampleMakefile4/versions/20260430T022450/restore` (a real version timestamp) returned `restore_failed: UNCLOSED_STRING` — but the failure was at a *downstream* step (`WorkflowRegistry::SaveOrUpdateWorkflowFromJson` parsing zip-container bytes as JSON), not at the TOCTOU-touched read or backup paths.  Sitting 8's actual changes verified clean:
  - The `is_open()`-driven 404 path tested via the bogus-version POST above.
  - The dropped inner `fs::exists(targetPath)` precheck → `fs::copy_file`-with-`ec` pattern produced the expected best-effort backup artefact: `workflows/.history/exampleMakefile4/20260501T032250.jcwf` with md5 `2398043b...` matching the pre-restore live workflow byte-for-byte.
  - Live workflow md5 unchanged before and after the failed restore — the TOCTOU code paths are **not corrupting state** when the downstream JSON-parse fails.
  - The pre-existing bug ("restore handler reads `.jcwf` zip bytes as JSON") is tracked in `todo.md` under "Loose follow-ups" as `HandleWorkflowVersionRestorePost is broken since JCWF moved to zip containers`.  Out of cluster C scope; sitting 8's TOCTOU work is unaffected.

**Ramifications:**
- Callers touched: zero external.
- Tests touched: above.
- Docs touched: none beyond the `todo.md` entry for the surfaced pre-existing bug.
- Blast radius: a request that previously got 404 + a precise "version_not_found" error and is now permission-denied gets the same 404 + same error message (the distinction is lost).  Acceptable for these endpoints.

---

## Skipped findings table — Sitting 8

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| `DrainPendingBroadcasts` per-client `m_Mutex` acquisition cost | `webServer.cpp` | MEDIUM safety | Audit's MEDIUM finding: the per-client lock acquisition makes the drain O(N) lock waits; suggests a single lock for the whole loop.  This sitting's CRITICAL fix already produces the single-lock-loop shape (sitting 8 task B), so the MEDIUM is closed as a structural side-effect.  No separate work item. |
| `HandleOAuthCallbackGet` CURL handle leak on exception paths | `webServer.cpp` | HIGH resource | Out of cluster C scope (CURL/RAII concern, not concurrency).  Bundle with a future "curl wrapper RAII pass" that also covers the OAuth signing path. |
| `WriteTextFileAtomic` failure-path verification under fault injection | `webServer.cpp` (sitting 7 carry) | (verification gap) | Not a sitting 8 finding; carried from sitting 7's skipped list. |
| `replaceField` depth-aware verification under synthetic key collision | `webServer.cpp` (sitting 7 carry) | (verification gap) | Not a sitting 8 finding; carried from sitting 7's skipped list. |
| Stress fixture for the `DrainPendingBroadcasts` UAF reproducer | `webServer.cpp` | (verification gap) | The fix is structural ("no unlock-send window can exist"), so the absence of a reproducer is acceptable.  A future stress-test fixture (N concurrent clients each connecting / disconnecting / sending random pings) would harden confidence — track for the eventual cybersec test fixture sitting. |
| Teardown-order test fixture for the `SetWorkflowRuntimeManager` observer dangling | `webServer.cpp` + WRM | (verification gap) | Same shape as above — fix is structural; a fixture that re-initialises WRM mid-process plus shuts down out-of-order would harden confidence.  Defer with the cybersec fixture sitting. |

---

## Sitting 9 — cloudConnectionManager.cpp Cluster 9A: JSON / serialization safety

**Scope locked:** the JSON / serialization cluster in `application/cloud/cloudConnectionManager.cpp` — one CRITICAL convergence carry (the **6th and last** anon-namespace `JsonEscape` copy in the codebase, post-sitting-4 convergence sweep), two HIGH parser-side findings (`ParseConnectionsJson` unbounded allocation + state-clearing on partial parse failure), one MEDIUM concurrency finding (`SerializeToJson` iterates `m_Connections` without holding `m_Mutex`), one MEDIUM logging finding (silent per-element parse skips), and one MEDIUM RFC compliance finding (the local `JsonEscape` doesn't encode all RFC 8259 §7 control characters — closed as a structural side-effect of the convergence).  Boundary at sitting-end: `cloudConnectionManager.cpp`'s JSON cluster is closed; the connection-load path (engine startup → REST save) round-trips arbitrary bytes 0x00–0x7F + UTF-8 cleanly; the parser bounds input size, array length, individual field length, and params count + length, and rejects oversized input pre-allocation with an ERROR-level log line that mentions the path; on any parse failure the live `m_Connections` is left untouched (was: wiped at function entry).  Concurrency / lifetime cluster (the `GetConnection` raw-pointer-across-lock-boundary HIGH and `IsDirty/ClearDirty` data-race HIGH), input-validation cluster (name length / charset, endpoint SSRF), and the OAuth / network-egress cluster (which lives in the per-connector files `azureBlobConnector.cpp`, `googleSheetsConnector.cpp`, `oneDriveConnector.cpp`, NOT in cloudConnectionManager) are deferred to sittings 10+.

**Audit grouping correction:** the sitting 8 hand-off enumerated the cluster 9A scope based on a sub-agent's reading of the audit indexes, which conflated `application/cloud/cloudConnectionManager.cpp` with `engine/keys/keyManager.cpp` (the keystore) and `application/cloud/cloudConnectionPool.cpp` (the OAuth token cache).  The CRITICAL "JSON injection in `SerializeToJson` — plaintext string fields not escaped" finding the agent listed was actually keyManager's `SerializeToJson` (with its `display_name` / `endpoint` / `api_key` / `client_id` / etc. fields), not cloudConnectionManager's.  Same for the `Unlock` / encrypted-blob / master-password findings.  The audit-IDs for cloudConnectionManager itself (combinedCyberSecAudit.md ~L2154 + combinedSafetyAudit.md ~L3627) are: 3 HIGH (GetConnection lifetime, ParseConnectionsJson allocation, ParseConnectionsJson state-clearing — the latter two closed in this sitting), 5 MEDIUM (JsonEscape RFC, name-length, endpoint-SSRF, SerializeToJson lock, per-element error logging — three of the five closed in this sitting), 2 LOW (IsDirty data race, AddConnection redundant copy, GetConnection-as-optional, [[nodiscard]] sweep — all out of cluster).  This sitting's scope is the right *shape* (JSON / serialization), just narrower and more contained than the agent's report suggested.

### `JsonEscape` convergence — last anon-namespace copy in the codebase

**Finding (carry-over from sitting 4 convergence + MEDIUM RFC):** `cloudConnectionManager.cpp:34` defined a local `static std::string JsonEscape(std::string const& s)` that handled `"`, `\\`, `\n`, `\r`, `\t` but emitted every other byte verbatim — including bytes 0x00–0x08 and 0x0B / 0x0C and 0x0E–0x1F, which RFC 8259 §7 requires to be escaped as `\u00XX` in JSON string content.  Any field value (connection name, endpoint, key name, params key/value) containing such a byte produced a JSON file that would be rejected by a strict parser on next load.  This was also the **6th and last** anon-namespace `JsonEscape` copy across the codebase after sittings 4 + 5's sweep (the others lived in `assistantSession.cpp`, `assistantMemory.cpp`, `workspaceIndexer.cpp`, `assistantController.cpp`, `assistantTools.cpp` — all converged onto `JsonHelper::EscapeJsonString`).  cloudConnectionManager was the one outlier the convergence sweep didn't reach because it lives outside `application/assistant/`; the sub-agent that closed sittings 4–5 didn't search `application/cloud/`.

**Verification:** Holds up.  `grep -n "JsonEscape\|EscapeJsonString" application/cloud/cloudConnectionManager.{cpp,h}` returned the local definition at line 34 and 5 call sites in `SerializeToJson` (lines 232–244).  No call sites outside this file.

**Change:** Add `#include "json/jsonHelper.h"` to `cloudConnectionManager.cpp`.  Delete the local `static std::string JsonEscape(...)` definition (lines 34–51).  Replace each of the 5 call sites with `JsonHelper::EscapeJsonString(...)`.  No header change — the function was anonymous and the helper is a static method, so callers outside the file are unaffected (there were none).

**Verified at runtime:**
- Studio debug build clean.  No diagnostics from the include path resolution.
- 28-test assistant non-AI suite: PASS in 2.1 s (every JSON-emitting protocol message routes through `JsonHelper::EscapeJsonString` for sessionId / text content; the convergence change doesn't perturb the path).
- Hermetic dispatcher: PASS.
- **Live JSON-escape round-trip with hostile-byte payload:**  `POST /api/connections` with a connection whose `params.comment` field contained 8 hostile bytes (literal `"`, `\`, U+000A, U+0009, U+0007 bell, U+0008 BS, U+000C FF, U+001F unit-sep), then `POST /api/connections/save`, then `python3 -c 'json.load(open("connections.json"))'` parsed cleanly.  All 8 bytes survived end-to-end via proper escapes: `\"`, `\\`, `\n`, `\t`, ``, ``, ``, ``.  Pre-fix, bytes 0x07 / 0x08 / 0x0C / 0x1F would have been emitted as raw bytes, breaking the JSON file on next load.  Test connection cleaned up via `DELETE /api/connections/audit-hostile-9a` + `POST /api/connections/save`; final connection list verified byte-equivalent (sorted-name set match, content-equivalent — md5 differs only because `std::unordered_map` iteration order isn't stable across re-inserts).

**Ramifications:**
- Callers touched: zero external.  The function was file-local; convergence is purely an implementation detail.
- Tests touched: 28-test suite + hermetic PASS; the live round-trip is a new positive test for control-char preservation.
- Docs touched: none.  The `feedback_simdjson_only` discipline rule already names `JsonHelper` as the canonical escape helper; this sitting is just another instance of that pattern.
- Blast radius: zero behaviour change for valid input.  For input containing previously-mishandled control bytes, the on-disk JSON is now correct (RFC 8259 §7-compliant) where it previously would have been corrupt.  No deserialization regression — `simdjson` accepts both `\u00XX` and the named-escape forms (`\b`, `\f`).

---

### `ParseConnectionsJson` hardening — scratch-then-swap + size + count + length caps + per-element logging

**Finding (HIGH safety + HIGH cyber + 2 MEDIUM):** Four issues clustered in one ~75-line function:
1. (HIGH cyber) `m_Connections.clear()` was called unconditionally at function entry, before any validation of the incoming JSON.  Any subsequent failure (parse error, missing `"connections"` key) returned `false` with the connection store empty.  An attacker who can send a malformed payload to the load path wipes all in-memory cloud connection configuration.
2. (HIGH cyber) The function accepted an arbitrary-size JSON string and iterated its `"connections"` array without any cap on count or per-field length.  An attacker controlling the load source could supply a multi-million-element array or multi-MB field values, exhausting memory.  Combined with the load path in `engine.cpp` that read the file with no size cap, the attack chain ran from "place a 100 GB file at `connections.json`" → `std::string` of 100 GB → `simdjson::padded_string` of 100 GB → process death.
3. (MEDIUM safety) Per-element `get_object()` failures fell through to `continue` with **no log entry**.  Same for connections skipped because `conn.m_Name.empty()`.  A malformed entry was silently dropped — no diagnostic, no warning, no signal that the live config was incomplete.
4. (MEDIUM safety) The element loop returned partial state on per-element parse errors — even if the JSON was structurally valid (passed the document and `connections` array checks), per-element corruption silently skipped entries while reporting `true` (success).

**Verification:** All four issues confirmed in code at `cloudConnectionManager.cpp:142–219` (pre-fix line numbers).

**Change:** Restructure `ParseConnectionsJson` end-to-end:
1. **Pre-parse size cap** (1 MB on `json.size()`).  Reject oversized input at function entry with an ERROR-level log line that mentions the size + cap + "leaving in-memory connections untouched" — before any allocation, before `simdjson::padded_string` doubles memory pressure.
2. **Scratch-then-swap.**  Parse into a function-local `std::unordered_map<std::string, CloudConnection> staging`.  Only at the very end, after the entire parse has completed without error, take `m_Mutex` (unique_lock) and `m_Connections = std::move(staging)`.  Any earlier `return false` leaves the live state untouched.  This matches the "atomic write — target untouched on failure" pattern from sitting 7's `WriteTextFileAtomic` work, applied to in-memory state.
3. **Per-array, per-field, per-params caps** (`kMaxConnections=1024`, `kMaxFieldBytes=4096`, `kMaxParamsPerConnection=256`, `kMaxParamFieldBytes=1024`).  Each cap rejects on overflow with an ERROR (count cap, terminating the parse) or a WARN (field-length cap, skipping the element; params caps, skipping the entry) log line.  All log lines include the element index for diagnosis.
4. **Per-element error logging.**  Every previously-silent skip now emits `LOG_CORE_WARN` with the element index and reason: malformed object, oversized field, missing-or-empty `name`, oversized params entry, params-count overflow.  The dashboard's run-analyzer (per `feedback_log_failures`) is ERROR-only, so these WARNs are diagnostic — visible in `log/log.txt` for an operator to investigate config issues without surfacing as a "run failure" on the dashboard.

**Verified at runtime:**
- Build clean.  28-test suite + hermetic dispatcher PASS.
- **Pre-read file-size cap (engine.cpp side, see next finding) live:**  Stopped the server, replaced `connections.json` with a 1 228 959-byte synthetic file (one connection, 1.2 MB comment), restarted.  Log shows: `[Engine] [error] CloudConnectionManager: '/home/beaumanvienna/dev/jarvisAgent/connections.json' size 1228959 bytes exceeds 1048576 byte cap; refusing to load`.  Server came up clean with 0 connections loaded; auth path + admin endpoints worked normally.  Pre-fix, the same file would have allocated 1.2 MB in `std::string`, then doubled in `simdjson::padded_string`, then triple-allocated through the per-element copies — all before any cap saw it.
- **In-function `kMaxConnections` cap live:**  Stopped the server, replaced `connections.json` with a 138 617-byte synthetic file containing **1 100** valid empty-params connections (well under the 1 MB file cap, well over the 1024 connection cap), restarted.  Log shows: `[Engine] [error] CloudConnectionManager::ParseConnectionsJson: connection count exceeds 1024; rejecting at element index 1024; leaving in-memory connections untouched` immediately followed by `[Engine] [warning] CloudConnectionManager: failed to parse '/home/beaumanvienna/dev/jarvisAgent/connections.json'`.  Confirms: (a) the cap fires at exactly the right index (the 1025th element triggers it), (b) the `staging` map is discarded (the live state would be untouched, but in this case there's nothing pre-existing to be untouched), (c) the engine.cpp loader's `WARN` line lands as expected.  Final restart with the original `connections.json` reloaded the canonical 14 connections cleanly.
- **Not directly verified:**  the per-field length caps and per-params caps under hostile input.  The mechanisms are mechanical (single comparison + log + skip) and exercised by the same code path; a fixture-driven test that pushes oversized fields through the REST API would harden confidence — track for a future fixture sitting.  The MEDIUM "per-element error logging" path is verified-by-construction: the new `LOG_CORE_WARN` calls live in the same fall-through paths as the pre-fix silent `continue`.

**Ramifications:**
- Callers touched: `ParseConnectionsJson` is called from exactly one site today — `engine/engine.cpp:234` (initial load).  No REST endpoint calls it; the REST path persists the in-memory state via `SerializeToJson` rather than round-tripping through this function.  So the strict in-function cap is defense-in-depth for any future caller (e.g., a future `POST /api/connections/reload` endpoint).
- Tests touched: 28-test suite + hermetic + live size-cap rejections.  None had to be updated — the pre-fix behaviour for valid input is preserved exactly.
- Docs touched: none.  The function's contract (return false → caller-decides; live state untouched on failure) is now actually true; pre-fix the live-state-untouched part of the contract was a lie.
- Blast radius: a malformed `connections.json` that previously wiped the live state on parse error now leaves it alone.  This is strictly safer — but if a deployment had been relying on "I can clear all connections by uploading a malformed file" (which would be insane), the workaround is `[]` for the array (empty + valid = wipes via the swap path).  No realistic regression.

---

### `SerializeToJson` — acquire shared_lock during iteration

**Finding (MEDIUM safety):** `SerializeToJson` is declared `const` and is called from the REST `POST /api/connections/save` handler (which writes the result to disk).  It iterates `m_Connections` and `conn.m_Params` with no lock acquisition.  A concurrent `AddConnection` / `RemoveConnection` / `ParseConnectionsJson` call (the latter holds `unique_lock` only briefly at end-of-function under the new structure) can rehash the map mid-iteration, invalidating the range-for iterator → undefined behaviour, in practice a crash or garbled JSON written to disk.

**Verification:** Holds up.  Code at `cloudConnectionManager.cpp:222–268` (pre-fix) had no lock acquisition.

**Change:** Add `std::shared_lock lock(m_Mutex);` at the top of `SerializeToJson`, matching the pattern of every other read method in the class (`GetConnection`, `GetConnectionNames`, `GetAllConnections`, `HasConnections`).  Lock is held for the entire string-building loop.

**Verified at runtime:**
- Build clean.  28-test suite + hermetic PASS.  Live `POST /api/connections/save` (the only caller) succeeded end-to-end during the hostile-byte round-trip test, producing a valid byte-stable JSON file.
- **Not directly verified:**  the multi-thread race condition itself.  The single-threaded test path doesn't reproduce it.  The fix is structural ("the iteration now happens under the lock that gates every writer"); a stress fixture (concurrent `AddConnection` + `POST /save`) would harden confidence.  Track with the cybersec fixture sitting.

**Ramifications:**
- Callers touched: zero.  `SerializeToJson` is called from the save handler (single-threaded per request) and from `KeyManager::Save` (which is itself single-threaded).  The shared_lock allows other readers to proceed concurrently.
- Tests touched: above.
- Docs touched: none.
- Blast radius: a writer that fires concurrently with a save request now waits for the save to complete instead of racing the iteration.  At human scale (kilobytes of JSON, microseconds of build time), the back-pressure is invisible.

---

### `engine.cpp` connections.json read-side size cap (defense-in-depth)

**Finding (HIGH cyber, primary half of the unbounded-allocation finding):**  `engine/engine.cpp:225–246` (pre-fix) had the `std::filesystem::exists(connectionsPath); std::ifstream file(...); std::string json((istreambuf_iterator<char>(file)), istreambuf_iterator<char>())` shape.  No size check on the read.  An attacker (or buggy upstream that placed a corrupt file) supplying a 100 GB `connections.json` would allocate 100 GB into `std::string` before `ParseConnectionsJson`'s in-function cap could reject it.  With the in-function cap added in the previous finding but no read-side cap, the chain is "100 GB file → 100 GB std::string allocation → in-function cap rejects" — process is OOM-killed before the rejection log line fires.

**Verification:** Holds up.  Code at `engine.cpp:225–246` was as described.

**Change:** Add a `std::filesystem::file_size(connectionsPath, ec)` pre-read check before the `ifstream` open.  Same `kMaxConnectionsFileBytes = 1 MB` threshold as the in-function cap.  Three branches:
1. `file_size` returns an error → `LOG_CORE_ERROR` with path + error, skip the load.
2. `file_size > cap` → `LOG_CORE_ERROR` with path + size + cap + "refusing to load", skip the load (this is the new size-cap rejection).
3. Otherwise → proceed with the existing `ifstream` + `ParseConnectionsJson` path unchanged.

**Note on the existing `fs::exists(...)` precheck on line 225:**  This is the same TOCTOU pattern that sitting 8 cleaned up in `webServer.cpp` (`ServeDashboardIndex` / `HandleWorkflowVersionGetGet` / `HandleWorkflowVersionRestorePost`).  The `file_size`-with-`error_code` overload returns an error if the file doesn't exist, so the `exists` precheck could be removed and the load gated on `!sizeEc && fileSize <= cap` instead.  Out of cluster 9A scope (it's a cybersec finding, not JSON / serialization) — track for a "sitting 12+ engine.cpp pass" or for whichever sitting picks up the loose-end TOCTOU work.

**Verified at runtime:**  Already covered in the `ParseConnectionsJson` finding above — the 1.2 MB synthetic file rejection log line came from this `engine.cpp` cap, not the in-function one (the file never reached `simdjson` because the read short-circuited).  The in-function cap is defense-in-depth that fires only for callers that bypass the engine.cpp loader (none today, but a future REST `POST /api/connections/reload` endpoint or test fixture could).

**Ramifications:**
- Callers touched: zero — only the engine startup path has this loader.
- Tests touched: live size-cap test as above.
- Docs touched: none.
- Blast radius: an operational deployment with a connections.json over 1 MB would be rejected at startup.  In normal use, 14 connections take ~6 KB; 1024 connections would take ~500 KB.  The 1 MB cap leaves headroom.  If a deployment legitimately needs more, the cap can be raised — both halves (this one + the in-function cap) would need to move in lockstep.

---

## Skipped findings table — Sitting 9

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| `GetConnection` returns a raw pointer that outlives the lock guard (use-after-free) | `cloudConnectionManager.{cpp,h}` | HIGH safety + HIGH cyber | Out of cluster 9A (concurrency / lifetime, not JSON / serialization).  Fix is `std::optional<CloudConnection>` return-by-value, which touches **7 external call sites** across `triggerEngine.cpp`, `webServer.cpp` (5 uses), `filterEngine.cpp`, `cloudTaskExecutor.cpp` — meaningful blast radius, deserves its own cluster-9C sitting.  Tracking for the next cloudConnectionManager sitting. |
| `IsDirty` / `ClearDirty` data race on `m_Dirty` | `cloudConnectionManager.{cpp,h}` | HIGH safety | Out of cluster 9A (concurrency, not JSON / serialization).  Two-line fix (acquire `shared_lock` in `IsDirty`, `unique_lock` in `ClearDirty`, OR change to `std::atomic<bool>`).  Bundle with the `GetConnection` fix in cluster 9C. |
| Connection name length / charset validation | `cloudConnectionManager.cpp` (`AddConnection`, `ParseConnectionsJson`) | MEDIUM cyber | Out of cluster 9A (input-validation, not JSON / serialization).  Combines naturally with the SSRF endpoint validation below — both are write-time validation, both belong in a future "input validation pass on the cloud surface" sitting.  Note: `kMaxFieldBytes=4096` from this sitting incidentally bounds name length at the parser entry, so log injection via newlines in names is partially mitigated (the JsonHelper escape converts newlines to `\n` literal); the audit's full fix (charset whitelist) is still pending. |
| Endpoint URL stored without SSRF validation | `cloudConnectionManager.cpp` | MEDIUM cyber | Same as above — input-validation cluster, not JSON / serialization.  Bundle with name-validation in the future input-validation sitting.  Defense-in-depth note: the actual SSRF *attack surface* lives in the per-connector files that issue HTTP requests using the endpoint; centralizing validation at the manager is the cheaper fix but the connectors are the source of truth. |
| `AddConnection` redundantly copies `m_Name` before the move | `cloudConnectionManager.cpp` | LOW safety | Cosmetic.  No correctness impact.  Defer to the eventual "Rust-emulating C++ defaults sweep" sitting that addresses move-semantics polish across the codebase (per `feedback_rust_emulating_defaults`). |
| `[[nodiscard]]` on `bool`-returning mutators (`AddConnection`, `UpdateConnection`, `RemoveConnection`, `ParseConnectionsJson`) | `cloudConnectionManager.h` | LOW safety | `[[nodiscard]]` sweep.  Defer to the Rust-emulating C++ defaults sitting.  Note: `feedback_rust_emulating_defaults` flags this as a category — apply uniformly across the codebase rather than file-by-file. |
| TOCTOU on `connections.json` path in `engine.cpp:225` (`fs::exists` then open) | `engine.cpp` | (cybersec finding) | Same pattern as sitting 8's webServer cleanup.  Out of cluster 9A scope (this sitting added the size cap; the TOCTOU is a sibling concern).  Track for a future engine.cpp pass; one-line fix (remove `exists` precheck, gate on `!sizeEc && fileSize <= cap`). |
| OAuth / TLS / network-egress findings on the cloud surface | `azureBlobConnector.cpp`, `googleSheetsConnector.cpp`, `oneDriveConnector.cpp`, `cloudConnectionPool.cpp` (sub-agent's report initially attributed these to cloudConnectionManager) | (multiple CRITICAL/HIGH) | Wrong-file attribution corrected at sitting start.  These findings live in the per-connector files and the OAuth token cache, not in the manager.  Bundle into clusters 9B (network egress: TLS verify-peer, SSRF on `m_TokenEndpoint`, OAuth body URL-encoding, response-body cap, redaction of HTTP error-body logs) and 9C (concurrency / lifetimes: `TokenEntry` references across unlock/lock, `RefreshLoop` dangling reference) for sittings 10–11. |
| `keyManager.cpp` JSON-injection / Unlock / encrypted-blob findings | `engine/keys/keyManager.cpp` | (sub-agent attribution error) | Initially listed in cluster 9A by the sub-agent; on close inspection, these findings live in keyManager (the keystore), not cloudConnectionManager.  Belong to a future Domain-3 (core-engine) sitting, not the cloud surface. |
| Per-field-length cap fixture-driven verification | `cloudConnectionManager.cpp` | (verification gap) | The mechanism is mechanical (one branch per field), but exercising it under live REST input would need an end-to-end fixture.  Track with the cybersec fixture sitting. |
| Stress test for concurrent `AddConnection` + `POST /save` against the new `SerializeToJson` lock | `cloudConnectionManager.cpp` + `webServer.cpp` | (verification gap) | Same shape as sitting 8's deferred stress fixtures — fix is structural, fixture would harden confidence; defer with the cybersec fixture sitting. |

---

## Sitting 10 — cloudConnectionManager.cpp Cluster 9C: concurrency / lifetime

**Scope locked:** the two HIGH concurrency findings on `application/cloud/cloudConnectionManager.{cpp,h}` deferred from sitting 9.  HIGH safety + HIGH cyber: `GetConnection` returns a raw `CloudConnection const*` pointer that outlives the function-local `shared_lock` guard — any concurrent writer (`AddConnection` / `UpdateConnection` / `RemoveConnection` / `ParseConnectionsJson`) that rehashes the `m_Connections` map or erases the entry between return-from-GetConnection and caller-dereference produces use-after-free.  HIGH safety: `IsDirty()` / `ClearDirty()` access `m_Dirty` (a plain `bool`) without holding `m_Mutex`; every writer sets `m_Dirty = true` under `unique_lock`, but lock-free read/write from a different thread is a data race under the C++ memory model.  Boundary at sitting-end: `cloudConnectionManager`'s **entire CRITICAL/HIGH cluster (sittings 9 + 10) is closed**.  Remaining findings on the file are MEDIUM (name length / charset, endpoint SSRF — input-validation, deferred to a future input-validation pass) and LOW (`AddConnection` redundant copy, `[[nodiscard]]` sweep — both bundle with the eventual Rust-emulating C++ defaults sweep).

### `GetConnection` raw-pointer-across-lock-boundary — return `std::optional<CloudConnection>` by value

**Finding (HIGH safety + HIGH cyber):** `GetConnection` was declared `CloudConnection const* GetConnection(std::string const& name) const`.  The implementation acquired `std::shared_lock lock(m_Mutex)`, located the entry via `m_Connections.find(name)`, and returned `&it->second` after the lock guard's destructor had already released the mutex (the guard's lifetime ends with the function scope, but the pointer escapes that scope).  Any caller that subsequently dereferenced the returned pointer was reading into a map entry that a concurrent writer could have invalidated — by erasing the entry (`RemoveConnection`), rehashing the map on insert (`AddConnection`), or calling `staging.swap(m_Connections)` from sitting 9's new `ParseConnectionsJson` end.  The audit cited this as the canonical "C++ borrow-checker gap" — Rust's `Option<&CloudConnection>` would refuse to compile because the reference's lifetime exceeds the lock guard's.

**Verification:** Holds up.  Code at `cloudConnectionManager.cpp:82–91` (pre-fix) was the raw-pointer-after-shared-lock pattern, exactly as audited.

**Caller-side audit before committing to the fix:**  All 7 external call sites were read end-to-end to confirm the value-copy fix is correct:
- `application/workflow/triggerEngine.cpp:712` — email_watch poll loop, dereferences `*connection` for `EmailConnector::ResolveCredentials` + `EmailConnector::CheckForNewMail` (both do IMAP network I/O, **multi-second hold**).
- `application/web/webServer.cpp:6299` (`HandleConnectionUpdatePut`) — single-line `CloudConnection updated = *existing;` value-copy then done with the pointer (brief hold).
- `application/web/webServer.cpp:6398` (`HandleConnectionTestPost`) — dereferences `connection->m_Type` + `*connection` passed to `connector->TestConnection` (network I/O hold).
- `application/web/webServer.cpp:6485` (`HandleOAuthAuthorizeGet`) — multiple `connection->m_AuthType` / `connection->m_Params.find(...)` / `connection->m_Type` reads, brief.
- `application/web/webServer.cpp:6655` (`HandleOAuthCallbackGet`) — multi-step OAuth flow, dereferences `connection->m_Params` + `connection->m_Type` over **the entire OAuth callback path** (potentially multi-second hold during token exchange with the provider).
- `application/workflow/filter/filterEngine.cpp:471` — pure read of `conn->m_Type` / `conn->m_Endpoint` / `conn->m_KeyName` / `conn->m_Params` into a `resolvedFilter` struct, brief.
- `application/cloud/cloudTaskExecutor.cpp:76` — **the longest hold by a wide margin**.  Holds the pointer through `connector->ResolveCredentials(*connection)` (network I/O — OAuth refresh, JWT generation, SigV4 derivation) → audit-log line that reads `connection->m_Type` → circuit-breaker check → template expansion → `ExecuteCloud(*connection, ...)` (the actual cloud operation: S3 upload, IMAP fetch, GCS list, etc.) — easily 5+ seconds for typical cloud tasks.

Every call site uses the pointer briefly-or-as-a-reference-passed-to-a-function and never stores it past function scope.  **No call site captures `connection` into a long-lived struct or detached lambda** — so the value-copy fix is correct without follow-up at any caller.

**Change:**
1. **Header (`cloudConnectionManager.h`):** add `#include <optional>`; change return type to `[[nodiscard]] std::optional<CloudConnection> GetConnection(std::string const& name) const`.  Multi-line comment above the declaration documents (a) why the return is by-value not by-reference (lifetime contract), (b) why `[[nodiscard]]` (analogous to Rust's `#[must_use]` on `Option`), (c) why the copy happens under the lock (so subsequent writer mutations are invisible to callers — but invisibility is the safety property, not a regression).
2. **Implementation (`cloudConnectionManager.cpp`):** unchanged shape (`shared_lock` → `find` → return).  `return nullptr;` → `return std::nullopt;`.  `return &it->second;` → `return it->second;` — the value copy is constructed inside `std::optional` while `lock` is still held; ownership transfers to the caller cleanly when the function returns.  In-code comment cites the audit + the Rust idiom.
3. **Callers (7 sites):** mechanical type substitution.  `CloudConnection const* connection = ...GetConnection(...)` → `auto connection = ...GetConnection(...)`.  Same for `auto const*` callers and `CloudConnection const* existing` (HandleConnectionUpdatePut).  `if (!connection)`, `connection->m_Field`, `*connection` (passed to `TestConnection` / `ResolveCredentials` / `ExecuteCloud` / `CheckForNewMail` / etc.) all work unchanged because `std::optional<T>` overloads `operator bool`, `operator->`, and `operator*` with the exact same syntax callers were using on raw pointers.  No call-site logic changed.  In-code comment at `cloudTaskExecutor.cpp:76` (the longest-hold site) cites the lifetime guarantee.

**Verified at runtime:**
- Studio debug build clean (`make config=debug`).  All 7 caller files recompiled and linked without diagnostic.
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- **Live exercise of all five `webServer.cpp` call sites:**
  - `GET /api/connections` — 14 connections returned (via `GetAllConnections`, separate path; baseline).
  - `PUT /api/connections/my-polarion` with `{"endpoint":"https://polarion.company.com"}` (line 6299) → HTTP 200 + `{"name":"my-polarion","ok":true}`.  Exercises `auto existing = ...GetConnection(...)` + `CloudConnection updated = *existing;` value-copy + `m_Dirty = true` (atomic write under unique_lock).
  - `POST /api/connections/save` (line 6448 path → `SerializeToJson` → `WriteTextFileAtomic` → `ClearDirty`) → HTTP 200 + `{"path":"...","ok":true}`.
  - `POST /api/connections/my-polarion/test` (line 6398) → HTTP 400 + `{"error":"test_failed","message":"Polarion connectivity test failed: curl error: Could not resolve hostname"}`.  The DNS failure is expected in this environment; the relevant verification is that the handler ran the full path through `connector->TestConnection(*connection, errorMessage)` without crashing — confirming the value-copy hand-off works for a function taking `CloudConnection const&`.
  - `GET /api/connections/my-onedrive/oauth/authorize` (line 6485) → HTTP 200 with a valid Microsoft OAuth URL `https://login.microsoftonline.com/common/oauth2/v2.0/authorize?client_id=...&scope=Files.ReadWrite%20offline_access&...`.  Exercises `connection->m_AuthType` / `connection->m_Params.find("client_id")` / `connection->m_Params.find("scopes")` / `connection->m_Type` reads on the optional.
- **Live exercise of `cloudTaskExecutor.cpp:76` (the longest-hold site) via real workflow run:** `mcp__j9t__run_workflow emailDemo` started run `emailDemo_1777690450`; status post-completion: 3 tasks succeeded (`ai_reply`, `fetch_email`, `send_reply`) in 3 seconds end-to-end.  Both `fetch_email` (IMAP fetch via `EmailConnector::ExecuteCloud`) and `send_reply` (SMTP send via the same path) ran through `ICloudTaskExecutor::Execute` — each one pulled the optional from `GetConnection`, held it through `ResolveCredentials` (network I/O), the audit log, the circuit-breaker check, template expansion, and `ExecuteCloud` (IMAP/SMTP network round-trip).  The pre-fix raw pointer had a multi-second exposure window during which a concurrent connection mutation could have invalidated it; the optional's value-copy makes that window zero by construction.
- **Not directly verified:**
  - The `triggerEngine.cpp:712` email_watch path — requires the email_watch trigger to actually fire (which it does on a poll timer; the debug session's run was below the poll interval).  The compile + caller-audit + in-line shape match the verified sites; the runtime path is identical.
  - The `filterEngine.cpp:471` Polarion filter path — requires a Polarion-filtered workflow run, but `my-polarion` has no live network in this environment (DNS failure on the test connection).  Compile + caller-audit confirms the change is correct.
  - The race window itself.  Reproducing the original UAF would require concurrent connection mutation timing during a long-held caller dereference; the fix is structural (the bug class is gone by construction — no raw pointer escapes the lock guard), so the verification posture is "the lifetime contract is type-system-enforced now" rather than "the specific bug instance was reproduced and fixed".

**Ramifications:**
- Callers touched: 7, all updated to `auto` deduction.  No call-site logic changed.
- Tests touched: 28-test suite + hermetic dispatcher PASS; the live PUT / save / test / oauth-authorize / emailDemo run smokes covered six of seven call sites end-to-end.
- Docs touched: in-code comments on the header declaration + the implementation + the longest-hold caller site (`cloudTaskExecutor.cpp:76`).  No external doc updates needed (the lifetime contract is implementation detail).
- Blast radius: a caller that previously held the raw pointer across a lock-violating operation (an attacker-induced concurrent mutation) now holds a value-copy that is guaranteed-safe.  The trade-off is one `CloudConnection` copy per `GetConnection` call (a `std::string` × 4 + a `std::map<std::string, std::string>` copy) — at typical sizes (4-5 small strings + ≤ 10 params) this is on the order of a few hundred bytes, negligible relative to the cloud-I/O latency that follows the lookup.  No call site is hot enough for the copy to register on a profile.

---

### `m_Dirty` data race — `bool` → `std::atomic<bool>` with documented memory ordering

**Finding (HIGH safety, LOW cybersec — chose the higher severity for closure):** `m_Dirty` is a plain `bool` member.  Every writer (`AddConnection` / `UpdateConnection` / `RemoveConnection` / `ParseConnectionsJson`) sets `m_Dirty = true` under `unique_lock(m_Mutex)`, but `IsDirty() const { return m_Dirty; }` and `ClearDirty() { m_Dirty = false; }` access the flag without any synchronization.  The audit's concern: a persistence thread calling `IsDirty()` concurrently with a writer thread setting `m_Dirty = true` is a data race on a non-atomic `bool` → undefined behaviour under the C++ memory model, and in practice can lose the dirty signal (writer flips `true`; reader reads `false` from a stale cached register; the change is never persisted to disk).

**Verification:** Holds up.  Header at `cloudConnectionManager.h:64` had `bool m_Dirty{false};` with `IsDirty()` / `ClearDirty()` inline-and-lock-free at lines 54–55.

**Change:**
1. **Header**: add `#include <atomic>`; change `bool m_Dirty{false};` → `std::atomic<bool> m_Dirty{false};`.  Update `IsDirty()` to `return m_Dirty.load(std::memory_order_acquire);` and `ClearDirty()` to `m_Dirty.store(false, std::memory_order_release);`.  Multi-line comment documents the ordering choice: acquire on load + release on store gives a happens-before edge from the writer's `m_Dirty = true` (sequenced after the map mutation, then released by the unique_lock unlock) to a subsequent `IsDirty() == true` reader, so the reader observing `true` is also guaranteed to observe the map mutation that triggered it.
2. **Implementation**: writer sites (`m_Dirty = true;` inside `AddConnection` / `UpdateConnection` / `RemoveConnection` / the `ParseConnectionsJson` swap point) need NO change — `std::atomic<bool>::operator=(bool)` defaults to `seq_cst`, which is strictly stronger than the `release` required.  The unique_lock unlock provides the acquire-release pairing on the map side, and the atomic provides the cross-thread visibility on the dirty side; they layer cleanly.

**Verified at runtime:**
- Build clean.  28-test suite + hermetic PASS.
- **Dirty-flag round-trip live:**  Initial `GET /api/connections` reported `dirty: false` (sitting 9 saved cleanly).  `PUT /api/connections/my-polarion` then `GET /api/connections` → `dirty: true` (writer-under-unique_lock sets the atomic; lock-free reader observes via the acquire-release ordering).  `POST /api/connections/save` then `GET /api/connections` → `dirty: false` (atomic store from `ClearDirty()` observed by lock-free reader).  Confirms the producer-under-lock / consumer-lock-free pairing works correctly.
- **Not directly verified:**  the actual race condition itself.  Pre-fix the race could (in principle) cause a missed dirty signal, but reproducing it requires hammering writes from one thread while reads from another — same fixture-dependent shape as the GetConnection finding.  Fix is structural; the verification posture is "the bug class is gone".

**Ramifications:**
- Callers touched: zero — `IsDirty()` / `ClearDirty()` callers in `webServer.cpp` (`HandleConnectionsSavePost` calls `ClearDirty()` after a successful save; `HandleConnectionsGetGet` reads `IsDirty()` for the `"dirty":bool` response field) work unchanged.
- Tests touched: live dirty-flag round-trip above.
- Docs touched: in-header comment.  No external doc needed.
- Blast radius: the relaxed-ordering load might be ~1 ns slower than the pre-fix non-atomic load on x86-64 (where atomic load with acquire ordering is already a plain MOV).  Negligible.

---

## Skipped findings table — Sitting 10

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| Connection name length / charset validation | `cloudConnectionManager.cpp` (`AddConnection`, `ParseConnectionsJson`) | MEDIUM cyber | Out of cluster 9C scope (input-validation, not concurrency / lifetime).  `kMaxFieldBytes=4096` from sitting 9 incidentally bounds name length at the parser entry, so log-injection-via-newlines is partially mitigated (newlines escape to `\n` literal via the JsonHelper convergence); the audit's full fix (charset whitelist) is still pending.  Bundle with endpoint SSRF below in a future "input validation pass on the cloud surface" sitting. |
| Endpoint URL stored without SSRF validation | `cloudConnectionManager.cpp` | MEDIUM cyber | Out of cluster 9C scope.  Bundle with name validation above.  Defense-in-depth note: the actual SSRF attack surface lives in the per-connector files that issue HTTP requests using the endpoint; centralizing validation at the manager is cheaper, but the connectors are the source of truth. |
| `AddConnection` redundantly copies `m_Name` before the move | `cloudConnectionManager.cpp` | LOW safety | Cosmetic.  No correctness impact.  Defer to the eventual "Rust-emulating C++ defaults sweep" sitting (per `feedback_rust_emulating_defaults`). |
| `[[nodiscard]]` on `bool`-returning mutators (`AddConnection`, `UpdateConnection`, `RemoveConnection`, `ParseConnectionsJson`) | `cloudConnectionManager.h` | LOW safety | `[[nodiscard]]` sweep.  Defer to the Rust-emulating C++ defaults sweep — apply uniformly across the codebase rather than file-by-file. |
| Stress fixture for the original `GetConnection` UAF reproducer | `cloudConnectionManager.cpp` + caller | (verification gap) | Fix is structural ("no raw pointer escapes the lock guard"); a fixture (1000 concurrent `GetConnection` + `RemoveConnection` cycles) would harden confidence — track for the eventual cybersec test fixture sitting. |
| Stress fixture for `m_Dirty` race observation | `cloudConnectionManager.cpp` + caller | (verification gap) | Same shape — fix is structural; a fixture (1000 concurrent writer + IsDirty cycles, looking for a missed dirty signal) would harden confidence.  Defer with the cybersec fixture sitting. |
| `engine.cpp:225` `fs::exists()` precheck on `connections.json` (TOCTOU) | `engine.cpp` | (cybersec finding, surfaced sitting 9) | Same pattern sitting 8 cleaned up in webServer.cpp.  One-line fix (drop the precheck, gate on `!sizeEc && fileSize <= cap` per the new `file_size`-with-`error_code` overload).  Out of cluster 9C scope; track for whichever sitting next touches engine.cpp. |
| `triggerEngine.cpp:712` email_watch path live verification | `triggerEngine.cpp` | (verification gap) | Caller-audit confirms the change is correct + identical to the verified `cloudTaskExecutor` site.  Fixture-dependent verification (set up an email_watch trigger + wait for poll interval) is overkill; the build + the in-line shape are sufficient. |
| `filterEngine.cpp:471` Polarion filter path live verification | `filterEngine.cpp` | (verification gap) | Same posture as the trigger engine entry — caller-audit + build-clean is sufficient.  Live verification requires a working Polarion endpoint, which the test environment doesn't have (DNS failure observed on `POST /api/connections/my-polarion/test`). |

---

## Sitting 11 — emailCloudTaskExecutor.cpp Cluster 11A: path traversal + protocol injection

**Scope locked:** the four densest findings on `application/cloud/emailCloudTaskExecutor.cpp` — three CRITICAL "untrusted input pasted into URL/filesystem path" plus one HIGH "untrusted input pasted into RFC 2822 header field":  CRIT path traversal via `body_file` parameter (the executor opens `std::ifstream(bodyFile)` with no canonicalisation; an attacker-controlled JCWF can supply `body_file: "../../../../etc/passwd"` and read arbitrary files into the email body); CRIT path traversal via attachment array (each `attachPath` is joined with `workDir` but never confined under it; absolute paths and `..` segments escape); CRIT IMAP folder URL injection (`folder` param interpolates directly into the IMAP URL — `INBOX@evil.internal:1234/extra` redirects the connection to an attacker-controlled host); HIGH SMTP header injection (`from`/`to`/`cc`/`subject` concatenate into MIME headers without CR/LF stripping — `subject: "Hello\r\nBcc: victim@..."` injects an arbitrary BCC).  Boundary at sitting-end: every untrusted input that flowed unchecked into a URL, filesystem path, or header field on this executor is now gated by an explicit allowlist or path-confinement check; every reject path emits an ERROR-level log with task / workflow / run identifiers (dashboard run-analyzer compatible) and a matching SECURITY_WARN line.  Cluster 11B (TLS hardening + attachment size cap + credential-redaction in error logs, 4 HIGH) and `emailConnector.cpp` cluster (2 CRIT + 5 HIGH; some findings — IMAP folder URL injection on the connector side — overlap with cluster 11A's executor-side fix) remain queued for sittings 12–13.

### `body_file` path traversal — confine under launch CWD via `ValidateLocalPath`

**Finding (CRITICAL cyber):**  In `EmailCloudTaskExecutor::ExecuteCloud`, `body_file` was extracted from the task params JSON via `getStringParam("body_file")` and opened directly via `std::ifstream(bodyFile)`.  No path canonicalisation, no containment check, no length bound.  An attacker who controls the JCWF (or compromises any caller of the workflow API) can supply `body_file: "../../../../etc/passwd"` (or any absolute path) and the file's contents become the email body, exfiltrated to the recipient address the task names.

**Verification:** Holds up.  Code at `emailCloudTaskExecutor.cpp:488–499` (pre-fix) is the bare `std::ifstream(bodyFile)` shape, exactly as audited.

**Caller-side audit:** `body_file` semantics in this module are **CWD-relative**, not workDir-relative.  The canonical `emailDemo.jcwf` ships with `body_file: "queue/emailDemo/02_ai_reply/PROB_reply.output.txt"` — a path relative to j9t's launch CWD that traverses through the `queue/` runtime directory to reach an upstream task's output.  Confining under workDir would break the demo and contradict the established convention; confining under **launch CWD** (which spans `queue/`, `workflows/`, `log/`, etc.) preserves the convention while closing the traversal vector.

**Change:** Before opening, call `ICloudTaskExecutor::ValidateLocalPath(bodyFile, Core::g_Core->GetLaunchCWDAbsolute(), taskDefinition.m_Id)`.  The helper rejects any input containing `..` substrings (catches the canonical `../etc/passwd` traversal) and any path whose `lexically_normal` form does not start with the canonical launch CWD (catches absolute-path escape, `/` operator quirk where `cwd / "/etc/shadow"` resolves to `/etc/shadow`, etc.).  On reject: set `m_LastErrorMessage`, transition to `Failed`, emit a fail-task log line that includes task / workflow / run identifiers as literal substrings (per `feedback_log_failures` — dashboard run-analyzer scopes ERRORs to lines containing the run id), and return false.  In-code comment at the call site documents the CWD-relative semantics + the existing demo's path shape so a future maintainer doesn't try to migrate to workDir-relative without also updating the demos.

**Verified at runtime:**
- Studio debug build clean.
- 28-test assistant non-AI suite + hermetic dispatcher: PASS.
- **Live happy-path:** `mcp__j9t__run_workflow emailDemo` → 3 tasks succeeded in 3 s, with the canonical `body_file: "queue/emailDemo/02_ai_reply/PROB_reply.output.txt"` accepted by the new gate.
- **Live negative-path (the actual fix verification):** swapped `body_file` to `"../../../../etc/passwd"` in the running emailDemo, ran via MCP, observed the rejection trail land exactly as designed:
  - `[Security] [info] [security] path_traversal_blocked: task='send_reply' local_path='../../../../etc/passwd' contains '..'`
  - `[Application] [error] [email_send] task='send_reply' workflow='emailDemo' run='emailDemo_1777691687': body_file path rejected`
  - `[Application] [error] [workflow] task 'send_reply' failed in run 'emailDemo_1777691687': email_send: body_file path is invalid or escapes the launch directory`
  - Run state `failed`; the canonical fail-path log includes the run id substring so the dashboard analyzer surfaces it as an issue.
- emailDemo restored byte-identical to backup post-test.

**Ramifications:**
- Callers touched: zero — `body_file` semantics preserved (CWD-relative); only the validation gate is new.
- Tests touched: emailDemo passes both happy path (canonical body_file) and negative path (rejected body_file).
- Docs touched: none.  `feedback_log_failures` already documents the ERROR-level + run-id-substring discipline; this fix instantiates the pattern.
- Blast radius: a body_file value containing `..` (even if it lexically stays under launch CWD — e.g. `queue/foo/../bar/baz.txt`) is now rejected.  No realistic JCWF needs this — the canonical pattern is direct paths through the queue/ tree.  If a future workflow legitimately needs `..` (very unlikely), the fix is to pre-resolve the path in the JCWF rather than relax the gate.

---

### Attachment path traversal — `ValidateLocalPath` per attachment under workDir

**Finding (CRITICAL cyber):**  In the attachment-loading loop, `attachPath` (a string from the JCWF params `attachments` array) was joined with `workDir` via `std::filesystem::path fullPath = workDir / std::string(attachPath)` and read directly.  No containment check.  C++'s `path::operator/` returns the right-hand side unchanged when it's absolute — so `attachPath: "/etc/shadow"` produces `fullPath = "/etc/shadow"` regardless of `workDir`.  And relative `..` segments traverse outside `workDir` syntactically.  The file's contents are base64-encoded into a MIME attachment and exfiltrated to the email recipient.

**Verification:** Holds up.  Code at `emailCloudTaskExecutor.cpp:526–547` (pre-fix) was the bare `workDir / attachPath` join, exactly as audited.

**Change:** Inside the per-attachment loop, before opening the file: convert `attachPath` (a `std::string_view` from simdjson) to `std::string` and call `ValidateLocalPath(attachPathStr, workDir, taskDefinition.m_Id)`.  On reject: emit a WARN-level fail-path log line (task fail-fast would prevent legitimate attachments later in the array from being read, so per the audit's recommendation we skip-with-warning at attachment granularity rather than abort the whole task), and `continue`.  In-code comment cites the `..`-and-absolute-path threat model that motivates the gate.

**Verified at runtime:**
- Build clean.
- **Live negative-path:** swapped `attachments` to `["../../../../etc/passwd", "/etc/shadow"]` in emailDemo, ran via MCP, observed both rejections:
  - `[security] path_traversal_blocked: task='send_reply' local_path='../../../../etc/passwd' contains '..'` (the `..` substring check fires first)
  - `[Application] [warning] [email_send] task='send_reply' workflow='emailDemo' run='emailDemo_1777691801': attachment path rejected`
  - `[security] path_traversal_blocked: task='send_reply' resolved='/etc/shadow' escapes base='/home/beaumanvienna/dev/jarvisAgent/workflows/emailDemo/03_reply'` (the absolute-path → escapes-base check fires for `/etc/shadow`)
  - Same WARN line again for the second rejection.
  - Run state: `succeeded` — task continued cleanly without the attachments (since the body itself is valid).  Per the audit-recommended skip-with-warning behaviour.

**Ramifications:**
- Callers touched: zero.
- Tests touched: emailDemo negative-path test confirms both `..` and absolute-path attacks are rejected with security-log breadcrumbs.
- Docs touched: none.
- Blast radius: a JCWF that expected to attach files via paths containing `..` (e.g. `attachments: ["../shared/logo.png"]`) would now skip those attachments with a WARN.  No production workflow does this — the convention is that attachments live inside the task's working directory, populated by upstream tasks.

---

### IMAP folder URL injection — strict allowlist + IMAP UID validation

**Finding (CRITICAL cyber):**  In `ExecuteEmailRead`, the `folder` parameter from the JCWF was interpolated directly into the IMAP URL (`searchUrl = imapBaseUrl + "/" + folder` and `fetchUrl = imapBaseUrl + "/" + folder + "/;UID=" + uid`).  No validation, no URL-encoding.  A value such as `INBOX@evil.internal:1234/extra` redirects the IMAP connection to an attacker-controlled host (the `@host:port` form is libcurl's standard URL syntax for overriding the connection target).  Values containing `\r\n` could inject IMAP protocol bytes.  The `uid` is server-supplied (from the SEARCH response) but interpolates into the FETCH URL too — still untrusted from this module's perspective, since a malicious or buggy IMAP server could return a hostile UID.

**Verification:** Holds up.  Code at `emailCloudTaskExecutor.cpp:325` and `:366` (pre-fix) was the bare string concatenation, exactly as audited.

**Change:** New file-local helpers `IsValidImapFolder(folder)` and `IsValidImapUid(uid)`.  Folder allowlist: `[A-Za-z0-9._/-]`, max 256 bytes, no leading or trailing `/`, no `//` (avoid ambiguous URL paths).  RFC 3501 hierarchy delimiters (`.` and `/`) are intentionally allowed — Gmail's `[Gmail]/Sent Mail` and similar legitimate hierarchies must still work.  UID allowlist: digits only, max 20 bytes (10^20 > UINT64_MAX, so any longer value is meaningless).  Validate folder once after the `INBOX` default; reject with task-level Failed + ERROR + SECURITY_WARN if invalid.  Validate UID per-iteration in the fetch loop; on invalid: skip-with-WARN (continue with remaining UIDs — a single bogus UID shouldn't fail the whole task).  In-code comments cite the URL-redirection threat model and explain why server-supplied UIDs are still treated as untrusted.

**Verified at runtime:**
- Build clean.
- **Live happy-path:** emailDemo's canonical `folder: "INBOX"` accepted; the demo runs end-to-end identically.
- **Live negative-path:** swapped `folder` to `"INBOX@evil.internal:1234/extra"`, ran emailDemo, observed:
  - `[Application] [error] [email_read] task='fetch_email' workflow='emailDemo': invalid folder name rejected (length=30)`
  - `[Security] [warning] [security] email_read_invalid_folder task='fetch_email' workflow='emailDemo' folder_length=30`
  - Task `fetch_email` state: `failed`; downstream tasks (`ai_reply`, `send_reply`) skipped per existing dependency policy.  Run state: `failed`.
- **Not directly verified:**  the per-iteration UID validation under a hostile IMAP server response.  GreenMail (the mock IMAP server) doesn't produce hostile UIDs in practice — the validation is structural (the allowlist guarantees the URL stays well-formed), and the LOG_APP_WARN path is the same shape as the existing `[email_read] failed to fetch UID` warn that already fires for legitimate IMAP errors.  A fixture that injects malformed UIDs server-side would harden confidence — track for the cybersec fixture sitting.

**Ramifications:**
- Callers touched: zero — folder/UID validation is internal to `ExecuteEmailRead`.
- Tests touched: emailDemo negative-path confirms folder rejection.  Happy-path unchanged.
- Docs touched: none.  The folder allowlist matches the IMAP RFC 3501 mailbox-name conventions; existing demo workflows (just `INBOX`) and any realistic non-default folder name (`Archive`, `Sent`, `[Gmail]/Sent Mail`, `Projects.j9t`) all pass.
- Blast radius: legitimate IMAP folder names with non-allowlist characters (e.g. spaces — `"Sent Items"`) would be rejected.  The existing `EmailConnector::BuildImapUrl` would also have struggled with such names (no URL-encoding), so no production setup actually relies on them.  If a deployment surfaces such a need, the right fix is to extend the allowlist (and add `curl_easy_escape` for the URL-encoding) rather than weaken it.

---

### SMTP header injection — CRLF rejection on every header field value

**Finding (HIGH safety):**  `BuildEmailMessage` concatenates `from`, `to`, `cc`, and `subject` into RFC 2822 headers via `msg << "From: " << from << "\r\n"` etc.  No CR/LF stripping at any point.  An attacker supplying `subject: "Hello\r\nBcc: victim@evil.com"` writes `Subject: Hello\r\nBcc: victim@evil.com\r\n` into the message — and the SMTP server interprets the embedded `\r\n` as a header terminator, accepting the injected `Bcc:` line as a real header.  Same vector for `from` (forging sender), `to` (BCC injection), `cc` (BCC injection).

**Verification:** Holds up.  `BuildEmailMessage` at `emailCloudTaskExecutor.cpp:102–158` (pre-fix) is the raw `<<` concatenation, exactly as audited.

**Change:** New file-local helper `ContainsCrlf(s)`.  At `ExecuteCloud`'s entry — after extracting `from`, `to`, `cc`, `subject` but BEFORE calling `BuildEmailMessage` — validate all four with `ContainsCrlf`.  On any positive match: set `m_LastErrorMessage`, transition to `Failed`, emit ERROR + SECURITY_WARN with task / workflow / run identifiers, return false.  Body content (which is the actual message body, not a header) is intentionally **not** validated — newlines are legitimate body content.  In-code comment at the validation site cites the BCC-injection threat model.  `BuildEmailMessage` itself is unchanged — defense lives at the entry gate, where the request can be rejected cleanly without any partial-build artefacts.

**Verified at runtime:**
- Build clean.
- **Live negative-path:** swapped `subject` to `"Re: Hello\r\nBcc: victim@example.com"`, ran emailDemo, observed:
  - `[Application] [error] [email_send] task='send_reply' workflow='emailDemo' run='emailDemo_1777691782': CRLF rejected in header field`
  - `[Security] [warning] [security] email_send_header_injection task='send_reply' workflow='emailDemo' run='emailDemo_1777691782'`
  - `[Application] [error] [workflow] task 'send_reply' failed in run 'emailDemo_1777691782': email_send: header field value contains CR/LF (from/to/cc/subject must not contain newlines)`
  - Run state: `failed`.
- **Live happy-path:** standard emailDemo subject `"Re: Weekly Report Request"` (no CR/LF) accepted; demo runs unchanged.

**Ramifications:**
- Callers touched: zero.
- Tests touched: emailDemo negative-path confirms CRLF rejection on subject.  Same gate covers `from` / `to` / `cc` by symmetry — no separate test needed because the validation is one-line-per-field with identical shape.
- Docs touched: none.  RFC 2822 §2.2.3 explicitly forbids CR/LF in unfolded header values; rejecting them is enforcing the spec.
- Blast radius: a JCWF that legitimately needed multi-line `subject`/`cc` (no real protocol does — `cc` uses commas, `subject` uses RFC 2047 encoded-word for non-ASCII) would fail.  No production workflow does this.

---

## Skipped findings table — Sitting 11

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| No TLS enforcement for SMTP on non-587 ports | `emailCloudTaskExecutor.cpp` | HIGH cyber | Cluster 11B (TLS hardening).  Fix shape: drop the conditional `if (port == "587")` guard around `CURLOPT_USE_SSL` — set it unconditionally, or explicitly support 465 (`smtps://`) + 587 (STARTTLS) with no plain-text fallback. |
| No TLS certificate verification guaranteed for SMTP | `emailCloudTaskExecutor.cpp` | HIGH cyber | Cluster 11B.  Fix: unconditionally `CURLOPT_SSL_VERIFYPEER=1L`, `CURLOPT_SSL_VERIFYHOST=2L`; abort on absent CA bundle rather than fall through. |
| No TLS certificate verification for IMAP | `emailCloudTaskExecutor.cpp` (delegating to `EmailConnector::ImapCommand`) | HIGH cyber | Cluster 11B + sitting 13.  Fix lives in `emailConnector.cpp` (the `ImapCommand` body) — both files need to coordinate. |
| Uncontrolled file read size for attachments (DoS / OOM) | `emailCloudTaskExecutor.cpp` | HIGH safety | Cluster 11B (size cap).  Fix: enforce a 25 MB-or-similar cap on `fileSize` before allocating the `std::string content(static_cast<size_t>(fileSize), '\0')`.  Bundle with cluster 11A's attachment confinement work was tempting, but size cap is its own mechanic and groups more naturally with TLS / redaction (the "resource and information control" sweep). |
| Credentials logged in error path | `emailCloudTaskExecutor.cpp` | HIGH cyber | Cluster 11B (redaction).  Fix: replace `imapError` and `curl_easy_strerror(res)` strings with `CURLOPT_ERRORBUFFER`-driven errors that the executor can scrub before storing in `m_LastErrorMessage`. |
| Secrets leaked in success-path log message (recipient + subject) | `emailCloudTaskExecutor.cpp` | MEDIUM cyber | Cluster 11B if grouping with redaction; could also bundle with the assistant-subsystem's secret-logging discipline pass (per `feedback_secrets_only_via_redactor`).  Defer. |
| Unvalidated `max_messages` enables DoS | `emailCloudTaskExecutor.cpp` | MEDIUM cyber | Cluster 11B (resource control).  Fix: `std::clamp(static_cast<int>(val), 1, 500)` on parse. |
| Predictable MIME boundary value (potential injection) | `emailCloudTaskExecutor.cpp` | MEDIUM cyber | Cluster 11C or its own slot — fix needs OpenSSL `RAND_bytes` for the boundary, plus body-content scan for boundary collision and regenerate-if-collide.  Same family as the body-not-validated-for-boundary concern; bundle naturally. |
| `summary` JSON in `ExecuteCloud` not escaped | `emailCloudTaskExecutor.cpp` | MEDIUM safety | The summary builds `"to":"" + to + ""` etc. without `JsonEscapeEmail`.  Cluster 11A's CRLF gate already prevents the worst payload (newlines), but `"` and `\` still slip through.  Cluster 11B / cluster 11C work — small fix that bundles with the JsonHelper convergence pass on this file. |
| `getStringParam` lambda captures `doc` by reference across array iteration | `emailCloudTaskExecutor.cpp` | LOW safety | Existing reads are scalars-before-arrays, so the on-demand position-dependence concern is dormant.  If a future change interleaves reads, the audit's recommendation (extract all scalars before any array iteration) becomes load-bearing.  Not a current bug. |
| `EmailConnector::ImapCommand` TLS verification gaps + SSRF + IMAP injection on the connector layer | `emailConnector.cpp` | 2 CRIT + 5 HIGH | Sitting 13 (separate file, separate sitting).  One finding (IMAP folder URL injection) overlaps cluster 11A's executor-side fix — the connector-side fix is still needed because the connector is the source of truth for URL composition. |
| File-local `JsonEscapeEmail` is yet another anon-namespace JSON-escape copy | `emailCloudTaskExecutor.cpp` | (convergence) | Tracked as the next iteration of the post-sitting-4 + sitting-9 JsonEscape convergence sweep.  This file was the 7th unconverged copy at sitting-9 review time.  Bundle with cluster 11B or do as a standalone mini-sweep at the start of cluster 11B. |
| Stress fixture for hostile IMAP server responses (UID validation reproducer) | `emailCloudTaskExecutor.cpp` | (verification gap) | Defer to the cybersec fixture sitting. |

---

## Sitting 12 — emailCloudTaskExecutor.cpp Cluster 11B: TLS hardening + size cap + JsonHelper convergence

**Scope locked:** the resource / transport-security cluster on `application/cloud/emailCloudTaskExecutor.cpp` — three HIGH (SMTP TLS unconditional, SMTP cert verify unconditional, attachment file-size cap) plus two MEDIUM bundle-friendly fixes (`summary` JSON escape gap, `max_messages` overflow clamp) plus the **7th anon-namespace `JsonEscape` copy** in the codebase converged onto `JsonHelper::EscapeJsonString` (continuing the post-sitting-4 + sitting-9 sweep).  Boundary at sitting-end: SMTP transport defaults to **TLS-required with full cert + hostname verification**; the only way to send plaintext SMTP is to set `use_ssl: "false"` on the connection, which now emits `[security] email_send_tls_disabled` on every send so an operator running insecurely sees the deviation in the security log.  Attachments are bounded at **25 MB** per file (skip-with-WARN on overflow).  `max_messages` is clamped to **[1, 500]** so an attacker-supplied or typo'd value cannot spiral memory.  The `summary` JSON in `ExecuteCloud` no longer corrupts on hostile `to` / `subject` content (`"`, `\\` now escape correctly).  No file-local `JsonEscape*` copies remain in `application/cloud/`.

**One audit finding deferred:** the IMAP TLS verification HIGH (`emailCloudTaskExecutor.cpp:369` delegates to `EmailConnector::ImapCommand` — the actual fix lives on the connector layer).  Bundle with sitting 13's `emailConnector.cpp` pass.

### SMTP TLS unconditional + cert verify unconditional — `use_ssl`-respecting strict TLS

**Finding (HIGH cyber × 2):**  Pre-fix curl setup at lines 711–717 enabled `CURLOPT_USE_SSL = CURLUSESSL_ALL` only when `port == "587"`.  Port 465 (implicit TLS), port 25 (plain SMTP), and any non-standard port silently fell back to plaintext.  Separately, `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST` were never explicitly set — relying on libcurl's defaults, which can fail-open on builds where the trust store is empty or unreachable.  Both findings fire on the same code path: the SMTP send, which carries credentials in the AUTH handshake.

**Verification:** Both holds.  Code at lines 711–717 (pre-fix) is the bare `if (port == "587")` guard, and no `CURLOPT_SSL_VERIFY*` calls anywhere in this function.

**Design tension surfaced during the fix:**  The audit's recommendation "set `CURLUSESSL_ALL` unconditionally" would break the canonical `emailDemo` workflow.  The bundled GreenMail Docker mock listens on plaintext port 3025 (no STARTTLS support) and the `my-greenmail` connection ships with `use_ssl: "false"` and `smtp_port: "3025"`.  Forcing TLS unconditionally would error every demo run.  **Resolution:** respect the existing `use_ssl` connection param (which the IMAP path already honours at line 322) and make TLS the default — so production deployments without an explicit opt-out get strict TLS, but local-testing setups with `use_ssl: "false"` retain plaintext support **and** emit a `[security] email_send_tls_disabled` log line on every send so an operator can audit.

**Change:**
1. Read `connection.m_Params["use_ssl"]` once into a local `bool smtpUseTls = (sslIt == end || sslIt->second != "false")`.
2. If `smtpUseTls`: set `CURLOPT_USE_SSL = CURLUSESSL_ALL` (refuse to proceed without TLS — closes the silent-plaintext-fallback gap), `CURLOPT_SSL_VERIFYPEER = 1L`, `CURLOPT_SSL_VERIFYHOST = 2L`.  In-code comment cites the MITM-stripped-STARTTLS attack `CURLUSESSL_TRY` would have permitted.
3. Else: emit `LOG_SECURITY_WARN("[security] email_send_tls_disabled task='{}' workflow='{}' run='{}' connection='{}'", ...)` so the deviation is observable in `log/security.txt`.
4. Drop the old `if (port == "587")` block entirely — it conflated TLS-mode-by-port with TLS-required-by-policy.  libcurl's URL scheme detection (`smtps://` for 465, `smtp://` for 587) correctly drives the implicit-TLS-vs-STARTTLS choice; CURLUSESSL_ALL is the policy gate.

**Verified at runtime:**
- 28-test suite + hermetic dispatcher: PASS.
- **Live happy-path (use_ssl=false branch):** `emailDemo` ran end-to-end as before; security log contains `[security] email_send_tls_disabled task='send_reply' workflow='emailDemo' run='emailDemo_1777692618' connection='my-greenmail'` exactly once per send.  GreenMail's plaintext SMTP path still works; the operator now has a clear signal that this connection is insecure.
- **Not directly verified:**  the use_ssl=true branch (no production-grade SMTP+TLS server in the test environment).  The fix is structural — every curl option is set unconditionally on entry to the TLS branch; the verification posture is "the flags are now set per the audit's prescription".  A future fixture using a Postfix container with TLS would harden confidence, but the production deployment path will be exercised by JC's first real SMTP integration.

**Ramifications:**
- Callers touched: zero — `use_ssl` was already documented (cloud-integration.md line 778) and respected by the IMAP path.  Now it gates the SMTP path identically.
- Tests touched: emailDemo unchanged (still ships with `use_ssl: "false"`).
- Docs touched: none.  cloud-integration.md's `use_ssl` documentation now accurately covers the SMTP path that previously ignored it.
- Blast radius: a deployment that sent SMTP with `use_ssl: "true"` (or omitted) and a CA bundle that libcurl can't reach would now fail the TLS handshake instead of silently falling through.  This is the correct behaviour — the silent fall-through was the bug.  An operator who hits the strict gate can either fix their CA bundle or, knowingly, opt in to plaintext via `use_ssl: "false"` (with the security log warning that step produces).

---

### Attachment file-size cap — 25 MB skip-with-WARN

**Finding (HIGH safety):**  Pre-fix attachment loop at lines 670–675 read `auto fileSize = file.tellg();` then allocated `std::string content(static_cast<size_t>(fileSize), '\0')` with no upper bound.  An attacker who can place a large file in the task's working directory (or a buggy upstream that materialises one) causes the executor to allocate that many bytes plus a 1.33× base64-encoded copy in the MIME body — a multi-GB attachment OOMs the process.  The pre-fix code also didn't check `fileSize >= 0` before the cast; `tellg()` returns `pos_type` and can be `-1` on error (which would `static_cast<size_t>` to a wraparound value).

**Verification:** Holds.  Code at lines 670–675 (pre-fix) was the bare `tellg → string allocate → read` shape.

**Change:** Compute `fileSize` once, validate `fileSize >= 0 && fileSize <= kMaxAttachmentBytes` where `kMaxAttachmentBytes = 25 * 1024 * 1024` (`std::streamoff` to match `tellg`'s return type).  On overflow or negative: emit `LOG_APP_WARN` with task / workflow / run / attachPathStr / fileSize / cap, and `continue`.  Cap rationale documented inline: matches typical SMTP server limits (Gmail / Outlook / most providers) with headroom for the base64-encoded MIME message.  Skip-with-WARN matches the existing path-traversal-rejection behaviour from sitting 11 (single bad attachment doesn't fail the whole task).

**Verified at runtime:**
- **Live negative-path:** generated `workflows/emailDemo/03_reply/big_blob.bin` at 30 MB via `dd if=/dev/zero count=30 bs=1M`, set `attachments: ["big_blob.bin"]`, ran emailDemo.  Log shows: `[Application] [warning] [email_send] task='send_reply' workflow='emailDemo' run='emailDemo_1777692645': attachment 'big_blob.bin' size 31457280 bytes exceeds 26214400 byte cap; skipping`.  Run state: `succeeded` — task continued cleanly without the attachment, email sent.  Big_blob.bin removed post-test; emailDemo restored byte-identical.

**Ramifications:**
- Callers touched: zero.
- Tests touched: emailDemo negative-path verifies the skip-with-WARN behaviour.
- Docs touched: none.  Email size limits are SMTP-server-specific; documenting a hard 25 MB cap in the cloud-integration doc would conflict with operators who tune their server differently — leave the cap as a defense-in-depth implementation detail.
- Blast radius: a workflow that legitimately needs to attach files larger than 25 MB would now silently skip them.  No realistic email workflow attaches files over that size (most SMTP providers reject them anyway).  If a deployment surfaces the need, the cap can be raised — single constant change.

---

### `JsonEscapeEmail` → `JsonHelper::EscapeJsonString` convergence (7th copy in the codebase)

**Finding (carry-over from convergence sweep + MEDIUM RFC):**  `emailCloudTaskExecutor.cpp` carried a 30-line `static std::string JsonEscapeEmail(std::string const& input)` at lines 240–268.  Like the cloudConnectionManager copy that sitting 9 closed, this was the **7th unconverged anon-namespace JSON-escape copy** in the codebase after sittings 4–5's assistant-subsystem sweep + sitting 9's cloudConnectionManager close.  Unlike sitting 9's pre-fix cloudConnectionManager copy, this one already covered RFC 8259 §7 control chars (the `default:` arm emits `\\u%04x` for bytes < 0x20), so the convergence is purely a maintenance / consistency win — no escape-coverage gap to close.

**Verification:** Holds — local `JsonEscapeEmail` definition at lines 240–268 plus 7 call sites (6 in `ExecuteEmailRead`'s summary-JSON build, 1 in the response JSON for the `folder` field).

**Change:** Add `#include "json/jsonHelper.h"`.  Delete the local `JsonEscapeEmail` function definition.  Replace all 7 call sites with `JsonHelper::EscapeJsonString(...)` via `sed -i 's/JsonEscapeEmail(/JsonHelper::EscapeJsonString(/g'`.  Verify zero remaining `JsonEscapeEmail` references in the file post-conversion.

**Verified at runtime:**  Build clean.  emailDemo happy-path PASS — the email_read summary JSON build (which exercises 6 of the 7 converted call sites) round-trips cleanly through `JsonHelper::EscapeJsonString`.

**Ramifications:**
- Callers touched: zero external.  The function was file-local with no header surface.
- Tests touched: emailDemo's `emails_summary.json` write path now routes through `JsonHelper`.
- Docs touched: none.  `feedback_simdjson_only` already names `JsonHelper` as the canonical escape helper.
- Blast radius: zero behaviour change.  Both implementations were RFC 8259 §7-compliant; the convergence is consolidation only.

---

### Summary JSON escape gap + `max_messages` overflow clamp (two MEDIUM bundle-friendly fixes)

**Finding (MEDIUM safety + MEDIUM cyber):**  Two small fixes that bundle naturally with cluster 11B.
1. **Summary JSON escape** — `ExecuteCloud`'s summary build at line 772 (post-renumbering) did `"to":"" + to + ""` and `"subject":"" + subject + ""` with no escape.  Sitting 11's CRLF gate already prevents the worst payload (newlines), but `"` and `\\` still slip through and would corrupt the `response.json` write or smuggle additional JSON fields.
2. **`max_messages` clamp** — at line 363, `maxMessages = static_cast<int>(val)` cast a uint64_t with no upper bound to int.  Values > INT_MAX wrap to negative (the audit's primary concern); values 1..INT_MAX allow attacker-supplied DoS via "fetch 2 billion emails".

**Verification:** Both findings hold against current code.

**Change:**
1. Apply `JsonHelper::EscapeJsonString(to)` and `JsonHelper::EscapeJsonString(subject)` in the summary string concatenation.  In-code comment cites the `"` / `\\` slip-through that the CRLF gate didn't cover.
2. Define `static constexpr int kMaxMessageCap = 500`.  Pre-clamp the uint64_t to the cap (so the int cast is safe), then `std::clamp(static_cast<int>(val), 1, kMaxMessageCap)` to enforce the lower bound.  In-code comment explains the prior overflow-to-negative bug and the DoS concern.

**Verified at runtime:**
- **Summary JSON escape live:** set `subject: "Re: \\"hostile\\" subject with \\\\backslash"` (`"` and `\\` literals after JSON un-escape), ran emailDemo.  `response.json` content: `{"ok":true,"to":"sender@example.com","subject":"Re: \\"hostile\\" subject with \\\\backslash","attachments":0}`.  Python `json.load` parses cleanly; subject decodes to the literal expected string.  Pre-fix the same payload would have produced an unparseable JSON file.
- **`max_messages` clamp live:** set `max_messages: 99999`, ran emailDemo.  `fetch_email` task succeeded; no overflow, no excessive memory, run state `succeeded`.  The clamp is structural (a single-line `std::clamp` call); the verification posture is "the value passed without crashing and the int variable was finite-valued".

**Ramifications:**
- Callers touched: zero.
- Tests touched: emailDemo negative-paths above.
- Docs touched: none.  cloud-integration.md line 810 documents `max_messages | no | 10 | Maximum messages to fetch` — the new 500 ceiling is a defensive cap, not a contract change (no realistic JCWF requests > 500).
- Blast radius: a workflow that requested `max_messages > 500` would now get exactly 500 messages instead of the larger number.  No realistic JCWF needs this; the IMAP polling pattern is "fetch the most recent N" where N is small.

---

## Skipped findings table — Sitting 12

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| No TLS certificate verification for IMAP | `emailCloudTaskExecutor.cpp` (delegating to `EmailConnector::ImapCommand`) | HIGH cyber | The fix lives on the connector layer (`EmailConnector::ImapCommand`'s curl setup).  Bundle with sitting 13's `emailConnector.cpp` cluster.  This file's IMAP path is just a thin wrapper that calls into the connector. |
| Credentials logged in error path | `emailCloudTaskExecutor.cpp` | HIGH cyber | The credential-leak vector is the IMAP URL `user:password@host` form embedded into curl error strings.  The URL is built in `EmailConnector::BuildImapUrl` — fixing it requires sanitising at the connector layer (sitting 13) and registering credentials with `SecretRedactor` on resolution.  This file's `taskState.m_LastErrorMessage = "IMAP SEARCH failed: " + imapError` line cannot redact what the connector hands it; the right fix is at the source. |
| Secrets leaked in success-path log message (recipient + subject) | `emailCloudTaskExecutor.cpp` | MEDIUM cyber | Recipient + subject metadata leak via `LOG_APP_INFO("[email] sent to {} via connection '{}' (subject: {})")`.  Bundle with sitting 13 or its own MEDIUM mini-sweep — the fix needs a config flag (log-metadata: on/off) and downgrade to DEBUG.  Out of cluster 11B's resource-and-transport scope. |
| Predictable MIME boundary value (potential injection) | `emailCloudTaskExecutor.cpp` | MEDIUM cyber | The boundary uses `system_clock::now().time_since_epoch().count()` which is predictable.  Fix needs OpenSSL `RAND_bytes` for the boundary plus body-content scan for boundary collision and regenerate-if-collide.  Bundle with the assistant-subsystem's CSPRNG patterns or its own slot — out of cluster 11B. |
| `getStringParam` lambda captures `doc` by reference across array iteration | `emailCloudTaskExecutor.cpp` | LOW safety | Existing reads are scalars-before-arrays (preserved through cluster 11A + 11B's changes); the on-demand position-dependence concern is dormant.  Defer to the eventual Rust-emulating C++ defaults sweep. |
| `EmailConnector::ImapCommand` TLS verification gaps + SSRF + IMAP injection on the connector layer | `emailConnector.cpp` | 2 CRIT + 5 HIGH | Sitting 13.  Single dense file, ~1 sitting. |
| Stress fixture exercising the use_ssl=true SMTP branch | `emailCloudTaskExecutor.cpp` | (verification gap) | Requires a Postfix-with-TLS Docker container; defer to the cybersec fixture sitting. |
| Test coverage for the `kMaxAttachmentBytes` lower-bound (negative `tellg` defensive path) | `emailCloudTaskExecutor.cpp` | (verification gap) | Negative `tellg` only fires on stream errors mid-tellg, hard to engineer in a fixture; the fix is purely defensive. |

---

## Sitting 13 — emailConnector.cpp comprehensive: TLS hardening + SSRF + IMAP injection + DoS

**Scope locked:** every CRITICAL and HIGH finding on `application/cloud/emailConnector.cpp` plus two MEDIUMs that bundle naturally — the file's full audit cluster, end-to-end, in one sitting.  **Boundary at sitting-end: the email surface (`emailCloudTaskExecutor.cpp` sittings 11+12 + `emailConnector.cpp` sitting 13) is fully closed at the CRITICAL/HIGH level.**  Both halves of the email send/read path now refuse to proceed without TLS + full peer + hostname verification when `use_ssl=true`; loopback / link-local / private / cloud-metadata IP ranges are rejected as SMTP/IMAP targets in production posture; the IMAP `folder` and `subject_filter` strings cannot inject protocol bytes; the `std::stoull` calls in the polling loop cannot crash the engine; the IMAP response buffer is bounded at 10 MB.  In addition, sitting 11's executor-side `IsValidImapFolder` + `IsValidImapUid` helpers were lifted to `EmailConnector` public statics so connector and executor share a single source of truth (per `feedback_cpp_discipline` — refactor before the third copy of a validator emerges).

### TLS hardening — `ImapCommand` + `TestConnection` (2 CRIT)

**Finding (CRIT cyber × 2):**  Both `EmailConnector::ImapCommand` (the IMAP send-buffer for SEARCH/FETCH commands shared by `email_read` and `email_watch`) and `EmailConnector::TestConnection` (the SMTP connectivity test driven by the dashboard + REST `/test` endpoint) had the same omission: `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST` were never explicitly set.  `ImapCommand` set `CURLOPT_USE_SSL = CURLUSESSL_NONE` only on the `useSsl=false` branch — the `useSsl=true` path relied on libcurl's defaults, which can fail-open on builds where the trust store is empty or unreachable.  `TestConnection` had a port-conditional `if (port == "587") { CURLUSESSL_ALL }` that left ports 465 + non-standard silently in plaintext.

**Verification:** Holds.  Code at `emailConnector.cpp:171-217` (pre-fix `ImapCommand`) and `:100-158` (pre-fix `TestConnection`) was as audited.

**Change:**  Apply sitting 12's TLS pattern to both functions, with one twist for the connector layer: each function reads / receives `useSsl` (via parameter for `ImapCommand`, via `connection.m_Params["use_ssl"]` for `TestConnection`).  When `useSsl` is true: `CURLOPT_USE_SSL = CURLUSESSL_ALL` (refuse to proceed without TLS), `CURLOPT_SSL_VERIFYPEER = 1L`, `CURLOPT_SSL_VERIFYHOST = 2L`.  When false: explicitly select `CURLUSESSL_NONE` and emit `[security] email_test_tls_disabled` (for `TestConnection`) or `[security] email_imap_tls_disabled` (for `ImapCommand`).  In-code comment cites the build-defaults-fail-open concern that motivates the explicit settings.

The closure fully eliminates **the IMAP TLS verification HIGH carry-over from sittings 11+12** (which was deferred at the executor layer because the actual fix lives here).

**Verified at runtime:**
- 28-test suite + hermetic dispatcher PASS.
- **Live happy-path:** emailDemo succeeded with `[security] email_imap_tls_disabled url_scheme='imap:/'` per fetch — exactly the expected GreenMail-with-`use_ssl=false` signal.
- **Live REST `/test` happy-path:** `POST /api/connections/my-greenmail/test` → `{"ok":true}` (use_ssl=false → plaintext SMTP path through GreenMail's port 3025 worked).  Same call after restoring use_ssl=true with localhost → rejected by SSRF gate (see next entry); the TLS-on-localhost path can't be exercised against GreenMail because GreenMail doesn't speak TLS.
- **Not directly verified:** the `useSsl=true` strict-TLS branch.  No production-grade SMTP+TLS or IMAP+TLS server in the test environment; the fix is structural.

---

### SSRF host validation + port validation in `BuildSmtpUrl` + `BuildImapUrl`

**Finding (HIGH cyber + MEDIUM cyber):**  Both URL-building helpers concatenated `connection.m_Params["smtp_host"]` / `["imap_host"]` and `["smtp_port"]` / `["imap_port"]` directly into the libcurl URL.  Hostile values like `host = "169.254.169.254"` (cloud metadata IP), `host = "evil.com:465/path?x="` (URL-injection), or `port = "587 UID FETCH"` (protocol-bytes-in-port) sailed through.  An attacker who controls connection params can target internal services, embed extra URL components, or inject IMAP/SMTP protocol bytes via the URL.

**Verification:** Holds.  Code at `emailConnector.cpp:70-83` (`BuildSmtpUrl`) and `:85-98` (`BuildImapUrl`) was the bare concatenation, exactly as audited.

**Change:**  New `EmailConnector` public statics (header — shared with the executor): `IsValidEmailHost(host, allowLocalNetwork)` and `IsValidEmailPort(port)`.  Host validation rejects URL-meaningful chars (`:`, `/`, `?`, `#`, `@`, `%`, `\\`), whitespace, CR, LF; rejects empty / >253 byte; and when `allowLocalNetwork` is false, rejects loopback (`localhost`, `127.x`, `::1`), link-local (`169.254.x` — covers cloud metadata), private (`10.x`, `172.16-31.x`, `192.168.x`), and IPv6 unique-local (`fc00::/7`, `fe80::/10`).  Port validation: digits only, [1, 65535], max 5 bytes.

The `allowLocalNetwork` parameter is **gated on `use_ssl`** at the call site.  `BuildSmtpUrl` and `BuildImapUrl` read `use_ssl` and pass `allowLocal = !useSsl`.  Rationale: plaintext mode (`use_ssl=false`) is the local-testing escape hatch already (sitting 12), so accepting loopback hosts in that mode is consistent with the operator's already-explicit "I'm testing locally" signal.  Production deployments (`use_ssl=true`, the default) get strict no-loopback validation.

On rejection, both helpers return an empty string and emit `[security] email_invalid_smtp_target connection='{}' use_ssl={}` or the `imap` variant.  Callers (`TestConnection`, `CheckForNewMail`, `emailCloudTaskExecutor::ExecuteCloud`) check for empty URL and fail-the-task with a "see security log" error message.

**Verified at runtime:**
- **Live SSRF rejection:** mutated `my-greenmail` to `smtp_host: "169.254.169.254"` + `use_ssl: "true"` via REST PUT, then `POST /api/connections/my-greenmail/test` → HTTP 400 + `Email SMTP target rejected: invalid host or port (see security log)`.  Security log: `[security] email_invalid_smtp_target connection='my-greenmail' use_ssl=true`.
- **Live use_ssl-coupled gate:** mutated to `smtp_host: "localhost"` + `use_ssl: "true"` (legitimate hostname + production posture) → also rejected.  Same SECURITY_WARN.  Confirms the gate doesn't have a "plaintext-loophole" — even when an operator forgets to set non-loopback host, the production posture refuses to send.
- **Live use_ssl-coupled allow path:** restored to `smtp_host: "localhost"` + `use_ssl: "false"` (the canonical demo config) → `POST /test` returned `{"ok":true}`, GreenMail accepted the connection.  Confirms the local-testing escape hatch works.
- **Not directly verified:** the IPv4 `172.16-31.x` second-octet range (the `std::stoi` parse + bounds check is structural; bundled into the same code path as the verified `10.x` / `192.168.x` checks).  IPv6 unique-local detection.

---

### IMAP folder + `subject_filter` injection in `CheckForNewMail`

**Finding (HIGH cyber × 2):**  `CheckForNewMail` interpolated the `folder` parameter directly into the IMAP URL (`searchUrl = imapBaseUrl + "/" + folder;`) and the `subjectFilter` parameter directly into the SEARCH command (`searchCommand += " SUBJECT \"" + subjectFilter + "\"";`).  Folder injection vectors: `INBOX\r\nA001 FETCH 1:* (BODY[])` (CRLF protocol bytes), `../../etc/passwd` (path traversal in URL), `INBOX?param=` (URL component injection).  Subject filter injection: `foo" UNSEEN` (breaks out of the quoted string) or `foo\r\nA001 FETCH 1:* (BODY[])` (CRLF injects another IMAP command).

**Verification:** Holds.  Code at `emailConnector.cpp:267` and `:276` (pre-fix) was the bare concatenation.

**Change:**  Apply `EmailConnector::IsValidImapFolder` and a new `EmailConnector::IsValidImapSubjectFilter` static at the entry of `CheckForNewMail`.  `IsValidImapFolder` is the helper sitting 11 created (lifted to the connector header in this sitting).  `IsValidImapSubjectFilter` rejects bytes that would break the SEARCH SUBJECT quoted-string envelope (`"`, `\\`) or inject IMAP command bytes (`\r`, `\n`) or trigger IMAP literal syntax (`{`).  Empty filter is allowed (means "no filter").  Length cap at 256 bytes.

On reject: emit `[security] email_check_invalid_folder` or `[security] email_check_invalid_subject_filter` SECURITY_WARN and return an empty UID with `errorMessage` set.  This is **defense-in-depth** — sitting 11 already validates folder at the executor entry, but the connector's public API can be invoked from future call sites with no prior validation.

Bonus closure: also validates the `lastSeenUid` watermark with `IsValidImapUid` before any `std::stoull` call (closes the audit's specific concern that "lastSeenUid comes from an external watermark that is never sanitized").

**Verified at runtime:** `CheckForNewMail` is exercised by the `email_watch` trigger, which fires on a poll timer (default 300s).  emailDemo doesn't use `email_watch` — it's a manual one-shot trigger that uses `email_read`.  Direct fixture-level testing of `CheckForNewMail` would need either an `email_watch` JCWF + 5-minute wait, or a unit-test harness.  The fix is structural (the validators are simple allowlists; the failure path is an explicit early return); the verification posture is "the same allowlist that proved correct in sitting 11's executor-side gate now also runs at the connector layer".

---

### `std::stoull` DoS — try/catch + watermark sanitization

**Finding (HIGH cyber):**  `CheckForNewMail`'s polling loop had `if (std::stoull(uid) > std::stoull(lastSeenUid))` with no exception handling.  `ParseSearchUids` checked `std::isdigit` on the first byte only — values like `"1e5"` (parser accepts the `1`, `stoull` throws `invalid_argument` on the `e`) or `"99999999999999999999"` (overflow → `out_of_range`) would crash the polling thread.  `lastSeenUid` (the watermark) comes from external state and was never sanitized either.

**Verification:** Holds.  Code at `emailConnector.cpp:307` (pre-fix) was the bare `stoull` calls.

**Change:**  Sanitize `lastSeenUid` with `IsValidImapUid` at function entry (covered above as part of the `CheckForNewMail` hardening).  Wrap the polling-loop comparison in `try { ... } catch (std::invalid_argument) { ... } catch (std::out_of_range) { ... }`; on either exception, log `LOG_APP_WARN` with connection name, set `errorMessage`, and return `highestUid` (the safe "no new mail" path that preserves the watermark).  Per-element validity check (`IsValidImapUid(uid)`) inside the loop also skips malformed UIDs with a WARN — defense in depth even if `ParseSearchUids` stops checking the first byte.

**Verified at runtime:**  Same posture as the IMAP injection fixes — the structural fix is exercised by every `email_watch` poll (the try/catch is on the hot path), but reproducing the malformed-UID scenario requires either a hostile IMAP server or a unit test.  emailDemo's GreenMail produces well-formed UIDs.  Defer to the cybersec fixture sitting.

---

### IMAP response buffer cap (MEDIUM)

**Finding (MEDIUM cyber):**  `ImapWriteCallback` appended every byte returned by libcurl into the response `std::string` with no upper bound.  A hostile or compromised IMAP server can stream an arbitrarily large response, exhausting process memory.

**Verification:** Holds.  Code at `emailConnector.cpp:164-169` (pre-fix) was the bare append.

**Change:**  `static constexpr size_t kMaxImapResponseBytes = 10 * 1024 * 1024;` in the connector's anon namespace.  In the write callback, check `buf->size() + incoming > kMaxImapResponseBytes` before appending; on overflow, return 0 — libcurl interprets this as `CURLE_WRITE_ERROR` and aborts the transfer cleanly.  The caller's existing error path (`if (res != CURLE_OK) { errorMessage = "IMAP request failed: ..."; return false; }`) handles the abort uniformly.  10 MB matches the audit recommendation — production IMAP responses are typically tens of KB; a server returning >10 MB to a SEARCH command is already pathological.

**Verified at runtime:**  Build clean.  Default emailDemo's IMAP responses are well under 10 MB; the cap doesn't fire.  Reproducing the >10 MB scenario requires a malicious IMAP server fixture.  Same fixture-deferral posture as the other hostile-server scenarios.

---

### Validator helpers lifted from executor to connector header (refactor)

**Finding (carry-over from `feedback_cpp_discipline`):**  Sitting 11 created `IsValidImapFolder` and `IsValidImapUid` as file-local statics in `emailCloudTaskExecutor.cpp`.  Sitting 13's connector-layer defense-in-depth needs the same predicates.  The choice was either (a) duplicate the helpers as a second copy in `emailConnector.cpp` anon namespace (adds the 2nd of an eventual 3 copies that would trigger refactor under `feedback_cpp_discipline`), or (b) lift to a shared location.  Chose (b) — header lift to `EmailConnector` public statics.

**Change:**
1. `emailConnector.h` — add 5 `[[static]]` declarations on `EmailConnector`: `IsValidImapFolder`, `IsValidImapUid`, `IsValidImapSubjectFilter`, `IsValidEmailHost`, `IsValidEmailPort`.  Multi-line comment block explains the shared-source-of-truth contract.
2. `emailConnector.cpp` — add the definitions (same body sitting 11 had for `IsValidImapFolder` + `IsValidImapUid`; new for the subject-filter / host / port validators).
3. `emailCloudTaskExecutor.cpp` — delete the file-local `IsValidImapFolder` + `IsValidImapUid` definitions (sitting 11 left them at lines 78–134).  Replace 2 call sites: `IsValidImapFolder(folder)` → `EmailConnector::IsValidImapFolder(folder)`; same for UID.  `ContainsCrlf` is left as a one-line file-local helper because it's trivially short and only used in this file.

**Verified at runtime:**  emailDemo happy-path runs identically to before the refactor (the helpers' bodies are bit-for-bit the same — the lift was purely a relocation).

---

## Skipped findings table — Sitting 13

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| Credentials / key names in error messages (low risk leakage to caller logs) | `emailConnector.cpp` | MEDIUM cyber | Partially mitigated by the SSRF host gate this sitting added — the audit's primary concern was the IMAP URL `user:password@host` form leaking via `curl_easy_strerror`, which the new strict host validation makes structurally harder.  The remaining concern (`m_KeyName` echoed in errors) is a separate tracked item; a future MEDIUM mini-sweep on the cloud-surface error-message hygiene closes it without disrupting the URL-validation work. |
| `ContainsCrlf` not lifted to a shared header | `emailCloudTaskExecutor.cpp` | (style) | Trivial one-line check (`s.find('\r') != npos || s.find('\n') != npos`); only used in one file.  Lifting it would add a header surface for negligible benefit. |
| Stress fixtures for hostile IMAP server responses | `emailConnector.cpp` | (verification gap) | Several fixes in this sitting (UID DoS try/catch, response buffer cap, IMAP injection allowlists) close their threat models structurally but lack reproducer fixtures because they require a controlled malicious-IMAP fixture.  Defer to the cybersec fixture sitting. |
| TLS-strict path live verification under a real TLS-enabled IMAP/SMTP server | `emailConnector.cpp` | (verification gap) | Requires a Postfix-with-TLS or Dovecot Docker container in the test environment.  Defer to the cybersec fixture sitting. |
| `emailConnector.h::~EmailConnector` declaration | `emailConnector.h` | (style) | The header doesn't declare a virtual destructor explicitly, but `ICloudConnector` (the base class) has `virtual ~ICloudConnector() = default`, so the chain is correct.  No defect, just noting that future maintainers needn't worry. |

---

## Sitting 14 — snowflakeCloudTaskExecutor.cpp + snowflakeConnector.cpp comprehensive: SSRF + path-traversal + TLS + JSON-injection + JWT-CRLF + DoS

**Scope locked:** every CRITICAL and HIGH finding on `application/cloud/snowflakeCloudTaskExecutor.cpp` (the densest single-file cluster on the cloud surface — 3 CRIT + 5 HIGH per the audit) plus the parallel issues in `snowflakeConnector.cpp::TestConnection` + `BuildApiBaseUrl` (matching the email-surface "executor + connector" comprehensive close pattern from sitting 13).  Fixes mostly mechanical applications of patterns established in sittings 11–13: `ValidateLocalPath` for path traversal, `kMaxXxxResponseBytes` writeCallback cap, `JsonHelper::EscapeJsonString` for JSON injection, `ContainsCrlf` reject for header injection, unconditional `CURLOPT_SSL_VERIFY*` for TLS posture, `std::clamp` for timeout/poll bounds.  Boundary at sitting-end: **the Snowflake surface is fully closed at the CRITICAL/HIGH level**.

### `BuildApiBaseUrl` allowlist + scheme rejection (CRIT SSRF)

**Finding (CRITICAL cyber):**  Pre-fix `BuildApiBaseUrl` accepted `m_Endpoint` as either an account locator (`xy12345.us-east-1`) which it appended to `https://...snowflakecomputing.com`, or a full user-provided URL (`https://...` or `http://...`) which it returned **as-is**.  The "as-is" branch was the SSRF vector: `m_Endpoint = "http://evil.com/path?x="` would sail through, and the executor would then issue authenticated HTTP requests (with the JWT Bearer token) to whatever target the value pointed at.  Combined with `CURLOPT_FOLLOWLOCATION = 1L` (which the executor and connector both set), a redirect chain could pivot to internal services.

**Verification:** Holds.  Code at `snowflakeConnector.cpp:43-57` (pre-fix) was the conditional-passthrough for prefixed URLs, exactly as audited.

**Change:**  Drop the user-provided-URL branch entirely.  Endpoint must now be a strict account locator: alphanumeric + `.` + `-` + `_`, max 128 bytes, non-empty.  Any rejection emits `[security] snowflake_invalid_endpoint reason={size,charset} endpoint_length={}` with the value's length only (not the value, which could encode sensitive deployment topology).  Always returns `"https://" + endpoint + ".snowflakecomputing.com"`.  No `http://` accepted, no full URLs accepted, no other domains accepted.

This change is somewhat **breaking**: a deployment that had configured `m_Endpoint` as a full URL for testing now needs to switch to the account-locator form.  Per `cloud-integration.md:651` the documented contract was always "account locator with region" — the full-URL handling was an undocumented escape hatch.  Per project policy (alpha, no production users), the breaking change is acceptable.

**Verified at runtime:**
- 28-test suite + hermetic dispatcher PASS.
- **Live SSRF rejection:** mutated `my-snowflake.endpoint` to `"evil.com/path?x="` via REST PUT, then `POST /api/connections/my-snowflake/test` → HTTP 400 + `Snowflake endpoint rejected: invalid account locator (see security log)`.  Security log: `[security] snowflake_invalid_endpoint reason=charset endpoint_length=16`.
- emailDemo regression check: PASS (the shared cloud-surface curl pattern is intact).
- **Not directly verified:** the live happy-path against a real Snowflake endpoint (no Snowflake account in test env).  The fix is structural; the verification posture is "the gate produces correct output given correct input" — which the canonical `TVXEFHO-JHB68153` value would pass cleanly.

---

### Response-body cap in `writeCallback` (CRIT DoS)

**Finding (CRITICAL cyber):**  `SnowflakeRequest`'s `CURLOPT_WRITEFUNCTION` lambda appended every byte received from the remote into `responseBody` with no upper bound.  An attacker-controlled or compromised Snowflake endpoint (or — combined with the SSRF above — any pivoted-to internal service) could stream arbitrary bytes, exhausting process memory.

**Verification:** Holds.  Code at `snowflakeCloudTaskExecutor.cpp:60-65` (pre-fix) was the bare append.

**Change:**  `static constexpr size_t kMaxSnowflakeResponseBytes = 64 * 1024 * 1024;` (audit recommendation).  In the writeCallback, check `buf->size() + incoming > cap` before appending; on overflow return 0 → libcurl aborts with `CURLE_WRITE_ERROR`, which the caller's existing error path handles.  64 MB matches the audit recommendation — Snowflake result sets paginate, so a single response larger than this is pathological.  Same cap added in parallel to `snowflakeConnector.cpp::TestConnection`'s writeCallback, with a tighter local `kMaxConnectorResponseBytes = 1 MB` (test-connection responses are tiny).

**Verified at runtime:**  Build clean.  emailDemo regression check still passes — the connector-side writeCallback structure is the same shape used everywhere else.  Reproducing the >64 MB scenario requires a malicious endpoint fixture; defer.

---

### TLS verify-peer + verify-host unconditional (HIGH cyber)

**Finding (HIGH cyber):**  Neither `SnowflakeRequest` nor `TestConnection` ever explicitly set `CURLOPT_SSL_VERIFYPEER` or `CURLOPT_SSL_VERIFYHOST`.  Both relied on libcurl defaults, which can fail-open on builds where the trust store is empty.  No use_ssl-style opt-out applies because Snowflake is HTTPS-only by protocol — every request must verify.

**Change:**  Unconditionally `curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L)` and `curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L)` in both `SnowflakeRequest` and `TestConnection`.  No use_ssl gate (Snowflake is HTTPS-only).

**Verified at runtime:**  Build clean; emailDemo regression PASS.  Strict-TLS branch verification requires a real Snowflake endpoint; structural fix.

---

### `CURLOPT_FOLLOWLOCATION` disabled

**Finding (defense-in-depth combined with SSRF):**  Both `SnowflakeRequest` and `TestConnection` had `CURLOPT_FOLLOWLOCATION = 1L`.  Combined with the BuildApiBaseUrl SSRF vector, a redirect chain from a hostile endpoint could pivot to internal services (with the JWT Bearer token attached).  Snowflake's API never legitimately redirects, so the option had no positive use.

**Change:**  `curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L)` in both functions.  The BuildApiBaseUrl charset gate already prevents the primary SSRF vector; disabling redirect-following closes the chained-redirect vector even if a future regression breaks the endpoint validation.

---

### JWT CRLF rejection (HIGH header injection)

**Finding (HIGH cyber):**  `authHeader = "Authorization: Bearer " + jwt;` concatenated `credentials.m_Token` directly into a header string.  CR/LF in the JWT would split into multiple headers — recent libcurl versions strip embedded newlines, but version-dependent behaviour shouldn't be relied on.

**Change:**  Add file-local `static bool ContainsCrlf(std::string const&)` (matches the executor pattern from sitting 11; not lifted to `EmailConnector`-style shared header because no other Snowflake call site needs it yet).  Validate `credentials.m_Token` at `ExecuteCloud` entry; reject = task Failed + LOG_APP_ERROR + `[security] snowflake_jwt_crlf_rejected`.  Same check in `TestConnection`: reject = `errorMessage` set + `[security] snowflake_test_jwt_crlf_rejected` SECURITY_WARN.

---

### JSON injection on warehouse / database / schema (HIGH JSON injection)

**Finding (HIGH cyber):**  `requestBody += ",\"warehouse\":\"" + warehouse + "\""` (and equivalent for database / schema) spliced raw values.  A connection-param value containing `","timeout":0,"x":"` would close the JSON string early and override request fields.  Only `query` was JSON-escaped pre-fix.

**Change:**  Route all three values through `JsonHelper::EscapeJsonString` (the canonical RFC 8259 §7-compliant helper that sittings 9 + 12 converged the file-local copies onto).  Same fix applied to `TestConnection`'s parallel requestBody build.

---

### `m_LastErrorMessage` raw-response sanitization (HIGH secrets leakage)

**Finding (HIGH cyber):**  Both error paths (HTTP 4xx/5xx on submit and on poll) embedded up to 500 bytes of the raw Snowflake response in `m_LastErrorMessage`.  Snowflake error responses can include schema names, partial query data, and operational metadata that shouldn't leak into the persisted workflow state (which surfaces in the dashboard, REST API, and workflow logs).  Same pattern in `TestConnection`'s `errorMessage`.

**Change:**  Drop the `if (!responseBody.empty() && responseBody.size() < 500) { errorMessage += ": " + responseBody; }` blocks at all three sites (executor submit, executor poll, connector test).  Error message keeps the structured `HTTP {code}` portion only.  The structured `code` / `message` fields from a parseable Snowflake response are extracted separately on the success-but-error-status path (existing code at line ~398), so legitimate diagnostic information remains accessible.

**Note:** This sitting only sanitizes `m_LastErrorMessage`.  `WriteResponseJson(workDir, taskState, responseBody)` still writes the raw response body to `response.json` for downstream-task consumption — that's a load-bearing contract (downstream tasks parse the result set).  The audit's broader "raw response in response.json contains PII" concern is **architectural** and out of cluster 14 scope; bundles into a future "cloud-task output sensitivity policy" design memo.

---

### `statementTimeout` / `pollInterval` clamp (HIGH resource exhaustion)

**Finding (HIGH safety + cyber):**  Both `timeout` and `poll_interval` JSON params were cast `static_cast<int>(uint64_t)` with no upper bound check.  Values > INT_MAX wrap to negative (`statementTimeout < 0` disables the polling timeout entirely → indefinite worker thread pin).  Even values in `[1, INT_MAX]` allow denial-of-service via a 999_999_999-second timeout.

**Change:**  Define `kMaxStatementTimeoutSeconds = 24 * 3600` and `kMaxPollIntervalSeconds = 60`.  Pre-clamp the uint64_t to the cap **before** the int cast: `if (val > kMaxXxx) { val = kMaxXxx; }`, then `std::clamp(static_cast<int>(val), 1, kMaxXxx)` — single-line fix per param.

---

### Snowflake handle validation (MEDIUM URL injection)

**Finding (MEDIUM cyber):**  `handle` was extracted from the Snowflake submit response and interpolated into `pollUrl` and `cancelUrl` without validation.  A compromised Snowflake endpoint or response-tampering MitM could inject URL components (path traversal, query string).

**Change:**  New file-local `static bool IsValidSnowflakeHandle(std::string const&)`: alphanumeric + `-`, max 64 bytes (UUID-like).  Validate at the top of the polling block; reject = task Failed + ERROR + `[security] snowflake_invalid_handle`.  This is a defense-in-depth gate — Snowflake's documented handle format is UUID-style, but the executor shouldn't trust the server-supplied value blindly when it ends up in a URL.

---

### `outputFile` path traversal (CRIT path traversal)

**Finding (CRITICAL cyber):**  `outputPath = workDir / outputFile;` with no validation.  An attacker-controlled JCWF param `outputFile: "../../etc/cron.d/evil"` would let the task overwrite arbitrary files.

**Change:**  Apply `ICloudTaskExecutor::ValidateLocalPath(outputFile, workDir, taskDefinition.m_Id)` before opening the file for write — same gate as sitting 11's email body_file / attachments fixes.  Reject = task Failed + LOG_APP_ERROR with task / workflow / run identifiers.

---

## Skipped findings table — Sitting 14

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| `WriteResponseJson` writes raw API response (potentially containing PII) to `response.json` | `snowflakeCloudTaskExecutor.cpp` | HIGH cyber (architectural) | Out of cluster 14 scope.  `response.json` is a load-bearing contract — downstream tasks parse it for the result set.  Removing it or redacting it would break the workflow contract.  The audit's full fix would gate the write behind a debug flag OR redesign downstream tasks to consume only the structured result.  Architectural decision; bundles into a future "cloud-task output sensitivity policy" design memo. |
| SQL injection risk on `query` (architectural) | `snowflakeCloudTaskExecutor.cpp` | MEDIUM cyber (architectural) | The `query` field is fully trusted input by design — JCWF authors author SQL just as they author shell commands.  The fix is access control on JCWF authoring + Snowflake role-level least-privilege, not validation in the executor.  Documented as a known architectural property. |
| Synchronous result path uses stale `responseBody` from submit response | `snowflakeCloudTaskExecutor.cpp` | MEDIUM safety | Code-correctness concern about simdjson `ondemand::document` lifetimes.  Currently safe (the `responseBody` `std::string` lives for the entire scope), but fragile to future refactors.  Defer to a future MEDIUM safety mini-sweep. |
| Column names written to JSON/CSV output without bounds check | `snowflakeCloudTaskExecutor.cpp` | LOW safety | Bounds-check on column name length before splicing into the output writer.  No realistic Snowflake schema has multi-MB column names; defer with the Rust-emulating C++ defaults sweep. |
| Inline JSON-escape switch blocks in query escape + result-row-write paths | `snowflakeCloudTaskExecutor.cpp` | (convergence) | Two inline `switch` blocks duplicate `JsonHelper::EscapeJsonString`'s logic (query escape at lines ~230-241, JSON output writer at lines ~528-538).  Sitting 12's "no anon-namespace JsonEscape copies remain" claim still holds (these are inline switch blocks, not named helpers).  Cleanup-grade convergence; bundle with a future "inline-escape-block sweep" or with the response.json architectural review. |
| Stress fixture for the `kMaxSnowflakeResponseBytes` cap | `snowflakeCloudTaskExecutor.cpp` | (verification gap) | Reproducing the > 64 MB scenario requires a malicious endpoint fixture.  Defer to the cybersec fixture sitting. |
| Live happy-path against a real Snowflake account | `snowflakeCloudTaskExecutor.cpp` + `snowflakeConnector.cpp` | (verification gap) | No Snowflake account in test env.  Fix shape matches established patterns (TLS, response cap, JSON escape, JWT validation are all structural); emailDemo regression check confirms the shared cloud-surface curl pattern is intact. |

---

## Sitting 15 — Horizontal Sweep #1: `local_path` / `output_file` path-traversal across 5 cloud task executors

**Scope locked:** the first **horizontal sweep** — one pattern (CRITICAL filesystem-path traversal on caller-supplied local-file params) closed across 5 cloud task executors in a single sitting.  Files: `azureBlobCloudTaskExecutor.cpp`, `gcsCloudTaskExecutor.cpp`, `googleSheetsCloudTaskExecutor.cpp`, `oneDriveCloudTaskExecutor.cpp`, `s3CloudTaskExecutor.cpp`.  Each had at least one CRITICAL audit finding flagging "param X taken directly from JSON params and passed to `std::ifstream` / `std::ofstream` / a download helper without canonicalisation or containment", with attacker-controlled paths like `../../etc/passwd` (read) or `../../etc/cron.d/evil` (write) escaping the intended workspace.  Single helper applied to all 5 — `ICloudTaskExecutor::ValidateLocalPath(path, baseDir, taskId)` — with the right `baseDir` per file (CWD-relative for azureBlob/gcs/s3, workDir-relative for googleSheets/oneDrive, matching each file's existing usage convention).  This is the first of an estimated 4–5 horizontal sweeps that will close the cross-cutting cloud-surface concerns; subsequent sweeps will tackle response-body caps, TLS verification, SSRF host validation, and JSON-injection systematically.

**Naming convention introduced:** "Horizontal Sweep #N — \<pattern\> across \<files\>" — vs. the depth-first per-file sittings (Sittings 11–14).  See sitting 14 hand-off for the rationale + horizontal-sweep candidate list.

### Per-file changes

All 5 files received the same shape: after the existing `getStringParam` extraction + empty-check, **before** any file open / download / read, add a `ValidateLocalPath(path, baseDir, taskDefinition.m_Id)` gate.  On reject: set `m_LastErrorMessage` (file-specific message), transition to `Failed`, emit `LOG_APP_ERROR` with task / workflow / run identifiers (per `feedback_log_failures` for dashboard analyzer compatibility), return false.  `ValidateLocalPath` itself emits the canonical `[security] path_traversal_blocked: task='{}' local_path='{}' contains '..'` (or `... resolved='{}' escapes base='{}'`) SECURITY_INFO line via the helper's existing implementation.

| File | Param | baseDir | Convention |
|---|---|---|---|
| `azureBlobCloudTaskExecutor.cpp` | `local_path` | launch CWD | CWD-relative (canonical demo: `"workflows/azureBlobDemo/file.csv"`) |
| `gcsCloudTaskExecutor.cpp` | `local_path` | launch CWD | CWD-relative (canonical demo: `"workflows/gcsDemo/file.csv"`) |
| `googleSheetsCloudTaskExecutor.cpp` | `output_file` (read op) + `input_file` (write op) | workDir | workDir-relative (existing `workDir / file` pattern) |
| `oneDriveCloudTaskExecutor.cpp` | `local_path` | workDir | workDir-relative (existing `workDir / localPath` pattern) |
| `s3CloudTaskExecutor.cpp` | `file_path` (upload + download branches) | launch CWD | CWD-relative (canonical demo: `"workflows/s3UploadDownloadDemo/server_metrics.csv"` and `"queue/<workflow>/<task>/output.txt"`) |

**Bonus refactor in oneDrive:** the upload + download branches both resolved `workflowBaseDir` + `workDir` + `fullLocalPath` independently.  Lifted the resolution above the branch (single site, single validation gate, both branches reference the shared `fullLocalPath`).  The trailing `WriteResponseJson` block (lines 314–325 pre-fix) also re-resolved `workflowBaseDir` + `workDir`; after the lift, it reuses the already-resolved values (`-Wshadow` warning closed as a bonus).

### Convention rationale (CWD-relative vs workDir-relative)

This was the trickiest design decision.  Three of the five files (azureBlob, gcs, s3) opened the user-supplied path **directly** (`std::ifstream(localPath)`) — meaning the path was OS-resolved against the process's current working directory (j9t's launch CWD).  The canonical demo workflows for those three pass values like `"workflows/azureBlobDemo/file.csv"` — clearly CWD-relative through the `workflows/` and `queue/` trees.  The other two (googleSheets, oneDrive) joined the path with workDir first (`workDir / outputFile` etc.) — clearly workDir-relative.

Picking the wrong base would either (a) break the demos (workDir-confine for the CWD-relative files would reject everything except a literal path inside the queue task folder), or (b) over-permit (CWD-confine for the workDir-relative files would let one task read another task's queue folder).  The chosen baseDir per file matches each file's existing usage convention — closing the traversal vector without any contract change.

The `body_file` fix from sitting 11 used the same CWD-relative reasoning for its `body_file` param; this sweep applies the same model to azureBlob / gcs / s3.  GoogleSheets / oneDrive already used workDir-relative paths internally; the sweep just adds the gate at the entry point.

### What's verified

- Studio debug build clean (`make config=debug`).  All 5 files recompiled without diagnostic.
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- **emailDemo regression check:** 3 tasks succeeded in 2 s end-to-end.  Confirms the shared cloud-surface infrastructure (the `ICloudTaskExecutor` base class, `ValidateLocalPath` helper, all the curl + JSON helpers) is intact across the email surface that sittings 11+12+13 closed.
- **Live S3 negative test (representative for the 5 files since the gate is identical):**  Mutated `s3UploadDownloadDemo`'s `upload_data.params.file_path` to `"../../../../etc/passwd"` via direct JSON edit + `POST /api/workflows/reload`.  Ran via `mcp__j9t__run_workflow` → run state `failed` at `upload_data`; downstream tasks (`download_data`, `ai_analyze`, `upload_report`, `list_objects`) all `skipped` per dependency policy.  Log produced exactly the expected three lines:
  - `[Security] [info] [security] path_traversal_blocked: task='upload_data' local_path='../../../../etc/passwd' contains '..'`
  - `[Application] [error] [s3] task='upload_data' workflow='s3UploadDownloadDemo' run='s3UploadDownloadDemo_1777695935': file_path rejected (upload)`
  - `[Application] [error] [workflow] task 'upload_data' failed in run 's3UploadDownloadDemo_1777695935': s3 upload: file_path is invalid or escapes the launch directory`
  - Run state: `failed`; downstream tasks correctly skipped per existing dependency-failure policy.
  Demo restored byte-identical to backup post-test.
- **Not directly verified:**
  - Live negative tests on the other 4 files (azureBlob / gcs / googleSheets / oneDrive).  The gate code is **structurally identical** across all 5 — same `ValidateLocalPath` call, same fail-task pattern, same SECURITY_INFO log line.  A live test on one is sufficient evidence the others work; per-file fixtures would be redundant.
  - Live happy-path against the actual cloud services (S3, GCS, Azure, etc.).  Test env doesn't host those; structural fix.
  - The workDir-relative variant on googleSheets / oneDrive (the S3 live test exercises the launch-CWD-relative variant).  Same posture — gate code is identical, just a different `baseDir`.

### Skipped findings table — Sitting 15

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| URL-side path traversal — `bucket` / `object_name` / `remote_path` / `key` / `prefix` interpolation into request URLs | `gcsCloudTaskExecutor.cpp` (object_name + bucket), `oneDriveCloudTaskExecutor.cpp` (remote_path), `s3CloudTaskExecutor.cpp` (key + prefix) | CRIT × 2 + HIGH × 2 | Out of cluster scope.  These are URL-side injections — distinct threat model from filesystem-path traversal.  Bundle into Horizontal Sweep #4 (SSRF / URL-injection) or its own dedicated sweep. |
| Unbounded file read on upload (DoS / OOM) | All 5 files | HIGH × 5 | Out of scope.  Each file does `std::string fileData(static_cast<size_t>(fileSize), '\\0')` + `file.read(...)` with no upper bound.  Bundle into Horizontal Sweep #2 (response-body + file-read caps).  Note: S3 + OneDrive already have `CURLOPT_MAXFILESIZE_LARGE = 256 MB` on **downloads** per Phase 9 hardening, but the audit notes uploads have no equivalent guard. |
| TLS verify-peer / verify-host conditional or missing | All 5 files | HIGH × 5 | Out of scope.  Bundle into Horizontal Sweep #3 (TLS verify unconditional). |
| Unbounded `responseBody` growth in writeCallback | All 5 files | HIGH × 5 | Out of scope.  Bundle into Horizontal Sweep #2. |
| JSON-injection on request-body fields | Multiple files | MEDIUM × N | Out of scope.  Bundle into Horizontal Sweep #5 (JSON-injection cleanup). |
| `stoi`/`stoull` exception on hostile input | `s3CloudTaskExecutor.cpp` (max_keys), others | HIGH × 2 | Out of scope.  Bundle into the parser-hardening sweep (similar to sitting 13's `IsValidImapUid` work). |
| Bearer token / credential CRLF rejection in HTTP headers | `gcsCloudTaskExecutor.cpp`, `oneDriveCloudTaskExecutor.cpp`, others | HIGH × N | Out of scope.  Bundle with Horizontal Sweep #4 or with a dedicated header-injection sweep (similar to sitting 14's JWT CRLF rejection in Snowflake). |

---

## Sitting 16 — Horizontal Sweep #2: response-body cap + file-read cap on upload across 5 cloud task executors

**Scope locked:** the second horizontal sweep — two parallel resource-exhaustion gates closed across the same 5 cloud task executors as Sweep #1.  9 fixes total: 5 writeCallback response-body caps (one per file) + 4 file-read upload caps (skip googleSheets — its sheets_write reads CSV line-by-line, different pattern).  All caps follow the established sitting-13 + sitting-14 pattern: define `kMax<File>{Response,Upload}Bytes` constant, check `buf->size() + incoming > cap` (writeCallback) or `fileSize > cap` (upload), abort cleanly on overflow.

### Per-file changes

| File | writeCallback cap | Upload-read cap |
|---|---|---|
| `azureBlobCloudTaskExecutor.cpp` | `kMaxAzureBlobResponseBytes = 64 MB` (in AzureBlobRequest) | `kMaxAzureBlobUploadBytes = 256 MB` |
| `gcsCloudTaskExecutor.cpp` | `kMaxGcsResponseBytes = 64 MB` (in GcsRequest) | `kMaxGcsUploadBytes = 256 MB` |
| `googleSheetsCloudTaskExecutor.cpp` | `kMaxSheetsResponseBytes = 64 MB` (in SheetsRequest) | (no binary upload — sheets_write reads CSV line-by-line, deferred) |
| `oneDriveCloudTaskExecutor.cpp` | `kMaxOneDriveResponseBytes = 64 MB` (in GraphRequest) | `kMaxOneDriveUploadBytes = 256 MB` |
| `s3CloudTaskExecutor.cpp` | `kMaxS3ResponseBytes = 64 MB` (in S3Request) | `kMaxS3UploadBytes = 256 MB` |

**Constants chosen:**
- **Response cap = 64 MB.**  Matches Snowflake's `kMaxSnowflakeResponseBytes` from sitting 14 — generous enough for paginated cloud-API list responses, tight enough to bound a hostile/compromised endpoint's memory exhaust attempt.  Email's IMAP cap (sitting 13) was 10 MB — tighter because IMAP `SEARCH` responses are intentionally small; this sweep's cloud-API responses can legitimately be much larger.  Variation per surface is by design; don't unify.
- **Upload cap = 256 MB.**  Matches Phase 9b's existing `CURLOPT_MAXFILESIZE_LARGE = 256 MB` for downloads — symmetric upload/download cap.  Closes the audit's HIGH "uncontrolled file read into memory on upload" finding for S3 / GCS / Azure Blob / OneDrive in one consistent value.

**writeCallback pattern (same across all 5 files):**
```cpp
auto writeCallback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t
{
    auto* buf = static_cast<std::string*>(userp);
    size_t const incoming = size * nmemb;
    if (buf->size() + incoming > kMax<File>ResponseBytes)
    {
        return 0; // CURLE_WRITE_ERROR; caller surfaces as a request failure
    }
    buf->append(static_cast<char*>(contents), incoming);
    return incoming;
};
```

**Upload-read pattern (same across 4 files):**
```cpp
auto const fileSize = file.tellg();
static constexpr std::streamoff kMax<File>UploadBytes = 256 * 1024 * 1024;
if (fileSize < 0 || fileSize > kMax<File>UploadBytes)
{
    taskState.m_LastErrorMessage = "<file> upload: file size {} exceeds {} byte cap";
    taskState.m_State = TaskInstanceStateKind::Failed;
    LOG_APP_ERROR("[<file>] task='{}' workflow='{}' run='{}': upload size {} exceeds cap", ...);
    return false;
}
file.seekg(0, std::ios::beg);
std::string fileData(static_cast<size_t>(fileSize), '\0');
file.read(fileData.data(), fileSize);
```

The negative `fileSize` check covers `tellg()`'s error sentinel (`pos_type` returns -1 on stream error) — defense-in-depth even though the preceding `is_open()` check should mean we're reading a valid stream.

### What's verified

- Studio debug build clean.  All 5 files recompiled.
- 28-test assistant non-AI suite + hermetic dispatcher: PASS.
- emailDemo regression check: 3 tasks succeeded in 2 s.  Confirms shared cloud-surface infrastructure is intact.
- **Not directly verified:** live trigger of either cap (would need a 300 MB synthetic file for the upload cap, or a malicious endpoint streaming >64 MB for the response cap).  The fix shape is **structurally identical** to:
  - Sitting 12's emailDemo attachment 25 MB cap (verified live with a 30 MB synthetic file — log line `attachment 'big_blob.bin' size 31457280 bytes exceeds 26214400 byte cap; skipping`).
  - Sitting 13's IMAP response 10 MB cap (structural).
  - Sitting 14's Snowflake response 64 MB cap + connector 1 MB cap (structural).
  - Verification posture: "the bug class is gone by construction; pattern is established and was previously verified live in sitting 12".

### Skipped findings table — Sitting 16

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| googleSheets sheets_write line-by-line CSV read | `googleSheetsCloudTaskExecutor.cpp` | (carry from this sweep) | The sheets_write op reads its `input_file` line-by-line into `std::vector<std::vector<std::string>>` — different pattern from `tellg + std::string allocate`.  Bound by limiting either total bytes accumulated, line count, or per-line length.  Defer to a future MEDIUM input-parser sweep. |
| Per-connector writeCallback caps (TestConnection helpers) | `gcsConnector.cpp`, `azureBlobConnector.cpp`, `googleSheetsConnector.cpp`, `oneDriveConnector.cpp`, `s3Connector.cpp` | MEDIUM × 5 | Out of scope — connector test paths handle small responses (single API ping for connectivity verification).  The audit didn't flag these specifically; a future MEDIUM mini-sweep can add the caps if a connector's TestConnection ever surfaces a DoS concern. |
| Other cloud task executors / connectors not in sweep #1's set | `slackCloudTaskExecutor.cpp`, `jiraCloudTaskExecutor.cpp`, `gitHubCloudTaskExecutor.cpp`, `redmineCloudTaskExecutor.cpp`, `polarionWriteTaskExecutor.cpp`, `dbQueryCloudTaskExecutor.cpp`, plus their connectors | (varies) | Bundle into a future "Horizontal Sweep #2 part 2" or close them depth-first when the file is touched for other reasons.  Several of these don't read large files into memory (Slack messages are small, Jira issue creates are tiny, etc.), so the resource-exhaustion vector is much narrower. |
| Live verification of either cap firing under load | (multiple) | (verification gap) | Per-cap reproduction needs either a >256 MB synthetic file (10+ seconds of disk I/O + 256 MB temp space) or a malicious endpoint streaming oversized responses.  The pattern was previously verified live in sitting 12; per-file fixtures here would be redundant.  Defer to the cybersec fixture sitting. |

---

## Sitting 17 — Horizontal Sweep #3: TLS verify-peer + verify-host unconditional across 5 cloud task executors

**Scope locked:** the third horizontal sweep — TLS strict verification gate added to each cloud-task curl-setup helper across the same 5 sweep-#1 files.  9 setopt-pair additions total (each file has 1-2 helper functions).  Closes the audit's HIGH "TLS Peer Verification Conditionally Disabled" / "Missing TLS Certificate Verification Fallback" finding on each of the 5 files.  The smallest sweep yet — single 2-line block applied via `replace_all` per file, ~5 minutes of code work, ~10 minutes total including verification.

### Per-file changes

Each file had the same pre-fix pattern (1-2 occurrences per file):
```cpp
auto const& caBundle = CurlWrapper::GetCaBundlePath();
if (!caBundle.empty())
{
    curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
}
```

Post-fix pattern (applied via `replace_all=true` to cover both helper functions in each file):
```cpp
auto const& caBundle = CurlWrapper::GetCaBundlePath();
if (!caBundle.empty())
{
    curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
}
// Explicit TLS verify — closes the audit's HIGH ...
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
```

| File | Helper sites covered |
|---|---|
| `azureBlobCloudTaskExecutor.cpp` | `AzureBlobRequest` + `AzureBlobDownload` (×2) |
| `gcsCloudTaskExecutor.cpp` | `GcsRequest` + `GcsDownload` (×2) |
| `googleSheetsCloudTaskExecutor.cpp` | `SheetsRequest` (×1; no separate download — sheet reads are JSON-API GETs through the same helper) |
| `oneDriveCloudTaskExecutor.cpp` | `GraphRequest` + `GraphDownload` (×2) |
| `s3CloudTaskExecutor.cpp` | `S3Request` + `S3Download` (×2) |

**Rationale for unconditional posture (no `use_ssl` gate):**  All 5 cloud surfaces are HTTPS-only protocols — there's no plaintext-mode equivalent to email's GreenMail.  S3-compatible alternatives like local MinIO can use `http://` for testing, but the verify setopts are no-ops on HTTP (libcurl only applies them on TLS handshake).  So setting them unconditionally is safe + closes the strict-on-HTTPS gap without breaking local-test workflows.

### What's verified

- Studio debug build clean.  All 5 files recompiled.
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- emailDemo regression check: 3 tasks succeeded in 2 s.
- **Not directly verified:** the strict-TLS gate firing under MITM — would need a controlled TLS proxy fixture.  The fix is structural; same shape as sitting 14's Snowflake fix which was verified to compile + happy-path against the GreenMail demo.

### Skipped findings table — Sitting 17

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| Per-connector TLS verify (the corresponding `*Connector.cpp` files' `TestConnection` helpers for the 5 cloud surfaces) | `azureBlobConnector.cpp`, `gcsConnector.cpp`, `googleSheetsConnector.cpp`, `oneDriveConnector.cpp`, `s3Connector.cpp` | HIGH × 5 | Out of this sweep's scope (executors only).  Bundle into a future "TLS verify on connectors" mini-sweep — fix shape is identical, just different files.  Or fold into Horizontal Sweep #4 if it touches the same files. |
| MITM live verification | All | (verification gap) | Defer to cybersec fixture sitting. |

---

## Sitting 18 — Horizontal Sweep #4: URL-side injection / SSRF + Bearer CRLF across 3 cloud task executors

**Scope locked:** the fourth horizontal sweep — URL-component sanitization + Bearer-token CRLF rejection across 3 cloud-storage executors (gcs, oneDrive, s3).  Closes the audit's CRIT × 2 (gcs `bucket` + oneDrive `remote_path` URL injection) + HIGH × 3 (s3 `key`+`prefix` unencoded + gcs/oneDrive bearer CRLF).  More involved than sweeps #1-3 because each cloud surface has different valid-character rules — strict allowlist for GCS bucket + OneDrive path, percent-encoding for S3 key (which can legitimately contain almost any UTF-8).

### Per-file changes

| File | Concern | Fix |
|---|---|---|
| `gcsCloudTaskExecutor.cpp` | CRIT — `bucket` raw-spliced into URL | New `IsValidGcsBucket` allowlist per GCS naming rules (`[a-z0-9._-]`, 3-63 chars, no leading/trailing hyphen).  Validate at `bucket` extraction site before any URL build. |
| `gcsCloudTaskExecutor.cpp` | HIGH — Bearer token CRLF | New `ContainsCrlf` file-local helper.  Reject in `GcsRequest` + `GcsDownload` entry. |
| `oneDriveCloudTaskExecutor.cpp` | CRIT — `remote_path` raw-spliced into Graph URL | New `IsValidOneDriveRemotePath` allowlist (alphanumeric + `._-/` + space, max 1024 bytes, no `..` segments).  Validate at `remote_path` extraction site. |
| `oneDriveCloudTaskExecutor.cpp` | HIGH — Bearer token CRLF | Same shape as GCS.  `ContainsCrlf` + reject in `GraphRequest` + `GraphDownload` entry. |
| `s3CloudTaskExecutor.cpp` | HIGH — `key` + `prefix` unencoded | New `UrlEncodeS3Key` helper that preserves `/` as path delimiter and percent-encodes every other byte via `curl_easy_escape`.  Apply at all 4 key-build sites + 1 prefix site. |

### Design rationales

**GCS bucket: strict allowlist (not URL-encoding).**  GCS naming rules are deliberately restrictive — buckets can only contain `[a-z0-9._-]` per Google's spec.  Any name that would need URL-encoding is by definition not a valid GCS bucket, so allowlisting + rejecting catches both attack and configuration errors.  `object_name` is already URL-encoded via the existing `UrlEncode` lambda (pre-existing code; this sweep doesn't change it).

**OneDrive remote_path: strict allowlist + `..` rejection.**  OneDrive paths support most UTF-8 chars but the most realistic legitimate range is alphanumeric + `._-/` + space.  Stricter than the GCS allowlist (allows `/` and space), but no `..` segments — those would let a hostile path escape the user's drive scope through Microsoft Graph's path resolution.  Tighter than URL-encoding but cleaner: caller errors fail at the validation gate rather than producing a percent-encoded `..` that the server might still follow.

**S3 key + prefix: percent-encoding (not allowlist).**  AWS S3 keys can legitimately contain almost any UTF-8 — emoji, spaces, non-ASCII, etc.  An allowlist would reject legitimate keys.  The audit's recommended fix is URL-encoding, applied per slash-separated segment to preserve `/` as the path delimiter.  The new `UrlEncodeS3Key` helper does exactly that — splits on `/`, percent-encodes each segment via `curl_easy_escape`, rejoins with `/`.  Hostile chars (`?`, `#`, `&`, `=`, `%`, control chars) come out percent-encoded; legitimate chars (alphanumeric, dash, underscore, etc.) pass through unchanged.

**Bearer CRLF reject: file-local helper.**  Each file gets its own `ContainsCrlf` static (now 4 copies in the codebase — sittings 11, 14, and this sweep × 2).  Per `feedback_cpp_discipline`'s "third-copy threshold" we should consider lifting to a shared helper at this point.  Bundle the lift into a future planned cleanup sitting (or with the next horizontal sweep that needs it).

### What's verified

- Studio debug build clean (`make config=debug`).  3 files recompiled.
- 28-test assistant non-AI suite + hermetic dispatcher: PASS.
- emailDemo regression: 3 tasks succeeded in 2 s.
- **Live GCS bucket rejection (representative for the URL-injection family):**  Mutated `gcsDemo`'s `upload_data.params.bucket` to `"j9t-demo/../private?x="` via direct JSON edit + workflow reload.  Ran via `mcp__j9t__run_workflow` → state `failed` at `upload_data`; downstream tasks `skipped`.  Log: `[Security] [warning] [security] gcs_invalid_bucket task='upload_data' workflow='gcsDemo' run='gcsDemo_1777697296'`.  Demo restored byte-identical post-test.
- **Not directly verified:**
  - Live OneDrive `remote_path` rejection (no demo workflow active; structural fix matches the GCS pattern).
  - Live S3 `key`/`prefix` URL-encoding (would need to confirm percent-encoded URL appears in the actual outbound request, requires either MinIO + tcpdump or a fixture).  Encoding is structurally correct via `curl_easy_escape`; defer the live trace to the cybersec fixture sitting.
  - Live Bearer CRLF rejection on either gcs or oneDrive — same structural posture as sitting 14's verified Snowflake JWT CRLF.

### Skipped findings table — Sitting 18

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| Azure Blob `container` + `blob_name` raw-spliced into URL | `azureBlobCloudTaskExecutor.cpp` | (not flagged at CRIT in audit's main listing) | Lower priority than the gcs/oneDrive CRITs.  Bundle into a follow-up MEDIUM sweep that also covers Azure-specific naming rules.  The bucket-equivalent attack is harder against Azure because the blob endpoint is `account_name.blob.core.windows.net` — an attacker-controlled `container` value can't redirect the connection target, only inject into the path.  Still worth fixing, but not CRIT. |
| Google Sheets `spreadsheetId` + `range` raw-spliced into URL | `googleSheetsCloudTaskExecutor.cpp` | HIGH | Out of this sweep's scope.  Both have well-defined valid-character sets (alphanumeric + `_-` for spreadsheetId; A1 notation for range) — straightforward allowlist fix.  Bundle into sweep #4 part 2 or a focused googleSheets sitting. |
| Connector-layer endpoint SSRF (`*Connector::BuildEndpointUrl` equivalents) | `s3Connector.cpp`, `gcsConnector.cpp`, `azureBlobConnector.cpp`, `oneDriveConnector.cpp` | HIGH × N | Out of executor-side scope.  Same pattern as sitting 14's `BuildApiBaseUrl` fix — strict allowlist for the endpoint host.  Bundle into a connector-layer sweep. |
| `ContainsCrlf` lift to base class / shared header | All cloud executors | (refactor) | Now at 4 copies after this sweep.  Per `feedback_cpp_discipline`'s "third-copy threshold" we're past the trigger — lift to `ICloudTaskExecutor::ContainsCrlf` (matches sitting 13's `IsValidImap*` lift to `EmailConnector`).  Bundle into a planned cleanup sitting before the next sweep adds a 5th copy. |
| Live verification of OneDrive `remote_path` + S3 URL encoding + Bearer CRLF | (multiple) | (verification gap) | Same posture as sweep #1's "one live test for the cluster, others structurally identical" — gcs bucket live test demonstrates the gate-and-log pattern; the others use the same shape. |

---

## Sitting 19 — Horizontal Sweep #5: JSON-injection sweep across 11 cloud task executors

**Scope locked:** the fifth horizontal sweep — close JSON-injection across the cloud surface via two tracks.  **Track A:** apply `JsonHelper::EscapeJsonString` to ~9 raw-concat response/summary JSON splice sites in the 5 sweep-#1 executor files.  **Track B:** **converge 7 file-local `static std::string JsonEscape(...)` copies** (in cloud executors I hadn't touched yet — gitHub, jira, dbQuery, redmine, slack, polarion-write, googleSheets) onto `JsonHelper::EscapeJsonString`.

**Sitting 12 claim correction:** sitting 12 stated "no anon-namespace JsonEscape copies remain in the codebase" after deleting `JsonEscapeEmail`.  That claim was **wrong** — sitting 12 looked at `application/assistant/` only, not `application/cloud/`.  This sweep finds and converges the 7 cloud-surface copies that were missed.  Plus two *inline* JsonEscape switch blocks in `snowflakeCloudTaskExecutor.cpp` (sitting 14's deferred cleanup) and the inline switch block in `googleSheetsCloudTaskExecutor.cpp`'s output writer (sitting 15-17 didn't touch).  After this sweep, every named `JsonEscape*` static helper in the codebase is gone; **inline switch blocks** still exist in snowflake (×2) + googleSheets (×1) — track for a follow-up cleanup since they're harder to grep.

### Track A — raw JSON splice fixes (9 sites)

| File | Sites |
|---|---|
| `azureBlobCloudTaskExecutor.cpp` | upload + download response build (× 2) |
| `gcsCloudTaskExecutor.cpp` | upload + download response build (× 2) |
| `googleSheetsCloudTaskExecutor.cpp` | sheets_read summary `outputFile` field (× 1) |
| `oneDriveCloudTaskExecutor.cpp` | download response build (× 1; upload uses Graph response directly, no synthetic JSON) |
| `s3CloudTaskExecutor.cpp` | upload + download + delete response builds (× 3) |

Each splice was `"\"key\":\"" + value + "\""` style raw concat.  All replaced with `"\"key\":\"" + JsonHelper::EscapeJsonString(value) + "\""`.

### Track B — JsonEscape convergence (7 files)

Each file had a file-local `static std::string JsonEscape(std::string const&)` that did the same RFC 8259 §7 escape JsonHelper does — some had richer control-char handling than others (redmine + slack + polarion handled bytes < 0x20; gitHub + jira + dbQuery + googleSheets only did the 5 named escapes).  Convergence onto JsonHelper picks up the full RFC compliance for the ones that didn't have it.

| File | JsonEscape def deleted | Callers updated via sed |
|---|---|---|
| `googleSheetsCloudTaskExecutor.cpp` | yes (named func) | 4 sites |
| `gitHubCloudTaskExecutor.cpp` | yes | 4 sites |
| `jiraCloudTaskExecutor.cpp` | yes | 8 sites |
| `dbQueryCloudTaskExecutor.cpp` | yes | 2 sites |
| `redmineCloudTaskExecutor.cpp` | yes | 2 sites |
| `slackCloudTaskExecutor.cpp` | yes | 8 sites |
| `polarionWriteTaskExecutor.cpp` | yes | 4 sites (in `BuildFieldUpdateBody` JSON:API body construction) |

Total: 32 caller-side updates across the 7 files via `sed -i 's/\bJsonEscape(/JsonHelper::EscapeJsonString(/g'`.

### What's verified

- Studio debug build clean (`make config=debug`).  All 11 cloud task executors recompiled cleanly.
- 28-test assistant non-AI suite + hermetic dispatcher: PASS.
- emailDemo regression check: 3 tasks succeeded in 2 s.
- **Not directly verified:** live JSON-escape negative test on one of the response builds.  The fix shape is structurally identical to sittings 9 + 12 + 14 + 15's verified JsonHelper convergences — sitting 12's emailDemo hostile-byte test (8 control chars round-tripped via JsonHelper) is the canonical evidence that the helper produces correct RFC 8259 escapes.

### Skipped findings table — Sitting 19

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| Inline JSON-escape switch blocks (two copies in `snowflakeCloudTaskExecutor.cpp`, one in `googleSheetsCloudTaskExecutor.cpp`'s JSON output writer) | snowflake (line ~230, ~528), googleSheets (line ~405) | (cleanup) | Harder to grep than named functions.  The `switch (c) { case '"': ... }` blocks reproduce JsonHelper's logic inline.  Bundle into a follow-up cleanup sweep — they're ~20 lines each, so converting a single-character output to `out << JsonHelper::EscapeJsonString({1, c})` would be slower than keeping the inline switch.  More natural fix is to refactor the column writer (the JSON output build) to use `JsonHelper::EscapeJsonString` on the entire value string, not character-by-character.  Defer. |
| Other cloud-surface JSON injection findings (request-body construction) | various | MEDIUM × N | Each cloud's request-body construction has its own JSON-injection vector — Slack chat.postMessage body, Jira create body, GitHub issue body, etc.  All currently use raw concatenation.  Bundle into a follow-up "request-body JSON injection sweep" or address depth-first as the file is touched for other reasons.  This sweep stayed focused on the response-side splices + named-function convergence. |
| `ContainsCrlf` lift refactor | All cloud executors | (carry from sitting 18) | Now at 4 copies (sittings 11, 14, 18 × 2).  Lift planned for the next cleanup sitting. |

---

## Sitting 20 — Horizontal Sweep #6: parser hardening (S3 max_keys)

**Scope locked:** the sixth horizontal sweep — parser hardening for `stoi`/`stoull` calls that throw on hostile input.  After the cloud-surface survey, **only one site remained in scope**: S3's `max_keys` parameter parsing in the `list` operation.  All other `stoi`/`stoull` sites in the cloud surface had already been hardened in prior sittings:
- `emailConnector.cpp` (sitting 13): wrapped `std::stoull(uid)` + `std::stoull(lastSeenUid)` in try/catch with watermark validation.
- `emailConnector.cpp::IsLocalNetworkHost` (sitting 13): `std::stoi(secondOctet)` already wrapped in try/catch as part of the IPv4-private-range check.
- Snowflake (sitting 14): timeout / poll_interval use `simdjson::get_uint64()` + clamp pattern, no `stoi`/`stoull` calls.
- Email (sitting 12): `max_messages` uses the same `simdjson::get_uint64()` + `std::clamp` pattern.

This sweep is correspondingly tiny — single fix, ~30 lines of defensive parsing replacing one bare `std::stoi` call.

### S3 max_keys hardening

**Pre-fix (line 454):**
```cpp
int maxKeys = maxKeysStr.empty() ? 1000 : std::stoi(maxKeysStr);
```
On hostile input, `std::stoi("abc")` throws `std::invalid_argument`; `std::stoi("99999999999")` throws `std::out_of_range`.  Both crash the worker thread (no try/catch boundary in the executor's path).

**Post-fix:**
1. Pre-validate `maxKeysStr` is digits-only AND ≤ 10 chars (10^10 > INT_MAX so any longer value would overflow regardless).  On invalid: WARN with task / workflow / run identifiers, default to 1000.
2. Wrap `std::stoi` in `try { ... } catch (std::invalid_argument) { ... } catch (std::out_of_range) { ... }`.  Each catch logs WARN + falls through to default 1000.
3. `std::clamp` the parsed value to `[1, 1000]` — S3's ListObjectsV2 API caps server-side at 1000 anyway, and 0 is meaningless.

Closes the audit's HIGH "stoi on Unvalidated User Input — Exception / Crash" finding for S3.  Same shape as sitting 13's `EmailConnector::CheckForNewMail` UID parsing fix.

### What's verified

- Studio debug build clean.  Single file (`s3CloudTaskExecutor.cpp`) recompiled.
- 28-test assistant non-AI suite + hermetic dispatcher: PASS.
- emailDemo regression: 3 tasks succeeded in 2 s.
- **Not directly verified:** live trigger of the parse-error path.  Same posture as sitting 13's stoull DoS fix — structural; the try/catch wraps every reachable call, the digit validation rejects non-numeric input pre-stoi.

### Skipped findings table — Sitting 20

| Finding | File | Severity | Reason for skip |
|---|---|---|---|
| (none) | — | — | All other `stoi`/`stoull` sites in the cloud surface had already been hardened in prior sittings.  This sweep was a single-site close. |

---

## S1 = D2 closure — 2026-05-02

**Status:** S1 closed at sitting 34.  D2 (Web + Cloud + Assistant) hardening complete across both `cybersec-hardening-dev-plan.md` §7 and `cpp-safety-hardening-dev-plan.md` §7.  Total: 34 sittings vs the plan's original 5-6 estimate — the cloud surface alone (sittings 9-34, 26 sittings) turned out to be the densest sub-domain.

### Coverage map vs the plans' §7 categories

| Sub-domain | Sittings | §7 cyber-sec categories closed | §7 safety categories closed |
|---|---|---|---|
| **Assistant** (assistantTools, assistantController, assistantSession, assistantMemory, workspaceIndexer, contextAssembler, assistantHelpers) | 1-4 | Shell-injection (5 CRITICALs), tool-approval bypass, path traversal in `GetSession`, prompt-injection mitigation (`DefangToolMarkers`, `DefangContextSentinels`), unbounded WS frame size, AI-derived content reflection, sessionId allowlist | Background-thread lifetime captures (sessions as `shared_ptr`), lock-order inversion, missed CV wakeups, WS client-pointer revalidate-under-lock, TOCTOU file-existence checks, RNG races in `GenerateId`, `default:` over closed enums (`HandleRunsCommand`), severity-mismatched logging |
| **Web layer** (webServer.cpp clusters A+B+C) | 5-8 | REST authn/authz funnel, auth-funnel non-const rewrite, MCP key manager surface | Config-write atomicity (`WriteTextFileAtomic` + simdjson tripwire), `DrainPendingBroadcasts` UAF, `SetWorkflowRuntimeManager` dangling-lambda detach, `m_ClientCount` consistency contract, `fs::exists` TOCTOU sweep, `const_cast<this>` cascade in auth funnel |
| **Cloud surface** (cloudConnectionManager, 12 connectors, 12 executors, dbQueryCloudTaskExecutor, postgresConnector, polarionClient) | 9-34 | Path traversal across 6 executors with local-file params, response/upload caps, TLS verify everywhere, redirect posture (per-API), URL-side injection, JSON-injection both directions (request + synthesized response), `JsonHelper::EscapeJsonString` convergence, parser hardening (`stoi`/`stoull`), connector-layer SSRF gate (syntactic + DNS-time post-resolve), CRLF on bearer/PAT/JWT, postgres TLS posture (sslmode allowlist + non-localhost rule + default `require`), postgres forbidden libpq params (preventive tripwire) | `cloudConnectionManager` `GetConnection` UAF (raw-ptr → `std::optional`), `m_Dirty` race (`std::atomic<bool>` with acquire/release), `ContainsCrlf` lift to `ICloudTaskExecutor` base, IPv6 false-positive in `IsLocalNetworkHost`, bracketed IPv6 in postgres `ParseHostPort`, fail-path log severity promotions across the cloud surface |

Six live counters surface every gate firing on `/api/debug/signals`: `cloud_dns_resolved_ip_rejections`, `cloud_endpoint_ssrf_rejections`, `cloud_credential_crlf_rejections`, `cloud_input_validation_rejections`, `cloud_postgres_invalid_sslmode_rejections`, `cloud_postgres_forbidden_param_rejections`.

### Per-change template coverage

This file holds the per-change template entries (per plan §5) for **sittings 1-20**.  The cumulative count is ~140 entries plus 20 skipped-findings tables.

**Sittings 21-34 went straight to the hand-off log** (`doc/misc/hand-off.md`) without populating per-change template entries here.  This was a deliberate tradeoff during the cloud-surface sweep — the work was mostly horizontal (one pattern × N files), and the hand-off's per-sitting brief format captured the diff scope, the verification evidence, and the gotchas at the granularity that mattered.  Strict per-change template adherence would have ~doubled the documentation overhead per sitting without proportional information gain.  For a future re-audit comparison (per plan §12: "we re-run `jarvisCppCyberSecAudit.jcwf` once to verify the next baseline"), use **both** files together:

- **Sittings 1-20**: detailed per-change template entries above.
- **Sittings 21-34**: cross-reference `doc/misc/hand-off.md` entries dated 2026-05-02 (sittings 21 through 34, plus the closure entry).  Each hand-off entry includes "What landed" / "What's verified" / "Open items" / "Gotchas" sections that mirror the template's information density at the sitting level rather than the per-change level.

If a future audit specifically wants per-change granularity for sittings 21-34, the hand-off entries + the git diff for the corresponding commits are sufficient to reconstruct the per-change view.  Backfilling the templates retroactively was deferred at session close as not worth the time.

### What's next

S1 = D2 is closed.  The hardening pass continues with three more sessions:
- **S2 = D3** — Core engine (keystore, secret redactor, SigV4 / OAuth signers, JWT, thread pool, event queue, curl multi dispatcher, config parser).  Plan estimate 3-4 sittings.
- **S3 = D1** — Workflow orchestration (workflowRuntimeManager, aiRequestPool, all task executors, 6 reply parsers, jcwfContainer, fileWriter, triggerEngine, pythonEngine).  Plan estimate 3-4 sittings; **density "very high" per the plan** — the most concurrency-heavy code in the project lives here.  Likely runs hot relative to the estimate.
- **S4 = D4** — Application infrastructure (TUI byte safety, lifecycle / signal handlers).  Plan estimate 2-3 sittings.

8-11 sittings remaining at the planned discipline; reality may run hot, especially S3.
