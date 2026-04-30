# Session Hand-off Log

End-of-session brief for next-session-Claude.  Newest entry on top.

**Convention** — when wrapping up a session, prepend a new entry under a date header (`## YYYY-MM-DD → next session`) covering:

- **What landed** — the major themes shipped this session (committed or working-tree).
- **What's verified** — tests run, results, what was deliberately not re-tested.
- **Open items / next-session candidates** — the natural follow-ups that didn't make today's cut.
- **Gotchas** — load-bearing knowledge that survives past today (config-schema breaks, debug-only paths, helpers to reuse).

Keep entries self-contained — a fresh-context Claude should be able to read just the latest entry and pick up cleanly.  Cross-reference into `todo.md` / `doc/misc/*-dev-plan.md` rather than duplicating their content.

**Scope discipline (this file is committed to git):** entries describe **project-state changes only**.  Per-machine tooling setup, OS specifics, account info, env-var values, dev-machine paths, personal preferences — all go into auto-memory (`~/.claude/projects/.../memory/`), **never** into this log.  See `feedback_no_private_info_in_repo` and `feedback_jc_dev_env_vars`.  If an entry needs a tooling decision as context, write a one-line cross-reference ("see `feedback_clangd_lsp_for_navigation.md`") rather than inlining the setup details.

---

## 2026-04-29 (S1 sitting 4) → next session

S1=D2 sitting 4.  Theme: close the four remaining D2 assistant-side audit clusters — `assistantSession.{h,cpp}`, `assistantMemory.{h,cpp}`, `workspaceIndexer.{h,cpp}`, `contextAssembler.{h,cpp}` — plus the long-deferred `JsonEscape` four-copy convergence and the `RandomHex` / `IsValidOpaqueId` extraction that sitting 3 explicitly tracked-but-skipped.  Every HIGH cyber-sec/safety finding in those four files plus the load-bearing MEDIUMs are closed.  Boundary at sitting-end: D2 cyber-sec audit is now complete for the **assistant subsystem** (sittings 1–4 covered `assistantTools.{cpp,h}` + `assistantController.{cpp,h}` + the four files above).  Sitting 5 starts on the rest of D2 — the web/cloud surface — or on the cross-component refactors (`JoinFinishedThreads → engine ThreadPool`, broader thread-safety contract audit) if JC prefers to clean up the assistant-internal debt first.

### What landed

1. **`engine/json/jsonHelper.{h,cpp}` rewrite + 3 broken `JsonEscape` copies retired.**  The existing `JsonHelper::SanitizeForJson` had a literal 0x0C (form-feed) `case` label that *dropped* the byte, plus no escaping for the rest of the 0x00–0x1F control range — broken since landed.  Rewritten as `static std::string EscapeJsonString(std::string_view)` with proper RFC 8259 §7 escaping (the four shorthand cases plus `\u00XX` for every other control byte); instance `SanitizeForJson` now delegates so 10+ existing callers in `aiTranscript.cpp` / `requestBuilder.cpp` upgrade transparently.  This is a positive side-effect for every outbound AI request body and persisted transcript.  `assistantSession.cpp::JsonEscape`, `assistantMemory.cpp::JsonEscapeMem`, `workspaceIndexer.cpp::JsonEscapeIdx` deleted; all three files now route through the central helper.  `assistantTools.cpp` and `assistantController.cpp` keep their anon-namespace copies (both already RFC-correct after sittings 2–3); migrating them is a mechanical sweep over ~50 QueueMessage call sites tracked as a follow-up.
2. **New `application/assistant/assistantHelpers.{h,cpp}`** with `RandomHex(numBytes)` (RAND_bytes-backed, ERROR-logged on failure, fail-closed empty return) and `IsValidOpaqueId(s)` (strict `[A-Za-z0-9_-]{1,128}` allowlist).  `assistantController.cpp` drops both local copies (was `RandomHex` in file-scope anon namespace + `IsValidSessionId` in `namespace AIAssistant { namespace { ... } }`).  Sitting 3 had landed at the third-copy threshold; sitting 4 needed both at three new sites, so convergence happened here.
3. **`assistantSession.{h,cpp}` reworked.**  HIGH path-traversal in resume ctor → `IsValidOpaqueId` validation as defense-in-depth alongside the controller-layer gate.  HIGH weak/predictable session ID → `"sess_" + RandomHex(16)` (128-bit entropy, no timestamp, no counter; fixes the same-millisecond-restart corruption bug as a side effect).  HIGH `AppendTurn` silent failures → renamed `AppendTurnLocked`, write-then-commit ordering, explicit flush + `good()` check, sticky `m_FileBroken`, `[[nodiscard]] bool` propagated through `AddUserMessage`/`AddAssistantMessage`; 6 controller call sites updated to `(void)` with the contract documented at the first site.  HIGH lock-from-ctor → `LoadFromFileLocked` (no lock acquisition, contract documented).  HIGH `ListSessions` TOCTOU → drop `exists` pre-check, distinguish missing-vs-permission-error.  MEDIUM unbounded JSONL load → `kMaxTurnsPerSession=10000`, `kMaxLineBytes=1 MiB`, `kMaxTurnTextBytes=256 KiB`.  MEDIUM `ExtractJsonString` missing `\uXXXX` decode → home-built parser deleted, replaced with simdjson per memory `feedback_simdjson_only`.  MEDIUM session-ID logging → new `LogSafeSessionId` truncates to 8 hex chars at every log site.  LOW POSIX file permissions → `fs::permissions(path, owner_read | owner_write, replace)` after first write (best-effort, ignored on Windows).  LOW role-validation exhaustiveness → load-time filter rejects roles outside `{user, assistant, system}`.
4. **`assistantMemory.{h,cpp}` reworked.**  HIGH RNG race → `mt19937_64` deleted, `GenerateId` returns `"mem_" + RandomHex(16)` with static-mutex-guarded counter fallback.  HIGH `GetRelevant` lock-pattern → extracted `RecallLocked`; public `GetRelevant` acquires the mutex once and trims under the same lock (atomicity gap closed, latent deadlock removed).  HIGH `SaveToDisk` silent failures → renamed `SaveToDiskLocked`, returns bool, ERROR-level logs, sticky `m_FileBroken`, in-memory rollback on persistence failure (so disk and RAM stay consistent).  HIGH `LoadFromDisk` TOCTOU + lock-from-ctor → drop `exists` pre-check, rename to `LoadFromDiskLocked`, no lock.  MEDIUM unbounded entries/fields → `kMaxEntries=10000`, `kMaxKeyBytes=256`, `kMaxValueBytes=64 KiB`, `kMaxTagBytes=256`, `kMaxTagsPerEntry=32` enforced at both load and Save.  MEDIUM raw pointers in `Recall` → `Scored { score, size_t idx }` instead.  MEDIUM control-char JSON escaping → routed through `JsonHelper::EscapeJsonString`.  MEDIUM logging severity → all persistence-failure paths at ERROR.  MEDIUM `[[nodiscard]]` on `Save`/`Delete`/`Recall`/`ListAll`/`GetRelevant`/`Size` (no caller warnings since `assistantTools.cpp` already captured all returns).  LOW key logging redaction → `LogSafeKey` strips control bytes, caps at 64 chars, appends original length.
5. **`workspaceIndexer.{h,cpp}` reworked.**  HIGH `ReadFileContent` path traversal → `static` removed, constructor captures `m_WorkspaceRoot = fs::weakly_canonical(fs::current_path())` at startup, new private `ResolveAndConfine` does `weakly_canonical(root / raw)` + `lexically_relative(root)` containment check; symlinks pointing out of tree caught at resolution; absolute paths rejected.  `assistantTools.cpp::ExecGetFileSummary` updated from static `WorkspaceIndexer::ReadFileContent(...)` to `m_WorkspaceIndexer->ReadFileContent(...)` (ToolRegistry already holds the pointer).  HIGH untrusted index data → every `relativePath` parsed from `file_index.jsonl` re-runs through `ResolveAndConfine`; rejects log `[security] indexer_index_path_escape len=...`; bonus `kMaxIndexEntries=100000` cap during load.  HIGH `LastScanTime` no lock → acquire mutex.  HIGH `ScanDirectory` ec ignored → check + skip on filesystem-call failure.  MEDIUM `ReadFileContent` truncation logic → save original size before clamping, single `fs::file_size` call.  MEDIUM `SaveIndex` silent errors → check `ofs.good()` after flush, ERROR on failure; `LoadIndex` distinguishes missing-vs-unreadable.  MEDIUM control-char escaping → `JsonHelper::EscapeJsonString`.  MEDIUM raw pointers in `GetRelevantFiles` → `Scored { score, size_t idx }`.  LOW `kMaxSummaryBytes=8 KiB` cap on `SetFileSummary`.  LOW `IsIndexableExtension` → `static`.  Header now documents the workspace-root snapshot semantics (cwd changes after construction don't widen access) and the orthogonality of this gate vs. `ToolRegistry::IsPathDenied` (containment vs. deny-list).
6. **`contextAssembler.{h,cpp}` reworked.**  MEDIUM prompt injection via `turn.text` → new `static DefangContextSentinels(text)` that (a) calls `ToolRegistry::DefangToolMarkers` for `<tool_call>`/`</tool_call>`/`<tool_result>`/`</tool_result>` and (b) replaces any run of 3+ `=` with the same number of U+2550 (BOX DRAWINGS DOUBLE HORIZONTAL); applied to every prior turn's text and to the new userMessage.  MEDIUM unbounded context → `kMaxUserMessageBytes=64 KiB`, `kMaxTurnTextBytes=32 KiB`, `kMaxConversationContextBytes=128 KiB`, `kMaxToolDescriptionsBytes=64 KiB`; per-turn truncation + total-context truncation; `userMessage` clamped before placing in `prompt.prob`.  LOW `userMessage` defang → covered by the same pass.

22 per-change template entries (including the two PRE-STEP entries) appended to `doc/misc/S1-D2-session-note.md` under "Sitting 4".  Skipped-findings table records the 12 deferred items with reasons.

### What's verified

- Studio debug build clean (`make config=debug` after `premake5 gmake` to pick up the new `assistantHelpers.cpp` — important: see Gotchas).
- **28-test assistant non-AI suite: PASS** end-to-end against the new binary in 2.1s.  Covers session create/resume/list, history replay, all 11 slash commands, completion, protocol error paths.  Implicitly exercises the rewritten JSONL save/load round-trip, the new `IsValidOpaqueId` gate, the simdjson-based session parser, the `LogSafeSessionId` truncation in INFO logs, the WorkspaceIndexer `ReadFileContent` instance call.
- `python3 test/dispatch/test_testinterface_hermetic.py`: PASS — request-body path through the rewritten `JsonHelper::SanitizeForJson` is unbroken.
- MCP sidecar verified live: `mcp__j9t__whoami` (admin), `mcp__j9t__debug_signals` (`keys_unlocked=true`, `uptime_seconds=754`, `workflow_runs_total_completed=2`).
- **Not directly verified:** the live `<tool_call>` / `=== ... ===` prompt-injection defang (covered structurally by the unit-test-equivalent build but no `--with-ai` runtime smoke); `ReadFileContent` workspace-root rejection on a real symlink-out-of-tree (would need a test fixture); `MemoryStore::Save` rollback path under simulated disk-full (would need fault injection).  These are the predictable carry-overs.

### Open items / next-session candidates

- **Sitting 5 candidates:**
  - **Cross-component refactors that have been tracked since sitting 3:** `JoinFinishedThreads` → engine `ThreadPool` (memory `feedback_no_jthread_use_threadpool`); thread-safety contract audit on `m_ToolRegistry`/`m_MemoryStore`/`m_WorkspaceIndexer` (background lambda + main thread accessing concurrently with no documented contract); `QueueMessage` drain CV/timer (responses produced after the last user message currently sit until the next `OnMessage`).
  - **Migrate the remaining `JsonEscape` copies** (`assistantTools.cpp` + `assistantController.cpp`) to `JsonHelper::EscapeJsonString` — both correct today but mechanical-sweep convergence eliminates the last two duplicates inside the assistant subsystem.
  - **D2 web/cloud surface:** the audit findings on `webServer.cpp`, the cloud task executors (`azureBlobCloudTaskExecutor`, `gcsCloudTaskExecutor`, etc.), the email/IMAP code path, the GitHub/Snowflake/Redmine integrations.  Densest CRITICAL surface after the assistant subsystem.
  - **Encrypted-at-rest memory store** (skipped MEDIUM in sitting 4) — architectural design, not a single-sitting fix.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **Premake regen is required when adding new `.cpp` files.**  Sitting 4 added `application/assistant/assistantHelpers.cpp`.  The premake glob `application/assistant/**` resolves at `premake5 gmake` time, not at `make` time.  Build will link-fail with `undefined reference` until you re-run `premake5 gmake`.  Memory `feedback_rebuild_after_premake_clean` covers a related case; this is a complementary rule.
- **`JsonHelper::EscapeJsonString` is now the canonical RFC 8259 escape.**  `JsonHelper::SanitizeForJson` (instance method) still works — kept as a thin delegator so the 10+ existing call sites in `aiTranscript.cpp` / `requestBuilder.cpp` upgrade transparently.  New code should call `JsonHelper::EscapeJsonString(x)` static directly.  The previous instance-method version had a literal 0x0C `case` label that silently dropped form-feed bytes — that bug is now closed.
- **`AssistantSession::AddUserMessage` / `AddAssistantMessage` are `[[nodiscard]] bool`** signalling whether the turn was both pushed in-memory AND durably written to disk.  All 6 controller call sites currently `(void)` the return because the session itself emits ERROR-level logs on persistence failure with a sid_prefix the dashboard's run analyzer surfaces.  A future caller that wants to reflect persistence state to the user (e.g., a "session degraded" badge in the dashboard) can switch to checking the bool.
- **`AssistantSession`-style sticky `m_FileBroken` flag** is now the project-wide pattern for "memory and disk diverged, refuse further writes": same flag in `MemoryStore` and same shape in (TBD) future stores.  When in degraded state, public mutators fail-fast at the head of the function with an ERROR log; the only recovery is operator intervention (delete or fix the file, restart j9t).
- **`WorkspaceIndexer::ReadFileContent` is no longer static** and now anchors against the workspace root captured at construction.  Caller is `ToolRegistry::ExecGetFileSummary` via `m_WorkspaceIndexer->ReadFileContent(filePath, 32768)`.  This is workspace-confinement only; sensitive-file deny-listing remains `ToolRegistry::IsPathDenied` and is applied at the same call site.  Future tools that read user-influenced relative paths should follow the same orthogonal pattern: workspace gate + deny-list gate.
- **`ContextAssembler::DefangContextSentinels` is the canonical pattern for AI-context-boundary defense at the inbound side.**  Sitting 3 added `ToolRegistry::DefangToolMarkers` for the `<tool_*>` markers; sitting 4 wraps that plus `===` collapse into a single helper applied to every user-origin turn and the new user message.  When a future component places attacker-influenced text into the AI prompt (e.g., a recalled memory, a recalled file summary), it should call this helper first.

### Doc sweep (post-sitting)

After sitting 4 closed: `engine/json/json.md` §4 rewritten for the new `JsonHelper::EscapeJsonString` static + retained `SanitizeForJson` instance delegator + RFC 8259 control-byte escape; `application/assistant/README.md` refreshed end-to-end so it reflects cumulative sittings 1–4 state (storage formats, all backend modules, expanded safety table from ~17 to ~22 rows, new `assistantHelpers` subsection, files list).  `doc/architecture.md` scanned, no edit needed (it stays at component-name level).  Doc-hygiene principle saved as `feedback_doc_routing` memory: each fact has one home, tracked docs cross-ref but never inline build/run/launcher mechanics, and never name memory files.  A repo-wide audit confirmed near-zero existing duplications.

S1=D2 sitting 3.  Theme: close the controller-layer security funnel in `application/assistant/assistantController.{h,cpp}` — every CRITICAL and HIGH cyber-sec finding plus the load-bearing concurrency-safety HIGHs that share the same code regions.  Plus a pre-step audit-trace sweep across sittings 1+2+3 driven by JC's directive that audit citations don't belong in source code.

### What landed

1. **Audit-trace sweep across sittings 1+2+3 + new memory.**  Stripped every audit citation, severity tag (`[HIGH]`, `[CRITICAL]`, `Cyber-sec §02`), session-tracking ref, and inline "memory `feedback_X`" note from the working-tree sitting changes.  Where the invariant the comment was protecting is non-obvious, kept it as plain English; otherwise deleted.  `LOG_SECURITY_*` runtime tags stay (operational signals, not change-trace).  New memory `feedback_no_audit_traces_in_code.md` records the rule.  Mechanical scan via `grep -nE '§|\[HIGH\]|cyber-sec|audit|...'` returns no matches in the affected files; build clean.
2. **`HandleLogCommand` rewritten on `std::ifstream` seek-tail** — pure C++ tail reader replaces the popen + `tail` + `WrapForBash` shell composition.  Closes the cyber-sec CRITICAL (latent shell-injection if the path were ever made configurable), the safety HIGH (popen + buffered stream silently swallowing errors), and the LOW path-disclosure (the user-visible error string no longer echoes the absolute path; that goes only to the server-side `LOG_APP_INFO` for operator triage).  `WrapForBash` and the `popen`/`pclose` Windows aliases deleted (now unused).
3. **`GetSession` strict allowlist + canonical-path containment** — sessionId regex-validated as `[A-Za-z0-9_-]{1,128}` before any path construction; resolved file additionally canonicalised under the sessions dir as defense-in-depth against symlinks.  Closes the CRITICAL path-traversal.  Logs `[security] assistant_session_invalid_id length=…` (length only) and `[security] assistant_session_path_escape sid_len=…` on rejection — never the value.
4. **Approvals bound to originating connection + cryptographic requestId** — `PendingApproval` grows `originConn` (pointer identity only, never dereferenced); `RunAiCallAsync` threads the connection through via lambda capture; `HandleApprovalResponse(conn, requestId, approved)` rejects any conn-mismatch with `LOG_SECURITY_WARN`.  `m_NextApprovalSeq` deleted; `requestId = "apr_" + RandomHex(16)` (128-bit `RAND_bytes` hex).  `OnClose` calls new `CancelApprovalsForConnection(&conn)` which fail-closes any approvals owned by the disconnecting client (otherwise the AI loop hangs on the 60 s timeout, *and* a future connection that reuses the pointer address could match by identity).  `Shutdown` notify_all moved out of `m_ApprovalsMutex` (snapshot under lock, notify outside).  Closes the HIGH approval-bypass + the MEDIUM sequential-requestId.
5. **WS frame size cap + maxEntries clamp** — `OnMessage` rejects frames > 64 KB before constructing `simdjson::padded_string` (logs `[security] assistant_ws_frame_too_large bytes=…`).  `get_history` clamps `maxEntries` with `std::clamp<int64_t>(val, 1, 500)` before the cast (a negative value previously caused an unbounded loop).
6. **`m_Sessions` `unique_ptr` → `shared_ptr<AssistantSession>`** — the background AI lambda captures a shared_ptr so the session stays alive across the multi-step tool loop regardless of `m_Sessions` evictions or `Shutdown` ordering.  All 10 callers updated.  `HandleListSessions` and `HandleCompletionRequest` now snapshot under `m_SessionsMutex` then iterate outside the lock — closes both the lock-order TOCTOU and the controller-mutex-while-session-side-mutex inversion.  `OnOpen` / `OnClose` read `m_Clients.size()` inside the lock scope (data-race fix).
7. **`DrainPendingMessages` per-client revalidate under lock** before each `send_text` — same pattern `WebServer::DrainPendingBroadcasts` already uses for the `/ws` broadcast loop.  Narrows the use-after-free window to Crow's deferred-destruction semantics, matching the rest of the codebase.
8. **`DefangToolMarkers` promoted to public `ToolRegistry::DefangToolMarkers`** — was private to `assistantTools.cpp` after sitting 2; this sitting added a second use site (`RunAiCallAsync` reflecting `result.output` into the `<tool_result>...</tool_result>` block).  Per memory `feedback_cpp_discipline` "refactor to one helper before adding a third copy", extracted before the third site.  All 4 prior call sites in `assistantTools.cpp` resolve via unqualified name lookup (member functions of the same class).  Closes the MEDIUM tool-result XML injection.
9. **`HandleRunsCommand` `default:` arm removed** — switch over `WorkflowRunState` is now closed; `-Wswitch` will catch any future enumerator addition.
10. **`QueueMessage` capped at 10k + log redaction + `WriteFile` flush check** — pending-queue overflow logs `LOG_APP_ERROR` and drops; memory recall + approval logs report counts/lengths/prefixes only (no message text, no memory values, no full requestId); `WriteFile` flushes explicitly + returns generic error string (no path leak).

Ten per-change template entries for the above plus two PRE-step entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 3".  Skipped-findings table records the deferred items.

### What's verified

- Studio debug build clean (`make config=debug`).
- **28-test assistant non-AI suite: PASS** end-to-end against the new binary (`python3 test/assistant/test_assistant.py` → 28 passed, 0 failed in 2.1 s).  Covers every controller protocol path including the rewritten `/log` slash command, the now-clamped `get_history`, the now-hardened `approval_response`, and all 11 slash commands.
- `python3 test/dispatch/test_testinterface_hermetic.py`: PASS — adjacent dispatcher path unbroken.
- **Not directly verified:** the AI-driven runtime path that exercises `RequestToolApproval` end-to-end with the connection-binding check, the WS frame-size rejection, and the tool-result defang on real tool stdout — these need either `--with-ai` or a manual dashboard chat session.  JC to drive that pre-commit if appetite allows.

### Open items / next-session candidates

- **Sitting 4** is now the natural follow-up cluster: `assistantSession.h` (HIGH path traversal in ctor + HIGH weak random session ID + MEDIUM/LOW data integrity), `assistantMemory.h` (HIGH RNG race + HIGH lock inversion + MEDIUM JSON-escape control-char gap + MEDIUM unbounded `m_Entries`), `workspaceIndexer.h::ReadFileContent` (HIGH path traversal — pairs with the rewritten `IsPathDenied`), `contextAssembler.h` (MEDIUM/LOW prompt injection).
- **Cross-component refactors deferred from sitting 3** (better in their own sittings):
  - `JoinFinishedThreads` → engine `ThreadPool` (memory `feedback_no_jthread_use_threadpool`).
  - `m_ToolRegistry` / `m_MemoryStore` / `m_WorkspaceIndexer` thread-safety contract audit (background lambda accesses these concurrently with no documented contract).
  - `QueueMessage` drain CV/timer (responses produced after the last user message currently sit until the next `OnMessage`).
  - `JsonEscape` triple-copy + new `RandomHex` triple-copy convergence — both flagged by `feedback_cpp_discipline`'s "third copy" rule; tracked but not bundled.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **Audit citations don't belong in source code.**  Memory `feedback_no_audit_traces_in_code` makes this explicit.  Code comments must not cite audit findings, severity tags, sitting numbers, or any session-tracking artifact — the why must stand on its own.  The change record + finding-citation lives in the session note (`doc/misc/S1-D2-session-note.md` etc.); the code's job is to read coherently for a fresh reader who doesn't care which sitting landed which line.  `LOG_SECURITY_*` runtime tags stay — those are operational signals, not change-trace.  The line: tags consumed at runtime stay; tags consumed only by the human reading the diff today don't.
- **Keystore unlock is explicit, not env-driven.**  Even with `JARVIS_MASTER_PASSWORD` exported, the binary needs `POST /api/settings/keys/unlock` with `{"master_password": "..."}` before MCP-key auth on REST/WS will succeed.  A fresh-start log will show `[Application] [warning] Blocked workflow run '...': contains ai_call tasks but no AI providers are configured. Unlock the key store via POST /api/settings/keys/unlock` if you forgot.
- **`DefangToolMarkers` is now `ToolRegistry::DefangToolMarkers` (public static)** — apply at every site where externally-sourced text re-enters the AI's view as tool-result content.  `assistantController.cpp::RunAiCallAsync` is the second site; the four prior sites in `assistantTools.cpp` resolve via unqualified name lookup.
- **`PendingApproval::originConn` is identity-only — never dereference it.**  The pointer is stored by the WS-handler thread, compared by the WS-handler thread (after `&conn` is also alive), and cleared by `OnClose → CancelApprovalsForConnection`.  Any code that wants to treat originConn as a live pointer is wrong.
- **Sessions are now `shared_ptr<AssistantSession>`.**  Background AI threads should declare `std::shared_ptr<AssistantSession> session = GetSession(sid);` to keep the session alive across blocking tool calls.  Don't store raw `AssistantSession*` from `GetSession()` anywhere that outlives the immediate handler.

---

## 2026-04-29 (S1 sitting 2) → next session

S1=D2 sitting 2.  Theme: close out `application/assistant/assistantTools.cpp` HIGH/MEDIUM findings — the canonical-path / external-content reflection class — plus a pre-work test-harness fix that pays for itself this sitting.  Sitting 3 starts on `assistantController.h` from a clean file.

### What landed

1. **Test harness TLS+self-signed+auth gap closed.**  `test/assistant/test_assistant.py` now defaults to `wss://localhost:8443/ws/assistant`, accepts `--token` (or `J9T_TOKEN` env-var) and passes it as `Authorization: Bearer ...` on the WS handshake, disables cert verification on `wss://`, and serializes every `ws.send()` / `ws.recv()` site behind a `_ws_io_lock` (websocket-client 1.7.0's WebSocket isn't thread-safe under TLS+concurrent send/recv — the ping-thread + main-thread race that previously dropped the connection within ~40 ms).  Result: **28 non-AI tests now pass**, previously zero ran since the j9t HTTPS migration.
2. **`IsPathDenied` rewritten** on `fs::weakly_canonical(projectRoot / path)` — resolves symlinks in any existing prefix, defends against the "safe.txt → /etc/passwd" exfiltration the audit caught.  Adds project-root-confinement (paths that resolve outside project root denied), case-folded filename + extension comparison, and `.bak`/`.tmp` extension denies plus per-base sibling filenames (`config.json.bak`, `keys.json.bak`, etc.) — closes the "ExecWriteFile/EditFile leak via predictable backup path" HIGH.  Fail-closed on any resolution error.
3. **`ExecGetFileSummary` now calls `IsPathDenied`** before reading + forwarding bytes to the external AI provider — closes the audit's MEDIUM/exfiltration finding.  Applied before the cached-summary check so a stale cache pre-dating the deny rule doesn't surface a denied file's summary either.
4. **`ExecJcwfWriteScript` path validation rewritten** — absolute-path reject + `fs::weakly_canonical(projectRoot / path)` + `lexically_relative(scriptsRoot)` empty-or-`..`-prefix reject + `IsPathDenied` call.  Closes the "checks `scripts/` prefix only" HIGH.
5. **`ExecJcwfGenerate` JSON-escapes `workflowId`** — new `JsonEscape` helper in the file's anonymous namespace (RFC 8259 string-content; handles `"`, `\\`, `\\n`, `\\r`, `\\t`, control chars via `\\u%04x`).  Closes the "raw string-concat into global.json" HIGH.  Note: this is now the third `JsonEscape` copy in the codebase (assistantSession.cpp + workspaceIndexer.cpp + this); convergence into `engine/json/jsonHelper.h` tracked as a follow-up.
6. **`<tool_call>` / `<tool_result>` markers defanged** in reflected external bytes — `ExecGetTaskOutput` (error + stdout + stderr) and `ExecGetRunStatus` (per-task error) now run their reflected text through `DefangToolMarkers`, which replaces literal `<tool_call>`/`</tool_call>`/`<tool_result>`/`</tool_result>` ASCII sequences with U+27E8/U+27E9 mathematical-angle-bracket equivalents.  Visual content preserved; the parser-keying ASCII bytes are gone.  Closes the indirect-prompt-injection MEDIUM where a script printing `<tool_call>` to stdout becomes a parsed tool call on the next AI turn.
7. **Per-change template entries** for all 6 changes appended to `doc/misc/S1-D2-session-note.md` under "Sitting 2 — assistantTools.cpp HIGHs (canonical-path theme + JSON-escape + tool-marker defang)".  Skipped-findings table records the deferred items with reasons.

### What's verified

- Studio debug build clean (`make config=debug`); only `assistantTools.cpp` recompiled, link succeeds.
- 28-test assistant non-AI suite: **PASS** end-to-end against TLS+auth j9t (with the harness fix).  This is regression coverage that the protocol/session/command surface still works after the rewrites.
- `python3 test/dispatch/test_testinterface_hermetic.py`: **PASS** — adjacent code paths unbroken.
- **Not directly verified:** the rewritten C++ tool-execution code paths (IsPathDenied on real symlinks, deny-list extension matches, JsonEscape on adversarial workflowIds, DefangToolMarkers on adversarial stdout) require AI-driven tool calls.  The 28-test suite is protocol-level and doesn't reach them.  AI-suite or dashboard manual smoke is the runtime confirmation; not blocking sitting 3 but worth driving before considering S1=D2 closed at the file level.

### Open items / next-session candidates

- **Sitting 3** is `application/assistant/assistantController.h` whole-file: CRITICAL approval-bypass via unauthenticated `approval_response`, CRITICAL path traversal in `GetSession`, plus the safety-side HIGH cluster (background-thread lifetime captures, lock-order inversions, missed CV wakeups, WS client-pointer races, thread-vector unbounded growth — reuse engine threadpool per memory `feedback_no_jthread_use_threadpool`, severity-mismatched logging, `default:` over closed enum).  This is where the densest CRITICALs in the audit live; expect a substantial sitting.
- **`assistantTools.cpp` deferred items** that still need pickup (per skipped-findings table in `S1-D2-session-note.md`): Windows PowerShell `-Command "..."` quoting (HIGH; needs `-EncodedCommand` / script-file pattern), ParseJsonString simdjson rewrite (MEDIUM; per memory `feedback_simdjson_only`), JsonEscape three-copy convergence (discipline).
- **`workspaceIndexer.h::ReadFileContent` HIGH** — different file, pairs with the now-rewritten `IsPathDenied`.  Trivial fix: pass workspace root in, `fs::weakly_canonical` against it.
- **`contextAssembler.h` MEDIUM/LOW** — prompt-injection via unsanitized `turn.text` concatenation, unbounded conversation context.  Can be done alongside the controller work or after.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **`test/assistant/test_assistant.py` is now usable for regression testing** — default URL is `wss://localhost:8443`, picks up `J9T_TOKEN` automatically.  Run as `python3 test/assistant/test_assistant.py` (28 non-AI tests) or `--with-ai --auto-approve` (full 70 tests, burns provider quota).  Use the 28-test run as a quick regression check after any assistant-controller / assistant-tools change.
- **`IsPathDenied` is now structural, not just a list match.**  Fail-closed on any path-resolution error.  If a legitimate test path starts getting rejected, check that `fs::current_path()` resolves to the project root at server-start time (it should — that's how j9t is launched).
- **`JsonEscape` is duplicated three times in this codebase** (assistantSession.cpp anon-namespace + workspaceIndexer.cpp + assistantTools.cpp anon-namespace).  Memory `feedback_cpp_discipline` calls for refactor at the third copy — landed past it intentionally to bound this sitting's diff.  Sitting 3 or a dedicated follow-up should converge them into `engine/json/jsonHelper.h` (the existing `JsonHelper::SanitizeForJson` there is broken — silently drops form-feed (line 35 of jsonHelper.cpp has a literal 0x0C char as the case label), missing `\\u%04x` for control chars; replace it).
- **`DefangToolMarkers` is the canonical pattern for AI-context-boundary defense** — apply at every site where externally-sourced text re-enters the AI's view as tool-result content.  Replaces literal ASCII sequences with U+27E8/U+27E9 mathematical-angle-bracket equivalents.  Visual content preserved; parser-keying bytes neutralized.

---

## 2026-04-29 → next session

Two themes today: the §19 deferred SanitizeUtf8 boundary work landed across 7 files, then the first hardening session (S1=D2 sitting 1) opened and closed at the documented boundary "argv-only execution + canonical-cwd in `application/assistant/assistantTools.cpp`".

### What landed

1. **§19 deferred SanitizeUtf8 plumbing** — `application/json/replyParser{API1..API5}.cpp` (real AI reply text fields + error messages), `application/workflow/pythonTaskExecutor.cpp` (captured stdout/stderr once-after-capture), `application/workflow/shellTaskExecutor.cpp` (per-line LOG_APP_INFO sanitize on both popen and fork/exec paths + once-after-capture in caller). Bonus: 4 `std::cout` violations in API1/API3 fixed during the same edits (per memory `feedback_use_log_macros`). Bedrock Anthropic delegates to API4 so it inherits the sanitization for free.
2. **Per-interface mock-transport plan added to `todo.md`** under Pre-1.0, sequenced after the 4 hardening sessions. JC's design call: `IInterfaceTransport` abstraction with `LiveTransport` / `MockTransport` per InterfaceType; matches on `X-J9T-Mock-Fixture` header only (NOT URL or full-header). Complementary to the existing aoai-api-simulator (API6) and LocalStack (API5) HTTP mocks — different layers, different bug classes. Goal: byte-level fault injection through real parsers + per-interface contract tests + drift catches without burning quota.
3. **S1=D2 hardening sitting 1** — 5 cyber-sec CRITICALs in `assistantTools.cpp` closed:
   - `ExecSearchFiles`: POSIX argv exec via new `RunArgvCapture` helper; Windows keeps popen-via-bash but with `PosixSingleQuote` full single-quote escaping; grep fallback dropped (was reintroducing the same injection per audit MEDIUM).
   - `ExecListFiles`: rewritten on `std::filesystem::recursive_directory_iterator` — no exec, no shell, fully portable.
   - `ExecGetLogTail`: deleted (was unreachable from `m_ToolFns`).
   - `IsCommandBlocked`: deleted (blocklist anti-pattern; allowlists fail closed, blocklists fail open per memory `feedback_allowlist_not_blocklist`). Run_shell now relies solely on the human-approval flow at the controller layer for security.
   - `ExecRunShell` cwd validation: new `IsCwdInsideProjectRoot` helper does `fs::weakly_canonical` comparison against `fs::current_path()` — defends against `..`, absolute paths, and symlinks pointing outside the project root. POSIX path applies cwd via `chdir()` in the child (not `cd '$cwd' &&` composition); Windows path applies via CreateProcessA's `lpCurrentDirectory` (not `Set-Location`/`cd` shell prefix). The CWD-injection class is now structurally impossible.
4. **Per-change template entries** for each of the 5 CRITICALs landed in `doc/misc/S1-D2-session-note.md` (new session-tracking file; format per cybersec-hardening §5). Skipped-findings table at the end records every HIGH/MEDIUM in the same file with reasons for sitting-2 deferral. No silent drops.

### What's verified

- Studio debug build clean (`make config=debug`); 5 .cpp files recompiled (assistantController.cpp, assistantTools.cpp, jarvisAgent.cpp, webServer.cpp, webServer_studio.cpp), link succeeds.
- `python3 test/dispatch/test_testinterface_hermetic.py` PASS — the rebuild + new helpers + adjacent code paths all function.
- `python3 test/dispatch/test_stress_tui_utf8_invalid.py` PASS (during the §19 verification earlier in the session) — 140 ai_call tasks with malformed bytes; log/log.txt = 6 MB new, all valid UTF-8. No regression.
- **Not directly verified:** runtime smoke of search_files / list_files / run_shell via the assistant chat surface. The existing `test/assistant/test_assistant.py` connects via plain `ws://localhost:8080` and does not negotiate TLS or accept self-signed certs, so it can't drive a TLS-only j9t. Build + code review + adjacent-test passing is the verification floor for sitting 1; manual dashboard chat by JC is the recommended pre-sitting-2 confirmation.

### Open items / next-session candidates

- **S1 sitting 2** is what comes next: assistantTools.cpp HIGHs (`IsPathDenied` symlink/case canonical-path refactor, `.bak`/`.tmp` leak via deny-list miss, `ExecJcwfWriteScript` path-prefix validation, `ExecJcwfGenerate` global.json string-concat JSON, `ExecGetFileSummary` missing `IsPathDenied` call) and the `assistantController.h` CRITICALs + HIGHs (auth bypass via unauthenticated `approval_response`, path traversal in `GetSession`, background-thread lifetime captures, lock-order inversions, missed CV wakeups, WebSocket client-pointer races, thread vector unbounded growth — reuse engine threadpool, severity-mismatched logging, `default:` over closed enum). Plan tracks the schedule; `S1-D2-session-note.md` is the cumulative artifact.
- **Test harness TLS+self-signed gap** — `test/assistant/test_assistant.py` likely silently un-runnable since the j9t HTTPS migration. The fix is one-line in the harness (pass `sslopt={"cert_reqs": ssl.CERT_NONE}` to `websocket.create_connection`). Worth picking up before sitting 2 so we have actual end-to-end automated coverage of the assistant tool changes.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video. See `todo.md`.

### Gotchas next-session-Claude should know

- **`POST /api/shutdown` is now the only legitimate way to stop a running j9t for relaunch** — never `pkill -f jarvisAgent-studio`. Memory `feedback_shutdown_via_rest` captures the rule. Signal-kill bypasses keystore re-seal + audit-log flush; orderly shutdown via REST is the only correct path.
- **`assistantTools.cpp` anonymous-namespace helpers** — `RunArgvCapture` (POSIX, fork/execvp/poll/timeout/capture), `IsCwdInsideProjectRoot` (cross-platform weakly_canonical-vs-project-root), `PosixSingleQuote` (Windows-only — full POSIX single-quote escape). Reuse these for new tool sites instead of reintroducing popen-with-string-composition. The helpers are intentionally *not* extracted to a shared header; if a third file needs them, that's the moment to extract per the C++ discipline rule "refactor to one helper before adding a third copy".
- **`run_shell` security model is now: cwd validation + approval flow only.** No blocklist. The cwd validation is structural (canonical path inside project root + chdir/lpCurrentDirectory plumbing); the approval flow lives in the controller layer and is the next-sitting target. Until controller-layer approval enforcement lands in sitting 2, run_shell is "less safe than it looks" — the comment block in `ExecRunShell` documents this explicitly.
- **`SanitizeUtf8` is now applied at the real reply-parser boundaries (not just TestInterface)** — when adding new providers / parsers, sanitize content/text/error-message fields at the simdjson `string_view` → `std::string` boundary. The `feedback_established_safety_patterns` memory describes the pattern; `replyParserAPI{1..5}.cpp` are the worked examples.

---

## 2026-04-28 (clangd-LSP setup) → next session

Tooling-only session — set up structural C++ navigation for my own use; no project code changes.  Per-machine setup details captured in `feedback_clangd_lsp_for_navigation.md` (memory) so they don't pollute this tracked log.

### Project-relevant findings

- **`compile_commands.json` is now generated by Bear** at the project root.  Refresh via `premake5 clean && premake5 gmake && bear -- make config=debug` after build-system changes.  Both `compile_commands.json` and `.cache/` are gitignored.
- **Smoke test surfaced a count I'd missed:** `webServer.cpp` has **10** call sites of `SanitizeUtf8` (lines 285, 293, 301, 308, 316, 3409, 3535, 3538, 3542, 4105).  When the morning's commit removed the local anonymous-namespace duplicate, all 10 callers transparently re-resolved to the canonical `SanitizeUtf8` in `application/workflow/workflowTypes.h`.  Real validation that the consolidation worked correctly — clangd's `findReferences` returned the full set in ~200 ms.

### Open items / next-session candidates

Unchanged from the prior 2026-04-28 entries (Tier B + TUI sanitization + 2 hardening plans landed; §18 / §19 sessions are next).  See `todo.md` (project root).

---

## 2026-04-28 (post-commit maintenance) → next session

Pure documentation + file-structure cleanup pass after the morning's big commit.  One small post-commit C++ change (`ResetTestState()` `#ifdef DEBUG`-gated for hygiene), no functional code work.  JC committing as `"maintenance"`.

### What landed

1. **`ResetTestState()` gated `#ifdef DEBUG`** in `engine/curlWrapper/curlMultiDispatcher.{h,cpp}` — completes the symmetry with the route gating.  All 4 binaries rebuilt (Studio Debug/Release, Engine Debug/Release); Engine symbol-isolation invariant intact (Engine Release ~1 MB smaller than Studio Release).
2. **Hand-off log convention established** — `doc/misc/hand-off.md` (this file).  Newest entry on top, self-contained per entry, cross-references rather than duplicates other docs.  New auto-memory entry `feedback_session_handoff_log.md` so future-Claude reads the latest entry at session start and prepends a new one at session end.
3. **TODO restructure** — three archives moved into `doc/misc/`:
   - `JarvisAgent TODO List.md` → `doc/misc/JarvisAgent TODO List.md` (global archive)
   - `application/workflow/doc/todo.md` → `doc/misc/application-workflow-todo.md` (backend archive)
   - `workflow-editor/todo.md` → `doc/misc/workflow-editor-todo.md` (frontend archive)
   New consolidated `todo.md` at project root holds the live open items only.  The two scope-specific files were re-created empty with header-only stubs at the original locations (so the directory shape stays valid; new scope-specific TODOs land there if they ever arise).  Memory `reference_todo_files.md` updated to reflect the new shape.
4. **§5i verified done + tail items extracted** — read `doc/misc/engine-studio-capability-review.md` carefully; the §5i refactor landed in the **2026-04-25** session (way before today).  Today's commit pulled that work in as part of the big bundle but didn't add to it.  Struck the §5i entry from the archived global TODO with a verification summary.  Four real tail items from the review doc's "Open items / follow-ups" section that hadn't been captured anywhere live-tracked got added to `todo.md` under a new "§5i follow-ups (post-implementation)" subsection: shutdown audit-log gap, `HandleAiInterfaceTestPost` Engine fallback decision, AI-WS dispatch extraction (drops `#ifdef` count 10→7), bootstrap `admin/admin` badge collision.
5. **Audited the 4 recent refactor docs** for forgotten TODOs:
   - `doc/misc/API refactor.md` (5h Bedrock + Azure) — fully done.
   - `doc/misc/AI dispatch refactor.md` (5g) — main follow-ups already in `todo.md`; **one missing**: live-backed E2E tests for schema-validation retry, chunking, and markitdown (today's tests are hermetic-only).  Added to `todo.md` §5g remaining follow-ups.
   - `doc/misc/AI call performance optimization.md` (rev 7) — fully done after today's Tier B.
   - `doc/misc/cloud-integration-dev-plan.md` — surfaced **two genuinely open items** (`email_watch` doesn't actually IMAP-poll for new mail; Mailpit JCWF coverage gap that explains the 14-vs-13 dashboard mismatch JC noticed).  Added to `todo.md` under a new "Cloud integration tail" subsection.  Two **stale checkboxes** in the plan flipped to `[x]` with verification notes: Redmine frontend (in `ConnectionsView.tsx` + `WorkflowEditorView.tsx`) and Snowflake round-trip (verified end-to-end per `snowflakeQueryDemo.md`).
6. **Self-hosted Docker registry** removed from the consolidated TODO — misunderstanding from old session notes; GitHub Container Registry is fine, no migration needed.

### Where things live now

- `todo.md` (project root) — live open items only; consolidated 2026-04-28
- `application/workflow/doc/todo.md` — header-only stub, scope clarified
- `workflow-editor/todo.md` — header-only stub, scope clarified
- `doc/misc/JarvisAgent TODO List.md` — global archive (read-only)
- `doc/misc/application-workflow-todo.md` — backend archive (read-only)
- `doc/misc/workflow-editor-todo.md` — frontend archive (read-only)
- `doc/misc/hand-off.md` — this file
- `doc/misc/cybersec-hardening-dev-plan.md` — §18 plan
- `doc/misc/cpp-safety-hardening-dev-plan.md` — §19 plan
- `doc/misc/engine-studio-capability-review.md` — §5i design + impl log
- `doc/misc/AI call performance optimization.md` — §5g-rl design ref (refactor done)
- `doc/misc/AI dispatch refactor.md` — §5g design ref (refactor done)
- `doc/misc/cloud-integration-dev-plan.md` — Phase 0-12 tracker (mostly done; two tail items in `todo.md`)
- `doc/misc/API refactor.md` — §5h design ref (refactor done)

### Open items / next-session candidates

Unchanged from the morning entry below (Tier B + TUI sanitization + 2 hardening plans landed; §18/§19 sessions are next).  See `todo.md` for the full live list — all items now consolidated there with cross-refs to the relevant dev plans.

### Gotchas next-session-Claude should know

All of the morning's gotchas still apply.  One new one from this pass:

- **Always read `doc/misc/hand-off.md`'s latest entry first when picking up an active project** — even if the user opens with "let's keep going on X", the hand-off has the load-bearing context (config-schema breaks, debug-only paths, helpers worth reusing) that recently landed.  Memory `feedback_session_handoff_log.md` makes this explicit.

---

## 2026-04-28 → next session

### What landed

Three large pieces of work plus assorted fixes, all committed + pushed.

1. **AI dispatch performance refactor (Phases 1–5) committed** — uncommitted since 2026-04-26.  Rate-limit controller (`engine/curlWrapper/rateLimitController.{h,cpp}`), per-provider strategies (`rateLimitStrategy.{h,cpp}`), normalized observation (`rateLimitObservation.h`), AIMD + token-bucket + server-directed waits, size-aware budget via `CURLOPT_TIMEOUT_MS`, dual-timeout collapse, cascade cancellation.  Verified live 2026-04-26 against Anthropic Sonnet (137 tasks, AIMD converged 4→16, zero 429s).
2. **§14 Tier B hermetic dispatcher tests** — 8 Python tests + C++ infra (4 new debug endpoints: `recent-submissions`, `mock-ai-response`, `test-observe-idempotent`, `reset-dispatcher-state`).  All 8 verified across 3 sweeps in one j9t process.
3. **TUI ncurses stress tests** — `test_stress_tui_utf8_heavy.py` (3-way concurrent jarvisCpp at 420 ai_call tasks with multi-byte UTF-8) and `test_stress_tui_utf8_invalid.py` (140 tasks with malformed bytes).  Surfaced + fixed a real bug (raw invalid bytes leaking into `log/log.txt`).  New `SanitizeUtf8` helper in `application/workflow/workflowTypes.h` (companion to `TruncateUtf8Safe`).
4. **Two new dev plans** — `doc/misc/cybersec-hardening-dev-plan.md` (§18) and `doc/misc/cpp-safety-hardening-dev-plan.md` (§19).  4-domain split, 4 combined sessions, importance rubric, per-change template, memo with Rust-emulating defaults.  Plans are review-ready; sessions to execute them not yet scheduled.
5. **10 new auto-memory entries** distilled from the §10 memos of both hardening plans (argv-only shell, allowlist-not-blocklist, path-confinement-edition, secrets-only-via-redactor, auth-funnel, constant-time-compare, capture-by-value-async, no-jthread-use-threadpool, rust-emulating-defaults, established-safety-patterns).

### Bug fixes surfaced via testing today

These came up while building the Tier B / TUI tests; all fixed in the same commit:

- **`ApplyAiInterfaceRateLimitFromJson` padded_string bug** — `simdjson::ondemand::parser::iterate(req.body)` silently no-opped on non-padded `std::string`, so every `rate_limit` override sent via `POST /api/settings/ai-interfaces` was dropped.  Every interface ended up with C++ struct defaults regardless of operator config.
- **`m_MaxRetries429 == 0` treated as "use default 10"** — `> 0` check at `curlMultiDispatcher.cpp:776` meant operators couldn't actually disable retries via config.  Switched to `>= 0`; `-1` is now the sole "unset" sentinel.  Same fix for `m_MaxRetriesTransient`.
- **Malformed UTF-8 from AI replies leaking raw into `log/log.txt` + dashboard WS** — fixed at the TestInterface boundary; real-AI parser + captured-stdout coverage deferred to §19 (D1, S3).
- **Localhost SSL bypass added in DEBUG builds** — so the dispatcher can hit the j9t server's own self-signed cert during hermetic tests.  Production paths still verify; bypass is `#ifdef DEBUG && (host == localhost|127.0.0.1|::1)`.

### What's verified

| Sweep | Result |
|---|---|
| Tier A (existing) — `test_rate_limit_observation_parse.py` | green |
| Tier B (new today) — 8 tests × 3 sweeps in one j9t process | 24/24 pass |
| TUI heavy UTF-8 — 3 jarvisCpp JCWFs concurrent, 420 ai_call | pass, j9t alive, 18.4 MB log clean |
| TUI invalid UTF-8 — 140 ai_call with malformed fixture | pass after `SanitizeUtf8` fix, 6.1 MB log clean |
| All 4 binaries built post-commit (Studio Debug/Release, Engine Debug/Release) | clean, edition isolation intact (Engine Release ~1 MB smaller than Studio Release) |
| Existing dispatch tests (`test_testinterface_hermetic.py`, schema-roundtrip, etc.) | not re-run today; should still pass — no breaking changes to those code paths |

### Open items / next-session candidates

1. **§19 cpp-safety hardening pass** — 4 sessions to execute the plan.  Among the entries: `SanitizeUtf8` at real AI reply parsers (`replyParserAPI{1..5}.cpp`) and at captured stdout/stderr (`shellTaskExecutor`, `pythonTaskExecutor`) — flagged in the plan's §6.1 D1 row "UTF-8 sanitization at external-byte boundaries" as the deferred companion work to today's TUI fix.
2. **§18 cyber-sec hardening pass** — 4 sessions, runs combined with §19 per the dual-plan schedule (S1=D2 web+cloud+assistant, S2=D3 core engine, S3=D1 workflow orchestration, S4=D4 app infrastructure).

### Gotchas next-session-Claude should know

Load-bearing past today:

- **Don't restart j9t lightly.**  Dispatcher state (controller AIMD caps, observation history) lives in-memory; restart loses it.  For repeated test runs in one j9t, call `POST /api/debug/reset-dispatcher-state` between tests (each Phase B test does this at startup).
- **`rate_limit.max_retries_429 = 0` now means 0** — previously meant "use default 10".  In practice nobody sets it explicitly, so unlikely to bite anyone, but worth flagging in changelog if shipping.
- **TestInterface fixture content gets sanitized via `SanitizeUtf8` now** — the on-disk output file is also sanitized (downstream Python combiners read it as UTF-8 text; raw-byte preservation isn't worth breaking the combiner).  If a future test needs raw bytes on disk, that's a flag on the interface, not a default.
- **Localhost SSL bypass + `ResetTestState()` are DEBUG-only** — both `#ifdef DEBUG`-gated.  Release builds verify TLS normally and don't expose the reset endpoint.  Don't write tests that depend on these under Release builds.
- **`SanitizeUtf8` is the project-wide pattern for external-byte boundaries** (alongside `TruncateUtf8Safe` for size bounds).  See `feedback_established_safety_patterns.md` memory.
