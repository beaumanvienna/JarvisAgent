# scripts/helpers/md_chunker.py
# -*- coding: utf-8 -*-

"""
Markdown chunking utilities for JarvisAgent.

Responsibilities:
- Inspect markdown files created by MarkItDown (or any .md file).
- If a file is larger than the configured "max file size in kB",
  split it into multiple chunk_XXX.md files.
- Chunk folder: "<original_filename>.md_chunks" in the same directory.

Copyright (c) 2025 JC Technolabs
License: GPL-3.0
"""

import os
import json
import re
import ctypes
from pathlib import Path
from typing import List

from helpers.log import log_info, log_warn, log_error
from helpers.fileutils import read_text_file, write_text_file


# --------------------------------------------------------------------
# Local Python → C++ error forwarder (same pattern used in other modules)
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
    log_error(message)
    try:
        _pystatus.send(message)
    except Exception:
        pass


# --------------------------------------------------------------------
# Main entry point
# --------------------------------------------------------------------
def chunk_markdown_if_needed(markdown_file_path: str) -> None:
    """
    Entry point called from main.py::OnEvent() when a .md file is added.
    """

    markdown_path = Path(markdown_file_path)

    if not markdown_path.is_file():
        log_warn(f"chunk_markdown_if_needed: path is not a file: {markdown_file_path}")
        return

    # ---------- load config.json ----------
    try:
        max_file_size_bytes = _load_max_file_size_bytes()
    except Exception as exception:
        _notify_chunker_error(f"Failed to load max file size from config.json: {exception}")
        return

    # ---------- file size ----------
    try:
        file_size_bytes = markdown_path.stat().st_size
    except OSError as exception:
        _notify_chunker_error(f"Failed to get file size for {markdown_file_path}: {exception}")
        return

    # ---------- no chunking required ----------
    if file_size_bytes <= max_file_size_bytes:
        log_info(
            f"Markdown file is within size limit ({file_size_bytes} bytes <= "
            f"{max_file_size_bytes} bytes); no chunking required: {markdown_file_path}"
        )
        return

    log_info(
        f"Large markdown detected ({file_size_bytes} bytes > {max_file_size_bytes} bytes), "
        f"chunking: {markdown_file_path}"
    )

    # ---------- read markdown ----------
    try:
        markdown_text = read_text_file(str(markdown_path))
    except Exception as exception:
        _notify_chunker_error(f"Failed to read markdown file {markdown_file_path}: {exception}")
        return

    # ---------- chunk ----------
    try:
        chunks = _create_chunks(markdown_text, max_file_size_bytes)
    except Exception as exception:
        _notify_chunker_error(f"Chunking algorithm failed for {markdown_file_path}: {exception}")
        return

    if not chunks:
        log_warn(f"No chunks produced for {markdown_file_path}; this should not happen.")
        return

    # ---------- chunk folder ----------
    chunks_folder = markdown_path.with_name(markdown_path.name + "_chunks")
    try:
        chunks_folder.mkdir(exist_ok=True)
    except OSError as exception:
        _notify_chunker_error(f"Failed to create chunks folder {chunks_folder}: {exception}")
        return

    log_info(f"Writing {len(chunks)} chunks to folder: {chunks_folder}")

    # ---------- write each chunk ----------
    for chunk_index, chunk_text in enumerate(chunks, start=1):
        chunk_filename = f"chunk_{chunk_index:03d}.md"
        chunk_path = chunks_folder / chunk_filename
        try:
            write_text_file(str(chunk_path), chunk_text)
        except Exception as exception:
            _notify_chunker_error(f"Failed to write chunk {chunk_path}: {exception}")
            return

    # ---------- stale chunk warning (soft condition) ----------
    try:
        existing_chunk_files = sorted(chunks_folder.glob("chunk_*.md"))
        if len(existing_chunk_files) > len(chunks):
            log_warn(
                f"Chunk folder {chunks_folder} contains {len(existing_chunk_files)} chunk_XXX.md files, "
                f"but only {len(chunks)} were written in this run. "
                f"Older extra chunks were left untouched (no deletions by design)."
            )
    except Exception as exception:
        _notify_chunker_error(f"Failed to inspect chunk folder {chunks_folder}: {exception}")
        return

    log_info(f"Markdown chunking completed for {markdown_file_path}")


# ----------------------------------------------------------------------
# Internal helpers
# ----------------------------------------------------------------------
def _load_max_file_size_bytes() -> int:
    this_file = Path(__file__).resolve()
    project_root = this_file.parents[2]
    config_path = project_root / "config.json"

    if not config_path.is_file():
        raise FileNotFoundError(f"config.json not found at {config_path}")

    with config_path.open("r", encoding="utf-8") as config_file:
        config = json.load(config_file)

    max_file_size_kb = config.get("max file size in kB", 24)
    if not isinstance(max_file_size_kb, (int, float)):
        raise ValueError(
            f"Invalid 'max file size in kB' value in config.json: {max_file_size_kb}"
        )

    return int(max_file_size_kb * 1024)


def _create_chunks(markdown_text: str, max_chunk_size_bytes: int) -> List[str]:
    sections = _split_into_sections(markdown_text)

    chunks: List[str] = []
    current_parts: List[str] = []
    current_size_bytes: int = 0

    for section in sections:
        section_bytes = section.encode("utf-8")
        section_size = len(section_bytes)

        if section_size > max_chunk_size_bytes:
            smaller_parts = _split_large_section(section, max_chunk_size_bytes)
            for part in smaller_parts:
                part_bytes = part.encode("utf-8")
                part_size = len(part_bytes)

                if current_size_bytes + part_size > max_chunk_size_bytes and current_parts:
                    chunks.append("".join(current_parts))
                    current_parts = []
                    current_size_bytes = 0

                current_parts.append(part)
                current_size_bytes += part_size
        else:
            if current_size_bytes + section_size > max_chunk_size_bytes and current_parts:
                chunks.append("".join(current_parts))
                current_parts = []
                current_size_bytes = 0

            current_parts.append(section)
            current_size_bytes += section_size

    if current_parts:
        chunks.append("".join(current_parts))

    return chunks


def _split_into_sections(markdown_text: str) -> List[str]:
    heading_pattern = r"(?m)(?=^#{1,6}\s+)"
    raw_sections = re.split(heading_pattern, markdown_text)

    return [s for s in raw_sections if s.strip()] or [markdown_text]


def _split_large_section(section_text: str, max_chunk_size_bytes: int) -> List[str]:
    parts: List[str] = []

    paragraph_pattern = r"(\n\s*\n+)"
    paragraph_pieces = re.split(paragraph_pattern, section_text)

    current_parts: List[str] = []
    current_size_bytes: int = 0

    for piece in paragraph_pieces:
        piece_bytes = piece.encode("utf-8")
        piece_size = len(piece_bytes)

        if piece_size > max_chunk_size_bytes:
            if current_parts:
                parts.append("".join(current_parts))
                current_parts = []
                current_size_bytes = 0

            oversized_chunks = _split_by_bytes_utf8_safe(piece, max_chunk_size_bytes)
            parts.extend(oversized_chunks)
            continue

        if current_size_bytes + piece_size > max_chunk_size_bytes and current_parts:
            parts.append("".join(current_parts))
            current_parts = []
            current_size_bytes = 0

        current_parts.append(piece)
        current_size_bytes += piece_size

    if current_parts:
        parts.append("".join(current_parts))

    return parts


def _split_by_bytes_utf8_safe(text: str, max_chunk_size_bytes: int) -> List[str]:
    encoded = text.encode("utf-8")
    length = len(encoded)
    offset = 0
    pieces: List[str] = []

    while offset < length:
        end = min(offset + max_chunk_size_bytes, length)
        chunk_bytes = encoded[offset:end]

        while True:
            try:
                decoded = chunk_bytes.decode("utf-8")
                break
            except UnicodeDecodeError:
                chunk_bytes = chunk_bytes[:-1]
                if not chunk_bytes:
                    offset += 1
                    break
        else:
            continue

        pieces.append(decoded)
        offset += len(chunk_bytes)

    return pieces
