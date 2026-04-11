# emailSendDemo Workflow -- Email SMTP Integration

## Executive Summary

The **emailSendDemo** workflow demonstrates how JarvisAgent sends emails with attachments via **SMTP** using the `email_send` task type and libcurl.

---

## Prerequisites

1. An SMTP server (Gmail, Outlook, SendGrid, self-hosted, etc.)
2. A CloudConnection named `my-email` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `email` |
| Key | A KeyManager credential with email address + password/app password |
| Auth Type | `basic_auth` |
| SMTP Host | `smtp.gmail.com` |
| SMTP Port | `587` (STARTTLS) or `465` (SSL) |
| From | `alerts@company.com` (optional, defaults to credential username) |

For Gmail, use an [App Password](https://myaccount.google.com/apppasswords) rather than your account password.

---

## Pipeline Overview

```
+-----------------+     +-----------------+
|  create_report  | --> |  send_email     |
|  shell: echo    |     |  email_send     |
|  (01_create)    |     |  (01_create)    |
+-----------------+     +-----------------+
```

---

## Trigger

Manual trigger only -- will not start at j9t startup.

---

## Task Details

### 1. create_report -- generate sample report

Creates a sample text file to use as an email attachment.

| Field | Value |
|-------|-------|
| Type | `shell` |
| Working dir | `emailSendDemo/01_create` |

### 2. send_email -- send email with attachment

Sends an email with the generated report attached.

| Field | Value |
|-------|-------|
| Type | `email_send` |
| Connection | `my-email` |
| To | `recipient@example.com` |
| Subject | `j9t Demo Report` |
| Body | `Please find the attached demo report from j9t.` |
| Attachments | `["stdout.txt"]` |
| Depends on | `create_report` |

---

## email_send Task Type Reference

| Param | Required | Description |
|-------|----------|-------------|
| `connection` | yes | Named CloudConnection (type `email`) |
| `to` | yes | Recipient(s), comma-separated |
| `subject` | yes | Email subject (supports template variables) |
| `body` | yes | Email body text |
| `cc` | no | CC recipients, comma-separated |
| `attachments` | no | Array of file paths relative to working directory |

Builds RFC 2822 messages. Attachments are base64-encoded as MIME multipart parts. Uses libcurl SMTP with STARTTLS (port 587) or implicit SSL (port 465).

---

## Running

```bash
curl -s -X POST http://localhost:8080/api/workflows/emailSendDemo/run
```

---

## Key Concepts Demonstrated

- **SMTP via libcurl** -- native SMTP support, no external mail library needed
- **MIME attachments** -- base64-encoded multipart/mixed for binary attachments
- **STARTTLS** -- TLS encryption enforced for port 587
- **Named connections** -- SMTP host, credentials, and sender centralized in Connections tab
- **Template variables** -- subject and body support `{{output}}`, `{{workflow_id}}`, etc.
