# Cloud Integration

This document covers the cloud integration layer introduced in j9t, including the abstraction framework, credential management, MCP interface, and connection management.

---

## 1. Overview

The cloud integration layer provides a uniform interface for connecting j9t workflows to external cloud services (Polarion, S3, OneDrive, Snowflake, PostgreSQL, Slack, Email, GitHub, Jira). All cloud code in the C++ backend uses libcurl + OpenSSL + simdjson — Python is reserved for workflow task scripts only.

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

**Files:** `engine/keys/oauthTokenManager.h/cpp`

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
| `J9T_URL` | `http://localhost:8080` | j9t base URL |
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

### Docker Deployment

The `docker-compose.example.yml` includes a `j9t_mcp` sidecar service that connects to `jarvis_agent` over the internal Docker network.

---

## 8. Next Steps

### Phase 2 — Polarion Enhancements
First connector to validate the abstraction layer. Wraps existing `PolarionClient` in `PolarionConnector : ICloudConnector`, adds write-back, creation, attachments, and traceability.

### Phase 3 — S3 (Object Storage)
First general-purpose connector. SigV4 signing via `SigV4Signer`, upload/download task executor, `s3_watch` trigger.

### Phase 4 — PostgreSQL
Database connector via libpq. Parameterized queries (no SQL injection), CSV/JSON output.

See `cloud-integration-dev-plan.md` for the complete roadmap through Phase 9.
