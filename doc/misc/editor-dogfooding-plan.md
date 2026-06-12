# Workflow Editor Dogfooding Plan

Rebuild each curated example workflow **by hand in the editor** — no JSON editing, no
AI generation — then save, validate, run, and confirm it behaves like the shipped
original. The point is to exercise the editor the way a first-time user would and
surface every friction point, missing field, and papercut along the way.

Five workflows, ordered easiest → hardest so each one introduces one new editor
muscle. Work through them in order; each section is a self-contained build script you
follow click-by-click. Claude rides along: when a run fails on a path or a field is
missing, that's the live-debug moment, not a doc error.

---

## How to use this plan

The loop for every workflow:

1. **Read "What it exercises"** so you know which editor features this one is testing.
2. **Stage prerequisites** — copy the listed data files into the new workflow folder
   (scripts and Python modules are shared in `scripts/`, never need copying).
3. **Build it** following the numbered editor steps.
4. **Validate** (sidebar *Validate*) → fix anything the validator flags.
5. **Save** under the hand-build id (see convention below).
6. **Run** (sidebar *Run*) and watch the node badges / inspector runtime panel.
7. **Verify** against the shipped original (same outputs, same shape).
8. **Mark complete** in the progress table and **log any friction** in the Findings log.

### Conventions

- **Build under a distinct id.** Append `Hand` (camelCase ids) or `-hand` (hyphenated
  ids) — e.g. `make-example` → `make-example-hand`, `aiZipDemo` → `aiZipDemoHand`.
  This keeps the shipped `example/workflows/*.jcwf` source pristine as a reference to
  diff against, and avoids clobbering the runtime `workflows/<id>/` folders the test
  suite depends on.
- **Shipped source is the answer key.** The extracted reference for each lives at
  `workflows/<original-id>/` (canvas + `global.json`); the git-tracked source is
  `example/workflows/<original-id>.jcwf`. Peek if stuck — but try the build cold first;
  a build that needs the answer key is itself a finding.
- **Server:** a Studio instance must be running (`./jarvisagent.sh`); editor at
  `https://localhost:8443/editor`. For a true first-user feel, dogfood on a **Release**
  build; Debug is fine for pure UX.
- **Shared resources need no copying** — every `scripts/*.sh` and every Python module
  (`combineEngineTroubleshootingGuide`, `printFileInfo`) already lives in `scripts/` and
  resolves project-relative.

---

## Progress

| # | Workflow | New editor muscle | Built | Validated | Ran | Verified |
|---|----------|-------------------|:-----:|:---------:|:---:|:--------:|
| 1 | make-example | nodes, dependency edges, shell `command`/`args` templating | ☑ | ☑ | ☑ | ☑ |
| 2 | aiZipDemo | ai_call, queue-binding (STNG/CNTX/TASK/PROB), fan-in | ☑ | ☑ | ☑ | ☑ |
| 3 | aiCarMaintenancePipeline | structured output schema, `internal` task, dataflow | ☑ | ☑ | ☑ | ☑ |
| 4 | vehicleTroubleshootingGuide | parallel structured AI, Python combiner, named dataflow | ☑ | ☑ | ☑ | ☑ |
| 5 | portfolioDividendAnalysis | CSV filter + per-item fan-out, `{{item.*}}` templates | ☑ | ☑ | ☑ | ☑ |

---

## Findings log

Record every editor gap, papercut, confusing label, or missing field here as we hit it.
Seeded with what the code review already suggests we'll meet — confirm or refute each
during the build.

| # | Workflow | Friction | Severity | Status |
|---|----------|----------|----------|--------|
| F-42 | 5 | **Queue-binding *path* fields (`FilePathInput` in `QueueBindingEditor`) were effectively uneditable** — a typed char wouldn't stick and the caret appeared to snap to the end. Presented as a caret bug; it was not. The `working_directory` plain `<input>` edited fine — only the `QueueBindingEditor` path field. | **HIGH / functional** | **FIXED** — instrumentation (per *instrument-before-guessing*) showed `onChange` fired with the correct value, then `updateSelectedTaskField` **threw** `Cannot read properties of undefined (reading 'file_inputs')` synchronously inside the handler → `setNodes` never committed → React reverted to the old value every keystroke. Root cause: the PROB-rename-propagation branch mapped over **every** node reading `n.data.task.file_inputs` with no type guard; `portfolioDividendAnalysis` has a CSV **filter** node (`n.data.task` `undefined`) → crash. The wd field was immune because its branch already guarded `n.type !== "task"`. Fix: one-line type guard (now `isTaskNode(n)`). The three prior caret attempts were chasing a symptom; the caret-restore machinery was removed and `FilePathInput` reverted to a plain controlled input. Durable safeguard: `isTaskNode` type guard added in `types.ts` (the node arrays are typed `EditorTaskNode[]` but hold filter/branch nodes — TS won't catch the missing guard). |
| F-41 | 5 | **A per_item task's parent node stays in the running animation after the fan-out fails.** When a per_item run fails (e.g. a child `ai_call#12` trips the watchdog), the parent canvas node keeps pulsing "running" — the runtime snapshot maps child instance states (`ai_call#N`) but the parent node's `runtimeState` is never reset to failed. `list_active_runs` confirms nothing is live; a hard-refresh clears the stale pulse. | UX | **FIXED** — confirmed via a live run that per_item children report as `<parentId>#N` with a bare `<parentId>` parent entry; on an abort the backend freezes the parent non-terminal. `reconcileTerminalParentStates` (in `WorkflowEditorView.tsx`) now derives a non-terminal parent's state from its `#N` children (any failed → failed, else any cancelled → cancelled, else all succeeded → success) on the two terminal REST paths (`fetchFinalRunState` + the terminal poll). Only touches a provably-wrong (non-terminal-on-terminal-run) parent, so correctly-reported parents are untouched. |
| F-40 | 5 | **A per_item task with a non-templated PROB *path* silently collides all fan-out items onto one file.** The PROB *content* can carry `{{pos.*}}` while the PROB *filename* stays the default `PROB_new_1.txt`; every one of the N rows materialises + writes to the same `PROB_new_1.{txt,output.txt}`, racing → the file-activity watchdog expires on one item → the whole run fails. No editor-time signal. The F-11 collision class, now for fan-out. | functional/UX | **FIXED** — `validation.ts` warns on a per_item `ai_call` whose PROB *path* lacks a `{{…}}` per-row template token (a unique path per row is required). Verified it does **not** fire on the shipped `lookupDividend` (`PROB_{{pos.Symbol}}_{{pos.row_number_padded}}.txt`). |
| F-39 | 5 | **Dragging an edge from a filter node onto a per_item task deadlocks the run.** A manual dependency edge from the filter node serialised as `depends_on: ['filter:<id>']` — a dependency on a non-task that never completes → runtime **"deadlock/cycle detected"**. The filter→per_item relationship lives in the task's `filter` field, not a dependency edge. | **HIGH / functional** | **FIXED** — `graphToJcwf` builds `depends_on` only from edges whose **source is a real task** (a filter/branch-node source is dropped at save). Companion UX gap (not yet done): the drag should *set the per_item binding* / draw a fanout edge rather than a dep edge, and the fanout edge currently has no live visual until reload. |
| F-38 | 5 | **The Filter builder silently discards the entire edit when the filter id is renamed.** `onFilterBuilderSave` located the node to update by the **new** `updatedFilter.id`; renaming `filter_1 → positions` in the dialog matched zero nodes, so path / kind / binding / everything was dropped with no error (reopening showed the original empty filter). Made the whole filter unconfigurable by hand, which then 404'd the run (per_item task pointing at a filter that never persisted). | **HIGH / functional** | **FIXED** — `openFilterBuilder` captures the original id in a ref; `onFilterBuilderSave` matches the node on it (not the renamed id) and re-points any fanout edge to the renamed node. |
| F-37 | 4 | **The full-size default catch-all dep handle overlaps the inline dataflow slot rows.** When a node declares dataflow `inputs`/`outputs` but has no `file_inputs`/`depends_on` (so `hasDepHandles` is false), `TaskNode` rendered the big centered `dep-target`/`dep-source` handle, which sat on top of the first inline `in:`/`out:` slot — unreachable, and it stole drops. Sibling of F-28 (which fixed dataflow-vs-file overlap; this is catch-all-vs-dataflow). Hit wiring `combineGuideMd`'s input slots. | functional | **FIXED** — the visible-vs-hidden catch-all is now gated on `hasInlineLeftHandles` / `hasInlineRightHandles` (dep handles **or** dataflow slots), so the catch-all is hidden whenever any inline handle exists. |
| F-36 | 4 | **An auto-appended file-input port isn't registered with ReactFlow → the edge dangles.** `onConnect`'s auto-populate appends a new `file_inputs` entry (a `dephandle-N` port) and retargets the edge onto it, but `slotsSignature` (which drives `NodeInternalsUpdater`) keyed only on dataflow slot **names** — not `file_inputs`/`depends_on`/`file_outputs` — so the new port was never re-measured and the edge pointed at an unknown handle. Sibling of F-31 (same root mechanism, file ports). The 2nd edge into `combineGuideMd` dangled. | functional | **FIXED** — `slotsSignature` now also includes `file_inputs`/`depends_on`/`file_outputs` lengths, so an appended port registers immediately. |
| F-35 | 4 | **`defaults.ai.provider` / `ai.model` were free-text → silent mis-resolution.** `provider` is matched against the configured interface **name** (`aiCallTaskExecutor.cpp:1208`); a value that isn't an exact name (e.g. `openai`) silently falls through to the global API index with no editor-time signal — so the workflow had been running on the default index, not the named provider. | UX/functional | **FIXED** — both are now `<select>`s sourced from `/api/settings/ai-interfaces` (already loaded as `aiInterfaces`): provider lists interface names, model lists the distinct interface models (empty = inherit the interface's model); picking a provider auto-fills a blank model; an existing non-configured value is preserved as a `… (not configured)`/`(custom)` option. |
| F-34 | 4 | **Structured-output retries validate JSON shape, not downstream renderability.** `output_retries` re-asks only on JSON-schema mismatch; a `{title, mermaid}` reply with un-renderable Mermaid (parens in a label, `-->` inside a label, nodes jammed `]E[`) is a valid non-empty string → passes every retry and only fails two stages later at the shell PDF render (mermaid-cli, exit 256). Small local models (qwen7b) need retries≈5 to satisfy the schema and still emit invalid Mermaid on some rolls. | functional/design | **Out of scope (by design) — not a j9t defect.** Structured output validates *shape* (the engine's job); *semantic/render* validity is a **workflow-design** responsibility the designer composes from existing primitives. The render task already *is* the validator (its exit code = is-the-Mermaid-good); `expose_error_signal` + a branch node wire that failure into an error branch (re-ask `ai_call`, fall back, or fail with a custom message). Baking a Mermaid-specific gate into the dispatch core would hardcode one renderer's policy into the engine — the opposite of j9t's compose-from-primitives model. Practical mitigations for a weak model: raise `output_retries`, pick a stronger interface, or Clean+re-run. |
| F-33 | 4 | **A duplicated `ai_call` node carries no per-node identity, so it's easy to paste the wrong PROB.** Copy-pasting an AI node to make the sibling tasks (244/250/301) leaves three near-identical nodes; the wrong PROB pasted into one (250 into the 244 node) surfaced only at **run** as a duplicated/missing code in the output — no editor-time signal that two PROBs were identical or that a code was missing. | UX | **FIXED** — `validation.ts` adds an info when two `ai_call` tasks share identical (non-empty, inline) PROB content, naming the siblings. |
| F-32 | 4 | **The structured-output editor is a raw JSON-Schema textarea — no field builder, no `additionalProperties` toggle.** Building the `{title, mermaid}` schema by hand requires knowing JSON-Schema syntax (and that `additionalProperties:false` is the "no extra keys" keyword); the only aid is "insert example schema". Leaks schema mechanics the model should own. | UX | **FIXED** — `StructuredOutputEditor` now leads with a **field builder**: name/type(`string`/`number`/`integer`/`boolean`/`object`/`array`)/required rows, an optional comma-separated `enum` for string fields, and an "allow extra fields" toggle that writes `additionalProperties`. Builder edits regenerate the schema; the raw JSON-Schema textarea is kept as a collapsible advanced fallback and re-seeds the builder when it parses. |
| F-31 | 3 | **A just-added dataflow slot's port wasn't connectable until a save+reload.** Adding an `inputs`/`outputs` slot in the inspector renders a new `<Handle>`, but ReactFlow doesn't re-measure node internals on its own, so the handle isn't registered — the port looks present but rejects connections until the workflow is saved and reloaded (which re-mounts everything). This was the root cause of the whole "can't connect the dataflow" struggle. | functional | **FIXED** — a `NodeInternalsUpdater` rendered inside `<ReactFlow>` calls `updateNodeInternals` whenever the per-task slot-name signature changes, so new ports register immediately and are connectable without a save. |
| F-30 | 3 | **Duplicate + orphaned dataflow edges.** Re-dragging the same wire piled up identical `dataflow` entries (one run accumulated 11), and renaming a slot left stale edges pointing at the old name (a `archhive_path` typo slot, renamed to `archive_path`, left 6 dead edges) — which broke the run (`get_file_info() missing 'filename'`) and rendered as a tangle of overlapping teal edges. | functional | **FIXED** — `onConnect` refuses a duplicate dataflow edge; `graphToJcwf` dedupes `dataflow` entries on save and drops orphans whose `from_output`/`to_input` slot no longer exists on the task. |
| F-29 | 3 | **Dataflow edge wouldn't connect.** With ReactFlow in default **strict** connection mode and the `filename` dataflow port sitting next to `printZipInfo`'s `answer.zip` file-input port, the drag was finicky — easy to mis-snap to the wrong handle or get rejected by direction. | UX | **FIXED** — `connectionMode={ConnectionMode.Loose}` (connect from either end, more forgiving snapping) + `onConnect` now normalizes direction (output-like handle → source, input-like → target) so a reversed drag still resolves correctly, for dataflow, dep, and controlflow edges. |
| F-28 | 3 | **Dataflow ports overlap file-I/O ports on the node face — can't grab them.** File-input/output handles render as *inline* row handles (CSS-positioned per row, so they stack), but dataflow slot handles were *absolutely* positioned at `top: 50+(idx+1)*16%`. On a task with both a file output and a dataflow output (e.g. `zipAnswer`: `answer.zip` file + `archive_path` slot), the two land on the same spot on the right edge — the dataflow port is unclickable, so the edge can't be drawn. | functional | **FIXED** — `TaskNode` now renders dataflow input/output slots as the same inline-row handles as file I/O (`taskNodeInlineHandle`/`…Right`, teal), so they stack below the file ports instead of overlapping. Handle ids (`in:<name>`/`out:<name>`) unchanged, so `onConnect`/serialisation are unaffected. |
| F-27 | 3 | **`python` task had no dedicated `module`/`function` fields.** The inspector's params editor special-cased `shell` (command/args) and fell back to a raw **params (JSON)** textarea for everything else — so a python task's `module`/`function` had to be hand-written as JSON. Same gap class as F-2 (internal `action`). | UX | **FIXED** — dedicated **params (python)** block with `module` + `function` inputs (preserves any extra params via spread); raw JSON remains the fallback for other types. |
| F-26 | 3 | **The queue-binding inline/ref toggle button labels the *action*, not the *state*.** The button reads `ref` when the entry is currently **inline** (click → becomes ref) and `inline` when it's a ref — so a user reads `ref` as "this is a ref" and never clicks it. Caused F-25 to recur twice: the PROB stayed inline despite the user believing they'd switched it. Also: the queue-binding path fields used a plain input that clipped the *end* of long paths (the F-18 issue, not yet applied here). | UX | **FIXED** — replaced the single ambiguous button with a **two-segment `inline | ref` control**: the active mode is highlighted, the other segment is the click target to switch (each side a clear target; the combined-text button read as not-clickable). Queue-binding path fields also now use `FilePathInput` (scroll-to-end) so the filename is visible (extends F-18 to `QueueBindingEditor.tsx`). |
| F-25 | 3 | **An inline queue-binding entry with a ref-style (traversal) path fails silently at runtime.** A PROB entry left as *inline* (`{path, content}`) but given a `../../../workflows/…/message.txt` path makes the runtime compute the expected reply at the resolved source location (`workflows/…/message.output.txt`) instead of the task's queue wd where the reply actually lands → the file-activity watchdog expires ("no curl dispatch within 5s") with no editor-time signal. Re-adding a PROB defaults to inline, so it's easy to give it a ref path without toggling `ref`. | UX/functional | **FIXED** — `graphToJcwf` now auto-normalizes on save: an inline entry whose `path` contains `/` or `..` **and** has empty content is rewritten as a ref string (reads the existing file). Targeted so legit inline entries (simple filename, or real content) are untouched. Pairs with the F-26 toggle clarity. |
| F-24 | 3 | **Save/API errors swallowed the backend's reason.** `api/workflows.ts::ensureOk` threw only `HTTP <status> <statusText>`, discarding the JSON error body (`MakeWorkflowJsonError → { code, message }`) — so a rejected save showed a bare "HTTP 400 Bad Request" with no clue why. | UX | **FIXED** — `ensureOk` now reads the error body and throws `HTTP <status>: <code>: <message>`; all 15 call sites awaited. (Surfaced while diagnosing a save that 400'd on a `…/message.txt.txt` double-extension PROB ref.) |
| F-23 | 3 | **An edge into an `ai_call` dangled.** `ai_call` wasn't in the auto-populate target set (`targetIsScript` = shell/python/internal), so wiring into one neither auto-filled its `file_inputs` nor ran the F-9 edge-retarget — with no input ports yet, the edge bound to the hidden catch-all handle, and once a port appeared (input added via the inspector) the edge was left dangling. Hit wiring `internal → answerWithManual`. | functional | **FIXED** — `ai_call` added to the file-input target set (renamed `targetReadsFileInputs`), so an edge into an `ai_call` auto-fills the input and snaps to the matching `dephandle-N` port like the other types; F-21 sync extended to `ai_call` targets too. |
| F-22 | 3 | **No wired-input suggestion downstream of an `internal` task, and the path math skewed across the queue/workflow boundary.** Two bugs in `deriveUpstreamOutputPaths`: (a) it handled only `ai_call`/`shell`/`python` sources, so an `internal` upstream (which writes `file_outputs` like `manual.txt`) produced no candidate for its downstream — the live symptom on `answerWithManual`; (b) it counted target-wd segments naively, so when a downstream wd contains `..` (e.g. an `ai_call` at `../../queue/<wf>/<task>` reading a file the upstream wrote at the workflow base) it produced a wrong path like `../../../../../manual.txt`. | functional | **FIXED** — `internal` added as a source; path derivation rewritten to resolve both wds against the workflow base (`workflows/<wfId>/`) and take the relative path between them (`normalizePathSegments` + `relativePathBetween`), correct through `..`. Verified on 4 cases incl. internal→ai_call (`../../../workflows/<wf>/manual.txt`). |
| F-21 | 3 | **Wired `file_inputs` go stale when the upstream's path changes — broader than F-15.** F-15 only patched a *rename* of an existing `file_outputs` name; a sweep found four uncovered cases (all node types): (1) `file_outputs` set **empty→value** (no old name to match — the live bug: classify's output set after wiring left the internal task on the stale PROB fallback `message.txt.output…`); (2) a **PROB-derived fallback** input that never matched any old output name; (3) the upstream's **`working_directory`** changing (only the filename tail was patched, not the directory); (4) the **target's own `working_directory`** changing (its inputs are relative to its wd). | functional | **FIXED** — replaced the string-rename propagation with an **edge-anchored recompute**: when a task's `file_outputs`/`working_directory` changes, downstream inputs (and, on a wd change, its own inputs) are recomputed from the upstream via the shared `deriveUpstreamOutputPaths` helper, anchored to each dep edge's `dephandle-N` port. Covers all four cases and every source type. |
| F-20 | 3 | **`+ file_input` didn't offer the already-wired upstream output.** Auto-populate fires only at edge-draw time; clicking `+ file_input` added a blank row with no awareness of an incoming edge, so a user who wired the dependency first then added the input by hand got an empty box and no hint of the path. | UX | **FIXED** — the file_inputs editor now derives the wired upstream output paths (shared `deriveUpstreamOutputPaths` helper, also used by `onConnect` — extracted to avoid a second copy), offers them as a **datalist dropdown** on each field, and **pre-fills `+ file_input`** with the next wired-but-unused path (button reads `+ file_input (N wired)`). |
| F-19 | 3 | **`internal` task has no `file_inputs`/`file_outputs` editor.** Both sections were gated to `shell`/`python`/`ai_call`, but a backend `internal` task (e.g. `buildManual` / `CarMaintenanceTask`) reads its input via `file_inputs` and writes via `file_outputs`. With no editor, there was no way to wire the input or declare the output — the user ended up mis-declaring `manual.txt` as a *dataflow output slot*. Directly analogous to F-10 (ai_call). Also: an edge **into** an internal task didn't auto-populate `file_inputs` (auto-populate's `targetIsScript` excluded internal). | functional | **FIXED** — both editors now render for `internal`, and `onConnect` auto-populate treats internal as a file-input target (so an `ai_call → internal` edge fills the upstream output path). |
| F-18 | 2 | **Long file paths in the inspector show only the beginning, clipping the filename.** A `../../queue/<wf>/<task>/file.md` input/output field is too narrow, and a left-anchored text input hides the meaningful tail (the filename). | UX | **FIXED** — new `FilePathInput.tsx` (used for `file_inputs`/`file_outputs`) scrolls to the **end** of the value when unfocused so the filename is visible, edits normally when focused, and exposes the full path in a hover `title`. |
| F-17 | 2 | **A failed node shows only an "F" badge — no reason on the node face.** The runtime `lastErrorMessage` existed in the snapshot and the inspector Runtime panel read it, but it was never threaded into node data, so "why did it fail?" needed the inspector. Hit when `zip_responses` failed with `Missing 'command' field`. | UX | **FIXED** — `runtimeError` now threaded into node data (both populate sites) and rendered on the node face (red, truncated, full text in `title`) when `runtimeState === "failed"`, taking priority over static validation text. |
| F-16 | 2 | **Re-dropping an edge onto a now-open port appends a new port instead of reusing it.** Delete a dep edge (the edge goes but the auto-derived `file_input`/port stays) then re-draw onto that same port → the F-9 "occupied-by-a-different-file" rule appended a 4th port rather than reusing the freed one. Empirically hit re-wiring `ai_python_trivia_random` → `zip_responses` on `aiZipDemo-hand`. | UX/functional | **FIXED** — `onConnect` now detects a drop onto an explicit `dephandle-N` whose port has **no live edge** (freed) and **overwrites** that slot with the edge's file instead of appending; the edge retargets to it. Ports with a live edge are still never clobbered. |
| F-15 | 2 | **Downstream `file_inputs` capture the upstream `file_outputs` at edge-draw time and never re-sync.** Copy-paste an AI node (carries the original's `file_outputs`, e.g. `stl.output.md`), wire it into a shell task, *then* rename its `file_outputs` → the downstream input keeps the **stale** name. Empirically: `aiZipDemo-hand` zip input #2 showed `stl.output.md` from a wired-then-renamed `ai_python_trivia_random` (correct name `pythonTrivia.output.md`); node #3 was fine because its output was named before wiring. | UX/functional | **FIXED** — `updateSelectedTaskField` now propagates a `file_outputs` rename to downstream `file_inputs` (matched by bare name or `/<name>` suffix) on tasks wired by a `dep:` edge from the renamed node — mirrors the existing PROB-rename propagation. |
| F-14 | 2 | **Empty shell `args` silently auto-inject `inputs-first, outputs-last`, which fails for archive-first tools.** A shell task with `file_inputs`/`file_outputs` but no `args` gets `EnsureDefaultInputOutputArgs` (shellTaskExecutor.cpp) → `{{input[0]}} … {{output[0]}}`. For `zipTool.sh`/`tar` (archive is `$1`) this runs `zipTool.sh stl.output.md aiResponses.zip` → treats the `.md` as the archive → "Zip file structure invalid", exit 768. No editor-time signal; the `+ arg` affordance reads as "optional". Empirically hit on `aiZipDemo-hand` (run `aiZipDemo-hand_1781057724273`). | UX/functional | **FIXED** — `validation.ts` adds an info when a shell/python task has file I/O (`file_inputs`/`file_outputs`) but empty `params.args`, spelling out the inputs-first/outputs-last auto-injection and that archive-first tools need explicit args. |
| F-1 | 3, 4 | `output_schema` / `output_retries` had **no inspector UI** — confirmed: zero refs anywhere in the frontend, so structured-output tasks were not buildable by hand. | functional | **FIXED** — new `StructuredOutputEditor.tsx` renders for `ai_call`: a JSON-Schema textarea (raw-text local state so intermediate invalid JSON survives keystrokes; commits the parsed object only when it parses, inline parse-error otherwise; "insert example schema" helper) + an `output_retries` number field. Round-trip already worked (graphToJcwf spreads the whole task; both are top-level fields); validator now warns on a malformed loaded value. |
| F-2 | 3 | `internal` task `action` had **no dedicated field** — confirmed: the type existed (button + dropdown option) but `params.action` was unreachable in the UI. | functional | **FIXED** — `internal` inspector block edits `params.action` (text + `carMaintenance` datalist, the only registered C++ factory); `validation.ts` now **errors** when an internal task has no `action`. |
| F-3 | all | `depends_on` is edge-only (no text list) — confirm drawing an edge is discoverable. | UX | **FIXED** — inspector now shows a read-only **"depends_on (from edges)"** chip list derived from `dep:` edges into the task, with a hint ("drag from a task's right edge to this task's left edge to set run-order") when empty. Makes the edge→ordering model legible. |
| F-4 | 3, 4 | named `dataflow` (output→input) reachable only by dragging `out:`/`in:` ports, which only exist when a task declares `inputs`/`outputs` — and there was **no UI to declare those slots**, so dataflow was unreachable for a hand-built workflow. | functional | **FIXED** — collapsible **"Dataflow slots"** inspector section declares named `inputs`/`outputs` slots (name + `required`); declared slots render as ports on the node face, and the existing `onConnect`/graphToJcwf/jcwfToGraph `df:`-edge wiring serialises the drag into a `dataflow` entry. Validator warns on an empty slot name. |
| F-7 | 1 | **Hand-drawn dependency edges are silently dropped on save (functional bug).** New edges from `onConnect` get a ReactFlow auto-id (`reactflow__edge-…`, `WorkflowEditorView.tsx:1828`), but `graphToJcwf.ts:193` only serialises edges whose id starts with `dep:` — so `depends_on` is **never written** for a from-scratch edge (loaded workflows are fine: `jcwfToGraph.ts:293` assigns `dep:` ids). Orphaned auto-id edges also show as "stray connections" on canvas. **Empirically confirmed** — `make-example-hand.json` has zero `depends_on` despite a full set of drawn edges. Fix: assign a `dep:${source}->${target}:${depHandleIdx}` id in the `else` branch of `onConnect`. | **HIGH / functional** | **FIXED** (`onConnect` now stamps a `dep:` id) |
| F-8 | 1 | **file_inputs order = shell argument order, with no guard.** Empty `args` auto-injects `{{input[N]}}`/`{{output[N]}}` in list order (`shellTaskExecutor.cpp:289`), so a mis-ordered `file_inputs` silently feeds args in the wrong positions (here `link.sh` got `libmylib.a` as `$1` → exit 256). | UX | **FIXED** — inspector now shows an "order matters — maps to `{{input[0]}}, {{input[1]}}, …` in args" hint under `file_inputs` for shell/python |
| F-13 | UX pass | **Args file-ref dropdown emitted the wrong template syntax.** The shell-`args` picker inserted `${input[N]}` / `${output[N]}`, but the backend template engine only resolves `{{input[N]}}` / `{{output[N]}}` (nothing converts them) — so an arg chosen from the dropdown was passed to the script as a literal `${input[0]}`. | functional | **FIXED** — dropdown now emits `{{input[N]}}` / `{{output[N]}}` and detection matches |
| F-12 | 2 | **AI runtime artifacts pollute the `.jcwf` → "Broken Workflow".** An `ai_call` with no `working_directory` writes its runtime files (incl. `PROB_*.transcript.json`, a JSON **array**) into the workflow folder; the editor packs the whole folder into the `.jcwf`, and on reload the loader picked **any** non-`global.json` `.json` as the root canvas → it grabbed the transcript → `Canvas JSON root must be an object` → broken. | functional | **FIXED** — `workflowRegistry.cpp` prefers the canonical `<stem>.json` for the root canvas (stray runtime JSON can't break load); **and** new `ai_call` nodes auto-fill `working_directory` = `../../queue/<wf>/<taskId>` so artifacts go to the queue area by default (no nag warning — the node self-configures) |
| F-11 | 2 | **Colliding `ai_call` output paths, no warning.** Multiple AI tasks sharing a working directory **and** PROB filename (the `+` PROB default is `PROB_new_1.txt`; copy-paste carries it over) all register the same expected-output path (`PROB_new_1.output.txt`). Only one resolves; the rest hang in `waiting_external` ("No queries") or fail the file-activity watchdog — silent before. | functional/UX | **FIXED** — `validation.ts` now warns on each AI task that shares a `working_directory`+PROB-filename with another (naming the colliders) |
| F-10 | 2 | **`ai_call` has no `file_inputs` / `file_outputs` editor.** The inspector gated both sections to `shell`/`python` (`WorkflowEditorView.tsx:4268,4301`), so an AI task's output file (e.g. `stl.output.md`) couldn't be set in the UI — even though the backend honors `file_outputs` on `ai_call` (reply lands in the declared file as well as `<prob>.output.txt`) and AI tasks legitimately use `file_inputs` (e.g. reading `message.txt`). | functional | **FIXED** — both editors now render for `ai_call`; the AI→shell auto-populate also prefers the AI task's `file_outputs` over the PROB-derived name |
| F-9 | 1 | **Edge doesn't snap to the port holding its file.** Two cases: (a) drop before any port exists → edge stays on the now-hidden catch-all and dangles; (b) drop a *different* file onto an already-occupied port → auto-populate appends a new input but the edge stays on the occupied port (wrong file). | UX/functional | **FIXED** — `onConnect` always retargets the just-added edge to the `dephandle-N` that holds its own file (creating a new port when the dropped one is occupied by a different file; redundant same-file re-drops are dropped) |
| F-6 | 1,2 | **Edge ≠ file_input for shell→shell.** Drawing a dependency edge auto-populated `file_inputs` **only** for `ai_call → shell/python`. For `shell → shell` / `python → shell` the edge set ordering only and left the input filename blank. Also the `ai_call` branch *assumed* a `../queue/<wf>/NN_<id>` path for unset-wd AI tasks, but an unset-wd `ai_call` writes its `file_outputs` to the **workflow base** → downstream got a bogus queue path (`Missing required input file`). | UX/functional | **FIXED** — `onConnect` auto-fills from a shell/python source's `file_outputs` too; **and** both branches now compute the path from the source/target *actual* working dirs (empty wd ⇒ bare filename), preferring the source's declared `file_outputs` |
| F-5 | 1 | **`working_directory` validation asymmetry** — `undefined` is a Tier-D info but `""` is a blocking error, despite both running identically (the backend defaults empty→workflow folder; the task ran **OK** with `""`). Once the field is touched it can't return to `undefined` (clearing text yields `""`); the only offered escape (Tab-accept) injects a per-task subfolder that breaks flat-layout workflows. | cosmetic/UX | **FIXED** — `validation.ts` now treats `""` the same as unset (Tier-D info, not a blocking error), so the field can be cleared freely |

---

## 1 · make-example — shell DAG warm-up

**What it exercises:** adding task nodes, drawing dependency edges, shell `command` +
`args` with `{{input[n]}}`/`{{output[n]}}` templating, `file_inputs`/`file_outputs`.
No AI, no queue-binding — pure structure. This is the editor's skeleton.

**Target shape** — a diamond/fan compile graph (6 shell tasks):

```
compile_lib1 ─┐
compile_lib2 ─┴─> make_static_lib ─┐
compile_main ───────────────────────┤
compile_app  ───────────────────────┴─> make_executable
```

| Task | command | file_inputs | file_outputs | depends_on |
|------|---------|-------------|--------------|------------|
| compile_lib1 | `scripts/compile.sh` | `lib1.cpp` | `lib1.o` | — |
| compile_lib2 | `scripts/compile.sh` | `lib2.cpp` | `lib2.o` | — |
| compile_main | `scripts/compile.sh` | `main.cpp` | `main.o` | — |
| compile_app | `scripts/compile.sh` | `app.cpp` | `app.o` | — |
| make_static_lib | `scripts/archive.sh` | `lib1.o`, `lib2.o` | `libmylib.a` | compile_lib1, compile_lib2 |
| make_executable | `scripts/link.sh` | `main.o`, `app.o`, `libmylib.a` | `myapp` | compile_main, compile_app, make_static_lib |

`args` pattern: compile = `{{input[0]}} {{output[0]}}`; archive = `{{input[0]}} {{input[1]}} {{output[0]}}`; link = `{{input[0]}} {{input[1]}} {{input[2]}} {{output[0]}}`.

**Prerequisites:** copy the C++ sources into the new folder —
`cp workflows/make-example/{app,lib1,lib2,main}.cpp workflows/make-example/mylib.h workflows/make-example-hand/` (create the folder on first save, then copy and re-run).

**Build steps:**

1. New editor canvas (App → *Editor*, no workflow selected).
2. Sidebar *Workflow* card: set **label** = "Makefile-style compilation test"; leave
   base_directory empty; **defaults.timeout_ms** = 5000.
3. Click **+ Shell** four times → four nodes. Select each, set **Label**, **command**,
   **file_inputs** (one entry), **file_outputs** (one entry), and **args** per the table.
   For `args`, use the input/output reference dropdown if present, else type
   `{{input[0]}}` / `{{output[0]}}`.
4. Click **+ Shell** for `make_static_lib`; set its two `file_inputs`, one `file_output`,
   command `scripts/archive.sh`, args as above.
5. Drag dependency edges: from `compile_lib1` right handle → `make_static_lib` left
   handle; repeat for `compile_lib2`. Confirm the inspector now shows both in depends_on.
6. Click **+ Shell** for `make_executable`; three `file_inputs`, one output, link args.
   Draw edges from `compile_main`, `compile_app`, `make_static_lib` into it.
7. **Triggers** card: enable **manual_start**; add a **manual** trigger.

> **Leave `working_directory` untouched** on every task — make-example is flat (all tasks
> run in the workflow folder). Clicking the field leaves `""`, a blocking error (F-5); an
> unset field is just a harmless blue info. Don't Tab-accept the `…/NN_taskName` suggestion.
>
> **Edges ≠ file_inputs (F-6).** For shell→shell tasks, drawing an edge sets ordering
> (`depends_on`) only — it does **not** fill the input filename. Type every `file_inputs` /
> `file_outputs` path by hand in the inspector; use edges just to wire the dependency order.

**Save / run / verify:**

- *Save* → id `make-example-hand`. Copy the C++ sources in (prereq), then *Run*.
- Success: all six nodes go **OK**; `workflows/make-example-hand/myapp` exists and is a
  built executable; `stdout.txt` shows the compile/link chain. Matches shipped
  `make-example` output.

---

## 2 · aiZipDemo — first AI + queue-binding

**What it exercises:** `ai_call` tasks, the **queue-binding editor** (STNG/CNTX/TASK/PROB
with inline content), `single` mode, fan-in from three AI tasks into one shell zip,
and the workflow-level **defaults.ai** provider/model.

**Target shape:** three independent AI explainers → one shell zip.

```
ai_stl_random ──────────┐
ai_python_trivia_random ┼─> zip_responses
ai_vulkan_method_random ┘
```

The three AI tasks share identical queue-binding **STNG/CNTX/TASK**; only the **PROB**
(the actual prompt) differs. Each writes one `.md` output; the shell task zips all three.

| Task | type | output | PROB content (the prompt) |
|------|------|--------|---------------------------|
| ai_stl_random | ai_call | `stl.output.md` | "Pick ONE random C++ STL component … short C++20 example, Allman braces. Output in Markdown." |
| ai_python_trivia_random | ai_call | `pythonTrivia.output.md` | "Pick ONE random Python trivia fact … tiny code example. Output in Markdown." |
| ai_vulkan_method_random | ai_call | `vulkanRendering.output.md` | "Pick ONE random rendering method / shader algorithm … Vulkan mindset … Output in Markdown." |
| zip_responses | shell | `aiResponses.zip` | command `scripts/zipTool.sh`, args = `{{output[0]}} {{input[0]}} {{input[1]}} {{input[2]}}` |

Shared queue-binding for the three AI tasks:
- **STNG** (inline): "friendly, educational, computer science tone"
- **CNTX** (inline): "AI may pull C++, Python, and Vulkan/computer-graphics info from the net."
- **TASK** (inline): "Create markdown output."
- **PROB** (inline): the per-task prompt above.

**Prerequisites:** none — all AI inputs are inline. A working AI interface (the default
in *Workflow → defaults.ai*, e.g. a local ollama/qwen or a cloud provider) must be usable.

**Build steps:**

1. New canvas. *Workflow* card: **label** "AI Topic Trio + Zip"; **defaults.ai.provider**
   and **defaults.ai.model** = your usable interface (e.g. the local qwen, or
   `gpt-4.1-mini`).
2. Click **+ AI Call**. Set **Label**, **Mode** = single. Open the **queue-binding
   editor**: add one STNG, one CNTX, one TASK, one PROB entry, each toggled to **inline**,
   pasting the content above (STL prompt in PROB). Set **file_outputs** = `stl.output.md`.
3. Repeat for the Python-trivia and Vulkan tasks (same STNG/CNTX/TASK, their own PROB and
   output filename). *(Note any friction copying shared binding content — F-?)*
4. Click **+ Shell** for `zip_responses`: command `scripts/zipTool.sh`; one `file_output`
   `aiResponses.zip`; three `file_inputs` (the three `.md` outputs); args as in the table.
5. Draw dependency edges from each AI node → `zip_responses`. Confirm `file_inputs`
   auto-populate (the editor links ai_call outputs into downstream shell inputs).

**Save / run / verify:**

- *Save* → `aiZipDemoHand`. *Run*.
- Success: three AI nodes **OK** with non-empty `.md` outputs; `zip_responses` **OK**;
  `aiResponses.zip` contains the three markdown files. Matches shipped `aiZipDemo`.

---

## 3 · aiCarMaintenancePipeline — structured output + internal task

**What it exercises:** **structured AI output** (`output_schema` enum + `output_retries`),
the **`internal`** task type (C++ `action`), a 5-stage serial chain, file_inputs that
reach into other task folders, and **dataflow** (named output → input).

> **Inspector fields for #3 (F-1/F-2/F-4 — closed):**
> - **Structured output** lives in the collapsible **"Structured output (output_schema)"** section
>   on an `ai_call` — a JSON-Schema textarea + `output_retries`, with an **"Insert example schema"**
>   button (its built-in example *is* the `{category: enum[engine,tires,rephrase]}` schema this
>   workflow needs).
> - **Internal `action`** is the **"Internal task params"** block on an `internal` task (a missing
>   action is a blocking validation error).
> - **Named dataflow** uses the collapsible **"Dataflow slots (named output → input)"** section:
>   declare `outputs`/`inputs` slots, which then appear as round ports on the node face — drag an
>   output port to a downstream input port to wire the `dataflow` edge.

**Target shape** — strict serial pipeline:

```
classifyQuestion (ai_call, structured) ─> buildManual (internal) ─> answerWithManual (ai_call) ─> zipAnswer (shell) ─> printZipInfo (python)
```

| Task | type | key config |
|------|------|-----------|
| classifyQuestion | ai_call | `output_schema`: object `{category: enum[engine,tires,rephrase]}`, required, `additionalProperties:false`; `output_retries`: 3; output `classification.output.json`; PROB = `message.txt` (file ref) |
| buildManual | internal | `params.action` = `carMaintenance`; input = classification JSON; output `manual.txt` |
| answerWithManual | ai_call | CNTX = `manual.txt` (ref); PROB = `message.txt` (ref); output `answer.output.txt` |
| zipAnswer | shell | `scripts/zipTool.sh`, args `{{output[0]}} {{input[0]}}`; output `answer.zip`; declares output `archive_path` |
| printZipInfo | python | `module` `printFileInfo`, `function` `get_file_info`; input `filename` ← dataflow from zipAnswer.archive_path |

**Prerequisites:** copy the question file into the `-hand` folder (deref the shipped symlink) —
`cp -L workflows/aiCarMaintenancePipeline/message.txt workflows/aiCarMaintenancePipeline-hand/`.
The shipped `message.txt` is a symlink picking one of the `message_*.txt` variants; a plain copy
is fine. The staged content is `The engine light is on. Advise with first steps.` → classifies as
**`engine`**.

**`-hand` path convention:** the shipped answer-key points file refs at
`…/workflows/aiCarMaintenancePipeline/…`; yours must point at the **`-hand`** folder. A new AI node
auto-fills `working_directory` = `../../queue/aiCarMaintenancePipeline-hand/<taskId>`, from which
`../../../workflows/aiCarMaintenancePipeline-hand/message.txt` resolves to the staged file.

**Build steps:**

1. New canvas. *Workflow* card: label per shipped; defaults.ai = usable interface.
2. **+ AI Call** `classifyQuestion` — Mode **single**:
   - **Structured output** section → click **"Insert example schema"** (already the engine/tires/
     rephrase schema), set **`output_retries` = 3**.
   - **file_outputs:** `classification.output.json`
   - **Queue-binding** (STNG/TASK/CNTX **inline**; PROB as a **ref**):
     - **STNG:** `You are a strict classifier. Return ONLY a JSON object that matches the declared schema.` / `No prose, no markdown fences — raw JSON.`
     - **TASK:** `Classify the user's question into exactly one of: engine, tires, rephrase.` / `Emit a JSON object of the form {"category": "<one of engine|tires|rephrase>"}.`
     - **CNTX:** `Classification rules:` / `- engines, engine lights, oil, starting issues, overheating, … -> engine` / `- tires, tire wear, pressure, alignment, rotation, punctures, … -> tires` / `- Otherwise -> rephrase`
     - **PROB (ref):** `../../../workflows/aiCarMaintenancePipeline-hand/message.txt`
   - **file_inputs:** `../../../workflows/aiCarMaintenancePipeline-hand/message.txt`
3. **+ Internal** `buildManual` (leave `working_directory` **empty** → runs at the workflow base):
   - **Internal task params** → `action` = `carMaintenance`
   - **file_outputs:** `manual.txt`
   - **Edge** classify → build, then on `buildManual` use the `file_inputs` dropdown / `+ file_input (1 wired)` to add the classifier JSON: `../../queue/aiCarMaintenancePipeline-hand/ai_call/classification.output.json`
4. **+ AI Call** `answerWithManual` — Mode **single** (its auto-filled wd `../../queue/aiCarMaintenancePipeline-hand/ai_call_2` means refs reach the workflow base via `../../../workflows/…`):
   - **Queue-binding** (STNG/TASK **inline**; CNTX/PROB as **refs**):
     - **STNG:** `Be practical and concise. Use bullet points where helpful. Do not invent facts.`
     - **TASK:** `Use the provided context/manual (CNTX) to answer the user's question (PROB).` / `If CNTX asks for rephrasing, ask 2-3 focused follow-up questions.`
     - **CNTX (ref):** `../../../workflows/aiCarMaintenancePipeline-hand/manual.txt`  ← full path, **not** bare `manual.txt`
     - **PROB (ref):** `../../../workflows/aiCarMaintenancePipeline-hand/message.txt`
   - **file_inputs:** the same two paths — wire `buildManual → answerWithManual` and pick `manual.txt`'s path from the dropdown, then add `message.txt` by hand.
   - **file_outputs:** `answer.output.txt`
5. **+ Shell** `zipAnswer`: `scripts/zipTool.sh`, **args `{{output[0]}} {{input[0]}}`** (archive
   first — F-14), output `answer.zip`; **Dataflow slots → declare output `archive_path`**. Edge
   answer → zip.
6. **+ Python** `printZipInfo`: module `printFileInfo`, function `get_file_info`; **Dataflow slots →
   declare input `filename`**. Edge zip → print, then drag `zipAnswer`'s `archive_path` output port
   to `printZipInfo`'s `filename` input port to wire the dataflow.

**Save / run / verify:**

- *Save* → `aiCarMaintenancePipeline-hand`. *Run*.
- Success: `classification.output.json` is valid `{category:"engine"}`; `manual.txt` is the engine
  manual; `answer.output.txt` answers using the manual; `answer.zip` built; `printZipInfo` logs the
  zip's file info. Matches the shipped pipeline for the same `message.txt`.

---

## 4 · vehicleTroubleshootingGuide — parallel structured AI → Python → PDF

**What it exercises:** three **parallel** structured `ai_call` tasks (schema `{title,
mermaid}`), a **Python combiner** with three named `file_inputs` from sibling folders,
**named dataflow** (three outputs → three inputs), a final shell PDF conversion, and
`{{defaults.ai.*}}` template references in params.

**Target shape:**

```
aiCode244 ─┐
aiCode250 ─┼─> combineGuideMd (python) ─> convertGuidePdf (shell)
aiCode301 ─┘
```

| Task | type | key config |
|------|------|-----------|
| aiCode244/250/301 | ai_call | `output_schema` `{title:str(1-120), mermaid:str(≥1)}` required, `additionalProperties:false`; `output_retries`:3; output `codeNNN.output.json`; STNG/TASK/CNTX shared, PROB = the engine-code description |
| combineGuideMd | python | module `combineEngineTroubleshootingGuide`, function `buildEngineTroubleshootingGuide`; 3 inputs `code244/250/301JsonPath`; output `engineTroubleshootingGuide.md` |
| convertGuidePdf | shell | `scripts/mermaidMdToPdf.sh`, args `{{input[0]}} {{output[0]}}`; output `Vehicle Troubleshooting Guide.pdf` |

**Prerequisites:** none — every field is authored from the literal content below; the Python
module (`scripts/combineEngineTroubleshootingGuide.py`) + PDF script (`scripts/mermaidMdToPdf.sh`)
ship in the repo. PDF step needs `mmdc` + `pandoc` on PATH — if absent, the first two stages still
verify and the PDF stage failure is itself an environment finding, not an editor one.

### Build recipe (from scratch — type every field; never open a shipped `.jcwf`)

**Step 0 — workflow settings.** New canvas → workflow panel: *label* `vehicleTroubleshootingGuide`;
set **`defaults.ai.provider`** + **`defaults.ai.model`** to your provider/model (the AI tasks
reference them as `{{defaults.ai.provider}}` / `{{defaults.ai.model}}`, so they're set in one place).

**Step 1 — the three AI tasks** (`aiCode244`, `aiCode250`, `aiCode301`). They share all scaffolding;
only **PROB** and the output filename differ. For each node — **+ AI Call**, then:

- *id/label*: `aiCodeNNN`.
- *working_directory*: `../../queue/vehicleTroubleshootingGuide/0N_aiCodeNNN` (01/02/03 — isolates
  each task's queue area so PROB outputs don't collide; auto-filled on node creation, keep it).
- *file_outputs*: `codeNNN.output.json`.
- *params*: `provider` = `{{defaults.ai.provider}}`, `model` = `{{defaults.ai.model}}`, mode `one_shot`.
- *Structured output* (the F-1 inspector): paste this schema, set `output_retries` = `3`:
  ```json
  {
    "type": "object",
    "properties": {
      "title":   { "type": "string", "minLength": 1, "maxLength": 120 },
      "mermaid": { "type": "string", "minLength": 1 }
    },
    "required": ["title", "mermaid"],
    "additionalProperties": false
  }
  ```
- *queue_binding* — four files. **STNG / CNTX are identical for all three tasks; TASK shares one body
  (only the `e.g.` title hint changes); PROB is unique per code:**

  **STNG** `STNG_structured.txt` (all 3 identical):
  ````
  Return ONLY a JSON object that matches the declared schema.
  No prose, no markdown fences — raw JSON.
  The 'mermaid' field MUST contain the raw Mermaid flowchart source, i.e. the text that normally lives INSIDE a ```mermaid block — do NOT include the opening or closing ``` fences in the string.
  ````
  **CNTX** `CNTX_needMermaidCfg.txt` (all 3 identical):
  ```
  For a vehicle troubleshooting guide, a Mermaid flowchart shows the troubleshooting steps for one engine code.
  ```
  **TASK** `TASK_generateCfg.txt` (body identical; swap the `e.g.` title per code — 244 → `Code 244 — Engine Temperature`, 250 → `Code 250 — Tire Alignment`, 301 → `Code 301 — Headlights Circuit Breaker`):
  ```
  Generate a control flow graph for the engine code described in PROB.

  Emit JSON with these fields:
  - title   : a short section heading for this code (e.g. 'Code 244 — Engine Temperature')
  - mermaid : the raw Mermaid flowchart source (no fence)

  MERMAID SYNTAX RULES — follow strictly:
  - Use flowchart TD
  - Node labels MUST use square brackets: A["My Label"] — always quote the label text
  - Decision nodes use curly braces: B{"Yes or No?"}
  - NEVER use parentheses () in node labels — Mermaid interprets them as shape delimiters
  - NEVER use special characters ( ) [ ] { } in label text — if needed, spell them out
  - Keep labels short: max 40 characters per label
  - Every edge token MUST start with '-->' (three characters: dash, dash, greater-than). Never '--|' or '-- '.
  - Unlabeled edge: A --> B
  - Labeled edge: A -->|"label text"| B  (note the '>' between the two dashes and the pipe)
  - WRONG: A --|"label"| B   (missing '>', Mermaid rejects this)
  ```
  **PROB** `PROB_codeNNN.txt` (unique — the scenario you're solving):
  - `PROB_code244.txt`:
    ```
    engine code 244 -> prompt if code 244 'engine temperature' is active check if there is code 245 'low cooling liquid' present. if so floow the instructions there. if not check if the radiator is clogged. if so clean it and were done. if not, check if the cooling pump runs. if not check if the circuit breaker is in. if not check and fix wiring. put back in circuit breaker. check if cooling pump works now. if yes we're done. if not replace cooling pump.
    ```
  - `PROB_code250.txt`:
    ```
    e.g. engine code 250 'tire alignment' means uneven tire wire. if code 250 is present then adjust the alignment of the wheels as per proceedure 5 from the tire manual.
    ```
  - `PROB_code301.txt`:
    ```
    code 301 is present 'headlights light circut breaker tripped'. If code 301 is present check the wiring of the headlights. if there is a short or faulty wiring then fix the wiring. then put circuit breaker back in and switch on the headlights. if the breaker does not trip again, we are done. if it trips again then disconnect left light and put circuit breaker back in. switch on lights. if circuit breakers stays in, replace left light. if breaker trips replace right light.
    ```

**Step 2 — Python combiner** (`combineGuideMd`). **+ Python**: module `combineEngineTroubleshootingGuide`,
function `buildEngineTroubleshootingGuide`. This node reads the three AI tasks' JSON outputs and writes
one combined Markdown file. Declare the slots so the dataflow edges (Step 4) have somewhere to land —
**each slot name must equal the function's parameter name**:

| Slot | dir | function param | wired in Step 4 from / to |
|------|-----|----------------|---------------------------|
| `code244JsonPath` | input | `code244JsonPath` | ← `aiCode244` output slot (its `code244.output.json`) |
| `code250JsonPath` | input | `code250JsonPath` | ← `aiCode250` output slot (its `code250.output.json`) |
| `code301JsonPath` | input | `code301JsonPath` | ← `aiCode301` output slot (its `code301.output.json`) |
| `outputMdPath` | output | `outputMdPath` | → `convertGuidePdf` input |

Then set *file_outputs* `engineTroubleshootingGuide.md` — the `outputMdPath` output slot maps to it, so
the function receives the resolved path to that file as its `outputMdPath` argument and writes the guide there.

**Step 3 — Shell PDF** (`convertGuidePdf`). **+ Shell**: command `scripts/mermaidMdToPdf.sh`,
args `{{input[0]}} {{output[0]}}`; *file_outputs* `Vehicle Troubleshooting Guide.pdf`.

**Step 4 — wire the dataflow edges** (the real test of F-28…F-31):
- 3× AI → python: drag each `aiCodeNNN` **output slot** → the matching `codeNNNJsonPath` **input slot**.
- 1× python → shell: drag `combineGuideMd`'s `outputMdPath` output → `convertGuidePdf`'s input.

**Save / run / verify:**

- *Save* → `vehicleTroubleshootingGuideHand`. *Run*.
- Success: three valid `{title, mermaid}` JSONs; `engineTroubleshootingGuide.md` with three
  fenced ```mermaid blocks; PDF produced (env permitting). Matches shipped guide.
- **Local-LLM note:** the schema validates the JSON *shape*, not that the Mermaid renders (F-34).
  Small models (qwen7b) need `output_retries ≈ 5` to satisfy the schema and still emit invalid
  Mermaid on some rolls (parens in a label, `-->` inside a label) — which fails at the PDF stage,
  not at validation. Pick a stronger interface via the `ai.provider` dropdown, or Clean+re-run.

---

## 5 · portfolioDividendAnalysis — CSV fan-out (the boss level)

**What it exercises:** the **Filter builder** (CSV source), **per-item fan-out** mode
bound to a filter, **`{{pos.*}}` row templates** in the PROB path and content, a **glob
CNTX input** (`PROB_*.output.txt`) for the fan-in summary, and a 2-stage AI DAG. Fully
**self-contained** — it reads only the bundled `port62pos.csv` (`Symbol,Name,Percentage`);
the AI supplies the dividend figures from its own knowledge (so numbers vary by model and may
be stale — the *structure*, not the figures, is what #5 validates).

**Target shape:**

```
lookupDividend (ai_call, per_item over CSV) ─> portfolioSummary (ai_call, single)
```

| Task | type | key config |
|------|------|-----------|
| lookupDividend | ai_call, **per_item** | filter `positions`; wd `../../queue/<wf>/01_lookupDividend`; PROB path `PROB_{{pos.Symbol}}_{{pos.row_number_padded}}.txt`, content from `{{pos.Symbol}}` / `{{pos.Name}}` / `{{pos.Percentage}}` |
| portfolioSummary | ai_call, single | wd `../../queue/<wf>/02_portfolioSummary`; CNTX = glob ref `../01_lookupDividend/PROB_*.output.txt`; depends_on lookupDividend |

**Filter `positions`:** source kind **csv**, path `port62pos.csv`, delimiter `,`,
header_row on, binding prefix `pos`, max_items 100.

**Prerequisites:** after the first *Save* (which creates `workflows/portfolioDividendAnalysis-hand/`),
copy the seed CSV so the filter has its source —
`cp workflows/portfolioDividendAnalysis/port62pos.csv workflows/portfolioDividendAnalysis-hand/`.
No external data source — the CSV ships with only allocations; the model looks up the figures.

**Working-directory note:** the AI nodes' wd uses **numbered prefixes** (`01_`/`02_`) so the
glob CNTX ref resolves; the editor auto-fills `../../queue/<wf>/<taskId>` on node creation, so
**rename each to `01_lookupDividend` / `02_portfolioSummary`** and substitute the real workflow id
(`portfolioDividendAnalysis-hand`) for `<wf>`.

**Build steps:**

1. New canvas. *Workflow*: label per shipped; manual trigger; `defaults.ai.provider`/`model` set
   (the new dropdowns).
2. **+ Filter**: open the Filter builder → **Filter ID** `positions`, **kind** csv, **path**
   `port62pos.csv`, **delimiter** `,`, **header_row** on, **binding** `pos`, **max_items** 100.
3. **+ AI Call** `lookupDividend`: **Mode** = `per_item`, **Filter ID** = `positions`; wd
   `../../queue/<wf>/01_lookupDividend`. Queue-binding per the content below; PROB path
   `PROB_{{pos.Symbol}}_{{pos.row_number_padded}}.txt`.
4. **+ AI Call** `portfolioSummary`: **Mode** = single; wd `../../queue/<wf>/02_portfolioSummary`;
   CNTX as a **glob ref** `../01_lookupDividend/PROB_*.output.txt`; TASK/PROB inline. Edge
   lookupDividend → portfolioSummary.

### Queue-binding content (type verbatim — from scratch, never open a shipped `.jcwf`)

Every file below is **inline** (`{path, content}`) — the entry's inline/ref toggle stays on *inline* —
**except** `portfolioSummary`'s CNTX, which is a **ref** (a glob path to existing files, no content).

**`lookupDividend`** (per_item over `positions`):

- **STNG** `STNG_succinct.txt`:
  ```
  Be succinct and precise. Report numbers clearly.
  Use a consistent structured format.
  Keep each position analysis under 150 words.
  ```
- **CNTX** `CNTX_portfolio.txt`:
  ```
  This is a 60-position investment portfolio.
  You are analyzing one position at a time.
  The allocation percentage represents how much of the total
  portfolio value is invested in this position.
  If the security is a bond ETF or fixed-income fund, report
  the SEC yield or distribution yield as the dividend yield.
  ```
- **TASK** `TASK_dividendLookup.txt`:
  ```
  For the stock or ETF position provided (PROB):

  1. Look up the current annual dividend yield (%)
  2. Look up the annual dividend per share ($)
  3. Given the portfolio allocation percentage, compute the
     weighted dividend contribution to total portfolio yield:
       weighted contribution = yield * allocation / 100
  4. State whether the dividend has been growing, stable,
     or declining over the past 5 years

  Format your response exactly as:
    Symbol: <TICKER>
    Name: <full name>
    Allocation: <X.XX%>
    Dividend Yield: <X.XX%>
    Annual Dividend/Share: $<X.XX>
    Weighted Contribution: <X.XXXX%>
    Dividend Trend (5yr): Growing | Stable | Declining
    Notes: <one-line note>
  ```
- **PROB** — **inline** (`{path, content}`, NOT a ref). The **path** templates the per-row
  *filename* so each of the 60 rows writes its own file; the **content** templates the per-row
  *body*. Both carry `{{pos.*}}`. Path `PROB_{{pos.Symbol}}_{{pos.row_number_padded}}.txt`, content:
  ```
  Symbol: {{pos.Symbol}}
  Name: {{pos.Name}}
  Portfolio Allocation: {{pos.Percentage}}
  ```
  > **Critical:** the path MUST be templated, not the default `PROB_new_1.txt`. A static per_item
  > PROB path makes all 60 rows write the same file → collision → file-activity-watchdog failure (F-40).

**`portfolioSummary`** (single):

- **STNG** `STNG_succinct.txt`:
  ```
  Be succinct and professional. Present a clear executive summary.
  Use tables where appropriate. Round percentages to two decimals.
  ```
- **CNTX** — a **glob ref** (not inline): `../01_lookupDividend/PROB_*.output.txt`
- **TASK** `TASK_portfolioSummary.txt`:
  ```
  You are given dividend reports for all 60 positions in a portfolio (CNTX files).

  Produce an account summary with:

  1. **Estimated Total Portfolio Dividend Yield** — sum of all weighted contributions
  2. **Top 10 Dividend Contributors** — sorted by weighted contribution (table)
  3. **Bottom 10 Contributors** — positions with lowest or zero yield (table)
  4. **Dividend Trend Overview** — how many positions are Growing / Stable / Declining
  5. **Asset Class Breakdown** — group positions into Equities, REITs, Bond ETFs,
     and other categories; show subtotal yield per group
  6. **Key Observations** — 3-5 bullet points about the portfolio's income profile,
     concentration risk, and suggestions for improvement
  ```
- **PROB** `PROB_summarize.txt`:
  ```
  Analyze all 60 dividend reports provided as context files and produce a comprehensive portfolio dividend summary as described in the TASK file.
  ```

**Save / run / verify:**

- *Save* → `portfolioDividendAnalysisHand`. *Run*.
- Success: the fan-out spawns **one AI call per CSV row** (watch the per-item badges); each writes a
  per-item report `queue/<wf>/01_lookupDividend/PROB_<SYM>_<NN>.output.txt` (60 distinct files);
  `portfolioSummary` globs all of them (`../01_lookupDividend/PROB_*.output.txt`) and writes the
  **final output**:
  ```
  queue/portfolioDividendAnalysis-hand/02_portfolioSummary/PROB_summarize.output.txt
  ```
  (= `<portfolioSummary wd>/<PROB-basename>.output.txt`). There is **no** packaged report file in this
  2-task version — the executive summary is that `.output.txt`. Matches the shipped original's shape.
- **All-or-nothing fan-out:** a per_item task fails if **any** single row fails (e.g. one item trips the
  file-activity watchdog), which **skips** `portfolioSummary` — so `02_portfolioSummary/` won't exist.
  Clean + re-run until all 60 rows land (or raise the row reliability). Leftover `PROB_new_1.*` in
  `01_lookupDividend/` is a stale artifact from a pre-template run — Clean removes it.
- **Note:** the dividend figures come from the model's own knowledge (self-contained example), so
  exact numbers vary by model and may be stale — the **per-item fan-out + glob fan-in** structure is
  what #5 validates, not the figures.

---

## Wrap-up

**Pass 1 complete** — all five rows ☑; the editor builds the full curated set by hand. Every
finding F-1…F-42 is **FIXED**; the one non-fix, **F-34**, is **out of scope by design** (render/
semantic validation is a workflow the designer composes via `expose_error_signal` + a branch node,
not an engine feature). So the pass has **zero deferred findings**. The text-input class of bug
(F-42) is guarded against recurrence by `isTaskNode` in `types.ts` + a memory note. Remaining next
steps, not part of this pass: the **Editor UX review** below, and the AI-assistant dogfooding pass.

---

## Editor UX review — proposed before finishing #4/#5 (JC's call, 2026-06-09)

**Why:** 30+ findings across **three simple** workflows (#1–#3) is a signal, not bad luck.
The point fixes (F-1…F-31) made the editor *usable*, but the friction keeps coming from one
root cause: **the editor exposes filesystem mechanics that should be the model's job.** Before
grinding through #4/#5, step back and redesign around the data model — most remaining findings
collapse into three themes:

1. **Edges should carry data, not just order.** Today an edge means `depends_on` (sequencing);
   the *file* it implies is a separate, hand-typed/auto-populated `file_inputs` path that drifts
   (F-15/F-16/F-19/F-20/F-21/F-22 were all patches around this). Make an edge from an output port
   to an input port *be* the data hand-off — the downstream input is the upstream output, by
   construction, not a copied string.
2. **Artifact files as first-class nodes.** External inputs (e.g. `message.txt`) and produced
   files are referenced today only as path strings buried in queue-binding/file_inputs. Put them
   on the canvas as nodes you wire into a task — data sources/sinks become visible and
   typo-proof (the `.txt.txt` / `message.txt.output.txt` / truncated-`message.txt` incidents all
   stem from hand-typed paths).
3. **Hide the `../../../` traversals.** The queue↔workflows relative paths (`../../queue/<wf>/…`,
   `../../../workflows/<wf>/…`) should never reach the user. The editor already computes them
   correctly (`deriveUpstreamOutputPaths` resolves against the workflow base) — extend that so the
   UI shows friendly names/ports and synthesises the paths on save. This also removes the
   inline-vs-ref + queue-wd-convention papercuts (F-5/F-12/F-25/F-26).

**Suggested approach for the review session:** synthesise the Findings log into these themes +
any others, sketch the target node/edge/artifact model, then decide whether to refactor before
or after #4/#5. The dogfooding gave the evidence; this turns it into a design.
