# mailpitSmtpDemo Workflow -- SMTP Send + HTTP API Verification

## Executive Summary

The **mailpitSmtpDemo** workflow validates SMTP send via the `my-email`
(mailpit) connection by sending a fixed test message, then verifying it
landed by querying mailpit's HTTP API.

This demo exists to close a test-coverage gap: every other configured cloud
connection had a JCWF exercising it (per the `confirmed_healthy` panel in
the dashboard), but `my-email` did not -- only `my-greenmail` was covered
by `emailDemo`, because `email_read` requires IMAP and mailpit is SMTP-only.

---

## Prerequisites

1. mailpit running locally on its default ports:

| Port | Purpose |
|------|---------|
| `1025` | SMTP listener |
| `8025` | Web UI + HTTP API |

For local testing:
```bash
docker run -d --name mailpit -p 1025:1025 -p 8025:8025 axllent/mailpit
```

mailpit web UI: http://localhost:8025

2. A CloudConnection named `my-email` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `email` |
| Key | `mailpit` (any KeyManager credential -- mailpit accepts any auth by default) |
| Auth Type | `basic_auth` |
| SMTP Host | `localhost` |
| SMTP Port | `1025` |
| From | `j9t-demo@localhost` |
| Use SSL | `false` (mailpit is plaintext for local testing) |

Note: mailpit has no IMAP, so this demo intentionally only exercises
`email_send`. For full IMAP+SMTP round-trip see `emailDemo` (greenmail).

---

## Pipeline Overview

```
+---------------------+     +------------------------+
|  send_email         | --> |  verify_received       |
|  email_send         |     |  python                |
|  (01_send)          |     |  (02_verify)           |
+---------------------+     +------------------------+
       |                              |
       v                              v
   mailpit:1025               GET mailpit:8025/api/v1/messages
   (SMTP)                     match Subject = "[mailpitSmtpDemo] hello"
```

---

## Trigger

Manual trigger only.

---

## Task Details

### 1. send_email -- send fixed test message via mailpit

Connects to mailpit's SMTP listener and sends a single email with a
deterministic subject so the verifier can find it.

| Field | Value |
|-------|-------|
| Type | `email_send` |
| Connection | `my-email` |
| To | `test@localhost` |
| Subject | `[mailpitSmtpDemo] hello` |
| Body | inline (no `body_file`) |

### 2. verify_received -- assert the message landed

Calls mailpit's HTTP API at `http://localhost:8025/api/v1/messages` and
checks that the most recent page contains a message whose `Subject`
matches the expected string. Writes a one-line summary to
`verification.txt` (sender + message id + subject); empty match is treated
as task failure.

| Field | Value |
|-------|-------|
| Type | `python` |
| Module | `verifyMailpitMessage` |
| Function | `verify` |
| Outputs | `verification.txt` (matched message summary) |

The verifier script is `scripts/verifyMailpitMessage.py` and follows the
standard `# @jarvis-script` registration convention.

---

## Task Type Reference

### email_send

See `emailDemo.md` -- same task type, same params; this demo just uses
fewer of them (no `body_file`, no `cc`, no `attachments`).

### python (workflow registration)

| Param | Required | Description |
|-------|----------|-------------|
| `module` | yes | Script's module name (filename minus `.py`); the script must live under `scripts/` and carry a `# @jarvis-script` header to be registered |
| `function` | yes | Callable inside the module; receives `context=dict` with `_task_working_directory` and any `_file_input_N` paths |

Returning `{"error": "..."}` from the function fails the task (surfaced as
`lastErrorMessage`); `{"ok": True}` (or any non-error dict) succeeds.

---

## Testing the Round-Trip

```bash
# 1. Start mailpit (if not already running)
docker run -d --name mailpit -p 1025:1025 -p 8025:8025 axllent/mailpit

# 2. Run the workflow (via UI or API)
curl -sk -X POST -H "Authorization: Bearer $J9T_TOKEN" \
  -H "Content-Type: application/json" --data '{}' \
  https://localhost:8443/api/workflows/mailpitSmtpDemo/run

# 3. Verify the message in mailpit web UI:
#    http://localhost:8025

# 4. Or check the verification artifact:
cat workflows/mailpitSmtpDemo/02_verify/verification.txt
# Expected: Verified: id=... from=j9t-demo@localhost subject=[mailpitSmtpDemo] hello
```

---

## Key Concepts Demonstrated

- **SMTP-only email connection** -- `my-email` (mailpit) covers the SMTP
  send path that `my-greenmail` also covers, but with a separate
  connection record so both are exercised by mass-runs
- **External-API verification via python task** -- a plain `urllib`
  request to a sibling docker container's REST API, no extra Python deps
- **Closing a test-coverage gap** -- before this demo, `my-email` showed
  `confirmed_healthy: false` after the mass-run because no JCWF was
  pinging it; only manual dashboard tests would mark it healthy
