# @jarvis-script
# @short: Verify the most recent mailpit message matches the expected subject
# @params: (none — context-only; reads MAILPIT_API + EXPECTED_SUBJECT constants)
# @description: Hits mailpit's HTTP API at localhost:8025 and confirms the most
#   recent message has the expected [mailpitSmtpDemo] subject. Used by the
#   mailpitSmtpDemo workflow to validate end-to-end SMTP send via the my-email
#   (mailpit) connection. Writes a one-line summary to verification.txt.
# @outputs: verification.txt — message id + from + subject of the matched mail
"""Verify a mailpit message landed end-to-end after an email_send task.

Called as a python task with file_outputs=["verification.txt"]. Returns
{"ok": True} on success or {"error": "..."} on any failure path; the j9t
runtime treats a non-empty error as task failure.
"""

import json
import os
import urllib.request


MAILPIT_API = "http://localhost:8025/api/v1/messages"
EXPECTED_SUBJECT = "[mailpitSmtpDemo] hello"


def verify(context=None, **kwargs):
    if context is None:
        return {"error": "no context provided"}

    output_dir = context.get("_task_working_directory", ".")

    try:
        request = urllib.request.Request(MAILPIT_API)
        with urllib.request.urlopen(request, timeout=10) as response:
            payload = json.load(response)
    except Exception as exc:
        return {"error": f"mailpit API request failed: {exc}"}

    messages = payload.get("messages", [])
    match = next((m for m in messages if m.get("Subject") == EXPECTED_SUBJECT), None)
    if not match:
        return {"error": f"no mailpit message with subject '{EXPECTED_SUBJECT}' "
                         f"(checked {len(messages)} most-recent)"}

    from_addr = (match.get("From") or {}).get("Address", "<unknown>")
    summary = (f"Verified: id={match.get('ID')} from={from_addr} "
               f"subject={match.get('Subject')}\n")

    output_path = os.path.join(output_dir, "verification.txt")
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(summary)

    return {"ok": True}
