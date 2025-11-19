# scripts/helpers/chunk_combiner.py
#
# Chunk combination handler for JarvisAgent
# -----------------------------------------
# Combines chunk_###.output.md files into a single final .output.md
# once *all* chunk outputs are present and newer than their inputs.
#
# Usage (from main.py):
#     from helpers.chunk_combiner import handle_chunk_output_added
#     handle_chunk_output_added(file_path)
#
# Copyright (c) 2025 JC Technolabs
# License: GPL-3.0


import re
from pathlib import Path


# Regex for chunk numbering
CHUNK_INPUT_REGEX = re.compile(r"^chunk_(\d+)\.md$")
CHUNK_OUTPUT_REGEX = re.compile(r"^chunk_(\d+)\.output\.md$")


def log(msg: str):
    """Local logging helper — override if needed."""
    print(f"[PY][ChunkCombiner] {msg}")


def handle_chunk_output_added(trigger_file: str):
    """
    Called by main.py when a new chunk_###.output.md file appears.

    Steps:
      1. Identify folder containing chunks.
      2. Ensure all chunk_###.md files have matching chunk_###.output.md files.
      3. Ensure output is newer than input.
      4. Combine all outputs, sorted numerically.
      5. Write combined file into parent folder:
         <OriginalFileName>.output.md
    """

    trigger_path = Path(trigger_file)
    folder = trigger_path.parent

    # ------------------------------------------------------------------
    # 1) Gather all chunk input files
    # ------------------------------------------------------------------
    chunk_inputs = {}
    for f in folder.iterdir():
        match = CHUNK_INPUT_REGEX.match(f.name)
        if match:
            idx = int(match.group(1))
            chunk_inputs[idx] = f

    if not chunk_inputs:
        return  # No chunks in this folder — just ignore

    # ------------------------------------------------------------------
    # 2) Check for all output chunks + timestamps
    # ------------------------------------------------------------------
    chunk_outputs = {}
    for idx, input_file in chunk_inputs.items():
        output = folder / f"chunk_{idx:03d}.output.md"
        if not output.exists():
            log(f"Chunk {idx:03d} output missing — cannot combine yet.")
            return

        if output.stat().st_mtime <= input_file.stat().st_mtime:
            log(f"Chunk {idx:03d} output older than input — waiting.")
            return

        chunk_outputs[idx] = output

    # ------------------------------------------------------------------
    # 3) Determine output filename
    # Folder: "Vulkan Cookbook.pdf.md_chunks"
    # Original: "Vulkan Cookbook.pdf.md"
    # Combined: "Vulkan Cookbook.pdf.output.md"
    # ------------------------------------------------------------------
    folder_name = folder.name
    if not folder_name.endswith("_chunks"):
        log(f"Folder name does not end with '_chunks': {folder_name}")
        return

    original_md = folder_name.replace("_chunks", "")  # "Vulkan Cookbook.pdf.md"
    combined_output = folder.parent / original_md.replace(".md", ".output.md")

    # ------------------------------------------------------------------
    # 4) Combine sorted chunk outputs
    # ------------------------------------------------------------------
    sorted_indices = sorted(chunk_outputs.keys())
    log(f"Combining {len(sorted_indices)} chunks into: {combined_output}")

    combined_parts = []
    for idx in sorted_indices:
        text = chunk_outputs[idx].read_text(encoding="utf-8").rstrip()
        combined_parts.append(text + "\n\n")

    combined_text = "".join(combined_parts)

    combined_output.write_text(combined_text, encoding="utf-8")

    log(f"Combined file written: {combined_output}")
