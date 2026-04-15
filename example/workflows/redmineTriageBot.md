# redmineTriageBot Workflow -- Redmine Triage Bot Round-Trip

## Executive Summary

The **redmineTriageBot** workflow is a true Redmine round-trip demo with AI-driven assignee routing. It lists open issues from a Redmine project (`redmine_issue/list_issues`), per-item asks an AI to classify each one as backend (JC) or frontend (Ahmet) and to write a triage comment, then updates each issue back in Redmine with the assigned user ID and the comment in a single `PUT /issues/{id}.json` call. Round-trip = read external → process → write back to the same external system.

The classifier AI is constrained to output a single digit (the numeric Redmine user ID) so its `captured_stdout` can be wired straight into the downstream `assigned_to_id` template variable -- no python parsing step needed.

---

## Prerequisites

### 1. Run Redmine locally

```bash
docker run -d --name redmine -p 3000:3000 redmine
```

Wait ~30 seconds for Rails to boot, then open http://localhost:3000.

### 2. First-run setup in the Redmine UI

1. Sign in as `admin` / `admin`. Redmine forces a password change -- pick anything.
2. Go to **Administration**. A yellow banner reads *"Roles, trackers, issue statuses and workflow have not been configured yet."* Click **Load the default configuration** (English). This seeds trackers, statuses, workflows, roles, and enumerations all at once -- without it, you'll hit a series of "Default status cannot be blank" / "Role cannot be empty" errors trying to create issues.
3. **My account** -> right panel, **API access key** is missing because REST is disabled. Skip to step 4 first.
4. **Administration** -> **Settings** -> **API** tab -> check **Enable REST web service** -> Save.
5. Back to **My account** -> right panel now shows **API access key** -> click **Reset**, then **Show**, copy the hex string.
6. **Administration** -> **Users** -> **+ New user**:
   - Login `JC`, First `JC`, Last `Backend`, Email `jc@test.local`, password anything. Note the user ID from the URL bar (`/users/N`).
   - **+ New user** again: Login `Ahmet`, First `Ahmet`, Last `Frontend`, Email `ahmet@test.local`, password anything. Note its user ID.
7. **Projects** -> **+ New project**: Name `j9t demo`, Identifier `j9t-demo` (must match exactly -- the JCWF hard-codes it as the `project_identifier` param). Leave all trackers checked. Create.
8. Inside the project -> **Settings** -> **Members** -> **+ New member** -> check both JC and Ahmet -> expand **Roles** at the bottom -> check **Developer** -> Add.
9. Inside the project -> **+ New issue** ×3 from `example/workflows/redmine_seed_tickets.csv`:

   | Tracker | Subject | Description |
   |---|---|---|
   | Bug | Login API returns 500 on empty password | (see CSV) |
   | Bug | Dashboard charts flicker on dark-mode toggle | (see CSV) |
   | Feature | Add CSV export button to workflow runs table | (see CSV) |

   Leave **Assignee** empty for all three -- the workflow assigns them.

### 3. Hard-code the routing user IDs

The JCWF's classifier prompt embeds JC's user ID and Ahmet's user ID inline in the system message (`STNG_router.txt`). The shipped JCWF assumes JC = 5 and Ahmet = 6 (the IDs you get on a fresh Redmine install where admin is user 1). If your IDs differ (e.g. you created extra users), edit `workflows/redmineTriageBot/redmineTriageBot.json` and update both the STNG_router.txt content and the CNTX_team.txt content with the correct IDs.

### 4. Store the Redmine API key in KeyManager

```bash
curl -sk -X POST https://localhost:8443/api/settings/providers \
  -H 'Content-Type: application/json' \
  -d '{"name":"redmine-key","display_name":"Redmine API Key","api_key":"<paste hex from step 5>","credential_type":"api_key","api_type":"redmine"}'
curl -sk -X POST https://localhost:8443/api/settings/providers/save -d '{}'
```

### 5. Create the `my-redmine` CloudConnection

```bash
curl -sk -X POST https://localhost:8443/api/connections \
  -H 'Content-Type: application/json' \
  -d '{
    "name":"my-redmine",
    "type":"redmine",
    "endpoint":"http://localhost:3000",
    "key_name":"redmine-key",
    "auth_type":"bearer",
    "params":{"project_identifier":"j9t-demo"}
  }'
curl -sk -X POST https://localhost:8443/api/connections/save -d '{}'
curl -sk -X POST https://localhost:8443/api/connections/my-redmine/test -d '{}'
# -> {"ok":true}
```

`auth_type` is set to `bearer` because the existing CloudAuthType enum doesn't have a dedicated `ApiKey` value -- the Redmine connector ignores the auth-type metadata and unconditionally builds an `X-Redmine-API-Key` header from the credential's token.

---

## Trigger

Manual only.

```bash
curl -sk -X POST https://localhost:8443/api/workflows/redmineTriageBot/run -d '{}'
```

---

## Task Graph

```
list_issues (redmine_issue/list_issues, project=j9t-demo)
    |
    v
convert_to_csv (python: convertRedmineIssuesToCsv)
    |
    +---> filter "issues" (csv, binding: issue)
    |
    +-----> ai_classify (per_item, one_shot, output = one digit user id)
    |             |
    |             v
    |        update_issue (per_item, redmine_issue/update_issue)
    |             ^
    |             |
    +-----> ai_comment  (per_item, one_shot, output = triage note)
```

`update_issue` depends on both `ai_classify` and `ai_comment` so it gets the matching item's outputs from each.

### list_issues -- `redmine_issue/list_issues`

`GET /issues.json?project_id=j9t-demo&status_id=open&limit=50` with the `X-Redmine-API-Key` header. Writes the raw response body to `01_list/response.json`.

### convert_to_csv -- python

`scripts/convertRedmineIssuesToCsv.py` reads the JSON and writes `02_convert/issues.csv` with columns `id`, `subject`, `description`, `tracker`. The csv filter consumes this file.

### filter `issues`

Source: `02_convert/issues.csv`, binding: `issue`. Each downstream `mode: per_item` task with `filter: issues` runs once per row.

### ai_classify -- `ai_call`, per_item, one_shot

Strict-output classifier. The system prompt (`STNG_router.txt`) hard-codes the team mapping (JC = id 5, Ahmet = id 6) and instructs the AI to emit exactly one digit -- no words, no punctuation, no newlines. The PROB file is templated per item with the issue's id / tracker / subject / description from the filter binding. `captured_stdout` ends up as the literal string `"5"` or `"6"`, which feeds straight into the downstream `update_issue` task's `assigned_to_id` param.

### ai_comment -- `ai_call`, per_item, one_shot

Triage-comment writer. Different persona (senior engineer doing PM-style triage), different prompt, but consumes the same per-item issue data. `captured_stdout` is the triage note that will be posted to Redmine.

### update_issue -- `redmine_issue/update_issue`, per_item

`PUT /issues/{{issue.id}}.json` with body `{"issue":{"notes":"...","assigned_to_id":N}}`. Both the assignee ID and the note text are pulled per-item from the matching `ai_classify` and `ai_comment` instance via `{{ai_classify.captured_stdout}}` / `{{ai_comment.captured_stdout}}`. Redmine returns 204 No Content on success; the executor synthesizes a `{ok:true}` payload so downstream tasks (or the dashboard) see something useful.

---

## Verify

After the run, open http://localhost:3000/projects/j9t-demo/issues. All three issues should now be:

- assigned (Login API -> JC Backend, Dashboard flicker -> Ahmet Frontend, CSV export -> Ahmet Frontend)
- carrying a fresh **Notes** entry from `Redmine Admin` with the AI-generated triage comment

Click into any issue to read the comment.

---

## Key Concepts Demonstrated

- **True round-trip pattern** -- read external (`list_issues`), process with AI, write back (`update_issue`)
- **Two-AI per-item pattern** -- one strict-format classifier feeding a structured field, one freeform writer feeding a text field, both consumed by a single downstream cloud task
- **Per-item output piping** -- `{{taskId.captured_stdout}}` resolves to the matching item's output for the same row index, no python glue needed for the routing
- **Self-hosted FOSS connector** -- complements the proprietary GitHub + Jira connectors with an open-source tracker that runs entirely from a Docker image
- **Redmine REST API** -- `X-Redmine-API-Key` header auth, `GET /issues.json` for listing, `PUT /issues/{id}.json` with `{"issue":{...}}` body for atomic notes + assignee updates
- **AI-driven team routing** -- the AI's classification literally selects which engineer owns the ticket; the demo doubles as a sanity check that the model can read a description and pick the right side of a backend/frontend split
