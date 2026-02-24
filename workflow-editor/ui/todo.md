# Workflow Editor TODO

Last reviewed: Feb 2026

---

## ~~Bug: Workflow-level data loss on save~~ ✓

Verified already fixed. `exportJcwfObject` merges `{ ...loadedJcwfRef.current, ...result.jcwf }`.
`graphToJcwf` only returns `version`, `id`, `tasks`, `filters` — no shadowing of
`defaults`, `label`, `doc`, `base_directory`, or `dataflow`.

---

## ~~Runtime visualization improvements~~ ✓

All runtime state colors implemented: running=yellow with pulse animation, fresh=green
dimmed, failed=bold red (2px border + glow), queued=grey, cancelled=orange.

Stop button implemented (uses existing cancel endpoint). Pause/Resume buttons added
as disabled placeholders — backend endpoints documented in `doc/api-endpoints.md` as
*(planned)* but not yet implemented.

---

## ~~queue_binding editor~~ ✓

Implemented in `src/editor/QueueBindingEditor.tsx`. Four collapsible sections
(STNG, TASK, CNTX, PROB), each entry with path input, inline content textarea,
toggle between inline/reference mode, add/remove. Types: `JcwfQueueFileEntry`,
`JcwfQueueBinding` in `src/jcwf/types.ts`.

---

## ~~Workflow-level defaults editor~~ ✓

Implemented in sidebar Workflow card: `timeout_ms`, `ai.provider`, `ai.model`
number/text inputs. State initialized on load, merged into JCWF on export.

---

## ~~Dataflow editor~~ ✓

Implemented: `JcwfDataflowEntry` type, dashed green edges with `df:` prefix,
output/input port handles on TaskNode, `jcwfToGraph`/`graphToJcwf` serialization,
`onConnect` handler distinguishes dependency vs dataflow connections.

---

## ~~file_inputs / file_outputs editor~~ ✓

Inline list editors in inspector for shell/python tasks. Add/remove string entries.
Cleans up empty arrays on removal.

---

## ~~Workflow-level label and doc~~ ✓

Added `label` (text input) and `doc` (textarea) to sidebar Workflow card.
Multi-line doc serializes as `string[]` per JCWF convention.

---

## Additional items implemented

- **Per-task timeout_ms** — number input in inspector for all task types
- **inputs/outputs slot editor** — name, type dropdown, required checkbox

---

## Remaining backend work

- Implement `POST /api/workflow-runs/<runId>/pause` in `workflowRuntimeManager.cpp`
- Implement `POST /api/workflow-runs/<runId>/resume`
- Implement `POST /api/workflow-runs/<runId>/stop`
- Then remove `disabled` from Pause/Resume buttons in editor
- Template variable autocomplete for `{{binding.field}}` in queue_binding prob_files
- Drag-to-reorder in queue_binding entries (stretch goal)
