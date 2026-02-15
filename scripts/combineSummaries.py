#!/usr/bin/env python3
"""
Combine per-chapter AI summary files into a single markdown document.

Usage (standalone):
    python3 combineSummaries.py <input_glob> <output.md>

Example:
    python3 combineSummaries.py 'queue/bookSummary/03_summarize/PROB_*.output.txt' summaries/book_summary.md

Usage (as JCWF python task):
    Called with keyword args: input_glob, output_file
"""

import glob
import os
import re
import sys
import urllib.request


def _heartbeat():
    """Reset the inactivity watchdog timer via the Jarvis REST API.

    JARVIS_PORT and JARVIS_TASK_ID are set by the runtime for task processes.
    Silently ignored when running outside the runtime (standalone CLI).
    """
    port = os.environ.get("JARVIS_PORT", "")
    task_id = os.environ.get("JARVIS_TASK_ID", "")
    if port and task_id:
        try:
            req = urllib.request.Request(
                f"http://localhost:{port}/api/task/{task_id}/heartbeat",
                method="POST",
            )
            urllib.request.urlopen(req, timeout=2)
        except Exception:
            pass


def combine_summaries(input_glob, output_file):
    files = sorted(glob.glob(input_glob))

    if not files:
        raise FileNotFoundError(f"No files matched pattern: {input_glob}")

    # Sort numerically by the padded index in the filename (e.g. PROB_001, PROB_002)
    def sort_key(path):
        m = re.search(r"(\d+)\.output\.txt$", os.path.basename(path))
        return int(m.group(1)) if m else 0

    files.sort(key=sort_key)

    os.makedirs(os.path.dirname(os.path.abspath(output_file)), exist_ok=True)

    with open(output_file, "w", encoding="utf-8") as out:
        out.write("# Book Summary\n\n")

        for i, filepath in enumerate(files):
            with open(filepath, "r", encoding="utf-8") as f:
                content = f.read().strip()

            if not content:
                continue

            if i > 0:
                out.write("\n---\n\n")

            out.write(content)
            out.write("\n\n")

            _heartbeat()  # keep watchdog alive while processing chapters

    print(f"Combined {len(files)} summaries into {output_file}")
    return {"output_file": output_file, "chapter_count": str(len(files))}


def run(**kwargs):
    """Entry point for JCWF python task.

    Accepts:
        input_glob: glob pattern for per-chapter summary files
        output_file / outputPath: path to write combined markdown
        context: dict with _task_working_directory (auto-injected by JCWF)
    """
    context = kwargs.get("context", {})
    wd = context.get("_task_working_directory", ".")

    input_glob = kwargs.get("input_glob", "")
    output_file = kwargs.get("output_file", "") or kwargs.get("outputPath", "")

    if not output_file:
        output_file = os.path.join(wd, "book_summary.md")

    if not input_glob:
        raise ValueError("Required: input_glob (glob pattern for summary files)")

    return combine_summaries(input_glob, output_file)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input_glob> <output.md>", file=sys.stderr)
        sys.exit(1)

    combine_summaries(sys.argv[1], sys.argv[2])
