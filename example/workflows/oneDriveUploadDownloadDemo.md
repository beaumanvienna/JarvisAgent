# oneDriveUploadDownloadDemo Workflow -- OneDrive Integration

## Executive Summary

The **oneDriveUploadDownloadDemo** workflow demonstrates how JarvisAgent interacts with **Microsoft OneDrive** through the cloud integration layer and Microsoft Graph API.

At its core, this workflow shows:

- how `onedrive_upload` and `onedrive_download` task types transfer files via the Graph API,
- how a named **CloudConnection** with OAuth2 authentication centralizes OneDrive access,
- how the OAuth PKCE flow authorizes j9t to act on behalf of a Microsoft account,
- and how tasks chain via `depends_on` to build an upload-then-download pipeline.

---

## Prerequisites

1. A Microsoft account (personal, work, or school) with OneDrive access
2. An Azure AD app registration with `Files.ReadWrite` and `offline_access` permissions
3. A CloudConnection named `my-onedrive` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `onedrive` |
| Endpoint | *(empty for default Graph API, or custom)* |
| Key | A KeyManager credential name (tokens managed by OAuthTokenManager) |
| Auth Type | `oauth2` |
| Client ID | Azure AD application (client) ID |
| Tenant ID | `common` (multi-tenant) or your specific tenant ID |
| Scopes | `Files.ReadWrite offline_access` |

4. Click **Authorize with Microsoft** in the Connections tab to complete the OAuth PKCE flow

---

## Pipeline Overview

```
+-----------------+
|  create_file    |
|  shell: echo    |
|  (01_create)    |
+--------+--------+
         |
         v
+-----------------+
|  upload         |
|  onedrive_upload|
|  (01_create)    |
+--------+--------+
         |
         v
+-----------------+
|  download       |
|  onedrive_down  |
|  (03_download)  |
+-----------------+
```

---

## Trigger

This workflow uses a **manual trigger** only. It will not start automatically when j9t loads -- it must be started explicitly via the web UI or REST API.

---

## Task Details

### 1. create_file -- generate sample data

Creates a sample text file with a timestamp to upload.

| Field | Value |
|-------|-------|
| Type | `shell` |
| Script | `scripts/run.sh` |
| Args | `echo`, `Hello from j9t OneDrive demo at $(date)` |
| Working dir | `oneDriveUploadDownloadDemo/01_create` |
| Output | `stdout.txt` |

### 2. upload -- upload to OneDrive

Uploads the generated file to OneDrive under the path `j9t-demo/hello.txt`.

| Field | Value |
|-------|-------|
| Type | `onedrive_upload` |
| Connection | `my-onedrive` |
| Operation | `upload` |
| Remote path | `j9t-demo/hello.txt` |
| Local path | `stdout.txt` (relative to working directory) |
| Working dir | `oneDriveUploadDownloadDemo/01_create` |
| Depends on | `create_file` |

The task executor reads the local file and sends a `PUT /me/drive/root:/j9t-demo/hello.txt:/content` request to the Graph API with the file body.

### 3. download -- download from OneDrive

Downloads the same file back to a local path to verify the round-trip.

| Field | Value |
|-------|-------|
| Type | `onedrive_download` |
| Connection | `my-onedrive` |
| Operation | `download` |
| Remote path | `j9t-demo/hello.txt` |
| Local path | `downloaded.txt` |
| Working dir | `oneDriveUploadDownloadDemo/03_download` |
| Depends on | `upload` |

The task executor sends a `GET /me/drive/root:/j9t-demo/hello.txt:/content` request and writes the response body to the local file.

---

## OneDrive Task Type Reference

The `onedrive_upload` and `onedrive_download` task types are backed by `OneDriveCloudTaskExecutor`, which extends `ICloudTaskExecutor`. The base class automatically resolves the named connection and OAuth2 credentials before delegating to the OneDrive-specific logic.

### Supported Operations

| Operation | Required Params | Description |
|-----------|----------------|-------------|
| `upload` | `remote_path`, `local_path` | PUT file content to OneDrive |
| `download` | `remote_path`, `local_path` | GET file content from OneDrive to local file |

### Common Params

| Param | Required | Description |
|-------|----------|-------------|
| `connection` | yes | Named CloudConnection (type `onedrive`) |
| `operation` | yes | `upload` or `download` |
| `remote_path` | yes | OneDrive path (e.g. `Documents/reports/output.pdf`) |
| `local_path` | yes | Local file path relative to task working directory |

---

## OAuth 2.0 Authentication

OneDrive uses **OAuth 2.0 with PKCE** (Proof Key for Code Exchange). The flow:

1. In the Connections tab, click **Authorize with Microsoft**
2. A popup opens to Microsoft login -- sign in and consent to the requested scopes
3. Microsoft redirects back to j9t with an authorization code
4. j9t exchanges the code for access and refresh tokens
5. `OAuthTokenManager` stores tokens and automatically refreshes them before expiry

No client secret is required -- PKCE replaces it with a cryptographic code challenge.

---

## Running

```bash
# Manual trigger only -- start via REST API or web UI
curl -s -X POST http://localhost:8080/api/workflows/oneDriveUploadDownloadDemo/run
```

Or click the play button in the workflow editor / dashboard.

---

## Expected Execution

### Task States at Completion

| Task | Final State | Notes |
|------|-------------|-------|
| `create_file` | Succeeded | Generated sample text file |
| `upload` | Succeeded | Uploaded to `onedrive:/j9t-demo/hello.txt` |
| `download` | Succeeded | Downloaded to local `downloaded.txt` |

### Output Files

| Path | Content |
|------|---------|
| `oneDriveUploadDownloadDemo/01_create/stdout.txt` | Sample text with timestamp |
| `oneDriveUploadDownloadDemo/01_create/response.json` | Graph API upload response (file metadata) |
| `oneDriveUploadDownloadDemo/03_download/downloaded.txt` | Copy of the uploaded file |
| `oneDriveUploadDownloadDemo/03_download/response.json` | `{"ok":true,"operation":"download",...}` |

---

## Key Concepts Demonstrated

- **OAuth 2.0 PKCE flow** -- secure token acquisition without a client secret
- **Cloud task executor pattern** -- `ICloudTaskExecutor` resolves connection + credentials, delegates to `ExecuteCloud()`
- **Named connections** -- OneDrive endpoint, OAuth tokens, and client config are centralized in the Connections tab
- **Automatic token refresh** -- `OAuthTokenManager` refreshes tokens in the background before expiry
- **Disk-first output** -- all responses are written to `response.json` in the task working directory
- **DAG dependency** -- download depends on upload completing first
