# jarvisCppDocu Workflow — C++ Class Documentation

**Label:** JarvisAgent C++ Docu Generator

**Workflow doc:** Generates Markdown documentation for each C++ header (and matching .cpp when present) in `application/` and `engine/`. Each task is an `ai_call` and writes its artifacts into a per-task folder under `../queue/<workflowId>/`.

This workflow runs a senior-C++-engineer documentation pass over every C++ source file in the j9t tree, one `ai_call` per file. Each task produces a Markdown class doc with role / public API / collaborators / threading / notes. A final `combineDocumentation` Python reducer aggregates every per-file doc into a single navigable `combinedDocumentation.md` with a folder-structured table of contents.

The same per-file fan-out shape is shared with `jarvisCppCyberSecAudit` (cyber-security review) and `jarvisCppSafetyAudit` (non-security safety review). All three are generated from the same source-file table (`doc/misc/jarvisCppDoc.md`) by `scripts/buildJarvisCppDocu.py` — just pick a `--mode`.

## What it generates

Per-file docs are structured Markdown:

- **Role** — one paragraph on what the file/class is responsible for in the system.
- **Public API** — each method's purpose, expected inputs, side effects.
- **Collaborators** — classes/modules the file works with and how.
- **Threading / lifetime** — ownership rules, locking, who-owns-who, async constraints.
- **Notes** — non-obvious behaviour, invariants, gotchas, constraints worth knowing.

Trivial getters/setters and code-style commentary are skipped.

## Triggers

- `manual` (`manual-run`) — enabled, exposed in the dashboard Run button.

## Directory layout

- Workflow file lives under `workflows/jarvisCppDocu/`.
- Each AI task uses a working directory under `queue/jarvisCppDocu/<NN>_<taskId>/`.
- The combiner writes its output to `workflows/jarvisCppDocu/<NN>_combineDocumentation/combinedDocumentation.md`.

## Queue artifacts produced per AI task

Each `ai_call` task declares a `queue_binding` with four parts:

- **STNG** — `STNG_docu.txt` — senior-C++-engineer persona; output rules (no triple-backtick fences, plain Markdown only).
- **TASK** — `TASK_docu.txt` — instruction listing the five doc dimensions above.
- **CNTX** — the source header path and the matching `.cpp` when one exists.
- **PROB** — `PROB_docu.txt` — the structured Markdown skeleton the model fills in.

## How to run

```bash
# From the dashboard:
# Run button on jarvisCppDocu, or
mcp__j9t__run_workflow workflowId="jarvisCppDocu"
```

The default API interface follows whatever is set as `engine.api_interfaces.default` in `config.json`.

## Expected runtime and cost

Empirically measured (140 ai_call tasks):

| Model | Wall time | Approximate cost (per run) |
|---|---|---|
| `claude-sonnet-4-6` (default) | ~10–15 min | a few US$ |
| `claude-opus-4-7` | ~45–60 min | ~5–10× higher |
| `gpt-4.1` | ~5–8 min | lowest |

Numbers vary with how chatty the model is on a given file. The dispatcher's adaptive rate-limit controller (`code/backend/engine/curlWrapper/rateLimitController`) keeps concurrency at the provider's safe ceiling, so the run won't melt under tier-1 quotas.

## Reading the output

`combinedDocumentation.md` opens with a folder-structured table of contents, then one section per source file. The 2026-04-27 baseline run produced docs for 140 files; total length was ~1 MB.

### Example excerpt

```text
## Application

**Role:** `Application` is the abstract base class that every j9t host application
must implement. It defines the mandatory lifecycle contract — startup, per-frame
update, event dispatch, and shutdown — that the engine's run-loop calls in
sequence. Concrete subclasses supply the actual logic for each phase…

**Public API:**

- `OnStart() → void` — Called once by the engine immediately after the
  application object is installed and before the first update tick. Subclasses
  perform one-time initialisation here. If initialisation cannot succeed, the
  subclass must populate `m_FatalStartupMessage` before returning so callers
  can surface the failure cleanly.
- `OnUpdate() → void` — Called repeatedly by the engine's main loop while
  `IsFinished()` returns `false`. …
- `OnEvent(std::shared_ptr<Event>&) → void` — Delivers a single engine event to
  the application. …

**Collaborators:** …

**Threading / lifetime:** No internal synchronisation; all four lifecycle
methods are assumed to be called from the engine's main thread. Subclasses
that spawn background threads must handle their own synchronisation before
these methods return.

**Notes:** `m_FatalStartupMessage` is the only formal error-reporting channel
between `OnStart` and the engine. There is no return code or exception
contract on `OnStart`…
```

## Up-to-date behavior

The JCWF freshness model is Makefile-like: an `ai_call` task is skipped when its declared `file_outputs` exist and are newer than all declared `file_inputs`, with the task's dependencies satisfied. Editing a single source file re-runs only that file's docu task plus the combiner — the other 139 stay cached.

Editing the docu prompts (STNG/TASK/PROB) in `scripts/buildJarvisCppDocu.py` and re-packing with `--mode docu --pack` invalidates every per-task hash and forces a full re-run.

## Re-running just the combiner

```bash
python3 -c "
import sys; sys.path.insert(0, 'scripts')
from combineDocumentation import BuildCombinedDocumentation
BuildCombinedDocumentation(
    docsDirectory='queue/jarvisCppDocu',
    outputFileName='combinedDocumentation.md',
    documentTitle='JarvisAgent C++ Documentation',
    workflowId='jarvisCppDocu',
    context={
        '_task_working_directory': 'workflows/jarvisCppDocu/141_combineDocumentation',
        '_workflow_base_directory': 'workflows/jarvisCppDocu',
    },
)
"
```

(The combiner short-circuits when `combinedDocumentation.md` is newer than every per-task input — delete the existing combined file first if you want a forced rebuild.)

## Notes

- This workflow is intentionally **AI-only** for the per-file work; the only Python step is the final reducer.
- The companion audits `jarvisCppCyberSecAudit` and `jarvisCppSafetyAudit` share the same source-file table and pattern. Pick a mode in `scripts/buildJarvisCppDocu.py` to switch.
- If you add new C++ files to `application/` or `engine/`, add them to `doc/misc/jarvisCppDoc.md` and re-pack the workflow with `python3 scripts/buildJarvisCppDocu.py --mode docu --pack` — otherwise the docu silently skips them.
