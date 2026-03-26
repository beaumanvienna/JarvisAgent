# AI Assistant Terminal — Software Development Plan

**Status:** Draft — 2026-03-23
**Prerequisite reading:** `log/j9t agentic AI in a terminal.txt`

---

## 1. Vision

Add a browser-based AI assistant terminal to JarvisAgent — a conversational,
project-aware agent that can inspect files, search code, run workflows, read
logs, propose fixes, and remember context across sessions.

The assistant lives in a new **"Assistant"** tab in the workflow editor React app,
rendered as an xterm.js terminal connected via WebSocket to a new C++ backend
subsystem.

### Levels of delivery (incremental)

| Level | Name | What it adds |
|-------|------|-------------|
| L1 | Chat terminal | Browser terminal, AI answers questions, `/run` `/status` `/help` commands, runtime context injection |
| L2 | Project-aware assistant | Persistent workspace memory, file reading/searching, log inspection, chunked context assembly |
| L3 | Agentic assistant | Multi-step tool-use loop, planning, file edits with approval, compile/test/fix cycle |

**This plan covers L1 and L2.** L3 is deferred — it builds naturally on L2 once
the tool system and memory layer are solid.

---

## 2. Architecture overview

```
┌─────────────────────────────────────┐
│  React App — "Assistant" tab        │
│  ┌───────────────────────────────┐  │
│  │  xterm.js terminal emulator   │  │
│  │  + markdown rendering panel   │  │
│  └──────────┬────────────────────┘  │
│             WS /ws/assistant        │
└─────────────┼───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│  C++ Backend                        │
│                                     │
│  AssistantController (WS handler)   │
│       │                             │
│       ├── SessionManager (sessions) │
│       ├── ContextAssembler          │
│       ├── ToolRegistry + Executor   │
│       ├── MemoryStore (text files)  │
│       └── AI provider (existing)    │
│                                     │
│  Existing infrastructure:           │
│  WebServer, AiRequestPool,          │
│  WorkflowRegistry, RuntimeManager,  │
│  PythonEngine, Log API              │
└─────────────────────────────────────┘
```

### Key design decisions

- **No SQL.** All persistence uses human-readable text files (JSON, JSONL,
  Markdown). Easy to debug, diff, inspect, and version-control. If performance
  becomes an issue later, we can add SQLite behind the same interface.
- **File-based transparency.** Every memory entry, context injection, and
  conversation turn is written to disk in a readable format. The user can open
  `assistant/` and see exactly what the AI knows.
- **Reuse existing AI provider stack.** The assistant uses the same
  `AiRequestPool` / `SessionManager` / `CurlWrapper` pipeline that workflows
  use. No new HTTP client code.
- **Separate WebSocket route.** `/ws/assistant` is a dedicated WS endpoint,
  independent of the existing `/ws` broadcast channel. This avoids polluting
  the workflow monitoring channel with chat traffic.

---

## 3. Storage layout (all text files)

```
assistant/                          # root — lives next to queue/ and workflows/
├── sessions/
│   ├── sess_1711234567.jsonl       # conversation history (append-only)
│   └── sess_1711234999.jsonl
├── memory/
│   ├── workspace.md                # project facts (auto-generated + user-pinned)
│   ├── rules.md                    # user-defined rules/conventions
│   ├── decisions.jsonl             # architectural decisions log
│   └── summaries/
│       ├── application_workflow.md  # per-folder summaries (auto-generated)
│       ├── workflow-editor_ui.md
│       └── ...
├── context/
│   └── last_injection.json         # snapshot of what was injected into the last prompt
│                                   # (for debugging — shows exactly what the AI saw)
└── index/
    ├── file_index.jsonl            # path, size, mtime, type, hash — built by indexer
    └── file_summaries.jsonl        # path, summary text, generated_at
```

### File formats

**Session history (`sessions/*.jsonl`)** — one JSON object per line:
```jsonl
{"role":"user","text":"Why did cyber2 fail?","ts":"2026-03-23T21:06:19Z"}
{"role":"assistant","text":"Looking at the logs...","ts":"2026-03-23T21:06:21Z","tools_used":["get_run_status"]}
{"role":"tool","name":"get_run_status","input":{"run_id":"cyber2_17743"},"output":{"state":"Failed","error":"..."},"ts":"2026-03-23T21:06:20Z"}
```

**Workspace memory (`memory/workspace.md`)** — Markdown, human-editable:
```markdown
# Workspace: JarvisAgent

- C++20 backend with Crow HTTP/WS server
- React 18 + Vite + TypeScript frontend (workflow editor)
- Workflow definitions are .jcwf JSON DAGs
- AI providers: OpenAI (API1/API2), Google Gemini (API3)
- Build: `make config=release && make config=debug`
- Python tasks use embedded CPython interpreter
- Queue folder holds task artifacts (STNG/TASK/CNTX/PROB/PROV files)
```

**Rules (`memory/rules.md`)** — user-pinned constraints:
```markdown
# Rules

- Prefer minimal dependencies
- Backend in modern C++20, keep Windows/macOS compatibility
- Use existing Crow WS stack — no new backend frameworks
- Never run sudo commands from the assistant
- Do not commit or push from the assistant
```

**Context injection log (`context/last_injection.json`)**:
```json
{
  "session_id": "sess_1711234567",
  "timestamp": "2026-03-23T21:06:19Z",
  "system_prompt_tokens": 1200,
  "memory_tokens": 340,
  "conversation_tokens": 2100,
  "tool_results_tokens": 800,
  "total_tokens": 4440,
  "injected_memory": ["workspace.md lines 1-8", "rules.md lines 1-6"],
  "injected_summaries": ["application_workflow.md"],
  "injected_tool_results": ["get_run_status(cyber2_17743)"]
}
```

This means you can always open `assistant/context/last_injection.json` and see
exactly what was sent to the AI and how the token budget was spent.

---

## 4. WebSocket protocol

Dedicated endpoint: `ws://localhost:8080/ws/assistant`

### Client → Server

```jsonc
// User message
{
  "type": "user_message",
  "sessionId": "sess_1711234567",   // empty string = create new session
  "text": "Why did the last workflow fail?"
}

// Slash command (parsed client-side for instant feedback, forwarded to backend)
{
  "type": "command",
  "sessionId": "sess_1711234567",
  "command": "run",
  "args": ["exampleMakefile"]
}

// Tool approval response
{
  "type": "approval_response",
  "requestId": "apr_42",
  "approved": true
}

// Session management
{ "type": "list_sessions" }
{ "type": "resume_session", "sessionId": "sess_1711234567" }
{ "type": "new_session" }
```

### Server → Client

```jsonc
// Streamed assistant text (delta mode — append to current message)
{
  "type": "assistant_delta",
  "sessionId": "sess_1711234567",
  "text": "Let me check the recent runs..."
}

// Assistant finished responding
{
  "type": "assistant_done",
  "sessionId": "sess_1711234567"
}

// Tool call in progress (shown as status line in terminal)
{
  "type": "tool_status",
  "sessionId": "sess_1711234567",
  "tool": "search_files",
  "status": "running",
  "detail": "Searching 142 files for 'webhook callback'..."
}

// Tool result (optionally shown to user, always logged)
{
  "type": "tool_result",
  "sessionId": "sess_1711234567",
  "tool": "search_files",
  "ok": true,
  "summary": "Found 3 matches in webServer.cpp, webhookHandler.cpp, triggerEngine.cpp"
}

// Approval request (blocks until user responds)
{
  "type": "approval_request",
  "requestId": "apr_42",
  "sessionId": "sess_1711234567",
  "action": "run_shell",
  "description": "Run: make config=release",
  "risk": "low"
}

// Session list
{
  "type": "session_list",
  "sessions": [
    {"id": "sess_1711234567", "started": "2026-03-23T21:06:19Z", "turns": 12, "summary": "Debugging cyber2 failure"}
  ]
}

// Error
{
  "type": "error",
  "sessionId": "sess_1711234567",
  "message": "AI provider timeout after 30s"
}
```

---

## 5. Backend modules

### 5.1 AssistantController (`assistantController.h/.cpp`)

- Owns the `/ws/assistant` WebSocket route (registered in `WebServer`)
- Parses incoming messages, routes to appropriate handler
- Manages per-connection state (which session is active)
- Streams AI responses back to the client as deltas
- Handles slash commands (`/run`, `/status`, `/help`, `/memory`, `/forget`)

### 5.2 AssistantSession (`assistantSession.h/.cpp`)

- One instance per conversation session
- Owns the JSONL history file (append-only writes)
- Tracks: session ID, start time, turn count, active tool calls
- Provides `GetRecentTurns(maxTokens)` — returns the most recent conversation
  turns that fit within a token budget (simple token estimator: chars / 4)
- Provides `AddUserMessage()`, `AddAssistantMessage()`, `AddToolResult()`
- On creation, generates a session ID from timestamp
- On resume, reads existing JSONL file

### 5.3 MemoryStore (`memoryStore.h/.cpp`)

- Reads/writes `assistant/memory/` directory
- `GetWorkspaceMemory()` → returns `workspace.md` content
- `GetRules()` → returns `rules.md` content
- `SaveMemory(type, content)` → appends to appropriate file
- `PinRule(content)` → appends to `rules.md`
- `SearchMemory(query)` → simple substring/keyword search across memory files
- `GetFolderSummary(folderPath)` → returns cached summary or empty string
- `SaveFolderSummary(folderPath, summary)` → writes to `memory/summaries/`
- All operations are file I/O with mutex protection
- **No embeddings in v1.** Keyword/substring search is sufficient for the
  memory sizes we'll have. Semantic search can be added later.

### 5.4 ContextAssembler (`contextAssembler.h/.cpp`)

Assembles the full prompt for each AI call. This is the critical module that
determines what the AI sees.

**Token budget allocation** (configurable, default 16K context):

| Slot | Budget | Source |
|------|--------|--------|
| System prompt | ~800 tokens | Static template + project type |
| Rules | ~200 tokens | `memory/rules.md` (truncated if over budget) |
| Workspace memory | ~400 tokens | `memory/workspace.md` (truncated if over budget) |
| Relevant summaries | ~600 tokens | Folder/file summaries matching query keywords |
| Tool results | ~2000 tokens | Results from tool calls in current turn |
| Conversation history | ~4000 tokens | Recent turns (newest first, trim oldest) |
| User message | variable | Current user input |
| **Reserved for response** | ~8000 tokens | Left for AI to generate |

**Assembly algorithm:**
1. Start with system prompt (always included)
2. Append rules (always included, truncate if huge)
3. Append workspace memory (always included)
4. Run keyword extraction on user message
5. Find matching folder summaries → append top matches within budget
6. Append tool results from current turn (if any)
7. Fill remaining budget with conversation history (newest first)
8. Write assembled context to `assistant/context/last_injection.json`
9. Send to AI provider

### 5.5 ToolRegistry and ToolExecutor (`assistantTools.h/.cpp`)

**L1 tools (chat terminal):**

| Tool | Args | Description | Approval |
|------|------|-------------|----------|
| `get_system_status` | — | Returns JarvisAgent status (uptime, active runs, Python state) | No |
| `list_workflows` | — | Returns workflow IDs + labels | No |
| `run_workflow` | `workflow_id` | Starts a workflow run | Yes |
| `get_run_status` | `run_id` | Returns run state + task states | No |
| `get_task_output` | `run_id, task_id` | Returns captured stdout/stderr | No |
| `list_recent_runs` | `count?` | Returns last N completed/failed runs | No |

**L2 tools (project-aware):**

| Tool | Args | Description | Approval |
|------|------|-------------|----------|
| `read_file` | `path, start?, end?` | Reads file content (line range) | No |
| `search_files` | `query, glob?` | Grep-style search (shells out to `rg` or built-in) | No |
| `list_files` | `path, depth?` | Lists directory contents | No |
| `get_log_tail` | `lines?` | Returns last N log lines | No |
| `analyze_last_run` | `index?` | Returns structured run analysis | No |
| `save_memory` | `content, type?` | Saves a fact/note to workspace memory | **Yes** |
| `pin_rule` | `content` | Adds a rule to rules.md | **Yes** |
| `run_shell` | `command, cwd?` | Executes a shell command | **Yes** |

**L2 tools (JCWF development — see §14):**

| Tool | Args | Description | Approval |
|------|------|-------------|----------|
| `jcwf_read` | `workflow_id` | Reads a JCWF file and returns its full JSON content | No |
| `jcwf_read_plan` | `workflow_id` | Reads the markdown development plan for a JCWF | No |
| `jcwf_write_plan` | `workflow_id, content` | Creates or updates the markdown development plan | **Yes** |
| `jcwf_generate` | `workflow_id` | Generates/regenerates a JCWF from its markdown plan using the existing batched generate pipeline | **Yes** |
| `jcwf_explain` | `workflow_id, task_ids?` | Explains the JCWF: lists and numbers all task nodes, describes edges, triggers, and data flow. If `task_ids` given, explains only those nodes. | No |
| `jcwf_fix_task` | `workflow_id, task_id, instructions` | Fixes a single task node based on review of errors, logs, and user instructions | **Yes** |
| `jcwf_write_script` | `path, content, type` | Writes a Python or shell script referenced by a JCWF task | **Yes** |
| `jcwf_validate` | `workflow_id` | Validates a JCWF against the JC Workflow Specification (structure, edges, required fields) | No |

**L2 tools (runtime control):**

| Tool | Args | Description | Approval |
|------|------|-------------|----------|
| `workflow_start` | `workflow_id` | Starts a workflow run | **Yes** |
| `workflow_pause` | `run_id` | Pauses a running workflow | **Yes** |
| `workflow_resume` | `run_id` | Resumes a paused workflow | **Yes** |
| `workflow_stop` | `run_id` | Stops/cancels a running workflow | **Yes** |
| `get_dashboard_status` | — | Returns a full status report: registered JCWFs, active runs, JCWFs in flight, AI queries in flight, warnings, errors, Python engine state | No |

**Tool call flow:**
1. AI response includes a structured tool call (JSON in response text, parsed by backend)
2. Backend validates tool name + args against registry
3. If approval required → send `approval_request` to client, block until response
4. Execute tool, capture output
5. Append tool result to conversation
6. Re-send to AI with tool result for follow-up response
7. Log tool invocation to session JSONL

**Tool call format in AI responses:**
The system prompt instructs the AI to emit tool calls as:
```
<tool_call>{"name": "read_file", "args": {"path": "application/web/webServer.cpp", "start": 1074, "end": 1108}}</tool_call>
```
The backend parses `<tool_call>...</tool_call>` blocks from the response text.
This is simpler than function-calling APIs and works with any provider.

### 5.6 WorkspaceIndexer (`workspaceIndexer.h/.cpp`)

Background indexer that maintains `assistant/index/file_index.jsonl`.

**Runs on:**
- Startup (full scan)
- Periodically (every 60s, incremental — only re-index changed files by mtime)
- On-demand (when assistant needs fresh data)

**Index entry:**
```json
{"path": "application/web/webServer.cpp", "size": 198432, "mtime": 1711234567, "type": "cpp", "lines": 4945}
```

**File classification:**
- `source` — .cpp, .h, .py, .ts, .tsx, .js, .css
- `config` — .json, .yaml, .toml, .md (in root or doc/)
- `workflow` — .jcwf
- `script` — files in scripts/
- `generated` — files in queue/, node_modules/, build/, bin/
- `binary` — images, executables, archives (skip)

**What the indexer does NOT do in v1:**
- No embeddings
- No AST parsing
- No symbol extraction
- These are L3 features. Keyword search + file summaries are sufficient for L2.

---

## 6. Frontend

### 6.1 New tab: "Assistant"

Add an "Assistant" button to the existing header bar in `App.tsx`, between
"Log" and "Dashboard".

### 6.2 Terminal component (`AssistantTerminal.tsx`)

- Uses **xterm.js** + `xterm-addon-fit` for a real terminal feel
- Custom input handling: captures user input line-by-line
- Renders assistant responses with basic formatting:
  - Code blocks with syntax highlighting (via simple regex coloring)
  - Bold/italic via ANSI escape codes
  - Tool status lines in dim/grey
  - Errors in red
- Shows approval prompts inline: `[Y/n] Run: make config=release`
- Session selector dropdown at top (resume previous / start new)

### 6.3 Auto-completion (keystroke-by-keystroke)

The terminal provides inline auto-completion suggestions as the user types,
rendered as dimmed ghost text after the cursor (like fish shell / GitHub
Copilot CLI).

**Completion sources** (checked on every keystroke, debounced 100ms):

| Source | Trigger | Examples |
|--------|---------|----------|
| **Slash commands** | Input starts with `/` | `/run`, `/status`, `/memory show` |
| **Workflow IDs** | After `/run ` | `/run exampleMakefile`, `/run cyber2` |
| **Command history** | Any input | Previous user messages matching prefix |
| **File paths** | Input contains path-like tokens (`.` or `/`) | `application/web/webServer.cpp` |

**Implementation:**

1. Client maintains a local completion index:
   - Slash command list (static)
   - Workflow IDs (fetched from `/api/workflows` on tab open, cached)
   - Session command history (all user messages from current + past sessions)
   - File path list (fetched from backend `list_files` on tab open, refreshed
     on `/index` or every 60s)
2. On each keystroke, run prefix match against all sources, pick best match
3. Render suggestion as dim ANSI text after cursor position
4. **Tab** accepts the suggestion (fills input buffer)
5. **→ (Right arrow)** accepts one character at a time
6. **Esc** or any non-matching keystroke dismisses the suggestion
7. If multiple matches exist, **Tab** cycles through them; a small popup
   shows the candidate list (max 8 items) anchored above the input line

**Protocol addition** — server assists with completions for dynamic data:
```jsonc
// Client → Server (debounced, only when local sources have no match)
{ "type": "completion_request", "prefix": "application/w", "kind": "path" }

// Server → Client
{ "type": "completion_response", "candidates": ["application/web/", "application/workflow/"] }
```

The backend serves completion requests from the `WorkspaceIndexer` file index.
This is a fast lookup (no AI call), so latency stays under 10ms.

### 6.4 Reverse history search (Ctrl+R)

Standard reverse-incremental-search, modeled after bash/zsh `Ctrl+R`:

1. **Ctrl+R** enters search mode — prompt changes to `(reverse-search): `
2. Each keystroke filters the full session command history (all sessions,
   newest first) for entries containing the typed substring
3. The best match is displayed inline with the matching portion highlighted
4. **Ctrl+R** again cycles to the next older match
5. **Enter** executes the matched command
6. **Tab** or **→** places the matched command on the input line for editing
7. **Esc** or **Ctrl+C** exits search mode without selecting
8. **Ctrl+S** reverses direction (forward search through matches)

**History storage:** All user messages are already persisted in session JSONL
files. On tab open, the client requests the full command history:

```jsonc
// Client → Server
{ "type": "get_history", "maxEntries": 500 }

// Server → Client
{ "type": "history", "entries": [
    {"text": "/run exampleMakefile", "ts": "2026-03-23T21:06:19Z", "sessionId": "sess_171..."},
    {"text": "Why did cyber2 fail?", "ts": "2026-03-23T21:05:02Z", "sessionId": "sess_171..."}
]}
```

The history is cached client-side and appended to as the user sends new
messages. Search is performed entirely client-side (no server round-trip
during typing).

### 6.5 Keyboard shortcuts

| Shortcut | Action |
|----------|--------|
| **Enter** | Send message / execute command |
| **Tab** | Accept auto-completion suggestion; cycle candidates |
| **→** | Accept one character of suggestion |
| **Ctrl+R** | Reverse history search |
| **Ctrl+S** | Forward history search |
| **Ctrl+C** | Cancel current input / abort search / cancel pending approval |
| **Ctrl+L** | Clear terminal display (same as `/clear`) |
| **Ctrl+U** | Clear input line |
| **Ctrl+A** | Move cursor to start of input |
| **Ctrl+E** | Move cursor to end of input |
| **↑ / ↓** | Navigate command history (previous / next) |
| **Esc** | Dismiss auto-completion / exit search mode |
| **Ctrl+D** | Close assistant session (with confirmation) |

### 6.6 WebSocket connection

- Connects to `ws://localhost:8080/ws/assistant` when Assistant tab is active
- Reconnects on disconnect (same pattern as existing `/ws` connection)
- Buffers deltas and renders progressively (streaming feel)
- On reconnect, re-fetches history and completion data silently

### 6.7 Slash commands (client-side shortcuts)

| Command | Action |
|---------|--------|
| `/help` | Show available commands |
| `/run <workflow>` | Trigger a workflow run |
| `/status` | Show system status |
| `/runs` | List recent runs |
| `/memory` | Show current workspace memory |
| `/rules` | Show current rules |
| `/pin <text>` | Pin a new rule |
| `/forget <text>` | Remove a memory entry |
| `/sessions` | List previous sessions |
| `/new` | Start a new session |
| `/clear` | Clear terminal display (not history) |

These are parsed client-side and sent as `"type": "command"` messages.
The backend handles them directly without AI involvement (fast path).

---

## 7. AI provider integration

### System prompt template

```
You are the JarvisAgent AI Assistant — a project-aware coding assistant
embedded in the JarvisAgent workflow automation system.

You help the user understand, debug, and manage their workflows and codebase.
You can read files, search code, inspect logs, check run status, and run
shell commands (with user approval).

When you need to use a tool, emit a tool call block:
<tool_call>{"name": "tool_name", "args": {... }}</tool_call>

Available tools:
[... tool descriptions injected here ...]

Project context:
[... workspace memory injected here ...]

Rules:
[... rules.md injected here ...]

Recent conversation:
[... last N turns injected here ...]
```

### Streaming

- Use the existing `AiRequestPool` / `SessionManager` pipeline
- Poll for partial responses and stream deltas to the client via WS
- If the provider supports streaming (SSE), use it; otherwise poll completion

### Provider selection

- Uses the **default AI interface** from config.json (same as workflow runs)
- Future: allow per-session provider override (like JCWF AI Interface setting)

---

## 8. Implementation phases

### Phase 1: Skeleton (L1 — chat terminal)

**Goal:** User can open Assistant tab, type a message, get an AI response.

1. Create `assistant/` directory structure on startup
2. Add `/ws/assistant` WebSocket route in `WebServer`
3. Implement `AssistantController` — parse messages, create sessions
4. Implement `AssistantSession` — JSONL history read/write
5. Implement basic `ContextAssembler` — system prompt + conversation history only
6. Wire AI call through `AiRequestPool` → stream response back via WS
7. Frontend: add "Assistant" tab + xterm.js terminal + WS connection
8. Implement `/help`, `/status`, `/runs` slash commands
9. Build and test end-to-end: type question → get AI answer

**Deliverables:** Working chat terminal with conversation persistence.

### Phase 2: Tools (L1 → L2 bridge)

**Goal:** AI can call tools to inspect the system.

1. Implement `ToolRegistry` with tool schemas
2. Implement `ToolExecutor` with L1 tools (`get_system_status`, `list_workflows`,
   `run_workflow`, `get_run_status`, `get_task_output`, `list_recent_runs`)
3. Parse `<tool_call>` blocks from AI responses
4. Execute tool → append result → re-send to AI for follow-up
5. Add approval flow for `run_workflow`
6. Stream tool status events to frontend
7. Add L2 read-only tools (`read_file`, `search_files`, `list_files`,
   `get_log_tail`, `analyze_last_run`)
8. Add `run_shell` with mandatory approval

**Deliverables:** AI can inspect files, search code, check status, run commands.

### Phase 3: Memory (L2)

**Goal:** Assistant remembers project context across sessions.

1. Implement `MemoryStore` — read/write `workspace.md`, `rules.md`, `decisions.jsonl`
2. Implement `save_memory` and `pin_rule` tools
3. Implement `/memory`, `/rules`, `/pin`, `/forget` slash commands
4. Seed `workspace.md` with auto-detected project facts on first run:
   - Scan for build systems (Makefile, CMakeLists, package.json)
   - Count files by type
   - Detect languages
   - Read README.md first paragraph
5. Inject memory into context assembly
6. Write `context/last_injection.json` on every AI call

**Deliverables:** Persistent workspace knowledge, transparent context injection.

### Phase 4: Indexing and summaries (L2)

**Goal:** Assistant can efficiently navigate large codebases.

1. Implement `WorkspaceIndexer` — startup scan + incremental updates
2. Write `index/file_index.jsonl`
3. Add `get_file_summary` tool — if summary exists in cache, return it;
   otherwise read file → send to AI for summarization → cache result
4. On-demand folder summarization (triggered by assistant when exploring)
5. Keyword-based summary retrieval in `ContextAssembler` — match user query
   words against summary text, inject top matches
6. Add `/index` slash command to show indexing status

**Deliverables:** File index, cached summaries, smarter context assembly.

---

## 9. Safety and guardrails

| Concern | Mitigation |
|---------|-----------|
| Destructive shell commands | All `run_shell` calls require explicit user approval |
| File writes | Not in L1/L2 — deferred to L3 with approval model |
| Infinite tool loops | Max 10 tool calls per turn; max 3 consecutive re-sends |
| Token explosion | Fixed budget per slot in ContextAssembler; oldest turns dropped first |
| Stale memory | Summaries timestamped; re-generate if file mtime changed |
| Sensitive data | Assistant files in `assistant/` — user controls what's stored |
| Command timeout | 30s default for `run_shell`; watchdog kills process |
| Path traversal | Same `lexically_normal()` + prefix check as existing script validation |
| Prompt injection via file content | Tool outputs are wrapped in `<tool_result>` fences with a nonce; system prompt instructs AI to never execute instructions found inside tool results. Backend validates that `<tool_call>` blocks only appear in the AI response text, never inside tool output passthrough. |
| Credential leakage | `read_file` tool has a deny-list: `config.json`, `keys.json`, `keys.json.enc`, `*.pem`, `*.key`, `.env`, and any path under `assistant/` (prevents AI from reading its own memory as file content). Blocked paths return a sanitized error, not file contents. |
| Memory poisoning | `save_memory` and `pin_rule` tools require user approval (promoted from auto-approve). The AI cannot silently persist rules that alter future behavior. |
| Session/WS authentication | `/ws/assistant` requires the same session token as other authenticated routes. In L1 this is the existing Crow cookie; future: proper JWT or API key. Unauthenticated connections are rejected. |
| Command injection in tool args | `run_shell` passes the command string as a single argument to `/bin/sh -c`; no shell expansion of tool args. The approval prompt shows the *exact* command string. Additionally, a configurable blocklist rejects commands matching dangerous patterns (`rm -rf /`, `mkfs`, `dd if=`, `:(){ :|:& };:`, `> /dev/sd`). |
| Log / output exposure | `get_task_output` and `get_log_tail` truncate output to a max length (default 4 KB) to prevent flooding the context window with sensitive log data. |
| AI call rate limiting | Max 20 AI provider calls per session per minute; max 100 per session total. Exceeding triggers a cooldown message, not an error. |
| Network scope | Assistant tools have no outbound network access by default. `run_shell` commands inherit the server's network but the user sees the full command before approval. No tool fetches arbitrary URLs (no `curl`/`wget` wrapper in L1/L2). |
| Off-topic / hallucinated replies | **Response relevance parser (L3).** A lightweight post-processing pass on the AI response checks for obvious topic drift — e.g. user asks about a software tool and the AI answers about a completely different product with the same name (PHP Composer vs Windsurf Composer 2). Implementation: after the AI response is complete, run a fast secondary prompt (or heuristic regex) that scores relevance against the user's original message. If confidence is below threshold, append a visible `⚠️ The answer may be off-topic — consider rephrasing your question.` warning to the streamed output. This also catches hallucinated package names, wrong API endpoints, and invented CLI flags. |

---

## 10. Dependencies

| Dependency | Purpose | Status |
|-----------|---------|--------|
| xterm.js | Terminal emulator in browser | New npm dependency |
| xterm-addon-fit | Auto-resize terminal | New npm dependency |
| Crow WebSocket | WS transport | Already in stack |
| AiRequestPool | AI provider calls | Already in stack |
| SessionManager | HTTP → AI provider | Already in stack |
| ripgrep (`rg`) | Fast code search | Optional; falls back to recursive `grep` |

No new C++ libraries required. The backend is pure C++20 + existing vendors.

---

## 11. Files to create/modify

### New C++ files
- `application/assistant/assistantController.h/.cpp`
- `application/assistant/assistantSession.h/.cpp`
- `application/assistant/contextAssembler.h/.cpp`
- `application/assistant/memoryStore.h/.cpp`
- `application/assistant/assistantTools.h/.cpp`
- `application/assistant/workspaceIndexer.h/.cpp`

### Modified C++ files
- `application/web/webServer.h/.cpp` — add `/ws/assistant` route, `AssistantController` member
- `application/jarvisAgent.h/.cpp` — create `AssistantController` on startup, wire to WebServer
- `premake5.lua` — add new source files to build

### New frontend files
- `workflow-editor/ui/src/views/AssistantView.tsx` — main view component
- `workflow-editor/ui/src/components/AssistantTerminal.tsx` — xterm.js wrapper
- `workflow-editor/ui/src/api/assistant.ts` — WS connection + message types

### Modified frontend files
- `workflow-editor/ui/src/App.tsx` — add Assistant tab/route
- `workflow-editor/ui/package.json` — add xterm.js dependencies

---

## 12. Non-goals (explicitly deferred)

- **SQL / SQLite** — not in v1. Text files are debuggable and sufficient.
- **Embeddings / vector search** — not in v1. Keyword search is sufficient.
- **AST parsing / symbol extraction** — not in v1. File summaries cover this.
- **File editing by the assistant** — L3 feature, requires approval model.
- **Multi-user sessions** — single-user for now (matches JarvisAgent model).
- **Git integration** — deferred. User manages git manually.
- **Autonomous multi-step planning** — L3 feature.

---

## 13. Success criteria

**Phase 1 done when:**
- User can open Assistant tab, type a question, get a streamed AI response
- Conversation persists in JSONL file, survives page refresh
- `/help`, `/status`, `/runs` commands work

**Phase 2 done when:**
- AI can read files, search code, check run status via tool calls
- Tool status shown in terminal ("Searching 142 files...")
- `run_shell` requires and respects user approval

**Phase 3 done when:**
- `assistant/memory/workspace.md` auto-seeded on first run
- User can `/pin` rules that persist across sessions
- `context/last_injection.json` shows exactly what the AI saw

**Phase 4 done when:**
- File index built on startup, updated incrementally
- AI can request and cache file/folder summaries
- Context assembly pulls relevant summaries based on query

---

## 14. JCWF Development Capabilities

The assistant can create, explain, fix, and iteratively improve JC Workflows
(JCWFs) through a **markdown-plan-first** workflow. It can also write the
Python and shell scripts that JCWF tasks reference, and control workflow
execution (start / pause / resume / stop).

### 14.1 Markdown-first development model

Every JCWF managed by the assistant has an associated **development plan** — a
Markdown document stored alongside the `.jcwf` file:

```
workflows/
├── myPipeline.jcwf
├── myPipeline.plan.md          ← development plan (source of truth for intent)
├── scripts/
│   ├── myPipeline_extract.py   ← Python script referenced by a task
│   └── myPipeline_report.sh    ← shell script referenced by a task
```

**Rule: plan must be up to date before any JCWF modification.**

Before generating or modifying a JCWF, the assistant checks file dates:

| Condition | Action |
|-----------|--------|
| No `.plan.md` exists | Create the plan first from the current JCWF (reverse-document it) |
| `.jcwf` mtime > `.plan.md` mtime | The JCWF was edited outside the assistant — update the plan first to reflect current state |
| `.plan.md` mtime ≥ `.jcwf` mtime | Plan is current — proceed with generation/modification |

This ensures the Markdown plan is always the **source of truth for design
intent**, even if the user edits the JCWF manually in the workflow editor.

### 14.2 Plan document format

```markdown
# myPipeline — Development Plan

## Purpose
One-paragraph description of what this workflow does.

## Trigger
- Type: file_watch / manual / webhook / cron
- Details: watched path, schedule, etc.

## Task nodes

### 1. extractData (shell)
- Script: `scripts/myPipeline_extract.py`
- Inputs: `message.txt` from trigger
- Outputs: `extracted.json`
- Working directory: (relative to jcwf)
- Error handling: on failure → goto errorReport

### 2. analyzeData (ai_call)
- AI interface: API1
- System prompt: "You are a data analyst..."
- Task prompt: contents of `extracted.json`
- Outputs: `analysis.prob`

### 3. errorReport (shell)
- Script: `scripts/myPipeline_report.sh`
- Inputs: error output from failed upstream task
- Trigger condition: only on error from extractData

## Edges
- extractData → analyzeData (success)
- extractData → errorReport (failure)

## Notes
- Requires API key for OpenAI (API1)
- extract script needs `jq` installed
```

### 14.3 Generation pipeline (reuses existing AiJcwfService)

The assistant delegates JCWF generation to the **same batched pipeline** used
by the workflow editor's generate/explain feature:

1. **Plan → task descriptions** — the assistant extracts per-task specs from
   the Markdown plan
2. **Batched generation** — tasks are sent to the AI in batches (existing
   batch-size logic), each task generated independently
3. **Early validation** — each generated task is validated against the JC
   Workflow Specification before proceeding
4. **Script generation** — for `shell` tasks, the assistant generates the
   referenced Python or shell scripts and writes them via `jcwf_write_script`
5. **Final assembly** — all tasks + edges assembled into the `.jcwf` JSON
6. **Full validation** — the complete JCWF is validated (structure, edges,
   required fields, script paths exist)
7. **Plan update** — the `.plan.md` is updated with any adjustments made
   during generation (e.g. added error handling, renamed outputs)

### 14.4 Fixing workflows — one task at a time

When a workflow fails, the assistant can diagnose and fix it incrementally:

1. **Review the JCWF** — `jcwf_read` to load the workflow definition
2. **Review the log** — `get_log_tail` / `read_file` on `log/log.txt` to find
   error messages
3. **Review run output** — `get_run_status` + `get_task_output` to see which
   task failed and its stdout/stderr
4. **Explain the failure** — present the user with a numbered list of task
   nodes, highlight which one failed and why
5. **Fix one task** — `jcwf_fix_task` modifies a single task node based on the
   diagnosis (e.g. fix a script path, correct an edge, adjust a prompt)
6. **Re-validate** — `jcwf_validate` to confirm the fix didn't break anything
7. **Update the plan** — `jcwf_write_plan` to reflect what was fixed and why
8. **Re-run** — optionally `workflow_start` to test the fix

The assistant fixes **one task node at a time** to keep changes small,
reviewable, and reversible.

### 14.5 Interactive JCWF discussion

The assistant can discuss a JCWF with the user conversationally:

- **Explain** — "Explain this workflow" → numbered list of all task nodes with
  their type, inputs, outputs, and connections
- **Drill down** — "What does task 3 do?" → detailed explanation of a specific
  node including its script content
- **Suggest improvements** — "The error output isn't wired" → assistant
  identifies the missing error branch and offers to create it
- **Apply user requests** — "Change the trigger to cron every 5 minutes" →
  assistant updates both the plan and the JCWF
- **Wire error branches** — "Add error handling for task 2" → assistant creates
  an error-handling task node, writes the script if needed, and wires the
  failure edge from task 2
- **Refactor** — "Split task 4 into two steps" → assistant updates the plan,
  regenerates affected tasks, rewires edges

All modifications go through the plan-first flow: update `.plan.md`, then
regenerate or patch the `.jcwf`.

### 14.6 Runtime control and status reporting

The assistant can control workflow execution and provide system-wide status:

**Control commands:**

| Command | Tool | Description |
|---------|------|-------------|
| "Start myPipeline" | `workflow_start` | Queues a new run |
| "Pause run 42" | `workflow_pause` | Pauses a running workflow (tasks in flight complete, no new tasks start) |
| "Resume run 42" | `workflow_resume` | Resumes a paused workflow |
| "Stop run 42" | `workflow_stop` | Cancels a running workflow |

**Status reporting:**

"What's the status?" → `get_dashboard_status` returns:

```
JarvisAgent Status
──────────────────
Registered workflows:  6
Active runs:           2  (myPipeline: running, cyber2: paused)
JCWFs in flight:       1
AI queries in flight:  3
Completed runs:       14
Failed runs:           2
Warnings:              1  (cyber2: paused for >10 min)
Errors:                0
Python engine:         ready
Uptime:                2h 34m
```

### 14.7 Slash commands (JCWF-specific)

| Command | Action |
|---------|--------|
| `/explain <workflow>` | Explain workflow — numbered task list with connections |
| `/fix <workflow>` | Review last failure + suggest fix |
| `/generate <workflow>` | Generate JCWF from its plan |
| `/validate <workflow>` | Validate JCWF against spec |
| `/start <workflow>` | Start a workflow run |
| `/pause <run_id>` | Pause a running workflow |
| `/resume <run_id>` | Resume a paused workflow |
| `/stop <run_id>` | Stop/cancel a running workflow |
| `/dashboard` | Show full system status |
