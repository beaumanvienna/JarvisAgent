#!/usr/bin/env python3
"""
Build workflows/jarvisCppDocu/jarvisCppDocu.json from log/jarvisCppDoc.md.

The input table has one row per documentation task; both columns may contain
backtick-quoted paths.  All paths in a row become the file_inputs (and
cntx_files) of one ai_call task — the first path drives the task id and label.
A trailing combineDocumentation Python task aggregates every per-task
docu.output.md into combinedDocumentation.md.

Re-run any time the source list changes.  Repack the .jcwf zip with --pack.

    python3 scripts/buildJarvisCppDocu.py
    python3 scripts/buildJarvisCppDocu.py --pack
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
TABLE_FILE = REPO_ROOT / "log" / "jarvisCppDoc.md"
WORKFLOW_DIR = REPO_ROOT / "workflows" / "jarvisCppDocu"
CANVAS_FILE = WORKFLOW_DIR / "jarvisCppDocu.json"
GLOBAL_FILE = WORKFLOW_DIR / "global.json"
JCWF_FILE = REPO_ROOT / "workflows" / "jarvisCppDocu.jcwf"
EXAMPLE_JCWF = REPO_ROOT / "example" / "workflows" / "jarvisCppDocu.jcwf"

# Boilerplate — shared by every ai_call task.  Lifted verbatim from the existing
# canvas so regenerated tasks match the human-authored ones byte-for-byte.
STNG_CONTENT = (
    "write consise, succinct, no guessing, no embelishments "
    "Do not wrap the output in triple-backtick code fences (no ```markdown). "
    "Output plain Markdown text only."
)
TASK_CONTENT = (
    "Write a docu about this C++ class that helps humans and AIs to quickly "
    "and efficiently learn about what the function does."
)
PROB_CONTENT = (
    "Generate documentation for the provided C++ class. Output Markdown.\n"
)

# Path under workflows/<id>/ → the engine + application source roots are three
# directory hops above (../../../) — same convention used by the existing tasks.
SOURCE_PREFIX = "../../../"


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


def build_ai_task(index: int, paths: list[str]) -> dict:
    primary = paths[0]
    tid = task_id_from_path(primary)
    label = f"Docu: {primary}"
    if len(paths) > 1:
        label += f" (+{len(paths)-1})"
    workdir = f"../../queue/jarvisCppDocu/{index:02d}_{tid}"
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
            "stng_files": [{"path": "STNG_docu.txt", "content": STNG_CONTENT}],
            "task_files": [{"path": "TASK_docu.txt", "content": TASK_CONTENT}],
            "cntx_files": file_refs,
            "prob_files": [{"path": "PROB_docu.txt", "content": PROB_CONTENT}],
        },
    }


def build_reduce_task(index: int, dep_ids: list[str]) -> dict:
    """The Python reducer that scans the queue dir and writes combinedDocumentation.md."""
    return {
        "id": "combineDocumentation",
        "type": "python",
        "label": "Combine generated class docs",
        "doc": (
            "Generates a single combined Markdown document with hyperlinks "
            "and folder-structured TOC."
        ),
        "mode": "single",
        "depends_on": dep_ids,
        "working_directory": f"{index:02d}_combineDocumentation",
        "file_inputs": ["../../../queue/jarvisCppDocu"],
        "file_outputs": ["combinedDocumentation.md"],
        "params": {
            "module": "combineDocumentation",
            "function": "BuildCombinedDocumentation",
        },
        "inputs": {
            "docsDirectory": {"type": "string", "required": False},
            "context": {"type": "object", "required": False},
        },
    }


def build_canvas(rows: list[list[str]]) -> dict:
    tasks: dict[str, dict] = {}
    dep_ids: list[str] = []
    for i, paths in enumerate(rows, start=1):
        task = build_ai_task(i, paths)
        if task["id"] in tasks:
            sys.exit(
                f"duplicate task id {task['id']} — primary path {paths[0]} "
                "appears in more than one row of the table"
            )
        tasks[task["id"]] = task
        dep_ids.append(task["id"])
    reduce_index = len(rows) + 1
    reduce_task = build_reduce_task(reduce_index, dep_ids)
    tasks[reduce_task["id"]] = reduce_task
    return {"tasks": tasks}


def repack_jcwf() -> None:
    """Repackage the .jcwf zip from the canonical extracted folder, then mirror
    the result to example/workflows/.  Uses the system `zip` so the resulting
    archive matches what j9t produced previously (deflate, no extra attrs)."""
    if JCWF_FILE.exists():
        JCWF_FILE.unlink()
    subprocess.run(
        ["zip", "-q", str(JCWF_FILE), "global.json", "jarvisCppDocu.json"],
        cwd=WORKFLOW_DIR,
        check=True,
    )
    shutil.copyfile(JCWF_FILE, EXAMPLE_JCWF)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pack",
        action="store_true",
        help="After writing the canvas JSON, repackage workflows/jarvisCppDocu.jcwf "
             "and mirror it to example/workflows/.",
    )
    args = parser.parse_args()

    if not TABLE_FILE.exists():
        sys.exit(f"input table not found: {TABLE_FILE}")
    if not GLOBAL_FILE.exists():
        sys.exit(f"workflow global.json not found: {GLOBAL_FILE} — refusing to bootstrap")

    rows = parse_table(TABLE_FILE)
    if not rows:
        sys.exit(f"no data rows parsed from {TABLE_FILE}")

    canvas = build_canvas(rows)
    CANVAS_FILE.write_text(json.dumps(canvas, indent=2) + "\n")

    n_doc = sum(1 for t in canvas["tasks"].values() if t["type"] == "ai_call")
    n_reduce = sum(1 for t in canvas["tasks"].values() if t["type"] == "python")
    print(f"wrote {CANVAS_FILE.relative_to(REPO_ROOT)}: {n_doc} ai_call tasks + {n_reduce} reducer")

    if args.pack:
        repack_jcwf()
        print(f"repacked {JCWF_FILE.relative_to(REPO_ROOT)} and mirrored to {EXAMPLE_JCWF.relative_to(REPO_ROOT)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
