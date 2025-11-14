
"""

Copyright (c) 2025 JC Technolabs

"""

import datetime

def _timestamp():
    return datetime.datetime.now().strftime("%H:%M:%S")

def log_info(message):
    print(f"[PY][INFO { _timestamp() }] {message}")

def log_warn(message):
    print(f"[PY][WARN { _timestamp() }] {message}")

def log_error(message):
    print(f"[PY][ERR  { _timestamp() }] {message}")
