# Workflow Editor UX Refactor — Development Plan

Companion to **`editor-ux-refactor-plan.md`** (the review: *why*, the principles,
the per-element proposals). This document is the *how* — the code-level plan:
current architecture, the abstractions/unifications that the refactor turns on,
and per-phase implementation detail with file/function anchors.

All paths below are under `code/frontend/workflow-editor/ui/src/` unless noted.

---

## 1. Current architecture (precise)

### 1.1 The data model
- **JCWF task** (`jcwf/types.ts::JcwfTask`) carries `depends_on?: string[]`,
  `filter?: string`, `mode?`, `working_directory?`, `params?`, plus — via the
  `[key: string]: unknown` index signature — the *untyped* `file_inputs`,
  `file_outputs`, `inputs`/`outputs` (dataflow slots), `queue_binding`,
  `output_schema`, `output_retries`. (These being untyped is itself a
  cleanup target — see U7.)
- **Editor nodes** (`editor/types.ts`): discriminated union `EditorNode =
  EditorTaskNode | EditorFilterNode | EditorControlNode`. Filter nodes have id
  `filter:<filterId>`; tasks have id `<taskId>`.

### 1.2 The round-trip (the load-bearing pair)
- **Save** `editor/graphToJcwf.ts::graphToJcwf`:
  - `depends_on` is **derived from `dep:` edges** (ordered by the `dephandle-N`
    target-handle index), then written onto each task (graphToJcwf.ts:138-233).
  - `dataflow[]` derived from `df:` edges (graphToJcwf.ts:150-164).
  - `controlflow[]` from `cf:` edges; **`fanout:` edges are skipped** (not
    serialized — the filter binding lives in `task.filter`).
  - **`file_inputs` is NOT derived here** — it is whatever string array is
    stored on the task. This is the asymmetry the refactor fixes.
- **Load** `editor/jcwfToGraph.ts::jcwfToGraph`:
  - For each `depends_on` entry → a `dep:` edge, with `sourceHandle` chosen to
    match the producer's `file_outputs` (`fileoutput-N`) and `targetHandle
    = dephandle-<depIdx>` (jcwfToGraph.ts:252-300).
  - A per_item task with `task.filter` → an auto `fanout:` edge from
    `filter:<id>` (jcwfToGraph.ts:302-314). **The fanout edge already exists as
    a derived artifact** — area B just needs the inverse (draw → set
    `task.filter`).
  - `dataflow[]` → `df:` edges (jcwfToGraph.ts:317-337).

### 1.3 Two parallel port systems (the redundancy)
`editor/TaskNode.tsx` renders **two** near-identical inline-row port stacks on
each side:
- **File ports**: left `dephandle-N` (from `file_inputs`/`depends_on`), right
  `fileoutput-N` (from `file_outputs`) — TaskNode.tsx:230-300.
- **Dataflow slot ports**: left `in:<name>`, right `out:<name>` (from the
  `inputs`/`outputs` slot objects) — TaskNode.tsx:255-271, 301-317.
Plus hidden catch-alls `dep-target`/`dep-source`, and `error-signal`.

So the handle-id grammar is four shapes (`dephandle-N`, `fileoutput-N`,
`in:<name>`, `out:<name>`) and edges are four kinds (`dep:`, `df:`, `cf:`,
`fanout:`). Two of these (file ports + dataflow slots) express the same idea —
"this output feeds that input" — and that duplication is area C/I.

### 1.4 The path authority (already exists, under-used)
`WorkflowEditorView.tsx::deriveUpstreamOutputPaths(sourceTask, targetTask, wfId,
sourceHandle)` (line 311) resolves the concrete relative path from a producer's
output to a consumer's input, correct through `..`, against the workflow base —
mirroring the backend `TaskPathResolver` contract. **Today it is called
eagerly** at two moments and the *result is stored* on `task.file_inputs`:
- in `onConnect` auto-populate (WorkflowEditorView.tsx:2106),
- in `updateSelectedTaskField`'s re-sync when a path-determining field changes
  (WorkflowEditorView.tsx:2566-2628).
Because the result is *stored*, it *drifts* whenever an upstream rename/wd change
isn't re-propagated — the entire F-15/F-16/F-21/F-22 patch cluster exists to
chase that drift. **This is the single most important thing to change.**

### 1.5 Capability checks, scattered
"Which task types read `file_inputs`" and "which produce outputs" are inlined in
several predicates: `targetReadsFileInputs` (WorkflowEditorView.tsx:2092),
`isInputTarget` (updateSelectedTaskField), the `dep`-vs-`df` handle decisions in
TaskNode, and the backend's own per-type executors. No single source of truth.

---

## 2. Core abstractions & unifications (the common denominators)

These are the reusable pieces every phase builds on. Introduce them as named
modules so the phases share one implementation each.

### U1 — `PathSynthesizer`: input paths are edge-derived **at save time**
**Problem solved:** the stored-`file_inputs` drift cluster (F-15/16/21/22), and
the user ever seeing/editing a `../../queue/...` path (area E).
**Move:** make `file_inputs` a *derived* artifact like `depends_on`. Promote
`deriveUpstreamOutputPaths` out of `WorkflowEditorView.tsx` into a shared module
`editor/pathSynthesis.ts`, and call it **inside `graphToJcwf`** to compute each
task's `file_inputs` from its incoming data edges (ordered by the input-port
index), instead of reading a stored array. Stop writing `file_inputs` from
`onConnect`/`updateSelectedTaskField`; they only manage *edges* now.
- Inputs to the synth: the incoming data edge (producer node + output port) +
  the consumer's working dir. Output: the relative path string.
- Sources can be a **task output** *or* an **artifact-file node** (U3) — one
  code path, one resolver.
- The same module exposes the inverse used on load (reconstruct which producer a
  stored `file_inputs[i]` came from) so round-trip is lossless for existing
  JCWFs that still carry explicit `file_inputs`.
**Net deletions:** the PROB-rename propagation and the `file_outputs`/`wd`
re-sync branches in `updateSelectedTaskField` (WorkflowEditorView.tsx:2514-2628)
largely disappear — nothing stored to keep in sync.

### U2 — One data-port / data-edge model
**Problem solved:** the two parallel port systems (C/I).
**Move:** collapse `dephandle-N`/`fileoutput-N` (file) and `in:`/`out:`
(dataflow) into a single **DataPort** concept: a node exposes ordered typed
**input ports** and **output ports**; an edge `out→in` is *the* data hand-off.
- One handle-id grammar (e.g. `inN`/`outN` with a port descriptor carrying name
  + kind), one edge kind (`data:`), replacing `dep:`+`df:`+the file/slot split.
- Serialization decides representation from the port kind: a file-bearing edge →
  contributes to `depends_on` + (derived) `file_inputs`; a named-value edge →
  a `dataflow[]` entry. The *user* sees one kind of wire.
- `depends_on`-only (ordering, no data) becomes an explicit edge style on the
  same model (dashed/grey), not a separate concept.
**Round-trip:** `jcwfToGraph` reconstructs ports from `file_inputs`/`file_outputs`
+ `inputs`/`outputs` as today, but emits the unified port descriptors; the JCWF
on disk is unchanged.

### U3 — Artifact-file node (editor overlay, not a JCWF task)
**Problem solved:** input files are invisible path strings (D); paths leak (E).
**Move:** a new `EditorNode` kind `"file"` (id e.g. `file:<workflowRelPath>`),
rendered as a small file chip with one output port. It is a **pure editor
overlay**: `graphToJcwf` does **not** emit a task for it; instead an edge
file→task feeds U1's synthesizer to produce the consumer's `file_inputs` path.
`jcwfToGraph` *reconstructs* file nodes from any `file_inputs` entry that
resolves to a real file in the workflow folder rather than an upstream task
output. **JCWF format untouched** (the decision in `editor-ux-refactor-plan.md`
Part 2). Persist file-node canvas positions in the existing `editor_layout`.

### U4 — `taskCapabilities` table (one source of truth)
**Problem solved:** scattered `targetReadsFileInputs`/`isInputTarget` predicates.
**Move:** a single `editor/taskCapabilities.ts` mapping each `JcwfTaskType` →
`{ readsFileInputs, producesFileOutputs, usesQueueBinding, namedSlots, ... }`.
Every port-rendering, onConnect, validation, and synthesis site reads from it.
Mirrors (and is cross-checked against) the backend executors. Makes adding a
task type a one-row change instead of touching five predicates.

### U5 — Inspector field-group abstraction (primary vs advanced)
**Problem solved:** field order / wall-of-inputs (H/I).
**Move:** the task inspector (`WorkflowEditorView.tsx`, the big conditional JSX
block ~3860-5160) becomes a small ordered list of **field-group descriptors**
(`{ id, tier: "primary"|"advanced", render }`), with all `advanced` groups
inside one collapsible `<details>`. Reorder once, in data, not by moving JSX.
Consolidates the duplicated inputs/outputs editors into one group.

### U6 — Pure assist helpers (small, self-contained, independently testable)
- `suggestOutputName(inputBasename) → "<stem>.output.<ext>"` (G) — also the one
  place that encodes the backend `.output.txt|.json` convention (shared with the
  AI output prediction in U1).
- `inferSchemaFromExample(exampleJson) → JSONSchema` (F) — types from values,
  keys required, `additionalProperties:false`.
- `buildFanoutBinding(columns, selected) → prob_files entry` (J) — emits the
  unique-per-row filename template + the per-row content from chosen columns;
  the only place that knows the `{{binding.field}}` grammar.

### U7 — Type the JCWF extras (quality, enables the above safely)
`file_inputs`/`file_outputs`/`inputs`/`outputs`/`queue_binding`/`output_schema`
currently reach `JcwfTask` only through the `[key: string]: unknown` escape
hatch. Add explicit optional fields so U1/U2/U4 are type-checked, keeping the
index signature for forward-compat. Low-risk, do alongside Phase 0.

---

## 3. Per-phase implementation

### Phase 0 — Inspector hygiene & assists (no model change, low risk)
**Touches:** `WorkflowEditorView.tsx` (inspector block), `StructuredOutputEditor.tsx`,
new `editor/{suggestOutputName,inferSchemaFromExample}.ts`, `validation.ts`.
- **U5 field-group reorder** + single `<details>` "Advanced" (`doc`, raw
  `params (JSON)`, `timeout_ms`, dataflow slots, `working_directory`). Move
  `file_inputs`/`file_outputs` + typed params up. Consolidate the two
  inputs/outputs editors. *(area H/I)*
- **U6 `suggestOutputName`**: ghost-text/chip in the `file_outputs` editor
  prefilled from the task's first `file_inputs` basename. *(G)*
- **U6 `inferSchemaFromExample`** + 2-3 presets in `StructuredOutputEditor`
  (new top tier: example → builder → raw). *(F)*
- **Filter ID → `<select>`** of existing filter ids (kill free text), as a
  stopgap before B's edge-implied version. *(B partial)*
- **U7** type the JCWF extras.
**Risk:** low — display + additive helpers; covered by the 70-test editor suite
+ a save/load round-trip check.

### Phase 1 — Files as things + hide paths
**Backend (new — see §4):** `POST /api/workflows/<id>/files` (upload),
`GET /api/workflows/<id>/files` (list).
**Frontend:**
- **U3 artifact-file node**: new node type + `TaskNode`-sibling renderer;
  `jcwfToGraph` reconstruction; `graphToJcwf` overlay-skip; `editor_layout`
  persistence.
- **Drag-and-drop** onto the canvas → upload via the new endpoint → create file
  node. **"+ file"** button → file dialog (upload) or picker (list endpoint).
- **U1 (display half)** + **area E**: render wired inputs as the **producer
  node/file name**, not the path; make `working_directory` an advanced/auto
  field; stop showing raw `../../` strings anywhere.
**Risk:** medium — new node kind in the union (the `n.type === "task"` guards are
centralised via `isTaskNode` / `isFileNode`). New endpoints need the
`ConfineUnderProjectRoot` gate (§4).

### Phase 2 — One data-carrying edge (the structural bet)
**Touches:** `pathSynthesis.ts` (U1), `taskCapabilities.ts` (U4),
`graphToJcwf.ts`, `jcwfToGraph.ts`, `TaskNode.tsx`, `onConnect` +
`updateSelectedTaskField` + `slotsSignature`/`NodeInternalsUpdater` in
`WorkflowEditorView.tsx`.
- **U1 full**: compute `file_inputs` inside `graphToJcwf` from incoming data
  edges; delete the stored-path sync/propagation branches.
- **U2**: collapse the four handle shapes/edge kinds into the unified DataPort +
  `data:` edge; serialization maps port-kind → `depends_on`+`file_inputs` or
  `dataflow[]`. Retire the "declare a slot" inspector flow (ports appear because
  a node produces/consumes, per U4).
- **B full**: `onConnect` of filter→task creates the `fanout:` edge **and** sets
  `task.filter` + `mode:"per_item"`; the "Filter ID" field hides when an edge
  implies it.
**Risk:** highest — it rewires the round-trip. Mitigation: keep `jcwfToGraph`
back-compat for all existing on-disk shapes; add golden round-trip tests
(load→save→load is identity) for all five example JCWFs **before** changing the
save path; re-run the dogfooding pass after.

### Phase 3 — No-code fan-out
**Touches:** new `editor/FanoutBuilder.tsx` + `buildFanoutBinding` (U6); the
per_item branch of the inspector; needs the filter source columns.
- Column source: parse the CSV header **client-side** from the uploaded file
  (Phase 1 already has the bytes) — avoids a new backend endpoint; fall back to
  the optional `GET /api/workflows/<id>/files` content fetch.
- UI: column checkboxes → live row-1 preview → emit the inline `prob_files`
  entry with a guaranteed-unique filename (kills F-40) and content from selected
  columns. Inline/ref (F-25/26) never surfaces.
**Risk:** medium — self-contained UI atop U6; depends on B (filter binding) and
Phase 1 (file bytes/columns).

---

## 4. Backend work items
From the touchpoint map:
- **`POST /api/workflows/<id>/files`** (Studio, operator+, auth): multipart;
  confine under `workflows/<id>/` via `ConfineUnderProjectRoot`
  (`code/backend/application/file/pathConfinement.h`); write the file, then
  `JcwfContainer::Pack()` to refresh the `.jcwf`. Model on
  `HandleWorkflowsCreatePost` (`webServer_studio.cpp`). *(Phase 1)*
- **`GET /api/workflows/<id>/files`** (Studio, operator+): list
  `workflows/<id>/` (`{path,size,modified,is_dir}`), every path confined; model
  on `HandleRunFilesListGet`. *(Phase 1)*
- **`GET /api/workflows/<id>/files/<path>`** (Studio, operator+): download one
  file's content, confined + size-capped (model on `HandleRunFileGet`). Needed
  by the FanoutBuilder (S6) to read a CSV header **on reload** of an existing
  workflow (the freshly-uploaded case has the bytes client-side; the reload case
  does not). *(Phase 3, small)*
- **No path-resolution backend change**: the editor must *match* the existing
  `TaskPathResolver` contract (file_inputs resolved relative to the task working
  dir; empty wd = workflow base; AI outputs `<prob>.output.{txt,json}`) — U1
  encodes exactly this. *(verify, don't change)*
- **CSV header read**: not needed if parsed client-side (Phase 3). Only build a
  read endpoint if client-side parsing proves insufficient.

### 4b. Existing backend seams the frontend worked around — RESOLVED
These were pre-existing friction points the refactor fixed rather than inheriting:
- **`GET /api/workflows/dependency-graph` 404s** — FIXED. The route was shadowed
  by `/api/workflows/<string>` because Crow's router picks the lowest
  (earliest-registered) rule index, not static-over-param. The static route now
  registers ahead of the param route in `webServer.cpp`. Live-verified `{ok:true}`.
- **Validation findings omit a structured `taskId`** — FIXED. The four
  `ValidateDataflows` findings (the only task-scoped ones that lacked it) now set
  `taskId`; the backend reliably populates it on every task-scoped finding, so the
  editor's dead `parseTaskIdFromMessage` regex fallback was removed — node
  attribution no longer depends on message wording.
- **Per-file existence polling — KEPT (not subsumed).** `/api/files/check`
  resolves each `file_input` at **project-root scope** via `TaskPathResolver`
  (incl. out-of-folder cross-task paths). `GET /api/workflows/<id>/files` only
  enumerates files under `workflows/<id>/`, so a client-side membership check
  would falsely flag legitimately out-of-folder inputs as missing. Not a safe
  swap; the per-file check stays.
- **Frontend mirrors backend path resolution (drift risk).** `pathSynthesis`
  (U1) re-implements `TaskPathResolver` + the `.output.{txt,json}` convention;
  the `.output.txt`-always bug proves the mirror can drift. Add a **contract
  test**: a tiny backend debug endpoint (or reuse `debug/signals` style) that
  returns the runtime-resolved `file_inputs`/output paths for a task, asserted
  equal to `pathSynthesis` output for the five example workflows. Cheap
  insurance against the two implementations diverging again.
- **Two `.jcwf` mutators (Phase 1 coordination).** The new upload endpoint and
  the existing canvas PUT both write + repack the container. Spec: upload writes
  the file + repacks and returns the updated file list; the editor **refetches**
  before a subsequent canvas save so the two don't race or double-repack. No
  shared-lock gymnastics — sequence them client-side.

---

## 5. Risk, round-trip & test strategy
- **Format invariant:** the JCWF on-disk schema does **not** change in any phase
  (artifact nodes and the unified ports are editor-side overlays). This is the
  primary safety property — shipped workflows and the runtime are unaffected.
- **Golden round-trip tests** (add first, before Phase 2): for each of the five
  example JCWFs, `jcwfToGraph` → `graphToJcwf` must be an identity (modulo the
  documented normalizations). This is the regression net for U1/U2/U3.
- **Editor unit suite** (70 tests) gates Phase 0/1.
- **Re-run the dogfooding pass** (`editor-dogfooding-plan.md`) after Phase 2 and
  Phase 3 — success = near-zero new "had to know an internal mechanic" findings
  (the plan's success criterion).
- **Backend negative-path:** the two new endpoints get path-confinement +
  size-cap tests in `test/hardening/` (the project's established pattern).

---

## 6. Sequencing & dependencies
```
Phase 0  (independent)                          ── ship first; pure win
   │  U5 reorder · U6 suggest/infer · U7 types · B(select)
   ▼
Phase 1  needs: backend upload/list endpoints
   │  U3 file node · drag-drop · U1(display) · E(hide paths)
   ▼
Phase 2  needs: Phase 1 (file nodes as a producer kind), golden round-trip tests
   │  U1(full, save-time) · U2(unified ports/edge) · U4 · B(full)
   ▼
Phase 3  needs: B (filter binding) + Phase 1 (file bytes for columns)
      U6 buildFanoutBinding · FanoutBuilder
```
Critical-path abstractions to build early because everything leans on them:
**U4 `taskCapabilities`** and **U1 `pathSynthesis`** (extract from
`WorkflowEditorView` in Phase 0/1 even before their full Phase-2 use).

---

## Resolved decisions (were "open" in the review plan, Part 2)
1. **Artifact nodes** = pure editor overlay reconstructed on load (format-neutral).
   Detection is FS-free — see Appendix A/S3.
2. **`depends_on`-only edge** = a flag/style on the unified data edge (a wire
   whose consumer port has no file/value bound), not a separate kind.
3. **Handle-id grammar: do NOT migrate it.** Keep the existing handle ids
   (`dephandle-N`/`fileoutput-N`/`in:`/`out:`); unify at the *classify +
   serialize + visual* layer instead (Appendix A/S5). This removes the riskiest
   piece of Phase 2 (no in-memory migration, no churn across TaskNode/onConnect/
   round-trip) while still giving the user "one kind of wire."
4. **Edge into an `ai_call` consumer** populates a **`cntx_files` ref**, not
   `file_inputs` (Appendix A/S4) — the idiomatic AI-input path (how
   portfolioSummary reads per-item outputs). Supersedes F-23's file_inputs
   auto-populate for ai_call.

---

# Appendix A — Resolved specs (blackbox elimination)

Each subsection is meant to be implementable verbatim tomorrow. Signatures are
TypeScript; "pure" means no React/IO so it gets a unit test.

## S1 — `inferSchemaFromExample(example): JSONSchema`  (U6 / area F)
Pure. New file `editor/inferSchemaFromExample.ts`. Maps an example JSON **object**
to a Draft-2020-12 subset schema (the same subset the builder + backend
`SchemaValidator` support: type/enum/min/max/minLength/maxLength/pattern/
required/properties/additionalProperties).

Algorithm — `schemaOf(value)`:
- `null`  → `{}` (no constraint) **and** the key is **omitted from `required`**
  (a null example means "optional / unknown").
- `boolean` → `{ "type": "boolean" }`
- `number` → `Number.isInteger(v) ? { "type":"integer" } : { "type":"number" }`
  (heuristic; the user can widen in the builder — note it in the UI).
- `string` → `{ "type": "string" }`  (no enum — a single example is not an enum;
  enums are added in the builder only).
- `array`  → `{ "type":"array", "items": schemaOf(v[0]) }`; empty array →
  `{ "type":"array" }` (no `items`).
- `object` → `{ "type":"object", "properties": {k: schemaOf(v[k])},
  "required": [keys with non-null value], "additionalProperties": false }`.

Top level: if `example` is not a JSON object, return
`{ "type":"object", "properties":{}, "additionalProperties":false }` and surface
a hint "paste a JSON *object* — one example answer." 

Integration: the inferred object is fed straight through the **existing**
`StructuredOutputEditor` commit path — `handleSchemaChange(JSON.stringify(schema,
null,2))` — which already re-seeds the builder rows (`parseSchemaToRows`) and the
raw textarea. So area F is: one pure function + a small "infer from example"
textarea/button; **no new commit plumbing.** Mixed-type arrays / deep nesting
are intentionally coarse (first-element items, `{}` fallback) — the raw textarea
remains for hand-tuning.

## S2 — `editor/pathSynthesis.ts` (U1, the keystone)
Extract + generalize `deriveUpstreamOutputPaths`. Producer abstraction:
```ts
type ProducerRef =
  | { kind: "task"; task: JcwfTask; portIndex?: number }   // upstream task output
  | { kind: "file"; workflowRelPath: string };             // artifact-file node (S3)
function resolveProducerOutput(p: ProducerRef, consumerWd: string, wfId: string): string
```
- **task / ai_call**: output name = declared `file_outputs[portIndex]` if any,
  else (ai_call) the PROB stem → `.output.<ext>` where **`ext = task.output_schema
  ? "json" : "txt"`** — *this fixes the current `.output.txt`-always bug*
  (`deriveUpstreamOutputPaths` line 331 ignores `output_schema`; the backend uses
  `.json` when a schema is set, aiCallTaskExecutor.cpp:1045). Then
  `relativePathBetween(base/consumerWd, base/sourceWd/outputName)`.
- **task / shell|python|internal**: `file_outputs[portIndex]` (narrow by the
  `fileoutput-N` handle), same relative-path step.
- **file**: `relativePathBetween(base/consumerWd, base/workflowRelPath)`.

> **Critical: resolve each working_directory through `resolveTaskDirSegments(wfId, wd)`, NOT a plain
> `base/wd` join.** It mirrors the backend `TaskPathResolver::ResolvePath` **base-leaf-strip**: when a
> wd's first segment equals the workflow-folder leaf (`<wfId>`), it resolves against `workflows/`, not
> `workflows/<wfId>/`, so a `<wfId>/…` wd (e.g. aiZipDemo's shell `aiZipDemo/04_zip_responses`) isn't
> doubled. A plain `base/wd` join drifts by one `..` and silently corrupts the save. (`taskPathResolver.cpp:160-185`.)

**Save-time** (`graphToJcwf`): for each consumer task whose capability input
mechanism is `file_inputs` (S4), build `file_inputs` from its incoming data edges
ordered by the target port index (`dephandle-N`):
`file_inputs[i] = resolveProducerOutput(edgeSource(i), consumerWd, wfId)`.
For an `ai_call` consumer, write a **`cntx_files` ref** instead (S4).
**Per-item producer → glob ref:** when the producer task is `mode:"per_item"`,
its outputs are many (`PROB_<sym>_<NN>.output.<ext>`), so `resolveProducerOutput`
emits a **glob** `<relpath>/PROB_*.output.<ext>`, not a single file — exactly the
shipped `portfolioSummary` cntx pattern. Single-mode producer → one concrete
path. (`deriveUpstreamOutputPaths` today maps prob entries individually and has
no per_item/glob branch — add it.)
**Transition rule (Phase 1 → 2):** until artifact nodes (S3) cover externals,
`file_inputs = [edge-derived for ports with an incoming edge] ∪ [stored entries
with no corresponding edge]` (preserve external inputs that aren't yet file
nodes — never drop them). Once Phase 1 lands, every input has an edge (task or
file node) and the union's right side is empty → fully derived.

**Load-time inverse** (`jcwfToGraph`, already partly present at 252-300): for
each stored `file_inputs[i]`, find the producer it came from by comparing it to
`resolveProducerOutput(t, consumerWd)` for each `t` in `depends_on`; on a match,
draw the data edge to that producer's `fileoutput-N`. No match → it's an
external file → emit a file node (S3). This is **pure path arithmetic**, no FS.

**Net deletion:** the PROB-rename propagation + `file_outputs`/`wd` re-sync
branches in `updateSelectedTaskField` (WorkflowEditorView.tsx:2514-2628) — once
inputs are derived on save, nothing stored drifts.

## S3 — Artifact-file node load detection, **FS-free** (U3)
The client has no filesystem, so detection is by **path-matching, not stat**:
1. A `file_inputs[i]` is an **upstream output** iff it equals
   `resolveProducerOutput(t, consumerWd, wfId)` for some `t ∈ depends_on` (S2
   inverse) → draw a data edge, **no file node**.
2. Otherwise it is an **external file** → file node with id
   `file:<workflowRelPath>` where `workflowRelPath =
   normalizePathSegments(consumerWd + "/" + file_inputs[i])` made relative to
   `workflows/<wfId>/`. Dedupe by id (one node, N out-edges if several tasks
   read it).
The `GET /api/workflows/<id>/files` endpoint is needed only for the **"+ file"
picker** and existence validation — **never for load reconstruction.** Drag-drop
upload creates the node directly from the uploaded path.

## S4 — `editor/taskCapabilities.ts` (U4) — the filled matrix
Model each type as `{ input: InputMechanism; output: OutputMechanism; queueBinding: boolean; namedSlots: boolean }`:
```
InputMechanism  = "file_inputs" | "queue_cntx" | "params" | "none"
OutputMechanism = "file_outputs" | "ai_reply"  | "params_file" | "none"
```
| type | input | output | queueBinding | namedSlots |
|---|---|---|---|---|
| python | file_inputs | file_outputs | – | yes |
| shell | file_inputs | file_outputs | – | yes |
| internal | file_inputs | file_outputs | – | – |
| ai_call | queue_cntx | ai_reply | yes | yes (outputs) |
| sub_workflow | none | none | – | – |
| db_query | none | params_file (`params.output_file`) | – | – |
| s3 / onedrive_* / azure_blob_* / gcs_* | params (`local_path`/`file_path`) | params_file (downloads) | – | – |
| polarion_write / snowflake_query / slack_message / email_send / email_read / github_issue / jira_issue / redmine_issue / sheets_read / sheets_write | none | none | – | – |

**Edge-wiring boundary:** only `input ∈ {file_inputs, queue_cntx}` and `output ∈
{file_outputs, ai_reply}` participate in the data-edge model (get file ports).
**Cloud/`params_file` types do NOT get file ports** in Phase 2 — their file
paths are `params` strings (param-level templating only; out of scope). Every
`targetReadsFileInputs`/`isInputTarget` predicate and TaskNode port-render branch
reads from this table. (Source: backend executor audit — file:line citations in
the session investigation; cross-check on edit.)

## S5 — Unify ports at the logic/visual layer, not the grammar (U2)
Keep handle ids. Add one classifier used by `onConnect` **and** `graphToJcwf`:
```ts
type EdgeClass = "fileflow" | "dataflow" | "controlflow" | "fanout";
function classifyEdge(sourceHandle, targetHandle, sourceTask, targetTask): EdgeClass
```
Rules: `out:`/`in:` → dataflow; `cf-*`/`error-signal` → controlflow; filter
source → fanout; everything else between two wire-able tasks (per S4) →
fileflow. **Serialization map:** fileflow → `depends_on` (+ S2-derived
`file_inputs`/`cntx_files`); dataflow → `dataflow[]`; fanout → skip (binding is
`task.filter`, set by S-B); controlflow → `controlflow[]`. **Visual unification:**
render the file-port rows and the dataflow-slot rows with one shared style so the
user perceives a single "data wire"; the distinction stays internal. This
delivers "one kind of wire" with zero handle-id migration.

## S6 — `buildFanoutBinding(...)` (U6 / area J)
Pure. `editor/buildFanoutBinding.ts`:
```ts
function buildFanoutBinding(binding: string, selectedColumns: string[]):
  { path: string; content: string }
```
- **path** (uniqueness is structural, not the user's problem):
  `PROB_{{<binding>.<firstSelected||"item">}}_{{<binding>.row_number_padded}}.txt`
  — the trailing `_{{...row_number_padded}}` guarantees one file per row even if
  the symbol column repeats, so **F-40 cannot recur by construction.**
- **content**: one line per selected column —
  `"<Col>: {{<binding>.<Col>}}\n"` joined. e.g. `pos` + [Symbol,Name,Percentage]
  → `"Symbol: {{pos.Symbol}}\nName: {{pos.Name}}\nPercentage: {{pos.Percentage}}\n"`.
- The `FanoutBuilder.tsx` component shows column checkboxes (columns from the
  parsed CSV header — Phase 1 already holds the bytes; parse client-side) and a
  **live row-1 preview** (substitute the first data row's values). The user never
  sees `{{ }}` or the inline/ref distinction; the builder always emits the
  correct inline `prob_files` entry.

## S7 — Phase 0 exact inspector order + golden-test normalizations
**Target field order** (driven by S4 + the U5 group descriptors):
- **Primary:** label · type · mode (+ filter via select/edge) · [ai_call] AI
  interface, Structured output · typed params ([shell] command/args, [python]
  module/function, [internal] action, [cloud] typed params) · file_inputs (if
  `input==file_inputs`) · file_outputs (if `output==file_outputs`) · [ai_call]
  queue_binding · depends_on (read-only chips).
- **Advanced** (one collapsed `<details>`): working_directory · timeout_ms ·
  expose_error_signal · doc · raw `params (JSON)` fallback · Dataflow named
  value slots (relabeled "Named value inputs/outputs — advanced").

**Golden round-trip test = "save is idempotent," not raw identity.** `graphToJcwf`
applies known one-time normalizations, so the assertion is
`save(load(save(load(jcwf)))) === save(load(jcwf))` (stable after the first
normalize). The normalizations to expect (so the test doesn't false-fail):
(a) ai_call STNG/TASK/CNTX defaults injected if missing (graphToJcwf.ts:237-258);
(b) inline-empty-with-ref-path → ref string (:260-298);
(c) dataflow dedupe + orphan-slot drop (:316-326);
(d) tasks & filters sorted by id; depends_on rebuilt + sorted from edges;
(e) version `1.0`/`1.1` chosen by feature presence;
(f) `editor_layout` added.
Add this test (all 5 example JCWFs) **before** touching the save path in Phase 2.
