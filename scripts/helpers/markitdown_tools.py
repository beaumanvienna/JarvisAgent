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
from pathlib import Path

from helpers.log import log_info, log_warn, log_error


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

        if not result.stdout.strip():
            log_warn(f"MarkItDown returned empty output for {input_path}")
            md_text = f"# WARNING\nMarkItDown produced empty output for:\n{input_path}"
        else:
            md_text = result.stdout

        md_path.write_text(md_text, encoding="utf-8")

        log_info(f"Converted successfully → {md_path}")
        return md_path

    except FileNotFoundError:
        log_error(
            "markitdown CLI not found. Install it using:\n"
            "    pipx install markitdown[pdf]"
        )
        md_path.write_text(
            f"# ERROR: markitdown missing\nCould not convert: {input_path}",
            encoding="utf-8",
        )
        return md_path

    except Exception as exception:
        log_error(f"Conversion FAILED for {input_path}: {exception}")
        md_path.write_text(
            f"# ERROR converting file\n\n{input_path}\n\nError:\n```\n{exception}\n```",
            encoding="utf-8",
        )
        return md_path
