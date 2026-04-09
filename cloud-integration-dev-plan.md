# Cloud Integration Dev Plan for j9t

## Design Principles

1. **Python in tasks only** — all cloud integration code in the C++ backend uses libcurl + OpenSSL + simdjson. Python is reserved for workflow task scripts (AI-generated, executed by the Python engine).
2. **GPL-3.0 compliance** — all libraries linked into j9t are GPL-3.0 compatible. All cloud services are accessed via open REST/HTTP protocols.
3. **Disk-first** — all cloud data (query results, downloaded files, API responses) is written to the workflow working directory before downstream tasks consume it.
4. **Documentation stays current** — `doc/cloud-integration.md` and `doc/cyber security.md` must be updated at the end of every iteration/session that completes a feature. When a TODO item is checked off, the corresponding documentation update is implicit — it is not a separate TODO. The documentation should always reflect the current state of implemented features.

---

## Impact on Existing AI Pipeline

The existing AI interface code (`CurlMultiDispatcher`, `aiRequestPool`, `aiCallTaskExecutor`, AI Manager UI) should **not** be refactored into the cloud abstraction — it is a different domain with its own well-working architecture. However, three foundation utilities benefit the AI pipeline directly:

| Utility | AI pipeline impact | When |
|---------|-------------------|------|
| `ICredential` hierarchy | AI API keys migrate from flat `Provider` to `ApiKeyCredential` automatically when `KeyManager` is extended. No separate effort — happens as part of Phase 0. | Phase 0 |
| `SecretRedactor` | AI API keys must also be scrubbed from logs. Wire `SecretRedactor` into `CurlMultiDispatcher` and `aiRequestPool` logging. | Phase 0 |
| `OAuthTokenManager` + `JwtGenerator` | Enables **Google Vertex AI** support. Vertex AI uses OAuth2 (service account) or JWT auth instead of simple API keys. Add `AuthStyle::OAuth2` to `CurlWrapper` alongside existing `Bearer` and `XGoogApiKey`. This is optional but comes for free once the cloud foundation exists. | Phase 5+ (optional) |

**Do not** make AI interfaces into `ICloudConnector`. AI interfaces have their own config model (URL, model, API type, linked key) and their own UI (AI Manager). Cloud connections are a separate concept.

**Google AI auth status:** Standard Gemini API (`x-goog-api-key` header) is already supported. Google Vertex AI (enterprise, OAuth2/JWT) is not yet supported but becomes possible with the cloud plan's `OAuthTokenManager` and `JwtGenerator`.

---

## TODO

### Phase 0 — Foundation

- [ ] Implement `ICloudConnector` abstract base class and `CloudConnectorRegistry`
- [ ] Implement `CloudConnection` config model (backend + REST API CRUD)
- [ ] Implement `ICredential` class hierarchy (`ApiKeyCredential`, `OAuthCredential`, `KeyPairCredential`, `BasicAuthCredential`) — replaces flat `key_type` string discriminator with type-safe polymorphism
- [ ] Extend `KeyManager` to store and resolve `ICredential` subtypes
- [ ] Implement `OAuthTokenManager` (token refresh loop, expiry tracking)
- [ ] Implement `JwtGenerator` (RSA RS256 via OpenSSL, reusable for Snowflake and others)
- [ ] Implement `CloudRetryPolicy` (shared exponential backoff with jitter, provider-specific retry codes, Retry-After header support)
- [ ] Implement `SecretRedactor` utility for log output (scrub Bearer tokens, JWT payloads, OAuth refresh tokens, SigV4 signatures, SQL query parameters from log messages)
- [ ] Add `ICloudTaskExecutor` base class and register in `TaskExecutorRegistry`
- [ ] Frontend: Add "Connections" tab and `ConnectionsView.tsx`
- [ ] Frontend: Rename "AI Keys" to "Keys", add credential type support to `ProvidersSettingsView.tsx`
- [ ] Frontend: Add `api/connections.ts` REST client
- [ ] Add `GET/POST/PUT/DELETE /api/connections` REST endpoints in `webServer.cpp`
- [ ] Add `POST /api/connections/<name>/test` endpoint

### Phase 1 — MCP Interface

MCP is a standalone TypeScript sidecar with zero C++ changes. It unlocks Claude Desktop, Claude Code, and other MCP client integrations immediately — highest ecosystem impact per effort.

- [ ] Create standalone MCP server (TypeScript, `@modelcontextprotocol/sdk`, MIT)
- [ ] Implement tools: `list_workflows`, `run_workflow`, `get_run_status`, `get_run_output`, `list_active_runs`, `cancel_run`
- [ ] Implement resources: `workflow://<id>`, `run://<runId>`
- [ ] Bearer token passthrough from MCP server to j9t REST API
- [ ] Add `mcp/` directory with `package.json`, `tsconfig.json`, `src/index.ts`
- [ ] Docker: add MCP sidecar to `docker-compose.example.yml`
- [ ] Dashboard: add MCP status indicator

### Phase 2 — Polarion Enhancements

First connector to validate the abstraction layer against existing, working code.

- [ ] Implement `PolarionConnector : ICloudConnector` (wraps existing `PolarionClient`)
- [ ] Add write-back: `PATCH /rest/v1/projects/{id}/workitems/{id}` via `PolarionClient`
- [ ] Add work item creation: `POST /rest/v1/projects/{id}/workitems`
- [ ] Add attachment download/upload
- [ ] Add linked items / traceability traversal
- [ ] Migrate `polarion_query` filter to use named Connection instead of inline `base_url`/`project_id`
- [ ] Add `polarion_write` task type and inspector UI

### Phase 3 — Object Storage (S3-compatible)

First general-purpose connector. Low UI complexity, high business value.

- [ ] Implement `S3Connector : ICloudConnector`
- [ ] Implement SigV4 request signing (OpenSSL HMAC-SHA256)
- [ ] Implement `S3CloudTaskExecutor` (upload/download)
- [ ] Add `s3_watch` trigger type (polling via `GET /?list-type=2`)
- [ ] Frontend: S3 connection config fields, task inspector sections
- [ ] Add example workflow demonstrating S3 upload/download

### Phase 4 — Database (PostgreSQL)

Lower auth complexity than Snowflake, easier to test locally, validates the DB abstraction before tackling JWT-based services.

- [ ] Vendor libpq (PostgreSQL License, permissive, GPL-compatible)
- [ ] Implement `PostgresConnector : ICloudConnector`
- [ ] Implement `DbQueryCloudTaskExecutor` (query to CSV/JSON to disk)
- [ ] Frontend: PostgreSQL connection config, `db_query` task inspector

--- Ship milestone: foundation + MCP + Polarion + S3 + PostgreSQL ---

### Phase 5 — OneDrive

- [ ] Implement `OneDriveConnector : ICloudConnector`
- [ ] OAuth 2.0 authorization code flow with PKCE
- [ ] Implement `OneDriveCloudTaskExecutor` (upload/download via Graph API)
- [ ] Add `onedrive_watch` trigger type (delta query polling)
- [ ] Frontend: OAuth flow dialog, OneDrive connection config, task inspector sections

### Phase 6 — Snowflake

- [ ] Implement `SnowflakeConnector : ICloudConnector`
- [ ] Implement `SnowflakeJwtAuth` (RSA RS256 JWT via `JwtGenerator`)
- [ ] Implement `SnowflakeCloudTaskExecutor` (SQL query via `/api/v2/statements`, async polling)
- [ ] Frontend: Snowflake connection config, `snowflake_query` task inspector

### Phase 7 — Messaging (Slack, Email)

- [ ] Implement `SlackConnector : ICloudConnector`
- [ ] Implement `SlackCloudTaskExecutor` (`chat.postMessage` via REST)
- [ ] Implement `EmailConnector : ICloudConnector` (SMTP/IMAP via libcurl)
- [ ] Implement `EmailCloudTaskExecutor` (send via SMTP)
- [ ] Add `email_watch` trigger type (IMAP polling)
- [ ] Frontend: Slack and Email connection config, task inspector sections

### Phase 8 — Additional Integrations

- [ ] GitHub/GitLab: REST API connector + webhook trigger enrichment
- [ ] Jira/Linear: REST API connector + task types for issue CRUD
- [ ] Google Sheets: REST API connector + read/write task types

### Phase 9 — Hardening

Split into three sub-phases to avoid a monolithic hardening bucket:

#### Phase 9a — Runtime Resilience

- [ ] Implement `CloudCircuitBreaker` per connection (track consecutive failures, short-circuit requests during outages, auto-recover after cooldown)
- [ ] Implement `CloudConnectionPool` for persistent-connection providers (PostgreSQL via libpq, future IMAP keep-alive). HTTP-based connectors already benefit from libcurl connection reuse (`CURLOPT_TCP_KEEPALIVE`), so the pool is primarily for non-HTTP protocols.
- [ ] Wire `TaskCancellationToken` into existing run cancel mechanism so long-running cloud operations (Snowflake async polling, large S3 downloads, SMTP timeouts) respond to workflow run cancellation. (Token parameter already in `ExecuteCloud()` signature from Phase 0.)
- [ ] Implement `ProviderRateLimitPolicy` extending `CloudRetryPolicy` with provider-specific semantics (Slack `Retry-After` with secondary limits, GitHub secondary rate limits, Graph API throttling windows, Snowflake async query polling cadence)
- [ ] Add connection health check to `/api/status` readiness probe
- [ ] Dashboard: connection health indicators in status bar

#### Phase 9b — Security & Audit

- [ ] Add audit logging for cloud operations (connection CRUD, OAuth grants, cloud task execution)
- [ ] Add OAuth state parameter CSRF protection
- [ ] Add download size limits (`CURLOPT_MAXFILESIZE_LARGE`)
- [ ] Add path traversal validation for cloud local_path params
- [ ] Enforce RSA key minimum 2048 bits in `JwtGenerator`
- [ ] Verify `CURLOPT_SSL_VERIFYPEER` enabled in all cloud connectors

#### Phase 9c — Deployment & Ops

- [ ] Document outbound firewall rules per integration
- [ ] Container image signing (cosign, Apache-2.0)

---

## 1. Cloud Abstraction Layer

All cloud integrations share common patterns: authentication, HTTP requests, connection testing, credential storage. A cloud abstraction layer prevents duplication and provides a uniform interface for all connectors.

### Abstract base class: `ICloudConnector`

```cpp
// application/cloud/cloudConnector.h

namespace AIAssistant
{
    // Identifies the authentication method for a cloud connection.
    enum class CloudAuthType
    {
        BearerToken,    // API key or PAT as Bearer header
        OAuth2,         // OAuth 2.0 access token (auto-refreshed)
        JwtRsa,         // RSA-signed JWT (e.g., Snowflake)
        BasicAuth,      // Username + password
        SigV4           // AWS Signature V4 (S3-compatible)
    };

    // Resolved credentials ready for use in HTTP requests.
    // This is a runtime transport bundle — not a persisted type. The persisted side uses
    // the ICredential hierarchy (ApiKeyCredential, OAuthCredential, etc.). CloudCredentials
    // is what connectors produce after resolving and refreshing stored credentials into a
    // form ready for immediate use in HTTP headers / connection strings.
    struct CloudCredentials
    {
        CloudAuthType m_AuthType{CloudAuthType::BearerToken};
        std::string m_Token;        // Bearer/OAuth token or JWT
        std::string m_AccessKeyId;  // SigV4
        std::string m_SecretKey;    // SigV4
        std::string m_Username;     // BasicAuth
        std::string m_Password;     // BasicAuth
    };

    // Configuration for a named cloud connection (persisted in config.json).
    struct CloudConnection
    {
        std::string m_Name;         // Unique connection name (user-defined)
        std::string m_Type;         // "polarion", "s3", "onedrive", "snowflake", etc.
        std::string m_Endpoint;     // Base URL or account identifier
        std::string m_KeyName;      // Reference to KeyManager credential
        CloudAuthType m_AuthType{CloudAuthType::BearerToken};
        std::map<std::string, std::string> m_Params;  // Type-specific config
    };

    // Abstract interface for all cloud service connectors.
    class ICloudConnector
    {
    public:
        virtual ~ICloudConnector() = default;

        // Return the connector type name (e.g., "polarion", "s3", "snowflake").
        virtual std::string GetType() const = 0;

        // Test connectivity using the given connection config.
        // Returns true on success, populates errorMessage on failure.
        virtual bool TestConnection(CloudConnection const& connection,
                                    std::string& errorMessage) = 0;

        // Resolve credentials from KeyManager for the given connection.
        // Handles OAuth refresh, JWT generation, SigV4 derivation, etc.
        virtual bool ResolveCredentials(CloudConnection const& connection,
                                        CloudCredentials& credentials,
                                        std::string& errorMessage) = 0;
    };
}
```

### Connector registry: `CloudConnectorRegistry`

```cpp
// application/cloud/cloudConnectorRegistry.h

namespace AIAssistant
{
    class CloudConnectorRegistry
    {
    public:
        void Register(std::unique_ptr<ICloudConnector> connector);
        ICloudConnector* GetConnector(std::string const& type) const;
        std::vector<std::string> GetRegisteredTypes() const;

    private:
        std::unordered_map<std::string, std::unique_ptr<ICloudConnector>> m_Connectors;
    };
}
```

### Cloud task executor base: `ICloudTaskExecutor`

Cloud task executors extend `ITaskExecutor` and resolve their connection from the task params:

```cpp
// application/cloud/cloudTaskExecutor.h

namespace AIAssistant
{
    // Base for task executors that operate on cloud connections.
    // Subclasses implement ExecuteCloud() with resolved credentials.
    class ICloudTaskExecutor : public ITaskExecutor
    {
    public:
        bool Execute(WorkflowDefinition const& workflowDef, WorkflowRun& run,
                     TaskDef const& taskDef, TaskInstanceState& state) override;

    protected:
        // Subclasses implement this with the actual cloud operation.
        // The cancellationToken allows cooperative cancellation of long-running operations
        // (Snowflake async polling, large S3 downloads, SMTP timeouts). Subclasses should
        // check token.IsCancelled() between poll iterations and during chunked I/O.
        virtual bool ExecuteCloud(WorkflowDefinition const& workflowDef, WorkflowRun& run,
                                  TaskDef const& taskDef, TaskInstanceState& state,
                                  CloudConnection const& connection,
                                  CloudCredentials const& credentials,
                                  TaskCancellationToken const& cancellationToken) = 0;
    };
}
```

The `Execute()` base method: looks up the connection by name from `taskDef.m_Params["connection"]`, calls `ICloudConnector::ResolveCredentials()`, then delegates to `ExecuteCloud()`.

**Cancellation support:** The `ExecuteCloud()` signature includes a `TaskCancellationToken` from Phase 0. The token is initially a no-op (never cancelled). Phase 9 wires it into the existing run cancel mechanism so that `POST /api/workflow-runs/{runId}/cancel` propagates to in-flight cloud tasks. Subclasses check `token.IsCancelled()` between poll iterations, during chunked downloads, and around blocking I/O.

### Concrete connectors (one per service)

| Connector class | File | Service |
|----------------|------|---------|
| `PolarionConnector` | `application/cloud/polarionConnector.h/cpp` | Polarion ALM (Lucene queries, work item CRUD) |
| `S3Connector` | `application/cloud/s3Connector.h/cpp` | S3-compatible storage (AWS, MinIO, GCS interop) |
| `OneDriveConnector` | `application/cloud/oneDriveConnector.h/cpp` | Microsoft Graph API + OAuth2 |
| `SnowflakeConnector` | `application/cloud/snowflakeConnector.h/cpp` | Snowflake SQL REST API + JWT RSA |
| `SlackConnector` | `application/cloud/slackConnector.h/cpp` | Slack Web API |
| `EmailConnector` | `application/cloud/emailConnector.h/cpp` | SMTP/IMAP via libcurl |
| `PostgresConnector` | `application/cloud/postgresConnector.h/cpp` | PostgreSQL via libpq (PostgreSQL License) |
| `GitConnector` | `application/cloud/gitConnector.h/cpp` | GitHub/GitLab REST API |
| `JiraConnector` | `application/cloud/jiraConnector.h/cpp` | Jira/Linear REST API |

### Shared utilities

| Utility class | File | Purpose |
|--------------|------|---------|
| `OAuthTokenManager` | `engine/keys/oauthTokenManager.h/cpp` | OAuth 2.0 token refresh loop, expiry tracking, thread-safe token access |
| `JwtGenerator` | `engine/keys/jwtGenerator.h/cpp` | RSA RS256 JWT creation via OpenSSL `EVP_DigestSign`, base64url encoding |
| `SigV4Signer` | `engine/keys/sigV4Signer.h/cpp` | AWS Signature V4 request signing via OpenSSL HMAC-SHA256 |
| `CloudRetryPolicy` | `application/cloud/cloudRetryPolicy.h/cpp` | Centralized retry with exponential backoff + jitter (see below) |
| `SecretRedactor` | `engine/log/secretRedactor.h/cpp` | Scrub sensitive values from log output (see below) |
| `CloudConnectionPool` | `application/cloud/cloudConnectionPool.h/cpp` | Connection pooling for persistent-connection providers (Phase 9) |
| `TaskCancellationToken` | `application/cloud/taskCancellationToken.h` | Cooperative cancellation for long-running cloud operations (Phase 9) |
| `ProviderRateLimitPolicy` | `application/cloud/providerRateLimitPolicy.h/cpp` | Per-provider rate-limit semantics extending `CloudRetryPolicy` (Phase 9) |

### CloudRetryPolicy

Every cloud provider rate-limits. PolarionClient already has ad-hoc retry logic (3 retries, 1s backoff) and CurlMultiDispatcher handles 429 backoff, but there is no shared utility. `CloudRetryPolicy` centralizes this:

```cpp
// application/cloud/cloudRetryPolicy.h

class CloudRetryPolicy
{
public:
    struct Config
    {
        int m_MaxRetries{3};
        int m_InitialBackoffMs{1000};
        int m_MaxBackoffMs{30000};
        double m_JitterFactor{0.2};       // +/- 20% randomization
        std::set<int> m_RetryableHttpCodes{429, 500, 502, 503, 504};
    };

    // Execute fn() with retries. Returns the result of the last attempt.
    // Respects Retry-After headers when present.
    template <typename Fn>
    auto Execute(Fn&& fn, Config const& config, std::string& errorMessage) -> decltype(fn());
};
```

All cloud connectors and task executors use this instead of hand-rolled loops. Provider-specific retry codes can be added to `m_RetryableHttpCodes` per connector.

**Provider-specific rate limiting (Phase 9):** Some providers need semantics beyond simple retry-after backoff. Examples: Slack 429 with `Retry-After` header and secondary rate limits, GitHub secondary rate limits (separate from primary), Graph API throttling windows, Snowflake async query polling cadence. Phase 9 adds `ProviderRateLimitPolicy` extending `CloudRetryPolicy` with per-provider rate awareness. Each connector can register its provider's specific throttling behavior. Not required for Phase 0 — the base `CloudRetryPolicy` with `Retry-After` header support is sufficient for initial connectors.

### SecretRedactor

Cloud integrations generate log output containing tokens, keys, and query content. `SecretRedactor` ensures secrets never land in logs accidentally:

```cpp
// engine/log/secretRedactor.h

class SecretRedactor
{
public:
    // Register a secret value to be redacted from all log output.
    void AddSecret(std::string const& secret);

    // Remove a previously registered secret (e.g., after token rotation).
    void RemoveSecret(std::string const& secret);

    // Scrub all registered secrets from a log message, replacing with [REDACTED].
    std::string Redact(std::string const& message) const;
};
```

Integrated into the logging pipeline: `OAuthTokenManager` registers access/refresh tokens on acquisition, `JwtGenerator` registers generated JWTs, `SigV4Signer` registers signing keys. `SecretRedactor::Redact()` is called by the log macros before writing to `log/security.txt` and the application log.

### CloudCircuitBreaker (Phase 9 — roadmap)

Per-connection circuit breaker to prevent failure cascading during cloud provider outages:

```cpp
// application/cloud/cloudCircuitBreaker.h

class CloudCircuitBreaker
{
public:
    enum class State { Closed, Open, HalfOpen };

    // Record a success or failure for a named connection.
    void RecordSuccess(std::string const& connectionName);
    void RecordFailure(std::string const& connectionName);

    // Check if requests should be allowed for this connection.
    bool AllowRequest(std::string const& connectionName) const;

    State GetState(std::string const& connectionName) const;
};
```

Configurable: failure threshold (default 5), cooldown period (default 60s), half-open probe count (default 1). Not Phase 0 — add after the core connectors are stable.

---

## 2. Backend Folder Structure

New files shown with `(+)`, modified files with `(~)`:

```
application/
  cloud/                                    (+) new directory
    cloudConnector.h                        (+) ICloudConnector, CloudConnection, CloudCredentials, CloudAuthType
    cloudConnectorRegistry.h                (+) CloudConnectorRegistry
    cloudConnectorRegistry.cpp              (+)
    cloudRetryPolicy.h                      (+) CloudRetryPolicy (shared retry + backoff + jitter)
    cloudRetryPolicy.cpp                    (+)
    cloudCircuitBreaker.h                   (+) CloudCircuitBreaker (Phase 9)
    cloudCircuitBreaker.cpp                 (+)
    cloudConnectionPool.h                   (+) CloudConnectionPool (Phase 9)
    cloudConnectionPool.cpp                 (+)
    taskCancellationToken.h                 (+) TaskCancellationToken (Phase 9)
    providerRateLimitPolicy.h               (+) ProviderRateLimitPolicy (Phase 9)
    providerRateLimitPolicy.cpp             (+)
    cloudTaskExecutor.h                     (+) ICloudTaskExecutor base class
    cloudTaskExecutor.cpp                   (+)
    polarionConnector.h                     (+) PolarionConnector : ICloudConnector
    polarionConnector.cpp                   (+) wraps existing PolarionClient
    s3Connector.h                           (+) S3Connector : ICloudConnector
    s3Connector.cpp                         (+) SigV4 auth, bucket operations
    oneDriveConnector.h                     (+) OneDriveConnector : ICloudConnector
    oneDriveConnector.cpp                   (+) Graph API, OAuth2
    snowflakeConnector.h                    (+) SnowflakeConnector : ICloudConnector
    snowflakeConnector.cpp                  (+) SQL REST API, JWT RSA
    slackConnector.h                        (+) SlackConnector : ICloudConnector
    slackConnector.cpp                      (+) Slack Web API
    emailConnector.h                        (+) EmailConnector : ICloudConnector
    emailConnector.cpp                      (+) SMTP/IMAP via libcurl
    postgresConnector.h                     (+) PostgresConnector : ICloudConnector
    postgresConnector.cpp                   (+) libpq wrapper
    gitConnector.h                          (+) GitConnector : ICloudConnector
    gitConnector.cpp                        (+) GitHub/GitLab REST API
    jiraConnector.h                         (+) JiraConnector : ICloudConnector
    jiraConnector.cpp                       (+)
  workflow/
    cloudTaskExecutors/                     (+) new directory
      s3TaskExecutor.h                      (+) S3CloudTaskExecutor : ICloudTaskExecutor
      s3TaskExecutor.cpp                    (+) upload/download
      oneDriveTaskExecutor.h                (+) OneDriveCloudTaskExecutor : ICloudTaskExecutor
      oneDriveTaskExecutor.cpp              (+) upload/download via Graph API
      snowflakeTaskExecutor.h               (+) SnowflakeCloudTaskExecutor : ICloudTaskExecutor
      snowflakeTaskExecutor.cpp             (+) SQL query, async polling, results to disk
      slackTaskExecutor.h                   (+) SlackCloudTaskExecutor : ICloudTaskExecutor
      slackTaskExecutor.cpp                 (+) chat.postMessage
      emailTaskExecutor.h                   (+) EmailCloudTaskExecutor : ICloudTaskExecutor
      emailTaskExecutor.cpp                 (+) SMTP send
      dbQueryTaskExecutor.h                 (+) DbQueryCloudTaskExecutor : ICloudTaskExecutor
      dbQueryTaskExecutor.cpp               (+) SQL query via libpq, results to CSV/JSON
      polarionWriteTaskExecutor.h           (+) PolarionWriteTaskExecutor : ICloudTaskExecutor
      polarionWriteTaskExecutor.cpp         (+) PATCH/POST work items
    filter/
      polarionClient.h                      (~) refactor to accept CloudConnection + CloudCredentials
      polarionClient.cpp                    (~)
      filterEngine.cpp                      (~) resolve connection for polarion_query filter
    taskExecutorRegistry.cpp                (~) register new cloud task executors
    triggerEngine.h                          (~) add cloud trigger types (s3_watch, onedrive_watch, email_watch)
    triggerEngine.cpp                        (~)
    workflowTypes.h                         (~) add new task types, trigger types, connection ref field

engine/
  keys/
    credential.h                            (+) ICredential, ApiKeyCredential, OAuthCredential, KeyPairCredential, BasicAuthCredential
    keyManager.h                            (~) store ICredential subtypes, resolve by type
    keyManager.cpp                          (~)
    keyEncryption.h                         (~) serialize credential hierarchy
    keyEncryption.cpp                       (~)
    oauthTokenManager.h                     (+) OAuth 2.0 token lifecycle
    oauthTokenManager.cpp                   (+)
    jwtGenerator.h                          (+) RSA RS256 JWT creation
    jwtGenerator.cpp                        (+)
    sigV4Signer.h                           (+) AWS Signature V4 signing
    sigV4Signer.cpp                         (+)
  log/
    secretRedactor.h                        (+) SecretRedactor (scrub secrets from log output)
    secretRedactor.cpp                      (+)

application/web/
    webServer.h                             (~) add /api/connections endpoints
    webServer.cpp                           (~) HandleConnectionsList, HandleConnectionTest, etc.

mcp/                                        (+) new top-level directory
  package.json                              (+) @modelcontextprotocol/sdk (MIT)
  tsconfig.json                             (+)
  src/
    index.ts                                (+) MCP server entry point
    tools.ts                                (+) tool implementations
    resources.ts                            (+) resource implementations
```

---

## 3. Polarion Enhancements

### Current state

`PolarionClient` (`application/workflow/filter/polarionClient.h/cpp`, ~507 lines) handles read-only Lucene queries with pagination, Bearer token auth, per-item JSON output, and freshness tracking via SHA-256 manifest. Connection details are inline in each JCWF filter source (`base_url`, `project_id`, `key_name`). Example: `goKartComplianceCheck.jcwf` queries 18 requirements and fans out to parallel AI compliance assessments.

### Dev plan

**3.1 Wrap in PolarionConnector**

Create `PolarionConnector : ICloudConnector`. The connector resolves credentials from KeyManager and provides them to `PolarionClient`. The `polarion_query` filter source gains an optional `connection` field; when set, it looks up the named connection instead of reading inline fields. Backward-compatible: inline fields still work if `connection` is absent.

**3.2 Write-back (PATCH work items)**

New task type: `polarion_write`

```json
{
  "id": "write_status",
  "type": "polarion_write",
  "params": {
    "connection": "my-polarion",
    "project_id": "GoKartProcurement",
    "work_item_id": "{{req.work_item_id}}",
    "fields": {
      "customField_complianceStatus": "{{output}}"
    }
  }
}
```

Implementation: `PolarionWriteTaskExecutor` calls `PATCH /rest/v1/projects/{project_id}/workitems/{work_item_id}` with JSON:API body. Add `HttpPatch()` method to `PolarionClient` (alongside existing `HttpGet`).

**3.3 Create work items (POST)**

Same task executor, different mode. When `work_item_id` is absent, POST to create a new work item. The created ID is written to the task output file.

**3.4 Attachments**

Add methods to `PolarionClient`:
- `DownloadAttachment(workItemId, attachmentId, localPath)` — `GET /rest/v1/projects/{id}/workitems/{id}/attachments/{id}/content`
- `UploadAttachment(workItemId, localPath, filename)` — `POST /rest/v1/projects/{id}/workitems/{id}/attachments` with multipart/form-data

**3.5 Linked items / traceability**

Add `FetchLinkedItems(workItemId, linkRole)` to `PolarionClient`. Calls `GET /rest/v1/projects/{id}/workitems/{id}/linkedworkitems`. Returns linked item IDs and link roles (parent, child, satisfies, verifies). Enables requirement tree traversal in workflows.

---

## 4. MCP Interface

### Architecture

Standalone TypeScript process using `@modelcontextprotocol/sdk` (MIT). Communicates with j9t via REST API over localhost. Supports both stdio and SSE transports. The MCP sidecar targets **Engine edition** for production deployment (security-hardened, RBAC-enforced, audit-logged). It also works with Studio edition during development and testing, but production MCP deployments should use Engine — Studio exposes workflow CRUD and AI tooling that MCP clients should not have access to.

```
AI Assistant (Claude, etc.)
    |
    | MCP protocol (JSON-RPC over stdio or SSE)
    |
j9t MCP Server (TypeScript, mcp/ directory)
    |
    | REST API (localhost:8080, Bearer token auth)
    |
j9t Engine
```

### MCP tools

| Tool | Maps to | Description |
|------|---------|-------------|
| `list_workflows` | `GET /api/workflows` | List available workflows with labels |
| `run_workflow` | `POST /api/workflows/<id>/run` | Start a workflow with optional context/bindings |
| `get_run_status` | `GET /api/workflow-runs/<runId>` | Task-level progress and state |
| `get_run_output` | `GET /api/workflow-runs/<runId>` | Retrieve completed workflow results |
| `list_active_runs` | `GET /api/workflow-runs/active` | Currently running workflows |
| `cancel_run` | `POST /api/workflow-runs/<runId>/cancel` | Cancel a running workflow |

### MCP resources

| Resource URI | Maps to | Description |
|-------------|---------|-------------|
| `workflow://{id}` | `GET /api/workflows/{id}` | Workflow definition and metadata |
| `run://{runId}` | `GET /api/workflow-runs/{runId}` | Run status, task states, outputs |

### File structure

```
mcp/
  package.json          — @modelcontextprotocol/sdk (MIT), node-fetch
  tsconfig.json
  src/
    index.ts            — MCP server setup, transport selection (stdio/SSE)
    tools.ts            — Tool handlers proxying to j9t REST API
    resources.ts        — Resource handlers
    config.ts           — j9t URL, Bearer token, transport config
```

### Auth and security

- MCP server reads j9t Bearer token from `engine_api_token.txt` or environment variable
- SSE transport: only exposed when TLS is configured
- RBAC: MCP tools respect j9t roles (viewer = list/get, operator = run/cancel, admin = all)

### Docker deployment

Add MCP sidecar to `docker-compose.example.yml`:

```yaml
mcp:
  build: ./mcp
  depends_on: [jarvisagent]
  environment:
    J9T_URL: http://jarvisagent:8080
    J9T_TOKEN_FILE: /app/engine_api_token.txt
  volumes:
    - ./data:/app:ro
```

---

## 5. Object Storage (S3-compatible)

Covers AWS S3, MinIO (self-hosted, accessed over HTTP), and GCS (S3-compatible interop mode). All access via S3 REST protocol using libcurl + SigV4 signing via OpenSSL.

### S3Connector

Implements `ICloudConnector`. Handles SigV4 request signing via `SigV4Signer`.

**Connection params:**
```json
{
  "name": "my-s3",
  "type": "s3",
  "endpoint": "https://s3.amazonaws.com",
  "key_name": "aws-creds",
  "auth_type": "sigv4",
  "params": {
    "region": "us-east-1",
    "bucket": "workflow-outputs"
  }
}
```

**TestConnection:** `HEAD /{bucket}` — checks bucket exists and credentials work.

### SigV4Signer

```cpp
// engine/keys/sigV4Signer.h
class SigV4Signer
{
public:
    // Sign an HTTP request using AWS Signature V4.
    // Populates Authorization, X-Amz-Date, and X-Amz-Content-Sha256 headers.
    static void SignRequest(std::string const& method, std::string const& url,
                            std::string const& body, std::string const& region,
                            std::string const& service, std::string const& accessKeyId,
                            std::string const& secretKey,
                            std::vector<std::pair<std::string, std::string>>& headers);
};
```

Uses OpenSSL `HMAC()` with SHA-256 for signing key derivation and `EVP_Digest()` for payload hashing.

### S3CloudTaskExecutor

Task types: `s3_upload`, `s3_download`

```json
{
  "type": "s3_upload",
  "params": {
    "connection": "my-s3",
    "bucket": "reports",
    "key": "output/{{run_id}}/report.pdf",
    "local_path": "report.pdf"
  }
}
```

- Upload: `PUT /{bucket}/{key}` with file body
- Download: `GET /{bucket}/{key}` → write to `local_path` in working directory

### s3_watch trigger

New trigger type in `triggerEngine`. Polls `GET /{bucket}?list-type=2&prefix={prefix}` on configurable interval. Compares `LastModified` timestamps against last poll. Fires workflow when new objects appear. Stores last-seen timestamp in trigger state.

---

## 6. OneDrive

### OneDriveConnector

Implements `ICloudConnector`. Uses OAuth 2.0 authorization code flow with PKCE via `OAuthTokenManager`.

**Connection params:**
```json
{
  "name": "my-onedrive",
  "type": "onedrive",
  "endpoint": "https://graph.microsoft.com/v1.0",
  "key_name": "onedrive-oauth",
  "auth_type": "oauth2",
  "params": {
    "client_id": "...",
    "tenant_id": "...",
    "scopes": "Files.ReadWrite offline_access"
  }
}
```

**TestConnection:** `GET /me/drive` — verifies token and returns drive info.

### OAuthTokenManager

```cpp
// engine/keys/oauthTokenManager.h
class OAuthTokenManager
{
public:
    void Start();   // Start background refresh thread
    void Stop();

    // Get a valid access token (blocks if refresh in progress).
    std::string GetAccessToken(std::string const& keyName, std::string& errorMessage);

    // Store initial tokens after OAuth consent flow completes.
    void StoreTokens(std::string const& keyName, std::string const& accessToken,
                     std::string const& refreshToken, int64_t expiresInSeconds);

private:
    // Background thread: refreshes tokens expiring within 5 minutes.
    void RefreshLoop();
};
```

Tokens persisted in encrypted key store (`keys.json.enc`) with `key_type: "oauth"` and metadata for `refresh_token`, `expires_at`, `scopes`.

### OAuth consent flow

1. Frontend "Authorize" button calls `GET /api/connections/{name}/oauth/authorize` → backend returns authorization URL
2. Browser redirects to provider login → user consents → redirected back to `GET /api/connections/{name}/oauth/callback` with auth code
3. Backend exchanges code for tokens via provider's token endpoint
4. Tokens stored in KeyManager, `OAuthTokenManager` starts refresh loop

### OneDriveCloudTaskExecutor

Task types: `onedrive_upload`, `onedrive_download`

- Upload: `PUT /me/drive/root:/{remote_path}:/content` with file body
- Download: `GET /me/drive/root:/{remote_path}:/content` → write to local path in working directory

### onedrive_watch trigger

Polls `GET /me/drive/root:/{folder}:/delta` on interval. Delta queries return only changed items since last sync. Stores delta token in trigger state for efficient polling.

---

## 7. Snowflake

All C++ — no Python in backend. Uses Snowflake SQL REST API with RSA JWT authentication. All dependencies already vendored (libcurl, OpenSSL, simdjson).

### SnowflakeConnector

Implements `ICloudConnector`. Uses RSA RS256 JWT via `JwtGenerator`.

**Connection params:**
```json
{
  "name": "my-snowflake",
  "type": "snowflake",
  "endpoint": "xy12345.us-east-1",
  "key_name": "snowflake-keypair",
  "auth_type": "jwt_rsa",
  "params": {
    "account": "xy12345",
    "user": "SVC_JARVIS",
    "warehouse": "COMPUTE_WH",
    "database": "ANALYTICS",
    "schema": "PUBLIC"
  }
}
```

**TestConnection:** `POST /api/v2/statements` with `SELECT 1`.

### JwtGenerator

```cpp
// engine/keys/jwtGenerator.h
class JwtGenerator
{
public:
    // Generate an RS256-signed JWT.
    static std::string Generate(std::string const& headerJson, std::string const& payloadJson,
                                std::string const& privateKeyPem, std::string& errorMessage);

    // Convenience: generate a Snowflake-specific JWT with account + user + key fingerprint.
    static std::string GenerateSnowflakeJwt(std::string const& account, std::string const& user,
                                             std::string const& privateKeyPem,
                                             std::string& errorMessage);
private:
    static std::string Base64UrlEncode(std::vector<uint8_t> const& data);
};
```

Uses OpenSSL: `PEM_read_bio_PrivateKey()` for RSA key loading, `EVP_DigestSignInit/Update/Final()` with `EVP_sha256()` for RS256 signing. Base64url encoding for JWT segments.

### SnowflakeCloudTaskExecutor

Task type: `snowflake_query`

```json
{
  "type": "snowflake_query",
  "params": {
    "connection": "my-snowflake",
    "warehouse": "COMPUTE_WH",
    "query": "SELECT * FROM sales WHERE date = '{{binding.date}}'",
    "output_format": "csv"
  }
}
```

Execution flow:
1. Generate JWT via `JwtGenerator::GenerateSnowflakeJwt()`
2. `POST /api/v2/statements` with SQL body → receive `statementHandle`
3. Poll `GET /api/v2/statements/{statementHandle}` until status = `"resultSetAvailable"`
4. Parse result set JSON → write to `{task_id}.output.csv` or `.json` in working directory

Estimated implementation: ~1000-1500 lines C++ (SnowflakeConnector + JwtGenerator + SnowflakeCloudTaskExecutor).

---

## 8. Messaging (Slack, Email)

### Slack

**SlackConnector:** REST API via `https://slack.com/api/`. Bearer token auth (Bot token from KeyManager).

**TestConnection:** `POST /api/auth.test` — verifies token validity and returns workspace info.

**SlackCloudTaskExecutor** — task type: `slack_message`

```json
{
  "type": "slack_message",
  "params": {
    "connection": "my-slack",
    "channel": "#alerts",
    "text": "Workflow {{workflow_id}} completed: {{output}}"
  }
}
```

Implementation: `POST /api/chat.postMessage` with `channel` + `text` body via libcurl.

### Email

**EmailConnector:** SMTP send and IMAP read via libcurl (already supports both protocols).

**TestConnection (SMTP):** `EHLO` handshake. **TestConnection (IMAP):** `LOGIN` + `SELECT INBOX`.

**EmailCloudTaskExecutor** — task type: `email_send`

```json
{
  "type": "email_send",
  "params": {
    "connection": "my-email",
    "to": "team@company.com",
    "subject": "Report: {{workflow_id}}",
    "body": "See attached report.",
    "attachments": ["report.pdf"]
  }
}
```

Uses libcurl SMTP with `CURLOPT_MAIL_FROM`, `CURLOPT_MAIL_RCPT`, and RFC 2822 message format. Attachments as MIME multipart.

### email_watch trigger

Polls IMAP folder on interval via libcurl. Checks for unseen messages matching filter criteria (subject pattern, sender). Marks processed messages as seen. Stores last-seen UID in trigger state.

---

## 9. Database (PostgreSQL)

### PostgresConnector

Links libpq (PostgreSQL License — permissive, GPL-compatible).

**Connection params:**
```json
{
  "name": "my-postgres",
  "type": "postgres",
  "endpoint": "db.internal:5432",
  "key_name": "pg-creds",
  "auth_type": "basic_auth",
  "params": {
    "database": "analytics",
    "sslmode": "require"
  }
}
```

**TestConnection:** `PQconnectdb()` + `PQstatus()` check.

### DbQueryCloudTaskExecutor

Task type: `db_query`

```json
{
  "type": "db_query",
  "params": {
    "connection": "my-postgres",
    "query": "SELECT id, name, status FROM orders WHERE date = $1",
    "query_params": ["{{binding.date}}"],
    "output_format": "csv"
  }
}
```

Execution: `PQexecParams()` (parameterized query — template bindings are passed as query parameters, never interpolated into the SQL string, preventing SQL injection) → iterate `PQgetvalue()` → write CSV or JSON to working directory.

**Connection pooling (Phase 9):** Without pooling, each workflow task pays `PQconnectdb()` setup cost (TCP + TLS handshake + auth). For workflows with many sequential `db_query` tasks this becomes significant. Phase 9 adds `CloudConnectionPool` which maintains a pool of idle libpq connections keyed by connection name, with configurable max idle time and pool size. HTTP-based connectors (S3, Slack, etc.) already benefit from libcurl's built-in connection reuse and do not need explicit pooling.

---

## 10. Additional Integrations

### GitHub / GitLab

`GitConnector : ICloudConnector`. REST API via `https://api.github.com` or GitLab instance URL. Bearer token auth (PAT).

Task types:
- `git_create_issue` — POST issue with title, body, labels
- `git_comment_pr` — POST comment on a pull request
- `git_get_file` — GET file content from a repo (useful as workflow input)

Trigger: existing webhook trigger already handles GitHub/GitLab payloads. Enrich trigger metadata with event type, repo, branch for display in the dashboard.

### Jira / Linear

`JiraConnector : ICloudConnector`. REST API with Bearer token or OAuth.

Task types:
- `jira_create_issue` — POST issue with summary, description, type, priority
- `jira_update_issue` — PATCH fields on existing issue
- `jira_query` — JQL query → per-item fan-out (similar to Polarion filter)

### Google Sheets

REST API via `https://sheets.googleapis.com/v4/spreadsheets/`. OAuth 2.0 auth via `OAuthTokenManager`.

Task types:
- `sheets_read` — GET range → CSV/JSON to disk
- `sheets_write` — PUT range from workflow output

---

## 11. KeyManager Extension

### Current model

```cpp
struct Provider {
    std::string m_Name;
    std::string m_ApiKey;
};
```

### Extended model — typed credential hierarchy

Instead of a flat struct with a `key_type` string discriminator, use a polymorphic credential model for type safety:

```cpp
// engine/keys/credential.h

class ICredential
{
public:
    virtual ~ICredential() = default;
    virtual std::string GetType() const = 0;

    std::string m_Name;             // Unique credential name
};

class ApiKeyCredential : public ICredential
{
public:
    std::string GetType() const override { return "api_key"; }

    std::string m_ApiKey;           // API key or PAT
};

class OAuthCredential : public ICredential
{
public:
    std::string GetType() const override { return "oauth"; }

    std::string m_AccessToken;
    std::string m_RefreshToken;
    int64_t m_ExpiresAt{0};         // Unix timestamp
    std::string m_Scopes;
};

class KeyPairCredential : public ICredential
{
public:
    std::string GetType() const override { return "key_pair"; }

    std::string m_PrivateKeyPem;    // RSA private key (PEM format)
};

class BasicAuthCredential : public ICredential
{
public:
    std::string GetType() const override { return "credentials"; }

    std::string m_Username;
    std::string m_Password;
};
```

`KeyManager` stores `std::unordered_map<std::string, std::unique_ptr<ICredential>>`. Connectors request the specific type they need via `dynamic_cast` — a `SnowflakeConnector` asks for `KeyPairCredential`, an `OneDriveConnector` asks for `OAuthCredential`. Type mismatches produce clear error messages at connection test time rather than silent field misinterpretation at runtime.

Serialization to `keys.json.enc`: the `GetType()` string is written as a JSON field, and the appropriate subclass fields are serialized/deserialized based on it. All fields encrypted via existing AES-256-GCM. Backward-compatible: existing keys without a type field are loaded as `ApiKeyCredential`.

---

## 12. Frontend Changes

### New navigation

```
Workflows | Editor | AI Manager | Keys | Connections | Assistant | Log | Dashboard
```

### New files

| File | Description |
|------|-------------|
| `views/ConnectionsView.tsx` | Cloud connection CRUD, test, OAuth authorization. Follows `AiManagerView.tsx` pattern: list + edit form + type-specific fields + test button + save + dirty state tracking. |
| `api/connections.ts` | REST client: `listConnections()`, `createConnection()`, `updateConnection()`, `deleteConnection()`, `testConnection()`, `getOAuthUrl()` |
| `components/OAuthFlowDialog.tsx` | Modal that opens OAuth consent URL in popup window, listens for callback, reports success/failure |

### Modified files

| File | Change |
|------|--------|
| `App.tsx` | Add `"connections"` to `RouteKey` union. Rename "AI Keys" button to "Keys". Add "Connections" nav button with dirty indicator (`*`). Render `ConnectionsView` for the `"connections"` route. Add `connectionsDirty` state (same pattern as `aiManagerDirty` and `keysDirty`). |
| `views/ProvidersSettingsView.tsx` | Add credential type dropdown (api_key / oauth / key_pair / credentials). Conditional fields: oauth shows token status + "Authorize" button + expiry; key_pair shows textarea for PEM; credentials shows username + password. |
| `api/providers.ts` | Extend `ProviderEntry` type to match `ICredential` hierarchy: add `credential_type`, and type-specific fields (`username`, `refresh_token`, `expires_at`, `scopes`, `private_key_pem`). |
| `jcwf/types.ts` | Add to `JcwfTaskType`: `s3_upload`, `s3_download`, `onedrive_upload`, `onedrive_download`, `snowflake_query`, `db_query`, `slack_message`, `email_send`, `polarion_write`, `git_create_issue`, `git_comment_pr`, `git_get_file`, `jira_create_issue`, `jira_update_issue`, `jira_query`, `sheets_read`, `sheets_write`. Add to `JcwfTriggerType`: `s3_watch`, `onedrive_watch`, `email_watch`. Add to `JcwfFilterSourceKind`: `jira_query` (JQL fan-out, like `polarion_query`). Add optional `connection` field to `JcwfTask` and `JcwfFilterSource`. |
| `editor/WorkflowEditorView.tsx` | Inspector panel: add cloud task type sections with connection dropdown + type-specific param fields. Trigger panel: add cloud trigger types with connection dropdown + config fields. |
| `editor/FilterBuilderDialog.tsx` | Add `connection` dropdown for `polarion_query` and `jira_query` filter sources (replaces inline `base_url`/`project_id`/`key_name` fields with a single connection reference). |
| `editor/validation.ts` | Add validation rules for cloud task types: require `connection` param, validate type-specific required fields (e.g., `s3_upload` needs `bucket` + `key` + `local_path`; `db_query` needs `query`; `slack_message` needs `channel` + `text`). |
| `templates/workflowTemplates.ts` | Add example cloud workflow templates (e.g., "S3 Upload Pipeline", "Snowflake ETL", "Slack Notification Workflow") so users can start from a working example. |
| `dashboard/ui/src/components/StatusBar.tsx` | Add connection health status indicators. Show circuit breaker state (Phase 9) when a connection is in "open" state. |

### ConnectionsView layout

```
+-------------------------------------------------------+
| Connections                              [+ Add] [Save]|
+-------------------------------------------------------+
| [list of connections]                                  |
| +---------------------------------------------------+ |
| | polarion: my-polarion          Connected  [Test][x]| |
| |   endpoint: https://polarion.company.com           | |
| |   project: GoKartProcurement                       | |
| |   key: polarion-pat                                | |
| +---------------------------------------------------+ |
| | s3: production-bucket          Connected  [Test][x]| |
| |   endpoint: https://s3.amazonaws.com               | |
| |   region: us-east-1                                | |
| |   bucket: workflow-outputs                         | |
| +---------------------------------------------------+ |
|                                                       |
| [edit form when connection selected]                  |
| Name: [____________]                                  |
| Type: [dropdown: polarion|s3|onedrive|snowflake|...]  |
| Endpoint: [____________]                              |
| Key: [dropdown from Keys page]                        |
| [type-specific fields rendered dynamically]           |
+-------------------------------------------------------+
```

---

## 13. REST API Endpoints

New endpoints in `webServer.cpp`:

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/connections` | List all connections with status |
| `POST` | `/api/connections` | Create a new connection |
| `PUT` | `/api/connections/{name}` | Update an existing connection |
| `DELETE` | `/api/connections/{name}` | Delete a connection |
| `POST` | `/api/connections/{name}/test` | Test connectivity |
| `POST` | `/api/connections/save` | Persist connections to config.json |
| `GET` | `/api/connections/{name}/oauth/authorize` | Get OAuth authorization URL |
| `GET` | `/api/connections/{name}/oauth/callback` | OAuth redirect callback |

RBAC: connections endpoints require `admin` role in Engine mode.

---

## 14. Security Review

All cloud access in this plan is secure. Every integration uses HTTPS via libcurl (TLS 1.2+), credentials are stored in the AES-256-GCM encrypted key store, and no secrets appear in JCWF files. This section documents the security measures and identifies items to verify during implementation.

### Outbound connection security

| Measure | Status | Notes |
|---------|--------|-------|
| All cloud API calls over HTTPS | Covered | libcurl with OpenSSL, TLS 1.2+ enforced |
| TLS certificate verification | **Verify** | `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST` must remain enabled. Never disable for convenience. CA bundle path already configured in existing `CurlWrapper`. |
| Credentials never in JCWF files | Covered | All credentials referenced by `key_name` → resolved at runtime from encrypted key store |
| Credentials in transit | Covered | Bearer tokens, JWTs, SigV4 signatures sent over HTTPS only |
| Credentials at rest | Covered | AES-256-GCM encrypted `keys.json.enc` with master password. OAuth refresh tokens, RSA private keys, DB passwords all stored encrypted. |
| Credential memory handling | **Verify** | Use `OPENSSL_cleanse()` to zero sensitive buffers (JWT tokens, decrypted PEM keys) after use. Already done in `keyEncryption.cpp` — extend to new code. |

### Authentication security

| Measure | Status | Notes |
|---------|--------|-------|
| OAuth 2.0 PKCE | Covered | Prevents auth code interception in OneDrive/Google flows |
| OAuth state parameter | **Add** | OAuth authorize URL must include a random `state` parameter. Callback must verify it matches. Prevents CSRF attacks on the OAuth callback endpoint. |
| JWT token expiry | Covered | `JwtGenerator` sets `exp` claim (1 hour). `OAuthTokenManager` refreshes before expiry. |
| RSA key minimum size | **Enforce** | Reject RSA keys shorter than 2048 bits in `JwtGenerator`. |
| SigV4 timestamp validation | Covered | SigV4 includes `X-Amz-Date` — requests older than 15 minutes are rejected by the S3 service. |

### Data security

| Measure | Status | Notes |
|---------|--------|-------|
| SQL injection prevention | Covered | `PQexecParams()` with parameterized queries. Template bindings passed as parameters, never interpolated. |
| Download size limits | **Add** | Cloud downloads (S3, OneDrive, Polarion attachments) must enforce a configurable max file size (default 100 MB) via `CURLOPT_MAXFILESIZE_LARGE`. Prevents disk exhaustion from unexpected large files. |
| Path traversal in cloud paths | **Add** | Validate that `local_path` in S3/OneDrive task params does not contain `..` or absolute paths. Cloud task executors must resolve paths relative to the workflow working directory only. |
| Snowflake query result limits | **Add** | Enforce a configurable max result set size. Snowflake queries can return unbounded data — add a `LIMIT` guard or response size check. |

### Audit and access control

| Measure | Status | Notes |
|---------|--------|-------|
| Connection CRUD audit logging | **Add** | Log connection create/update/delete events to `log/security.txt` with admin identity, connection name, and type. |
| OAuth grant audit logging | **Add** | Log OAuth authorization and token refresh events to security log. Include connection name, scopes granted, token expiry. |
| Cloud task execution audit logging | **Add** | Log cloud task execution to security log: task type, connection name, target (bucket/path/query), outcome. Enables tracing of what data left or entered the system via cloud connectors. |
| Connection endpoint RBAC | Covered | Connections endpoints require `admin` role in Engine mode (see section 13). Viewers and operators cannot see or modify connection configurations. |

### Docker and network

| Measure | Status | Notes |
|---------|--------|-------|
| Outbound network policy | **Document** | Cloud integrations require outbound HTTPS to various endpoints. Document recommended egress firewall rules per connector type. Provide Kubernetes `NetworkPolicy` and Docker `--network` examples. |
| Container filesystem isolation | Covered | Cloud-downloaded files land in the mounted data directory only. Container cannot write outside `/app/`. |
| MCP sidecar isolation | Covered | MCP server communicates with j9t over localhost HTTP. Bearer token required. Read-only volume mount for token file. |

### Cyber security documentation update

`doc/cyber security.md` must be updated to cover cloud integration security. Add the following sections:

1. **Cloud connector security model** — how connections are stored, how credentials are resolved, outbound TLS enforcement, and audit logging for cloud operations.
2. **OAuth 2.0 security** — PKCE flow, state parameter CSRF protection, token refresh lifecycle, encrypted storage of refresh tokens.
3. **Outbound data flow** — new data paths introduced by cloud integrations (j9t → S3, j9t → Snowflake, j9t → OneDrive, etc.). What data leaves the system, under what circumstances, and how it is protected in transit.
4. **Egress network policy** — recommended firewall rules, allowed outbound endpoints per connector type, Kubernetes `NetworkPolicy` examples.
5. **Cloud-downloaded content risks** — downloaded files (S3 objects, OneDrive files, Polarion attachments) are untrusted input. Size limits, path traversal prevention, and content-type validation.
6. **MCP security** — bearer token auth, RBAC scope limiting, SSE transport requires TLS.
7. **Updated "Editions at a Glance" table** — add cloud connector columns (Studio: connections configurable, Engine: connections configurable + audit logged).
8. **Updated "Remaining threats" section** — add: credential exposure if encrypted key file is compromised, OAuth token theft if master password is weak, outbound data exfiltration via misconfigured cloud tasks.
9. **Updated "Admin responsibility" section** — add: review cloud connections, restrict OAuth scopes to minimum needed, configure egress firewall rules, audit cloud task execution logs.

---

## 15. Cloud Integration Documentation

A new document `doc/cloud-integration.md` should be created covering:

### User-facing documentation

1. **Getting started with Connections** — how to add a cloud connection via the Connections tab, link a key, test connectivity.
2. **Connection types reference** — one section per connector type (Polarion, S3, OneDrive, Snowflake, Slack, Email, PostgreSQL, GitHub, Jira, Google Sheets). For each:
   - Required connection params
   - Authentication method and key type
   - Example connection configuration
   - Supported task types with JSON examples
   - Supported trigger types with configuration
3. **Keys page** — how to create keys of each type (API key, OAuth, key pair, credentials). OAuth authorization flow walkthrough with screenshots.
4. **Cloud task types** — reference for each cloud task type with full param documentation, template variable support, file input/output conventions.
5. **Cloud trigger types** — reference for each trigger type with polling behavior, configuration, and example JCWF snippets.

### Operator documentation

6. **Security configuration** — egress firewall rules per connector, OAuth scope recommendations, audit log monitoring for cloud events.
7. **Docker deployment with cloud access** — updated `docker-compose.example.yml`, outbound network requirements, environment variables for MCP sidecar.
8. **Troubleshooting** — common connection errors (TLS failures, OAuth token expiry, SigV4 clock skew, rate limiting by cloud providers).

### Developer documentation

9. **Adding a new connector** — step-by-step guide for implementing `ICloudConnector`, registering in `CloudConnectorRegistry`, adding a task executor, and adding frontend support.
10. **Cloud abstraction architecture** — class diagram, data flow, credential resolution lifecycle.

### File location

```
doc/
  cloud-integration.md              (+) main cloud integration documentation
  cyber security.md                 (~) updated with cloud security sections (see above)
```

### Cross-references

- `doc/api-endpoints.md` — add `/api/connections` endpoints
- `doc/JC_Workflow_Specification.md` — add new task types and trigger types to the spec
- `integration/README.md` — reference cloud connectors as an alternative to raw webhooks

---

## 16. Implementation Order

1. **Phase 0 — Foundation:** abstraction layer (`ICloudConnector`, `ICredential` hierarchy, `CloudRetryPolicy`, `SecretRedactor`), connections API + UI
2. **Phase 1 — MCP:** standalone TypeScript server — highest ecosystem leverage, zero C++ changes
3. **Phase 2 — Polarion:** first connector, validates abstraction against existing working code
4. **Phase 3 — S3:** first general-purpose connector, low UI complexity, high business value
5. **Phase 4 — PostgreSQL:** validates DB abstraction, lower complexity than Snowflake, easy local testing
6. --- **Ship milestone:** foundation + MCP + Polarion + S3 + PostgreSQL ---
7. **Phase 5 — OneDrive:** OAuth flow, Graph API, upload/download, delta polling
8. **Phase 6 — Snowflake:** JWT RSA auth, SQL query executor, async polling
9. **Phase 7 — Messaging:** Slack (REST) and Email (SMTP/IMAP via libcurl)
10. **Phase 8 — Additional:** GitHub/GitLab, Jira, Google Sheets
11. **Phase 9a — Runtime Resilience:** circuit breaker, connection pool, cancellation wiring, provider rate limits, health checks
12. **Phase 9b — Security & Audit:** audit logging, OAuth CSRF, download limits, path traversal, TLS verification
13. **Phase 9c — Deployment & Ops:** firewall docs, container signing
