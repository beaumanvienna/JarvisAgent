# oneDriveUploadDownloadDemo Workflow -- OneDrive Round-Trip

## Executive Summary

The **oneDriveUploadDownloadDemo** workflow is a true OneDrive round-trip demo. It uploads a project status file to OneDrive (`onedrive_upload`), downloads it back (`onedrive_download`), feeds the downloaded content to an `ai_call` task that produces a PM-style review, then uploads the AI review back to OneDrive as a second file. Round-trip = read external → process → write back to the same external system.

---

## Prerequisites

### 1. Microsoft account + Azure free trial

A fresh personal Microsoft account (e.g. `outlook.com`, `live.com`, or a `gmail.com` MSA) **cannot directly access the Azure portal or Microsoft Entra admin center** without an associated tenant. To bootstrap a tenant, sign up for the Azure free trial at https://azure.microsoft.com/en-us/free/. The trial requires a credit card for ID verification (no charges if you stay in the free tier; App Registrations are free regardless and never consume any quota).

If you already have a Microsoft 365 work/school account, skip the free-trial step and use that account's tenant directly.

### 2. Azure AD (Entra ID) app registration

In the Azure portal → **App registrations** → **+ New registration**:

| Field | Value |
|---|---|
| Name | `j9t-onedrive` |
| Supported account types | **Accounts in any organizational directory + personal Microsoft accounts** |
| Redirect URI | (configured via Authentication blade after registration — see below) |

After clicking **Register**, note the **Application (client) ID** and **Directory (tenant) ID** from the overview page.

Then in the **Authentication** blade:
1. Add a platform → choose **Mobile and desktop applications** (NOT Web — Web requires a client secret, j9t uses PKCE-only)
2. Custom redirect URI: `https://localhost:8443/api/connections/my-onedrive/oauth/callback`
3. In **Settings**, set **Allow public client flows** = **Yes**
4. **Save**

In the **API permissions** blade → **+ Add a permission** → **Microsoft Graph** → **Delegated permissions**:
- `Files.ReadWrite`
- `offline_access`

(`User.Read` is added automatically and is fine to leave alone.)

### 3. Create the `my-onedrive` CloudConnection

```bash
curl -sk -X POST https://localhost:8443/api/connections \
  -H 'Content-Type: application/json' \
  -d '{
    "name":"my-onedrive",
    "type":"onedrive",
    "endpoint":"",
    "key_name":"onedrive-oauth",
    "auth_type":"oauth2",
    "params":{
      "client_id":"<Application (client) ID from Azure>",
      "tenant_id":"common",
      "scopes":"Files.ReadWrite offline_access"
    }
  }'
curl -sk -X POST https://localhost:8443/api/connections/save -d '{}'
```

Use `tenant_id: "common"` regardless of which tenant you registered in — it works for both Entra tenants and personal MSAs and matches the multitenant app type.

### 4. Run the OAuth PKCE flow

```bash
curl -sk https://localhost:8443/api/connections/my-onedrive/oauth/authorize
```

The response contains an `authorize_url`. Open it in your browser, sign in with the same Microsoft account, click **Continue** on the device confirmation, then **Accept** on the consent screen. The browser redirects to j9t's local callback (you may have to click through a self-signed-cert warning) and you'll see *"Authorization successful"*.

j9t exchanges the code for tokens, stores the refresh token encrypted in `keys.json.enc`, and the OAuthTokenManager hydrates from disk on every subsequent startup — meaning **the connection survives j9t restarts without re-consent**.

Verify:

```bash
curl -sk -X POST https://localhost:8443/api/connections/my-onedrive/test -d '{}'
# {"ok":true}
```

---

## Trigger

Manual only.

```bash
curl -sk -X POST https://localhost:8443/api/workflows/oneDriveUploadDownloadDemo/run -d '{}'
```

---

## Task Graph

```
upload (onedrive_upload, status.txt)
    |
    v
download (onedrive_download, downloaded_status.txt)
    |
    v
ai_review (ai_call, cntx_files=[downloaded_status.txt], PM persona)
    |
    v
upload_review (onedrive_upload, local_path={{ai_review.output_file}})
```

### upload -- `onedrive_upload`

Uploads the seed file `status.txt` (shipped inside the workflow folder at `01_upload/status.txt`) to OneDrive at `j9t-demo/status.txt`. `local_path` is resolved relative to the task working directory, so a bare filename works.

### download -- `onedrive_download`

Downloads `j9t-demo/status.txt` back to local `02_download/downloaded_status.txt`. This is the "read external" half of the round-trip — the file we'll feed to the AI was just freshly fetched from OneDrive, not read from the local seed.

### ai_review -- `ai_call`

One-shot AI request with a senior-PM system prompt. The downloaded status file is supplied via `cntx_files` pointing at `02_download/downloaded_status.txt`. The AI writes its review to `PROB_review.output.txt` in the queue directory.

### upload_review -- `onedrive_upload`

Uploads the AI's review back to OneDrive at `j9t-demo/ai_review.txt`. Uses the `{{ai_review.output_file}}` template variable — the runtime resolves this to an absolute path of the upstream task's output file, sidestepping the working-directory-relative path resolution that would otherwise require the `04_upload_review/` directory to pre-exist.

---

## Verify

After the run, open https://onedrive.live.com in your browser, sign in with the same Microsoft account, and look for a `j9t-demo` folder containing both `status.txt` and `ai_review.txt`.

---

## Key Concepts Demonstrated

- **OAuth 2.0 PKCE flow with public-client app** — no client secret, redirect URI registered as Mobile/desktop platform with public-client flows enabled
- **Encrypted refresh-token persistence** — `OAuthTokenManager` writes the refresh token to `keys.json.enc` after the OAuth callback and hydrates from disk on startup, surviving j9t restarts
- **Auto-provisioned KeyManager provider** — the OAuth callback creates a same-named provider entry on first authorize if one doesn't already exist, so the natural editor flow (Connections → New → Authorize) works without a manual placeholder step
- **True round-trip pattern** — read external state (download), process with AI, write back to the same external system (upload AI report)
- **`{{taskId.output_file}}` template** — used to feed an upstream `ai_call` output file into a downstream cloud upload's `local_path`, avoiding fragile relative-path resolution
- **Microsoft Graph API** — `PUT /me/drive/root:/{path}:/content` for upload, `GET ...:/content` for download, called from C++ via libcurl with bearer auth
