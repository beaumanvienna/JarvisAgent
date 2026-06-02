# @jarvis-script
# @short: Write a 65538-byte canary file straddling the 64 KiB output cap with a multibyte UTF-8 codepoint
# @params: (none — context-only; reads _task_working_directory)
# @description: Emits 65535 ASCII 'A' bytes followed by the 3-byte UTF-8 encoding
#   of '€' (U+20AC = E2 82 AC) to <task_working_directory>/big.txt. The € lands
#   precisely at the FireCompletionCallback/BuildCallbackPayload 64 KiB
#   per-output cap (lead byte at file offset 65535, continuations at 65536 and
#   65537). Test/hardening/test_negative_paths.py group 1.2 uses this to verify
#   that the callback payload truncates to a complete UTF-8 codepoint boundary
#   (65535 'A' bytes; the partial '€' is dropped) rather than emitting the lead
#   byte alone as invalid UTF-8.
# @outputs: big.txt — 65538 bytes (65535 'A' + UTF-8 '€')
"""Output-cap UTF-8 canary helper for test_negative_paths.py group 1.2."""

import os


def write_canary(context=None, **kwargs):
    if context is None:
        return {"error": "no context provided"}

    out_dir = context.get("_task_working_directory", ".")
    out_path = os.path.join(out_dir, "big.txt")
    payload = b"A" * 65535 + "€".encode("utf-8")  # 65535 + 3 = 65538 bytes
    with open(out_path, "wb") as f:
        f.write(payload)
    # Return the "content" slot as the abs file path so the callback-payload
    # reader (BuildCallbackPayload) opens this file and embeds up to 64 KiB
    # of its content per the per-output cap.
    return {"content": os.path.abspath(out_path)}
