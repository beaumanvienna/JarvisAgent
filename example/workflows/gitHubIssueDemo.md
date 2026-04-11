# gitHubIssueDemo Workflow -- GitHub REST API Integration

## Executive Summary

The **gitHubIssueDemo** workflow demonstrates how JarvisAgent interacts with **GitHub** through the cloud integration layer using the `github_issue` task type.

At its core, this workflow shows:

- how `github_issue` tasks create issues and list open issues via the GitHub REST API,
- how a named **CloudConnection** centralizes GitHub credentials and default repo config,
- and how tasks chain via `depends_on`.

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

### 1. create_issue -- create a demo issue

| Field | Value |
|-------|-------|
| Type | `github_issue` |
| Connection | `my-github` |
| Operation | `create` |
| Title | `j9t demo issue` |
| Body | `This issue was created by the gitHubIssueDemo workflow.` |
| Labels | `["automated", "demo"]` |

### 2. list_issues -- list open issues

| Field | Value |
|-------|-------|
| Type | `github_issue` |
| Connection | `my-github` |
| Operation | `list_issues` |
| Depends on | `create_issue` |

---

## Running

```bash
curl -s -X POST http://localhost:8080/api/workflows/gitHubIssueDemo/run
```

---

## Key Concepts Demonstrated

- **GitHub REST API** -- issue CRUD via Bearer token (PAT) authentication
- **Named connections** -- owner, repo, and credentials centralized in the Connections tab
- **Multiple operations** -- create, comment, close, get_file, list_issues in a single task type
