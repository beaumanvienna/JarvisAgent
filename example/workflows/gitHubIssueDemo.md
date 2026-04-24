# gitHubIssueDemo Workflow -- GitHub REST API Integration

## Executive Summary

The **gitHubIssueDemo** workflow is a GitHub round-trip demo with AI-driven triage. It lists open issues from a GitHub repository (`github_issue/list_issues`), converts the response to CSV, per-item asks an AI to triage each issue as bug/feature/question/chore/duplicate with priority P0..P3, and posts the structured triage back as a comment (`github_issue/comment`) on each issue.

The triage AI uses **structured output** — it emits a schema-validated JSON object `{category, priority, next_action, labels_ok}`, and the downstream `post_comment` composes the comment body from `{{ai_triage.json.<field>}}` templates. The schema enforces the category and priority enums; bounded retries handle transient drift. No more prompt-engineering kludge to force the AI into a parseable free-text shape.

---

## Prerequisites

1. A GitHub account with a Personal Access Token (PAT) that has `repo` scope
2. A CloudConnection named `my-github` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `github` |
| Endpoint | *(empty for `https://api.github.com`)* |
| Key | A KeyManager credential with the PAT |
| Auth Type | `bearer` |
| Owner | `myorg` |
| Repo | `myrepo` |

---

## Trigger

Manual trigger only -- will not start at j9t startup.

---

## Task Details

### 1. list_issues -- `github_issue/list_issues`

Fetches the repo's open issues via `GET /repos/{owner}/{repo}/issues?state=open`. The response lands at `01_list/response.json` with the GitHub REST API's array of issue objects (pull requests included — filtered out in step 2).

### 2. convert_to_csv -- `python`

Calls `scripts.convertGitHubIssuesToCsv.convert` to drop PR entries from the raw response and emit a filter-friendly `issues.csv` with `number / title / body / labels` columns. Body text is truncated to 500 chars and newlines are collapsed so the CSV stays parseable.

### 3. ai_triage -- `ai_call` (per-item, **structured output**)

Runs once per row in `issues.csv` via the `issues` CSV filter. Declares an `output_schema`:

```jsonc
"output_schema": {
  "type": "object",
  "properties": {
    "category":    { "type": "string", "enum": ["bug", "feature", "question", "chore", "duplicate"] },
    "priority":    { "type": "string", "enum": ["P0", "P1", "P2", "P3"] },
    "next_action": { "type": "string", "minLength": 1, "maxLength": 500 },
    "labels_ok":   { "type": "boolean" }
  },
  "required": ["category", "priority", "next_action", "labels_ok"],
  "additionalProperties": false
},
"output_retries": 3
```

The runtime validates the AI reply against the schema, and on mismatch retries with the validator error list appended as a correction message. The validated JSON lands at `03_ai_triage/PROB_issue_<N>_<row>.output.json`.

### 4. post_comment -- `github_issue/comment` (per-item)

Posts a comment on each issue via `POST /repos/{owner}/{repo}/issues/{number}/comments`. The body is composed from the structured triage fields via the engine's `.json.PATH` template resolver:

```
body: "Automated triage by j9t workflow:\n\nCategory: {{ai_triage.json.category}}\nPriority: {{ai_triage.json.priority}}\nNext action: {{ai_triage.json.next_action}}\nLabels OK: {{ai_triage.json.labels_ok}}"
```

Each per-item instance reads the matching AI triage child's output — the runtime tracks per-item correspondence automatically, symmetric with cloud tasks' `{{A.json.PATH}}` resolution from `response.json`.

---

## Running

```bash
curl -s -X POST http://localhost:8080/api/workflows/gitHubIssueDemo/run
```

---

## Key Concepts Demonstrated

- **GitHub REST API** -- issue CRUD via Bearer token (PAT) authentication
- **Named connections** -- owner, repo, and credentials centralized in the Connections tab
- **Per-item fan-out** -- a CSV filter expands one task definition into N per-issue instances
- **Structured output** -- schema-validated `{category, priority, next_action, labels_ok}` JSON replacing prompt-engineered free-text triage; unlocks `{{ai_triage.json.<field>}}` template wiring with bounded-retry self-healing
- **Multiple operations** -- list_issues, comment in a single task type
