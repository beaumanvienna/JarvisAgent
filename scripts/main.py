#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
JarvisAgent Python Scripting Layer
----------------------------------

Supports:
- Document conversion using MarkItDown CLI (PDF/DOCX/XLSX/PPTX/etc.)
- Markdown chunking
- Chunk-output combining
- Future STNG/CNTX/TASK preprocessing

Copyright (c) 2025 JC Technolabs
License: GPL-3.0

"""

from pathlib import Path
import re
import sys
import ctypes

from helpers.log import log_info, log_warn, log_error
from helpers.fileutils import (
    is_pdf,
    is_docx,
    is_xlsx,
    is_pptx,
)
from helpers.markitdown_tools import convert_with_markitdown
from helpers.md_chunker import chunk_markdown_if_needed
from helpers.chunk_combiner import handle_chunk_output_added


# --------------------------------------------------------------------
# Redirect Python stdout/stderr → C++ log (JarvisRedirect)
# --------------------------------------------------------------------
class _JarvisRedirect:
    def __init__(self):
        try:
            C = ctypes.CDLL(None)
            C.JarvisRedirect.argtypes = [ctypes.c_char_p]
            C.JarvisRedirect.restype = None
            self._redirect = C.JarvisRedirect
        except Exception:
            self._redirect = None

    def write(self, msg):
        if not msg or self._redirect is None:
            return
        try:
            self._redirect(msg.encode("utf-8"))
        except Exception:
            pass

    def flush(self):
        pass


redir = _JarvisRedirect()
sys.stdout = redir
sys.stderr = redir


# --------------------------------------------------------------------
# Regex for chunk output files
# --------------------------------------------------------------------
CHUNK_OUTPUT_REGEX = re.compile(r"^chunk_(\d+)\.output\.md$")


def is_md_up_to_date(markdown_path: Path) -> bool:
    """Return True if foo.output.md exists and is newer."""
    if not markdown_path.name.endswith(".md"):
        return False

    out_path = markdown_path.with_suffix(".output.md")
    if not out_path.exists():
        return False

    try:
        return out_path.stat().st_mtime >= markdown_path.stat().st_mtime
    except OSError:
        return False


# --------------------------------------------------------------------
# Hook implementations
# --------------------------------------------------------------------
def OnStart():
    log_info("Python OnStart() called.")


def OnUpdate():
    # Disabled — not used anymore
    return


def OnEvent(event):

    event_type = event.get("type")
    file_path = event.get("path", "")
    file_name = Path(file_path).name

    # ------------------------------------------------------------
    # DOCUMENT CONVERSION (PDF, DOCX, XLSX, PPTX)
    # ------------------------------------------------------------
    if event_type == "FileAdded" and (
        is_pdf(file_path)
        or is_docx(file_path)
        or is_xlsx(file_path)
        or is_pptx(file_path)
    ):
        log_info(f"Document detected: {file_path}")
        try:
            md_path = convert_with_markitdown(file_path)
            log_info(f"Converted → Markdown: {md_path}")
        except Exception as exception:
            log_error(f"Conversion failed for {file_path}: {exception}")
        return

    # ------------------------------------------------------------
    # COMBINE CHUNK OUTPUT FILES
    # ------------------------------------------------------------
    if event_type == "FileAdded" and CHUNK_OUTPUT_REGEX.match(file_name):
        try:
            handle_chunk_output_added(file_path)
        except Exception as exception:
            log_error(f"Chunk combining failed for {file_path}: {exception}")
        return

    # ------------------------------------------------------------
    # CHUNK LARGE MARKDOWN FILES
    # ------------------------------------------------------------
    if event_type == "FileAdded" and file_path.endswith(".md"):

        # skip combined results
        if file_name.endswith(".output.md"):
            return

        # skip chunk outputs
        if CHUNK_OUTPUT_REGEX.match(file_name):
            return

        md_path = Path(file_path)

        # skip if already processed
        if is_md_up_to_date(md_path):
            log_info(f"Markdown already processed — skipping: {file_path}")
            return

        log_info(f"Markdown file detected for chunking: {file_path}")
        try:
            chunk_markdown_if_needed(file_path)
        except Exception as exception:
            log_error(f"Markdown chunking failed for {file_path}: {exception}")
        return


def OnShutdown():
    log_info("Python OnShutdown() called.")
