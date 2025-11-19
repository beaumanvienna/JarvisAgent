#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
JarvisAgent Python Scripting Layer
----------------------------------

This file provides automation hooks.

These four functions will be called directly from the C++ PythonEngine:

    OnStart()
    OnUpdate()
    OnEvent(event)
    OnShutdown()

Use these hooks to implement:
- File transformers (PDF → Markdown)
- Preprocessors (rewrite STNG/CNTX/TASK)
- Specialized handlers (e.g., images / XML → text)
- Markdown chunking
- Chunk output combining

Copyright (c) 2025 JC Technolabs
"""

from pathlib import Path
import re

from helpers.log import log_info, log_warn, log_error
from helpers.fileutils import is_pdf, is_binary_file, read_text_file, write_text_file
from helpers.markitdown_tools import convert_pdf_to_markdown
from helpers.md_chunker import chunk_markdown_if_needed
from helpers.chunk_combiner import handle_chunk_output_added


# regex to detect chunk output files
CHUNK_OUTPUT_REGEX = re.compile(r"^chunk_(\d+)\.output\.md$")


# This is called once when JarvisAgent starts
def OnStart():
    log_info("Python OnStart() called.")
    # You can preload libraries, scan folder trees, etc.

# Called every C++ engine tick (≈ 60 times per second)
def OnUpdate():
    # Keep this lightweight unless there is a good reason
    return

# Called for **every event**: file add/modify/remove, engine events, chat events, etc.
def OnEvent(event):
    """
    event is a dict passed from C++:
        {
            "type": "FileAdded",
            "path": "../queue/ICE/PROB_1_12345.txt"
        }
    """

    event_type = event.get("type")
    file_path = event.get("path", "")
    file_name = Path(file_path).name

    # ----------------------
    # AUTO-CONVERT PDF FILES
    # ----------------------
    if event_type == "FileAdded" and is_pdf(file_path):
        log_info(f"PDF detected: {file_path}")
        try:
            markdown_path = convert_pdf_to_markdown(file_path)
            log_info(f"PDF → Markdown ready: {markdown_path}")
        except Exception as exception:
            log_error(f"PDF conversion failed for {file_path}: {exception}")
        return

    # ----------------------
    # HANDLE CHUNK OUTPUT FILES
    # ----------------------
    if event_type == "FileAdded" and CHUNK_OUTPUT_REGEX.match(file_name):
        try:
            handle_chunk_output_added(file_path)
        except Exception as exception:
            log_error(f"Chunk output combining failed for {file_path}: {exception}")
        return

    # ----------------------
    # CHUNK LARGE MARKDOWN FILES
    # ----------------------
    if event_type == "FileAdded" and file_path.endswith(".md"):

        # Skip combined final output files
        if file_name.endswith(".output.md"):
            log_info(f"Final output file detected — skipping chunking: {file_path}")
            return

        # Skip chunk outputs (already handled above)
        if CHUNK_OUTPUT_REGEX.match(file_name):
            return

        # Original logic
        log_info(f"Markdown file detected for potential chunking: {file_path}")
        try:
            chunk_markdown_if_needed(file_path)
        except Exception as exception:
            log_error(f"Markdown chunking failed for {file_path}: {exception}")
        return

    return


# Called once when the C++ application is shutting down
def OnShutdown():
    log_info("Python OnShutdown() called.")
