# AI Assistant — Technical Documentation

**Status:** Implemented
**Last updated:** 2026-03-28

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
- **Reuse existing AI provider stack.** Same `AiRequestPool` / `SessionManager` /
  `CurlWrapper` pipeline that workflows use.
- **Separate WebSocket route.** `/ws/assistant` is independent of the existing `/ws`
  broadcast channel.

---

## Storage layout

```
assistant/
├── sessions/
│   ├── sess_1711234567.jsonl       # conversation history (append-only)
│   └── sess_1711234999.jsonl
├── memory.json                     # persistent key-value memory store
└── index/
    └── file_index.jsonl            # workspace file index with cached summaries
```

### File formats

**Session history (`sessions/*.jsonl`)** — one JSON object per line:
```jsonl
{"role":"user","text":"Why did cyber2 fail?","ts":"2026-03-23T21:06:19Z"}
{"role":"assistant","text":"Looking at the logs...","ts":"2026-03-23T21:06:21Z"}
```

**Memory store (`memory.json`)** — JSON array of key-value entries with tags:
```json
[
  {"key":"user_name","value":"John","tags":["personal"],"created_at":"2026-03-24T10:00:00Z"}
]
```

**File index (`index/file_index.jsonl`)** — one entry per indexed file:
```jsonl
{"path":"application/assistant/assistantController.cpp","ext":".cpp","size":45000,"mtime":1711234567,"summary":"Manages the /ws/assistant WebSocket...","summary_mtime":1711234567}
```

---

## Backend modules

### AssistantController (`assistantController.h/.cpp`)

- Owns the `/ws/assistant` WebSocket route (registered in `WebServer`)
- Parses incoming messages, routes to appropriate handler
- Manages per-connection state (active session)
- Handles AI calls via background threads using `AiRequestPool`
- Multi-step tool loop with up to 10 iterations (L3)
- Tool approval flow (L3): tools requiring approval send `approval_request` to
  frontend, then block the background thread on a `std::condition_variable`
  inside a `PendingApproval` struct (mutex-protected map keyed by request ID).
  Frontend `approval_response` sets the result and notifies the CV. 60s timeout
  auto-denies with "Approval timed out".
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
- `GetRecentTurns(maxTokens)` — returns recent turns within a token budget
- `AddUserMessage()`, `AddAssistantMessage()`
- Session ID generated from milliseconds timestamp + atomic counter at creation

### MemoryStore (`assistantMemory.h/.cpp`)

- Persistent key-value store backed by `assistant/memory.json`
- `Save(key, value, tags)`, `Delete(key)`, `ListAll()`
- `GetRelevant(query, maxResults)` — keyword-based relevance search
- Punctuation stripping for robust keyword matching
- Thread-safe (mutex-protected)

### ContextAssembler (`contextAssembler.h/.cpp`)

Assembles the full prompt for each AI call:

1. **System prompt** — static template with assistant identity, tool usage rules,
   memory guidelines, indexing guidelines, slash command reference
2. **Tool descriptions** — auto-generated from `ToolRegistry`
3. **Conversation context** — recent turns from the session
4. **Recalled memories** — keyword-matched entries from `MemoryStore`
5. **Relevant file summaries** — keyword-matched entries from `WorkspaceIndexer`
6. **User message** — the current input

### ToolRegistry (`assistantTools.h/.cpp`)

Registers and executes AI tools. Tools are described in the system prompt and
invoked via `<tool_call>` blocks in AI responses.

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
| `search_files` | Grep-style search | No |
| `list_files` | Directory listing | No |
| `get_log_tail` | Last N log lines | No |
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
| `run_shell` | Execute shell command via `/bin/sh -c`. Args: `command`, `cwd` (optional). 30s timeout, process group kill on timeout | Yes |
| `write_file` | Write content to a file. Args: `path`, `content`. Creates parent dirs, atomic write (`.tmp` + rename), `.bak` backup, path deny-list | Yes |
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
| `jcwf_write_script` | Write shell/Python script (validates shebang + `set -euo pipefail` for shell, sets executable). Args: `path`, `content`, `type` | Yes |

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

**Scanned directories:** `application/`, `engine/`, `workflow-editor/ui/src/`,
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
- Thread-safe (mutex-protected)
- `/index rescan` slash command for manual re-scan

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

- Connects to `ws://localhost:8080/ws/assistant`
- Auto-reconnect on disconnect (2s delay)
- 500ms ping interval for message draining
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
| Shell commands | `run_shell` requires approval (all commands go through approval flow), 30s timeout, process group kill |
| Workflow execution | `run_workflow` requires approval |
| File writes | `write_file`, `edit_file` require approval, atomic writes with `.bak` backup |
| JCWF mutations | `jcwf_generate`, `jcwf_fix_task`, `jcwf_write_plan`, `jcwf_write_script` require approval, atomic writes with backup |
| Runtime control | `workflow_pause`, `workflow_resume`, `workflow_stop` require approval |
| Infinite tool loops | Max 10 tool calls per turn, loop detection (>3× identical calls) |
| Token budget | ContextAssembler truncates oldest turns first |
| Stale summaries | Invalidated when file mtime changes |
| Sensitive data | `read_file` has a deny-list: `config.json`, `keys.json`, `*.pem`, `*.key`, `.env` |
| Tool output size | 4 KB default, 8 KB for file reads, 16 KB for JCWF reads |
| Prompt injection | Tool outputs wrapped in `<tool_result>` fences; system prompt forbids executing content from tool results |
| Path traversal | `lexically_normal()` + `..` rejection in `read_file`, `write_file`, `edit_file`, script paths |
| Script safety | `jcwf_write_script` validates shebang + `set -euo pipefail` for shell scripts, rejects absolute paths |
| Off-topic responses | Keyword overlap check (≥30% threshold) appends warning if response seems irrelevant |
| Hallucinated paths | File paths in AI responses verified against workspace; missing paths flagged with note |
| Approval timeout | 60s timeout — denied if user doesn't respond |

---

## Files

### C++ backend (`application/assistant/`)

| File | Purpose |
|------|---------|
| `assistantController.h/.cpp` | WS handler, message routing, AI calls, slash commands, approval flow, completion, history, response validation |
| `assistantSession.h/.cpp` | JSONL session persistence, turn management |
| `contextAssembler.h/.cpp` | Prompt assembly (system prompt + context + tools + memories + summaries + L3 guidelines) |
| `assistantTools.h/.cpp` | Tool registry, tool execution, tool descriptions (31 tools) |
| `assistantMemory.h/.cpp` | Persistent key-value memory store |
| `workspaceIndexer.h/.cpp` | Workspace file indexing, summary caching |

### Modified C++ files

| File | Changes |
|------|---------|
| `application/web/webServer.h/.cpp` | `/ws/assistant` route, `AssistantController` member |
| `application/web/aiJcwfService.h` | `ValidateJcwf` made public for tool access (L3) |
| `application/jarvisAgent.h/.cpp` | Creates `assistant/sessions/` on startup |
| `premake5.lua` | New source files in build |

### Frontend (`workflow-editor/ui/src/`)

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
