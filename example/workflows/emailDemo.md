# emailDemo Workflow -- Email Round-Trip (IMAP Read + AI + SMTP Reply)

## Executive Summary

The **emailDemo** workflow demonstrates a full email round-trip: read an
incoming email from an IMAP mailbox, have AI generate a reply, and send
the reply back via SMTP.

This is the first workflow to combine both `email_read` (IMAP) and
`email_send` (SMTP) task types in a single pipeline.

---

## Prerequisites

1. A mail server with both SMTP and IMAP access
2. A CloudConnection named `my-greenmail` (or `my-email`) configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `email` |
| Key | A KeyManager credential with email address + password |
| Auth Type | `basic_auth` |
| SMTP Host | `localhost` |
| SMTP Port | `3025` |
| IMAP Host | `localhost` |
| IMAP Port | `3143` |
| From | `test@greenmail.com` |
| Use SSL | `false` (for local testing) |

For local testing with GreenMail:
```bash
docker run -d --name greenmail -p 3025:3025 -p 3143:3143 -p 8080:8080 \
  -e GREENMAIL_OPTS='-Dgreenmail.setup.test.all -Dgreenmail.hostname=0.0.0.0 -Dgreenmail.users=test:test' \
  greenmail/standalone
```

GreenMail web UI: http://localhost:8080

---

## Pipeline Overview

```
+-----------------+     +-----------------+     +-----------------+
|  fetch_email    | --> |  ai_reply       | --> |  send_reply     |
|  email_read     |     |  ai_call        |     |  email_send     |
|  (01_fetch)     |     |  (02_ai_reply)  |     |  (03_reply)     |
+-----------------+     +-----------------+     +-----------------+
```

---

## Trigger

Manual trigger only -- will not start at j9t startup.

---

## Task Details

### 1. fetch_email -- read incoming email from IMAP

Connects to the IMAP server and fetches the most recent email from INBOX.

| Field | Value |
|-------|-------|
| Type | `email_read` |
| Connection | `my-greenmail` |
| Folder | `INBOX` |
| Max messages | `1` |
| Output | `emails_summary.json` (array of fetched messages) |

### 2. ai_reply -- AI generates a reply

The fetched email content is provided as context. AI generates a professional
reply addressing the sender's request.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Context | `emails_summary.json` from fetch_email |
| Role | Helpful project assistant |
| Output | Plain text reply content |

### 3. send_reply -- send the AI reply via SMTP

Sends the AI-generated reply back to the original sender.

| Field | Value |
|-------|-------|
| Type | `email_send` |
| Connection | `my-greenmail` |
| To | `sender@example.com` |
| Subject | `Re: Weekly Report Request` |
| Body | Read from AI output file via `body_file` |

---

## Task Type Reference

### email_read

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | -- | Named CloudConnection with `imap_host`/`imap_port` |
| `folder` | no | `INBOX` | IMAP folder to read from |
| `max_messages` | no | `10` | Maximum messages to fetch |
| `subject_filter` | no | -- | Only include messages whose subject contains this string |

Outputs:
- `emails_summary.json` -- array of `{uid, from, to, subject, date, body}`
- `response.json` -- `{ok, count, folder}`

### email_send

| Param | Required | Description |
|-------|----------|-------------|
| `connection` | yes | Named CloudConnection with `smtp_host`/`smtp_port` |
| `to` | yes | Recipient(s), comma-separated |
| `subject` | yes | Email subject |
| `body` | yes* | Email body text |
| `body_file` | no | Path to file whose contents become the body (takes precedence over `body`) |
| `cc` | no | CC recipients, comma-separated |
| `attachments` | no | Array of file paths relative to working directory |

*Required unless `body_file` is provided.

---

## Testing the Round-Trip

```bash
# 1. Start GreenMail
docker run -d --name greenmail -p 3025:3025 -p 3143:3143 -p 8080:8080 \
  -e GREENMAIL_OPTS='-Dgreenmail.setup.test.all -Dgreenmail.hostname=0.0.0.0 -Dgreenmail.users=test:test' \
  greenmail/standalone

# 2. Send a test email to GreenMail
curl -s smtp://localhost:3025 --user "test:test" \
  --mail-from "sender@example.com" --mail-rcpt "test@greenmail.com" \
  -T - <<< "From: sender@example.com
To: test@greenmail.com
Subject: Weekly Report Request

Please generate the weekly status report for j9t Cloud Integration project."

# 3. Run the workflow (via UI or API)

# 4. Verify the reply in GreenMail web UI:
#    http://localhost:8080/api/user/sender@example.com/messages/INBOX
```

---

## Key Concepts Demonstrated

- **Email round-trip** -- IMAP read + AI processing + SMTP reply in one workflow
- **email_read task type** -- IMAP fetch via libcurl with SEARCH and UID-based retrieval
- **email_send with body_file** -- reply body read from AI output file
- **SMTP/IMAP via libcurl** -- native protocol support, no external mail library
- **Named connections** -- SMTP host, IMAP host, credentials centralized in Connections tab
- **S3-compatible local testing** -- GreenMail Docker provides SMTP + IMAP with test credentials
