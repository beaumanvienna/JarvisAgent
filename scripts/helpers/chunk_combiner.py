# scripts/helpers/chunk_combiner.py
#
# Chunk combination handler for JarvisAgent
# -----------------------------------------
# Combines chunk_###.output.md files into a single final .output.md
# once *all* chunk outputs are present and newer than their inputs.
#
# Copyright (c) 2025 JC Technolabs
# License: GPL-3.0

import re
import ctypes
from pathlib import Path

# Regex for chunk numbering
CHUNK_INPUT_REGEX = re.compile(r"^chunk_(\d+)\.md$")
CHUNK_OUTPUT_REGEX = re.compile(r"^chunk_(\d+)\.output\.md$")


def log(msg: str) -> None:
    print(f"[PY][ChunkCombiner] {msg}")


# --------------------------------------------------------------------
# Local Python → C++ error forwarder
# --------------------------------------------------------------------
class _JarvisPyStatusHelper:
    def __init__(self):
        try:
            C = ctypes.CDLL(None)
            C.JarvisPyStatus.argtypes = [ctypes.c_char_p]
            C.JarvisPyStatus.restype = None
            self._send = C.JarvisPyStatus
        except Exception:
            self._send = None

    def send(self, msg: str):
        if self._send is None:
            return
        try:
            self._send(msg.encode("utf-8"))
        except Exception:
            pass


_pystatus = _JarvisPyStatusHelper()


def _notify_chunker_error(message: str):
    log(message)
    try:
        _pystatus.send(message)
    except Exception:
        pass


# --------------------------------------------------------------------
# Chunk combination
# --------------------------------------------------------------------
def handle_chunk_output_added(trigger_file: str) -> None:
    """
    Called by main.py when a new chunk_###.output.md file appears.

    Soft failures (normal workflow-waiting):
      - Missing matching chunk outputs
      - Output older than input

    Hard failures (dev mode):
      - Unexpected folder structure
      - Read/write failures

    Hard failures report to C++ → Stop Python Engine.
    """

    trigger_path = Path(trigger_file)
    folder = trigger_path.parent

    # ------------------------------------------------------------------
    # 1) Gather all chunk input files
    # ------------------------------------------------------------------
    chunk_inputs: dict[int, Path] = {}
    try:
        for file_path in folder.iterdir():
            match = CHUNK_INPUT_REGEX.match(file_path.name)
            if match:
                index = int(match.group(1))
                chunk_inputs[index] = file_path
    except Exception as exception:
        _notify_chunker_error(f"ChunkCombiner: failed to iterate folder {folder}: {exception}")
        return

    # No chunks -> ignore
    if not chunk_inputs:
        return

    # ------------------------------------------------------------------
    # 2) Check output chunks (soft waiting)
    # ------------------------------------------------------------------
    chunk_outputs: dict[int, Path] = {}
    for index, input_file in chunk_inputs.items():
        output = folder / f"chunk_{index:03d}.output.md"

        # Soft conditions
        if not output.exists():
            return

        try:
            if output.stat().st_mtime <= input_file.stat().st_mtime:
                return
        except Exception as exception:
            _notify_chunker_error(f"ChunkCombiner: failed timestamp check: {exception}")
            return

        chunk_outputs[index] = output

    # ------------------------------------------------------------------
    # 3) Determine combined output file location
    # ------------------------------------------------------------------
    folder_name = folder.name
    if not folder_name.endswith("_chunks"):
        _notify_chunker_error(
            f"ChunkCombiner: folder name does not end with '_chunks': {folder_name}"
        )
        return

    original_md_name = folder_name.replace("_chunks", "")
    combined_output = folder.parent / original_md_name.replace(".md", ".output.md")

    # ------------------------------------------------------------------
    # 4) Combine sorted outputs
    # ------------------------------------------------------------------
    sorted_indices = sorted(chunk_outputs.keys())
    log(f"Combining {len(sorted_indices)} chunks into: {combined_output}")

    combined_parts: list[str] = []

    for index in sorted_indices:
        output_file = chunk_outputs[index]
        try:
            text = output_file.read_text(encoding="utf-8").rstrip()
        except Exception as exception:
            _notify_chunker_error(
                f"ChunkCombiner: failed to read chunk output {output_file}: {exception}"
            )
            return

        combined_parts.append(text + "\n\n")

    combined_text = "".join(combined_parts)

    # ------------------------------------------------------------------
    # 5) Write final combined output
    # ------------------------------------------------------------------
    try:
        combined_output.write_text(combined_text, encoding="utf-8")
    except Exception as exception:
        _notify_chunker_error(
            f"ChunkCombiner: failed to write combined output {combined_output}: {exception}"
        )
        return

    log(f"Combined file written: {combined_output}")
