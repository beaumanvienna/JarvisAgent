#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
JarvisAgent Python Scripting Layer
----------------------------------

Automation hooks called directly from the C++ PythonEngine:

    OnStart()
    OnUpdate()
    OnEvent(event)
    OnShutdown()

Implements:
- PDF → Markdown conversion
- Markdown chunking
- Chunk-output combining
- STNG/CNTX/TASK preprocessing (future expansion)

Copyright (c) 2025 JC Technolabs
"""

from pathlib import Path
import re
import sys
import ctypes

from helpers.log import log_info, log_warn, log_error
from helpers.fileutils import is_pdf
from helpers.markitdown_tools import convert_pdf_to_markdown
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
            pass  # Silent fail — avoid recursion or loops

    def flush(self):
        pass


redir = _JarvisRedirect()
sys.stdout = redir
sys.stderr = redir
# --------------------------------------------------------------------


# Detect chunk output files
CHUNK_OUTPUT_REGEX = re.compile(r"^chunk_(\d+)\.output\.md$")


def is_md_up_to_date(markdown_path: Path) -> bool:
    """
    Returns True if <markdown>.output.md exists and is newer.
    """
    if not markdown_path.name.endswith(".md"):
        return False

    output_path = markdown_path.with_suffix(".output.md")
    if not output_path.exists():
        return False

    try:
        return output_path.stat().st_mtime >= markdown_path.stat().st_mtime
    except OSError:
        return False


# --------------------------------------------------------------------
# Hook implementations
# --------------------------------------------------------------------

def OnStart():
    log_info("Python OnStart() called.")


def OnUpdate():
    # light-weight; runs ~60 times per second
    return


def OnEvent(event):
    """
    event is a dict passed from C++:
        {
            "type": "FileAdded",
            "path": "../queue/STNG_xxx.txt"
        }
    """

    event_type = event.get("type")
    file_path = event.get("path", "")
    file_name = Path(file_path).name

    # ------------------------------------------------------------
    # AUTO-CONVERT PDF → MARKDOWN
    # ------------------------------------------------------------
    if event_type == "FileAdded" and is_pdf(file_path):
        log_info(f"PDF detected: {file_path}")
        try:
            markdown_path = convert_pdf_to_markdown(file_path)
            log_info(f"PDF → Markdown ready: {markdown_path}")
        except Exception as exception:
            log_error(f"PDF conversion failed for {file_path}: {exception}")
        return

    # ------------------------------------------------------------
    # COMBINE CHUNK OUTPUT FILES
    # ------------------------------------------------------------
    if event_type == "FileAdded" and CHUNK_OUTPUT_REGEX.match(file_name):
        try:
            handle_chunk_output_added(file_path)
        except Exception as exception:
            log_error(f"Chunk output combining failed for {file_path}: {exception}")
        return

    # ------------------------------------------------------------
    # CHUNK LARGE MARKDOWN FILES
    # ------------------------------------------------------------
    if event_type == "FileAdded" and file_path.endswith(".md"):

        # skip final combined results
        if file_name.endswith(".output.md"):
            return

        # skip chunk outputs (already handled)
        if CHUNK_OUTPUT_REGEX.match(file_name):
            return

        # skip if processed and up to date
        md_path = Path(file_path)
        if is_md_up_to_date(md_path):
            log_info(f"Markdown already processed — skipping: {file_path}")
            return

        # perform chunking
        log_info(f"Markdown file detected for potential chunking: {file_path}")
        try:
            chunk_markdown_if_needed(file_path)
        except Exception as exception:
            log_error(f"Markdown chunking failed for {file_path}: {exception}")

        return


def OnShutdown():
    log_info("Python OnShutdown() called.")
