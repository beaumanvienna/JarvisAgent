# slackMessageDemo Workflow -- Slack Messaging Integration

## Executive Summary

The **slackMessageDemo** workflow demonstrates how JarvisAgent sends messages to **Slack** channels via the Slack Web API using the `slack_message` task type.

---

## Prerequisites

1. A Slack workspace with a Bot installed
2. A Slack Bot token (`xoxb-...`) with `chat:write` scope
3. A CloudConnection named `my-slack` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `slack` |
| Endpoint | *(empty for default `https://slack.com/api`)* |
| Key | A KeyManager credential with the Bot token |
| Auth Type | `bearer` |

---

## Trigger

Manual trigger only -- will not start at j9t startup.

---

## Task Details

### send_message -- send Slack notification

| Field | Value |
|-------|-------|
| Type | `slack_message` |
| Connection | `my-slack` |
| Channel | `#j9t-demo` |
| Text | `Hello from j9t! This message was sent by the slackMessageDemo workflow.` |

Sends `POST /api/chat.postMessage` with the channel and text. The Slack API response is written to `response.json`.

---

## Running

```bash
curl -s -X POST http://localhost:8080/api/workflows/slackMessageDemo/run
```

---

## Key Concepts Demonstrated

- **Slack Web API** -- `chat.postMessage` via Bearer token authentication
- **Named connections** -- Slack Bot token centralized in the Connections tab
- **Template variables** -- message text supports `{{output}}`, `{{workflow_id}}`, etc.
