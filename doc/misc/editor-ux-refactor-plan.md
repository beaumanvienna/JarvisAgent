# Workflow Editor — UX Review & Refactor Plan

Dogfooding pass 1 rebuilt the five curated example workflows **by hand in the
editor** and logged 42 findings (F-1…F-42, see `editor-dogfooding-plan.md`).
The point fixes made the editor *usable*; this plan steps back and asks the
harder question: **is it usable by someone who does not program?** Today it is
not — too many fields expect the user to hand-author identifiers, relative
filesystem paths, JSON, and `{{template}}` expressions. This document reviews
every friction point against a single goal and lays out a phased refactor.

> **North-star goal: a "no-coding" editor.** A non-programmer should be able to
> build any of the five example workflows by placing nodes, drawing edges,
> picking from menus, and dropping in files — never by typing an internal id, a
> `../../queue/...` path, a JSON schema, or a `{{pos.*}}` expression.

The model already exists: the **AI provider/model dropdowns** (F-35). They used
to be free-text where the value had to exactly match an internal interface name
(`"openai"` didn't even match) — silently mis-resolving. Replacing them with
`<select>`s sourced from the configured interfaces made them foolproof. **Every
recommendation below is an instance of that same move.**

---

## Design principles (the review lens)

Each element is judged against these. They double as the acceptance criteria.

1. **No free-text where the valid set is known.** Pick from what exists; don't
   type an identifier that must match something elsewhere.
2. **An edge *is* the data hand-off.** Wiring output→input should carry the
   data; the user should never re-type on the downstream node what the edge
   already implies.
3. **Files are things on the canvas, not path strings.** Inputs and produced
   artifacts are nodes you wire, drag in, or pick from a dialog.
4. **Hide filesystem mechanics.** The `queue/` ↔ `workflows/` split, the
   `../../` traversals, and the `.output.txt` naming convention should never
   reach the user; the editor computes them on save.
5. **Progressive disclosure.** Fields used in every workflow sit at the top;
   rarely/never-used fields collapse or move down — never a wall of inputs.
6. **Suggest, don't require.** Default names, inferred outputs, prefilled
   values the user can accept with one keystroke.
7. **Templating without code.** Fan-out and variable substitution get a guided
   UI; the user picks columns, not types `{{binding.field}}`.

---

## Part 1 — Review (element-by-element)

Each entry: **Current** (what the user must do today) → **Problem** →
**Proposal** → **Evidence** (findings) → rough **effort**.

### A. AI provider / model  ✅ already fixed — the template
- **Current:** two `<select>`s sourced from `/api/settings/ai-interfaces`.
- **Why it's the model:** the value is now always valid and self-documenting;
  the user picks, never types an identifier that must match an internal name.
- **Evidence:** F-35. Use this as the pattern for B, and for every "type an id
  that must match X" field below.

### B. CSV filter id, typed twice
- **Current:** a filter node is given an id (e.g. `positions`); the per-item
  task then has a **"Filter ID" text field** where the user must type
  `positions` again. Get it wrong → no fan-out, or a deadlock if a stray edge
  serializes a `filter:` dependency (F-39).
- **Problem:** the id is an internal handle the user must remember and re-type.
  Worse, when the task is **connected to the filter node by an edge**, the
  binding is already unambiguous — the field is pure redundant work, and a
  source of error.
- **Proposal:** drive the per-item binding **from the fan-out edge**. Drawing
  filter→task sets `task.filter` automatically (to the filter's id, which the
  user never sees). If a task has exactly one incoming filter edge, the "Filter
  ID" field disappears; show instead a read-only chip *"fans out over:
  positions (port62pos.csv)"*. Keep a manual picker (a `<select>` of existing
  filters) only as a fallback for the unedged case — never a text field.
- **Evidence:** F-38 (filter rename dropped edits), F-39 (filter→task edge
  deadlock). Both are symptoms of the id being a hand-managed string.
- **Effort:** S–M (mostly `onConnect` + inspector; the fan-out edge already
  exists).

### C. Dependency edge vs. dataflow edge — two mechanisms, one concept
- **Current:** there are **two kinds of data wiring**:
  1. a **dependency edge** (`dep:`) → sets `depends_on` *and* auto-populates the
     downstream `file_inputs` path from the upstream `file_outputs`;
  2. a **dataflow edge** (`out:<name>` → `in:<name>`) → named-slot passing,
     declared via the "Dataflow slots" section.
  They look almost identical on the canvas and convey near-identical intent
  ("this feeds that"), but the user has to learn both, declare slots for one,
  and understand which to use when.
- **Problem:** this is the single biggest conceptual tax in the editor. Most of
  F-15/F-16/F-19/F-20/F-21/F-22/F-28…F-31 are patches *around* the seam between
  these two systems (paths drifting, ports not registering, edges dangling).
  Two ways to say "this feeds that" is one too many.
- **Proposal:** collapse to **one data-carrying edge**. An edge from a
  producer's output to a consumer's input *is* the hand-off; the editor derives
  the concrete `file_inputs` path (or named binding) on save and keeps it in
  sync — the user never sees or edits the path. Keep `depends_on`-only edges
  (pure ordering, no data) visually distinct (e.g. dashed/grey) and rare.
  Retire the separate "declare a slot, then draw a slot edge" flow; a port
  appears on a node because the node produces/consumes a file, not because the
  user declared an abstract slot.
- **Evidence:** the whole F-15…F-31 cluster; JC's dogfooding note that the two
  edge types "convey very similar info but create double work."
- **Effort:** L (this is the structural bet — see Part 2).

### D. Input files are hand-typed paths, not things
- **Current:** an external input (e.g. `port62pos.csv`, `message.txt`) exists
  only as a **path string** buried in `file_inputs` or a queue-binding entry.
  The user types the name and must know where it lives relative to the task.
- **Problem:** invisible, typo-prone (`.txt.txt`, truncated `message.txt`,
  `message.txt.output.txt` all happened — F-24/F-25), and there is no
  affordance to *bring a file in*.
- **Proposal:** **artifact nodes** + **drag-and-drop / file dialog**.
  - A file source becomes a first-class canvas node (a small "📄 port62pos.csv"
    node) that you wire into a task like any other producer.
  - **Drag a file from the OS** onto the canvas → uploads it into the workflow
    folder and creates the node (backend upload endpoint + dnd handler).
  - A **"+ file" button** opens a file dialog / a picker of files already in the
    workflow folder.
  - The task's input port binds to the artifact node by the edge; the path is
    synthesized on save. The user sees `port62pos.csv`, never
    `../../workflows/<id>/port62pos.csv`.
- **Evidence:** F-18 (long path clipping), F-24/F-25 (path-typo data loss),
  JC's "add a node for input files; drag-and-drop and/or file dialog."
- **Effort:** M–L (new node type + upload endpoint + dnd; pairs with E and C).

### E. Queue ↔ workflows relative paths leak everywhere
- **Current:** the user sees and sometimes edits `working_directory`
  (`../../queue/<wf>/NN_<task>`), `file_inputs` like
  `../../../workflows/<wf>/manual.txt`, queue-binding refs with `../`, and must
  understand the `queue/` (runtime scratch) vs `workflows/` (source) split.
- **Problem:** this is filesystem/runtime-architecture knowledge that a
  no-code user should never need. The editor already *computes these paths
  correctly* (`deriveUpstreamOutputPaths` resolves against the workflow base).
- **Proposal:** **never show a raw traversal.** `working_directory` becomes an
  advanced/auto field (AI nodes already self-assign it — F-12); the inspector
  shows a friendly *"runs in: <task> queue folder"* line, not the path. Wired
  inputs render as the **upstream node/file name**, not the relative path. The
  path machinery moves entirely to save-time synthesis. Inline-vs-ref
  queue-binding (F-25/F-26) disappears as a user concept once files are nodes.
- **Evidence:** F-5/F-12/F-25/F-26 and the entire "path drift" cluster.
- **Effort:** M (mostly display + making `working_directory` advanced; depends
  on D for the friendly names).

### F. JSON-Schema input for structured output
- **Current:** a field builder now exists (F-32 — name/type/required rows +
  enum + "allow extra fields") with the raw JSON-Schema textarea as an advanced
  fallback. Big improvement, but the builder is still schema-shaped thinking,
  and complex schemas still mean writing JSON.
- **Problem:** "write a JSON Schema" is a coding task. Even the builder asks the
  user to know what `additionalProperties` means (we relabeled it, but still).
- **Proposal (incremental on F-32):**
  - **Infer a schema from an example.** Let the user paste/type an *example
    JSON object* ("what should one answer look like?") and generate the schema
    from it (types from values, all keys required, `additionalProperties:false`
    by default). This is how a non-programmer thinks: by example, not by schema.
  - **Field templates / presets** for common shapes (classification with a
    fixed set of labels → enum builder; a list of items; a {label, value} pair).
  - Keep the builder and raw JSON as the two deeper tiers (progressive
    disclosure: example → builder → raw).
- **Evidence:** F-32 (builder), F-1 (no UI at all originally); JC: "in the real
  world it would require the user to write JSON."
- **Effort:** M (the example→schema inference is the valuable, self-contained
  piece).

### G. Output filenames are hand-typed
- **Current:** the user types `file_outputs` names from scratch.
- **Problem:** misses an obvious assist and invites mismatches with what
  downstream expects.
- **Proposal:** **suggest outputs from inputs/context.** If a task has an input
  `analysis.txt`, prefill `+ file_output` with `analysis.output.txt` (the
  convention the runtime already uses for AI replies). Offer the suggestion as
  ghost text / a one-click chip; the user accepts or edits. Same idea for
  shell/python tasks deriving an output name from the script or the primary
  input.
- **Evidence:** JC: "when we have an input file analysis.txt then we can suggest
  to name output analysis.output.txt."
- **Effort:** S.

### H. Inspector field order & progressive disclosure
- **Current order (task):** label → type → mode → Filter ID → AI Interface →
  Structured output → (cloud/internal params) → working_directory → file_inputs
  → file_outputs → depends_on (read-only) → **Dataflow slots** → **doc** →
  params (shell/python/**JSON**) → timeout_ms → queue_binding → (a second
  inputs/outputs slot editor near the bottom).
- **Problem (JC's, confirmed in code):** **`doc` and `params (JSON)` were never
  used** in the whole dogfooding pass, yet sit mid-list. The **`inputs`/
  `outputs` dataflow slot editors at the very bottom were never used** and are
  unexplained. Meanwhile `file_inputs`/`file_outputs` — used constantly — and
  the typed `params (shell/python)` are the real essentials. There also appear
  to be **two** inputs/outputs slot editors (the "Dataflow slots" collapsible
  *and* a bottom pair) — consolidate.
- **Proposal:**
  - **Essentials up:** label, type, mode/fan-out, AI interface + structured
    output (ai_call), typed params (shell command/args, python module/function),
    file_inputs/file_outputs, queue_binding (ai_call).
  - **Advanced, collapsed, down:** `doc`, raw `params (JSON)` fallback,
    `timeout_ms`, `working_directory` (auto by default — area E), the dataflow
    slots (relabeled — area I), and `depends_on` (read-only, keep as a small
    chip row).
  - Consolidate the duplicated inputs/outputs editors into one place.
- **Evidence:** JC's ordering note; F-2/F-27 (typed params beat raw JSON).
- **Effort:** S (pure inspector reorder + collapse) — **highest value-to-cost;
  do first.**

### I. The two "inputs/outputs" (dataflow slot) fields nobody understood
- **Current:** named dataflow slots (`inputs`/`outputs`) — distinct from
  `file_inputs`/`file_outputs` — used only for the python-combiner dataflow in
  workflow #4, and even then confusing (F-28…F-31, F-19's mis-declare).
- **Problem:** the names collide conceptually with `file_inputs`/`file_outputs`;
  users can't tell which is which or why both exist.
- **Proposal:** fold into area C. Once an edge carries data and ports appear
  because a node produces/consumes a file, the abstract "declare a slot" concept
  largely disappears. For the residual case (a python function parameter that
  isn't a file), **relabel and explain in place** — e.g. "Named value inputs
  (advanced — for passing a value to a script parameter, not a file)" — and
  collapse it by default.
- **Evidence:** F-19/F-28/F-29/F-30/F-31.
- **Effort:** S to relabel/collapse now; subsumed by C later.

### J. Fan-out PROB templating — the hardest "no-code" gap
- **Current:** to fan out over a CSV the user hand-authors an **inline
  queue-binding entry** whose **path** templates the per-row filename and whose
  **content** templates the per-row body, both with `{{pos.*}}` expressions —
  e.g. path `PROB_{{pos.Symbol}}_{{pos.row_number_padded}}.txt`, content
  `Symbol: {{pos.Symbol}}\nName: {{pos.Name}}\n...`. The user must also know
  that a *static* path collides all rows onto one file (F-40), and that
  inline-with-a-ref-path silently truncates (F-25).
- **Problem:** JC's words — "this would require a programming manual"; it is
  flatly incompatible with a no-code goal. The user must understand templating,
  the per-row filename-uniqueness requirement, and the inline/ref distinction.
- **Proposal:** a **guided fan-out builder** for a per-item AI task wired to a
  filter:
  - The user picks columns from the source (checkboxes over the CSV header:
    Symbol, Name, Percentage) to include in each row's prompt — the editor emits
    the `{{pos.*}}` content for them.
  - A **per-row filename** is generated automatically and guaranteed unique
    (e.g. `PROB_<Symbol>_<row>.txt`), with a friendly "one file per row"
    explanation — no template typing, and F-40's collision becomes impossible.
  - Show a **live preview** of row 1's materialized prompt + filename so the
    user sees what each call receives.
  - The inline/ref distinction (F-25/F-26) is removed from the user's view; the
    builder always produces the correct inline form.
- **Evidence:** F-11/F-25/F-26/F-40; JC's fan-out comprehension note.
- **Effort:** M–L (the highest-leverage no-code feature; depends on B for the
  filter binding).

---

## Part 2 — The structural bet: a data-carrying node/edge/artifact model

Areas C, D, E, I are facets of one redesign. The target mental model:

- **Nodes** are *steps* (AI call, script, internal) or *files* (artifact nodes,
  area D).
- **An edge from a producer port to a consumer port carries the data.** The
  consumer's input *is* the producer's output, by construction — not a copied
  path string the user maintains. The editor synthesizes the concrete
  `file_inputs`/queue paths at save time and keeps them in sync (the machinery
  already exists in `deriveUpstreamOutputPaths`; this makes it the *only* path
  authority).
- **`depends_on`-only edges** (ordering without data) remain, visually distinct
  and uncommon.
- **Filesystem paths, the queue/workflows split, and `.output.txt` conventions
  never appear in the UI** — they are an implementation detail of save/load.

This collapses the dual edge/slot system (C), makes inputs visible things (D),
hides traversals (E), and dissolves the mystery slots (I). The **JCWF on-disk
format does not change** — this is an editor-side authoring/translation layer
over the existing format, so shipped workflows and the runtime are unaffected.

Open design questions for this part (decide before building):
- Do artifact nodes persist in the JCWF (a new canvas node kind that serializes
  to nothing runtime-relevant) or are they purely an editor overlay rebuilt
  from `file_inputs` on load? (Leaning: editor overlay, to keep the format
  untouched.)
- How to represent a `depends_on`-only edge distinctly without a third concept.
- Migration/round-trip: loading an existing JCWF must reconstruct the friendly
  view from raw paths losslessly.

---

## Part 3 — Phased plan

Ordered by value-to-cost. Each phase is independently shippable and improves the
editor on its own. **Code-level detail** — the abstractions (U1–U7), per-phase
file/function map, backend endpoints, and round-trip/test strategy — is in the
companion **`editor-ux-refactor-dev-plan.md`**.

### Phase 0 — Inspector hygiene & assists (quick wins, low risk)
*Mostly inspector-local; no model change.*
- **H.** Reorder fields (essentials up; `doc`, raw `params (JSON)`, `timeout_ms`,
  dataflow slots, `working_directory` down/collapsed). Consolidate the duplicate
  inputs/outputs editors.
- **I.** Relabel + collapse the dataflow `inputs`/`outputs` slots with an
  inline explanation.
- **G.** Suggest `analysis.output.txt` from input `analysis.txt`.
- **F (part).** "Infer schema from an example JSON" button + a couple of schema
  presets, on top of the existing builder.
- **B (part).** Make "Filter ID" a `<select>` of existing filters (kill the free
  text) even before the edge-implied version.

### Phase 1 — Files as things + hide paths
- **D.** Artifact nodes; drag-and-drop file upload; "+ file" dialog/picker.
- **E.** Stop showing raw traversals; `working_directory` becomes auto/advanced;
  wired inputs render as upstream node/file names.

### Phase 2 — One data-carrying edge (the structural bet)
- **C + I.** Unify dependency and dataflow edges into a single data-carrying
  edge; retire the declare-a-slot flow; `depends_on`-only edges become the rare,
  visually-distinct case. (Part 2.)
- **B (full).** Filter binding fully driven by the fan-out edge; the Filter ID
  field disappears when an edge implies it.

### Phase 3 — No-code fan-out
- **J.** Guided fan-out builder: pick columns, auto-unique filenames, live
  row-1 preview; remove templating and inline/ref from the user's view.

---

## Success criteria

The refactor is "done" when a non-programmer can rebuild **#5
portfolioDividendAnalysis** (CSV fan-out + per-item AI + aggregation + the
deterministic verifier) **without typing a single id, path, JSON object, or
`{{template}}`** — only placing nodes, dropping in `port62pos.csv`, drawing
edges, picking columns, and choosing from menus. Re-run the dogfooding pass
afterward; the target is near-zero new findings of the "had to know an internal
mechanic" class.

---

## Relationship to other docs
- Supersedes the "Editor UX review" stub in `editor-dogfooding-plan.md` (the
  three themes there are subsumed by Part 2 here).
- Findings evidence lives in `editor-dogfooding-plan.md`'s Findings log
  (F-1…F-42); this plan does not duplicate it.
