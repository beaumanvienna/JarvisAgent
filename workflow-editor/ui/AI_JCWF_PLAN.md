# AI ↔ JCWF Generation — Development Plan

## Goal

Add a **prompt area** below the workflow editor canvas that supports two operations:

1. **JCWF → Prompt**: Serialize the current canvas to JCWF, send to AI, display a human-readable summary in the prompt area.
2. **Prompt → JCWF**: User writes a natural language description, a multi-stage AI pipeline generates valid JCWF, and the canvas updates.

Both directions integrate with the existing undo/redo stack (Ctrl+Z / Ctrl+Y).

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│  Workflow Editor (React)                                  │
│  ┌──────────┬──────────────────────┬──────────────────┐  │
│  │ Sidebar  │  ReactFlow Canvas    │  Inspector       │  │
│  │          │                      │                  │  │
│  │          │                      │                  │  │
│  │          ├──────────────────────┤                  │  │
│  │          │  Prompt Area         │                  │  │
│  │          │  [textarea] [▲ ▼]    │                  │  │
│  │          │  [Explain] [Generate]│                  │  │
│  │          └──────────────────────┘                  │  │
│  └──────────┴──────────────────────┴──────────────────┘  │
└──────────────────────────────────────────────────────────┘
         │                                    │
         │  POST /api/ai/explain-jcwf         │
         │  POST /api/ai/generate-jcwf        │
         ▼                                    ▼
┌──────────────────────────────────────────────────────────┐
│  JarvisAgent Backend (C++ / Crow)                        │
│                                                          │
│  explain-jcwf:  single ai_call (JCWF + spec → summary)  │
│  generate-jcwf: runs built-in __jcwf_generator pipeline  │
│                 (decompose → generate → validate → fix)  │
└──────────────────────────────────────────────────────────┘
```

---

## Phase 1: UI — Prompt Area Component

### Layout Change

Current `.editorShell` grid:
```css
grid-template-columns: 280px 1fr 320px;
height: calc(100vh - 56px);
```

The middle column currently contains only `<ReactFlow>`. Change it to a flex column:
```
<div style={{ display: "flex", flexDirection: "column", height: "100%" }}>
  <div style={{ flex: 1, position: "relative" }}>
    <ReactFlow ... />
  </div>
  <AiPromptArea ... />   /* collapsible, ~160px when open */
</div>
```

### `AiPromptArea` Component

New file: `workflow-editor/ui/src/editor/AiPromptArea.tsx`

**UI elements:**
- Resizable/collapsible panel with drag handle (or toggle button)
- `<textarea>` for prompt input/output (auto-grows, max ~8 lines)
- **"Explain"** button — serializes current canvas JCWF → calls backend → populates textarea with summary
- **"Generate"** button — sends textarea content → calls backend → loads result into canvas
- Status indicator (idle / generating… / error)
- Spinner/progress during generation

**Props interface:**
```typescript
type AiPromptAreaProps = {
  getCurrentJcwf: () => JcwfFile | null;   // serialize current canvas
  onJcwfGenerated: (jcwf: JcwfFile) => void; // load generated JCWF into canvas
  disabled?: boolean;
};
```

**Undo/redo integration:**
- `onJcwfGenerated` calls `loadFromJcwf(null, jcwf)` which updates nodes/edges, and the existing debounced history effect will automatically push the pre-change state to the undo stack.
- No special undo logic needed — it piggybacks on the existing mechanism.

---

## Phase 2: Backend — Two New REST Endpoints

### WebSocket Messages (reuse existing `/ws` connection)

Both operations use the **existing WebSocket** connection to stream progress and deliver results. No new REST endpoints needed.

#### Explain (JCWF → Prompt) — 2-Stage Pipeline

**Client sends:**
```json
{ "type": "ai-explain-jcwf", "jcwf": { ... } }
```

**Server streams:**
```json
{ "type": "ai-explain-progress", "message": "Generating explanation..." }
{ "type": "ai-explain-progress", "message": "Reviewing and enriching explanation..." }
{ "type": "ai-explain-result", "ok": true, "summary": "Human-readable explanation..." }
```

##### Stage 1: Initial Explanation (current implementation)

Build a single AI request:
- **STNG**: "You are a workflow documentation expert. Explain workflows clearly and
  concisely in structured English. Focus on what the workflow does, the task pipeline,
  dependencies, data flow, and any error handling."
- **TASK**: "Describe what this JCWF workflow does in plain English. Structure your
  explanation with: 1) Overview, 2) Tasks and their roles, 3) Dependencies and data
  flow, 4) Error handling (if any). Keep it concise but complete."
- **CNTX**: The JCWF JSON (pretty-printed).
- **PROB**: "Explain this JCWF workflow."

This produces a good high-level summary but may miss JCWF-specific implementation
details (materialize, expose_error_signal, controlflow edge kinds, shared working
directories, queue_binding structure, file_inputs/file_outputs data flow, etc.).

##### Stage 2: Review / Refine / Enrich

A second AI call receives the Stage 1 explanation *plus* the condensed JCWF spec
(`doc/jcwf_generation_guide.md`) so the AI knows what fields and concepts matter:

- **STNG**: "You are a JCWF workflow specification expert. You know every field in the
  JCWF format: materialize, expose_error_signal, controlflow edges (normal, error_signal,
  on_error), queue_binding (STNG/TASK/CNTX/PROB files), working_directory conventions,
  file_inputs/file_outputs, dataflow, control_nodes, and branching semantics."
- **TASK**: "Review the explanation below against the actual JCWF JSON and the JCWF
  specification. Correct any inaccuracies. Add missing details that are important for
  understanding the workflow's implementation — especially: materialize mappings,
  controlflow edge routing, expose_error_signal usage, shared vs unique working
  directories, queue_binding file content, and dataflow connections. Keep the same
  structure (Overview, Tasks, Dependencies, Error handling) but make it precise enough
  that someone could recreate the JCWF from the explanation alone."
- **CNTX**: The JCWF JSON + the condensed spec + the Stage 1 explanation.
- **PROB**: "Review and enrich this JCWF workflow explanation."

##### Why 2 stages?

- **Round-trip fidelity:** the explanation produced by Explain should be detailed enough
  that feeding it back into Generate recreates a structurally equivalent JCWF. The
  Stage 1 explanation alone is ~80% accurate (tested with exampleMakefile5.jcwf); it
  misses materialize, expose_error_signal, controlflow edges, and shared working
  directories. Stage 2 closes this gap.
- **Separation of concerns:** Stage 1 captures intent (human-readable). Stage 2 adds
  precision (machine-round-trippable). The user sees the enriched final version.
- **Spec awareness:** Stage 1 doesn't receive the spec (keeps the context window small
  and focused). Stage 2 receives the spec so it knows which JCWF details to verify.

##### Prompt Refinement Ideas (for Stage 1)

The current Stage 1 prompt could also be improved independently:
- Add a line to the TASK: "For each task, mention its working_directory, file_inputs,
  file_outputs, and any materialize mappings."
- Add to STNG: "Pay attention to controlflow edges and their kinds (normal,
  error_signal, on_error). Mention expose_error_signal if a task uses it."
- These additions would improve Stage 1 quality but don't eliminate the need for
  Stage 2 (the AI still needs spec context to get the details right).

#### Generate (Prompt → JCWF)

**Client sends:**
```json
{ "type": "ai-generate-jcwf", "prompt": "...", "currentJcwf": { ... } | null }
```

**Server streams:**
```json
{ "type": "ai-generate-progress", "stage": 1, "totalStages": 4, "message": "Decomposing prompt..." }
{ "type": "ai-generate-progress", "stage": 2, "totalStages": 4, "message": "Generating JCWF..." }
{ "type": "ai-generate-progress", "stage": 3, "totalStages": 4, "message": "Validating..." }
{ "type": "ai-generate-progress", "stage": 3, "totalStages": 4, "message": "Fixing errors (retry 1/2)..." }
{ "type": "ai-generate-result", "ok": true, "jcwf": { ... }, "validationResult": { ... }, "retries": 1 }
```

Implementation:
1. Receive the user's natural language prompt + optional current JCWF via WebSocket.
2. Run the multi-stage pipeline (see Phase 3), sending `ai-generate-progress` after each stage.
3. On completion, send the final JCWF JSON + validation results + retry count.

The `currentJcwf` field enables "modify" mode — the user can refine an existing workflow by describing changes.

---

## Phase 3: Built-in Generation Pipeline

### Strategy: Condensed Spec + Multi-Stage Pipeline

The full spec is 2,235 lines — too large for a single context window to be reliable. Strategy:

1. **Pre-extract a condensed generation guide** (~600 lines) covering:
   - Root object fields (§3.1)
   - Task object fields (§3.3) — all four types
   - `queue_binding` for ai_call (§3.3.6.4)
   - Dependencies (§3.4)
   - Dataflow (§3.5)
   - Controlflow/branching (§3.8)
   - JSON Schema excerpt (§9)
   - Two complete annotated examples

   This lives at: `doc/jcwf_generation_guide.md` — a human-curated subset.

2. **Multi-stage pipeline** (implemented as C++ code, not as a JCWF workflow itself):

```
Stage 1: DECOMPOSE (ai_call)
  Input:  user prompt + condensed spec (task types section only, ~200 lines)
  Output: structured plan (task list, types, dependencies, data flow sketch)

Stage 2: GENERATE (ai_call)
  Input:  structured plan + condensed spec (full generation guide)
  Output: JCWF JSON

Stage 3: VALIDATE (local, deterministic)
  Input:  JCWF JSON
  Action: call existing WorkflowValidator::Validate()
  Output: errors[] + warnings[]

Stage 4: FIX (ai_call, conditional — up to 2 retries)
  Input:  JCWF JSON + validation errors + condensed spec
  Output: corrected JCWF JSON
  → goto Stage 3
```

### Why NOT a JCWF workflow?

Using a JCWF workflow to drive generation is elegant in theory but introduces circular complexity:
- The generator pipeline needs to be registered, triggered, cleaned, and monitored.
- The editor would need to watch a separate run and extract the output.
- Error recovery within the pipeline (retry loops) would require dynamic workflow modification.

Instead: **implement the pipeline as a C++ function** (`AiJcwfGenerator::Generate()`) that makes sequential AI calls via the existing `AiRequestPool`. This keeps it self-contained, synchronous, and testable.

### Retry Logic (Branch on Error)

```
generate → validate → if errors → fix_1 → validate → if errors → fix_2 → validate → return
                    → if ok → return
```

Max 2 fix retries. After that, return whatever we have + the validation results. The editor displays the warnings/errors and the user can manually fix them.

---

## Phase 4: Condensed JCWF Generation Guide

New file: `doc/jcwf_generation_guide.md`

Curated from the spec, covering exactly what an AI needs to generate valid JCWF:

1. **Root object** — `version`, `id`, `manual_start`, `triggers`, `tasks`, `defaults`, `filters`, `dataflow`, `control_nodes`, `controlflow`
2. **Task fields** — `id`, `type`, `label`, `depends_on`, `params`, `working_directory`, `file_inputs`, `file_outputs`, `materialize`, `queue_binding`, `expose_error_signal`, `timeout_ms`, `mode`, `filter`, `inputs`, `outputs`
3. **Task types** — shell (command + args), ai_call (queue_binding with STNG/TASK/CNTX/PROB), python (module + function), internal (action)
4. **queue_binding** — stng_files, task_files, cntx_files, prob_files (inline content or path reference)
5. **Dependencies** — `depends_on` array, DAG constraint
6. **Dataflow** — `from_task`, `from_output`, `to_task`, `to_input`
7. **Controlflow** — `control_nodes` (branch), `controlflow` edges (normal, error_signal, on_error), `expose_error_signal`
8. **Filters** — csv, text_lines, query (per_item expansion)
9. **Two annotated examples** — one simple (exampleMakefile4: AI → compile → run), one with branching (exampleMakefile5: error recovery)
10. **Common pitfalls** — missing working_directory for ai_call, missing STNG/TASK/CNTX in queue_binding, cycles in depends_on

Target: ~500–700 lines. This is small enough for a single AI context window while covering all generation needs.

---

## Implementation Order

| Step | Component | Description |
|------|-----------|-------------|
| 1 | `doc/jcwf_generation_guide.md` | Curate condensed spec for AI context |
| 2 | `AiPromptArea.tsx` + CSS | UI component with textarea + buttons |
| 3 | Layout integration | Embed in middle column of `WorkflowEditorView` |
| 4 | `api/aiGenerate.ts` | Frontend API client for the two endpoints |
| 5 | Backend: `POST /api/ai/explain-jcwf` | Single ai_call endpoint |
| 6 | Backend: `POST /api/ai/generate-jcwf` | Multi-stage pipeline endpoint |
| 7 | `AiJcwfGenerator` class | C++ pipeline: decompose → generate → validate → fix |
| 8 | Wire up frontend ↔ backend | Connect buttons to API, load result into canvas |
| 9 | Testing | Manual testing with various prompts |

---

## Open Questions

1. **Streaming vs synchronous**: For MVP, synchronous with timeout. Should we add WebSocket progress updates for the generation pipeline (e.g., "Stage 2/4: Generating JCWF...")?

2. **"Modify" mode**: When the user already has a JCWF on canvas and types "add a retry branch to the shell task", should we send the current JCWF as context to the generator? (Recommended: yes, as optional `currentJcwf` field.)

3. **Model selection**: Should the generation pipeline use the user's configured default AI provider, or should it be configurable separately? (Recommended: use default, with an optional override in the prompt area UI.)

4. **Condensed guide maintenance**: The generation guide must stay in sync with the spec. Add a note in the spec pointing to the guide, and a checklist item to update both together.

---

## Files to Create/Modify

### New files
- `doc/jcwf_generation_guide.md` — condensed spec for AI
- `workflow-editor/ui/src/editor/AiPromptArea.tsx` — prompt area component
- `workflow-editor/ui/src/api/aiGenerate.ts` — API client
- `application/ai/aiJcwfGenerator.h` — C++ generator class header
- `application/ai/aiJcwfGenerator.cpp` — C++ generator implementation

### Modified files
- `workflow-editor/ui/src/editor/WorkflowEditorView.tsx` — embed prompt area
- `workflow-editor/ui/src/styles.css` — prompt area styles
- `application/web/webServer.cpp` — register new routes
- `premake5.lua` (or Makefile) — add new source files

---

## Phase 5: Revised Generation Pipeline with Script Registry

### Problem

The original pipeline (Phases 1-4) generates a JCWF but does **not** generate the
companion scripts that shell and python tasks reference. The user would have to write
them by hand, defeating the purpose of AI-driven workflow creation.

Additionally, the AI in Stage 1 (DECOMPOSE) has no knowledge of what scripts already
exist, so it cannot reuse them or avoid name collisions.

### Solution Overview

1. **Script registry** — j9t builds an in-memory inventory of all scripts in `scripts/`
   at startup, kept live via FileWatcher. The registry is serialized as a Markdown table
   for AI context injection.
2. **Revised pipeline** — script names and call signatures are defined in Stage 1
   (DECOMPOSE), so the JCWF (Stage 2) and the scripts (Stage 3) are mutually consistent.
   Validation (Stage 4) can then check both the JCWF and the scripts.

### Revised Pipeline (5 stages)

```
Stage 1: DECOMPOSE (ai_call)
  Input:  user prompt
        + condensed spec (task types section, ~200 lines)
        + script registry table (existing scripts with descriptions + params)
  Output: structured plan:
        - task list with types, labels, dependencies, data flow sketch
        - for each shell/python task: script filename, parameter signature,
          short description of what it does
        - which existing scripts to reuse vs. which new scripts to create

Stage 2: GENERATE JCWF (ai_call)
  Input:  structured plan from Stage 1
        + condensed spec (full generation guide)
  Output: JCWF JSON
        (shell tasks reference scripts by exact name/params from Stage 1;
         python tasks reference modules by exact name/function from Stage 1)

Stage 3: GENERATE SCRIPTS (ai_call, one per NEW script — parallel)
  Input:  per script: the structured plan entry for this script
        + the full JCWF (for workflow context)
        + the task definition that references this script
        + project conventions (bash: set -euo pipefail, @jarvis-script header;
          python: returns dict, @jarvis-script header)
  Output: raw script file content (no markdown fences)
  Note:   j9t's AI pipeline generates these — the scripts themselves do NOT call AI.
          Shell scripts run shell commands; python scripts run python logic.
          The AI is only used here to *author* the script code.

Stage 4: VALIDATE (local, deterministic)
  Input:  JCWF JSON + generated script files
  Action: call existing WorkflowValidator::Validate()
        + verify every referenced script exists (on disk or in generated set)
        + verify parameter usage consistency between JCWF and script headers
  Output: errors[] + warnings[]

Stage 5: FIX (ai_call, conditional — up to 2 retries)
  Input:  JCWF JSON + script files + validation errors + condensed spec
  Output: corrected JCWF JSON (and/or corrected scripts)
  → goto Stage 4
```

### Script Registry — Runtime Component

#### Data Model

```cpp
struct ScriptRegistryEntry
{
    std::string m_FilePath;       // e.g. "scripts/runMake.sh"
    std::string m_Short;          // one-line summary
    std::string m_Description;    // extended description (may be empty)
    std::vector<std::string> m_Params;   // parameter lines
    std::vector<std::string> m_Outputs;  // output descriptions
};
```

See `doc/JC_Workflow_Specification.md` §11 for the `@jarvis-script` metadata format
that scripts use to declare this information.

#### Startup Scan

At j9t startup, scan `scripts/` for `*.sh`, `*.py`, `*.ps1` files. For each file,
read the first 50 lines looking for `# @jarvis-script`. If found, parse the `@short`,
`@params`, `@description`, `@outputs` fields into a `ScriptRegistryEntry`.

Store entries in a `ScriptRegistry` class (thread-safe, mutex-guarded map keyed by
file path).

#### FileWatcher Integration

Add `scripts/` as a second watched directory alongside `queue/`. The existing
`FileWatcher` class already supports watching a single path. Options:

**Option A (simple):** Create a second `FileWatcher` instance for `scripts/`.
In `JarvisAgent::OnEvent()`, handle `FileAddedEvent` / `FileModifiedEvent` /
`FileRemovedEvent` for paths under `scripts/`:
- **Added/Modified:** re-parse the file header, update registry entry
- **Removed:** delete registry entry

**Option B (refactor):** Extend `FileWatcher` to accept multiple watched paths.
This is cleaner but more invasive. Defer to Option B if a third watched path
is ever needed.

Recommended: **Option A** for now.

#### Registry Table Serialization

The registry is serialized to Markdown for AI context injection:

```markdown
## Available Scripts

| Script | Short Description | Parameters |
|--------|-------------------|------------|
| `scripts/runMake.sh` | Wrapper for 'make' command | (none) |
| `scripts/clone_repo.sh` | Clone a git repository | `$1`: repo_url, `$2`: output_dir |
| `scripts/countLines.py` | Count lines of code | `input_dir` (str), `output_file` (str) |
```

This table is passed to the AI in Stage 1 so it can decide which scripts to reuse
and which new scripts need to be created.

#### REST API

Expose the registry for the editor frontend:

```
GET /api/scripts/registry
→ { "scripts": [ { "path": "scripts/runMake.sh", "short": "...", "params": [...] }, ... ] }
```

### UX Flow

```
[User clicks Generate]
  → Stage 1: DECOMPOSE (receives script registry table)
     Progress: "Analyzing prompt and planning tasks..."
  → Stage 2: GENERATE JCWF
     Progress: "Generating workflow definition..."
  → Stage 3: GENERATE SCRIPTS (parallel AI calls by j9t)
     Progress: "Writing scripts/clone_repo.sh (1/2)..."
     Progress: "Writing scripts/countLines.py (2/2)..."
  → Stage 4: VALIDATE
     Progress: "Validating workflow and scripts..."
  → Stage 5: FIX (if needed)
     Progress: "Fixing validation errors (retry 1/2)..."
  → Result:
    {
      "jcwf": { ... },
      "scripts": [
        { "path": "scripts/clone_repo.sh", "content": "...", "executable": true },
        { "path": "scripts/countLines.py", "content": "...", "executable": false }
      ],
      "validationResult": { ... },
      "retries": 0
    }
```

Frontend displays generated scripts in a **ScriptReviewPanel**. User can:
- **Accept all** → backend writes files to `scripts/`, sets `chmod +x` on `.sh` files,
  FileWatcher picks them up and updates the registry automatically
- **Edit** → inline editor for each script before accepting
- **Skip** → JCWF loads but scripts are not written (user creates them manually)

### WebSocket Protocol Extension

Progress messages (reuse existing `ai-generate-progress` type):
```json
{ "type": "ai-generate-progress", "stage": 3, "totalStages": 5,
  "message": "Writing scripts/clone_repo.sh (1/2)..." }
```

Result (extended with `scripts` array):
```json
{ "type": "ai-generate-result", "ok": true,
  "jcwf": { ... },
  "scripts": [
    { "path": "scripts/clone_repo.sh", "content": "...", "executable": true }
  ],
  "validationResult": { ... }, "retries": 0 }
```

Write accepted scripts to disk:
```json
// Client → Server
{ "type": "ai-write-scripts", "workflowId": "t1",
  "scripts": [
    { "path": "scripts/clone_repo.sh", "content": "...", "executable": true }
  ] }

// Server → Client
{ "type": "ai-write-scripts-result", "ok": true,
  "written": ["scripts/clone_repo.sh"],
  "errors": [] }
```

### Security

- Only write to `scripts/` directory (reject paths with `..` or absolute paths)
- Validate filename characters (alphanumeric, hyphens, underscores, dots)
- Overwrite protection: refuse to overwrite existing files unless user explicitly confirms
- Generated scripts must include the `@jarvis-script` header so they are registered

### Files to Create/Modify

**New files:**
- `application/file/scriptRegistry.h` — `ScriptRegistry` class + `ScriptRegistryEntry`
- `application/file/scriptRegistry.cpp` — scan, parse, serialize to Markdown
- `workflow-editor/ui/src/editor/ScriptReviewPanel.tsx` — review UI for generated scripts
- `workflow-editor/ui/src/editor/ScriptReviewPanel.css` — styles

**Modified files:**
- `application/jarvisAgent.h/.cpp` — create second `FileWatcher` for `scripts/`,
  handle script file events, own `ScriptRegistry` instance
- `application/web/aiJcwfService.h/.cpp` — add `GenerateScripts()`, revised pipeline,
  `HandleWriteScripts()`
- `application/web/webServer.cpp` — register `ai-write-scripts` WS handler,
  `GET /api/scripts/registry` endpoint
- `workflow-editor/ui/src/editor/AiPromptArea.tsx` — trigger script review after generate
- `workflow-editor/ui/src/editor/WorkflowEditorView.tsx` — host the review panel

### Implementation Order

| Step | Description |
|------|-------------|
| 5a | Spec: `@jarvis-script` metadata format (§11 in JC_Workflow_Specification.md) — **done** |
| 5b | Backend: `ScriptRegistry` class — scan, parse headers, serialize Markdown table |
| 5c | Backend: second `FileWatcher` for `scripts/`, wire events to registry updates |
| 5d | Backend: `GET /api/scripts/registry` endpoint |
| 5e | Backend: inject registry table into Stage 1 (DECOMPOSE) AI context |
| 5f | Backend: Stage 3 (GENERATE SCRIPTS) — one AI call per new script |
| 5g | Backend: Stage 4 revised — validate script existence + param consistency |
| 5h | Backend: `HandleWriteScripts()` — write to disk + chmod |
| 5i | Frontend: `ScriptReviewPanel.tsx` — preview + accept/edit/skip |
| 5j | Frontend: wire into `AiPromptArea` generate flow |
| 5k | Add `@jarvis-script` headers to existing scripts in `scripts/` |
| 5l | Testing: generate a multi-task JCWF with shell+python, verify scripts |

---

## Open Issue: Unconfigured JCWF Assistant

The JCWF assistant currently assumes a working AI provider is configured in
`config.json`. If the provider is missing, has an invalid API key, or the
endpoint is unreachable, the assistant will silently hang or return a cryptic
error.

**TODO:**
- Detect when no AI provider is configured and show a clear message in the
  prompt area (e.g. "AI provider not configured — open AI Manager to set up a
  provider") instead of attempting the request.
- Surface cURL / HTTP errors from `SessionManager` as user-facing error messages
  in the `ai-generate-result` / `ai-explain-result` WebSocket responses.
- Consider a quick connectivity pre-check (like the planned AI test button in
  TODO §20) before starting the multi-step generation pipeline.
