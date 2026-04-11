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
| `googleSheetsConnector` | `google_sheets` | API key or OAuth2 | Google Sheets API v4 |

## Task Executors

| File | JCWF Task Type(s) | Description |
|------|--------------------|-------------|
| `polarionWriteTaskExecutor` | `polarion_write` | Create/update work items, attachments, linked items |
| `s3CloudTaskExecutor` | `s3` | Upload, download, list, delete objects |
| `oneDriveCloudTaskExecutor` | `onedrive_upload`, `onedrive_download` | File upload/download via Graph API |
| `snowflakeCloudTaskExecutor` | `snowflake_query` | SQL submit + async poll + result parsing |
| `dbQueryCloudTaskExecutor` | `db_query` | SQL query via libpq, CSV/JSON output |
| `slackCloudTaskExecutor` | `slack_message` | chat.postMessage |
| `emailCloudTaskExecutor` | `email_send` | SMTP send with MIME attachments |
| `gitHubCloudTaskExecutor` | `github_issue` | Issue create/comment/close, file retrieval, list issues |
| `jiraCloudTaskExecutor` | `jira_issue` | Issue create/update/transition/comment/get |
| `googleSheetsCloudTaskExecutor` | `sheets_read`, `sheets_write` | Read/write spreadsheet ranges |

## Shared Infrastructure

| File | Purpose |
|------|---------|
| `cloudConnector.h` | `ICloudConnector` interface, `CloudConnection`, `CloudCredentials`, `CloudAuthType` |
| `cloudConnectorRegistry` | Registry mapping type names to connector instances |
| `cloudConnectionManager` | In-memory CRUD store for `CloudConnection` configs |
| `cloudTaskExecutor` | `ICloudTaskExecutor` base class with connection/credential/circuit-breaker wiring |
| `cloudRetryPolicy` | Exponential backoff with jitter, Retry-After header support |
| `cloudCircuitBreaker` | Per-connection circuit breaker (Closed/Open/HalfOpen) |
| `cloudConnectionPool` | Generic connection pool for persistent-connection providers (libpq) |
| `providerRateLimitPolicy` | Per-provider rate-limit awareness (burst limits, min intervals) |
| `taskCancellationToken` | Cooperative cancellation for long-running cloud operations |
| `sigV4Signer` | AWS Signature V4 request signing (HMAC-SHA256 via OpenSSL) |

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
| `engine/keys/sigV4Signer.h` | AWS SigV4 signing (also in this directory) |
| `engine/log/secretRedactor.h` | Scrubs secrets from log output |
| `engine/core.h` | Owns `CloudConnectionManager`, `CloudConnectorRegistry`, `CloudCircuitBreaker`, `OAuthTokenManager` |
| `application/web/webServer.cpp` | REST API endpoints for connections CRUD, OAuth flow, health status |
| `application/workflow/triggerEngine.h` | Cloud trigger types (s3_watch, onedrive_watch, email_watch) |
