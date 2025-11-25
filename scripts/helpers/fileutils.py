import os
import ctypes

"""
Helpers for file detection.
Copyright (c) 2025 JC Technolabs
License: GPL-3.0
"""


# --------------------------------------------------------------------
# Python → C++ status callback (identical to main.py infrastructure)
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


def _notify_fileutils_error(message: str):
    """
    Local emergency error reporter.
    Does not import main.py (avoids circular import).
    Simply informs C++ → engine stops.
    """
    try:
        _pystatus.send(message)
    except Exception:
        pass


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
    except Exception as exception:
        # Previously silent failure → now a hard-stop error in development mode
        _notify_fileutils_error(f"fileutils.is_binary_file failed for '{path}': {exception}")
        return False


def read_text_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def write_text_file(path, content):
    try:
        with open(path, "w", encoding="utf-8") as file:
            file.write(content)
    except Exception as exception:
        # Hard-fail: writing output files must never silently fail in dev mode.
        _notify_fileutils_error(
            f"fileutils.write_text_file failed for '{path}': {exception}"
        )
        raise

