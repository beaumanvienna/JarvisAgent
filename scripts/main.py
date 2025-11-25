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
import traceback

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
# Notify C++ about Python error states → triggers "Python Offline" UI
# --------------------------------------------------------------------
class _JarvisPyStatus:
    def __init__(self):
        try:
            C = ctypes.CDLL(None)
            C.JarvisPyStatus.argtypes = [ctypes.c_char_p]
            C.JarvisPyStatus.restype = None
            self._send = C.JarvisPyStatus
        except Exception as exception:
            # Hard-stop: cannot report errors → engine must shut down
            log_error(f"Failed to initialize JarvisPyStatus: {exception}")
            self._send = None

    def send(self, msg: str):
        if self._send is None:
            # Hard-stop: error reporting path broken
            log_error("JarvisPyStatus.send() called but C callback is missing")
            return
        try:
            self._send(msg.encode("utf-8"))
        except Exception as exception:
            log_error(f"JarvisPyStatus.send() failed: {exception}")


pystatus = _JarvisPyStatus()


def notify_python_error(message: str):
    """Send error state to C++ and log it."""
    log_error(message)
    pystatus.send(message)


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
        except Exception as exception:
            notify_python_error(f"Failed to initialize JarvisRedirect: {exception}")
            self._redirect = None

    def write(self, msg):
        if not msg:
            return

        if self._redirect is None:
            notify_python_error("JarvisRedirect.write() called but redirect function is missing")
            return

        try:
            self._redirect(msg.encode("utf-8"))
        except Exception as exception:
            notify_python_error(f"JarvisRedirect.write() failed: {exception}")

    def flush(self):
        pass


redir = _JarvisRedirect()
sys.stdout = redir
sys.stderr = redir


# --------------------------------------------------------------------
# Global exception hook — catches ANY unhandled Python exception
# --------------------------------------------------------------------
def _global_exception_hook(exc_type, exc, tb):
    details = "".join(traceback.format_exception(exc_type, exc, tb))
    notify_python_error("Unhandled Python exception:\n" + details)


sys.excepthook = _global_exception_hook


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
            notify_python_error(f"Conversion failed for {file_path}: {exception}")
        return

    # ------------------------------------------------------------
    # COMBINE CHUNK OUTPUT FILES
    # ------------------------------------------------------------
    if event_type == "FileAdded" and CHUNK_OUTPUT_REGEX.match(file_name):
        try:
            handle_chunk_output_added(file_path)
        except Exception as exception:
            notify_python_error(f"Chunk combining failed for {file_path}: {exception}")
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
            notify_python_error(f"Markdown chunking failed for {file_path}: {exception}")
        return


def OnShutdown():
    log_info("Python OnShutdown() called.")
