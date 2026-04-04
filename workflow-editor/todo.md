# Workflow Editor — TODO & Reference

Last reviewed: 2026-03-23

---

## Completed Features

- [x] ~~React + Vite + TypeScript scaffold, Crow serving at `/editor`~~
- [x] ~~React Flow canvas with custom `TaskNode`, dependency edges, auto-layout~~
- [x] ~~Workflow CRUD (Open / Save / Save As / Validate / Run / Clean)~~
- [x] ~~Undo / redo (Ctrl+Z / Ctrl+Shift+Z)~~
- [x] ~~Trigger editing (auto, manual, cron, file_watch)~~
- [x] ~~Backend validation with severity tiers (A–D), displayed in sidebar + node badges~~
- [x] ~~Filter builder dialog (csv, text_lines, query, polarion_query)~~
- [x] ~~Template browser with starter workflows~~
- [x] ~~MiniMap, zoom controls, debug panel~~
- [x] ~~Live run monitoring via WebSocket (task state badges on nodes)~~
- [x] ~~Workflow-level round-trip fix (`defaults`, `label`, `doc`, `base_directory` preserved)~~
- [x] ~~queue_binding editor (`QueueBindingEditor.tsx` — STNG/TASK/CNTX/PROB sections)~~
- [x] ~~file_inputs / file_outputs editor (inline list editors for shell/python)~~
- [x] ~~Workflow-level defaults editor (`timeout_ms`, `ai.provider`, `ai.model`)~~
- [x] ~~Workflow-level label and doc editor~~
- [x] ~~Dataflow editor (dashed green edges, `df:` prefix, output/input port handles)~~
- [x] ~~inputs/outputs slot editor (name, type, required)~~
- [x] ~~Per-task timeout_ms editor~~
- [x] ~~Runtime visualization (running=yellow+pulse, failed=bold red+glow, skipped=green dimmed)~~
- [x] ~~Stop/Pause/Resume buttons~~ — fully functional with iconic controls (▶ ❚❚ ■), PAUSED banner, ❚❚ badges on queued nodes, `WorkflowRunState::Stopped` terminal state
- [x] ~~Shell task stdout/stderr capture (hover tooltip + side panel, stderr in red)~~ — tooltip is scrollable and persistent on mouse-over
- [x] ~~Dirty indicator~~ — `computeGraphSignature` serializes entire task object; all field changes trigger asterisk
- [x] ~~Script path validation (`GET /api/scripts/check`, inline warnings)~~
- [x] ~~Dashboard Log Viewer (virtual scroll, delta polling, search, analyze run panel)~~
- [x] ~~Log analyze: multi-run cycling (◀/▶), issue filtering by runId/workflowId~~
- [x] ~~JCWF generation assistant~~ — "Generate" button: multi-stage AI pipeline (decompose → generate JCWF with batched fan-out → generate scripts → review → validate → fix). Supports both Python and **shell (bash)** script generation with POSIX awk rules, host OS detection, and positional arg mapping. "Explain" button: AI summarizes loaded workflow. **Fix Script** button: sends failed script + stderr to AI, user reviews fix in `ScriptReviewPanel`. Backend: `AiJcwfService` in `aiJcwfService.cpp`. E2E verified with `cyber2` (Python) and `cyber3` (shell). See `example/workflows/cyber2_e2e.md` and `cyber3_e2e.md`.
- [x] ~~AI Assistant terminal~~ — "Assistant" tab with xterm.js terminal connected via `/ws/assistant` WebSocket. Conversational AI with 31 tools: read-only queries (file read/search, workflow status, logs), memory persistence, workspace file indexing with cached AI summaries, mutating tools (`run_shell`, `write_file`, `edit_file`), JCWF development tools (8), runtime control tools (4). Multi-step tool loop (max 10 iterations), user approval flow for mutating tools (60s timeout), loop detection, response validation (keyword overlap + path existence). Ghost-text auto-completion + Ctrl+R history search. Slash commands: `/help`, `/status`, `/runs`, `/log`, `/memory`, `/index`, `/sessions`, `/new`, `/clear`. Assistant button greyed out when no AI provider configured. Backend: `application/assistant/` (6 C++ modules). 70-test suite in `test/assistant/`. See `application/assistant/ai-assistant.md`.

---

## Remaining Work

### ~~Disclaimer~~ ✅
- [x] ~~Add a disclaimer/about section somewhere in the UI~~ — Added to `SettingsModal.tsx` footer: logo + "JarvisAgent v0.8.5 · MIT License · © 2026 JC Technolabs · GitHub (link)".

### ~~Unify look & feel~~ ✅
- [x] ~~Dashboard updated to match editor: sans-serif font, `#0b0f14` background, `#e8eef5` text, blue glassmorphic buttons/panels/borders, blue active tab accent~~
- Dashboard `App.css`: font, background, color, status bar, all buttons neutral glassmorphic (blue only on active tab), panels (translucent rgba), tab bar (blue active), log viewer toolbar/buttons, log highlights, analyze panel, master password dialog. Action buttons (Workflow Editor, Quit, Run) are neutral — color only conveys active/selected state.
- [x] ~~Log analyze panel was translucent (`rgba(255,255,255,0.03)`) — unreadable against the log background~~ — Fixed: `.log-analyze-panel` background changed to solid `#1e1e1e`, border to `rgba(255,255,255,0.12)`, matching editor Settings modal and AI Manager modal solid style.

### Editor UI
- [x] ~~Box select: Shift+drag on canvas opens a selection rectangle (partial intersection mode). Implemented via `SelectionMode.Partial` + `selectionOnDrag={shiftHeld}` in `WorkflowEditorView.tsx`.~~
- [x] ~~Generated Scripts review panel UX facelift — per-script accept/reject buttons,
      collapsible taller code editor, sticky action bar (Accept All / Skip always visible),
      clearer visual hierarchy for the review step.~~
- [x] ~~"Accept & Save All" combined button after AI generation — when neither the JCWF nor
      the generated scripts are saved yet, offer a single action that writes the scripts to
      disk and saves the JCWF in one step. Currently the user must accept scripts and save
      the workflow separately, which is confusing if they hit Run before either is persisted.~~
      Resolved: Run now auto-flushes pending AI scripts before saving JCWF and starting the run.
- [x] ~~AI test button~~ — direct curl ping with 10s timeout (bypasses SessionManager). Backend: `POST /api/settings/ai-interfaces/test`, `TestAiInterface()` in `aiJcwfService`. Frontend: test button + LED indicator in `AiManagerView.tsx`. Bad URLs fail instantly (e.g. HTTP 404), no more 2-minute hang.
- [x] ~~AI Manager edit modal~~ — centered overlay replaces inline edit form at bottom of provider list. Click-outside-to-close. Updated 2026-03-23: restyled to use `modalOverlay`/`modalContent`/`modalHeader`/`modalBody` CSS classes matching `SettingsModal.tsx` (solid dark background, no transparency, no blur).
- [x] ~~Settings dialog for workflow editor~~ — config.json fields exposed in the gear-icon Settings modal. Backend: `GET/PUT /api/settings/config`. Frontend: `SettingsModal.tsx` with Default AI Interface, Max Threads, Max File Size, JCWF Batch Size, Verbose toggle.
- [x] ~~Template variable autocomplete for `{{binding.field}}` in queue_binding prob_files~~ — `TemplateTextarea` component with inline dropdown for `{{...}}` placeholders. Suggestions sourced from task inputs, file_inputs/outputs indices, and upstream task outputs.
- [x] ~~Drag-to-reorder in queue_binding entries~~ — ▲/▼ buttons on each entry in `QueueBindingEditor`
- [x] ~~Status LEDs in editor header~~ — `StatusLeds` component + `useStatusWebSocket` hook in `App.tsx` header. Shows Connected/Disconnected, Queries in flight, Workflow running, Python Offline, succeeded/failed counters — matching dashboard.

### Validation Expansion (all implemented in `workflowValidator.cpp`)
- [x] ~~Task-type required fields (shell `params.command`, python `module`/`function`, internal `action`, ai_call `prob_files`)~~
- [x] ~~Shell command `scripts/` prefix policy enforcement (validator + `ShellTaskExecutor::ValidateScriptPath`)~~
- [x] ~~Path traversal rejection (`..` in command)~~
- [x] ~~Dataflow validation (`from_task`/`to_task` existence, slot matching, type compatibility, duplicate binding)~~
- [x] ~~Trigger validation (cron expression, file_watch path)~~
- [x] ~~Runtime feasibility preflight (script existence check)~~
- [x] ~~Validation API returns severity/tier/code/path/taskId per issue~~

### Error branching / controlflow (runtime + editor)
- [x] ~~Branch node type + error-signal edges in JCWF schema~~ — `control_nodes` array with `"type": "branch"`, `controlflow` edges with `"kind": "normal"/"error_signal"/"on_error"`
- [x] ~~`expose_error_signal` per-task field~~ — parsed, validated, surfaced in editor inspector
- [x] ~~Runtime branch firing + Rule A (handled failures)~~ — `FireBranchIfReady`, re-activation of previously-skipped tasks, handled failures don't fail the run
- [x] ~~`exampleMakefile5` error recovery workflow~~ — verified end-to-end: shell fails → AI fix → retry → run hello

### WebSocket Push (replace polling)
- [x] ~~Replace the editor's 500ms `workflow-runs-request` polling with true server-push broadcasts.~~
      Backend already called `BroadcastWorkflowRunsSnapshot()` on every state change from
      `JarvisAgent::OnUpdate()`. Fix: removed the 500ms `setInterval` client poll, added
      `capturedStdout`/`capturedStderr` to the server-pushed snapshot, kept a 10s REST
      safety-net fallback for `pendingRunId`. Verified real-time badge updates on both
      simple (cyber2) and large (jarvisCppDocu) workflows.

### Broken JCWF visibility
- [ ] Show broken JCWFs (parse failures) in the workflow editor with an error badge and the parse error message, so the user can fix them visually instead of digging through `log/log.txt`

### Future (n8n integration)
- [x] ~~Seamless n8n integration~~ — `POST /api/webhook/<workflowId>` with HMAC-SHA256, completion callback POST to `callbackUrl`, n8n custom node v2 (webhook/legacy toggle + HMAC signing). Legacy `POST /api/integrations/n8n/start` retained for backward compat.
- [ ] Sub-workflows / workflow-call node (Remaining TODO #3)
- [ ] Persist editor layout (node positions) in JCWF — do after sub-workflows since sub-workflow nodes need layout too
- [x] ~~Security log viewer in dashboard~~ — `GET /api/log/security` endpoint (both editions, admin-auth). Dashboard LogViewerPanel has Application/Security tab toggle with 3s delta polling for security log. Security log shows auth events, rate limits, lockouts, webhook decisions, run control actions.
- [x] ~~JCWF assistant provider override~~ — "JCWF AI Interface" dropdown in Settings modal selects a non-default AI interface for the Generate / Explain / Fix Script pipeline. Stored as `jcwf_ai_interface` in `config.json`. Backend resolves selected interface and writes `PROV_provider.json` sidecar files. E2E verified.
- [x] ~~Python task stdout/stderr capture~~ — inline `_JarvisTee` in `PythonEngine` tees output to real-time terminal + `StringIO` buffer. `PythonTaskExecutor` writes `stdout.txt`/`stderr.txt` + stores in `TaskInstanceState`. Tooltip shows captured output.
- [x] ~~Workflow reload auto-trigger fix~~ — navigating to the Workflows page no longer re-fires all auto-trigger workflows. `AddAutoTrigger` accepts `fireImmediately` param; reload path passes `false`.
- [x] ~~Workflow versioning~~ — auto-backup on save to `.history/<workflowId>/<timestamp>.jcwf`. REST API: `GET /api/workflows/<id>/versions` (list), `GET /api/workflows/<id>/versions/<ts>` (get), `POST /api/workflows/<id>/versions/<ts>/restore` (restore with pre-restore backup). Frontend: "History" button in toolbar opens `VersionHistoryModal` with version list, sizes, and restore buttons.

---

## Gap Analysis: Example Workflows vs. Editor

| Feature | Status | Notes |
|---------|--------|-------|
| queue_binding (stng/task/cntx/prob) | ✅ Done | `QueueBindingEditor.tsx` |
| file_inputs / file_outputs | ✅ Done | Inline list editors |
| dataflow wiring | ✅ Done | Dashed green edges |
| Workflow-level defaults | ✅ Done | Sidebar fields |
| Workflow-level label, doc | ✅ Done | Sidebar fields |
| base_directory | ✅ Done | Preserved on save |
| outputs/inputs slot editor | ✅ Done | Name, type, required |
| Per-task timeout_ms | ✅ Done | Number input |
| Runtime state colors | ✅ Done | Yellow/red/green/grey/orange |

---

## Validation Expansion Plan

### Severity Model
- **Tier A — Schema/semantics:** Error (invalid JCWF)
- **Tier B — Runtime policy:** Error (e.g. `scripts/` prefix)
- **Tier C — Feasibility preflight:** Warning (machine-state dependent)
- **Tier D — Informational:** Info (missing optional fields)

### Checklist
- **A) Task-type required fields** — `params.command` for shell, prompt for ai_call
- **B) Shell command policy** — `scripts/` prefix, no `..` traversal
- **C) Path rules** — well-formed `working_directory`, `file_inputs`, `file_outputs`
- **D) Dataflow validation** — `from_task`/`to_task` exist, slot type compatibility
- **E) Trigger validation** — cron expression, file_watch path, event types
- **F) Runtime feasibility** — script exists + executable, file_inputs exist

---

## Recent Features (Feb 2026)

### Shell Task stdout/stderr Capture
- Backend: separate pipes for stdout/stderr, written to `stdout.txt`/`stderr.txt`
- First 1024 chars stored in `TaskInstanceState`, exposed via REST + WebSocket
- Frontend: hover tooltip (stderr in red), side panel display

### Script Path Validation
- `GET /api/scripts/check?path=<scriptPath>` — exists + executable check
- Lexical path normalization rejects `..` traversal
- Frontend caches results, shows inline warnings on shell task nodes

### Dashboard Log Viewer
- `GET /api/log?tail=N` — last N lines + byte offset
- `GET /api/log?offset=N` — delta mode (lines since offset)
- `GET /api/log/analyze-last-run?index=N` — structured run analysis with multi-run cycling
- Virtual scroll (100K lines), delta polling (500ms), search (`/` or `Ctrl+F`)
- Analyze panel: run metadata, issues with severity, ▲/▼ navigation, ◀/▶ run cycling

---

## Architecture

```
+-----------------------------+
| React + React Flow (UI)     |
|  - Workflow Editor          |
|  - Run Monitor              |
|  - Template Browser         |
+-------------+---------------+
              |
      REST + WebSocket
              |
+-------------v---------------+
| JarvisAgent Web Server      |
| (Crow)                      |
|  - Workflow CRUD API        |
|  - Validation API           |
|  - Run Control API          |
|  - WebSocket Broadcasts     |
+-------------+---------------+
              |
+-------------v---------------+
| JarvisAgent Runtime         |
|  - WorkflowRegistry         |
|  - WorkflowRuntimeManager   |
|  - Trigger Engine           |
+-----------------------------+
```

### Design Principles
- **JCWF is the single source of truth.** The editor never invents runtime behavior.
- **Round-trip fidelity.** Load → Save preserves every field, even those the editor does not yet understand.
- **Progressive disclosure.** Show fields relevant to the selected task type.
- **Backend-authoritative validation.** C++ validator is definitive; UI shows results.

---

## Manual Test Plan

### Prerequisites
- JarvisAgent running (`bin/Release/jarvisAgent` or `bin/Debug/jarvisAgent`)
- Editor at `http://localhost:8080/editor`
- At least one AI provider key configured
- `workflows/port62pos.csv` exists

### Test Workflows

**Test 1: exampleMakefile** — ai_call generates hello.c + Makefile → shell runs make
**Test 2: stockAnalyzerTop6** — CSV filter (rows 5–10) → per_item ai_call (6 stocks) → summary ai_call
**Test 3: techTermGlossary** — 3-task ai_call chain: generate terms → expand → format glossary

### Log Viewer Test Scenarios
1. Initial load — Log tab shows lines with line numbers
2. Live tail — new lines appear during workflow execution
3. Scroll pause/resume — auto-scroll pauses on scroll up
4. Search — `/` to search, `Enter`/`Shift+Enter` to navigate matches
5. Analyze — press `1`, verify run info and issues, cycle with ◀/▶
6. Large log — smooth virtual scrolling

### stdout/stderr Test Scenarios
1. Hover tooltip on completed task (stdout) and failed task (stderr in red)
2. Side panel output display
3. Truncation at >1024 chars

### Script Validation Scenarios
1. Valid script — no warning
2. Missing script — warning shown
3. Path traversal (`scripts/../etc/passwd`) — rejected
