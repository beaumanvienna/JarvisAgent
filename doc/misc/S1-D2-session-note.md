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
