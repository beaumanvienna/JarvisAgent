#!/usr/bin/env python3
# @jarvis-script
# @short: Extract chapter titles from a Markdown file
# @params: input_file — path to the input Markdown file
#   output_file — path to write extracted chapter titles (one per line)
# @description: Extracts chapter titles matching "Chapter N, Title" patterns,
#   deduplicates them, sorts by chapter number, and writes to output.
#   Called as a JCWF python task via run() or standalone CLI.
# @outputs: output_file — text file with one chapter title per line
"""
Extract chapter titles from a markdown file and write one chapter per line.

Usage:
    python extractChapters.py <input.md> <output.txt>

The script looks for patterns like "Chapter N, Title" in the markdown content,
deduplicates them, and writes each unique chapter title as a line in the output.
"""

import re
import sys
import os


def extract_chapters(input_path, output_path):
    with open(input_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Match "Chapter N, Title" — capture the full chapter heading.
    # The title ends at the next sentence (comma + verb, or next Chapter).
    pattern = r"(Chapter\s+\d+,\s+[A-Z][^.]*?)(?:\s*,\s+(?:introduces|simplifies|details|helps|illustrates|implements|gives|outlines|offers|involves|adds|briefly|describes|provides)|\s*(?=Chapter\s+\d+))"

    matches = re.findall(pattern, content)

    # Clean up: strip trailing bullets, whitespace, non-alpha junk
    def clean_title(title):
        title = title.strip().rstrip("•·▪● \t")
        return title

    # Normalize for dedup: strip bullets/whitespace so "Chapter 1, Foo" and "Chapter 1, Foo•" match
    def norm_key(title):
        return re.sub(r"\s+", " ", title.strip().rstrip("•·▪● \t"))

    seen = set()
    chapters = []
    for match in matches:
        title = clean_title(match)
        key = norm_key(title)
        # Skip overly long matches (sentence fragments, not titles)
        if len(title) > 80:
            continue
        if key not in seen:
            seen.add(key)
            chapters.append(title)

    # Sort by chapter number
    def chapter_num(title):
        m = re.match(r"Chapter\s+(\d+)", title)
        return int(m.group(1)) if m else 0

    chapters.sort(key=chapter_num)

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)

    with open(output_path, "w", encoding="utf-8") as f:
        for chapter in chapters:
            f.write(chapter + "\n")

    print(f"Extracted {len(chapters)} chapters to {output_path}")
    for i, ch in enumerate(chapters, 1):
        print(f"  {i}. {ch}")

    return chapters


def run(**kwargs):
    """Entry point for JCWF python task.

    Accepts:
        input_file / input[0]: path to the markdown file
        output_file / outputPath: path to write chapters.txt
        context: dict with _task_working_directory (auto-injected by JCWF)
    """
    context = kwargs.get("context", {})
    wd = context.get("_task_working_directory", ".")

    input_path = kwargs.get("input_file", "") or kwargs.get("input[0]", "") or context.get("_file_input_0", "")
    output_path = kwargs.get("output_file", "") or kwargs.get("outputPath", "")

    if not output_path:
        output_path = os.path.join(wd, "chapters.txt")

    if not input_path:
        raise ValueError("Required: input_file or input[0] (path to markdown file)")

    chapters = extract_chapters(input_path, output_path)

    # Build the expected glob for the downstream combine task.
    # ai_call outputs land in queue/<workflow_id>/03_summarizeChapter/PROB_*.output.txt
    workflow_id = context.get("_workflow_id", "")
    base_dir = context.get("_workflow_base_directory", wd)
    ai_output_glob = ""
    if workflow_id:
        ai_output_glob = os.path.normpath(
            os.path.join(base_dir, "..", "..", "queue", workflow_id, "03_summarizeChapter", "PROB_*.output.txt")
        )

    return {
        "outputPath": output_path,
        "chapter_count": str(len(chapters)),
        "ai_output_glob": ai_output_glob,
    }


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.md> <output.txt>", file=sys.stderr)
        sys.exit(1)

    extract_chapters(sys.argv[1], sys.argv[2])
