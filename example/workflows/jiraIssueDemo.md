# jiraIssueDemo Workflow -- Jira REST API Integration

## Executive Summary

The **jiraIssueDemo** workflow demonstrates how JarvisAgent creates **Jira** issues through the cloud integration layer using the `jira_issue` task type and Jira REST API v3.

---

## Prerequisites

1. A Jira Cloud or Data Center instance
2. Credentials:
   - **Jira Cloud**: email + API token (BasicAuth)
   - **Jira Data Center**: Personal Access Token (Bearer)
3. A CloudConnection named `my-jira` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `jira` |
| Endpoint | `https://mycompany.atlassian.net` |
| Key | A KeyManager credential |
| Auth Type | `basic_auth` (Cloud) or `bearer` (Data Center) |
| Project Key | `PROJ` |

---

## Trigger

Manual trigger only -- will not start at j9t startup.

---

## Task Details

### create_issue -- create a demo Jira issue

| Field | Value |
|-------|-------|
| Type | `jira_issue` |
| Connection | `my-jira` |
| Operation | `create` |
| Summary | `j9t demo issue` |
| Description | `This issue was created by the jiraIssueDemo workflow.` |
| Issue Type | `Task` |
| Labels | `["automated"]` |

Uses Jira REST API v3 with Atlassian Document Format (ADF) for the description field.

---

## Running

```bash
curl -s -X POST http://localhost:8080/api/workflows/jiraIssueDemo/run
```

---

## Key Concepts Demonstrated

- **Jira REST API v3** -- issue CRUD with ADF (Atlassian Document Format) for rich text
- **Dual auth** -- BasicAuth for Jira Cloud, Bearer PAT for Jira Data Center
- **Named connections** -- endpoint, project key, and credentials centralized in the Connections tab
- **Multiple operations** -- create, update, transition, comment, get in a single task type
