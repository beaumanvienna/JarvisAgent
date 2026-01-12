# n8n ↔ JarvisAgent Integration Plan (React Flow Editor)

## Goals

1. **React Flow DAG Editor** in the browser for authoring and reviewing workflows.
2. **Import** existing **n8n workflows (JSON)** into the editor.
3. **Export** editor graphs back into **n8n workflow JSON** (optional “round-trip”).
4. Enable **JarvisAgent-native execution** of workflows (JCWF / internal runtime), independent of n8n, while still supporting n8n as an interchange format.
5. Keep the integration **safe** (no credential leakage), **versioned**, and **testable**.

> n8n supports exporting and importing workflows in JSON format through multiple methods (UI, copy/paste, CLI, etc.).  
> See n8n docs: https://docs.n8n.io/workflows/export-import/


---

## High-level Architecture

### Components

- **Frontend (React Flow Editor)**
  - Node/edge editing, validation UI, inspector panel, palette, minimap
  - Import/export UI actions
  - Communicates with JarvisAgent via **REST** + **WebSocket**

- **JarvisAgent (Crow Web Server)**
  - Serves static editor assets (bundled build output)
  - REST endpoints:
    - `GET /api/workflow/:id` (load internal graph)
    - `POST /api/workflow` (save internal graph)
    - `POST /api/n8n/import` (upload n8n JSON → convert → internal graph)
    - `POST /api/n8n/export` (internal graph → n8n JSON)
    - `POST /api/workflow/validate`
    - `POST /api/workflow/run`
  - WebSocket:
    - Push runtime status updates (task started/finished, logs, errors)
    - Push validation results and server-side warnings

- **Conversion Layer (in JarvisAgent)**
  - `n8n JSON ⇄ InternalGraph`
  - Maintains a node-type registry and parameter mapping rules
  - Enforces “round-trip safety” rules if exporting back to n8n

---

## Data Model Strategy

### Recommendation: Internal graph != n8n JSON

Treat **n8n workflow JSON** as an **interchange format** rather than the native representation.

- **InternalGraph**: JarvisAgent’s canonical workflow model
- **n8n JSON**: import/export formats

Why:
- n8n node types (`n8n-nodes-base.*`) and parameter schemas are specific to n8n releases and node packs.
- JarvisAgent should remain decoupled from n8n’s internal conventions and version changes.
- Round-trip (edit → export → re-import into n8n) is possible, but should be *explicitly controlled*.

---

## n8n Workflow JSON: What we map

An n8n workflow JSON generally contains:
- `nodes[]`: node definitions (type, parameters, position, etc.)
- `connections{}`: wiring between nodes (directed edges)
- workflow metadata (`name`, `settings`, etc.)

n8n docs confirm workflow import/export is JSON based:
- https://docs.n8n.io/workflows/export-import/

CLI import/export is also supported:
- https://docs.n8n.io/hosting/cli-commands/

Connections use a `main/index` structure (community explanation):
- https://community.n8n.io/t/connecting-nodes-json/2854

---

## React Flow Editor Mapping

### InternalGraph → React Flow

- Node:
  - `id` (string)
  - `type` (string) (your editor node-type key)
  - `position` `{x,y}`
  - `data`:
    - label/name
    - parameters/config
    - runtime status
    - validation issues

- Edge:
  - `id`
  - `source`, `target`
  - optional handles for multi-output nodes (`sourceHandle`, `targetHandle`)

### n8n → React Flow (via InternalGraph)

- Use `node.position` from n8n as initial layout.
- Convert n8n `connections` to directed edges.
- Store n8n-specific fields inside a “foreign payload” blob for round-trip safety:
  - `data.n8n.originalType`
  - `data.n8n.typeVersion`
  - `data.n8n.parameters` (or normalized subset)
  - `data.n8n.raw` (optional: full node object)

---

## Import Path: n8n JSON → InternalGraph

### Steps

1. **Upload JSON** to JarvisAgent (`POST /api/n8n/import`)
2. **Parse and validate**
   - Schema sanity: required fields (`nodes`, `connections`)
   - Detect unsupported constructs early (e.g., certain special node types)
3. **Node registry resolution**
   - Map `n8nNode.type` to an internal node family (e.g., `HttpRequest`, `CronTrigger`, `If`, `Merge`, etc.)
4. **Parameter conversion**
   - Keep full original parameters in a “foreign” payload if round-trip is desired
   - Optionally normalize to JarvisAgent’s internal parameter schema
5. **Connection conversion**
   - Convert each `connections[sourceName].main[outputIndex][*]` into explicit edges
   - Preserve output index in edge metadata (`edge.data.outputIndex`)
6. **Emit InternalGraph**
   - Return converted graph to the editor for display
7. **Report**
   - Warn about node types you imported “as opaque” (non-editable) vs fully supported nodes

### Import modes

- **Strict import**
  - Fail if any unknown node types exist
  - Best for round-tripping back to n8n

- **Lenient import**
  - Unknown node types become “opaque nodes” in editor (still wireable / movable)
  - Good for visualization and partial editing

---

## Export Path: InternalGraph → n8n JSON

### Export modes (choose explicitly)

1. **n8n Round-trip Export (Strict)**
   - Only allowed if:
     - Node types remain n8n-compatible
     - Required n8n fields are preserved (`typeVersion`, parameters schema)
     - Connections are exportable back into `connections{}` structure
   - This mode is for: “edit layout/wiring in our editor, then run in n8n.”

2. **n8n Template Export (Best-effort)**
   - Export what can be exported; warn about unsupported areas.
   - Unknown nodes exported as placeholders with notes.
   - This mode is for: “generate a starter workflow for n8n.”

### Export mechanics

- Build `nodes[]`:
  - Use stored `data.n8n.*` when present (to preserve fidelity)
  - Set `position` from React Flow positions
- Build `connections{}`:
  - Group edges by `source`
  - Encode per-output `main[outputIndex]` lists
  - Keep edge metadata such as `index` and `type: "main"`

---

## Credential & Secret Handling (Critical)

n8n workflows can include sensitive configuration in node parameters (depending on node type and user behavior). You must assume workflow JSON may contain secrets.

Mitigations:
- On import, run a **secret scanner**:
  - detect tokens/keys in common fields (headers, auth config, static strings)
- Offer a “redact secrets” toggle:
  - store a sanitized version in JarvisAgent
  - keep the raw version only if explicitly requested and secured
- Never log raw workflow JSON on the server.
- If round-trip export is enabled:
  - treat exported files as sensitive artifacts
  - provide “sanitized export” mode by default

---

## Validation Rules for “DAG Editor”

Even if n8n allows various constructs, your editor should define what it guarantees.

Suggested baseline rules:
- unique node IDs
- no dangling edges
- optional “must be acyclic” (DAG) rule:
  - detect cycles and warn/fail
- type-specific constraints:
  - triggers: exactly one trigger per workflow (if you want that)
  - certain node families require at least one input/output, etc.

If the editor claims “DAG,” enforce acyclicity (or clearly label it “graph editor”).

---

## Execution Strategy Options

### Option A: JarvisAgent executes InternalGraph (recommended)

- You implement node executors in JarvisAgent:
  - `HttpRequest`, `Shell`, `AI_Call`, `Zip`, etc.
- Workflows become first-class JarvisAgent assets (fits your current direction)

n8n is only used for:
- importing existing user workflows
- exporting templates / compatibility

### Option B: JarvisAgent delegates execution to n8n

- JarvisAgent acts like an orchestrator and editor host
- When user hits “Run,” JarvisAgent calls n8n (API/CLI) to execute
- Requires n8n availability + auth + version coupling

n8n has a public REST API (feature availability depends on plan/hosting):
- https://docs.n8n.io/api/

---

## Implementation Milestones

### Milestone 0 — Agree on scope
- Decide: import-only, export-only, or full round-trip
- Decide: strict DAG enforcement or allow general graphs

### Milestone 1 — Editor + InternalGraph
- Implement React Flow editor UI
- Define `InternalGraph` JSON schema and versioning
- Basic save/load to JarvisAgent via REST

### Milestone 2 — n8n Import (Lenient)
- Implement `POST /api/n8n/import`
- Convert nodes + connections
- Opaque nodes for unsupported types
- Display warnings in UI

### Milestone 3 — n8n Export (Strict Round-trip for supported subset)
- Preserve original n8n node payload
- Allow only safe edits:
  - position changes
  - edge rewiring (within supported constructs)
  - renaming (optional)
- Export back to n8n JSON and validate it

### Milestone 4 — Quality & Safety
- Secret redaction pipeline
- Regression tests with golden JSON fixtures
- Fuzz testing for malformed JSON input
- Workflow diff tooling (internal vs imported)

---

## Testing Plan

1. **Golden fixtures**
   - Keep a folder of known n8n workflow JSON files
   - Each has expected InternalGraph output and expected exported JSON

2. **Round-trip tests**
   - Import → export → import
   - Compare normalized graph equivalence (ignoring positions, timestamps)

3. **Schema validation tests**
   - Malformed JSON
   - Missing required keys
   - Unknown connection structures

4. **UI e2e tests**
   - Import a workflow
   - Move nodes
   - Rewire connections
   - Export and verify download output

---

## Risk Register

- **n8n version drift**: node typeVersion changes or schema changes break strict round-trip.
- **Secrets in exported JSON**: accidental leakage through logs, commits, or artifacts.
- **Non-DAG workflows**: if users import cyclic workflows, your “DAG editor” promise may be violated.
- **Complex node semantics**: n8n has rich execution semantics that may not translate 1:1 into JarvisAgent execution.

---

## References (n8n docs)

```text
Export/import workflows (JSON):
- https://docs.n8n.io/workflows/export-import/

CLI import/export workflows:
- https://docs.n8n.io/hosting/cli-commands/

Public REST API overview:
- https://docs.n8n.io/api/

Connections JSON structure discussion:
- https://community.n8n.io/t/connecting-nodes-json/2854
```
