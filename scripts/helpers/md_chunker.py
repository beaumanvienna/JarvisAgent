# scripts/helpers/md_chunker.py
# -*- coding: utf-8 -*-

"""
Markdown chunking utilities for JarvisAgent.

Responsibilities:
- Inspect markdown files created by MarkItDown (or any .md file).
- If a file is larger than the configured "max file size in kB",
  split it into multiple chunk_XXX.md files.
- Chunk folder: "<original_filename>.md_chunks" in the same directory.
- Never delete anything. Existing chunk files with the same names
  will be overwritten, but old extra files (e.g., chunk_041.md when
  we now only need 40 chunks) are left untouched.

Algorithm overview:
1. Load max file size (in kB) from config.json.
2. If markdown file size <= limit: do nothing.
3. Otherwise:
   - Split text by markdown headings (#, ##, ###, ...).
   - Aggregate sections into chunks, keeping each chunk under max size.
   - If a single section is too large, split by paragraphs.
   - If a paragraph is still too large, split by byte length with
     UTF-8-aware slicing.

Copyright (c) 2025 JC Technolabs
License: GPL-3.0

"""

import os
import json
import re
from pathlib import Path
from typing import List

from helpers.log import log_info, log_warn, log_error
from helpers.fileutils import read_text_file, write_text_file


def chunk_markdown_if_needed(markdown_file_path: str) -> None:
    """
    Entry point called from main.py::OnEvent() when a .md file is added.

    :param markdown_file_path: Path to the markdown file (string from C++).
    """

    markdown_path = Path(markdown_file_path)

    if not markdown_path.is_file():
        log_warn(f"chunk_markdown_if_needed: path is not a file: {markdown_file_path}")
        return

    try:
        max_file_size_bytes = _load_max_file_size_bytes()
    except Exception as exception:
        log_error(f"Failed to load max file size from config.json: {exception}")
        # Fail-safe: do nothing if config cannot be read
        return

    try:
        file_size_bytes = markdown_path.stat().st_size
    except OSError as exception:
        log_error(f"Failed to get file size for {markdown_file_path}: {exception}")
        return

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

    try:
        markdown_text = read_text_file(str(markdown_path))
    except Exception as exception:
        log_error(f"Failed to read markdown file {markdown_file_path}: {exception}")
        return

    chunks = _create_chunks(markdown_text, max_file_size_bytes)

    if not chunks:
        log_warn(f"No chunks produced for {markdown_file_path}; this should not happen.")
        return

    chunks_folder = markdown_path.with_name(markdown_path.name + "_chunks")
    try:
        # Do NOT delete anything; just create the folder if needed.
        chunks_folder.mkdir(exist_ok=True)
    except OSError as exception:
        log_error(f"Failed to create chunks folder {chunks_folder}: {exception}")
        return

    log_info(
        f"Writing {len(chunks)} chunks to folder: {chunks_folder}"
    )

    # Overwrite existing files with the same names, but do NOT delete others.
    for chunk_index, chunk_text in enumerate(chunks, start=1):
        chunk_filename = f"chunk_{chunk_index:03d}.md"
        chunk_path = chunks_folder / chunk_filename
        try:
            write_text_file(str(chunk_path), chunk_text)
        except Exception as exception:
            log_error(f"Failed to write chunk {chunk_path}: {exception}")

    # Optional: warn if there might be stale extra chunk files.
    existing_chunk_files = sorted(chunks_folder.glob("chunk_*.md"))
    if len(existing_chunk_files) > len(chunks):
        log_warn(
            f"Chunk folder {chunks_folder} contains {len(existing_chunk_files)} chunk_XXX.md files, "
            f"but only {len(chunks)} were written in this run. "
            f"Older extra chunks were left untouched (no deletions by design)."
        )

    log_info(f"Markdown chunking completed for {markdown_file_path}")


# ----------------------------------------------------------------------
# Internal helpers
# ----------------------------------------------------------------------

def _load_max_file_size_bytes() -> int:
    """
    Load the 'max file size in kB' setting from config.json and return bytes.

    We locate config.json by going two directories up from this file:
        md_chunker.py -> helpers/ -> scripts/ -> project root/

    :return: max file size in bytes
    """
    this_file = Path(__file__).resolve()
    project_root = this_file.parents[2]  # .../scripts/helpers/md_chunker.py -> project root
    config_path = project_root / "config.json"

    if not config_path.is_file():
        raise FileNotFoundError(f"config.json not found at {config_path}")

    with config_path.open("r", encoding="utf-8") as config_file:
        config = json.load(config_file)

    # Default to 24 kB if the field is missing; you can adjust this if needed.
    max_file_size_kb = config.get("max file size in kB", 24)
    if not isinstance(max_file_size_kb, (int, float)):
        raise ValueError(
            f"Invalid 'max file size in kB' value in config.json: {max_file_size_kb}"
        )

    max_file_size_bytes = int(max_file_size_kb * 1024)
    return max_file_size_bytes


def _create_chunks(markdown_text: str, max_chunk_size_bytes: int) -> List[str]:
    """
    Split the markdown text into chunks, each <= max_chunk_size_bytes.

    Strategy:
    1. Split by headings (#, ##, ###, ...) using a regex that keeps the headings.
    2. Try to keep each section intact within a chunk.
    3. If a section alone is too large, split it further by paragraphs.
    4. If a paragraph is still too large, split it by UTF-8 byte size.

    :param markdown_text: original markdown file content
    :param max_chunk_size_bytes: maximum allowed chunk size in bytes
    :return: list of chunk strings
    """

    sections = _split_into_sections(markdown_text)

    chunks: List[str] = []
    current_parts: List[str] = []
    current_size_bytes: int = 0

    for section in sections:
        section_bytes = section.encode("utf-8")
        section_size = len(section_bytes)

        # If the whole section is larger than the chunk size,
        # we need to split the section itself.
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
            # Section fits as a whole.
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
    """
    Split markdown text into sections based on headings.

    We use a zero-width lookahead regex that matches just before any line
    starting with 1–6 '#' characters followed by a space, e.g.:

        # Title
        ## Subtitle
        ### Sub-subtitle

    This keeps the heading line inside each section.

    :param markdown_text: original markdown
    :return: list of sections
    """
    # (?m) enables multiline mode so ^ matches the beginning of each line.
    heading_pattern = r"(?m)(?=^#{1,6}\s+)"
    raw_sections = re.split(heading_pattern, markdown_text)

    sections: List[str] = []
    for section in raw_sections:
        if section.strip() == "":
            continue
        sections.append(section)

    return sections if sections else [markdown_text]


def _split_large_section(section_text: str, max_chunk_size_bytes: int) -> List[str]:
    """
    Split a single large section into smaller parts.

    Strategy:
    1. Try splitting by paragraphs separated by blank lines.
    2. If a paragraph is still too large, split it by byte size with a
       UTF-8-aware helper.

    :param section_text: section that is larger than max_chunk_size_bytes
    :param max_chunk_size_bytes: maximum allowed chunk size in bytes
    :return: list of smaller pieces that each fit into a chunk
    """

    parts: List[str] = []

    # Split into paragraphs, keeping the separators (blank lines) so that
    # we do not accidentally glue all paragraphs together.
    paragraph_pattern = r"(\n\s*\n+)"
    paragraph_pieces = re.split(paragraph_pattern, section_text)

    current_parts: List[str] = []
    current_size_bytes: int = 0

    for piece in paragraph_pieces:
        piece_bytes = piece.encode("utf-8")
        piece_size = len(piece_bytes)

        if piece_size > max_chunk_size_bytes:
            # Flush what we have so far.
            if current_parts:
                parts.append("".join(current_parts))
                current_parts = []
                current_size_bytes = 0

            # Now split this oversized piece directly by byte length.
            oversized_chunks = _split_by_bytes_utf8_safe(piece, max_chunk_size_bytes)
            parts.extend(oversized_chunks)
            continue

        # Normal paragraph piece
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
    """
    Hard-split text by byte length, making sure we only cut at valid
    UTF-8 boundaries.

    This is a last-resort fallback when even a paragraph does not fit into
    a single chunk.

    :param text: input string
    :param max_chunk_size_bytes: maximum bytes per piece
    :return: list of pieces, each safely decodable as UTF-8
    """

    encoded = text.encode("utf-8")
    length = len(encoded)
    offset = 0
    pieces: List[str] = []

    while offset < length:
        # Tentative end index
        end = min(offset + max_chunk_size_bytes, length)
        chunk_bytes = encoded[offset:end]

        # Move back until we can decode a valid UTF-8 sequence.
        while True:
            try:
                decoded = chunk_bytes.decode("utf-8")
                break
            except UnicodeDecodeError:
                # Shorten by one byte and try again.
                chunk_bytes = chunk_bytes[:-1]
                if not chunk_bytes:
                    # If we somehow cannot decode even a single byte,
                    # fall back to skipping one byte (very unlikely).
                    offset += 1
                    break
        else:
            # Only reached if we hit "break" in the inner while with empty chunk_bytes.
            continue

        pieces.append(decoded)
        offset += len(chunk_bytes)

    return pieces
