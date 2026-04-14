# jiraIssueDemo Workflow — Jira Round-Trip

## Executive Summary

The **jiraIssueDemo** workflow exercises the full read/write round-trip of the Jira cloud integration: an `ai_call` task generates a plain-text bug report, a `jira_issue/create` task submits it as a new issue using the AI output as the description, a `jira_issue/get` task retrieves the issue back using JSON-path template resolution on the create response, and a Python verify task asserts that the retrieved fields match what was submitted.

This is the canonical demo for two v1.1 capabilities:

1. **`description_file` param on `jira_issue/create`** — reads the description body from a file, enabling AI output to flow directly into Jira without inlining large strings in the JCWF.
2. **JSON-path template resolution (`{{taskId.json.PATH}}`)** — any downstream task can reference fields from an upstream cloud task's `response.json` using dotted paths. In this demo, `get_issue` uses `issue_key: "{{create_issue.json.key}}"` to target the freshly-created issue by its returned key.

---

## Prerequisites

1. A Jira Cloud tenant (free tier is sufficient) with at least one software project.
2. An Atlassian API token created at https://id.atlassian.com/manage-profile/security/api-tokens.
3. A Key in the j9t Keys tab:
   - **Credential type:** `Username / Password`
   - **Username:** your Jira account email
   - **Password:** the API token
4. A CloudConnection named `my-jira` in the Connections tab:

| Field | Example |
|-------|---------|
| Type | `jira` |
| Endpoint | `https://<tenant>.atlassian.net` |
| Key | The key created above |
| Auth type | `basic_auth` |
| `project_key` param | Your project key (e.g. `SCRUM`) |

Test the connection from the Connections tab — the test calls `GET /rest/api/3/myself` and must return 200.

---

## Trigger

Manual only. Run from the dashboard, via `POST /api/workflows/jiraIssueDemo/run`, or via the MCP `run_workflow` tool.

---

## Task Graph

```
ai_bug_report  ──►  create_issue  ──►  get_issue  ──►  verify
  (ai_call)        (jira_issue/create)  (jira_issue/get)   (python)
```

### ai_bug_report — AI generates bug report

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Mode | `one_shot` |
| Working directory | `../../queue/jiraIssueDemo/01_ai_report` |
| Outputs | `bug_report` (slot) — maps automatically to the session manager's natural `PROB_bug.output.txt` |
| Queue binding | STNG + CNTX + TASK + PROB files providing role, product context, writing task, and the concrete bug scenario |

Produces a 3–4 sentence plain-text bug report at `queue/jiraIssueDemo/01_ai_report/PROB_bug.output.txt`. The task does **not** declare `file_outputs` — that would write an arbitrary-name file inside the queue folder, which the file categorizer would then pick up as a new requirements file and fire a second wasted AI call. Instead the `outputs` slot is mapped automatically to the `<PROB_stem>.output.<ext>` file the session manager produces.

### create_issue — submit to Jira

| Field | Value |
|-------|-------|
| Type | `jira_issue` |
| Operation | `create` |
| Connection | `my-jira` |
| Summary | `OneDrive upload timeout for large files shows no error` |
| Issue type | `Bug` |
| Labels | `["automated", "cloud-integration"]` |
| Description file | `{{ai_bug_report.output_file}}` |
| Working directory | `jiraIssueDemo/02_create` |

The executor reads the file at `{{ai_bug_report.output_file}}` (resolves to `queue/jiraIssueDemo/01_ai_report/PROB_bug.output.txt`), collapses embedded newlines to spaces (Jira ADF text nodes disallow newlines), wraps the content in a single-paragraph ADF document, and POSTs to `/rest/api/3/issue`. The response is written to `response.json` with shape `{"id": "10000", "key": "SCRUM-1", ...}`.

### get_issue — retrieve by key

| Field | Value |
|-------|-------|
| Type | `jira_issue` |
| Operation | `get` |
| Connection | `my-jira` |
| Issue key | `{{create_issue.json.key}}` |
| Working directory | `jiraIssueDemo/03_get` |

The runtime flattens `create_issue`'s `response.json` and injects `{{create_issue.json.key}}` into this task's template context, so `issue_key` resolves to the real key before the GET request is built. The full issue is written to `jiraIssueDemo/03_get/response.json`.

### verify — assert round-trip

| Field | Value |
|-------|-------|
| Type | `python` |
| Module | `verifyJiraRoundTrip` |
| Function | `verify` |
| Working directory | `jiraIssueDemo/04_verify` |

Reads both `response.json` files plus the original `PROB_bug.output.txt` and asserts:

- `create_issue.key == get_issue.key`
- `fields.summary` contains the expected substring
- `fields.issuetype.name == "Bug"`
- `fields.labels` contains `automated` and `cloud-integration`
- The first 20 characters of the AI-generated bug report appear in the retrieved ADF description (proving the text survived the round-trip end-to-end)

Any failure returns an `errors` list; success returns `{"ok": true, "issue_key": "...", "summary": "...", ...}`.

---

## Running

```bash
curl -sk -X POST https://localhost:8443/api/workflows/jiraIssueDemo/run
```

Then watch progress in the dashboard, or poll:

```bash
curl -sk https://localhost:8443/api/workflow-runs/<runId>
```

On success, the issue persists in your Jira project. Delete it manually from the Jira UI if you don't want test artifacts accumulating.

---

## Key Concepts Demonstrated

- **AI-to-cloud piping via files** — `description_file` lets AI output flow into a cloud write without inlining strings in JCWF template params.
- **JSON-path template resolution** — `{{taskId.json.dotted.path}}` reads fields from an upstream cloud task's `response.json`, so chained cloud operations don't need ad-hoc glue scripts to thread IDs through.
- **Round-trip verification** — create, get, and programmatically assert field equality so the demo fails loudly if the backend regresses.
- **Jira REST API v3 + ADF** — the executor constructs Atlassian Document Format bodies for `description` and `comment` fields from plain-text inputs.
- **BasicAuth credential type** — Jira Cloud uses email + API token via the `credentials` key type.
