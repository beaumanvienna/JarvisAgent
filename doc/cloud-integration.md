# Cloud Integration

This document covers the cloud integration layer introduced in j9t, including the abstraction framework, credential management, MCP interface, and connection management.

---

## 1. Overview

The cloud integration layer provides a uniform interface for connecting j9t workflows to external cloud services (Polarion, S3, OneDrive, Snowflake, PostgreSQL, Slack, Email, GitHub, Jira, Azure Blob Storage, Google Cloud Storage). All cloud code in the C++ backend uses libcurl + OpenSSL + simdjson — Python is reserved for workflow task scripts only.

**Design principles:**
- **Disk-first** — all cloud data (query results, downloaded files, API responses) is written to the workflow working directory before downstream tasks consume it.
- **GPL-3.0 compliance** — all libraries linked into j9t are GPL-3.0 compatible. Cloud services are accessed via open REST/HTTP protocols.
- **Credential safety** — secrets are stored in the encrypted key store (`keys.json.enc`) and never appear in JCWF files or log output.

---

## 2. Cloud Abstraction Layer (Phase 0)

### ICloudConnector

Abstract interface for all cloud service connectors. Each connector implements:
- `GetType()` — returns the connector type name (e.g., `"polarion"`, `"s3"`)
- `TestConnection()` — validates connectivity with given config
- `ResolveCredentials()` — resolves stored credentials into a runtime `CloudCredentials` bundle

**File:** `application/cloud/cloudConnector.h`

### CloudConnectorRegistry

Registry for connector plugins. Connectors register themselves at startup; task executors and the REST API look them up by type name.

**Files:** `application/cloud/cloudConnectorRegistry.h/cpp`

### CloudConnection

Configuration for a named cloud connection, persisted in `connections.json`:

```json
{
  "name": "my-polarion",
  "type": "polarion",
  "endpoint": "https://polarion.company.com",
  "key_name": "polarion-pat",
  "auth_type": "bearer",
  "params": {
    "project_id": "GoKartProcurement"
  }
}
```

**Auth types:** `bearer`, `oauth2`, `jwt_rsa`, `basic_auth`, `sigv4`

### CloudConnectionManager

In-memory CRUD store for `CloudConnection` configs, with JSON serialization for persistence. Thread-safe with `shared_mutex` (same pattern as `KeyManager`). Loaded from `connections.json` on startup.

**Files:** `application/cloud/cloudConnectionManager.h/cpp`

### CloudCredentials

Runtime transport bundle produced by connectors after resolving and refreshing stored credentials. Not persisted — this is what goes into HTTP headers.

### ICloudTaskExecutor

Base class for task executors that operate on cloud connections. The `Execute()` method:
1. Extracts the `connection` name from the task's params JSON
2. Looks up the `CloudConnection` in `CloudConnectionManager`
3. Gets the `ICloudConnector` from `CloudConnectorRegistry`
4. Calls `ResolveCredentials()` to get a `CloudCredentials` bundle
5. Delegates to `ExecuteCloud()` which subclasses implement

Includes a `TaskCancellationToken` for cooperative cancellation (no-op in Phase 0; Phase 9 wires it into the run cancel mechanism).

**Files:** `application/cloud/cloudTaskExecutor.h/cpp`, `application/cloud/taskCancellationToken.h`

---

## 3. Credential Hierarchy (Phase 0)

### ICredential

Abstract base for all credential types stored in `KeyManager`. Replaces the flat `ProviderConfig.m_ApiKey` with type-safe polymorphism.

| Subclass | `GetType()` | Fields | Use case |
|----------|-------------|--------|----------|
| `ApiKeyCredential` | `"api_key"` | `m_ApiKey` | API keys, PATs (OpenAI, Polarion, Slack) |
| `OAuthCredential` | `"oauth"` | `m_AccessToken`, `m_RefreshToken`, `m_ExpiresAt`, `m_Scopes` | OneDrive, Google, OAuth2 services |
| `KeyPairCredential` | `"key_pair"` | `m_PrivateKeyPem` | Snowflake JWT auth, service accounts |
| `BasicAuthCredential` | `"credentials"` | `m_Username`, `m_Password` | PostgreSQL, SMTP, IMAP |

**File:** `engine/keys/credential.h`

### KeyManager Extension

`ProviderConfig` now carries a `m_CredentialType` field (default `"api_key"`) plus type-specific fields. Serialization is backward-compatible: existing keys without a `credential_type` field load as `api_key`.

### OAuthTokenManager

Manages OAuth 2.0 token lifecycle: stores access/refresh tokens, tracks expiry, runs a background refresh loop (checks every 30s, refreshes tokens expiring within 5 minutes). Thread-safe. Registers tokens with `SecretRedactor` on acquisition.

**Persistence across restarts.** On `Start()`, the manager calls `HydrateFromKeyManager()` which walks all providers with `credential_type == "oauth"` and a non-empty `refresh_token` and seeds in-memory entries with the stored `refresh_token`, `token_endpoint`, `client_id`, and `client_secret`. The access_token itself is not persisted (short-lived). The hydrated entry has `m_ExpiresAt = 0`, so the first `GetAccessToken` call after startup performs an **on-demand synchronous refresh** using the stored refresh_token + client credentials, yielding a fresh access_token. This lets j9t restart without triggering a new user consent dialog.

On successful consent in the OAuth callback, `webServer.cpp` stores the tokens in `OAuthTokenManager` **and** writes `refresh_token` + `expires_at` + `scopes` + `token_endpoint` + `client_id` + `client_secret` into the `KeyManager::ProviderConfig`, then calls `KeyManager::Save()` to encrypt the updated registry into `keys.json.enc`. The master password is cached in `KeyManager` after successful `Load`/`Unlock`/`Save` so the callback can re-encrypt without re-prompting the user (it falls back to `JARVIS_MASTER_PASSWORD` if the cache is empty).

**Files:** `engine/keys/oauthTokenManager.h/cpp`, `engine/keys/keyManager.h/cpp`

### JwtGenerator

RSA RS256 JWT creation via OpenSSL `EVP_DigestSign`. Features:
- Minimum 2048-bit RSA key enforcement
- Base64URL encoding
- Snowflake convenience method with public key fingerprint computation
- Auto-registers generated JWTs with `SecretRedactor`

**Files:** `engine/keys/jwtGenerator.h/cpp`

---

## 4. Shared Utilities (Phase 0)

### CloudRetryPolicy

Centralized retry with exponential backoff + jitter. All cloud connectors and task executors use this instead of hand-rolled retry loops.

- Configurable: max retries (default 3), initial backoff (1s), max backoff (30s), jitter factor (20%)
- Respects `Retry-After` headers (clamped to max backoff)
- Retryable HTTP codes: 429, 500, 502, 503, 504 (configurable per connector)

**Files:** `application/cloud/cloudRetryPolicy.h/cpp`

### SecretRedactor

Thread-safe singleton that scrubs registered secret values from log output. Secrets shorter than 4 characters are ignored to prevent false positives. Integrated into the logging pipeline — `OAuthTokenManager`, `JwtGenerator`, and connectors register their secrets on acquisition.

**Files:** `engine/log/secretRedactor.h/cpp`

---

## 5. Connections REST API (Phase 0)

All endpoints are Studio-only.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/connections` | List all connections with type, endpoint, key, params |
| `POST` | `/api/connections` | Create a new connection |
| `PUT` | `/api/connections/{name}` | Update an existing connection (merge semantics) |
| `DELETE` | `/api/connections/{name}` | Delete a connection |
| `POST` | `/api/connections/{name}/test` | Test connectivity via the registered connector |
| `POST` | `/api/connections/save` | Persist to `connections.json` |

### GET /api/connections

**Response (200):**
```json
{
  "ok": true,
  "dirty": false,
  "connections": [
    {
      "name": "my-polarion",
      "type": "polarion",
      "endpoint": "https://polarion.company.com",
      "key_name": "polarion-pat",
      "auth_type": "bearer",
      "params": { "project_id": "GoKartProcurement" }
    }
  ]
}
```

### POST /api/connections

**Request body:**
```json
{
  "name": "my-s3",
  "type": "s3",
  "endpoint": "https://s3.amazonaws.com",
  "key_name": "aws-creds",
  "auth_type": "sigv4",
  "params": { "region": "us-east-1", "bucket": "workflow-outputs" }
}
```

**Response (201):** `{ "ok": true, "name": "my-s3" }`
**Response (409):** Connection already exists.

### POST /api/connections/{name}/test

Tests the connection using the registered `ICloudConnector::TestConnection()`.

**Response (200):** `{ "ok": true }`
**Response (400):** `{ "ok": false, "error": "test_failed", "message": "..." }`

---

## 6. Frontend Changes (Phase 0)

### Keys Page (formerly "AI Keys")

The "AI Keys" nav button is renamed to "Keys". The `ProvidersSettingsView` now includes a **Credential Type** dropdown with four options:
- **API Key** — shows API key input (same as before)
- **OAuth 2.0** — shows scopes input + note about Connections page
- **Key Pair (RSA)** — shows PEM textarea
- **Username / Password** — shows username + password inputs

### Connections Tab (new)

New "Connections" nav button between "Keys" and "Assistant" in the workflow editor. `ConnectionsView` provides:
- Connection list with type, endpoint, key reference
- **Test** button per connection (calls `/api/connections/{name}/test`)
- Edit form with type dropdown, endpoint, key name, auth type, and dynamic key-value params editor
- **Save** button persists to `connections.json`
- Dirty state indicator (`*`) in the nav button

**Files:**
- `workflow-editor/ui/src/views/ConnectionsView.tsx`
- `workflow-editor/ui/src/api/connections.ts`

---

## 7. MCP Interface (Phase 1)

### Architecture

Standalone TypeScript sidecar using `@modelcontextprotocol/sdk` (MIT). Communicates with j9t via REST API over localhost. Supports stdio (default) and SSE transports.

```
AI Assistant (Claude Desktop, Claude Code, etc.)
    |
    | MCP protocol (JSON-RPC over stdio or SSE)
    |
j9t MCP Server (TypeScript, mcp/ directory)
    |
    | REST API (localhost:8080, Bearer token auth)
    |
j9t Engine or Studio
```

### MCP Tools

| Tool | Maps to | Description |
|------|---------|-------------|
| `list_workflows` | `GET /api/workflows` | List available workflows with labels and flags |
| `run_workflow` | `POST /api/workflows/<id>/run` | Start a workflow with optional context bindings |
| `get_run_status` | `GET /api/workflow-runs/<runId>` | Per-task progress and state |
| `get_run_output` | `GET /api/workflow-runs/<runId>` | Completed workflow results |
| `list_active_runs` | `GET /api/workflow-runs/active` | Currently running/queued workflows |
| `cancel_run` | `POST /api/workflow-runs/<runId>/cancel` | Cancel a running workflow |

### MCP Resources

| Resource URI | Description |
|-------------|-------------|
| `workflow://list` | JSON list of all workflows with IDs, labels, and URIs |

### Configuration

Environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `J9T_URL` | `http://localhost:8080` | j9t base URL (port 8080 is the default; check `config.json` for the configured value) |
| `J9T_TOKEN` | *(empty)* | Bearer token for Engine auth |
| `J9T_TOKEN_FILE` | *(empty)* | Path to file containing the token |
| `J9T_MCP_TRANSPORT` | `stdio` | Transport: `stdio` or `sse` |
| `J9T_MCP_PORT` | `3100` | Port for SSE transport |

### Connecting from Claude Code

Add to your Claude Code MCP config (e.g., `~/.claude/settings.json` or project `.mcp.json`):

```json
{
  "mcpServers": {
    "j9t": {
      "command": "node",
      "args": ["<path-to-jarvisAgent>/mcp/dist/index.js"],
      "env": {
        "J9T_URL": "http://localhost:8080"
      }
    }
  }
}
```

### File Structure

```
mcp/
  package.json          — @modelcontextprotocol/sdk (MIT), tsx
  tsconfig.json         — ES2022, Node16, strict
  .gitignore            — node_modules/, dist/
  Dockerfile            — Node 22 slim for production
  src/
    index.ts            — Entry point, transport selection (stdio/SSE)
    config.ts           — Environment variable loading
    j9tClient.ts        — HTTP client for j9t REST API
    tools.ts            — MCP tool implementations
    resources.ts        — MCP resource implementations
```

### Dashboard Status Indicator

The MCP sidecar sends a periodic heartbeat (`POST /api/mcp/heartbeat`) to j9t every 15 seconds. The backend tracks the last heartbeat timestamp and exposes `mcp_connected` (boolean) and `mcp_last_heartbeat_secs_ago` (integer) in the `GET /api/status` response. The dashboard shows a purple LED labeled "MCP connected" when a heartbeat has been received within the last 35 seconds, or a gray "MCP offline" LED otherwise.

### Docker Deployment

The `docker-compose.example.yml` includes a `j9t_mcp` sidecar service that connects to `jarvis_agent` over the internal Docker network.

---

## 8. Polarion Enhancements (Phase 2)

### PolarionConnector

`PolarionConnector : ICloudConnector` wraps the existing `PolarionClient` into the cloud abstraction layer. Registered at startup in `jarvisAgent.cpp`.

- **GetType()** → `"polarion"`
- **TestConnection()** — performs a minimal query (page 1, size 1) to validate connectivity
- **ResolveCredentials()** — looks up the PAT from KeyManager via `m_KeyName`

CloudConnection params for Polarion:

| Key | Description |
|-----|-------------|
| `project_id` | Polarion project ID (required) |

Endpoint is the Polarion server base URL (e.g. `https://polarion.example.com/polarion`).

### PolarionClient Extensions

The existing `PolarionClient` now supports write operations:

| Method | REST Endpoint | Description |
|--------|--------------|-------------|
| `UpdateWorkItem()` | `PATCH /rest/v1/projects/{id}/workitems/{id}` | Update work item fields |
| `CreateWorkItem()` | `POST /rest/v1/projects/{id}/workitems` | Create new work item |
| `DownloadAttachment()` | `GET .../attachments/{id}/content` | Download attachment to local file |
| `UploadAttachment()` | `POST .../attachments` | Upload file as attachment (multipart) |
| `FetchLinkedWorkItems()` | `GET .../linkedworkitems` | Traceability traversal |

All methods use JSON:API (`application/vnd.api+json`) content type and Bearer token auth.

### polarion_write Task Type

JCWF task type `"polarion_write"` enables write operations in workflows. Uses the `ICloudTaskExecutor` base class for automatic connection/credential resolution.

**Task params:**

| Key | Required | Description |
|-----|----------|-------------|
| `connection` | yes | Named CloudConnection |
| `operation` | yes | `update`, `create`, `upload_attachment`, `download_attachment`, `linked_items` |
| `work_item_id` | for update/attachment/linked_items | Target work item ID |
| `body` | for update/create | JSON:API request body (alternative to `field_name` + `field_value`) |
| `field_name` | for update (convenience) | Attribute name to update; executor builds the JSON:API body internally with proper escaping |
| `field_value` | with `field_name` | Plain text value for the field |
| `field_value_file` | with `field_name` | Path to a file whose contents become the field value (takes precedence over `field_value`; supports per-item output piping via `{{taskId.output_file}}`) |
| `attachment_id` | for download_attachment | Attachment ID to download |
| `file_path` | for attachment operations | Local file path |
| `file_name` | optional for upload | Attachment filename (defaults to basename) |

Response is written to `response.json` in the task working directory and captured as stdout.

### Named Connection for polarion_query Filters

The `polarion_query` filter source now supports a `"connection"` field that references a named CloudConnection. When set, it overrides the inline `base_url`, `project_id`, and `key_name` fields. This centralizes Polarion server config in the Connections tab.

```json
{
  "id": "requirements",
  "source": {
    "kind": "polarion_query",
    "connection": "my-polarion",
    "query": "type:requirement AND status:approved",
    "fields": ["id", "title", "severity"]
  },
  "binding": "item"
}
```

Inline `base_url`/`project_id`/`key_name` still works for backward compatibility.

### Workflow Editor UI

- Task type dropdown includes `polarion_write`
- Inspector panel shows Polarion-specific fields (connection, operation, work_item_id, body, attachment fields) with a purple accent
- Filter builder dialog shows `connection` field for `polarion_query` sources, with hints that it overrides inline fields

---

## 9. S3 Object Storage (Phase 3)

### SigV4 Request Signing

`SigV4Signer` implements AWS Signature Version 4 using OpenSSL HMAC-SHA256. It produces the `Authorization`, `X-Amz-Date`, `X-Amz-Content-Sha256`, and `Host` headers required for S3 authentication.

Key methods:
- `Sign()` — signs an HTTP request, returns headers to add
- `Sha256Hex()` — SHA-256 hex digest of arbitrary data
- `EmptyPayloadHash()` — cached SHA-256 of empty string (used for GET/DELETE)

### S3Connector

`S3Connector : ICloudConnector` supports AWS S3 and S3-compatible services (MinIO, Wasabi, R2).

CloudConnection params:

| Key | Description |
|-----|-------------|
| `region` | AWS region, e.g. "us-east-1" (required) |
| `bucket` | Default bucket name (can be overridden per-task) |

- **Endpoint**: custom URL for S3-compatible services (empty = AWS default `https://{bucket}.s3.{region}.amazonaws.com`)
- **Auth type**: SigV4
- **Credentials**: stored as `username`/`password` (access key ID / secret key) or as `api_key` in `"ACCESS_KEY_ID:SECRET_KEY"` format

### S3 Task Type

JCWF task type `"s3"` supports four operations:

| Operation | Required params | Description |
|-----------|----------------|-------------|
| `upload` | `key`, `file_path` | Upload local file to S3 |
| `download` | `key`, `file_path` | Download S3 object to local file |
| `list` | `prefix` (optional) | List objects (XML response) |
| `delete` | `key` | Delete an S3 object |

All operations accept an optional `bucket` param (falls back to connection default). Response is written to `response.json` in the task working directory.

### s3_watch Trigger Type

JCWF trigger type `"s3_watch"` polls an S3 bucket at a configurable interval. On each poll interval, the trigger fires and the workflow runs — the workflow itself checks for new data.

```json
{
  "type": "s3_watch",
  "id": "watch-uploads",
  "params": {
    "connection": "my-s3",
    "bucket": "my-bucket",
    "prefix": "inbox/",
    "poll_interval_seconds": 300
  }
}
```

| Param | Default | Description |
|-------|---------|-------------|
| `connection` | (required) | Named S3 CloudConnection |
| `bucket` | connection default | Bucket to watch |
| `prefix` | empty (all) | Key prefix filter |
| `poll_interval_seconds` | 300 | Poll interval (minimum 60s) |

### Connections UI

The ConnectionsView shows dedicated fields for S3 connections (Region, Bucket) and Polarion connections (Project ID) alongside the generic parameters table.

### Example Workflow

`workflows/s3UploadDownloadDemo.jcwf` — creates a sample file, uploads it to S3, downloads it back, and lists the prefix. Requires an S3 connection named `my-s3`.

---

## 10. PostgreSQL Database (Phase 4)

### System Dependency

PostgreSQL support requires the `libpq` client library installed on the build system:

| Platform | Install command |
|----------|----------------|
| Ubuntu/Debian | `sudo apt install libpq-dev` |
| macOS (Homebrew) | `brew install libpq` |
| Windows | Install PostgreSQL or set `LIBPQ_DIR` environment variable |

The premake build uses `pkg-config` to discover libpq include paths and link flags on Linux/macOS. On Windows it checks `LIBPQ_DIR` or the default PostgreSQL install path.

### PostgresConnector

`PostgresConnector : ICloudConnector` uses the libpq C API directly (no C++ wrapper library).

CloudConnection params:

| Key | Description |
|-----|-------------|
| `database` | Database name (required) |
| `sslmode` | `disable`, `prefer` (default), `require`, `verify-ca`, `verify-full` |

- **Endpoint**: `host:port` (default `localhost:5432`)
- **Auth type**: BasicAuth (username/password from KeyManager)
- **TestConnection()**: connects and runs `SELECT 1`

### db_query Task Type

JCWF task type `"db_query"` executes SQL queries and writes results to disk.

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `postgres`) |
| `query` | yes | | SQL query string |
| `format` | no | `csv` | Output format: `csv` or `json` |
| `output_file` | no | `result.csv`/`result.json` | Output filename |

- CSV output follows RFC 4180 (proper quoting/escaping)
- JSON output is an array of objects with column names as keys
- NULL values produce empty CSV fields or JSON `null`

### Connections UI

The ConnectionsView shows dedicated fields for PostgreSQL connections: Database name and SSL Mode dropdown.

---

## 11. OneDrive Integration (Phase 5)

### OneDriveConnector

Implements `ICloudConnector` for Microsoft OneDrive via the Microsoft Graph API.

**Files:** `application/cloud/oneDriveConnector.h/cpp`

**Connection params:**

| Key | Description |
|-----|-------------|
| `client_id` | Azure AD application (client) ID (required) |
| `tenant_id` | Azure AD tenant ID (default: `"common"` for multi-tenant) |
| `scopes` | OAuth scopes (default: `"Files.ReadWrite offline_access"`) |

- **Endpoint**: Graph API base URL (default `https://graph.microsoft.com/v1.0`)
- **Auth type**: OAuth2 (tokens managed by `OAuthTokenManager`)
- **TestConnection()**: calls `GET /me/drive` to verify token and return drive info

### Generic OAuth 2.0 Authorization Code Flow with PKCE

The `/api/connections/<name>/oauth/authorize` and `/oauth/callback` handlers in `webServer.cpp` are **provider-agnostic**. They look up the connector for the connection's type via `CloudConnectorRegistry` and call `connector->GetOAuth2ProviderInfo(connection, info)`, which returns a per-provider `OAuth2ProviderInfo` struct:

```cpp
struct OAuth2ProviderInfo
{
    std::string m_AuthorizeUrl;              // e.g. https://accounts.google.com/o/oauth2/v2/auth
    std::string m_TokenUrl;                  // e.g. https://oauth2.googleapis.com/token
    std::string m_DefaultScopes;             // space-separated scopes
    std::map<std::string, std::string> m_ExtraAuthorizeParams;  // provider quirks
    bool m_RequiresClientSecret;             // Google: true, Microsoft PKCE: false
};
```

Current implementations:

| Connector | Authorize URL | Token URL | Default scopes | Client secret required |
|-----------|---------------|-----------|----------------|------------------------|
| OneDrive | `login.microsoftonline.com/{tenant}/oauth2/v2.0/authorize` | `login.microsoftonline.com/{tenant}/oauth2/v2.0/token` | `Files.ReadWrite offline_access` | No (PKCE public client) |
| Google Sheets | `accounts.google.com/o/oauth2/v2/auth` | `oauth2.googleapis.com/token` | `https://www.googleapis.com/auth/spreadsheets` | Yes |

Google Sheets additionally sets `access_type=offline&prompt=consent&include_granted_scopes=true` as extra authorize params — without these Google omits the refresh token on re-consent.

**Flow (generic):**

1. Frontend or client calls `GET /api/connections/{name}/oauth/authorize`
2. Backend generates a RFC 7636-compliant `code_verifier` (43 chars, base64url of 32 random bytes) and derives `code_challenge = BASE64URL(SHA256(code_verifier))`, plus a random CSRF state token. All three are stored in-memory per connection.
3. Backend looks up the connector and builds the provider's authorize URL with `client_id`, `redirect_uri`, `scope`, `code_challenge`, `state`, plus any provider-specific extra params.
4. Client opens the returned URL in a browser; user consents.
5. Provider redirects to `GET /api/connections/{name}/oauth/callback?code=...&state=...`
6. Backend validates the state token, retrieves the stored `code_verifier`, and POSTs a token exchange to the connector's `TokenUrl`. For confidential clients (`m_RequiresClientSecret`), `client_secret` from `connection.m_Params` is included in the POST body.
7. Tokens are stored in `OAuthTokenManager` **and** persisted to `keys.json.enc` via `KeyManager::Save()` (using the cached master password). On the next j9t startup, `HydrateFromKeyManager()` restores the refresh token and an on-demand refresh in `GetAccessToken` fetches a fresh access token — no user re-consent required.

**Redirect URI scheme** follows the server's TLS configuration: if `config.m_TlsCert` and `m_TlsKey` are set the redirect is `https://localhost:<port>/...`, otherwise `http://`. Pre-Phase-10 code hardcoded `http://` which broke every TLS-enabled deployment silently.

**Token refresh:** `OAuthTokenManager` runs a background thread that checks every 30 seconds and refreshes tokens 5 minutes before expiry. `GetAccessToken` also performs synchronous on-demand refresh when the in-memory entry is empty or expired — this covers hydrated-from-disk entries on the first call after startup.

**Files:** `engine/keys/oauthTokenManager.h/cpp`, `engine/keys/keyManager.h/cpp`, `application/web/webServer.cpp`, `application/cloud/cloudConnector.h` (`OAuth2ProviderInfo`), `application/cloud/oneDriveConnector.h/cpp`, `application/cloud/googleSheetsConnector.h/cpp`

### OneDrive Task Types

JCWF task types `"onedrive_upload"` and `"onedrive_download"` perform file operations via the Graph API.

| Param | Required | Description |
|-------|----------|-------------|
| `connection` | yes | Named CloudConnection (type `onedrive`) |
| `operation` | yes | `"upload"` or `"download"` |
| `remote_path` | yes | OneDrive path (e.g., `"Documents/reports/output.pdf"`) |
| `local_path` | yes | Local file path relative to task working directory |

- **Upload**: `PUT /me/drive/root:/{remote_path}:/content` with file body
- **Download**: `GET /me/drive/root:/{remote_path}:/content` to local file

**Files:** `application/cloud/oneDriveCloudTaskExecutor.h/cpp`

### onedrive_watch Trigger

Polls a OneDrive folder on a configurable interval. Fires the workflow on each poll; the workflow itself determines whether there are new/changed files.

**Trigger params:**

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `onedrive`) |
| `folder` | no | root | OneDrive folder path to watch |
| `poll_interval_seconds` | no | 300 | Polling interval (minimum 60) |

The trigger instance stores a `delta_token` field for future use with the Graph delta query API (`GET /me/drive/root:/{folder}:/delta`), which returns only changed items since the last sync.

### Connections UI

The ConnectionsView shows dedicated fields for OneDrive connections:
- **Client ID** — Azure AD application ID
- **Tenant ID** — Azure AD tenant (default: "common")
- **Scopes** — OAuth permission scopes
- **Authorize with Microsoft** button — initiates the OAuth PKCE flow in a popup window (only shown for saved connections)

### Task Inspector

The workflow editor shows a OneDrive task inspector panel (blue accent) for `onedrive_upload` and `onedrive_download` task types with fields: connection, operation, remote_path, local_path.

---

## 12. Snowflake Integration (Phase 6)

### SnowflakeConnector

Implements `ICloudConnector` for Snowflake via the Snowflake SQL REST API with RSA JWT authentication.

**Files:** `application/cloud/snowflakeConnector.h/cpp`

**Connection params:**

| Key | Description |
|-----|-------------|
| `account` | Snowflake account identifier (e.g. `"xy12345"`), required |
| `user` | Snowflake user name (e.g. `"SVC_JARVIS"`), required |
| `warehouse` | Default warehouse (e.g. `"COMPUTE_WH"`) |
| `database` | Default database (e.g. `"ANALYTICS"`) |
| `schema` | Default schema (e.g. `"PUBLIC"`) |

- **Endpoint**: Account locator with region (e.g. `"xy12345.us-east-1"`), used to construct the API URL `https://{endpoint}.snowflakecomputing.com`
- **Auth type**: JwtRsa (RSA RS256 JWT via `JwtGenerator`)
- **Key**: KeyPairCredential with RSA private key in PEM format
- **TestConnection()**: `POST /api/v2/statements` with `SELECT 1`

### JWT Authentication

Snowflake uses RSA key-pair authentication. The `JwtGenerator::GenerateSnowflakeJwt()` method (built in Phase 0):

1. Uppercases account and user per Snowflake convention
2. Extracts the public key fingerprint (SHA-256 of DER-encoded public key)
3. Builds JWT claims: `iss` (account + user + fingerprint), `sub` (account + user), `iat`, `exp` (1-hour)
4. Signs with RS256 via OpenSSL `EVP_DigestSign`

The `X-Snowflake-Authorization-Token-Type: KEYPAIR_JWT` header tells Snowflake to expect JWT auth.

### snowflake_query Task Type

JCWF task type `"snowflake_query"` executes SQL queries via the Snowflake SQL REST API with async polling.

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `snowflake`) |
| `query` | yes | | SQL statement |
| `warehouse` | no | connection default | Override warehouse |
| `database` | no | connection default | Override database |
| `schema` | no | connection default | Override schema |
| `output_format` | no | `csv` | `csv` or `json` |
| `output_file` | no | `result.csv`/`result.json` | Output filename |
| `timeout` | no | 3600 | Statement timeout in seconds |
| `poll_interval` | no | 2 | Polling interval in seconds |

**Execution flow:**
1. `POST /api/v2/statements` with SQL body — receives `statementHandle`
2. Poll `GET /api/v2/statements/{handle}` until `message == "Statement executed successfully."`
3. Parse `resultSetMetaData.rowType` for column names, `data` array for row values (jsonv2 format)
4. Write results to CSV (RFC 4180) or JSON (array of objects) in the task working directory
5. Raw Snowflake response saved to `response.json`

Supports cooperative cancellation: if the workflow run is cancelled, the executor sends `POST /api/v2/statements/{handle}/cancel` to Snowflake.

**Files:** `application/cloud/snowflakeCloudTaskExecutor.h/cpp`

### Connections UI

The ConnectionsView shows dedicated fields for Snowflake connections: Account, User, Warehouse, Database, Schema.

### Task Inspector

The workflow editor shows a Snowflake Query inspector panel (light blue accent) with fields: connection, query (SQL textarea), warehouse, database, schema, output_format, output_file.

---

## 13. Messaging — Slack and Email (Phase 7)

### SlackConnector

Implements `ICloudConnector` for the Slack Web API with Bearer token authentication.

**Files:** `application/cloud/slackConnector.h/cpp`

- **Endpoint**: Slack API base URL (default: `https://slack.com/api`)
- **Auth type**: BearerToken (Bot token `xoxb-...` from KeyManager)
- **TestConnection()**: `POST /api/auth.test` — verifies token and returns workspace info

### slack_message Task Type

JCWF task type `"slack_message"` sends messages to Slack channels via `POST /api/chat.postMessage`.

| Param | Required | Description |
|-------|----------|-------------|
| `connection` | yes | Named CloudConnection (type `slack`) |
| `channel` | yes | Slack channel name or ID (e.g. `#alerts`, `C01ABCDEF`) |
| `text` | yes (or `text_file`) | Inline message text (supports template variables) |
| `text_file` | yes (or `text`) | Read message text from file (relative to j9t cwd); trims trailing whitespace. Use for long AI-generated content (mirrors `email_send` `body_file`). |
| `thread_ts` | no | Post as a threaded reply to this parent message ts. |
| `thread_ts_file` | no | Read `thread_ts` from file. Used to wire an upstream `slack_read` task's `latest_ts.txt` into the reply. |

Checks Slack's `{"ok": true/false}` response for success.

### slack_read Task Type

JCWF task type `"slack_read"` reads recent messages from a Slack channel via `GET /api/conversations.history`. Requires the bot token to have the `channels:history` scope (plus `channels:read` to resolve channel names, if desired).

| Param | Required | Description |
|-------|----------|-------------|
| `connection` | yes | Named CloudConnection (type `slack`) |
| `channel` | yes | Channel **ID** (e.g. `C01ABCDEF`). Must not start with `#` — Slack's history API requires the ID, not the name. |
| `limit` | no (default `10`) | Max messages to fetch |
| `oldest` | no | Only return messages with `ts` strictly greater than this (for incremental polling) |
| `exclude_bots` | no (default `true`) | Skip messages that have a `bot_id` field. Essential to avoid self-reply loops when the same bot also posts into the channel. |

Outputs written to the task working directory:

| File | Content |
|---|---|
| `messages_summary.json` | Array of `{ts, user, text}` for all fetched non-bot messages (newest first). |
| `latest_message.txt` | Text of the most recent message after filtering (suitable as `cntx_files` for a downstream `ai_call`). |
| `latest_ts.txt` | `ts` of the most recent message after filtering (suitable as `thread_ts_file` for a downstream `slack_message`). |
| `response.json` | `{ok, count, channel, latest_ts}` |

See `example/workflows/slackQAndABot.md` for a full read → AI → threaded reply round-trip walkthrough.

**Files:** `application/cloud/slackCloudTaskExecutor.h/cpp`

### EmailConnector

Implements `ICloudConnector` for SMTP send and IMAP read via libcurl.

**Files:** `application/cloud/emailConnector.h/cpp`

**Connection params:**

| Key | Description |
|-----|-------------|
| `smtp_host` | SMTP server (e.g. `"smtp.gmail.com"`), required for send |
| `smtp_port` | SMTP port (default: `"587"` for STARTTLS, `"465"` for SSL) |
| `imap_host` | IMAP server (e.g. `"imap.gmail.com"`), required for email_watch |
| `imap_port` | IMAP port (default: `"993"`) |
| `from` | Sender address (default: credential username) |
| `use_ssl` | `"true"` (default) or `"false"` |

- **Auth type**: BasicAuth (email + password/app password from KeyManager)
- **TestConnection()**: SMTP `CONNECT_ONLY` handshake via libcurl

### email_send Task Type

JCWF task type `"email_send"` sends emails via SMTP with optional attachments.

| Param | Required | Description |
|-------|----------|-------------|
| `connection` | yes | Named CloudConnection (type `email`) |
| `to` | yes | Recipient(s), comma-separated |
| `subject` | yes | Email subject (supports template variables) |
| `body` | yes* | Email body text |
| `body_file` | no | Path to file whose contents become the body (takes precedence over `body`) |
| `cc` | no | CC recipients, comma-separated |
| `attachments` | no | Array of file paths relative to working directory |

*Required unless `body_file` is provided.

Builds RFC 2822 messages with MIME multipart for attachments. Base64-encodes attachment content. Uses libcurl SMTP with `CURLOPT_MAIL_FROM` and `CURLOPT_MAIL_RCPT`.

**Files:** `application/cloud/emailCloudTaskExecutor.h/cpp`

### email_read Task Type

JCWF task type `"email_read"` fetches emails from an IMAP mailbox via libcurl.

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection with `imap_host`/`imap_port` params |
| `folder` | no | `INBOX` | IMAP folder to read from |
| `max_messages` | no | `10` | Maximum messages to fetch |
| `subject_filter` | no | | Only include messages whose subject contains this string |

**Outputs:**
- `emails_summary.json` — JSON array of fetched messages, each with `uid`, `from`, `to`, `subject`, `date`, `body`
- `response.json` — `{"ok": true, "count": N, "folder": "INBOX"}`
- `captured_stdout` — the summary JSON (up to 1024 chars)

Uses libcurl IMAP with `SEARCH ALL` to find messages, then `FETCH` by UID. Parses RFC 2822 headers (From, To, Subject, Date) and extracts the plain text body. Respects `use_ssl` connection param (`imap://` vs `imaps://`).

**Files:** `application/cloud/emailCloudTaskExecutor.h/cpp`

### email_watch Trigger

Polls an IMAP folder on a configurable interval.

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `email`) |
| `folder` | no | `INBOX` | IMAP folder to watch |
| `subject_filter` | no | all | Subject pattern filter |
| `poll_interval_seconds` | no | 300 | Polling interval (minimum 60) |

Stores `last_seen_uid` for future efficient polling via IMAP UID.

### Connections UI

- **Slack**: No dedicated fields (just the generic connection name, key, auth type)
- **Email**: Dedicated fields for SMTP Host, SMTP Port, IMAP Host, From address

### Task Inspector

- **slack_message**: Pink accent panel with connection, channel, text (textarea)
- **email_send**: Orange accent panel with connection, to, subject, body (textarea), cc

---

## 14. Additional Integrations (Phase 8)

### GitHubConnector

Implements `ICloudConnector` for GitHub (and GitLab/GitHub Enterprise) via REST API with Bearer token (PAT).

**Files:** `application/cloud/gitHubConnector.h/cpp`

| Key | Description |
|-----|-------------|
| `owner` | Default repository owner / organization |
| `repo` | Default repository name |

- **Endpoint**: API base URL (default: `https://api.github.com`)
- **Auth type**: BearerToken (Personal Access Token)
- **TestConnection()**: `GET /user`

### github_issue Task Type

| Operation | Description |
|-----------|-------------|
| `create` | Create issue with title, body, labels |
| `comment` | Comment on issue/PR by number |
| `close` | Close an issue |
| `get_file` | Get file content from repo (base64-decoded to disk) |
| `list_issues` | List open issues to JSON |

**Files:** `application/cloud/gitHubCloudTaskExecutor.h/cpp`

### JiraConnector

Implements `ICloudConnector` for Jira REST API v3. Supports BasicAuth (Jira Cloud: email + API token) and BearerToken (Jira Data Center: PAT).

**Files:** `application/cloud/jiraConnector.h/cpp`

| Key | Description |
|-----|-------------|
| `project_key` | Default Jira project key |

- **TestConnection()**: `GET /rest/api/3/myself`

### jira_issue Task Type

| Operation | Description |
|-----------|-------------|
| `create` | Create issue with summary, description (ADF), type, priority, labels |
| `update` | Update fields on existing issue |
| `transition` | Transition issue status |
| `comment` | Add comment (ADF format) |
| `get` | Get issue details to JSON |

**create params:**

| Key | Required | Description |
|-----|----------|-------------|
| `connection` | yes | Named CloudConnection |
| `operation` | yes | `create` |
| `project_key` | yes (or on connection) | Jira project key |
| `summary` | yes | Issue title |
| `issue_type` | no | Default `Task` (e.g. `Bug`, `Story`) |
| `description` | no | Inline ADF description text |
| `description_file` | no | Path to a file whose contents become the description (takes precedence over `description`; supports AI output piping via `{{upstreamTask.output_file}}`). Newlines are collapsed to spaces since ADF text nodes cannot contain embedded newlines. |
| `priority` | no | Priority name (e.g. `High`) |
| `labels` | no | Array of label strings |

All operations write the Jira API response to `response.json` in the task working directory (per-item children use `response_<N>.json`). The response is captured as stdout and parsed for JSON-path template resolution — downstream tasks can reference `{{create_issue.json.key}}` to chain issue operations.

**Files:** `application/cloud/jiraCloudTaskExecutor.h/cpp`

### GoogleSheetsConnector

Implements `ICloudConnector` for Google Sheets API v4. Supports API key auth (read-only public sheets) or OAuth2 (read/write private sheets).

**Files:** `application/cloud/googleSheetsConnector.h/cpp`

| Key | Description |
|-----|-------------|
| `spreadsheet_id` | Default Google Sheets spreadsheet ID |

- **TestConnection()**: `GET /{spreadsheet_id}?fields=properties.title`

### sheets_read / sheets_write Task Types

- **sheets_read**: `GET /{spreadsheetId}/values/{range}` — parses values array, writes CSV or JSON
- **sheets_write**: `PUT /{spreadsheetId}/values/{range}` — reads local CSV, uploads as JSON values array

**Files:** `application/cloud/googleSheetsCloudTaskExecutor.h/cpp`

### Connections UI

- **GitHub**: owner, repo fields
- **Jira**: project_key field
- **Google Sheets**: spreadsheet_id field

### Task Inspector

- **github_issue**: Gray accent — operation dropdown, owner, repo, title, body, issue_number, path
- **jira_issue**: Blue accent — operation dropdown, project_key, summary, description, issue_type, issue_key, body
- **sheets_read/sheets_write**: Green accent — spreadsheet_id, range, format/file fields

---

## 15. Hardening (Phase 9)

### Runtime Resilience (9a)

- **CloudCircuitBreaker** — per-connection circuit breaker (Closed/Open/HalfOpen state machine). Tracks consecutive failures, short-circuits requests during outages, auto-recovers after 60s cooldown. Wired into `ICloudTaskExecutor::Execute()` — all cloud tasks automatically benefit.
- **CloudConnectionPool** — generic connection pool for persistent-connection providers (PostgreSQL libpq, future IMAP). Health-checks on acquire, evicts stale connections after 5 minutes idle.
- **TaskCancellationToken** — wired into `WorkflowRun`. When `POST /api/workflow-runs/{runId}/cancel` is called, the token propagates to in-flight cloud tasks. Snowflake executor already checks this during async polling and sends a cancel request to Snowflake.
- **ProviderRateLimitPolicy** — extends `CloudRetryPolicy` with per-provider rate-limit awareness (minimum request intervals, burst limits per window).
- **Connection health in /api/status** — circuit breaker state exposed as `connection_health` array in the status response. Dashboard shows a Cloud health LED (green/yellow/red).

### Security & Audit (9b)

- **Audit logging** — all cloud task executions logged to `log/security.txt` with task ID, connection name, type, and run ID.
- **OAuth CSRF protection** — `state` parameter added to OAuth authorize URL, validated on callback. Random 16-byte token per flow.
- **Download size limits** — `CURLOPT_MAXFILESIZE_LARGE` set to 256 MB on S3 and OneDrive download operations.
- **Path traversal validation** — `ICloudTaskExecutor::ValidateLocalPath()` rejects `local_path` params containing `..` or resolving outside the task working directory. Logged to security log.
- **RSA key minimum 2048 bits** — enforced in `JwtGenerator::Generate()` (Phase 0).
- **SSL verification** — `CURLOPT_SSL_VERIFYPEER` is never disabled; libcurl defaults to enabled.

### Deployment & Ops (9c)

#### Outbound Firewall Rules

| Integration | Endpoints | Ports |
|-------------|-----------|-------|
| Polarion | `polarion.company.com` (on-prem) | 443 |
| S3 (AWS) | `*.s3.{region}.amazonaws.com` | 443 |
| S3 (MinIO) | Custom endpoint | Configured port |
| OneDrive | `login.microsoftonline.com`, `graph.microsoft.com` | 443 |
| Snowflake | `{account}.snowflakecomputing.com` | 443 |
| Slack | `slack.com` | 443 |
| Email (SMTP) | SMTP server (e.g. `smtp.gmail.com`) | 587, 465 |
| Email (IMAP) | IMAP server (e.g. `imap.gmail.com`) | 993 |
| GitHub | `api.github.com` | 443 |
| Jira Cloud | `*.atlassian.net` | 443 |
| Google Sheets | `sheets.googleapis.com` | 443 |
| PostgreSQL | Database server | 5432 |

All connections use TLS (HTTPS or STARTTLS). No plaintext HTTP connections to cloud services.

#### Container Image Signing

For production container deployments, images should be signed using [cosign](https://github.com/sigstore/cosign) (Apache-2.0 license):

```bash
# Sign after build
cosign sign --key cosign.key ghcr.io/myorg/j9t:latest

# Verify before deploy
cosign verify --key cosign.pub ghcr.io/myorg/j9t:latest
```

The Docker build (`docker-compose.example.yml`) produces unsigned images by default. Production deployments should add signing to their CI/CD pipeline.

---

## 16. Per-Item Output Piping in Cloud Tasks

Cloud task executors support `{{...}}` template variable expansion in their `params` JSON. This enables per_item cloud write-back pipelines where each downstream instance consumes the output of its corresponding upstream instance.

**Available variables** (when per_item task B depends on per_item task A, same filter):

| Variable | Description |
|----------|-------------|
| `{{A.output_file}}` | Absolute path to A's first output file for the matching item |
| `{{A.captured_stdout}}` | Captured stdout (up to 1024 chars) from A's matching instance |
| `{{A.<slotName>}}` | Named output slot value from A's matching instance |
| `{{binding.field}}` | Filter binding variables (e.g. `{{dept.department}}`) |

**JSON safety**: All template values substituted into cloud task params are JSON-escaped (double quotes, backslashes, newlines). This prevents AI-generated text from breaking the JSON structure.

**Tested round-trip patterns**:

- **PostgreSQL**: query → per_item AI analyze → per_item INSERT via `{{ai_analyze.captured_stdout}}` (dollar-quoting for SQL safety)
- **GitHub**: list issues → per_item AI triage → per_item comment via `{{ai_triage.captured_stdout}}`
- **Snowflake**: query → per_item AI classify → per_item INSERT via `{{ai_analyze.captured_stdout}}` + `{{ai_analyze.output_file}}` (ready, needs Snowflake account)

---

## 17. Cloud Storage — Azure Blob / GCS Native (Phase 11)

The S3 connector (Phase 3) covers AWS S3, MinIO, and GCS in S3-interop mode (HMAC keys + SigV4). Phase 11 adds native connectors for Azure Blob Storage and Google Cloud Storage, supporting their standard auth methods and full API feature sets.

### AzureSharedKeySigner

`AzureSharedKeySigner` implements Azure Storage Shared Key request signing via OpenSSL HMAC-SHA256. It produces the `Authorization`, `x-ms-date`, and `x-ms-version` headers required for Azure Blob Storage REST API authentication.

The signing format differs from AWS SigV4:
- Authorization header: `SharedKey {accountName}:{Base64(HMAC-SHA256(key, StringToSign))}`
- Uses RFC 1123 date format (not ISO-8601)
- Canonicalized headers include all `x-ms-*` headers
- Canonicalized resource includes query parameters sorted by name

**Files:** `application/cloud/azureSharedKeySigner.h/cpp`

### AzureBlobConnector

`AzureBlobConnector : ICloudConnector` supports Azure Blob Storage with two auth methods:

- **Shared Key** (`azure_shared_key`): HMAC-SHA256 signing via `AzureSharedKeySigner`. Account key stored as `api_key` (Base64-encoded) or `password` in KeyManager.
- **Azure AD OAuth2** (`oauth2`): Bearer token from `OAuthTokenManager` with Azure AD token endpoint and scope `https://storage.azure.com/.default`.

CloudConnection params:

| Key | Description |
|-----|-------------|
| `account_name` | Azure Storage account name (required) |
| `container` | Default blob container (can be overridden per-task) |

- **Endpoint**: `https://{account_name}.blob.core.windows.net` (default) or custom URL (e.g., `http://127.0.0.1:10000/devstoreaccount1` for Azurite)
- **TestConnection()**: `GET /{container}?restype=container` — checks container exists and credentials work

**Files:** `application/cloud/azureBlobConnector.h/cpp`

### Azure Blob Task Types

JCWF task types `"azure_blob_upload"` and `"azure_blob_download"` perform blob operations.

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `azure_blob`) |
| `operation` | yes | | `"upload"` or `"download"` |
| `container` | no | connection default | Blob container name |
| `blob_name` | yes | | Blob path within container |
| `local_path` | yes | | Local file path |

- **Upload**: `PUT /{container}/{blob_name}` with `x-ms-blob-type: BlockBlob` header
- **Download**: `GET /{container}/{blob_name}` to local file

**Files:** `application/cloud/azureBlobCloudTaskExecutor.h/cpp`

### azure_blob_watch Trigger

Polls on configurable interval. Fires the workflow on each poll; the workflow itself determines whether there are new blobs.

| Param | Default | Description |
|-------|---------|-------------|
| `connection` | (required) | Named CloudConnection (type `azure_blob`) |
| `container` | connection default | Container to watch |
| `prefix` | empty (all) | Blob name prefix filter |
| `poll_interval_seconds` | 300 | Poll interval (minimum 60s) |

### GcsConnector

`GcsConnector : ICloudConnector` supports Google Cloud Storage via the GCS JSON API with service account JWT authentication.

**Auth flow:** Service account private key (PEM, stored as `KeyPairCredential` in KeyManager) → `JwtGenerator` creates a self-signed JWT with scope `https://www.googleapis.com/auth/devstorage.read_write` → exchange JWT for OAuth2 access token via `POST https://oauth2.googleapis.com/token` → token cached with 55-minute TTL (refresh 5 min before 1-hour expiry).

For local testing with fake-gcs-server, the JWT is used directly as a bearer token (token exchange is skipped).

CloudConnection params:

| Key | Description |
|-----|-------------|
| `bucket` | Default GCS bucket (can be overridden per-task) |
| `service_account_email` | Service account email (used in JWT `iss` claim) |

- **Endpoint**: `https://storage.googleapis.com` (default) or `http://localhost:4443` (fake-gcs-server)
- **Auth type**: JwtRsa
- **TestConnection()**: `GET /storage/v1/b/{bucket}` — checks bucket exists and credentials work

**Files:** `application/cloud/gcsConnector.h/cpp`

### GCS Task Types

JCWF task types `"gcs_upload"` and `"gcs_download"` perform object operations via the GCS JSON API.

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `gcs`) |
| `operation` | yes | | `"upload"` or `"download"` |
| `bucket` | no | connection default | GCS bucket name |
| `object_name` | yes | | Object path within bucket |
| `local_path` | yes | | Local file path |

- **Upload**: `POST /upload/storage/v1/b/{bucket}/o?uploadType=media&name={object_name}` with file body
- **Download**: `GET /storage/v1/b/{bucket}/o/{object_name}?alt=media` to local file

Object names with special characters are URL-encoded.

**Files:** `application/cloud/gcsCloudTaskExecutor.h/cpp`

### gcs_watch Trigger

Polls on configurable interval. Fires the workflow on each poll; the workflow itself determines whether there are new objects.

| Param | Default | Description |
|-------|---------|-------------|
| `connection` | (required) | Named CloudConnection (type `gcs`) |
| `bucket` | connection default | Bucket to watch |
| `prefix` | empty (all) | Object name prefix filter |
| `poll_interval_seconds` | 300 | Poll interval (minimum 60s) |

### Connections UI

The ConnectionsView shows dedicated fields for:
- **Azure Blob** connections: Account Name, Container
- **GCS** connections: Bucket, Service Account Email

### Task Inspector

The workflow editor shows:
- **Azure Blob** task inspector (blue accent, `rgba(0,120,212)`) for `azure_blob_upload` and `azure_blob_download` — fields: connection, operation, container, blob_name, local_path
- **GCS** task inspector (Google blue accent, `rgba(66,133,244)`) for `gcs_upload` and `gcs_download` — fields: connection, operation, bucket, object_name, local_path

### Local Testing

| Provider | Docker Command | Notes |
|----------|---------------|-------|
| Azure Blob | `docker run -d --name azurite -p 10000:10000 -p 10001:10001 -p 10002:10002 mcr.microsoft.com/azure-storage/azurite` | Azurite emulator: Blob on 10000. Default account: `devstoreaccount1`, well-known dev key |
| GCS | `docker run -d --name fake-gcs -p 4443:4443 fsouza/fake-gcs-server -scheme http` | fake-gcs-server for local testing; create bucket via REST |

---

## 18. Next Steps

See `cloud-integration-dev-plan.md` for the complete roadmap through Phase 11.
