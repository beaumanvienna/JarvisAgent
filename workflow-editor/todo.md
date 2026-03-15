# Workflow Editor — TODO & Reference

Last reviewed: 2026-03-12

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

---

## Remaining Work

### Editor UI
- [ ] Box select: left-click drag on empty canvas area opens a selection rectangle to select
      multiple nodes at once (for group move/delete). Note: Ctrl+click already works for
      multi-select; this adds the more intuitive drag-box alternative.
- [ ] Template variable autocomplete for `{{binding.field}}` in queue_binding prob_files
- [ ] Drag-to-reorder in queue_binding entries (stretch goal)

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
- [ ] Replace the editor's 500ms `workflow-runs-request` polling with true server-push broadcasts.
      Currently the Crow backend only drains pending broadcasts inside `onmessage` (i.e. when
      the client sends a message).  A Crow IO-thread timer or an `io_service::post()` wake-up
      would let the server push updates immediately without waiting for the next client message.
      This would reduce latency for AI progress, run-state changes, and log streaming.

### Future (n8n integration)
- [ ] `POST /api/integrations/n8n/start` endpoint with callback URL
- [ ] n8n → JCWF converter
- [ ] Sub-workflows / workflow-call node
- [ ] Workflow versioning

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
