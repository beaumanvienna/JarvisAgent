# jarvisCppCyberSecAudit Workflow — C++ Cyber-Security Audit

**Label:** JarvisAgent C++ Cyber-Security Audit

**Workflow doc:** Reviews each C++ header (and matching .cpp when present) in `application/` and `engine/` for cyber-security gaps. Each task is an `ai_call` producing severity-graded findings under `../queue/<workflowId>/`.

This workflow runs a senior-application-security-engineer review over every C++ source file in the j9t tree, one `ai_call` per file. Each task produces a Markdown report listing CRITICAL / HIGH / MEDIUM / LOW findings with a concrete location, impact, and proposed fix. A final `combineDocumentation` Python reducer aggregates every per-file report into a single navigable `combinedCyberSecAudit.md`.

The same `ai_call` shape and per-file fan-out pattern is shared with `jarvisCppDocu` (documentation) and `jarvisCppSafetyAudit` (safety review). All three are generated from the same source-file table (`doc/misc/jarvisCppDoc.md`) by `scripts/buildJarvisCppDocu.py`.

## Audit dimensions

The model is instructed to look for, and only for, cyber-security issues:

- Input validation
- Injection (SQL, shell, HTTP header, log)
- Authentication / authorization bypass
- Cryptography misuse — weak algorithms, hardcoded secrets / IVs / keys, RNG misuse, missing certificate verification
- Memory safety — use-after-free, out-of-bounds, lifetime / dangling references
- Race conditions and TOCTOU
- Secrets leakage in logs / errors
- Insecure deserialization (JSON, multipart, MIME)
- SSRF and path traversal
- Uncontrolled allocation / DoS
- TLS configuration

Stylistic concerns and code-quality observations that are not security issues are explicitly skipped — those belong in `jarvisCppSafetyAudit`.

## Triggers

- `manual` (`manual-run`) — enabled, exposed in the dashboard Run button.

## Directory layout

- Workflow file lives under `workflows/jarvisCppCyberSecAudit/`.
- Each AI task uses a working directory under `queue/jarvisCppCyberSecAudit/<NN>_<taskId>/`.
- The combiner writes its output to `workflows/jarvisCppCyberSecAudit/<NN>_combineDocumentation/combinedCyberSecAudit.md`.

## Queue artifacts produced per AI task

Each `ai_call` task declares a `queue_binding` with four parts:

- **STNG** — `STNG_docu.txt` — senior-app-sec-engineer persona + output rules (no triple-backtick fences, plain Markdown only).
- **TASK** — `TASK_docu.txt` — instruction listing the audit dimensions.
- **CNTX** — the source header path and the matching `.cpp` when one exists.
- **PROB** — `PROB_docu.txt` — the severity-graded output schema (see "Reading the output" below).

## How to run

```bash
# From the dashboard:
# Run button on jarvisCppCyberSecAudit, or
mcp__j9t__run_workflow workflowId="jarvisCppCyberSecAudit"
```

The default API interface is whichever one is set as `engine.api_interfaces.default` in `config.json`. Sonnet 4.6 produces strong findings at moderate cost; Opus 4.7 is more thorough but ~5× slower and noticeably more expensive — switch the default in `config.json` if you want Opus.

## Expected runtime and cost

Empirically measured (140 ai_call tasks):

| Model | Wall time | Approximate cost (per run) |
|---|---|---|
| `claude-sonnet-4-6` (default) | ~10–15 min | a few US$ |
| `claude-opus-4-7` | ~45–60 min | ~5–10× higher |
| `gpt-4.1` | ~5–8 min | lowest |

Numbers vary with how much code is in scope and how chatty the model is on a given file. The dispatcher's adaptive rate-limit controller (`engine/curlWrapper/rateLimitController`) keeps concurrency at the provider's safe ceiling, so the run won't melt under tier-1 quotas.

## Reading the output

`combinedCyberSecAudit.md` opens with a folder-structured table of contents, then one section per source file. Each finding within a section follows this shape:

```text
### [SEVERITY] short title
- **Location:** function/class and approximate line
- **Issue:** what's wrong
- **Impact:** what an attacker could do
- **Fix:** concrete change
```

`SEVERITY` is one of `CRITICAL | HIGH | MEDIUM | LOW`. Files with no security concerns produce a single `### NONE — No security issues identified.` block.

To triage, search for `### CRITICAL` first, then `### HIGH`. The 2026-04-27 baseline run produced ~700 severity findings across 140 files; expect a similar order of magnitude on first run.

### Example excerpt — CRITICAL finding

```text
### CRITICAL Path Traversal via `outputFile` and `inputFile` Parameters
- **Location:** `GoogleSheetsCloudTaskExecutor::ExecuteCloud`, around the
  `outputPath = workDir / outputFile` and `inputPath = workDir / inputFile`
  constructions
- **Issue:** `outputFile` and `inputFile` are taken directly from
  attacker-controlled task params JSON and joined to `workDir` with
  `std::filesystem::path::operator/`. If the value begins with `/` or contains
  `..` segments, `operator/` discards `workDir` entirely or traverses outside
  it. Zero canonicalization or containment check before `std::ofstream` /
  `std::ifstream` open the resulting path.
- **Impact:** An attacker who can submit a workflow task definition can write
  arbitrary content to any file writable by the process or read any file
  readable by the process.
- **Fix:** After constructing the candidate path, call
  `std::filesystem::weakly_canonical()` on both the candidate and `workDir`,
  then assert the candidate path starts with `workDir`. Reject any path that
  escapes. Additionally, reject filenames containing `/` or `\` before joining.
```

## Up-to-date behavior

The JCWF freshness model is Makefile-like: an `ai_call` task is skipped when its declared `file_outputs` exist and are newer than all declared `file_inputs`, with the task's dependencies satisfied. Editing a single source file re-runs only that file's audit task plus the combiner — the other 139 file audits stay cached.

If you change the audit prompts (STNG/TASK/PROB), the queue-binding hash changes and every task re-runs. Use `scripts/buildJarvisCppDocu.py --mode cyber-sec-audit --pack` to regenerate after editing the prompts in that script.

## Re-running just the combiner

If you only want to refresh the combined report without re-running any AI calls (e.g., after changing the combiner script), run:

```bash
python3 -c "
import sys; sys.path.insert(0, 'scripts')
from combineDocumentation import BuildCombinedDocumentation
BuildCombinedDocumentation(
    docsDirectory='queue/jarvisCppCyberSecAudit',
    outputFileName='combinedCyberSecAudit.md',
    documentTitle='JarvisAgent C++ Cyber-Security Audit',
    workflowId='jarvisCppCyberSecAudit',
    context={
        '_task_working_directory': 'workflows/jarvisCppCyberSecAudit/141_combineDocumentation',
        '_workflow_base_directory': 'workflows/jarvisCppCyberSecAudit',
    },
)
"
```

(The combiner short-circuits when `combinedCyberSecAudit.md` is newer than every per-task input — delete the existing combined file first if you want a forced rebuild.)

## Notes

- This workflow is intentionally **AI-only** for the per-file work; the only Python step is the final reducer.
- The companion docu generator `jarvisCppDocu` shares the same source-file table and pattern. The companion safety reviewer `jarvisCppSafetyAudit` covers the non-security half (concurrency, memory, lifetime, switch discipline, Rust-equivalents, …).
- If you add new C++ files to `application/` or `engine/`, add them to `doc/misc/jarvisCppDoc.md` and re-pack the workflow with `python3 scripts/buildJarvisCppDocu.py --mode cyber-sec-audit --pack` — otherwise the audit silently skips them.
