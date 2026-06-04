# AI Assistant — Technical Documentation

**Status:** Implemented
**Last updated:** 2026-04-30

---

## Overview

The AI Assistant is a browser-based conversational agent embedded in JarvisAgent. It
lives in the **"Assistant"** tab of the workflow editor React app, rendered as an
xterm.js terminal connected via WebSocket to the C++ backend.

The assistant can inspect files, search code, run workflows, read logs, remember
context across sessions, generate cached file summaries, execute shell commands,
edit source files, develop JCWF workflows, control running workflows, and
validate its own responses for relevance and hallucinations.

### Capability levels

| Level | Name | Status |
|-------|------|--------|
| L1 | Chat terminal | ✅ Implemented |
| L2 | Project-aware assistant | ✅ Implemented |
| L3 | Agentic assistant (multi-step planning, file edits, compile/test/fix) | ✅ Implemented |

The assistant exposes **31 tools** spanning read-only queries (system status,
file reading, code search), project-aware operations (memory persistence,
workspace indexing with cached AI-generated file summaries), and mutating
actions (shell execution, file writes/edits, JCWF workflow development, runtime
control). Tool invocations run inside a **multi-step loop** (up to 10
iterations per user message) with automatic **loop detection** that forces a
final answer when the same tool+args combination is called more than three
times. All mutating tools require explicit **user approval** via an inline Y/n
prompt (60-second timeout, auto-denied on expiry). The frontend provides
**ghost-text auto-completion** (slash commands, workflow IDs, command history)
and **Ctrl+R reverse history search** across all sessions. Before delivering
each response, a **relevance validator** checks keyword overlap between the
user's question and the AI's answer and verifies that any file paths mentioned
in the response actually exist in the workspace.

---

## Architecture

```
┌─────────────────────────────────────┐
│  React App — "Assistant" tab        │
│  ┌───────────────────────────────┐  │
│  │  xterm.js terminal emulator   │  │
│  └──────────┬────────────────────┘  │
│             WS /ws/assistant        │
└─────────────┼───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│  C++ Backend                        │
│                                     │
│  AssistantController (WS handler)   │
│       │                             │
│       ├── AssistantSession          │
│       ├── ContextAssembler          │
│       ├── ToolRegistry + Executor   │
│       ├── MemoryStore               │
│       ├── WorkspaceIndexer          │
│       ├── ResponseValidator         │
│       └── AI provider (existing)    │
│                                     │
│  Shared helpers:                    │
│       assistantHelpers (RandomHex,  │
│         IsValidOpaqueId)            │
│       JsonHelper::EscapeJsonString  │
│       ToolRegistry::DefangToolMarkers│
│                                     │
│  Existing infrastructure:           │
│  WebServer, AiRequestPool,          │
│  WorkflowRegistry, RuntimeManager,  │
│  AiJcwfService, ScriptRegistry,     │
│  PythonEngine, Log API              │
└─────────────────────────────────────┘
```

### Key design decisions

- **File-based transparency.** All persistence uses human-readable text files (JSON, JSONL). Easy to
  debug, diff, and inspect. Every memory entry, conversation turn, and file index
  is written to disk. Open `assistant/` to see exactly what the AI knows.
- **Reuse existing AI provider stack.** Same `AiRequestPool` + `CurlMultiDispatcher`
  pipeline that workflow `ai_call` tasks use.
- **Separate WebSocket route.** `/ws/assistant` is independent of the existing `/ws`
  broadcast channel.

---

## Storage layout

```
assistant/
├── sessions/
│   ├── sess_4f2e8a1c9d3b7e1604d8a2f15c9e3b27.jsonl   # 128-bit RandomHex IDs, 0600 perms on POSIX
│   └── sess_b1c4e2f7a8d916302c5d8f1e4a7b96e3.jsonl
├── memory.json                                       # persistent key-value memory store
└── index/
    └── file_index.jsonl                              # workspace file index with cached summaries
```

Session and memory IDs are 128-bit cryptographically random hex strings (16 bytes from `RAND_bytes`).  The previous timestamp+counter scheme was replaced after a session-ID-prediction finding — see `assistantHelpers::RandomHex`.

### File formats

**Session history (`sessions/*.jsonl`)** — one JSON object per line, parsed by `simdjson::ondemand`:
```jsonl
{"role":"user","text":"Why did cyber2 fail?","ts":"2026-03-23T21:06:19Z"}
{"role":"assistant","text":"Looking at the logs...","ts":"2026-03-23T21:06:21Z"}
```
Per-line bytes are bounded by `AssistantSession::kMaxLineBytes` (1 MiB) and the total turn count by `kMaxTurnsPerSession` (10 000).  Each turn's text is clamped to `kMaxTurnTextBytes` (256 KiB) at load time.  Roles outside `{user, assistant, system}` are dropped.

**Memory store (`memory.json`)** — JSON array of entries:
```json
[
  {"id":"mem_a3c8e1...","key":"user_name","value":"John","tags":["personal"],"createdAt":"2026-03-24T10:00:00Z","sourceSessionId":"sess_..."}
]
```
Bounded by `MemoryStore::kMaxEntries` (10 000), `kMaxKeyBytes` (256), `kMaxValueBytes` (64 KiB), `kMaxTagBytes` (256), `kMaxTagsPerEntry` (32).  Inputs that exceed caps are clamped; new entries past `kMaxEntries` are rejected.

**File index (`index/file_index.jsonl`)** — one entry per indexed file:
```jsonl
{"path":"code/backend/application/assistant/assistantController.cpp","ext":".cpp","size":45000,"mtime":1711234567,"summary":"Manages the /ws/assistant WebSocket...","summary_mtime":1711234567}
```
Bounded by `WorkspaceIndexer::kMaxIndexEntries` (100 000) and `kMaxSummaryBytes` (8 KiB).  Every `path` field read from disk is re-validated against the workspace root captured at startup (`ResolveAndConfine`); paths that escape the workspace are dropped with a `LOG_SECURITY_WARN`.

---

## Backend modules

### assistantHelpers (`assistantHelpers.h/.cpp`)

Shared helpers used across the assistant subsystem.  Lifted out of
`assistantController.cpp`'s anonymous namespace once a third caller appeared.

- `std::string RandomHex(size_t numBytes)` — cryptographically secure hex
  token from `RAND_bytes` (no `std::mt19937` anywhere).  Returns empty string
  on RAND_bytes failure (logged at `LOG_CORE_ERROR`); callers MUST treat empty
  as fail-closed.  Used for session IDs (16 bytes), memory IDs (16 bytes), and
  approval requestIds (16 bytes).
- `bool IsValidOpaqueId(std::string const&)` — strict allowlist
  `[A-Za-z0-9_-]{1,128}` for any opaque assistant identifier (session IDs,
  approval requestIds, memory IDs).  Used at every site where an
  attacker-influenced ID feeds into a filesystem path or audit-log substring.

### AssistantController (`assistantController.h/.cpp`)

- Owns the `/ws/assistant` WebSocket route (registered in `WebServer`)
- Parses incoming messages, routes to appropriate handler
- Manages per-connection state (active session)
- AI dispatch runs on the **engine `ThreadPool`** (`Core::g_Core->GetThreadPool().SubmitTask(...)`); the controller stores the `.share()`'d `std::shared_future<void>` per call and drops finished futures via `wait_for(0ms) == ready`.  No bespoke `std::thread` spawning per turn.
- **Drain CV (`m_DrainCv`).**  `QueueMessage` notifies on every successful enqueue; a long-running `DrainLoop` task (also on the engine ThreadPool) wakes from `wait_for(1s)` and calls `DrainPendingMessages` directly.  Without this loop, AI replies produced after the user's last message used to sit until the next inbound `OnMessage` triggered a drain.  The existing `OnMessage`-side drain calls remain as a synchronous flush before the handler returns (useful for protocol-error responses).  Crow's `send_text` is thread-safe (`asio::post` onto the io-context strand), so calling `DrainPendingMessages` from the drain loop is correct without bouncing through io_context.
- Multi-step tool loop with up to 10 iterations (L3)
- **Sessions are stored as `std::shared_ptr<AssistantSession>`.** Background
  AI lambdas capture the shared_ptr so the session stays alive across the
  multi-step tool loop regardless of `m_Sessions` evictions or `Shutdown`
  ordering.  Callers should declare
  `std::shared_ptr<AssistantSession> session = GetSession(sid);` rather than
  hold a raw `AssistantSession*` past the immediate handler.
- **`GetSession` gates on `IsValidOpaqueId`** before any path construction;
  resolved file is canonicalised under the sessions dir (`weakly_canonical` +
  `lexically_relative` containment check) as defense-in-depth against symlinks.
  Rejections log `[security] assistant_session_invalid_id length=…` /
  `[security] assistant_session_path_escape sid_len=…` (length only, never the
  value).
- **Tool approval flow (L3) is connection-bound.**
  `PendingApproval` carries an `originConn` pointer (identity-only — never
  dereferenced).  `RunAiCallAsync` threads the connection through via lambda
  capture; `HandleApprovalResponse(conn, requestId, approved)` rejects any
  conn-mismatch with `LOG_SECURITY_WARN`.  `OnClose` calls
  `CancelApprovalsForConnection(&conn)` to fail-close any approvals owned by
  the disconnecting client (otherwise the AI loop would hang on the 60s
  timeout, and a future connection that reuses the pointer address could
  match by identity).  `Shutdown`'s `notify_all` snapshots under
  `m_ApprovalsMutex` then notifies outside the lock to avoid lock inversion.
- **`requestId = "apr_" + RandomHex(16)`.** The previous sequential counter is
  gone — sequential request IDs were predictable across reconnects.
- **WS frame size cap.** `OnMessage` rejects frames > 64 KB before
  constructing `simdjson::padded_string` (logs
  `[security] assistant_ws_frame_too_large bytes=…`).
- **`get_history maxEntries` clamped via `std::clamp<int64_t>(val, 1, 500)`**
  before the cast — a negative value previously caused an unbounded loop.
- **Tool-result reflection runs through `ToolRegistry::DefangToolMarkers`.**
  Externally-sourced text re-entering the AI's view as a `<tool_result>...`
  block is defanged at five sites in `assistantTools.cpp` plus
  `RunAiCallAsync`.
- **`HandleListSessions` / `HandleCompletionRequest` snapshot under
  `m_SessionsMutex` then iterate outside the lock** — closes both the
  lock-order TOCTOU and the controller-mutex-while-session-side-mutex
  inversion.
- **`HandleLogCommand` is pure C++ tail-reader** on `std::ifstream` seek-tail
  — the prior popen + `tail` shell composition is gone (no shell on this path).
- Provides `MakeToolAiCall()` callback for tools that need nested AI calls
- `ValidateResponse()` — heuristic relevance checking before final response (L3):
  extracts keywords from user message (lowercase, ≥3 chars, stop-words filtered),
  checks ≥30% appear in AI response; scans for file paths (tokens with `/` and
  known source extensions), verifies each against workspace via `fs::exists()`.
  Returns warnings to append if issues found.
- `HandleCompletionRequest()` — autocomplete candidates for slash cmds, workflow IDs, history (L3)
- `HandleGetHistory()` — deduplicated user messages across all sessions for Ctrl+R (L3)
- Handles slash commands

### AssistantSession (`assistantSession.h/.cpp`)

- One instance per conversation session
- Owns the JSONL history file (append-only)
- `GetRecentTurns(maxTokens)` — returns recent turns within a token budget;
  `maxTokens == 0` returns an empty vector (the prior `!result.empty()` guard
  that always returned ≥1 turn was removed)
- **`[[nodiscard]] bool AddUserMessage()` / `AddAssistantMessage()`** — true
  iff the turn was both pushed in-memory and durably written to disk.
  Implementation writes-then-commits: `AppendTurnLocked` writes to disk first
  with explicit `flush()` + `good()` check, only commits to `m_Turns` on
  success.  On any failure the sticky `m_FileBroken` flag is set so subsequent
  appends fail fast with an ERROR log; no further writes are attempted until
  operator intervention.
- **Session IDs are `"sess_" + RandomHex(16)`** — 128-bit entropy, no
  timestamp, no counter.  The previous `sess_<ms>_<n>` scheme is gone, along
  with the same-millisecond-restart corruption it enabled.
- **Resume constructor validates `sessionId` via `IsValidOpaqueId`** as
  defense-in-depth alongside the controller-layer gate.
- **Logs use `LogSafeSessionId`** — first 8 hex chars + `…` — so log
  aggregators can't be used as a session-hijacking oracle.
- **`LoadFromFileLocked`** is the single load path; called from the
  constructor without acquiring the per-instance mutex (the object is not yet
  shared).  Public methods that need to load again would acquire the lock
  themselves.  Caps enforced at load: `kMaxLineBytes` (1 MiB),
  `kMaxTurnsPerSession` (10 000), `kMaxTurnTextBytes` (256 KiB).
- **Parsing uses `simdjson::ondemand`** — the prior home-built
  `ExtractJsonString` parser couldn't decode `\uXXXX` escapes and corrupted
  any round-trip through `JsonEscape` for control bytes.
- **`ListSessions` constructs `directory_iterator` directly with `error_code`** —
  no TOCTOU `exists()` pre-check.  Foreign `.jsonl` files are filtered by
  `IsValidOpaqueId` before surfacing as sessions.
- **POSIX file permissions are restricted to 0600** on first write
  (best-effort, `permissions(replace)`); Windows relies on inherited NTFS
  DACLs.

### MemoryStore (`assistantMemory.h/.cpp`)

- Persistent key-value store backed by `assistant/memory.json`
- `[[nodiscard]] Save(key, value, tags)`, `[[nodiscard]] Delete(key)`,
  `[[nodiscard]] Recall(query)`, `[[nodiscard]] ListAll()`,
  `[[nodiscard]] GetRelevant(query, maxResults)`,
  `[[nodiscard]] Size()`, `ClearAll()`
- **IDs are `"mem_" + RandomHex(16)`** — 128-bit entropy.  The previous
  process-local `std::mt19937_64` had a data race across instances and
  predictable seeds.
- **`GetRelevant` acquires the lock once** and calls a private `RecallLocked`
  helper that assumes the lock is held — the prior "drop lock between Recall
  return and resize" atomicity gap is closed, and a future inline of `Recall`
  inside `GetRelevant` would no longer deadlock on the non-recursive mutex.
- **`SaveToDiskLocked` returns `bool`.** Failure logs at `LOG_APP_ERROR`,
  sets sticky `m_FileBroken`, and `Save` rolls back the in-memory mutation so
  disk and memory stay consistent.  `Delete` similarly returns false on
  persistence failure.
- **Caps enforced:** `kMaxEntries` (10 000), `kMaxKeyBytes` (256),
  `kMaxValueBytes` (64 KiB), `kMaxTagBytes` (256), `kMaxTagsPerEntry` (32).
  Oversized inputs are clamped at `Save`; new entries past the entry cap are
  rejected.
- **Logs use `LogSafeKey`** — control bytes replaced with `?`, capped at 64
  chars, original byte length appended in parens — to prevent log injection
  via newline-bearing keys and to bound the log line.
- **`Scored` struct stores `size_t idx` rather than `MemoryEntry const*`** —
  the prior raw-pointer-into-vector pattern was one refactor away from a
  use-after-free.
- Punctuation stripping for robust keyword matching
- Thread-safe (mutex-protected, all public methods hold the lock for the whole
  operation)

### ContextAssembler (`contextAssembler.h/.cpp`)

Assembles the full prompt for each AI call:

1. **System prompt** — static template with assistant identity, tool usage rules,
   memory guidelines, indexing guidelines, slash command reference
2. **Tool descriptions** — auto-generated from `ToolRegistry`
3. **Conversation context** — recent turns from the session
4. **Recalled memories** — keyword-matched entries from `MemoryStore`
5. **Relevant file summaries** — keyword-matched entries from `WorkspaceIndexer`
6. **User message** — the current input

Every user-origin string (prior turn text + the new user message) runs through
`DefangContextSentinels` before placement in the prompt:

- Delegates `<tool_call>` / `</tool_call>` / `<tool_result>` / `</tool_result>`
  to `ToolRegistry::DefangToolMarkers` (mathematical-angle-bracket replacement,
  `U+27E8` / `U+27E9`).
- Replaces any run of 3+ `=` characters with the same number of `U+2550`
  (BOX DRAWINGS DOUBLE HORIZONTAL).  The system prompt uses literal `===` to
  delimit `=== Tool System ===`-style headers; without this defang a
  user-supplied `===…===` would spoof those structural boundaries.

Caps: `kMaxUserMessageBytes` (64 KiB), `kMaxTurnTextBytes` (32 KiB),
`kMaxConversationContextBytes` (128 KiB), `kMaxToolDescriptionsBytes`
(64 KiB).  Per-turn truncation + total-context truncation prevent OOM via
crafted long turns.

### ToolRegistry (`assistantTools.h/.cpp`)

Registers and executes AI tools. Tools are described in the system prompt and
invoked via `<tool_call>` blocks in AI responses.

**Thread-safety contract** (header-documented): the `Set*` methods and the
constructor are called once on the owning thread (`AssistantController`)
*before* any AI lambda runs.  After publication, the backing pointers
(`m_WorkflowRegistry`, `m_RuntimeManager`, `m_MemoryStore`,
`m_WorkspaceIndexer`, `m_AiCallFn`) are read-only.  `Execute` /
`BuildToolDescriptions` / `GetToolDefs` may be called from any thread.  The
targets of the backing pointers are individually thread-safe (MemoryStore +
WorkspaceIndexer mutex their state; the workflow registries have their own
contracts).  No internal mutex needed — but if a future change introduces
post-publication mutable state, add a mutex first.

**Implemented tools:**

*L1/L2 read-only tools:*

| Tool | Description | Approval |
|------|-------------|----------|
| `get_system_status` | JarvisAgent status (uptime, active runs, Python state) | No |
| `list_workflows` | Workflow IDs + labels | No |
| `get_run_status` | Run state + task states | No |
| `get_task_output` | Captured stdout/stderr | No |
| `list_recent_runs` | Last N completed/failed runs | No |
| `read_file` | Read file content (line range, 100 KB limit) | No |
| `search_files` | Grep-style search (ripgrep, argv exec) | No |
| `list_files` | Directory listing (`std::filesystem`, no shell) | No |
| `save_memory` | Save a key-value fact | No |
| `recall_memory` | Search memories by keyword | No |
| `list_memories` | List all stored memories | No |
| `delete_memory` | Delete a memory entry | No |
| `get_file_summary` | Cached or AI-generated file summary | No |
| `get_folder_summary` | All cached summaries in a directory | No |

*L3 mutating tools (Phase 5–6):*

| Tool | Description | Approval |
|------|-------------|----------|
| `run_workflow` | Start a workflow run. Args: `workflow_id` | Yes |
| `run_shell` | Execute shell command. Args: `command`, `cwd` (optional). **The deliberate AI shell-execution tool — its contract IS `/bin/sh -c <command>`** (POSIX) or PowerShell `-EncodedCommand` (Windows default) / bash (Windows when `use_bash: true`). POSIX: `fork()` + `setpgid(0,0)` + `chdir(canonicalCwd)` + `execl("/bin/sh","sh","-c",command,nullptr)` + 30 s timeout with process-group kill. Windows: `CreateProcess` with `lpCurrentDirectory` (cwd never composed into the shell command) + reader thread + `WaitForSingleObject` 30 s timeout + `TerminateProcess` on expiry. See `assistantTools.cpp` lines 1786–1835 for the full defense-layering comment block. | Yes |
| `write_file` | Write content to a file. Args: `path`, `content`. `.bak` backup, then `EngineCore::AtomicWriteFile` (creates parent dirs + atomic temp-and-rename), path deny-list | Yes |
| `edit_file` | Find-and-replace in file. Args: `path`, `old_text`, `new_text`. Must match exactly once (fails on 0 or >1 matches), atomic write, `.bak` backup | Yes |

*L3 runtime control tools (Phase 9):*

| Tool | Description | Approval |
|------|-------------|----------|
| `workflow_pause` | Pause a running workflow run. Args: `run_id` | Yes |
| `workflow_resume` | Resume a paused workflow run. Args: `run_id` | Yes |
| `workflow_stop` | Stop a running/paused/pending run (irreversible). Args: `run_id` | Yes |
| `get_dashboard_status` | Comprehensive system overview: version, uptime, session managers, all workflows with labels, active runs with states/timestamps, completed/failed counters, Python engine status. Output capped at 8 KB | No |

*L3 JCWF development tools (Phase 8):*

| Tool | Description | Approval |
|------|-------------|----------|
| `jcwf_read` | Read `.jcwf` file as raw JSON (16 KB limit). Args: `workflow_id` | No |
| `jcwf_explain` | Human-readable summary of tasks, triggers, dataflow, controlflow. Args: `workflow_id` | No |
| `jcwf_validate` | Validate workflow via `AiJcwfService::ValidateJcwf`. Args: `workflow_id` | No |
| `jcwf_read_plan` | Read `workflows/<id>.plan.md` development plan. Args: `workflow_id` | No |
| `jcwf_write_plan` | Write/update development plan (atomic write). Args: `workflow_id`, `content` | Yes |
| `jcwf_generate` | AI-generate JCWF from plan, validate, atomic write with backup. Args: `workflow_id` | Yes |
| `jcwf_fix_task` | AI-fix a specific failed task, validate, atomic write with backup. Args: `workflow_id`, `task_id`, `instructions` | Yes |
| `jcwf_write_script` | Write shell/Python/PowerShell script. For `.sh`: validates shebang + `set -euo pipefail`, sets executable. For `.ps1`: validates `# @jarvis-script` + `Set-StrictMode`. Args: `path`, `content`, `type` | Yes |

**Tool call flow:**
1. AI response contains `<tool_call>{"name": "...", "args": {...}}</tool_call>`
2. Backend parses and validates against registry
3. If approval required → send `approval_request` to frontend, block until user responds
4. Execute tool, capture output (max 4 KB default, 8 KB for file reads, 16 KB for JCWF/compiler output)
5. Append `<tool_result>` to context, re-send to AI for next iteration
6. Max 10 tool iterations per user message
7. Loop detection: identical tool+args called >3× triggers forced final answer
8. On the final iteration, tool descriptions are stripped to force a final answer
9. Response relevance validation before sending `assistant_done`

### WorkspaceIndexer (`workspaceIndexer.h/.cpp`)

Indexes source files for fast lookup and summary caching.

**Scanned directories:** `application/`, `engine/`, `code/frontend/workflow-editor/ui/src/`,
`scripts/`, `workflows/`, plus top-level files.

**Indexed extensions:** `.h`, `.cpp`, `.c`, `.hpp`, `.ts`, `.tsx`, `.js`, `.jsx`,
`.py`, `.lua`, `.sh`, `.md`, `.jcwf`, `.json`, `.css`, `.html`

**Skipped directories:** `node_modules`, `.git`, `bin`, `bin-int`, `vendor`,
`__pycache__`, `.cache`

**Features:**
- Full scan on startup, preserves cached summaries from previous runs
- Summary caching per file with mtime-based invalidation
- `GetRelevantFiles(query, maxResults)` — keyword-based relevance scoring
- Persistence via `assistant/index/file_index.jsonl` (JSONL, simdjson read)
- Thread-safe (mutex-protected, all public methods hold the lock for the whole
  operation — including `LastScanTime()`, which previously read
  `m_LastScanTime` racily without the lock)
- `/index rescan` slash command for manual re-scan

**Path confinement.**  The constructor captures
`m_WorkspaceRoot = fs::weakly_canonical(fs::current_path())` at startup
(typically right after `main()` sets cwd to the project root).  All later
path resolution anchors against this snapshot, so a future cwd change cannot
widen access.  The private `ResolveAndConfine(relativePath)` does
`weakly_canonical(root / raw)` and a `lexically_relative(root)` containment
check; symlinks pointing out of tree are caught at resolution.  Absolute
paths are rejected up front.

**`ReadFileContent(relativePath, maxBytes)` is an instance method** (the
prior `static` version had no anchoring).  Callers route through
`m_WorkspaceIndexer->ReadFileContent(...)`.  This is workspace-confinement
only — it is **not** a deny-list against sensitive files.  Sensitive-file
gating (e.g., `config.json`, `.env`, `*.pem`, `*.key`) lives in
`ToolRegistry::IsPathDenied` and must be applied separately at the call
site (e.g., `ExecGetFileSummary` applies both gates).

**Untrusted index data is re-validated.**  Every `relativePath` parsed from
`assistant/index/file_index.jsonl` runs through `ResolveAndConfine` again
before being trusted into `m_PathToIndex`.  Rejections log
`[security] indexer_index_path_escape len=…`.

**`Scored` struct stores `size_t idx` rather than `FileIndexEntry const*`**
in `GetRelevantFiles` — same lifetime-hardening as `MemoryStore::RecallLocked`.

**Filesystem-error propagation.**  `ScanDirectory` and the top-level-files
block check `ec` after every `fs::file_size` / `fs::last_write_time` and
skip the entry on failure; the prior code stored
`UINTMAX_MAX` / implementation-defined values into the index.

**Caps:** `kMaxIndexEntries` (100 000), `kMaxSummaryBytes` (8 KiB).
`SetFileSummary` clamps to `kMaxSummaryBytes` so a runaway provider response
or prompt-injected mega-summary cannot bloat the index file.

---

## Frontend

### AssistantView (`views/AssistantView.tsx`)

- xterm.js terminal with Tokyo Night color theme
- JetBrains Mono / Fira Code / Cascadia Code font
- Custom input handling: line-by-line capture, cursor movement, Ctrl+C/U/L shortcuts
- ANSI-colored output: green for user, cyan for assistant, magenta for tools,
  yellow for thinking indicator and approval prompts, red for errors
- Tool approval UI: inline Y/n prompt when a tool requires user consent (L3)
- Ghost-text auto-completion: dim suggestion text after cursor (L3)
  - Completion sources:
    | Source | Trigger | Data |
    |--------|---------|------|
    | Slash commands | Input starts with `/` | Static list from backend |
    | Workflow IDs | Any prefix match | From `WorkflowRegistry` via backend |
    | Command history | Any prefix match | Previous user messages from all sessions |
  - Tab accepts full suggestion or cycles candidates
  - Right arrow at end accepts one character
  - Esc dismisses suggestion
  - Debounced completion requests (150ms) to backend
- Ctrl+R reverse history search across all sessions (L3)
  - `(reverse-i-search)'query': matchedText` prompt
  - Ctrl+R cycles matches, Enter sends, Tab/→ places on input, Esc cancels
- Session management via WebSocket

### WebSocket hook (`hooks/useAssistantWebSocket.ts`)

- Connects to `/ws/assistant` (scheme + host inferred from `window.location`;
  `wss://` when the dashboard is loaded over HTTPS, `ws://` otherwise)
- Auto-reconnect on disconnect (2s delay)
- 500ms ping interval — keepalive only.  AI replies surface via the server-side
  drain CV without needing the client to poll; the ping serves as a liveness
  check + a redundant in-handler drain trigger.
- Handles: `session_active`, `assistant_done`, `tool_status`, `tool_result`,
  `session_history`, `error`, `clear`, `batch`, `approval_request`,
  `completion_response`, `history`

### Assistant button availability

The Assistant nav button is disabled (greyed out) when no AI provider interface
is configured. A tooltip explains: "No AI provider configured. Add one in AI Manager."

---

## Slash commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/status` | JarvisAgent system status |
| `/runs` | List active workflow runs |
| `/log [N]` | Show last N log lines (default 20) |
| `/memory` | List saved memories |
| `/memory clear` | Clear all memories |
| `/index` | Show file index status and coverage |
| `/index rescan` | Re-scan workspace files |
| `/sessions` | List previous sessions |
| `/new` | Start a new session |
| `/clear` | Clear terminal display |

---

## WebSocket protocol

**Endpoint:** `ws://localhost:8080/ws/assistant`

### Client → Server

```jsonc
{ "type": "user_message", "sessionId": "sess_...", "text": "..." }
{ "type": "command", "sessionId": "sess_...", "command": "help", "args": "" }
{ "type": "list_sessions" }
{ "type": "resume_session", "sessionId": "sess_..." }
{ "type": "new_session" }
{ "type": "ping" }
{ "type": "approval_response", "requestId": "...", "approved": true }
{ "type": "completion_request", "prefix": "/he" }
{ "type": "get_history", "maxEntries": 500 }
```

### Server → Client

```jsonc
{ "type": "session_active", "sessionId": "sess_..." }
{ "type": "assistant_done", "sessionId": "sess_...", "text": "..." }
{ "type": "assistant_thinking", "sessionId": "sess_..." }
{ "type": "tool_status", "sessionId": "sess_...", "tool": "...", "status": "running" }
{ "type": "tool_result", "sessionId": "sess_...", "tool": "...", "ok": true, "summary": "..." }
{ "type": "approval_request", "sessionId": "sess_...", "requestId": "...", "tool": "...", "args": {...}, "description": "..." }
{ "type": "completion_response", "prefix": "/he", "candidates": ["/help"] }
{ "type": "history", "entries": ["previous user message", ...] }
{ "type": "session_list", "sessions": [...] }
{ "type": "session_history", "sessionId": "sess_...", "turns": [...] }
{ "type": "error", "sessionId": "sess_...", "message": "..." }
{ "type": "batch", "messages": [...] }
```

---

## Safety and guardrails

| Concern | Mitigation |
|---------|-----------|
| Shell commands | `run_shell` requires approval and is **the deliberate AI shell-exec tool: POSIX runs `execl("/bin/sh","sh","-c",command,nullptr)`, Windows runs PowerShell `-EncodedCommand` or bash.**  Defenses: **(a)** controller-layer human approval (load-bearing — `ToolDef.requiresApproval=true`, `AssistantController` gates every approval-required call through `RequestToolApproval`; no MCP / REST / WebSocket path reaches `ExecRunShell` directly); **(b)** allowlist-over-blocklist (no command filter); **(c)** `IsCwdInsideProjectRoot` canonicalizes cwd via `fs::weakly_canonical` against `fs::current_path()` and applies the resolved cwd via `chdir()` in the child / `lpCurrentDirectory` on Windows — never composed into the shell command, so `..`, absolute paths, and symlinks pointing outside the project root are rejected structurally and a hostile cwd cannot smuggle `&& <other-command>` chains through string concatenation; **(d)** 30 s timeout + process-group kill on POSIX via `kill(-pid, SIGTERM/KILL)` after `setpgid(0,0)` in child / `TerminateProcess` on Windows.  Static analyzers WILL re-flag the `execl("/bin/sh", ...)` line on every pass — that's correct flagging of the right shape, not a regression.  Reviewer's job: confirm defenses (a)–(d) still load-bearing.  See `assistantTools.cpp` lines 1786–1835. |
| Workflow execution | `run_workflow` requires approval |
| File writes | `write_file`, `edit_file` require approval, route through `EngineCore::AtomicWriteFile` (`code/backend/engine/auxiliary/file.h`) for the temp-write-then-rename, `.bak` backup taken before the helper call |
| JCWF mutations | `jcwf_generate`, `jcwf_fix_task`, `jcwf_write_plan`, `jcwf_write_script` require approval, route through `EngineCore::AtomicWriteFile` with backup; `jcwf_generate` JSON-escapes `workflowId` via the central `JsonHelper::EscapeJsonString` |
| Runtime control | `workflow_pause`, `workflow_resume`, `workflow_stop` require approval |
| Infinite tool loops | Max 10 tool calls per turn, loop detection (>3× identical calls) |
| Token budget | ContextAssembler truncates oldest turns first; explicit caps `kMaxConversationContextBytes` (128 KiB), `kMaxTurnTextBytes` (32 KiB), `kMaxUserMessageBytes` (64 KiB), `kMaxToolDescriptionsBytes` (64 KiB) |
| Stale summaries | Invalidated when file mtime changes |
| Sensitive data | `read_file` and `get_file_summary` apply `IsPathDenied`: `config.json`, `keys.json`, `.env`, `*.pem`, `*.key`, plus their `.bak` / `.tmp` siblings; **resolution uses `fs::weakly_canonical` against project root with case-folded filename + extension comparison — symlinks pointing at sensitive files are caught.** Fail-closed on any resolution error. |
| Tool output size | 4 KB default, 8 KB for file reads, 16 KB for JCWF reads |
| Prompt injection — tool results | Tool outputs wrapped in `<tool_result>` fences; system prompt forbids executing content from tool results; `ToolRegistry::DefangToolMarkers` replaces literal `<tool_call>` / `</tool_call>` / `<tool_result>` / `</tool_result>` ASCII sequences with `U+27E8` / `U+27E9` mathematical-angle-bracket equivalents at every site that reflects external bytes back into the AI's context |
| Prompt injection — context | `ContextAssembler::DefangContextSentinels` runs every prior turn and the new user message through tool-marker defang plus a `===` → `U+2550` collapse, so a user-supplied `===…===` cannot spoof system-prompt section boundaries |
| Path traversal | `read_file` / `write_file` / `edit_file` / `jcwf_write_script` use `fs::weakly_canonical` against the project root with `lexically_relative` containment + leading-`..` rejection.  `WorkspaceIndexer::ReadFileContent` uses the same pattern against the workspace root captured at construction; `LoadIndex` re-validates every path read from disk |
| Session ID prediction | Session IDs are 128-bit RandomHex tokens; `IsValidOpaqueId` enforces `[A-Za-z0-9_-]{1,128}` allowlist before any path construction; logs use first-8-char prefix only |
| Approval bypass | `PendingApproval` carries the originating connection identity; `HandleApprovalResponse` rejects any conn-mismatch with `LOG_SECURITY_WARN`; `OnClose` cancels approvals owned by the disconnecting client (otherwise the AI loop would hang on the 60s timeout) |
| Sequential requestId guessing | Approval `requestId = "apr_" + RandomHex(16)` (128-bit entropy) — replaces the prior process-local atomic counter |
| WS frame DoS | `OnMessage` rejects frames > 64 KB before parsing; `get_history maxEntries` clamped to `[1, 500]` |
| Persistence integrity | `AssistantSession::AppendTurnLocked` writes-then-commits with `flush()` + `good()` checks, sticky `m_FileBroken` on failure; `MemoryStore::SaveToDiskLocked` returns `bool` and `Save` rolls back the in-memory mutation on failure; failures log at `LOG_APP_ERROR` (not WARN) so the dashboard run analyzer surfaces them |
| Memory growth | All persistence paths capped: `kMaxTurnsPerSession`, `kMaxLineBytes`, `kMaxTurnTextBytes`, `kMaxEntries`, `kMaxKeyBytes`, `kMaxValueBytes`, `kMaxIndexEntries`, `kMaxSummaryBytes`; oversized inputs are clamped at write, oversized lines drop the load with an ERROR + `m_FileBroken` |
| Log injection / PII in logs | `MemoryStore::LogSafeKey` strips control bytes and caps at 64 chars + length; `AssistantSession::LogSafeSessionId` truncates to first 8 hex chars + `…`; the home-built `ExtractJsonString` is gone — simdjson is the project's only JSON parser |
| Session file permissions | POSIX: 0600 set on first write (best-effort); Windows: relies on inherited NTFS DACLs |
| Script safety | `jcwf_write_script` validates shebang + `set -euo pipefail` for `.sh`; validates `# @jarvis-script` + `Set-StrictMode` for `.ps1`; rejects absolute paths |
| Off-topic responses | Keyword overlap check (≥30% threshold) appends warning if response seems irrelevant |
| Hallucinated paths | File paths in AI responses verified against workspace; missing paths flagged with note |
| Approval timeout | 60s timeout — denied if user doesn't respond |
| Background-thread lifetime | AI lambdas run on the engine `ThreadPool` (`Core::g_Core->GetThreadPool().SubmitTask(...)`), not bespoke `std::thread`s.  Sessions are `std::shared_ptr<AssistantSession>` so the lambda holds the session alive across the multi-step tool loop.  `Shutdown` waits on every `m_BackgroundFutures` entry + the drain loop future before returning, so no lambda outlives the controller. |
| ToolRegistry concurrency | Set-once setters called on the owning thread before any AI lambda runs; backing pointers immutable post-publication; `Execute` / `BuildToolDescriptions` / `GetToolDefs` safe from any thread.  Targets of the backing pointers (`MemoryStore`, `WorkspaceIndexer`, workflow registries) are individually thread-safe. |
| Stale messages on idle session | Server-side drain CV (`m_DrainCv`) wakes a long-running `DrainLoop` task on every `QueueMessage` enqueue; AI replies surface immediately rather than waiting on the next inbound `OnMessage`.  Pre-sitting-5 the queue would sit until the user typed again. |

---

## Files

### C++ backend (`code/backend/application/assistant/`)

| File | Purpose |
|------|---------|
| `assistantController.h/.cpp` | WS handler, message routing, AI calls, slash commands, approval flow (connection-bound), completion, history, response validation |
| `assistantSession.h/.cpp` | JSONL session persistence, turn management, simdjson load, RandomHex IDs, sticky `m_FileBroken` |
| `contextAssembler.h/.cpp` | Prompt assembly + `DefangContextSentinels` for prior-turn / user-message text |
| `assistantTools.h/.cpp` | Tool registry, tool execution, tool descriptions (31 tools), public `DefangToolMarkers`.  Most tools use the argv-only `RunArgvCapture` helper for subprocess execution.  The single exception is `run_shell`, which is by design a `/bin/sh -c` shell-exec tool — see the "Mutating tools" table above for its defense-layering contract.  `ExecJcwfFixTask` validates the AI-supplied `task_id` via `IsValidOpaqueId` before any further processing and uses a simdjson on-demand structural lookup against the canvas's `tasks` object (replaces the prior `find("\"" + taskId + "\"")` substring search that false-positive-matched on prose). |
| `assistantMemory.h/.cpp` | Persistent key-value memory store with `RecallLocked` + sticky `m_FileBroken` + `LogSafeKey` redaction |
| `workspaceIndexer.h/.cpp` | Workspace file indexing, summary caching, workspace-root path anchoring |
| `assistantHelpers.h/.cpp` | Shared `RandomHex` (RAND_bytes-backed) + `IsValidOpaqueId` (allowlist) used across the subsystem |

### Modified engine files

| File | Changes |
|------|---------|
| `code/backend/engine/json/jsonHelper.h/.cpp` | Rewrite: static `EscapeJsonString` (RFC 8259-compliant, control-byte `\u00XX` escape) is the canonical helper; instance `SanitizeForJson` retained as backwards-compatible thin delegator.  See `code/backend/engine/json/json.md` §4. |

### Modified C++ files

| File | Changes |
|------|---------|
| `code/backend/application/web/webServer.h/.cpp` | `/ws/assistant` route, `AssistantController` member |
| `code/backend/application/web/aiJcwfService.h` | `ValidateJcwf` made public for tool access (L3) |
| `code/backend/application/jarvisAgent.h/.cpp` | Creates `assistant/sessions/` on startup |
| `premake5.lua` | New source files in build |

### Frontend (`code/frontend/workflow-editor/ui/src/`)

| File | Purpose |
|------|---------|
| `views/AssistantView.tsx` | xterm.js terminal view |
| `hooks/useAssistantWebSocket.ts` | WebSocket connection + state management |
| `App.tsx` | "Assistant" nav button + route |

### npm dependencies

- `@xterm/xterm` — terminal emulator
- `@xterm/addon-fit` — auto-resize addon

---

## L3 implementation summary

| Feature | Status |
|---------|--------|
| Multi-step tool loop + approval flow | ✅ |
| Mutating tools (`run_shell`, `write_file`, `edit_file`) | ✅ |
| Compile/test/fix workflow + system prompt guidelines | ✅ |
| JCWF development tools (8 tools) | ✅ |
| Runtime control (`workflow_pause/resume/stop`, `get_dashboard_status`) | ✅ |
| Auto-completion + Ctrl+R reverse history search | ✅ |
| Response relevance checking (keyword overlap + path verification) | ✅ |

### System prompt guidelines (contextAssembler.cpp)

The system prompt includes guidelines for:

**Multi-step planning:**
```
When a user request requires multiple steps:
1. Think about what information you need and what tools to call.
2. Call tools one step at a time — examine results before deciding the next step.
3. You may call tools across multiple rounds.
4. When you have enough information, provide your final answer.
5. Never repeat a tool call with identical arguments.
```

**Compile/test/fix workflow:**
```
When the user asks you to fix a build error or implement a code change:
1. Read the relevant file(s) to understand the code.
2. Use edit_file to make the change (requires approval).
3. Use run_shell to compile: make config=release (requires approval).
4. If compilation fails, read the error output and fix the issue.
5. Repeat steps 2–4 until the build succeeds or you've tried 3 times.
6. Report the final result to the user.
```

**JCWF plan-first development model:**
```
When creating or modifying a JCWF:
1. Read the existing plan (jcwf_read_plan) or create one (jcwf_write_plan).
2. Update the plan to reflect the desired changes.
3. Generate/fix the JCWF from the plan (jcwf_generate or jcwf_fix_task).
4. Validate the result (jcwf_validate).
5. If validation fails, fix and re-validate.
```

**Runtime control:** check status before pausing/stopping, `workflow_stop` is irreversible.

**Memory persistence rules** and **file indexing usage** are also included.

---
