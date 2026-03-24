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
| `save_memory` | `content, type?` | Saves a fact/note to workspace memory | No |
| `pin_rule` | `content` | Adds a rule to rules.md | No |
| `run_shell` | `command, cwd?` | Executes a shell command | **Yes** |

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

### 6.3 WebSocket connection

- Connects to `ws://localhost:8080/ws/assistant` when Assistant tab is active
- Reconnects on disconnect (same pattern as existing `/ws` connection)
- Buffers deltas and renders progressively (streaming feel)

### 6.4 Slash commands (client-side shortcuts)

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
