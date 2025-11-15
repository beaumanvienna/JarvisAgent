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
- Auto-tagging or classification logic

Copyright (c) 2025 JC Technolabs

"""

from helpers.log import log_info, log_warn, log_error
from helpers.fileutils import is_pdf, is_binary_file, read_text_file, write_text_file
from helpers.markitdown_tools import convert_pdf_to_markdown

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

    # ----------------------
    # AUTO-CONVERT PDF FILES
    # ----------------------
    if event_type == "FileAdded" and is_pdf(file_path):
        log_info(f"PDF detected: {file_path}")
        try:
            md_path = convert_pdf_to_markdown(file_path)
            log_info(f"PDF → Markdown ready: {md_path}")

        except Exception as e:
            log_error(f"PDF conversion failed for {file_path}: {e}")

    # ------------------------
    # IGNORE binary input files
    # ------------------------
    if event_type == "FileAdded" and is_binary_file(file_path):
        log_warn(f"Ignoring binary file: {file_path}")
        return

    return


# Called once when the C++ application is shutting down
def OnShutdown():
    log_info("Python OnShutdown() called.")
