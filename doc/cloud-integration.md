# Cloud Integration

This document covers the cloud integration layer introduced in j9t, including the abstraction framework, credential management, MCP interface, and connection management.

---

## 1. Overview

The cloud integration layer provides a uniform interface for connecting j9t workflows to external cloud services (Polarion, S3, OneDrive, Snowflake, PostgreSQL, Slack, Email, GitHub, Jira, Redmine, Azure Blob Storage, Google Cloud Storage). All cloud code in the C++ backend uses libcurl + OpenSSL + simdjson — Python is reserved for workflow task scripts only.

**Design principles:**
- **Disk-first** — all cloud data (query results, downloaded files, API responses) is written to the workflow working directory before downstream tasks consume it.
- **GPL-3.0 compliance** — all libraries linked into j9t are GPL-3.0 compatible. Cloud services are accessed via open REST/HTTP protocols.
- **Credential safety** — secrets are stored in the encrypted key store (`keys.json.enc`) and never appear in JCWF files or log output.

**Relationship to the AI pipeline:** The existing AI interface code (`CurlMultiDispatcher`, `aiRequestPool`, `aiCallTaskExecutor`, AI Manager UI) is a separate domain with its own architecture and must **not** be refactored into `ICloudConnector`. Three cloud foundation utilities do benefit the AI pipeline: `ICredential` hierarchy (type-safe key storage), `SecretRedactor` (scrubs AI API keys from logs), and `OAuthTokenManager` + `JwtGenerator` (enables future Google Vertex AI support via service account OAuth2/JWT auth).

---

## 2. Cloud Abstraction Layer (Phase 0)

### ICloudConnector

Abstract interface for all cloud service connectors. Each connector implements:
- `GetType()` — returns the connector type name (e.g., `"polarion"`, `"s3"`)
- `TestConnection()` — validates connectivity with given config
- `ResolveCredentials()` — resolves stored credentials into a runtime `CloudCredentials` bundle

**File:** `code/backend/application/cloud/cloudConnector.h`

### CloudConnectorRegistry

Registry for connector plugins. Connectors register themselves at startup; task executors and the REST API look them up by type name.

**Files:** `code/backend/application/cloud/cloudConnectorRegistry.h/cpp`

### CloudConnection

Configuration for a named cloud connection, persisted in the master-password-encrypted `connections.json.enc`:

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

In-memory CRUD store for `CloudConnection` configs, with JSON serialization for persistence. Thread-safe with `shared_mutex` (same pattern as `KeyManager`). Loaded from the AES-256-GCM-encrypted `connections.json.enc` at unlock (`LoadEncrypted`) — moved out of the old plaintext `connections.json` so connection endpoint URLs + credential references can't be tampered with at rest without the master password. Mutations persist via `SaveEncrypted`.

**Files:** `code/backend/application/cloud/cloudConnectionManager.h/cpp`

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

**Files:** `code/backend/application/cloud/cloudTaskExecutor.h/cpp`, `code/backend/application/cloud/taskCancellationToken.h`

---

## 3. Credential Hierarchy (Phase 0)

### ICredential

Abstract base for all credential types stored in `KeyManager`. Type-safe polymorphism with `SecureString`-backed secret fields (mlock'd, zero-on-destruct).  Consumers `dynamic_cast` to the expected concrete subtype with fail-closed null-check on type mismatch — allowlist discipline (the cast either matches the expected type or refuses).

| Subclass | `GetType()` | Secret fields (`SecureString`) | Non-secret fields | Use case |
|----------|-------------|-------------------------------|-------------------|----------|
| `ApiKeyCredential`     | `"api_key"`     | `m_ApiKey` | — | Bearer / x-api-key / x-goog-api-key / api-key — OpenAI, Anthropic, Gemini, Azure, GitHub PAT, Slack bot, Polarion |
| `OAuthCredential`      | `"oauth"`       | `m_AccessToken`, `m_RefreshToken`, `m_ClientSecret` | `m_ExpiresAt`, `m_Scopes`, `m_TokenEndpoint`, `m_ClientId` | OneDrive, Google Sheets, OAuth2 services |
| `KeyPairCredential`    | `"key_pair"`    | `m_PrivateKeyPem` | — | Snowflake JWT auth, GCS service accounts |
| `BasicAuthCredential`  | `"credentials"` | `m_Password` | `m_Username` (logged for audit) | PostgreSQL, SMTP, IMAP |
| `AwsCredential`        | `"aws"`         | `m_SecretAccessKey`, `m_SessionToken` | `m_AccessKeyId` (public per AWS conventions; CloudTrail logs it), `m_Region` | AWS Bedrock, S3 (SigV4) |

Common metadata fields on `ICredential` base: `m_Name`, `m_DisplayName`, `m_Endpoint`, `m_DefaultModel`, `m_ApiType`, `m_Params` (string map for non-secret per-provider extras like Azure resource/deployment).  `m_Params` is intentionally public for extensibility; secrets must NOT be stored there — use the typed `SecureString` fields on the concrete subclass.

**File:** `code/backend/engine/keys/credential.{h,cpp}`

### KeyManager API

`KeyManager` owns the credential registry as `unordered_map<string, unique_ptr<ICredential>>`.  Read access via callbacks (`WithCredential(name, fn)` / `WithDefaultCredential(fn)`) — the callback receives `ICredential const&` and runs while a shared_lock is held, so the reference is guaranteed live for the call's duration but MUST NOT be stored beyond it.  Existence checks via `HasCredential(name)`.  Write access via `AddCredential(name, unique_ptr<ICredential>)` (CREATE), `ModifyCredential(name, mutator)` (atomic read-modify-write — closes the race window between read and write where a concurrent `RemoveProvider` would dangle), `UpsertCredential(name, builder)` (atomic add-or-update — used by the OAuth callback), and `RemoveProvider(name)`.  REST handlers build typed credentials via `CredentialFactory::CreateFromJson` (CREATE) or `CredentialFactory::CloneAndPatch` (UPDATE — partial-patch semantics, ignores `credential_type` field; DELETE + CREATE for type changes).  Serialization is backward-compatible: existing keys without a `credential_type` field load as `"api_key"`.

### OAuthTokenManager

Manages OAuth 2.0 token lifecycle: stores access/refresh tokens, tracks expiry, runs a background refresh loop (checks every 30s, refreshes tokens expiring within 5 minutes). Thread-safe. Registers tokens with `SecretRedactor` on acquisition.  See `doc/cyber security.md` "OAuthTokenManager Security" for the full safety guarantees (URL-encoding, refresh-failure backoff, snapshot-then-apply pattern, race-safe `Start`, `RemoveTokens` API).

**Persistence across restarts.** On `Start()`, the manager calls `HydrateFromKeyManager()` which walks all providers with `credential_type == "oauth"` and a non-empty `refresh_token` and seeds in-memory entries with the stored `refresh_token`, `token_endpoint`, `client_id`, and `client_secret`. The access_token itself is not persisted (short-lived). The hydrated entry has `m_ExpiresAt = 0`, so the first `GetAccessToken` call after startup performs an **on-demand synchronous refresh** using the stored refresh_token + client credentials, yielding a fresh access_token. This lets j9t restart without triggering a new user consent dialog.

On successful consent in the OAuth callback, `webServer.cpp` stores the tokens in `OAuthTokenManager` **and** calls `KeyManager::UpsertCredential(connection.m_KeyName, builder)`, where the builder receives the existing credential (or `nullptr` if new) and produces an `OAuthCredential` (preserving common metadata from any existing entry of any subtype, then writing `refresh_token` + `expires_at` + `scopes` + `token_endpoint` + `client_id` + `client_secret` into the typed fields).  `UpsertCredential` performs the add-or-update atomically under one `unique_lock`, eliminating any race window between the read and the write.  The callback then calls `KeyManager::Save()` to encrypt the updated registry into `keys.json.enc`. The master password is held in mlock-protected memory by `KeyManager` after a successful `Load` / `Unlock` / `Save` so the callback can re-encrypt without re-prompting. If the admin hasn't unlocked the key store yet this session, the OAuth tokens are held in memory only and the callback logs a warning — persistence resumes on the next `POST /api/settings/keys/unlock`.

**Files:** `code/backend/engine/keys/oauthTokenManager.h/cpp`, `code/backend/engine/keys/keyManager.h/cpp`

### JwtGenerator

RSA RS256 JWT creation via OpenSSL `EVP_DigestSign`. Features:
- Algorithm pinned to RS256 — header `{"alg":"RS256","typ":"JWT"}` is built internally by `Generate(payloadJson, privateKeyPem, errorMessage)`; callers cannot pass a header that lies about the algorithm
- Minimum 2048-bit RSA key enforcement, plus explicit `EVP_PKEY_id == EVP_PKEY_RSA` rejection of EC / DSA / Ed25519 keys
- Exception-safe via file-local `EvpPkeyPtr` / `EvpMdCtxPtr` RAII wrappers
- Base64URL encoding
- Snowflake convenience method with public key fingerprint computation
- Auto-registers generated JWTs with `SecretRedactor`

See `doc/cyber security.md` "JwtGenerator Security" for the alg-confusion threat model and design rationale.

**Files:** `code/backend/engine/keys/jwtGenerator.h/cpp`

---

## 4. Shared Utilities (Phase 0)

### CloudRetryPolicy

Centralized retry with exponential backoff + jitter. All cloud connectors and task executors use this instead of hand-rolled retry loops.

- Configurable: max retries (default 3), initial backoff (1s), max backoff (30s), jitter factor (20%)
- Respects `Retry-After` headers (clamped to max backoff)
- Retryable HTTP codes: 429, 500, 502, 503, 504 (configurable per connector)

**Files:** `code/backend/application/cloud/cloudRetryPolicy.h/cpp`

### SecretRedactor

Thread-safe singleton that scrubs registered secret values from log output before any spdlog sink writes them.  Wired in via a `RedactingFormatter` wrapping each sink's `pattern_formatter` — `log/log.txt`, the rotating `log/security.txt`, and the ncurses TUI all receive redacted text.  Cloud integration registers secrets at acquisition: `KeyManager` (per-credential-subtype virtual `RegisterSecrets()` invoked on every load / add / update), `OAuthTokenManager` (refresh / access / client secret tokens with the wiring-sandwich pattern on rotation), and `JwtGenerator` (signed JWTs).  See `doc/cyber security.md` "SecretRedactor" for the full coverage table and design boundaries.

**Files:** `code/backend/engine/log/secretRedactor.h/cpp`

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
| `POST` | `/api/connections/save` | Re-persist `connections.json.enc` (master-password re-auth required; mutations already persist per-call) |

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

### Keys Page

The **Keys** nav button opens `ProvidersSettingsView`, which includes a **Credential Type** dropdown with four options:
- **API Key** — shows the API key input
- **OAuth 2.0** — shows scopes input + note about Connections page
- **Key Pair (RSA)** — shows PEM textarea
- **Username / Password** — shows username + password inputs

### Connections Tab (new)

New "Connections" nav button between "Keys" and "Assistant" in the workflow editor. `ConnectionsView` provides:
- Connection list with type, endpoint, key reference
- **Test** button per connection (calls `/api/connections/{name}/test`) with green/red LED indicators showing the test result on each connection row
- Modal dialogs for connection and key editing (replacing earlier inline card layout)
- Edit form with type dropdown, endpoint, key name, auth type, and dynamic key-value params editor
- **Save** button re-persists `connections.json.enc` (requires master-password re-auth; create/update/delete already persist per-mutation)
- Dirty state indicator (`*`) in the nav button

**Files:**
- `code/frontend/shared-ui/views/ConnectionsView.tsx`
- `code/frontend/shared-ui/api/connections.ts`

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

`base_url`/`project_id`/`key_name` may also be specified inline on the task instead of through a named connection.

### Workflow Editor UI

- Task type dropdown includes `polarion_write`
- Inspector panel shows Polarion-specific fields (connection, operation, work_item_id, body, attachment fields) with a purple accent
- Filter builder dialog shows `connection` field for `polarion_query` sources, with hints that it overrides inline fields

---

## 9. S3 Object Storage (Phase 3)

### SigV4 Request Signing

`SigV4Signer` implements AWS Signature Version 4 using OpenSSL HMAC-SHA256. It produces the `Authorization`, `X-Amz-Date`, `X-Amz-Content-Sha256`, and `Host` headers required for S3 authentication.  See `doc/cyber security.md` "SigV4 Signing Security" for the full safety guarantees (4-test debug-build self-test, `OPENSSL_cleanse` of intermediate signing keys, OpenSSL primitive return-checks, file-local `IAuthSigner::Get` non-fallback).

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

### Local Testing

| Provider | Docker Command | Notes |
|----------|---------------|-------|
| S3 (MinIO) | `docker run -d --name minio -p 9000:9000 -p 9001:9001 -e MINIO_ROOT_USER=minioadmin -e MINIO_ROOT_PASSWORD=minioadmin123 minio/minio server /data --console-address ":9001"` | S3-compatible on port 9000, console on 9001. Create bucket: `docker exec minio mc mb /data/j9t-test` |

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
| `sslmode` | `disable`, `allow`, `prefer`, `require` (**default**), `verify-ca`, `verify-full`.  For non-localhost hosts the three plaintext-fallback modes (`disable` / `allow` / `prefer`) are rejected — only the TLS-required modes are accepted (mirrors email's `allowLocal = !useSsl` posture).  For local-network hosts (loopback / RFC 1918 / link-local), all 6 modes are accepted as a dev-mode opt-out. |

- **Endpoint**: `host:port` (default `localhost:5432`).  Bracketed IPv6 literals `[fc00::1]:5432` are supported — the brackets are stripped before host extraction so `IsLocalNetworkHost` recognizes the address.
- **Auth type**: BasicAuth (username/password from KeyManager)
- **TestConnection()**: connects and runs `SELECT 1`
- **Forbidden libpq params** — `m_Params` keys that would resolve to local file paths or external file lookups are rejected before any libpq call: `sslcert`, `sslkey`, `sslrootcert`, `sslcrl`, `sslcrldir`, `sslpassword`, `service`, `passfile`.  j9t's posture is "credentials live in KeyManager, not on disk" — any libpq param that asks the server to read a path is a path-traversal vector, so the validator (`PostgresConnector::ValidatePostgresParams`) rejects with `[security] postgres_forbidden_param` before reaching libpq.  `BuildConnectParams` only forwards `database` and `sslmode` to libpq; the validator is preventive — if a future PR adds `paramOrDefault("sslcert", ...)` to `BuildConnectParams` without first removing this gate, the gate fires and the security log catches it.

**Choosing an `sslmode` value:**

- **`require`** is the right floor for almost any j9t deployment.  TLS is mandatory; libpq won't fall back to plaintext.  Cert chain is NOT validated, so an attacker who can MITM the connection AND present a self-signed cert can intercept — but a passive eavesdropper can't.  Use `require` for local development (the typical pg dev install ships a self-signed cert that doesn't validate against a CA chain) and for production deployments where you accept the MITM-with-valid-cert risk for simpler ops.
- **`verify-ca`** validates the cert chain against a CA bundle.  Requires the bundle to be available at libpq's default search path (`~/.postgresql/root.crt`) or in the OS trust store accessed via libpq's `sslrootcert=system` (which j9t's forbid-list does NOT block since it's a literal string, not a path — `sslrootcert` itself is forbidden, but the special `=system` value applies through libpq's connection-config layer separately).  Doesn't validate hostname.
- **`verify-full`** validates BOTH the cert chain AND the hostname.  Same CA-bundle requirement as `verify-ca` plus the cert's CN (or SAN) must match the connection's host.  This is the production posture for managed pg services (Supabase, RDS, Cloud SQL, etc.) where the cert chain rolls up to a public CA already in the OS trust store and the hostname is a real DNS name.  For self-signed local-pg dev (CN typically the machine hostname, not `localhost`), `verify-full` won't work without per-machine CA provisioning — accept `require` for dev and use `verify-full` for production.

### db_query Task Type

JCWF task type `"db_query"` executes SQL queries and writes results to disk.

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `postgres`) |
| `query` | yes | | SQL query string (operator-authored — see trust model below) |
| `format` | no | `csv` | Output format: `csv` or `json` |
| `output_file` | no | `result.csv`/`result.json` | Output filename — **bare filename only**, no path separators (rejected) |
| `max_rows` | no | `100000` (ceiling `1000000`) | Refuse to write if the result set has more rows |
| `max_output_bytes` | no | `100MB` (ceiling `1GB`) | Refuse to keep writing if the output file exceeds this size |
| `statement_timeout_ms` | no | `60000` (ceiling `600000`) | Server-side `SET statement_timeout` enforced before the user query |

- CSV output follows RFC 4180 (proper quoting/escaping)
- JSON output is an array of objects with column names as keys
- NULL values produce empty CSV fields or JSON `null`

**Trust model.**  `query` is operator-authored — the workflow author writes the SQL.  Defense-in-depth lives in three layers: (1) operator gate at submission (db_query workflows reach the runtime only via JCWFs the operator authored or approved); (2) DB-side permissions (operators MUST configure the connection's DB user with the minimum permissions required — read-only for read-only workloads); (3) blast-radius caps above (`max_rows` / `max_output_bytes` / `statement_timeout_ms` with hard ceilings the JCWF can't override).  String-level "validation" inside the executor would either reject legitimate queries or miss a clever payload — DB-side permissions are the durable defense.

### Connections UI

The ConnectionsView shows dedicated fields for PostgreSQL connections: Database name and SSL Mode dropdown.

---

## 11. OneDrive Integration (Phase 5)

### OneDriveConnector

Implements `ICloudConnector` for Microsoft OneDrive via the Microsoft Graph API.

**Files:** `code/backend/application/cloud/oneDriveConnector.h/cpp`

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
7. Tokens are stored in `OAuthTokenManager` **and** persisted to `keys.json.enc` via `KeyManager::Save()` (using the master password held in mlock memory after unlock). On the next j9t startup, `HydrateFromKeyManager()` restores the refresh token and an on-demand refresh in `GetAccessToken` fetches a fresh access token — no user re-consent required.

> **Auto-provisioning:** if no `KeyManager` provider with `connection.m_KeyName` exists at callback time, the callback now auto-creates one (`credential_type: "oauth"`) before persisting. Earlier code only updated existing providers, so an `oauth2` connection authored in the React editor (or via REST) without a manually pre-created same-named entry in the providers/keys view would silently end up with memory-only tokens that vanished on restart. The callback logs `OAuth callback: auto-creating KeyManager provider '<name>' for connection '<conn>'` whenever it fires this path.

**Redirect URI scheme** follows the server's TLS configuration: if `config.m_TlsCert` and `m_TlsKey` are set the redirect is `https://localhost:<port>/...`, otherwise `http://`. Pre-Phase-10 code hardcoded `http://` which broke every TLS-enabled deployment silently.

**Token refresh:** `OAuthTokenManager` runs a background thread that checks every 30 seconds and refreshes tokens 5 minutes before expiry. `GetAccessToken` also performs synchronous on-demand refresh when the in-memory entry is empty or expired — this covers hydrated-from-disk entries on the first call after startup.

**Files:** `code/backend/engine/keys/oauthTokenManager.h/cpp`, `code/backend/engine/keys/keyManager.h/cpp`, `code/backend/application/web/webServer.cpp`, `code/backend/application/cloud/cloudConnector.h` (`OAuth2ProviderInfo`), `code/backend/application/cloud/oneDriveConnector.h/cpp`, `code/backend/application/cloud/googleSheetsConnector.h/cpp`

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

**Files:** `code/backend/application/cloud/oneDriveCloudTaskExecutor.h/cpp`

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

**Files:** `code/backend/application/cloud/snowflakeConnector.h/cpp`

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

**Files:** `code/backend/application/cloud/snowflakeCloudTaskExecutor.h/cpp`

### Connections UI

The ConnectionsView shows dedicated fields for Snowflake connections: Account, User, Warehouse, Database, Schema.

### Task Inspector

The workflow editor shows a Snowflake Query inspector panel (light blue accent) with fields: connection, query (SQL textarea), warehouse, database, schema, output_format, output_file.

### Implementation Notes

- **User-Agent header** — Both `SnowflakeConnector` and `SnowflakeCloudTaskExecutor` set `User-Agent: j9t/1.0` on every request. The Snowflake SQL REST API requires a `User-Agent` header; requests without one are rejected.
- **JWT fingerprint encoding** — The `RSA_PUBLIC_KEY_FP` value in the JWT `iss` claim uses **standard Base64** (with `+`, `/`, `=` padding), not Base64URL. Snowflake expects standard encoding for the fingerprint even though the JWT header/payload/signature use Base64URL per RFC 7519.
- **Single-statement requests** — Each REST API request must contain exactly one SQL statement. Multi-statement execution is not supported unless the `MULTI_STATEMENT_COUNT` parameter is explicitly set in the request body, which the executor does not currently use.

---

## 13. Messaging — Slack and Email (Phase 7)

### SlackConnector

Implements `ICloudConnector` for the Slack Web API with Bearer token authentication.

**Files:** `code/backend/application/cloud/slackConnector.h/cpp`

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
| `text_file` | yes (or `text`) | Read message text from file (resolved per JCWF spec §3.2.1: relative paths under the task `working_directory`; absolute values from `{{<task>.output_file}}` templates pass through); trims trailing whitespace. Use for long AI-generated content (mirrors `email_send` `body_file`). |
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

**Files:** `code/backend/application/cloud/slackCloudTaskExecutor.h/cpp`

### EmailConnector

Implements `ICloudConnector` for SMTP send and IMAP read via libcurl.

**Files:** `code/backend/application/cloud/emailConnector.h/cpp`

**Connection params:**

| Key | Description |
|-----|-------------|
| `smtp_host` | SMTP server (e.g. `"smtp.gmail.com"`), required for send |
| `smtp_port` | SMTP port (default: `"587"` for STARTTLS, `"465"` for SSL) |
| `imap_host` | IMAP server (e.g. `"imap.gmail.com"`), required for email_watch |
| `imap_port` | IMAP port (default: `"993"`) |
| `from` | Sender address (default: credential username) |
| `use_ssl` | `"true"` (default) or `"false"`.  Gates strict TLS on **both** SMTP and IMAP — when `"true"`, the connection refuses to proceed without TLS and enforces full certificate + hostname verification.  Setting `"false"` is the local-testing escape hatch (GreenMail / Mailpit on plaintext ports); each plaintext send emits a `[security] email_send_tls_disabled` line in `log/security.txt` so an operator can audit which connections opt out. |

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

**Files:** `code/backend/application/cloud/emailCloudTaskExecutor.h/cpp`

### email_read Task Type

JCWF task type `"email_read"` fetches emails from an IMAP mailbox via libcurl.

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection with `imap_host`/`imap_port` params |
| `folder` | no | `INBOX` | IMAP folder to read from |
| `max_messages` | no | `10` | Maximum messages to fetch (clamped to `[1, 500]`) |
| `subject_filter` | no | | Only include messages whose subject contains this string |

**Outputs:**
- `emails_summary.json` — JSON array of fetched messages, each with `uid`, `from`, `to`, `subject`, `date`, `body`
- `response.json` — `{"ok": true, "count": N, "folder": "INBOX"}`
- `captured_stdout` — the summary JSON (up to 1024 chars)

Uses libcurl IMAP with `SEARCH ALL` to find messages, then `FETCH` by UID. Parses RFC 2822 headers (From, To, Subject, Date) and extracts the plain text body. Respects `use_ssl` connection param (`imap://` vs `imaps://`).

**Files:** `code/backend/application/cloud/emailCloudTaskExecutor.h/cpp`

### email_watch Trigger

Polls an IMAP folder on a configurable interval.

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `email`) |
| `folder` | no | `INBOX` | IMAP folder to watch |
| `subject_filter` | no | all | Subject pattern filter |
| `poll_interval_seconds` | no | 300 | Polling interval (minimum 60) |

`EmailConnector::CheckForNewMail()` runs on each poll interval:

1. Opens an IMAP connection, issues `STATUS folder (UIDVALIDITY)` to capture the mailbox's current UIDVALIDITY (RFC 3501 §2.3.1.1), then `SEARCH ALL` on the configured folder (uses `SEARCH ALL` instead of `UID SEARCH` for broad IMAP server compatibility, e.g. GreenMail).
2. Compares returned UIDs against the stored `m_LastSeenUid` watermark.
3. On the **first poll**, seeds the watermark to the highest UID silently — does not fire the trigger for pre-existing mail.
4. On subsequent polls, fires the trigger only when UIDs strictly greater than the watermark are found, then advances the watermark.
5. IMAP network I/O runs **outside the trigger engine mutex** to avoid blocking other triggers during potentially slow connections.
6. Watermarks persist across restart at `<queue_folder>/.email_watermarks.json` (atomic-rename per successful poll); mail that arrived during a restart window still fires the trigger on the next poll instead of being absorbed into the seed baseline.

**Watermark integrity.**  UIDs are only meaningful within `(server, mailbox, UIDVALIDITY)`.  The persisted file (`format_version: 2`) records `connection_name`, `folder`, and `uid_validity` alongside `last_seen_uid` as load-bearing fields:

- **Registration-time guard** — `TriggerEngine::AddEmailWatchTrigger` discards the saved UID with a WARN when the JCWF has been edited to repoint a trigger to a different connection or folder.  Without this, a UID from server A would be applied to server B's UID space (silent under/over-fire).
- **Poll-time guard** — if the current mailbox UIDVALIDITY differs from the saved one (mailbox rename / delete+recreate / server restore from backup), the in-memory watermark is discarded with a WARN and the next poll re-seeds from current state without firing for pre-existing mail.
- **Format gate** — `format_version: 1` files (the pre-UIDVALIDITY shape) are discarded at load with a WARN per `feedback_no_legacy`; no compat shim.  Triggers re-seed from current IMAP state on the next poll.
- **Orphan prune** — before each write, `SavePersistedEmailWatermarksLocked` drops any persisted entry whose `(workflowId, triggerId)` no longer matches a live `m_EmailWatchTriggers` entry, so a removed workflow's watermark doesn't accumulate in the file indefinitely.  Workflow removal goes through `ClearAll()` + re-register, so the prune fires on the next surviving trigger's save (the survivors are re-registered before any save runs).

### Connections UI

- **Slack**: No dedicated fields (just the generic connection name, key, auth type)
- **Email**: Dedicated fields for SMTP Host, SMTP Port, IMAP Host, From address

### Task Inspector

- **slack_message**: Pink accent panel with connection, channel, text (textarea)
- **email_send**: Orange accent panel with connection, to, subject, body (textarea), cc

### Local Testing

| Provider | Docker Command | Notes |
|----------|---------------|-------|
| Email (SMTP mock) | `docker run -d --name mailpit -p 1025:1025 -p 8025:8025 axllent/mailpit` | Mailpit: SMTP on 1025, web UI on 8025 (no auth needed) |
| Email (IMAP mock) | `docker run -d --name greenmail -p 3025:3025 -p 3110:3110 -p 3143:3143 -e GREENMAIL_OPTS='-Dgreenmail.setup.test.all -Dgreenmail.users=test:test' greenmail/standalone` | GreenMail: SMTP 3025, POP3 3110, IMAP 3143. User `test`, pw `test`. Send test email via SMTP, poll via IMAP. |

---

## 14. Additional Integrations (Phase 8)

### GitHubConnector

Implements `ICloudConnector` for GitHub (and GitLab/GitHub Enterprise) via REST API with Bearer token (PAT).

**Files:** `code/backend/application/cloud/gitHubConnector.h/cpp`

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

**Files:** `code/backend/application/cloud/gitHubCloudTaskExecutor.h/cpp`

### JiraConnector

Implements `ICloudConnector` for Jira REST API v3. Supports BasicAuth (Jira Cloud: email + API token) and BearerToken (Jira Data Center: PAT).

**Files:** `code/backend/application/cloud/jiraConnector.h/cpp`

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

**Files:** `code/backend/application/cloud/jiraCloudTaskExecutor.h/cpp`

### GoogleSheetsConnector

Implements `ICloudConnector` for Google Sheets API v4. Supports API key auth (read-only public sheets) or OAuth2 (read/write private sheets).

**Files:** `code/backend/application/cloud/googleSheetsConnector.h/cpp`

| Key | Description |
|-----|-------------|
| `spreadsheet_id` | Default Google Sheets spreadsheet ID |

- **TestConnection()**: `GET /{spreadsheet_id}?fields=properties.title`

### sheets_read / sheets_write Task Types

- **sheets_read**: `GET /{spreadsheetId}/values/{range}` — parses values array, writes CSV or JSON
- **sheets_write**: `PUT /{spreadsheetId}/values/{range}` — reads local CSV, uploads as JSON values array

**Files:** `code/backend/application/cloud/googleSheetsCloudTaskExecutor.h/cpp`

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

- **CloudCircuitBreaker** — per-connection circuit breaker (Closed/Open/HalfOpen state machine). Tracks consecutive failures, short-circuits requests during outages, auto-recovers after 60s cooldown. Wired into `ICloudTaskExecutor::Execute()` — all cloud tasks automatically benefit. Each executor reports a typed `ConnectorErrorCode` on failure; `IsConnectionFailure(code)` keeps app-level cap rejections (`ValueOutOfRange`) from ticking the failure counter so an operator hitting a cap doesn't self-trip the breaker on a healthy connection.
- **CloudConnectionPool** — generic connection pool for persistent-connection providers (PostgreSQL libpq, future IMAP). Health-checks on acquire, evicts stale connections after 5 minutes idle.
- **TaskCancellationToken** — wired into `WorkflowRun`. When `POST /api/workflow-runs/{runId}/cancel` is called, the token propagates to in-flight cloud tasks. Snowflake executor already checks this during async polling and sends a cancel request to Snowflake.
- **ProviderRateLimitPolicy** — extends `CloudRetryPolicy` with per-provider rate-limit awareness (minimum request intervals, burst limits per window).
- **Connection health in /api/status** — circuit breaker state exposed as `connection_health` array in the status response. Dashboard shows a Cloud health LED (green/yellow/red).

### Security & Audit (9b)

- **Audit logging** — all cloud task executions logged to `log/security.txt` with task ID, connection name, type, and run ID.
- **OAuth CSRF protection** — `state` parameter added to OAuth authorize URL, validated on callback. Random 16-byte token per flow.
- **Resource caps (downloads + uploads + responses)** — Phase 9 set `CURLOPT_MAXFILESIZE_LARGE = 256 MB` on S3 and OneDrive **download** operations.  Subsequently extended (sittings 15-20) with matching **upload** caps (256 MB on S3, GCS, Azure Blob, OneDrive uploads — symmetric with downloads) and **response-body caps** in writeCallbacks (64 MB on the 5 cloud-storage executors; 10 MB on email IMAP; 64 MB on Snowflake; 1 MB on Snowflake's TestConnection; 25 MB on email attachments).
- **Path traversal validation** — `ICloudTaskExecutor::ValidateLocalPath()` is the security gate for every `local_path` / `file_path` / `body_file` / `attachments` param across the cloud surface.  Resolution follows JCWF spec §3.2.1: a relative path resolves under the caller-supplied base (the task `working_directory` for every executor), an absolute path (typically a `{{<task>.output_file}}` template) passes through.  Both forms are then confined under the JarvisAgent launch CWD as the project-tree security boundary — anything resolving outside the project (e.g. `/etc/passwd`) is rejected.  The 6 cloud executors with local-file params (azureBlob, email, gcs, oneDrive, s3, sheets) all share this gate.  Each rejection emits a `[security] path_traversal_blocked` line to `log/security.txt`.
- **RSA key minimum 2048 bits** — enforced in `JwtGenerator::Generate()` (Phase 0).
- **TLS verification** — `CURLOPT_SSL_VERIFYPEER = 1L` and `CURLOPT_SSL_VERIFYHOST = 2L` are set **explicitly** (not relying on libcurl defaults) across the entire cloud surface: email + Snowflake + the 6 executor data paths (azureBlob, gcs, googleSheets, oneDrive, s3, sheets) AND every `*Connector::TestConnection` HTTP path via the shared `ConnectorHttp::ApplyHardenedDefaults()` helper.  When the connection's `use_ssl` param is `"true"` (default for email; HTTP cloud surfaces are HTTPS-only and always verify), the gate is unconditional; the email surface's `use_ssl: "false"` opt-out emits a `[security] email_*_tls_disabled` log line so an operator running plaintext sees the deviation.
- **Connector-layer SSRF gate (two layers)** — every connector's `TestConnection` validates a user-supplied endpoint URL via two complementary gates:
  1. **Syntactic** — `ConnectorHttp::ValidatePublicHttpEndpoint()` runs before any network I/O.  Enforces scheme (http/https only), conservative host charset (alphanumeric + `.` + `-`), and rejects IP literals in RFC 1918 / loopback / link-local / cloud-metadata ranges via `IsLocalNetworkHost()` (which uses a structural IPv6 classifier so public hostnames starting with `fc` / `fd` / `fe80` aren't false-positive flagged).  Rejected endpoints emit `[security] <type>_endpoint_rejected` lines.
  2. **DNS-resolution-time** — `ConnectorHttp::ApplyHardenedDefaults()` and `ApplyExecutorRedirectDefaults()` install a `CURLOPT_OPENSOCKETFUNCTION` callback when the URL scheme is `https://`.  The callback fires after libcurl resolves DNS but before TCP connect; if the resolved IP is in the local-network ranges, it returns `CURL_SOCKET_BAD` and the request fails.  Closes the SSRF vector where a public DNS name (`evil.example.com`) resolves to an internal IP at attacker-controlled DNS time.  Rejections emit `[security] dns_resolved_ip_local_network_rejected resolved_ip='...'`.
  Plain `http://` URLs allow local-network hosts as a dev-mode opt-out at both gates (mirrors email's `allowLocal = !useSsl` heuristic); production `https://` does not.
- **Redirect-following has a deliberate per-API posture across the cloud surface** — set via two shared helpers in `ConnectorHttp`:
  - `ApplyHardenedDefaults()` enforces `CURLOPT_FOLLOWLOCATION = 0L`.  Used on every HTTP-based `TestConnection`, every `PolarionClient::Http*` call, and the 10 executor data paths whose vendor APIs respond directly: azureBlob, gcs, gitHub, googleSheets, jira, redmine, sheets, slack, snowflake.  A 30x from any of these is treated as hostile — the redirect-amplified SSRF vector would otherwise leak the bearer/PAT to an attacker-controlled host.  `emailConnector` and `emailCloudTaskExecutor` use libcurl SMTP/IMAP rather than HTTP and have their own `use_ssl`-gated TLS-verify dance — they do NOT route through `ApplyHardenedDefaults` (different protocol family).
  - `ApplyExecutorRedirectDefaults()` enforces `CURLOPT_FOLLOWLOCATION = 1L` + `CURLOPT_REDIR_PROTOCOLS_STR = "https"` (no http downgrade) + `CURLOPT_MAXREDIRS = 10L`.  Used only where the vendor API legitimately 30x's on the data path: the 4 sites in S3 (cross-region 301s, presigned-URL flows) and Microsoft Graph (CDN 302s on download, large-file upload session pivots).  An unencrypted-protocol redirect target is refused by libcurl as a hard error; the follow-depth cap prevents an attacker-controlled redirect loop pinning a worker thread.
  Both helpers also set explicit `CURLOPT_SSL_VERIFYPEER = 1L` + `CURLOPT_SSL_VERIFYHOST = 2L` and `CAINFO` from `CurlWrapper::GetCaBundlePath()` if available.
- **Live counters for every gate** — every cloud-surface security gate has both a security log line and an atomic lifetime counter on `/api/debug/signals` (DEBUG-builds, admin-gated).  Six counters total: `cloud_dns_resolved_ip_rejections`, `cloud_endpoint_ssrf_rejections`, `cloud_credential_crlf_rejections`, `cloud_input_validation_rejections`, `cloud_postgres_invalid_sslmode_rejections`, `cloud_postgres_forbidden_param_rejections`.  Counters reset to 0 on server restart.  See `doc/cyber security.md` § "Cloud Connection Security" for which gate each counter covers.

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

**Files:** `code/backend/application/cloud/azureSharedKeySigner.h/cpp`

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

**Files:** `code/backend/application/cloud/azureBlobConnector.h/cpp`

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

**Files:** `code/backend/application/cloud/azureBlobCloudTaskExecutor.h/cpp`

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

**Files:** `code/backend/application/cloud/gcsConnector.h/cpp`

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

**Files:** `code/backend/application/cloud/gcsCloudTaskExecutor.h/cpp`

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

## 18. FOSS Connectors — Redmine (Phase 12)

Redmine is a self-hosted open-source project management tool with a well-documented REST API. The Redmine connector complements the proprietary GitHub and Jira connectors with a FOSS option that runs entirely from a Docker image — no signup, no credit card, no API quota, no rate limits. It uses an `X-Redmine-API-Key` header for authentication (one key per Redmine user, generated under My account once the REST web service is enabled in Administration → Settings → API).

### RedmineConnector

**Files:** `code/backend/application/cloud/redmineConnector.h/cpp`

- **Endpoint**: Redmine instance URL (e.g. `http://localhost:3000`)
- **Auth**: API key stored in KeyManager; the connector unconditionally builds an `X-Redmine-API-Key` header from the credential token (the `auth_type` field on the connection is set to `bearer` purely because the existing `CloudAuthType` enum has no dedicated ApiKey value).
- **TestConnection()**: `GET /users/current.json` — verifies the key and returns the calling user.
- **Connection params**:
  - `project_identifier` — default Redmine project identifier (e.g. `j9t-demo`); used by `list_issues` when the task doesn't override it.

### redmine_issue Task Type

JCWF task type `"redmine_issue"` with two operations:

#### `list_issues`

`GET /issues.json?project_id=<ident>&status_id=<status>&limit=<N>` — fetches open issues from one Redmine project.

| Param | Required | Default | Description |
|---|---|---|---|
| `connection` | yes | — | Named CloudConnection (type `redmine`) |
| `operation` | yes | — | `"list_issues"` |
| `project_identifier` | no | from connection params | Redmine project identifier |
| `status` | no | `"open"` | `"open"`, `"closed"`, or `"*"` |
| `limit` | no | `25` | Max issues to return |

Writes the raw response body to `response.json` in the working directory. The `count` of issues + the JSON shape (`{"issues":[...]}`) match Redmine's REST conventions — feed it to a small python step to project into a CSV the `csv` filter can consume.

#### `update_issue`

`PUT /issues/{id}.json` with `{"issue":{"notes":"...","assigned_to_id":N}}` — atomically posts a comment and/or sets the assignee on an existing issue.

| Param | Required | Description |
|---|---|---|
| `connection` | yes | Named CloudConnection (type `redmine`) |
| `operation` | yes | `"update_issue"` |
| `issue_id` | yes | Integer Redmine issue id (accepts string or int in JSON) |
| `notes` | no* | Inline comment text |
| `notes_file` | no* | Read comment text from file (resolved per JCWF spec §3.2.1: relative paths under task `working_directory`; absolute template values pass through) |
| `assigned_to_id` | no* | Inline numeric Redmine user id |
| `assigned_to_id_file` | no* | Read assignee id from file (one integer per line) |

\*at least one of notes/notes_file/assigned_to_id/assigned_to_id_file must be present.

Redmine returns 204 No Content on success; the executor synthesizes `{"ok":true,"operation":"update_issue","issue_id":N}` so downstream tasks see something useful.

**Files:** `code/backend/application/cloud/redmineCloudTaskExecutor.h/cpp`

### Local Test Infrastructure

```bash
docker run -d --name redmine -p 3000:3000 redmine
```

Wait ~30 seconds for Rails to boot. On first login at http://localhost:3000 sign in as `admin`/`admin`, change the password, then go to **Administration** and click **Load the default configuration** to seed trackers, statuses, workflows, roles, and enumerations. **Without this step**, attempts to create issues will fail with cascading "Default status cannot be blank" / "Role cannot be empty" errors. After loading defaults, enable REST in **Administration → Settings → API**, then generate an API key under **My account → API access key → Reset → Show**.

See `example/workflows/redmineTriageBot.md` for the full setup walkthrough including users, project, members, and seed issues.

### Connections UI

The ConnectionsView shows a dedicated field for Redmine connections: **Project Identifier**.

### Task Inspector

The workflow editor shows a Redmine task inspector panel (red accent, `rgba(179,0,0)`) for the `redmine_issue` task type with fields: connection, operation (`list_issues` / `update_issue`), project_identifier, issue_id, notes, assigned_to_id, status, limit.

---

## 19. Next Steps

All phases 0–12 are complete and all 13 round-trip demos have been verified end-to-end.
