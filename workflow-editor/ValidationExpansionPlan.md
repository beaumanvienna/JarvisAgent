# Workflow Validation Expansion Plan

## Goal

Close the major validation gaps so the Workflow Editor can reliably answer:

- Is this workflow structurally valid JCWF?
- Does it comply with JarvisAgent runtime policies?
- Is it likely to run on this machine right now?

Validation should be **authoritative in the C++ backend**. The React UI should primarily:

- show validation results
- highlight issues
- optionally run light client-side checks for immediate feedback

---

## Scope

Add validation items covering:

- Shell task command policy
- Task-type required fields
- Path rules
- Dataflow correctness
- Trigger correctness
- Runtime feasibility (filesystem/executability preflight)

---

## Severity Model (recommended)

- **Tier A — Schema / semantics**: **Error**
  - Invalid JCWF or cannot be executed deterministically.

- **Tier B — Runtime policy**: **Error** (or Warning only if you decide to be permissive)
  - Project rules and security constraints (e.g. `scripts/` policy).

- **Tier C — Feasibility preflight**: **Warning by default**
  - Checks that depend on current machine state and may be intentionally unmet for some workflows.
  - Optionally add a future “strict mode” to treat Tier C warnings as errors.

---

## Where checks should live

- **Backend (C++):** Primary validation engine
  - `WorkflowValidator::Validate(...)`
  - Used by:
    - `POST /api/workflows/validate` (draft JSON)
    - `GET /api/workflows/{id}/validate` (on-disk workflow)

- **Parser (C++):** Parsing + type-level correctness
  - Parser should keep rejecting invalid JSON types and unknown enum strings.
  - Validator should handle cross-references and policy/feasibility.

- **Runtime (C++):** Last line of defense
  - Even with validation, runtime must still handle missing files/timeouts.

- **UI (React):** Lightweight “edit-time” checks
  - Keep current cycle/self-edge/id uniqueness checks.
  - Backend results remain authoritative.

---

## Validation Checklist

### A) Task-type required fields (Schema / semantics)

Validate required fields per task type and validate field types.

- **shell**
  - Required:
    - `params.command` (string)
  - Optional:
    - `params.args` (array of strings)
  - If present:
    - `file_inputs` / `file_outputs` must be arrays of non-empty strings

- **python**
  - Required: match the actual runtime/parser schema (e.g. entrypoint)
  - Validate any configured inputs/outputs shape

- **ai_call**
  - Required: match the runtime/parser schema for prompt
  - Validate prompt fields (e.g. `system`, `user` strings)

- **internal**
  - Required: match runtime/parser schema (internal task selector / params)

Common checks (all task types):

- `depends_on` must not contain duplicates (Warning)
- self-dependency (Error)
- unknown fields/typos (optional Warning; implement if parser preserves unknowns)

### B) Shell task command policy (Runtime policy)

Enforce:

- **Error** if `params.command` does not start with `scripts/`
- **Error** if `params.command` contains `..` segments (path traversal)
- Optional:
  - Warning if args include suspicious patterns (keep minimal initially)

### C) Path rules (Schema + policy)

Validate path fields are well-formed:

- `working_directory`:
  - Decide policy: required vs optional with default
  - Must be a string if present
  - Empty string should be Error

- `file_inputs` / `file_outputs`:
  - Must be strings, non-empty
  - Optional policy: warn on absolute paths (or allow)

### D) Dataflow validation (major gap)

For each dataflow binding:

- `from_task` exists
- `to_task` exists
- `from_output` exists in the source task outputs
- `to_input` exists in destination task inputs
- type compatibility (if types are declared)
- duplicate binding to same `to_task.to_input`:
  - Error (recommended) or Warning depending on semantics

Input satisfaction:

- If an input is marked required:
  - ensure it is satisfied by a dataflow binding or has a default

### E) Trigger validation

Validate:

- trigger `type` is valid
- `cron`:
  - `expression` present
  - timezone string format reasonable
- `file_watch`:
  - `path` present
  - `events` values valid
- `auto`/`manual`:
  - minimal fields

### F) Runtime feasibility preflight (filesystem/executability)

Best-effort checks (Warning by default):

- **shell**:
  - `scripts/<...>` exists relative to launch CWD
  - is readable; optionally executable bit set

- **python**:
  - entrypoint exists (resolved relative to workflow base dir)

- **file_inputs**:
  - warn if input path does not exist at validation time
  - keep as Warning because inputs may be produced by previous tasks

- **outputs**:
  - optionally warn if parent directory cannot be created

---

## API / UI result format improvements

Current backend validator has richer issue info (severity, code, message, path, taskId). Make the HTTP API responses return these fields so the UI can:

- highlight the exact node
- show a precise JSON-ish path
- separate errors vs warnings consistently

Recommended fields per issue:

- `severity`: `error | warning`
- `code`: stable machine-readable string
- `message`: human-readable
- `path`: JSON-ish path (e.g. `$.tasks.compile_lib1.params.command`)
- `taskId`: optional

---

## Implementation Steps (high level)

1. **Expand `WorkflowValidator`**
   - Add checks A–F.

2. **Upgrade validation API payloads**
   - Include severity/path/taskId.

3. **Update UI rendering**
   - Render backend issues consistently.
   - Highlight nodes/edges using `taskId` and/or `path`.

4. **Smoke tests with shipped example workflows**
   - Validate all known-good workflows => expect OK (and only expected warnings).
   - Add at least one intentionally-bad workflow => expect specific errors.

---

## Open Decisions

- Should `working_directory` be required, or optional with a default to workflow base directory?
- Which feasibility checks should be **error** vs **warning** (strict mode)?
- Exact schema expectations for `python`, `ai_call`, and `internal` tasks (must match parser/runtime).
