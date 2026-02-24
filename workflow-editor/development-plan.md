# Workflow Editor — Development Plan

Last updated: Feb 2026

---

## Current State

The editor is a React + TypeScript + Vite + React Flow application served by
JarvisAgent at `localhost:8080/editor`. It supports:

- Importing and rendering existing `.jcwf` workflows as DAG graphs
- Adding / editing / deleting task nodes (ai_call, shell, python, internal)
- Adding / editing filter nodes (csv, text_lines, query, polarion_query)
- Dependency edges (drag-connect) with cycle detection
- Task inspector: label, type, mode, filter ID, working_directory, doc, params (raw JSON)
- Trigger editing (auto, manual, cron, file_watch)
- Undo / redo (Ctrl+Z / Ctrl+Shift+Z)
- Auto-layout (topological DAG)
- Save / Save As / Validate / Run / Clean / Cancel Run
- Live run monitoring via WebSocket (task state badges on nodes)
- Backend validation with severity tiers (A–D), displayed in sidebar + node badges
- Filter builder dialog (source kind, path, binding, max_items)
- Template browser with starter workflows
- MiniMap, zoom controls, debug panel

---

## Gap Analysis: Example Workflows vs. Editor

The table below lists every JCWF feature actively used by the example workflows
in `example/workflows/` and whether the editor can create or edit it.

| Feature | Example(s) | Editor | Gap |
|---------|-----------|--------|-----|
| queue_binding (stng/task/cntx/prob with inline content) | portfolioDividendAnalysis, bookSummaryPipeline, goKartComplianceCheck, aiZipDemo, vehicleTroubleshootingGuide, aiCarMaintenancePipeline | Round-trips via `[key: string]` — no dedicated UI | **P1** |
| file_inputs / file_outputs | make-example, bookSummaryPipeline, vehicleTroubleshootingGuide | Round-trips — no UI | **P2** |
| dataflow wiring (from_task → to_task) | make-example, bookSummaryPipeline | Round-trips — no UI, no visual edges | **P3** |
| Workflow-level defaults (timeout_ms, ai.provider, ai.model) | portfolioDividendAnalysis, bookSummaryPipeline, make-example | Dropped on save (bug in graphToJcwf) | **P0 bug** |
| Workflow-level label, doc | All | Dropped on save | **P0 bug** |
| base_directory | portfolioDividendAnalysis, bookSummaryPipeline | Dropped on save | **P0 bug** |
| outputs (typed output slots per task) | make-example, portfolioDividendAnalysis | Round-trips — no UI | P4 |
| inputs (typed input slots per task) | make-example, bookSummaryPipeline | Round-trips — no UI | P4 |
| timeout_ms (per-task) | bookSummaryPipeline, vehicleTroubleshootingGuide | Preserved via `[key: string]` — no UI | P5 |
| Runtime state colors | — | Partial (running=blue, should be yellow; no fresh/skipped) | P6 |
| cntx_files with glob patterns | portfolioDividendAnalysis (02_portfolioSummary) | — | Covered by queue_binding editor |
| Template variable autocomplete ({{binding.field}}) | portfolioDividendAnalysis, bookSummaryPipeline | — | Covered by queue_binding editor |

---

## Phased Plan

### Phase 0 — Fix workflow-level round-trip data loss (bug)

**Problem:** `graphToJcwf` builds a fresh `JcwfFile` with only `version`, `id`,
`tasks`, and `filters`. `exportJcwfObject` merges back `triggers` and
`manual_start`, but `defaults`, `base_directory`, `label`, `doc`, and `dataflow`
are silently dropped on every save.

**Fix:** `exportJcwfObject` already starts from `loadedJcwfRef.current` (the
original parsed JCWF). The merge `{ ...loadedJcwfRef.current, ...result.jcwf }`
overwrites all keys from `graphToJcwf` output. Since `graphToJcwf` does not
include `defaults`, `label`, `doc`, `base_directory`, or `dataflow`, the spread
preserves them correctly — **unless** `graphToJcwf` returns keys that shadow
them.

Verify that `graphToJcwf` only returns `version`, `id`, `tasks`, `filters` and
does not introduce extra keys that would shadow the originals.

Add explicit preservation in `graphToJcwf` for safety: copy `defaults`, `label`,
`doc`, `base_directory`, `dataflow` from a passed-in original JCWF object.

**Files:**
- `src/editor/graphToJcwf.ts`
- `src/editor/WorkflowEditorView.tsx` (pass original JCWF to graphToJcwf)

**Acceptance:** Open portfolioDividendAnalysis → Save → re-open. Verify
`defaults`, `label`, `doc`, `base_directory`, `dataflow` are intact.

---

### Phase 1 — queue_binding editor

The core of every `ai_call` task. Currently no UI; users must hand-edit JSON in
the params textarea.

**Design:** A structured panel shown in the inspector when task type = `ai_call`.

Sections:
- **stng_files** — list of entries, each with path (text) + optional inline content (textarea)
- **task_files** — same
- **cntx_files** — same, plus glob support (e.g. `../01_lookup/PROB_*.output.txt`)
- **prob_files** — same, with `{{binding.field}}` template highlighting

Each entry:
- Text input for `path`
- Expandable textarea for `content` (collapsed if empty / file-reference-only)
- Add / remove buttons
- Drag-to-reorder (optional, stretch)

Template variable assist: when the task has `mode: per_item` and a `filter` ID,
inspect the filter's binding name to suggest `{{binding.field}}` placeholders.
For CSV filters, parse the header row to list available column names.

**Files:**
- `src/editor/QueueBindingEditor.tsx` — new component
- `src/editor/WorkflowEditorView.tsx` — render QueueBindingEditor in inspector when type=ai_call
- `src/jcwf/types.ts` — add `JcwfQueueBinding`, `JcwfQueueFileRef` types

**Acceptance:** Open portfolioDividendAnalysis in editor. The queue_binding for
`lookupDividend` should display stng/task/cntx/prob entries with inline content
visible. Edit a prob_files content → Save → verify round-trip.

---

### Phase 2 — file_inputs / file_outputs editor

For `shell` and `python` tasks. Simple list editors shown in the inspector.

**Design:**
- **file_inputs** — ordered list of file path strings with add/remove
- **file_outputs** — same
- Show below working_directory field in inspector
- Only visible when task type is `shell` or `python`

**Files:**
- `src/editor/WorkflowEditorView.tsx` — add list editors to inspector panel
- `src/jcwf/types.ts` — formalize `file_inputs`, `file_outputs` on JcwfTask

**Acceptance:** Open make-example. compile_lib1 shows file_inputs=["lib1.cpp"],
file_outputs=["lib1.o"]. Add a new file_input → Save → verify round-trip.

---

### Phase 3 — dataflow visual wiring

Explicit output→input wiring between tasks. Currently serialized to the
`dataflow` array in JCWF but not visualized or editable in the editor.

**Design:**
- Render small **output ports** (right side, below existing handle) and **input ports** (left side)
  based on declared `outputs` and `inputs` on each task
- Drag from output port → input port creates a dataflow edge
- Dataflow edges rendered with a **dashed line** and different color vs. depends_on edges
- Serialize to/from `dataflow` array
- Distinct from dependency edges in the graph model

**Implementation notes:**
- React Flow supports multiple handles per node via handle IDs
- Add `handleId` to distinguish dependency handles from dataflow handles
- `graphToJcwf` must serialize both edge types: dependency edges → `depends_on`, dataflow edges → `dataflow`
- `jcwfToGraph` must deserialize both

**Files:**
- `src/editor/TaskNode.tsx` — render output/input port handles
- `src/editor/WorkflowEditorView.tsx` — handle dataflow edge creation/deletion
- `src/editor/graphToJcwf.ts` — serialize dataflow edges to `dataflow` array
- `src/editor/jcwfToGraph.ts` — deserialize `dataflow` array into dataflow edges
- `src/editor/types.ts` — add `JcwfDataflow` type, distinguish edge kinds
- `src/styles.css` — dashed edge style for dataflow

**Acceptance:** Open make-example. Dataflow edges visible as dashed lines between
output/input ports. Delete one → Save → verify dataflow array is updated.

---

### Phase 4 — Workflow-level metadata editor

Simple form fields in the sidebar "Workflow" card.

**Fields:**
- `label` — text input
- `doc` — textarea
- `base_directory` — text input
- `defaults.timeout_ms` — number input
- `defaults.ai.provider` — text input (or dropdown from loaded AI interfaces)
- `defaults.ai.model` — text input

**Files:**
- `src/editor/WorkflowEditorView.tsx` — add fields to Workflow sidebar card
- Store values in component state, merge into JCWF on export

**Acceptance:** Open portfolioDividendAnalysis. Label, doc, defaults visible.
Edit defaults.timeout_ms → Save → verify round-trip.

---

### Phase 5 — inputs/outputs slot editor

For all task types. Declare named typed slots.

**Design:**
- **inputs** — key-value list: name (text), type (dropdown: string/object/number), required (checkbox)
- **outputs** — key-value list: name (text), type (dropdown)
- Shown in inspector for all task types
- Feeds into Phase 3 (dataflow ports)

**Files:**
- `src/editor/WorkflowEditorView.tsx` — add slot editors to inspector
- `src/jcwf/types.ts` — formalize `JcwfSlot` type

---

### Phase 6 — Runtime visualization polish

**Changes:**
- Running state: blue → **yellow** border + subtle pulse animation
- Fresh/skipped: new **green dimmed** border (lighter than success)
- Failed: bolder **red** border
- Queued: **grey** border
- Add **Stop** button (graceful: finish in-flight, no new dispatch)
- Add **Pause / Resume** buttons (suspend new dispatch)

**Backend prerequisites:**
- `POST /api/workflows/{id}/runs/{runId}/pause`
- `POST /api/workflows/{id}/runs/{runId}/resume`
- `POST /api/workflows/{id}/runs/{runId}/stop`

**Files:**
- `src/styles.css` — update runtime state classes
- `src/editor/TaskNode.tsx` — verify CSS class mapping
- `src/editor/WorkflowEditorView.tsx` — add Pause/Resume/Stop buttons
- `src/api/workflows.ts` — add API calls
- Backend: `workflowRuntimeManager.cpp` — implement pause/stop states

---

### Phase 7 — Per-task timeout editor

Add a `timeout_ms` number input in the inspector for all task types. Currently
preserved via `[key: string]` but not visible or editable.

**Files:**
- `src/editor/WorkflowEditorView.tsx` — add timeout_ms field to inspector

---

## Priority Order

1. **Phase 0** — Fix round-trip data loss (bug, blocks everything)
2. **Phase 1** — queue_binding editor (core ai_call editing capability)
3. **Phase 2** — file_inputs / file_outputs editor (core shell/python editing)
4. **Phase 4** — Workflow-level metadata editor (label, doc, defaults)
5. **Phase 3** — Dataflow visual wiring (make-example, bookSummaryPipeline)
6. **Phase 5** — inputs/outputs slot editor
7. **Phase 7** — Per-task timeout editor
8. **Phase 6** — Runtime visualization polish (backend work required)

---

## Design Principles

- **JCWF is the single source of truth.** The editor never invents runtime behavior.
- **Round-trip fidelity.** Load → Save must preserve every field, even those the editor does not yet understand.
- **Progressive disclosure.** Show fields relevant to the selected task type. Do not overwhelm with all possible fields at once.
- **Backend-authoritative validation.** The editor runs lightweight client checks for instant feedback but defers to the C++ validator for definitive results.
- **No emojis in generated JCWF.** Keep output clean and machine-readable.
