#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
MarkItDown CLI Conversion Tools
-------------------------------

Uses the MarkItDown CLI tool (NOT the Python module).

Supports any file type that MarkItDown can handle:
- PDF
- DOCX/DOC
- XLS/XLSX/CSV
- PPT/PPTX
- Images (OCR/EXIF)
- HTML
- ZIP
- Audio
- EPUB

Copyright (c) 2025 JC Technolabs
License: GPL-3.0
"""

import os
import subprocess
import ctypes
from pathlib import Path

from helpers.log import log_info, log_warn, log_error


# --------------------------------------------------------------------
# Local Python → C++ error notifier (avoids circular imports)
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

    def send(self, message: str):
        if self._send is None:
            return
        try:
            self._send(message.encode("utf-8"))
        except Exception:
            pass


_pystatus = _JarvisPyStatusHelper()


def _notify_markitdown_error(message: str):
    log_error(message)
    try:
        _pystatus.send(message)
    except Exception:
        pass


# --------------------------------------------------------------------
# Conversion function
# --------------------------------------------------------------------
def convert_with_markitdown(input_path: str) -> Path:
    """
    Convert any supported file into Markdown via MarkItDown CLI.
    Always uses:
        markitdown <input>
    """

    input_path = str(input_path)
    md_path = Path(input_path).with_suffix(".md")

    # ------------------------------------------------------
    # Skip when output exists and is newer
    # ------------------------------------------------------
    if md_path.exists():
        try:
            if md_path.stat().st_mtime >= Path(input_path).stat().st_mtime:
                log_info(f"Skipping conversion (up-to-date): {md_path}")
                return md_path
        except Exception as e:
            # Non-fatal warning — unchanged
            log_warn(f"Timestamp check failed for {input_path}: {e}")

    log_info(f"Running MarkItDown CLI for: {input_path}")

    try:
        result = subprocess.run(
            ["markitdown", input_path],
            capture_output=True,
            text=True,
        )

        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip())

        # Empty markdown output is NOT an engine-stop condition
        if not result.stdout.strip():
            log_warn(f"MarkItDown returned empty output for {input_path}")
            md_text = f"# WARNING\nMarkItDown produced empty output for:\n{input_path}"
        else:
            md_text = result.stdout

        md_path.write_text(md_text, encoding="utf-8")
        log_info(f"Converted successfully → {md_path}")
        return md_path

    except FileNotFoundError:
        # This is a real failure → notify C++
        _notify_markitdown_error(
            f"markitdown CLI not found. Install using:\n    pipx install markitdown[pdf]"
        )
        md_path.write_text(
            f"# ERROR: markitdown missing\nCould not convert: {input_path}",
            encoding="utf-8",
        )
        return md_path

    except Exception as exception:
        # This previously swallowed the exception — now reports to C++
        _notify_markitdown_error(f"Conversion FAILED for {input_path}: {exception}")

        md_path.write_text(
            f"# ERROR converting file\n\n{input_path}\n\nError:\n```\n{exception}\n```",
            encoding="utf-8",
        )
        return md_path
