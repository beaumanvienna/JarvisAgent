# jarvisCppSafetyAudit Workflow — C++ Non-Security Safety Audit

**Label:** JarvisAgent C++ Safety Audit

**Workflow doc:** Reviews each C++ header (and matching .cpp when present) in `application/` and `engine/` for non-security safety properties: concurrency, memory, lifetime, exception safety, resource cleanup, move semantics, fail-path logging, log severity, switch discipline, const-correctness, C++20 idiom uplift, TOCTOU, style, and Rust-by-default-equivalents.

This workflow runs a senior-C++-engineer review focused on **non-security** safety properties. Cyber-security concerns are explicitly out of scope here — those are covered by `jarvisCppCyberSecAudit`. Splitting the two reviews keeps each prompt focused and the per-finding signal clean.

The same per-file fan-out shape is shared with `jarvisCppDocu` and `jarvisCppCyberSecAudit`; all three are generated from `doc/misc/jarvisCppDoc.md` by `scripts/buildJarvisCppDocu.py`.

## Audit dimensions

The model is instructed to look for, and only for, non-security safety issues:

- **Concurrency safety** — data races, missing synchronization, lock ordering / deadlock potential, lock-free pattern correctness, condition-variable spurious-wakeup handling, atomic memory ordering.
- **Memory safety** — use-after-free, out-of-bounds, leaks, double-delete, raw `new`/`delete` in modern code, dangling references / iterators, `std::span` / `std::string_view` lifetime.
- **Lifetime / ownership** — references escaping scope, references captured by reference into async lambdas, smart-pointer hygiene (`unique_ptr` vs `shared_ptr` vs `weak_ptr`).
- **Exception safety** — RAII coverage, no-leak-on-throw, basic / strong / nothrow guarantees, `noexcept` correctness, exceptions across DLL/ABI boundaries.
- **Resource cleanup** — file handles, sockets, threads, mutexes, libcurl handles closed on every path including errors.
- **Move semantics** — moved-from object usage, missing `std::move`, `const&` parameters that should be by-value-and-move, `&&` overload correctness.
- **Fail-path logging completeness** — every error path emits an `LOG_*_ERROR` with `runId` / `workflowId` / `taskId` literals where the call site has them in scope.
- **Log severity appropriateness** — INFO that should be DEBUG, WARN that should be ERROR, ERROR that should be WARN, missing context.
- **`switch` discipline** — `default:` arms over closed enums, missing case for new variants, fallthrough without `[[fallthrough]]` (per CLAUDE.md).
- **Const-correctness** — non-const member functions that don't mutate, parameter passing style, `mutable` justified.
- **C++20 idiom uplift** (informational, low severity) — `std::optional`, `std::filesystem`, `std::span`, structured bindings, `[[nodiscard]]`, concepts.
- **TOCTOU / file-system races** — `exists()` followed by `open()`, race-prone tempfile patterns, missing `O_CREAT|O_EXCL`.
- **Style adherence** (low severity only) — Allman braces, `m_` prefix on members, 125-column limit, left-aligned pointer.
- **Rust-by-default-equivalents** — places where Rust's borrow checker, `Send`/`Sync`, `Option<T>` / `Result<T,E>`, exhaustive `match`, or runtime bounds checks would prevent the bug at compile time. Each finding suggests the C++ idiom that emulates the Rust guarantee.

## Triggers

- `manual` (`manual-run`) — enabled, exposed in the dashboard Run button.

## Directory layout

- Workflow file lives under `workflows/jarvisCppSafetyAudit/`.
- Each AI task uses a working directory under `queue/jarvisCppSafetyAudit/<NN>_<taskId>/`.
- The combiner writes its output to `workflows/jarvisCppSafetyAudit/<NN>_combineDocumentation/combinedSafetyAudit.md`.

## Queue artifacts produced per AI task

Each `ai_call` task declares a `queue_binding` with four parts:

- **STNG** — `STNG_docu.txt` — senior-C++-engineer persona scoped to non-security safety; same output rules.
- **TASK** — `TASK_docu.txt` — instruction listing every audit dimension above.
- **CNTX** — the source header path and the matching `.cpp` when one exists.
- **PROB** — `PROB_docu.txt` — the severity-graded output schema (see "Reading the output").

## How to run

```bash
# From the dashboard:
# Run button on jarvisCppSafetyAudit, or
mcp__j9t__run_workflow workflowId="jarvisCppSafetyAudit"
```

The default API interface follows whatever is set as `engine.api_interfaces.default` in `config.json`.

## Expected runtime and cost

Empirically measured (140 ai_call tasks). The safety prompt produces longer responses than the cyber-sec one (more dimensions to cover), so per-task wall time is somewhat higher:

| Model | Wall time | Approximate cost (per run) |
|---|---|---|
| `claude-sonnet-4-6` (default) | ~12–18 min | a few US$ |
| `claude-opus-4-7` | ~50–70 min | ~5–10× higher |
| `gpt-4.1` | ~6–10 min | lowest |

The dispatcher's adaptive rate-limit controller handles the load — no manual concurrency tuning needed.

## Reading the output

`combinedSafetyAudit.md` opens with a folder-structured TOC, then one section per source file. Each finding follows this shape:

```text
### [SEVERITY] short title
- **Category:** concurrency | memory | lifetime | exception | resource | move
                | logging | switch | const | C++20 | TOCTOU | style
                | rust-equivalent
- **Location:** function/class and approximate line
- **Issue:** what's wrong
- **Impact:** how this manifests at runtime (or what compile-time guarantee
              is missing)
- **Fix:** concrete change (for rust-equivalents, the C++ idiom that emulates
            the Rust guarantee)
```

`SEVERITY` is one of `CRITICAL | HIGH | MEDIUM | LOW`. Severity guidance baked into the prompt:

- **HIGH** — the pattern has caused a real bug in this kind of code.
- **MEDIUM** — the pattern is dangerous but not yet bitten.
- **LOW** — purely stylistic in this context.

Files with no safety concerns produce a single `### NONE — No safety issues identified.` block. Triage by searching for `### CRITICAL` and `### HIGH` first; the 2026-04-27 baseline run produced ~1240 severity findings across 140 files, with the bulk concentrated in concurrency / lifetime / resource categories.

### Example excerpt — CRITICAL concurrency finding

```text
### [CRITICAL] Lock released during createFn then active-count incremented on stale locals
- **Category:** concurrency
- **Location:** `CloudConnectionPool::Acquire`, around the `lock.unlock()` /
  `lock.lock()` bracket of the `createFn` call, then `++m_ActiveCount[...]`
- **Issue:** The lock is released before calling `createFn`, then re-acquired.
  After re-acquisition the code does `++m_ActiveCount[connectionName]`, but
  N threads can all pass the `total >= m_MaxConnectionsPerName` guard during
  the unlocked window, all unlock and call `createFn`, and all increment
  `m_ActiveCount` after re-locking, pushing the active count well above the
  configured maximum. The pool invariant is broken.
- **Impact:** Active-connection count can exceed the configured maximum,
  exhausting the underlying resource. Under high concurrency the count drifts
  permanently because each extra connection is released once but the
  over-inflated counter is never corrected.
- **Fix:** After re-acquiring the lock, re-check whether total connections
  still allow creation and bail out (returning an error or blocking again) if
  another thread already filled the pool. Alternatively, increment
  `m_ActiveCount` *before* unlocking and decrement on failure.
```

### Example excerpt — Rust-equivalent finding

```text
### [HIGH] Reference captured by reference into thread-pool lambda may outlive scope
- **Category:** rust-equivalent
- **Location:** any lambda passed to a thread pool that captures locals by
  reference (`[&]`)
- **Issue:** A reference captured by `[&]` into a task posted to the thread
  pool can outlive the enclosing scope. Rust's borrow checker prevents this
  at compile time via the `'static` bound on `spawn`; C++ silently accepts it.
- **Impact:** Use-after-free if the captured reference's referent goes out of
  scope before the lambda runs. Fired on this codebase's `m_ActiveRuns`
  invariant in 2026-04-23 — see `doc/misc/AI dispatch refactor.md`.
- **Fix:** Capture by value or by `shared_ptr` so the lambda owns the data;
  or document the lifetime invariant + assert it at runtime. As a codebase
  rule: prefer `[=]` or explicit captures for any lambda that crosses a
  thread boundary.
```

## Up-to-date behavior

JCWF freshness model is Makefile-like: an `ai_call` task is skipped when its declared `file_outputs` exist and are newer than all declared `file_inputs`, with dependencies satisfied. Editing a single source file re-runs only that file's audit task plus the combiner.

Editing the safety prompts (STNG/TASK/PROB) in `scripts/buildJarvisCppDocu.py` and re-packing with `--mode safety-audit --pack` invalidates every per-task hash and forces a full re-run.

## Re-running just the combiner

```bash
python3 -c "
import sys; sys.path.insert(0, 'scripts')
from combineDocumentation import BuildCombinedDocumentation
BuildCombinedDocumentation(
    docsDirectory='queue/jarvisCppSafetyAudit',
    outputFileName='combinedSafetyAudit.md',
    documentTitle='JarvisAgent C++ Safety Audit',
    workflowId='jarvisCppSafetyAudit',
    context={
        '_task_working_directory': 'workflows/jarvisCppSafetyAudit/141_combineDocumentation',
        '_workflow_base_directory': 'workflows/jarvisCppSafetyAudit',
    },
)
"
```

(The combiner short-circuits when the combined file is newer than every per-task input — delete it first to force a rebuild.)

## Notes

- The audit explicitly **excludes** cyber-security concerns — those go through `jarvisCppCyberSecAudit`. Two separate reviews → cleaner per-finding signal at the cost of double the AI bill. Acceptable per the dev plan §10 decision 5 (throughput-first; `max_concurrency` is the only cost lever).
- If you add new C++ files to `application/` or `engine/`, add them to `doc/misc/jarvisCppDoc.md` and re-pack the workflow with `python3 scripts/buildJarvisCppDocu.py --mode safety-audit --pack` — otherwise the audit silently skips them.
- The Rust-equivalent dimension is a deliberate hint at where C++ would benefit from compile-time safety guarantees that Rust gets for free. Findings in this category aren't "fix this now"; they're informational signals about which patterns deserve extra attention.
