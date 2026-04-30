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
