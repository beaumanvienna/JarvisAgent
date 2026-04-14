# sheetsQuizGrader Workflow — Google Sheets Read-Grade-Write Round-Trip

## Executive Summary

The **sheetsQuizGrader** workflow demonstrates a full Google Sheets read/write cycle with an AI evaluation step in the middle. It reads C++ and Vulkan quiz questions + student answers from a Google Sheet, runs an AI model that grades each answer and produces CSV, and writes the grades back to the same sheet.

This is the canonical demo for:

- `sheets_read` → `ai_call` → `sheets_write` chaining via `depends_on`
- OAuth2 authentication to Google (required for sheets_write; API keys work only for read-only)
- The `outputs:{}` slot pattern on `ai_call` (instead of the anti-pattern `file_outputs` inside a queue folder)
- Persistent OAuth refresh tokens — once consent is granted, j9t stores the refresh token in the encrypted key store, and subsequent restarts do not require re-consent

---

## Prerequisites

1. A Google account
2. A Google Cloud project with the **Google Sheets API** enabled
3. A Google OAuth 2.0 client credential (Web application type) — see OAuth setup below
4. A `my-sheets` CloudConnection configured in j9t
5. A Google Sheet populated with the sample quiz data from `example/workflows/sheetsQuizGrader_sample_data.csv`
6. An AI provider configured (OpenAI, Anthropic, Gemini etc.)

### Creating the Google Sheet

1. Import `example/workflows/sheetsQuizGrader_sample_data.csv` into a new Google Sheet. Google will name the tab after the file — default `sheetsQuizGrader_sample_data`.
2. Copy the spreadsheet ID from the URL — it's the long token between `/d/` and `/edit`:
   ```
   https://docs.google.com/spreadsheets/d/YOUR_SPREADSHEET_ID/edit
   ```
3. Note the tab name (bottom-left) — if it isn't `sheetsQuizGrader_sample_data`, adjust the `range` in the JCWF.

### Google Cloud OAuth setup (one-time)

1. Go to https://console.cloud.google.com and create (or select) a project.
2. **APIs & Services → Enable APIs → Google Sheets API → Enable.**
3. **APIs & Services → OAuth consent screen** (now called "Google Auth Platform" in the new UI):
   - User type: **External**
   - App name: `j9t local` (any name)
   - Add yourself as a **Test user** under **Audience** — required while the app is in Testing mode.
4. **APIs & Services → Credentials → + Create Credentials → OAuth client ID**:
   - Application type: **Web application**
   - Name: `j9t local`
   - Authorized redirect URI (exactly):
     ```
     https://localhost:8443/api/connections/my-sheets/oauth/callback
     ```
   - Click **Create** and copy the **Client ID** and **Client secret**.

### Configuring j9t

All config goes through the REST API — never edit `keys.json.enc` or `connections.json` by hand.

```bash
# 1. Create a key to host the OAuth tokens
curl -sk -X POST https://localhost:8443/api/settings/providers \
  -H "Content-Type: application/json" \
  -d '{
        "name": "google-sheets-oauth",
        "display_name": "Google Sheets OAuth",
        "credential_type": "oauth",
        "scopes": "https://www.googleapis.com/auth/spreadsheets"
      }'
curl -sk -X POST https://localhost:8443/api/settings/providers/save

# 2. Create the my-sheets connection
curl -sk -X POST https://localhost:8443/api/connections \
  -H "Content-Type: application/json" \
  -d '{
        "name": "my-sheets",
        "type": "google_sheets",
        "endpoint": "https://sheets.googleapis.com/v4/spreadsheets",
        "key_name": "google-sheets-oauth",
        "auth_type": "oauth2",
        "params": {
          "client_id": "YOUR_CLIENT_ID",
          "client_secret": "YOUR_CLIENT_SECRET",
          "spreadsheet_id": "YOUR_SPREADSHEET_ID",
          "scopes": "https://www.googleapis.com/auth/spreadsheets"
        }
      }'
curl -sk -X POST https://localhost:8443/api/connections/save

# 3. Start the OAuth authorize flow
curl -sk https://localhost:8443/api/connections/my-sheets/oauth/authorize
# → response contains "authorize_url": open in your browser
```

Walk through the browser flow:
- Google account picker → your Gmail
- "Google hasn't verified this app" → **Continue** (expected in Testing mode)
- Consent screen → **Continue**
- Self-signed cert warning on `https://localhost:8443/...` → **Advanced → Proceed to localhost (unsafe)**
- Final page: **Authorization successful**

At this point, j9t has:

- An access token in memory (valid for 1 hour)
- A refresh token persisted in `keys.json.enc` — subsequent j9t restarts hydrate the token entry from disk and refresh the access token on demand, **no re-consent needed**

---

## Task Graph

```
read_quiz  ──►  grade_answers  ──►  write_grades
(sheets_read)    (ai_call)          (sheets_write)
```

### 1. read_quiz — fetch quiz from Google Sheets

| Field | Value |
|-------|-------|
| Type | `sheets_read` |
| Connection | `my-sheets` |
| Range | `sheetsQuizGrader_sample_data!A1:B11` |
| Output format | `csv` |
| Output file | `quiz_data.csv` (written inside the task working directory) |

Pulls the question/answer rows into `workflows/sheetsQuizGrader/01_read/quiz_data.csv`.

### 2. grade_answers — AI grades each answer

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Mode | `one_shot` |
| Working directory | `../../queue/sheetsQuizGrader/02_grade` |
| Outputs | `grades` slot (auto-maps to `PROB_grade.output.txt`) |
| Depends on | `read_quiz` |

Queue binding:
- **STNG**: senior C++/Vulkan grader — output raw CSV only, no markdown, no headers
- **CNTX**: grading rubric (CORRECT / PARTIAL / WRONG semantics)
- **TASK**: emit CSV rows, one per question, same order as input
- **PROB**: the actual student answers (derived from `quiz_data.csv`)

The AI produces a plain-text CSV body at `queue/sheetsQuizGrader/02_grade/PROB_grade.output.txt`. The `outputs` slot (no `file_outputs`!) maps this path to `{{grade_answers.output_file}}` for downstream consumers.

### 3. write_grades — push grades back to the sheet

| Field | Value |
|-------|-------|
| Type | `sheets_write` |
| Connection | `my-sheets` |
| Range | `sheetsQuizGrader_sample_data!C1` |
| Input file | `{{grade_answers.output_file}}` (absolute path to the AI CSV) |
| Value input option | `USER_ENTERED` |
| Depends on | `grade_answers` |

`sheets_write` reads the CSV line-by-line (extension-agnostic) and PUTs it to Google Sheets at column C onward. Because the AI produces 3 columns (grade, score, explanation), the grades fan out across columns C, D, E.

---

## Running

```bash
curl -sk -X POST https://localhost:8443/api/workflows/sheetsQuizGrader/run
```

Poll the run:

```bash
curl -sk "https://localhost:8443/api/workflow-runs/<runId>"
```

---

## Expected Output

After the workflow completes, the sheet has 5 populated columns:

| A (Question) | B (Student Answer) | C (Grade) | D (Score) | E (Explanation) |
|---|---|---|---|---|
| What is VkInstance... | VkInstance is the connection... | Correct | 10 | Accurately describes VkInstance... |
| What does std::move do... | std::move copies and frees... | Incorrect | 0 | std::move casts to rvalue, it does not copy or free... |
| ... | ... | ... | ... | ... |

Expected total: around 70 / 100 (roughly 7 correct, 1 partial, 2 incorrect) — the sample data is deliberately mixed.

---

## Key Concepts Demonstrated

- **Full Google Sheets round-trip** — read, AI-process, write back to the same sheet
- **OAuth2 with PKCE + client secret** — j9t's generic OAuth2 flow handles both Microsoft (PKCE-only, public client) and Google (PKCE + client secret, confidential client) providers; per-connector `OAuth2ProviderInfo` drives the quirks.
- **Persistent refresh tokens** — after one consent, j9t restarts no longer require re-consent. Tokens live encrypted in `keys.json.enc`, hydrated on startup, and refreshed on demand the first time a workflow asks for an access token.
- **`outputs` slot on `ai_call`** — exposes the AI response as `{{grade_answers.output_file}}` without writing a stray `grades.csv` into the queue folder (which would mis-categorize as a new requirements file and fire a second wasted AI request).
- **Schema-constrained AI output** — STNG/TASK/CNTX explicitly tell the model to emit a fixed CSV schema with no markdown, no headers, no preamble. `sheets_write` then consumes it verbatim.
