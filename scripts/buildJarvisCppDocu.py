#!/usr/bin/env python3
"""
Build a per-file ai_call JCWF over the C++ source list in
``doc/misc/jarvisCppDoc.md``.  Three modes are supported, selected by ``--mode``:

  * ``docu``           — generate Markdown class documentation per file.
  * ``cyber-sec-audit`` — review each file for cyber-security gaps.
  * ``safety-audit``   — review each file for non-security safety properties
                         (concurrency, memory, lifetime, exception, resource,
                         move semantics, fail-path logging, switch discipline,
                         Rust-equivalent compile-time guarantees, …).

The input table has one row per task; both columns may contain backtick-quoted
paths.  All paths in a row become the file_inputs (and cntx_files) of one
ai_call task — the first path drives the task id and label.  A trailing
combineDocumentation Python task aggregates every per-task ``docu.output.md``
into a single combined Markdown document whose filename / title depends on the
mode.

Re-run any time the source list changes.  Repack the .jcwf zip with --pack.

    python3 scripts/buildJarvisCppDocu.py --mode docu
    python3 scripts/buildJarvisCppDocu.py --mode cyber-sec-audit --pack
    python3 scripts/buildJarvisCppDocu.py --mode safety-audit --pack
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
TABLE_FILE = REPO_ROOT / "doc" / "misc" / "jarvisCppDoc.md"

# Path under workflows/<id>/ — engine + application source roots are three
# directory hops above (../../../).
SOURCE_PREFIX = "../../../"


# ---------------------------------------------------------------------------
# Mode definitions
# ---------------------------------------------------------------------------
#
# Each mode produces a workflow at workflows/<workflow_id>/<workflow_id>.json
# (auto-bootstrapping global.json on first run) and, with --pack, a packed
# workflows/<workflow_id>.jcwf mirrored to example/workflows/<workflow_id>.jcwf.
#
# The combineDocumentation Python task receives outputFileName / documentTitle
# / workflowId via JCWF inputs; combineDocumentation.py uses those to title
# the combined document and choose the output filename.

DOCU_STNG = (
    "You are a senior C++ engineer documenting j9t — a workflow engine that handles AI requests, "
    "secrets, webhooks, multi-provider auth signing, file watching, embedded Python, and external "
    "integrations. Be concrete and specific. No guessing, no embellishments. Cite exact function or "
    "class names; describe what is actually in the code, not what could be there. If the file is a "
    "small header (forward declarations, type aliases only), keep the doc proportionally short. Do "
    "not wrap output in triple-backtick code fences (no ```markdown). Output plain Markdown text only."
)
DOCU_TASK = (
    "Document this C++ source for fellow engineers and AI assistants who need to learn the file's "
    "role quickly. Cover: (1) what the file/class is responsible for in one paragraph; (2) the "
    "public API — each method's purpose, expected inputs, side effects; (3) collaborators and "
    "dependencies the reader should know about; (4) ownership / lifetime / threading rules where "
    "they affect callers; (5) non-obvious behaviour, invariants, or constraints. Skip code-style "
    "commentary and trivial getters/setters."
)
DOCU_PROB = (
    "Generate documentation for the provided C++ source. Output Markdown using this structure:\n"
    "\n"
    "## <ClassOrFileName>\n"
    "\n"
    "**Role:** one-paragraph summary of what this file/class does in the system.\n"
    "\n"
    "**Public API:**\n"
    "- `MethodName(args) → return` — purpose, expected inputs, side effects.\n"
    "\n"
    "**Collaborators:** classes/modules this file works with and how.\n"
    "\n"
    "**Threading / lifetime:** ownership rules, locking, who-owns-who, async constraints.\n"
    "\n"
    "**Notes:** non-obvious behaviour, invariants, gotchas, constraints worth knowing.\n"
)


CYBER_SEC_STNG = (
    "You are a senior application security engineer reviewing C++ code for j9t — a workflow engine "
    "that handles AI requests, secrets, webhooks, multi-provider auth signing, file watching, "
    "embedded Python, and external integrations. Be concrete and specific. No guessing, no "
    "embellishments. Cite exact function or class names and approximate line numbers when flagging "
    "issues. If the file has no security concerns, say so in one sentence and stop. Do not invent "
    "issues to fill space. Do not wrap output in triple-backtick code fences (no ```markdown). "
    "Output plain Markdown text only."
)
CYBER_SEC_TASK = (
    "Review this C++ source for security gaps. Focus areas: input validation; injection (SQL, "
    "shell, HTTP header, log); authentication/authorization bypass; cryptography misuse (weak "
    "algorithms, hardcoded secrets/IVs/keys, RNG misuse, missing cert verification); memory safety "
    "(use-after-free, out-of-bounds, lifetime/dangling references); race conditions and TOCTOU; "
    "secrets leakage in logs/errors; insecure deserialization (JSON, multipart, MIME); SSRF and "
    "path traversal; uncontrolled allocation / DoS; TLS configuration. Skip purely stylistic "
    "concerns and code-quality observations that are not security issues."
)
CYBER_SEC_PROB = (
    "Identify cyber-security issues in the provided C++ source. For each finding output:\n"
    "\n"
    "### [SEVERITY] short title\n"
    "- **Location:** function/class and approximate line\n"
    "- **Issue:** what's wrong\n"
    "- **Impact:** what an attacker could do\n"
    "- **Fix:** concrete change\n"
    "\n"
    "SEVERITY is one of CRITICAL | HIGH | MEDIUM | LOW. Order findings most-severe first. "
    "If the file has no security concerns, output exactly:\n"
    "\n"
    "### NONE\n"
    "No security issues identified.\n"
)


SAFETY_STNG = (
    "You are a senior C++ engineer reviewing j9t for non-security safety properties — j9t is a "
    "workflow engine that handles AI requests, secrets, webhooks, multi-provider auth signing, "
    "file watching, embedded Python, and external integrations. Look for concurrency races, "
    "memory / lifetime / ownership bugs, exception-safety gaps, resource leaks, broken move "
    "semantics, fail-path logging holes, log-severity mismatches, switch-discipline violations, "
    "and places where Rust's borrow checker, Send/Sync traits, Option<T> / Result<T,E>, or "
    "exhaustive match would prevent a class of bug at compile time. Be concrete and specific. No "
    "guessing, no embellishments. Cite exact function or class names and approximate line numbers "
    "when flagging issues. If the file has no safety concerns, say so in one sentence and stop. Do "
    "not invent issues to fill space. Do not wrap output in triple-backtick code fences (no "
    "```markdown). Output plain Markdown text only."
)
SAFETY_TASK = (
    "Review this C++ source for safety properties. Cyber-security concerns are out of scope here "
    "— a separate audit covers those. Focus areas:\n"
    "\n"
    "- Concurrency safety: data races, missing synchronization, lock ordering / deadlock potential, "
    "lock-free pattern correctness, condition-variable spurious-wakeup handling, atomic memory "
    "ordering misuse.\n"
    "- Memory safety: use-after-free, out-of-bounds, leaks, double-delete, raw new/delete in modern "
    "code, dangling references / iterators, std::span / std::string_view lifetime.\n"
    "- Lifetime / ownership: references escaping scope, references captured by reference into "
    "async work or thread-pool lambdas, unclear ownership across boundaries, smart-pointer hygiene "
    "(`unique_ptr` vs `shared_ptr`, `weak_ptr` where cycles are possible).\n"
    "- Exception safety: RAII coverage, no-leak-on-throw, basic / strong / nothrow guarantees, "
    "`noexcept` correctness, exceptions across DLL/ABI boundaries.\n"
    "- Resource cleanup: file handles, sockets, threads, mutexes, libcurl handles closed on every "
    "path including errors.\n"
    "- Move semantics: moved-from object usage, missing `std::move` on local rvalues, `const&` "
    "parameters that should be by-value-and-move, `&&` overloads correctness.\n"
    "- Fail-path logging completeness: every error path emits `LOG_APP_ERROR` (or appropriate "
    "`LOG_*_ERROR`) with `runId` / `workflowId` / `taskId` literals where the call site has them in "
    "scope. Flag fail paths that return/throw silently or log at WARN/INFO when the path is "
    "unrecoverable.\n"
    "- Log severity appropriateness: INFO that should be DEBUG, WARN that should be ERROR, ERROR "
    "that should be WARN, missing context in log messages.\n"
    "- `switch` discipline: `default:` arms over closed enums, missing `case` for new enum "
    "variants, fallthrough without `[[fallthrough]]`.\n"
    "- Const-correctness: non-const member functions that don't mutate; parameters by `const&` "
    "vs by-value; mutable members justified.\n"
    "- C++20 idiom uplift (informational, low severity): places where `std::optional`, "
    "`std::filesystem`, `std::span`, structured bindings, `[[nodiscard]]`, or concepts would "
    "improve safety or expressivity.\n"
    "- TOCTOU / file-system races: `exists()` followed by `open()`, race-prone tempfile patterns, "
    "missing `O_CREAT|O_EXCL`.\n"
    "- Style adherence (low severity only): Allman braces, `m_` prefix on members, 125-column "
    "limit, left-aligned pointer.\n"
    "- Rust-by-default-equivalents: name C++ patterns where Rust's borrow checker, `Send`/`Sync` "
    "traits, lifetime annotations, `Option<T>`, `Result<T,E>`, exhaustive `match`, or runtime "
    "bounds checks would prevent the bug at compile time. Concretely call out: references that "
    "could outlive their referent; references captured by reference into async lambdas; shared "
    "mutable state without an explicit lock; iterator invalidation; `nullptr`-dereference paths "
    "where `std::optional<T>` would force a check; unchecked container indexing; functions that "
    "swallow errors silently; cross-thread sharing of types that aren't documented thread-safe; "
    "`switch` over an enum without `static_assert` on the variant count. For each Rust-equivalent "
    "finding, suggest the C++ idiom that emulates the Rust guarantee (`std::optional`, "
    "`gsl::not_null`, scoped lock + private data, `[[nodiscard]]` on error-returning functions, "
    "exhaustive switch via `static_assert`).\n"
)
SAFETY_PROB = (
    "Identify non-security safety issues in the provided C++ source. For each finding output:\n"
    "\n"
    "### [SEVERITY] short title\n"
    "- **Category:** concurrency | memory | lifetime | exception | resource | move | logging | "
    "switch | const | C++20 | TOCTOU | style | rust-equivalent\n"
    "- **Location:** function/class and approximate line\n"
    "- **Issue:** what's wrong\n"
    "- **Impact:** how this manifests at runtime (or what compile-time guarantee is missing)\n"
    "- **Fix:** concrete change (for rust-equivalents, the C++ idiom that emulates the Rust "
    "guarantee)\n"
    "\n"
    "SEVERITY is one of CRITICAL | HIGH | MEDIUM | LOW. Use HIGH when the pattern has caused a "
    "real bug in this kind of code, MEDIUM when the pattern is dangerous but not yet bitten, LOW "
    "when it is purely stylistic in this context. Order findings most-severe first. If the file "
    "has no safety concerns, output exactly:\n"
    "\n"
    "### NONE\n"
    "No safety issues identified.\n"
)


MODES: dict[str, dict] = {
    "docu": {
        "workflow_id": "jarvisCppDocu",
        "workflow_label": "JarvisAgent C++ Docu Generator",
        "workflow_doc": (
            "Generates Markdown documentation for each C++ header (and matching .cpp when "
            "present) in application/ and engine/. Each task is an ai_call and writes its "
            "artifacts into a per-task folder under ../queue/<workflowId>/."
        ),
        "label_prefix": "Doc:",
        "stng": DOCU_STNG,
        "task": DOCU_TASK,
        "prob": DOCU_PROB,
        "combined_filename": "combinedDocumentation.md",
        "combined_title": "JarvisAgent C++ Documentation",
    },
    "cyber-sec-audit": {
        "workflow_id": "jarvisCppCyberSecAudit",
        "workflow_label": "JarvisAgent C++ Cyber-Security Audit",
        "workflow_doc": (
            "Reviews each C++ header (and matching .cpp when present) in application/ and "
            "engine/ for cyber-security gaps. Each task is an ai_call producing severity-graded "
            "findings under ../queue/<workflowId>/."
        ),
        "label_prefix": "Sec:",
        "stng": CYBER_SEC_STNG,
        "task": CYBER_SEC_TASK,
        "prob": CYBER_SEC_PROB,
        "combined_filename": "combinedCyberSecAudit.md",
        "combined_title": "JarvisAgent C++ Cyber-Security Audit",
    },
    "safety-audit": {
        "workflow_id": "jarvisCppSafetyAudit",
        "workflow_label": "JarvisAgent C++ Safety Audit",
        "workflow_doc": (
            "Reviews each C++ header (and matching .cpp when present) in application/ and "
            "engine/ for non-security safety properties: concurrency, memory, lifetime, "
            "exception safety, resource cleanup, move semantics, fail-path logging, log severity, "
            "switch discipline, const-correctness, C++20 idiom uplift, TOCTOU, style, and "
            "Rust-by-default-equivalents."
        ),
        "label_prefix": "Safety:",
        "stng": SAFETY_STNG,
        "task": SAFETY_TASK,
        "prob": SAFETY_PROB,
        "combined_filename": "combinedSafetyAudit.md",
        "combined_title": "JarvisAgent C++ Safety Audit",
    },
}


def task_id_from_path(p: str) -> str:
    """Convert a source path into a task id matching the existing convention.

    `application/web/webServer.h`        → `doc_application_web_webServer_h`
    `application/web/webServer_studio.cpp` → `doc_application_web_webServer_studio_cpp`
    """
    sanitized = re.sub(r"[^A-Za-z0-9]+", "_", p).strip("_")
    return f"doc_{sanitized}"


def parse_table(md_path: Path) -> list[list[str]]:
    """Return one row per data row in the markdown table — each row is the
    ordered list of source paths mentioned in any column of that row.  Empty
    rows and the header/separator lines are skipped."""
    rows: list[list[str]] = []
    code = re.compile(r"`([^`]+)`")
    for line in md_path.read_text().splitlines():
        if not line.startswith("|"):
            continue
        # Skip the header row ("| Header (.h) | Paired ...").
        if "Header" in line and "Paired" in line:
            continue
        # Skip the separator row ("|---|---|").
        if re.fullmatch(r"\|\s*[-:|\s]+\|\s*[-:|\s]+\|\s*", line):
            continue
        paths = code.findall(line)
        if paths:
            rows.append(paths)
    return rows


def build_ai_task(mode_cfg: dict, index: int, paths: list[str]) -> dict:
    primary = paths[0]
    tid = task_id_from_path(primary)
    label = f"{mode_cfg['label_prefix']} {primary}"
    if len(paths) > 1:
        label += f" (+{len(paths)-1})"
    workdir = f"../../queue/{mode_cfg['workflow_id']}/{index:02d}_{tid}"
    file_refs = [f"{SOURCE_PREFIX}{p}" for p in paths]
    return {
        "id": tid,
        "type": "ai_call",
        "label": label,
        "mode": "single",
        "working_directory": workdir,
        "file_inputs": file_refs,
        "file_outputs": ["docu.output.md"],
        "queue_binding": {
            "stng_files": [{"path": "STNG_docu.txt", "content": mode_cfg["stng"]}],
            "task_files": [{"path": "TASK_docu.txt", "content": mode_cfg["task"]}],
            "cntx_files": file_refs,
            "prob_files": [{"path": "PROB_docu.txt", "content": mode_cfg["prob"]}],
        },
    }


def build_reduce_task(mode_cfg: dict, index: int, dep_ids: list[str]) -> dict:
    """The Python reducer that scans the queue dir and writes the combined doc.

    outputFileName / documentTitle / workflowId travel via JCWF inputs (with
    ``default`` values) so combineDocumentation.py can title and name the output
    appropriately for each mode without per-mode forks of the combiner.
    """
    return {
        "id": "combineDocumentation",
        "type": "python",
        "label": "Combine generated per-file output",
        "doc": (
            "Generates a single combined Markdown document with hyperlinks "
            "and folder-structured TOC."
        ),
        "mode": "single",
        "depends_on": dep_ids,
        "working_directory": f"{index:02d}_combineDocumentation",
        "file_inputs": [f"../../../queue/{mode_cfg['workflow_id']}"],
        "file_outputs": [mode_cfg["combined_filename"]],
        "params": {
            "module": "combineDocumentation",
            "function": "BuildCombinedDocumentation",
        },
        "inputs": {
            # Tell the combiner which queue dir to scan. Without this, _auto_find_docs_root
            # picks whichever workflow's queue/ folder has the most docu.output.md files —
            # which means all three audit/docu reducers race for the same fallback and the
            # cyber-sec / safety combiners end up reading the docu workflow's outputs.
            "docsDirectory": {
                "type": "string",
                "required": False,
                "default": f"../../../queue/{mode_cfg['workflow_id']}",
            },
            "outputFileName": {
                "type": "string",
                "required": False,
                "default": mode_cfg["combined_filename"],
            },
            "documentTitle": {
                "type": "string",
                "required": False,
                "default": mode_cfg["combined_title"],
            },
            "workflowId": {
                "type": "string",
                "required": False,
                "default": mode_cfg["workflow_id"],
            },
            "context": {"type": "object", "required": False},
        },
    }


def build_canvas(mode_cfg: dict, rows: list[list[str]]) -> dict:
    tasks: dict[str, dict] = {}
    dep_ids: list[str] = []
    for i, paths in enumerate(rows, start=1):
        task = build_ai_task(mode_cfg, i, paths)
        if task["id"] in tasks:
            sys.exit(
                f"duplicate task id {task['id']} — primary path {paths[0]} "
                "appears in more than one row of the table"
            )
        tasks[task["id"]] = task
        dep_ids.append(task["id"])
    reduce_index = len(rows) + 1
    reduce_task = build_reduce_task(mode_cfg, reduce_index, dep_ids)
    tasks[reduce_task["id"]] = reduce_task
    return {"tasks": tasks}


def ensure_global_json(workflow_dir: Path, mode_cfg: dict) -> None:
    """Auto-bootstrap workflows/<id>/global.json if missing.  The bounded
    ``--mode`` choices guarantee the workflow_id is one of three known values,
    so no typo can create a stray directory.  Existing global.json is left
    alone — the script never overwrites a hand-edited workflow header."""
    workflow_dir.mkdir(parents=True, exist_ok=True)
    global_file = workflow_dir / "global.json"
    if global_file.exists():
        return
    payload = {
        "version": "1.0",
        "id": mode_cfg["workflow_id"],
        "label": mode_cfg["workflow_label"],
        "doc": [mode_cfg["workflow_doc"]],
        "triggers": [
            {
                "type": "manual",
                "id": "manual-run",
                "enabled": True,
                "params": {"exposed_in_ui": True},
            }
        ],
    }
    global_file.write_text(json.dumps(payload, indent=2) + "\n")
    print(f"bootstrapped {global_file.relative_to(REPO_ROOT)}")


def repack_jcwf(workflow_dir: Path, workflow_id: str) -> None:
    """Repackage the .jcwf zip from the canonical extracted folder, then mirror
    the result to example/workflows/.  Uses the system `zip` so the resulting
    archive matches what j9t produced previously (deflate, no extra attrs)."""
    jcwf_file = REPO_ROOT / "workflows" / f"{workflow_id}.jcwf"
    example_jcwf = REPO_ROOT / "example" / "workflows" / f"{workflow_id}.jcwf"

    if jcwf_file.exists():
        jcwf_file.unlink()
    subprocess.run(
        ["zip", "-q", str(jcwf_file), "global.json", f"{workflow_id}.json"],
        cwd=workflow_dir,
        check=True,
    )
    example_jcwf.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(jcwf_file, example_jcwf)
    print(
        f"repacked {jcwf_file.relative_to(REPO_ROOT)} "
        f"and mirrored to {example_jcwf.relative_to(REPO_ROOT)}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mode",
        choices=sorted(MODES.keys()),
        required=True,
        help="Which workflow flavour to generate.",
    )
    parser.add_argument(
        "--pack",
        action="store_true",
        help="After writing the canvas JSON, repackage workflows/<id>.jcwf "
             "and mirror it to example/workflows/.",
    )
    args = parser.parse_args()

    mode_cfg = MODES[args.mode]
    workflow_id = mode_cfg["workflow_id"]
    workflow_dir = REPO_ROOT / "workflows" / workflow_id
    canvas_file = workflow_dir / f"{workflow_id}.json"

    if not TABLE_FILE.exists():
        sys.exit(f"input table not found: {TABLE_FILE}")

    ensure_global_json(workflow_dir, mode_cfg)

    rows = parse_table(TABLE_FILE)
    if not rows:
        sys.exit(f"no data rows parsed from {TABLE_FILE}")

    canvas = build_canvas(mode_cfg, rows)
    canvas_file.write_text(json.dumps(canvas, indent=2) + "\n")

    n_ai = sum(1 for t in canvas["tasks"].values() if t["type"] == "ai_call")
    n_reduce = sum(1 for t in canvas["tasks"].values() if t["type"] == "python")
    print(
        f"[{args.mode}] wrote {canvas_file.relative_to(REPO_ROOT)}: "
        f"{n_ai} ai_call tasks + {n_reduce} reducer"
    )

    if args.pack:
        repack_jcwf(workflow_dir, workflow_id)

    return 0


if __name__ == "__main__":
    sys.exit(main())
