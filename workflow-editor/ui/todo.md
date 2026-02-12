# Workflow Editor TODO

Last reviewed: Feb 2026

---

## Bug: Workflow-level data loss on save

`graphToJcwf` builds a fresh `JcwfFile` with only `{version, id, tasks, filters}`.
`exportJcwfObject` merges back `triggers` and `manual_start`, but the following
workflow-level fields are **silently dropped** on every save:

- `defaults` (timeout_ms, ai.provider, ai.model)
- `base_directory`
- `label` (workflow-level)
- `doc` (workflow-level)
- `dataflow` (output→input wiring between tasks)

Task-level extra fields (queue_binding, file_inputs, file_outputs, outputs) survive
because `JcwfTask` has `[key: string]: unknown` and `graphToJcwf` copies the full
task object.

**Fix:** Store the original `JcwfFile` on load and merge graph changes back into it
instead of constructing a new object.  Only overwrite `tasks`, `filters`, `triggers`,
`manual_start`, `version`, and `id`.

Files:
- `src/editor/graphToJcwf.ts`
- `src/editor/WorkflowEditorView.tsx` (`exportJcwfObject`, `performCreateWorkflow`)

---

## Runtime visualization improvements

### Better runtime state colors

Current colors don't match intuitive traffic-light semantics:

| State | Current | Desired |
|-------|---------|---------|
| queued | (none) | grey border |
| running | blue | **yellow** border + subtle glow |
| success | green | **green** border (keep) |
| fresh (skipped) | (none) | **green** border, dimmer than success |
| failed | red | **red** border (keep, make bolder) |
| cancelled | orange | orange (keep) |

"Fresh" / "skipped" is a new state: the task was already up-to-date based on manifest
freshness.  Backend needs to report this as a distinct state (e.g. `"skipped"` or
`"fresh"`) in the WebSocket snapshot.

Files:
- `src/editor/TaskNode.tsx` — add `taskNodeQueued`, `taskNodeFresh` CSS classes
- `src/editor/types.ts` — add `"fresh"` to `RuntimeTaskState`
- `src/editor/WorkflowEditorView.tsx` — map backend state to `"fresh"`
- `src/styles.css` — update `.taskNodeRunning` from blue to yellow, add new classes

### Stop and Pause buttons

Currently only "Cancel Run" exists (kills the entire run).  Add:

- **Stop** — gracefully stop after current in-flight tasks complete (no new tasks dispatched)
- **Pause** — suspend dispatching new tasks; resume later

This requires backend support:
- `POST /api/workflows/{id}/runs/{runId}/pause`
- `POST /api/workflows/{id}/runs/{runId}/resume`
- `POST /api/workflows/{id}/runs/{runId}/stop` (vs current cancel which is immediate)

Files:
- `src/editor/WorkflowEditorView.tsx` — add Pause/Resume/Stop buttons next to Cancel
- `src/api/workflows.ts` — add `pauseRun`, `resumeRun`, `stopRun` API calls
- Backend: `application/workflow/workflowRuntimeManager.cpp` — implement pause/stop states

---

## queue_binding editor

The core of every `ai_call` task.  Currently no UI — must hand-edit JSON.

Build a structured editor panel (shown when task type = `ai_call`) with:

- **stng_files** — list of file refs (path) or inline content (path + content)
- **cntx_files** — same, with glob pattern support (e.g. `*.md`)
- **task_files** — same
- **prob_files** — same, with `{{binding.field}}` template variable autocomplete

Each file entry needs:
- path input (text)
- optional inline content textarea
- add/remove buttons
- drag-to-reorder

Template variable autocomplete should inspect the task's filter binding to suggest
available `{{binding.field}}` placeholders.

Files:
- `src/editor/WorkflowEditorView.tsx` — render QueueBindingEditor in inspector
- `src/editor/QueueBindingEditor.tsx` — new component
- `src/jcwf/types.ts` — add `JcwfQueueBinding`, `JcwfQueueFileRef` types

---

## Workflow-level defaults editor

Simple form in the sidebar or a modal:

- `timeout_ms` (number input)
- `ai.provider` (text or dropdown)
- `ai.model` (text or dropdown)
- `retries` (number input)

Files:
- `src/editor/WorkflowEditorView.tsx` — add Defaults section to sidebar
- `src/jcwf/types.ts` — add `JcwfDefaults` type

---

## Dataflow editor

Visual wiring of task output slots → input slots of downstream tasks.

- Render output/input ports on task nodes
- Allow drag-connecting output port → input port (creates a dataflow edge)
- Serialize to/from `dataflow` array in JCWF
- Distinct edge style (e.g. dashed, different color) vs depends_on edges

Files:
- `src/editor/TaskNode.tsx` — render output/input port handles
- `src/editor/WorkflowEditorView.tsx` — handle dataflow edge creation/deletion
- `src/editor/graphToJcwf.ts` — serialize dataflow edges
- `src/editor/jcwfToGraph.ts` — deserialize dataflow edges
- `src/jcwf/types.ts` — add `JcwfDataflow` type

---

## file_inputs / file_outputs editor

For `shell` and `python` tasks, allow declaring:

- `file_inputs` — list of file paths consumed by the task
- `file_outputs` — list of file paths produced by the task
- `outputs` — named output slots with types

Simple list editor with add/remove, shown when task type is shell or python.

Files:
- `src/editor/WorkflowEditorView.tsx` — add to inspector panel
- `src/jcwf/types.ts` — formalize `JcwfFileIO` types

---

## Workflow-level label and doc

Add two small fields to the sidebar (Workflow card):

- `label` — text input
- `doc` — textarea

Files:
- `src/editor/WorkflowEditorView.tsx` — add fields to Workflow sidebar card
- `src/editor/graphToJcwf.ts` — include in serialized output

---

## Priority order

1. Fix workflow-level round-trip data loss (bug)
2. Runtime colors (yellow/green/red)
3. queue_binding editor
4. Stop / Pause buttons
5. defaults editor
6. Workflow label + doc fields
7. dataflow editor
8. file_inputs / file_outputs editor
