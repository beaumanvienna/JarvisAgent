# Cloud Integration Layer

This directory contains the cloud abstraction framework and all cloud service connectors for j9t.

## Architecture

Every cloud integration follows the same pattern:

```
ICloudConnector          — abstract interface (GetType, TestConnection, ResolveCredentials)
  |
  +-- PolarionConnector, S3Connector, OneDriveConnector, ...  (one per service)

ICloudTaskExecutor       — base class (resolves connection + credentials, delegates to ExecuteCloud)
  |
  +-- S3CloudTaskExecutor, SnowflakeCloudTaskExecutor, ...    (one per task type)
```

`ICloudTaskExecutor::Execute()` handles all cross-cutting concerns automatically:
- Connection lookup via `CloudConnectionManager`
- Credential resolution via the connector's `ResolveCredentials()`
- Circuit breaker check/record via `CloudCircuitBreaker`
- Audit logging to `log/security.txt`
- Cancellation token propagation from the workflow run

## Connectors

| File | Type | Auth | Service |
|------|------|------|---------|
| `polarionConnector` | `polarion` | Bearer (PAT) | Polarion ALM (Lucene queries, work item CRUD) |
| `s3Connector` | `s3` | SigV4 | S3-compatible storage (AWS, MinIO, R2) |
| `oneDriveConnector` | `onedrive` | OAuth2 (PKCE) | Microsoft OneDrive via Graph API |
| `snowflakeConnector` | `snowflake` | JWT RSA | Snowflake SQL REST API |
| `postgresConnector` | `postgres` | BasicAuth | PostgreSQL via libpq |
| `slackConnector` | `slack` | Bearer (Bot token) | Slack Web API |
| `emailConnector` | `email` | BasicAuth | SMTP send / IMAP read via libcurl |
| `gitHubConnector` | `github` | Bearer (PAT) | GitHub / GitLab REST API |
| `jiraConnector` | `jira` | BasicAuth or Bearer | Jira REST API v3 |
| `redmineConnector` | `redmine` | API key (X-Redmine-API-Key header) | Redmine REST API (self-hosted FOSS issue tracker) |
| `googleSheetsConnector` | `google_sheets` | API key or OAuth2 | Google Sheets API v4 |
| `azureBlobConnector` | `azure_blob` | Shared Key or OAuth2 | Azure Blob Storage REST API |
| `gcsConnector` | `gcs` | JWT RSA → OAuth2 | Google Cloud Storage JSON API |

## Task Executors

| File | JCWF Task Type(s) | Description |
|------|--------------------|-------------|
| `polarionWriteTaskExecutor` | `polarion_write` | Create/update work items, attachments, linked items |
| `s3CloudTaskExecutor` | `s3` | Upload, download, list, delete objects |
| `oneDriveCloudTaskExecutor` | `onedrive_upload`, `onedrive_download` | File upload/download via Graph API |
| `snowflakeCloudTaskExecutor` | `snowflake_query` | SQL submit + async poll + result parsing |
| `dbQueryCloudTaskExecutor` | `db_query` | SQL query via libpq, CSV/JSON output |
| `slackCloudTaskExecutor` | `slack_message`, `slack_read` | chat.postMessage, conversations.history |
| `emailCloudTaskExecutor` | `email_send`, `email_read` | SMTP send with MIME attachments, IMAP read |
| `gitHubCloudTaskExecutor` | `github_issue` | Issue create/comment/close, file retrieval, list issues |
| `jiraCloudTaskExecutor` | `jira_issue` | Issue create/update/transition/comment/get |
| `redmineCloudTaskExecutor` | `redmine_issue` | List issues (`list_issues`) and update with notes + assignee (`update_issue`) |
| `googleSheetsCloudTaskExecutor` | `sheets_read`, `sheets_write` | Read/write spreadsheet ranges |
| `azureBlobCloudTaskExecutor` | `azure_blob_upload`, `azure_blob_download` | Blob upload/download via Azure REST API |
| `gcsCloudTaskExecutor` | `gcs_upload`, `gcs_download` | Object upload/download via GCS JSON API |

## Shared Infrastructure

| File | Purpose |
|------|---------|
| `connectorHttp.h/cpp` | Shared libcurl helpers used by every cloud connector + executor.  `ApplyHardenedDefaults` sets `SSL_VERIFYPEER` / `SSL_VERIFYHOST` / `FOLLOWLOCATION=0` / CAINFO and (for `https://`) installs an `OPENSOCKETFUNCTION` that rejects DNS-resolved local-net IPs.  `ValidatePublicHttpEndpoint` is the SSRF gate for user-supplied endpoint URLs.  `UrlEncodeComponent` + `UrlEncodePathSegments` are the shared percent-encode helpers — every cloud executor that splices user-controlled values into URLs runs them through these (`UrlEncodeComponent` for single segments, `UrlEncodePathSegments` for `/`-separated paths with per-segment `..`/`.`/empty rejection).  Defense-in-depth after each executor's per-provider allowlist regex on identifier shape. |
| `cloudConnector.h` | `ICloudConnector` interface, `CloudConnection`, `CloudCredentials`, `CloudAuthType`.  `CloudCredentials::m_Token` / `m_SecretKey` / `m_Password` are `SecureString` (mlock'd, zero-on-destruct, non-copyable) so secret bytes never appear in a plain `std::string` heap allocation between `ResolveCredentials` and `curl_slist_append`.  `TestConnection` returns `[[nodiscard]] std::expected<void, ConnectorError>`; see `connectorError.h` for the 9-variant `Code` enum (`InvalidConfig` / `InvalidEndpoint` / `CredentialMissing` / `CredentialInvalid` / `OAuthError` / `NetworkError` / `AuthFailure` / `HttpError` / `UnknownError`). |
| `connectorError.h/cpp` | Typed error returned by `TestConnection`, `ValidatePublicHttpEndpoint`, `ValidatePostgresParams`.  `ConnectorError{Code, m_Details}` + `Make()` factory + `Describe()` switch helper.  Per-site `Code` selection: param missing/blank → `InvalidConfig`, SSRF/syntax rejection → `InvalidEndpoint`, `ResolveCredentials` failure (bridged) → `CredentialMissing`, CRLF / structurally-bad creds → `CredentialInvalid`, `curl_easy_init` / `CURLE != OK` → `NetworkError`, HTTP 401/403 → `AuthFailure`, HTTP ≥ 400 (other) → `HttpError`.  |
| `cloudConnectorRegistry` | Registry mapping type names to connector instances |
| `cloudConnectionManager` | In-memory CRUD store for `CloudConnection` configs |
| `cloudTaskExecutor` | `ICloudTaskExecutor` base class with connection/credential/circuit-breaker wiring |
| `cloudRetryPolicy` | Exponential backoff with jitter, Retry-After header support |
| `cloudCircuitBreaker` | Per-connection circuit breaker (Closed/Open/HalfOpen).  `RecordFailure(name, ConnectorErrorCode)` stores the latest typed code per circuit; `ConnectionHealth::m_LastFailureCode` surfaces it via `GetHealthSummary` for `/api/status::connection_health[].last_failure_code`. |
| `cloudConnectionPool` | Generic connection pool for persistent-connection providers (libpq) |
| `providerRateLimitPolicy` | Per-provider rate-limit awareness (burst limits, min intervals) |
| `taskCancellationToken` | Cooperative cancellation for long-running cloud operations |
| `sigV4Signer` | AWS Signature V4 request signing (HMAC-SHA256 via OpenSSL) |
| `azureSharedKeySigner` | Azure Storage Shared Key request signing (HMAC-SHA256 via OpenSSL) |

## Adding a New Connector

1. Create `myServiceConnector.h/cpp` implementing `ICloudConnector`
2. Create `myServiceCloudTaskExecutor.h/cpp` extending `ICloudTaskExecutor`
3. Add `TaskType::MyService` to `workflowTypes.h`
4. Add the string mapping in `workflowJsonParser.cpp`
5. Register both in `jarvisAgent.cpp` (`connectorRegistry.Register()` + `executorRegistry.RegisterExecutor()`)
6. Add connection config fields in `ConnectionsView.tsx`
7. Add task inspector section in `WorkflowEditorView.tsx`
8. Add the task type to `JcwfTaskType` in `types.ts`

## Related Files (outside this directory)

| Path | Purpose |
|------|---------|
| `engine/keys/keyManager.h` | Credential storage and retrieval |
| `engine/keys/oauthTokenManager.h` | OAuth2 token lifecycle and background refresh |
| `engine/keys/jwtGenerator.h` | RSA RS256 JWT creation (Snowflake, service accounts) |
| `engine/curlWrapper/awsSigV4.h` | AWS SigV4 signing (used by `s3Connector`, `azureBlobConnector` for AWS-compatible flows) — takes `SecureString const&` for secret_access_key + session_token |
| `engine/curlWrapper/authSigner.h` | `IAuthSigner` polymorphic auth-header production (Bearer / x-api-key / Azure / SigV4 / Anthropic) — emits secret-bearing headers through a caller-owned `SecureString` slot, not a `std::string` vector |
| `engine/curlWrapper/curlSlistHelper.h` | `AppendSecretHeader(curl_slist*&, prefix, SecureString const&, SecureString& scratch)` — canonical helper for inline `"Authorization: Bearer <secret>"` builds across cloud connectors / workflow filters.  Used wherever a connector talks to libcurl directly rather than via `IAuthSigner`.  See `doc/cyber security.md` "SecureString-only HTTP path". |
| `engine/log/secretRedactor.h` | Scrubs secrets from log output |
| `engine/core.h` | Owns `CloudConnectionManager`, `CloudConnectorRegistry`, `CloudCircuitBreaker`, `OAuthTokenManager` |
| `application/web/webServer.cpp` | REST API endpoints for connections CRUD, OAuth flow, health status |
| `application/workflow/triggerEngine.h` | Cloud trigger types (s3_watch, onedrive_watch, email_watch, azure_blob_watch, gcs_watch) |
