# slackQAndABot Workflow -- Slack Q&A Bot Round-Trip

## Executive Summary

The **slackQAndABot** workflow is a true Slack round-trip demo: it reads the most recent non-bot message from a Slack channel (`slack_read` → `conversations.history`), feeds that question to an `ai_call` task configured as a C++/Vulkan expert, then posts the answer back to the same channel as a **threaded reply** (`slack_message` → `chat.postMessage` with `thread_ts`). Round-trip = read external → process → write back to the same external system.

---

## Prerequisites

### 1. Slack app with scopes

Create a Slack app at https://api.slack.com/apps → *From scratch*. Under **OAuth & Permissions** → **Bot Token Scopes**, add all three:

| Scope | Why |
|---|---|
| `chat:write` | post the AI-generated answer |
| `channels:history` | read recent messages from a public channel |
| `channels:read` | (optional) resolve channel names to IDs |

Click **Install to Workspace** (or **Reinstall** if scopes changed after initial install). Copy the **Bot User OAuth Token** (`xoxb-...`).

### 2. Invite the bot to the channel

In Slack, in the target channel (e.g. `#j9t-demo`), run:

```
/invite @<bot-name>
```

The bot must be a member of the channel to both read messages and post replies.

### 3. Find the channel ID

Slack's `conversations.history` API requires a channel **ID** (e.g. `C0ASS7AG8TD`), not a `#name`. Get it via:

```bash
curl -sH "Authorization: Bearer xoxb-..." \
  "https://slack.com/api/conversations.list?types=public_channel" | python3 -m json.tool
```

Find the entry where `name == "j9t-demo"` and copy its `id`. Update the `channel` param in `slackQAndABot.json` to that ID (currently hardcoded to `C0ASS7AG8TD`).

### 4. Store the bot token in KeyManager

```bash
curl -sk -X POST https://localhost:8443/api/settings/providers \
  -H 'Content-Type: application/json' \
  -d '{"name":"slack-bot","display_name":"Slack Bot","api_key":"xoxb-...","credential_type":"api_key","api_type":"slack"}'
curl -sk -X POST https://localhost:8443/api/settings/providers/save -d '{}'
```

### 5. Create the `my-slack` CloudConnection

```bash
curl -sk -X POST https://localhost:8443/api/connections \
  -H 'Content-Type: application/json' \
  -d '{"name":"my-slack","type":"slack","endpoint":"https://slack.com/api","key_name":"slack-bot","auth_type":"bearer","params":{}}'
curl -sk -X POST https://localhost:8443/api/connections/save -d '{}'
curl -sk -X POST https://localhost:8443/api/connections/my-slack/test -d '{}'
```

---

## Trigger

Manual only. Post a question to the channel in Slack first (e.g. *"What is the difference between `std::move` and `std::forward` in C++?"* or *"How do I properly synchronize a Vulkan command buffer submission?"*), then trigger the workflow.

```bash
curl -sk -X POST https://localhost:8443/api/workflows/slackQAndABot/run -d '{}'
```

---

## Task Graph

```
fetch_messages (slack_read, 10 latest, exclude_bots=true)
    |
    v
ai_answer (ai_call, cntx_files=[latest_message.txt], C++/Vulkan expert persona)
    |
    v
post_answer (slack_message, text_file=<AI output>, thread_ts_file=<latest_ts>)
```

### fetch_messages -- `slack_read`

| Param | Value |
|---|---|
| `connection` | `my-slack` |
| `channel` | `C0ASS7AG8TD` (channel ID, not `#name`) |
| `limit` | `10` |
| `exclude_bots` | `true` (skip bot's own messages to avoid self-reply loops) |

Writes the following to the task working directory:
- `messages_summary.json` -- array of `{ts, user, text}` for all fetched non-bot messages
- `latest_message.txt` -- text of the most recent message (fed as cntx to the AI)
- `latest_ts.txt` -- ts of the most recent message (used as `thread_ts` on the reply)
- `response.json` -- `{ok, count, channel, latest_ts}`

### ai_answer -- `ai_call`

One-shot AI request with a C++/Vulkan expert system prompt. The user's question is supplied via `cntx_files` pointing at `workflows/slackQAndABot/01_fetch/latest_message.txt`. The AI writes its answer to `PROB_answer.output.txt` in the queue directory.

### post_answer -- `slack_message`

| Param | Value |
|---|---|
| `connection` | `my-slack` |
| `channel` | `C0ASS7AG8TD` |
| `text_file` | `queue/slackQAndABot/02_ai_answer/PROB_answer.output.txt` |
| `thread_ts_file` | `workflows/slackQAndABot/01_fetch/latest_ts.txt` |

Reads the AI's answer from disk (instead of inline `text`) and the parent message ts from disk (instead of inline `thread_ts`), then posts `chat.postMessage` with `thread_ts` set so the reply appears as a thread on the original question.

---

## Verify

After the run, open `#j9t-demo` in Slack. The bot's answer should appear as a **threaded reply** under your original question.

---

## Key Concepts Demonstrated

- **True round-trip pattern** -- read external state, process with AI, write back to the same external system
- **`slack_read` task type** -- `conversations.history` with bot-message filtering
- **`slack_message` with threading** -- `thread_ts` support for proper thread replies
- **File-based param wiring** -- `text_file` and `thread_ts_file` let downstream tasks consume outputs written to disk by upstream tasks (same pattern as `email_send` body_file)
- **Named connections + KeyManager** -- token centralized, never inlined in JCWF
