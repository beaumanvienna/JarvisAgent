#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
MarkItDown PDF Conversion Tools
-------------------------------

Provides PDF → Markdown conversion 

This module is used by scripts/main.py through OnEvent().

Copyright (c) 2025 JC Technolabs
License: GPL-3.0

"""

import os
import subprocess

from helpers.log import log_info, log_warn, log_error


def convert_pdf_to_markdown(pdf_path: str, md_path: str = None) -> str:
    """
    Convert a PDF file to Markdown using Microsoft's MarkItDown tool.

    - Skips conversion when the .md file already exists AND is newer than the PDF
    - Returns the output .md path (created or reused)
    """

    if md_path is None:
        md_path = pdf_path + ".md"

    # ----------------------------
    # Skip conversion if up-to-date
    # ----------------------------
    if os.path.exists(md_path):
        try:
            pdf_mtime = os.path.getmtime(pdf_path)
            md_mtime = os.path.getmtime(md_path)

            if md_mtime >= pdf_mtime:
                log_info(f"Skipping PDF conversion (already up-to-date): {md_path}")
                return md_path
        except Exception as e:
            log_warn(f"Timestamp check failed for {pdf_path}: {e}")
            # if timestamps fail, we fall through and re-convert

    # ----------------------------
    # Perform conversion
    # ----------------------------
    log_info(f"Running MarkItDown on: {pdf_path}")

    try:
        result = subprocess.run(
            ["markitdown", pdf_path],
            capture_output=True,
            text=True
        )

        if result.returncode != 0:
            log_error(f"MarkItDown failed: {result.stderr.strip()}")
            raise RuntimeError(f"MarkItDown error: {result.stderr}")

        # Write the markdown output manually
        with open(md_path, "w", encoding="utf-8") as f:
            f.write(result.stdout)

        log_info(f"PDF converted successfully: {md_path}")
        return md_path

    except FileNotFoundError:
        log_error(
            "markitdown not found. Install it using: "
            "pipx install markitdown[pdf]"
        )
        # Write a placeholder md file so JarvisAgent continues normally
        with open(md_path, "w", encoding="utf-8") as f:
            f.write(f"# ERROR: markitdown missing\nCould not convert {pdf_path}.")
        return md_path

    except Exception as e:
        log_error(f"Unexpected PDF conversion error: {e}")
        raise
