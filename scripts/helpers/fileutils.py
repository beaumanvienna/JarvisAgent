import os

"""
Helpers for file detection.
Copyright (c) 2025 JC Technolabs
License: GPL-3.0
"""


def is_pdf(path):
    return path.lower().endswith(".pdf")


def is_docx(path):
    lower = path.lower()
    return lower.endswith(".docx") or lower.endswith(".doc")


def is_xlsx(path):
    lower = path.lower()
    return (
        lower.endswith(".xlsx")
        or lower.endswith(".xls")
        or lower.endswith(".csv")
    )


def is_pptx(path):
    lower = path.lower()
    return lower.endswith(".pptx") or lower.endswith(".ppt")


def is_binary_file(path, block_size=512):
    try:
        with open(path, "rb") as f:
            chunk = f.read(block_size)
            return b"\0" in chunk
    except Exception:
        return False


def read_text_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def write_text_file(path, content):
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
