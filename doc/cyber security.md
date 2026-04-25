# Cyber Security

JarvisAgent ships as two editions with different security profiles. This document describes the safety measures in place, the remaining threats, and the responsibilities of operators and end users.

---

## Executive Summary

- **One auth funnel, both editions.** Per-IP lockout → credential validation → tier-appropriate rate limit → RBAC role gate. Studio and Engine differ only in *which routes* exist; *how* they authenticate is identical.
- **Three credential types.** MCP API key (`Authorization: Bearer mcp_…`), session cookie (browser, set by `POST /api/auth/login` after a valid MCP key), HMAC-SHA256 signature (webhooks). Anonymous access is restricted to a tightly enumerated bootstrap allowlist.
- **Two-tier rate limit.** Tight per-IP for unauthenticated/invalid traffic (100 req/min, burst 20); generous per-user once a credential validates (1200 req/min, burst 200). Defends against credential-stuffing without throttling legitimate clients.
- **Defense in depth.** AES-256-GCM-encrypted MCP key store, 90-day key expiry with self-renew, mandatory webhook secrets, full audit log to `log/security.txt`, edition-isolated attack surface (Engine compile-time excludes the workflow editor, AI assistant, AI JCWF generation, and workflow CRUD).
- **Identity from the credential, not the gateway.** When deployed behind an API gateway, j9t treats `X-Forwarded-User` / `X-Forwarded-Role` as a cross-check on top of the credential — they may downgrade the role but never authenticate alone.

---

## Abbreviations

| Term | Meaning |
|------|---------|
| **AI** | Artificial Intelligence — refers to cloud language models (e.g. GPT, Sonnet, Opus) that generate text from prompts |
| **API** | Application Programming Interface — a set of URLs that software uses to communicate with j9t |
| **Bearer token** | A secret string sent in an HTTP header to prove identity (like a password for API access) |
| **CRUD** | Create, Read, Update, Delete — the four basic operations on data |
| **CSP** | Content-Security-Policy — an HTTP header that controls which scripts, styles, and connections a web page is allowed to use |
| **DAG** | Directed Acyclic Graph — a workflow structure where tasks have dependencies but no circular loops |
| **DoS / DDoS** | Denial of Service / Distributed Denial of Service — an attack that floods a server with requests to make it unavailable |
| **HMAC-SHA256** | Hash-based Message Authentication Code using SHA-256 — a way to sign a message so the receiver can verify it was not tampered with and came from a trusted sender |
| **HSTS** | HTTP Strict-Transport-Security — an HTTP header that tells browsers to only connect via HTTPS, preventing downgrade attacks |
| **HTTP / HTTPS** | HyperText Transfer Protocol (/ Secure) — the protocol web browsers and APIs use to communicate. HTTPS adds encryption via TLS |
| **JCWF** | JC Workflow Format — j9t's JSON-based file format for defining workflows |
| **j9t** | Short name for JarvisAgent |
| **JWT** | JSON Web Token — a compact, self-contained token format used to pass identity claims between services |
| **KMS** | Key Management Service — a cloud service (e.g. AWS KMS, Azure Key Vault) that stores and manages encryption keys |
| **LDAP** | Lightweight Directory Access Protocol — a protocol for querying a corporate user directory (e.g. Active Directory) |
| **MFA** | Multi-Factor Authentication — requiring two or more verification methods (e.g. password + phone code) to prove identity |
| **OIDC** | OpenID Connect — an identity layer on top of OAuth 2.0 that lets applications verify user identity via an external provider (e.g. Google, Azure AD, Okta) |
| **Ops team** | Operations team — the people responsible for deploying, monitoring, and maintaining servers in production |
| **PII** | Personally Identifiable Information — data that can identify an individual (name, email, address, etc.) |
| **RBAC** | Role-Based Access Control — restricting system access based on a user's assigned role (e.g. admin, operator, viewer) |
| **SAML** | Security Assertion Markup Language — an XML-based standard for exchanging authentication data between an identity provider and a service |
| **SIEM** | Security Information and Event Management — software that collects and analyzes security logs from multiple systems to detect threats (e.g. Splunk, Microsoft Sentinel) |
| **SPA** | Single-Page Application — a web app that loads once and updates dynamically (the j9t dashboard) |
| **SSO** | Single Sign-On — allowing a user to log in once and access multiple systems without re-authenticating |
| **TLS** | Transport Layer Security — encryption for network traffic (the "S" in HTTPS) |
| **WAF** | Web Application Firewall — a security layer that filters, monitors, and blocks malicious HTTP traffic before it reaches the application |
| **WebSocket (WS)** | A persistent two-way connection between browser and server for real-time updates |

---

## Editions at a Glance

| | j9t Studio | j9t Engine |
|-|-----------|-----------|
| **Purpose** | Developer workstation | Production server |
| **Network exposure** | Localhost only | LAN / internet |
| **Browser UI auth** | Session cookie from `POST /api/auth/login` (HttpOnly + SameSite=Strict) | Same |
| **MCP / programmatic auth** | MCP API key required | Same |
| **Webhook auth** | HMAC-SHA256 (mandatory secret) | Same |
| **Rate limiting** | Two-tier: pre-auth per-IP (100 req/min, burst 20) + authenticated per-user (1200 req/min, burst 200) | Same |
| **Failed auth lockout** | 10 failures / 5 min → 15-min IP lockout | Same |
| **Key expiry / renewal** | 90-day MCP key lifetime; self-renew before expiry | Same |
| **RBAC** | 3 roles (admin, operator, viewer) on MCP keys, session cookies, and gateway headers | Same |
| **Gateway identity** | Optional cross-check on `X-Forwarded-User` / `X-Forwarded-Role`; never replaces the primary credential | Same |
| **Audit logging** | `log/security.txt` (rotating, 10 MB x 5), includes user identity | Same |
| **Security headers** | CSP, X-Frame-Options, Referrer-Policy, Permissions-Policy | Same + HSTS when TLS enabled |
| **Request body limit** | Configurable `MaxRequestBodyMB` (default 10 MB) | Same |
| **Built-in TLS** | Optional (`TlsCert`/`TlsKey` in config.json) | Same |
| **Workflow CRUD + AI assistant + AI JCWF generation** | Available | Compile-time removed (`removefiles` + `#ifdef J9T_STUDIO`) |
| **Attack surface** | Full feature set | Minimal — runtime + monitoring + admin (config / providers / connections) |

---

## j9t Studio — Developer Workstation

### Safety measures

- **Compile-time feature gating.** Studio-only code (workflow CRUD, AI assistant, AI JCWF generation, settings API) is included only when built with `J9T_STUDIO`. Engine builds physically exclude these modules.
- **Script path policy.** Shell tasks must reference scripts under the `scripts/` directory. Path traversal (`..`) is rejected by both the validator and the shell task executor.
- **Validation tiers.** The backend validator enforces schema correctness (Tier A), runtime policy (Tier B), feasibility checks (Tier C), and informational warnings (Tier D) before a workflow runs.
- **Disk-first design.** All inputs, outputs, and intermediate results are written to disk. Nothing is held only in memory, making post-incident forensics straightforward.

### Remaining threats

- **AI-generated scripts.** The Generate and Fix Script features produce shell and Python scripts from AI output. A malicious or buggy prompt can produce scripts that delete files, exfiltrate data, or consume disk space. The script review panel lets the developer inspect before accepting, but there is no sandbox.
- **AI assistant tool access.** The assistant can read files, write files, edit files, and run shell commands (with user approval). A compromised or manipulated conversation could lead to unintended file modifications.
- **No TLS by default.** Studio serves HTTP on the configured port unless `TlsCert`/`TlsKey` are set in `config.json`. If reachable beyond localhost, configure TLS to encrypt session cookies and the master-password unlock.

### Operator responsibility

Studio is designed for **single-developer or small-team use on workstations**. Authentication is identical to Engine — every browser session and MCP client must hold a valid MCP API key. The operator is responsible for:

- Reviewing AI-generated scripts before accepting them.
- Approving or rejecting assistant tool calls (mutating tools require explicit approval).
- Keeping API keys secure (stored encrypted in `keys.json.enc` with a master password; the same password also unlocks `mcp_keys.json.enc`).
- Activating the first-run MCP admin enrollment token j9t prints to stderr on initial startup — required before any browser session or MCP client can connect.
- Configuring TLS (`TlsCert` + `TlsKey`) when the port is reachable beyond localhost.

---

## j9t Engine — Production Server

### Safety measures

- **One auth funnel — same in both editions.** j9t authenticates each non-bootstrap request via either (1) `Authorization: Bearer mcp_...` validated against the encrypted MCP key store, or (2) a session cookie set by `POST /api/auth/login` after the user submitted a valid MCP key. Webhook routes (`POST /api/webhook/<id>`, `POST /api/integrations/n8n/start`) authenticate via mandatory HMAC-SHA256 over the raw body. Gateway-injected `X-Forwarded-User` / `X-Forwarded-Role` headers are an optional cross-check on top of one of the above primary credentials — they verify the gateway-asserted user matches and may cap the role downward, but never authenticate by themselves. Bootstrap routes (the eight enumerated below) are public; everything else requires a credential. There is no legacy bearer-token fallback and no anonymous-admin path. See `doc/engine-studio-capability-review.md` for the full funnel diagram and route surface.
- **MCP API keys.** Each key is a per-user credential (`mcp_` + 64 hex chars, 32 bytes of `RAND_bytes`) with its own identity, role (admin / operator / viewer), adhoc-submission flag, disk quota, and retention-policy ceiling. The raw key is shown to the user exactly once at activation; only its SHA-256 hash is persisted. The store (`mcp_keys.json.enc`) is encrypted at rest with AES-256-GCM + PBKDF2 — same format as `keys.json.enc`, unlocked with the same master password. Key comparison uses `CRYPTO_memcmp` for constant-time equality.
- **Enrollment-token provisioning.** The admin never sees a user's final MCP key. Admin calls `POST /api/auth/mcp-keys/enroll` to create a single-use enrollment token (30-min default TTL), shares it with the user over a secure channel, and the user exchanges it at `POST /api/auth/mcp-keys/activate` — receiving their real key once, never again. This follows the HashiCorp Vault / Auth0 / AWS IAM pattern.
- **Key expiry and self-renewal.** MCP keys expire after 90 days by default (admin-configurable). Within 30 days of expiry, the user can self-renew at `POST /api/auth/mcp-keys/self-renew` using their still-valid key — no admin involvement. The old key enters a 24-hour grace period, then is disabled. If the window is missed, the admin issues a fresh enrollment token.
- **Dashboard sessions.** Engine browser login flow: user pastes their MCP key into the login page → `POST /api/auth/login` validates it and returns an HttpOnly + SameSite=Strict cookie (plus `Secure` when TLS is enabled). Session IDs are 256-bit random, server-side only, 8-hour sliding timeout (configurable via `session_timeout_hours`). `POST /api/auth/logout` destroys both cookie and server state. Sessions do not persist across j9t restarts.
- **Master password in protected memory.** The master password is held in a `SecureString` RAII buffer that calls `mlock()` / `VirtualLock()` (preventing swap-to-disk) and `explicit_bzero()` / `SecureZeroMemory()` on destruction. It is never logged, never written to env vars, never persisted. Unlock happens once per j9t start via `POST /api/settings/keys/unlock`; until unlocked, MCP-authenticated requests and AI calls are unavailable. **Honest limitation:** a process with `ptrace` access (root on Linux, or a debugger attached to j9t) can still read the locked page — no userspace application can fully prevent that. `SecureString` raises the bar against every lesser class of attacker (swap files, crash dumps, casual memory scraping, freed-memory residue). The authoritative defence against privileged-process attackers is OS-level isolation: run j9t in Docker (Docker's default capability set already excludes `CAP_SYS_PTRACE`, so in-container ptrace is blocked out of the box — the provided `docker-compose.example.yml` also adds `security_opt: no-new-privileges:true` for defence in depth) or a dedicated VM with ptrace-restricted namespaces, and never grant `CAP_SYS_PTRACE` or run with `--privileged`.
- **Failed auth lockout.** After 10 failed authentication attempts from the same IP within 5 minutes, that IP is blocked for 15 minutes. Locked-out requests receive HTTP 403 with a `Retry-After: 900` header. The lockout is checked before any rate-limit work (locked IPs don't consume tokens in either tier). Successful authentication clears the failure count. Lockout entries are cleaned up automatically.
- **HMAC-SHA256 webhook authentication.** Webhook triggers require a per-workflow secret. The caller must include an `X-Webhook-Signature: sha256=<hex>` header computed over the raw request body. Signature verification uses constant-time comparison. In Engine mode, a webhook secret is mandatory — webhooks without a configured secret are rejected with HTTP 403.
- **WebSocket authentication.** Browser WebSocket upgrades carry the session cookie automatically; Crow's `.onaccept` hook calls `Authenticate()` at handshake time and rejects upgrades that do not produce a valid auth result. No in-band `type:"auth"` message is used — there is no post-upgrade auth window to exploit.
- **Two-tier rate limiting.** A token-bucket policy split by authentication state, so legitimate clients are not throttled together with unauthenticated probers:
  - **Pre-auth bucket (per-IP):** 100 req/min, burst 20. Applies when the request has no valid credential (missing/unrecognised header, or `mcp_`-prefixed token that fails validation). Defends against credential-stuffing and anonymous flooding. Combined with the failed-auth lockout (above), repeat offenders graduate from 429 → 15-minute ban. Logged as `rate_limited_preauth ip=…`.
  - **Authenticated bucket (per-user):** 1200 req/min, burst 200. Applies once an MCP key or session has validated, keyed by the credential's user. Sized to comfortably absorb dashboard polling, MCP heartbeats, and bursty automation; runaway authenticated traffic surfaces in the audit log (with the user's identity attached) for investigation rather than being blanket-blocked at the auth layer. Logged as `rate_limited_authenticated user=…`.

  Both tiers respond with HTTP 429 and a `Retry-After` header. Buckets idle for 10 minutes are evicted; the cleanup runs in the same lock as the bucket consume, so housekeeping never blocks the request path independently.
- **Security audit logging.** All auth-related events are logged to a dedicated rotating log file (`log/security.txt`, 10 MB x 5 files) as well as the application log (TUI/console). Logged events include: auth success/failure with IP and endpoint, rate limit triggers, lockout triggers, webhook accept/reject with workflow ID, shutdown requests, and run control actions (cancel/pause/resume/stop) with run ID. The security log is accessible via `GET /api/log/security` (admin-auth required) and visible in the dashboard Log Viewer's "Security" tab with 3-second polling. Log macros: `LOG_SECURITY_INFO` / `LOG_SECURITY_WARN`.
- **Built-in TLS (HTTPS).** Optional native TLS via Crow's SSL support. Set `"TlsCert"` and `"TlsKey"` in `config.json` to point to PEM certificate and key files. When configured, j9t serves HTTPS on port 8443 instead of HTTP on 8080. If only one field is set or the files don't exist, j9t refuses to start (no silent fallback). `GET /api/status` includes `"tls": true/false`. This eliminates the cleartext last-mile between a reverse proxy and j9t, and can replace the reverse proxy entirely for simpler deployments.
- **Gateway-trusted identity headers.** When deployed behind an API gateway (Kong, AWS API Gateway, Traefik, nginx), j9t trusts identity headers injected by the gateway. Configure `"TrustedProxyHeader": "X-Forwarded-User"` and `"TrustedRoleHeader": "X-Forwarded-Role"` in `config.json`. The gateway handles authentication (OIDC, MFA, SSO) and j9t reads the authenticated user and role from the headers. This allows per-user identity in audit logs without j9t implementing its own identity provider integration. **Important:** the headers are a *cross-check* on top of the credential — they may downgrade the credential's role to a lower one but never authenticate alone, and never escalate. If `TrustedRoleHeader` is configured but the header is absent on a request, the credential's role passes through unchanged (no implicit default-deny). The credential remains the source of truth.
- **Role-Based Access Control (RBAC).** Three roles with descending privilege: **admin** (full access including shutdown, security logs, and MCP key CRUD), **operator** (run control, workflow monitoring, adhoc submission when enabled, application logs), **viewer** (read-only dashboard, workflow list, run status). The role is carried by the MCP key itself (set at enrollment), by the session cookie derived from it, or by the `X-Forwarded-Role` header injected by an API gateway (default `viewer` if absent). Routes enforce minimum required role — a viewer attempting to stop a run, access security logs, or create an MCP enrollment receives HTTP 403 `insufficient_role`.
- **Request body size limit.** Configurable maximum HTTP body size (`"MaxRequestBodyMB": 10` in `config.json`, default 10 MB). Oversized requests are rejected with HTTP 413 `payload_too_large` before parsing. Protects against memory exhaustion attacks via large webhook payloads.
- **Security response headers.** All HTTP responses include: `Content-Security-Policy` (restricts script/style/connection sources to `'self'`), `X-Frame-Options: DENY` (prevents clickjacking), `X-Content-Type-Options: nosniff`, `Referrer-Policy: strict-origin-when-cross-origin`, `Permissions-Policy: camera=(), microphone=(), geolocation=()`. When TLS is enabled, `Strict-Transport-Security` (HSTS) is also set.
- **Reduced attack surface.** Studio-only modules (workflow editor, AI assistant, AI JCWF generation, workflow CRUD, validation) are excluded at compile time. The Engine binary is physically smaller and exposes fewer endpoints. The `removefiles {...}` directive in `premake5.lua` (gated on `_OPTIONS["engine"]`) drops the Studio-only source files from the engine's compile graph entirely — they are never compiled, never linked, never present as symbols in the engine binary. Verified clean separation (2026-04-25): on a `premake5 clean` slate, `nm bin/Debug/jarvisAgent-engine | grep -c "AssistantController\|AiJcwfService"` returns **0** while the studio counterpart returns **>600**; the engine `bin-int/engine/Debug/` objdir contains no `aiJcwfService.o`, `assistant*.o`, or `webServerStudio.o`. Re-run that two-line check whenever the edition gating in `premake5.lua` is touched, and after any change to which sources are studio-only.
- **Public bootstrap allowlist.** The following routes are reachable without a credential because they are the doors used to obtain one:
  | Endpoint | Method | Reason |
  |----------|--------|--------|
  | `/` | GET | Dashboard HTML shell |
  | `/dash-assets/<path>` | GET | Dashboard static assets |
  | `/api/status` | GET | Health probe |
  | `/api/auth/login` | POST | The MCP key in the body *is* the auth |
  | `/api/auth/mcp-keys/activate` | POST | The enrollment token in the body *is* the auth |
  | `/api/settings/keys/status` | GET | Pre-unlock probe |
  | `/api/settings/keys/unlock` | POST | The master password in the body *is* the auth |
  | `/api/auth/logout` | POST | Requires session cookie but no token |
  Every other route requires a credential. The list is enforced as code in `WebServer::RegisterCommonRoutes()`; adding a route to it must accompany an explicit risk note.

### Remaining threats

- **Gateway as the only line of identity.** When `TrustedProxyHeader` is configured, j9t still requires a primary credential (MCP token or session cookie) and uses the gateway header only as a cross-check. The historical "trust the header alone" path was removed by §5i so that an accidentally-exposed Engine cannot be impersonated by injecting a fake `X-Forwarded-User`. The remaining residual risk is on the gateway itself: if the gateway is misconfigured to forward unauthenticated requests, j9t still requires the token, but a stolen token plus a forged header chain would replay successfully. **Mitigation:** keep the gateway authenticated against a real IdP; treat MCP tokens as the source of truth for identity; rotate compromised keys via the admin enrollment flow.
- **No encryption at rest.** Workflow data, AI outputs, and logs are stored as plaintext files on disk. If the server's storage is compromised (stolen disk, leaked snapshot, improper decommissioning), all data is exposed. **The operator must deploy j9t on encrypted storage** — see "Admin responsibility" below. Worth calling out specifically: webhook trigger secrets live in plaintext inside the JCWF (`workflows/<id>/global.json` → `triggers[].params.secret`). The runtime never returns trigger params to any API client (`GET /api/workflows/<id>` strips them), so dashboard or MCP users — at any role — cannot read the secret over the wire; but anyone with read access to the install directory can.
- **Log data sensitivity.** `GET /api/log` returns application logs that may contain prompt content, AI responses, file paths, and error traces. `GET /api/log/security` exposes IP addresses, user identities, and auth event history. Both are role-protected (operator+ for app log, admin for security log) but log content is not redacted.
- **Unauthenticated shutdown via process signal.** The bearer token protects the `POST /api/shutdown` endpoint, but an attacker with OS-level access can still kill the process via signals (SIGTERM, SIGKILL). This is outside j9t's control.
- **Denial of service.** Rate limiting, auth lockout, and request body size limits mitigate application-level attacks, but do not protect against network-level attacks (SYN floods, bandwidth exhaustion). Use a WAF or cloud-level DDoS protection for internet-facing deployments.

### Adhoc workflow submission

`POST /api/workflows/run-adhoc` lets an MCP client stage and execute a JCWF that was *not* pre-registered. Because the JCWF is caller-supplied, the feature is deliberately locked down:

- **No script submission.** The adhoc payload is a canvas JSON only; any `shell` / `python` task must reference a script that already exists under `scripts/` (created by admins, the editor's Generate button, or the AI assistant). Executable code cannot be injected through this endpoint.
- **Opt-in per key.** `adhoc_enabled` on the MCP key defaults to `false`. Admin must enable it explicitly at enrollment time.
- **Role gate.** Requires at least `operator` role — viewers cannot submit.
- **Per-user disk quota.** Each MCP key carries `disk_quota_mb` (default 1 GB). The `AdhocWorkflowManager` tracks cumulative usage across the user's active adhoc folders; submissions past the quota return HTTP 413 `quota_exceeded`.
- **Per-run AI call cap.** `max_ai_calls_per_jcwf` in `config.json` (default 0 = unlimited; test value 1000) bounds the total AI calls a single run can dispatch. Tasks exceeding the cap fail fast, preventing a single JCWF from consuming the entire AI budget.
- **Isolated folder per run, namespaced by user.** Each submission lives in `_adhoc/<user_slug>/<timestamp>_<counter>_del-<timestamp>/` with its own `workflows/` and `queue/` subfolders. The `user_slug` is derived from the MCP key's `user` field with a strict character whitelist (`[A-Za-z0-9._@-]`, everything else collapsed to `_`, capped at 64 bytes) so the filesystem itself enforces per-tenant isolation in addition to the handler-level ownership check. The delete-at instant is encoded in the folder name so the reaper is stateless and restart-safe.
- **Retention policy.** `on_completion` wipes the folder inline the moment the run reaches a terminal state; TTL policies (`ttl_1h` / `ttl_24h` / `ttl_48h` / `ttl_72h`, default 72h) are enforced by a 60-second reaper thread; `retain` keeps the folder until admin deletion.
- **Audit trail.** Every submission logs `adhoc_submitted user=… key_id=… runId=… workflowId=… policy=…` to `log/security.txt`.
- **Docker recommended.** For Engine deployments serving untrusted adhoc JCWFs, run j9t inside a container — the filesystem isolation caps the blast radius to `~/JarvisAgent/` even if a submitted JCWF misbehaves.

### Artifact retrieval — download plane

Once a run completes, its outputs are reachable through `GET /api/workflow-runs/<runId>/files` (listing) and `GET /api/workflow-runs/<runId>/files/<path>` (download). These are gated by the same RBAC rules as the run itself:

- **Ownership check.** The caller's user slug must match the run's owner slug, or the caller must hold the `admin` role. Non-matching access returns `403 not_owner`; viewer role returns `403 insufficient_role`. The filesystem layout (`_adhoc/<user_slug>/…`) acts as a second check — even if a bug skipped the handler's ownership test, the path prefix still separates tenants.
- **Admin cross-user reads are audit-logged.** Every access to *another* user's files emits `admin_cross_user_read kind=<list|file> caller=… owner=… runId=… path=…` at INFO — durable evidence for SOC 2 / ISO 27001 audits. Denials log at WARN.
- **Path-safety is defensive.** Before touching the filesystem the download handler rejects `..` segments, absolute paths, URL-encoded traversal (`.%2E/foo`), and symlinks (regardless of where they point — no symlink-following, closes a TOCTOU class where a malicious task could swap a regular file for a symlink between listing and read). Directories, FIFOs, sockets, and devices are refused; only regular files are served. Internal bookkeeping (`meta.json`, `manifest.json`) is never served through this endpoint.
- **Response size caps.** Single-response body capped at 10 MB; oversize requests return `413 file_too_large` with a suggested `Range:` header. Range requests (`bytes=start-end`) are supported for chunked reads. Full-file responses carry `X-Content-SHA256` so clients can verify integrity against the hash in the listing response.
- **Retention echoed.** Every response (listing and download) surfaces the retention policy, `delete_at` instant, and `seconds_remaining`, so callers know how long an artifact URL will stay valid.
- **No write path.** The artifact plane is read-only. Mutation stays with the workflow runtime; there is no API path that can modify a run folder's contents after staging.

The script catalog (`GET /api/scripts`) carries no sensitive data — it reports only the `@jarvis-script` metadata from files already on disk — so it's viewer-accessible and doesn't add to the authorisation surface. It exists to let agents compose valid JCWFs without guessing script paths and eating `400 missing_scripts` rejections.

### MCP + adhoc threat surface

The MCP programmatic interface and adhoc submission add a small, well-scoped threat surface on top of the baseline Engine:

| Threat | Severity | Mitigation |
|--------|----------|------------|
| **Adhoc JCWF abuse** — submitted JCWF references scripts to exfiltrate data or abuse resources | High | No script submission through the API (caller-supplied scripts rejected); scripts must pre-exist under `scripts/`; `adhoc_enabled` opt-in off by default; per-user disk quota; per-run AI call cap; every submission audit-logged with user + key_id |
| **MCP key theft** — a leaked MCP key grants that user's role in full | High | 90-day key expiry, auto-disable on configured inactivity window, immediate revocation via the Settings > MCP Keys tab, per-key audit trail, encrypted key store (SHA-256 hashes only on disk) |
| **MCP key brute force** — attacker tries random values | Medium | MCP keys are 256-bit random — computationally infeasible to guess. The pre-auth rate limit (per-IP) caps probe rate; the failed-auth lockout (10 failures / 5 min / IP → 15-minute ban) handles persistent attackers |
| **Adhoc disk exhaustion** — malicious submissions fill disk | Medium | Per-user disk quota (default 1 GB, admin-configurable per key), TTL-based cleanup policies, `retain` requires explicit admin intent, `MaxRequestBodyMB` caps individual HTTP requests at 10 MB |
| **Privilege escalation via MCP** — viewer key used to run workflows | Low | RBAC enforced at the route level in `Authenticate()`; the role is carried on the key record itself and cross-checked on every request |
| **MCP sidecar compromise** — attacker gains control of the Node process | Medium | Sidecar holds exactly one bearer token — its own MCP key. Blast radius is limited to that key's role, quota, and adhoc_enabled flag. Revoke the key to cut off immediately |
| **Path traversal on artifact download** — agent crafts `..` / symlink / URL-encoded path | Low | Lexical normalisation + prefix check before any filesystem access; symlinks refused regardless of target; directories, FIFOs, sockets, and devices refused; `meta.json` / `manifest.json` explicitly reserved |
| **Cross-tenant artifact leak** — operator A reads operator B's run | Low | Ownership check in handler *and* per-user folder namespace (`_adhoc/<user_slug>/`) — defence in depth. Admin cross-user reads are audit-logged at INFO with `admin_cross_user_read` |

### Admin responsibility

The admin (operator) is responsible for:

- **Encrypted storage.** j9t does not encrypt data at rest. All workflow data, AI outputs, and logs (`queue/`, `workflows/`, `log/`) are plaintext on disk. **The operator must deploy j9t on encrypted storage** — full-disk encryption (LUKS, BitLocker), encrypted cloud volumes (AWS EBS encryption, Azure Disk Encryption, GCP CMEK), or encrypted Docker volumes. This is the same requirement as PostgreSQL, Elasticsearch, and other backend services that rely on infrastructure-level encryption.
- **TLS configuration.** Either enable built-in TLS (`TlsCert`/`TlsKey` in config.json → HTTPS on port 8443) or deploy behind a TLS-terminating reverse proxy. Never expose plain HTTP to the internet.
- **API gateway.** Deploy j9t behind an API gateway (Kong, AWS API Gateway, Traefik) that handles OIDC/SAML authentication and MFA. Configure `TrustedProxyHeader` and `TrustedRoleHeader` so j9t receives per-user identity and role from the gateway.
- **Private subnet.** Place j9t on a private subnet with no direct internet access. Only the API gateway should be able to reach j9t's port.
- **MCP key hygiene.** Treat each MCP key like a password. Keys are stored only as SHA-256 hashes in `mcp_keys.json.enc` (AES-256-GCM encrypted); raw keys are shown exactly once at activation and self-renewal, never reissued. Rotate via `POST /api/auth/mcp-keys/enroll` (new enrollment for the same user) or `POST /api/auth/mcp-keys/self-renew` (user-driven). Revoke immediately via the Settings > MCP Keys tab or `DELETE /api/auth/mcp-keys/<key_id>` if a key is suspected compromised.
- **Master password after restart.** j9t's key stores are encrypted with a master password held exclusively in `mlock()`-protected memory (`SecureString`). After every restart an admin must provide the password via the login page or `POST /api/settings/keys/unlock`. Until unlocked: MCP key authentication is unavailable, AI provider API calls cannot resolve credentials, OAuth token refresh is paused, and any cloud connection requiring encrypted credentials is unavailable. `GET /api/status` exposes `"keys_unlocked": bool` so monitors can alert when unlock is pending.
- **Adhoc data retention.** Adhoc workflow artifacts are automatically deleted according to the retention policy configured per MCP key (default: 72 hours). The admin is responsible for configuring retention policies appropriate to their organization's requirements and for informing users that adhoc run data is ephemeral. j9t provides no backup or recovery mechanism for cleaned-up adhoc artifacts. The `retain` policy is available for admins who need permanent data.
- **Webhook secret management.** Configure a strong, unique secret for every webhook trigger. Share secrets with integration partners over a secure channel.
- **Network segmentation.** Restrict access to the Engine port (default 8080, or 8443 with TLS) using firewall rules. Only the API gateway, webhook callers, and admin workstations should be able to reach it.
- **SIEM integration.** Forward `log/security.txt` to your organization's SIEM (Splunk, Elastic, Microsoft Sentinel, Datadog) for centralized monitoring, alerting, and compliance retention. Each event includes IP, user identity, role, endpoint, and outcome.
- **Multi-tenant isolation.** For deployments serving multiple teams or customers, run one j9t instance per tenant with separate data directories. Route traffic via the API gateway based on tenant identity.
- **Log access.** Application and security logs may contain sensitive data (prompts, IP addresses, user identities, file paths). Security log access is restricted to admin role. Consider log redaction for compliance-sensitive environments. Security log rotation is automatic (10 MB x 5 files).
- **Keeping j9t up to date.** Apply updates promptly to pick up security fixes.

### End user responsibility

End users interact with j9t **indirectly** through a frontend application (e.g. a chatbot, a web portal) that calls j9t's webhook API on their behalf. End users never see an MCP key, the dashboard, or the log viewer. Their responsibilities are:

- **Use the frontend application as intended.** Do not attempt to access j9t endpoints directly.
- **Report unexpected behavior.** If the frontend application returns errors or unexpected results, report them to the application operator — not to j9t directly.
- **Understand AI limitations.** AI-generated answers are based on the context provided by the workflow (e.g. a repair manual). They may be incomplete, outdated, or incorrect. Verify critical information through official channels.

---

## Cloud AI Backend Safety

JarvisAgent sends prompts to cloud AI providers (OpenAI, Google Gemini, etc.) via their public APIs. These providers have their own built-in safety layers:

- **Content filtering.** Providers reject or flag prompts and responses that violate their usage policies (hate speech, illegal content, personal data extraction attempts).
- **Rate limiting and abuse detection.** Providers enforce per-key rate limits and monitor for abuse patterns.
- **Data handling policies.** API data is subject to each provider's data processing agreement. Most providers do not use API inputs for model training (check your provider's terms).
- **Model guardrails.** Models are trained with safety alignment to refuse harmful instructions.

These protections apply automatically to every AI call j9t makes. However, they are controlled by the provider — j9t cannot override or extend them. The operator should:

- Review the provider's terms of service and data processing agreement.
- Use API keys with appropriate rate limits and spending caps.
- Avoid sending personally identifiable information (PII) in prompts unless the provider's terms permit it and the deployment's privacy policy allows it.

### Self-hosted AI models for confidential data

For organizations handling sensitive or classified information, the recommended approach is to run **in-house AI models on self-hosted machines** without public internet access. j9t supports any OpenAI-compatible API endpoint — simply point the AI interface URL in `config.json` to an internal server (e.g. running Ollama, vLLM, or llama.cpp).

When j9t and the AI model both run on the same private network (or the same machine), no prompt data ever leaves the organization's infrastructure. Combined with encrypted storage on the AI server, this achieves a data classification level up to **"confidential"**, provided the server-side encryption and network segmentation meet the organization's security standards.

| Deployment | Data leaves organization? | Suitable classification |
|-----------|--------------------------|------------------------|
| Cloud AI providers (OpenAI, Google, etc.) | Yes — prompts sent to external servers | Public / internal use only |
| Self-hosted AI on private network | No — all data stays in-house | Up to confidential (depending on server-side encryption and access controls) |

---

## Are Credentials Sent Unencrypted?

j9t supports **built-in TLS (HTTPS)** via the `TlsCert` and `TlsKey` fields in `config.json`. When configured, j9t serves HTTPS on port 8443 and all traffic is encrypted. Without TLS enabled, all communication between the browser/client and j9t travels over plain HTTP.

| What is transmitted | Encrypted in transit? | Risk |
|--------------------|-----------------------|------|
| MCP API key (`Authorization: Bearer mcp_...`) | **Yes** with built-in TLS or reverse proxy; **No** otherwise | An attacker on the same network can intercept the key if unencrypted |
| Session cookie (`session=...`) | **Yes** with built-in TLS (cookie carries the `Secure` flag); **No** otherwise | Without TLS the cookie is not sent with `Secure` and can be intercepted |
| Enrollment token (`POST /api/auth/mcp-keys/activate`) | **Yes** with built-in TLS or reverse proxy; **No** otherwise | The token is single-use and short-lived, but must still travel over TLS |
| Webhook HMAC signatures (`X-Webhook-Signature`) | **Yes** with built-in TLS or reverse proxy; **No** otherwise | The signature itself is not secret (it proves authenticity, not confidentiality), but the request body is visible if unencrypted |
| AI API keys (sent to OpenAI, Google, etc.) | **Yes** — j9t connects to cloud providers via HTTPS | These never travel unencrypted |
| Master password for `keys.json.enc` / `mcp_keys.json.enc` | **Yes** with built-in TLS or reverse proxy; **No** otherwise | Same network interception risk as the MCP key |
| Workflow data, prompts, AI responses | **Yes** with built-in TLS or reverse proxy; **No** otherwise | Visible to anyone who can observe network traffic if unencrypted |

**For Studio (localhost):** This is not a concern. Traffic stays on the local machine and never crosses a network.

**For Engine (production):** Enable built-in TLS by adding certificate and key paths to `config.json`:

```json
{
  "TlsCert": "/path/to/cert.pem",
  "TlsKey": "/path/to/key.pem"
}
```

j9t will serve HTTPS on port 8443. If either file is missing or only one field is set, j9t refuses to start (no silent fallback to HTTP).

Alternatively, deploy behind a **TLS-terminating reverse proxy** (nginx, Caddy, Traefik, or a cloud load balancer) that handles encryption and forwards decrypted requests to j9t over a short localhost connection.

**Bottom line:** Either enable built-in TLS or deploy behind a reverse proxy. Without TLS, MCP keys and session cookies travel in cleartext and can be intercepted.

---

## Docker as an Additional Security Layer

Docker runs j9t inside an isolated container — a lightweight virtual environment with its own filesystem, network, and process space. This adds defense-in-depth on top of the measures described above.

### What the container can and cannot access

When j9t runs in Docker, the run command (`scripts/run-docker.sh`) mounts exactly **one host directory** into the container:

```
Host:       ~/JarvisAgent/     →     Container:  /app/
```

This is the only folder on the host computer that the container can read or write. Everything else — the user's home directory, emails, documents, desktop files, SSH keys, browser profiles, other applications — is **completely invisible** to j9t inside the container.

The mounted `~/JarvisAgent/` directory contains only j9t working data:

| Folder | Contents |
|--------|----------|
| `workflows/` | Workflow definitions (`.jcwf` files) and supporting data files (e.g. repair manuals, CSV files) |
| `queue/` | Runtime task inputs and AI outputs |
| `_adhoc/` | One-shot adhoc workflow runs (auto-cleaned per TTL policy) |
| `log/` | Application logs (`log.txt`) and security audit logs (`security.txt`) |
| `config.json` | Server configuration (AI provider URLs, thread count — no credentials) |
| `keys.json.enc` | Encrypted AI provider credentials (unlocked with master password) |
| `mcp_keys.json.enc` | Encrypted MCP API keys (same master password, SHA-256 hashes only) |

The j9t binary itself, the dashboard UI, the workflow editor UI, and all scripts are baked into the Docker image at `/opt/jarvisagent/` and are **read-only**. The container cannot modify its own executable or inject code into the installation.

### Engine in Docker (production)

Running Engine in Docker provides:

- **Filesystem isolation.** The container can only access `~/JarvisAgent/`. A compromised workflow cannot read confidential files anywhere else on the host (emails, documents, credentials, other projects).
- **Process isolation.** Processes inside the container cannot see or interact with host processes. Even if an attacker gains code execution inside j9t, they are confined to the container.
- **Read-only installation.** The j9t binary, scripts, and UI assets are baked into the image. An attacker cannot modify the server binary or inject malicious scripts into the installation directory.
- **Resource limits.** Docker supports CPU and memory limits (`--memory`, `--cpus`), preventing a runaway workflow from consuming all host resources.
- **Network isolation.** The container only exposes port 8080. It cannot initiate connections to other services on the host unless explicitly configured. Combined with a reverse proxy on the host, this creates a clean network boundary.
- **Reproducible environment.** The Docker image is a known-good snapshot. Restarting the container resets everything except `~/JarvisAgent/`, making recovery from compromise straightforward.

Docker does **not** replace bearer token auth, HMAC verification, or TLS. It adds a containment layer: even if the application-level defenses fail, the blast radius is limited to the container and the `~/JarvisAgent/` data directory.

### Studio in Docker (development)

Docker provides the **same filesystem isolation** for Studio developers. The developer creates a `~/JarvisAgent/` folder with copies of the data files needed for their workflows (repair manuals, CSV data, etc.). The container can only see that folder — the rest of the developer's machine (emails, documents, source code for other projects, browser profiles) is unreachable.

This is relevant for organizations evaluating j9t as a new application:

- **What is exposed:** Only the contents of `~/JarvisAgent/` — workflow definitions and their supporting data files. These are working copies placed there intentionally by the developer.
- **What is not exposed:** Everything else on the developer's computer. The container has no access to the home directory, other project folders, the desktop, email clients, or any system files.
- **AI-generated scripts** run inside the container. If a generated script misbehaves (deletes files, writes garbage), the damage is contained to `~/JarvisAgent/`. The developer can delete the folder and start fresh.

Additional benefits for Studio in Docker:

- **Untrusted workflow testing.** Testing a workflow from an external source in Docker means it cannot access anything outside `~/JarvisAgent/`.
- **Team onboarding.** A pre-built Docker image ensures every developer has the same environment (Python version, dependencies, tools) without manual setup.
- **CI/CD.** Automated testing of workflows in Docker provides a clean, reproducible environment for each test run.

**Recommendation:** Use Docker for both Engine production deployments and Studio development when filesystem isolation matters. 

### Capability hardening (defence against ptrace / in-container escalation)

Docker's default capability set already excludes `CAP_SYS_PTRACE`, so in-container processes cannot attach to j9t and read the master-password `SecureString` buffer out of memory. The only ways to lose this guarantee are (a) running with `--privileged`, (b) `cap_add: SYS_PTRACE`, or (c) disabling AppArmor/SELinux profiles. None of those are required by j9t — if a deployment adds them, document why.

For belt-and-suspenders hardening the provided `docker-compose.example.yml` also sets `security_opt: no-new-privileges:true`, which blocks setuid/setgid escalation for any child processes j9t spawns (shell tasks, Python scripts). This turns privilege escalation attempts inside the container into hard failures rather than silent success.

---

## Direct Public Internet Exposure — Not Supported

**j9t Engine must NOT be exposed directly to the public internet.**

j9t is designed as an **execution backend** that sits behind an infrastructure layer (API gateway, reverse proxy, load balancer). It is not a public-facing web application and lacks the following protections required for direct internet exposure:

- **No WAF** (Web Application Firewall) — j9t cannot inspect or block malicious HTTP traffic patterns (SQL injection, XSS payloads, bot signatures)
- **No DDoS protection** — rate limiting handles application-level flooding but not network-level attacks (SYN floods, amplification attacks, bandwidth exhaustion)
- **No federated identity** — j9t has its own session store but no direct OIDC / SAML / LDAP integration. Enterprise SSO is delivered via the API gateway, which authenticates users and forwards identity as headers to j9t.
- **CSRF defense via `SameSite=Strict` only** — session cookies are set with `SameSite=Strict` (the browser refuses to send them on cross-site requests); MCP-key bearer auth is immune because browsers never auto-attach `Authorization` headers cross-site. There are no anti-forgery tokens beyond this.
- **No automated certificate management** — TLS certificates must be manually configured; there is no ACME / Let's Encrypt integration

**The supported deployment model is: API gateway → j9t Engine.** See the next section.

---

## Recommended Deployment Architecture

### Reference architecture

```
┌──────────┐    ┌─────────────────┐     ┌───────────────────────────────┐    ┌──────────────────┐
│ Internet │───▶│ WAF / DDoS      │───▶│ API Gateway                   │───▶│ j9t Engine       │
│          │    │ (CloudFlare,    │     │ (Kong, AWS API GW, Traefik)   │    │ (private subnet) │
│          │    │  AWS Shield)    │     │                               │    │                  │
│          │    │                 │     │ • OIDC / SAML authentication  │    │ • TLS enabled    │
│          │    │ • L3/L4 filter  │     │ • MFA enforcement             │    │ • Bearer token   │
│          │    │ • Rate limiting │     │ • Role mapping → headers      │    │ • RBAC enforced  │
│          │    │ • Bot detection │     │ • X-Forwarded-User            │    │ • Audit logging  │
│          │    │                 │     │ • X-Forwarded-Role            │    │ • Body size limit│
└──────────┘    └─────────────────┘     └───────────────────────────────┘    └──────────────────┘
```

### Step-by-step deployment

1. **Place j9t on a private subnet** with no direct internet access. Only the API gateway can reach j9t's port (8080 or 8443).

2. **Configure the API gateway** with your organization's identity provider (OIDC via Azure AD, Okta, Google Workspace; or SAML). Enable MFA at the identity provider level.

3. **Map identity to headers.** Configure the gateway to inject:
   - `X-Forwarded-User` — the authenticated user's identity (email or subject claim)
   - `X-Forwarded-Role` — the user's role: `admin`, `operator`, or `viewer`

4. **Configure j9t.** Add to `config.json`:
   ```json
   {
     "TrustedProxyHeader": "X-Forwarded-User",
     "TrustedRoleHeader": "X-Forwarded-Role",
     "TlsCert": "/etc/j9t/cert.pem",
     "TlsKey": "/etc/j9t/key.pem",
     "MaxRequestBodyMB": 10
   }
   ```

5. **MCP keys for programmatic access.** Browser users authenticate via the gateway (no MCP key needed); automation and MCP clients each get their own MCP API key, provisioned via the enrollment flow. The legacy shared bearer token has been removed — every machine credential is tied to a named identity.

6. **Forward security logs to SIEM.** Configure your log aggregator (Splunk, Elastic, Datadog) to ingest `log/security.txt`. Each event includes IP, user identity, role, endpoint, and outcome.

### Multi-tenant deployment

j9t does not have built-in tenant isolation at the data level (all workflows share one filesystem). For multi-tenant deployments:

- **Recommended: one j9t instance per tenant.** Each tenant gets its own j9t Engine container with its own `workflows/`, `queue/`, and `log/` directories. The API gateway routes requests to the correct instance based on the tenant's identity. This provides full data isolation.
- **Alternative: shared instance with role-based scoping.** A single j9t instance serves multiple teams. The gateway maps each team to a role (`admin` for the platform team, `operator` for workflow runners, `viewer` for stakeholders). All teams share the same workflow pool. This is simpler but does not isolate data between teams.

---

## Summary

| Concern | Studio | Engine |
|---------|--------|--------|
| Who should run it | Developer, on localhost | Ops team, behind API gateway on private subnet |
| Browser UI | Open (localhost) | Login required (MCP key → session cookie) |
| MCP / programmatic auth | MCP API key (same store) | MCP API key (same store) |
| Webhook auth | HMAC-SHA256 (optional secret) | HMAC-SHA256 (mandatory secret) |
| RBAC | 3 roles on MCP keys | 3 roles on MCP keys, sessions, and gateway headers |
| Rate limiting | Two-tier: pre-auth per-IP (100/min, burst 20) + authenticated per-user (1200/min, burst 200) | Same |
| Auth lockout | 10 failures / 5 min → 15-min IP lockout | Same |
| Key lifecycle | 90-day MCP key expiry, self-renew before expiry | Same |
| Adhoc submission | MCP key + adhoc_enabled | MCP key + adhoc_enabled (quota + AI cap enforced) |
| Audit logging | `log/security.txt` — MCP auth events | Full audit to `log/security.txt`, rotating, dashboard viewer |
| Security headers | CSP, X-Frame-Options, Referrer-Policy | Same + HSTS when TLS enabled |
| Request body limit | None | Configurable `MaxRequestBodyMB` (default 10 MB) |
| AI script execution | Yes (review before accept) | No (AI tooling removed at compile time) |
| AI assistant | Yes (approval required for mutations) | No (removed at compile time) |
| TLS | Optional (built-in or not needed on localhost) | Built-in (`TlsCert`/`TlsKey`) or reverse proxy |
| Encryption at rest | N/A (localhost) | **Operator must deploy on encrypted storage** (LUKS, EBS, Azure Disk) |
| Log sensitivity | Developer sees own logs | Role-protected; security log admin-only |
| Direct internet exposure | Not applicable (localhost) | **Not supported** — deploy behind API gateway |
| Cloud AI safety | Provider-managed content filtering, rate limits, and data policies apply to all AI calls |
| Cloud connections | Configurable (no audit) | Configurable + audit logged |
| Credential types | API key, OAuth, key pair, basic auth | Same, encrypted at rest |
| Secret redaction | Log output scrubbed | Log output scrubbed |

---

## Cloud Integration Security

### Credential Hierarchy

Cloud credentials are stored in the encrypted key store (`keys.json.enc`, AES-256-GCM with PBKDF2 key derivation). Four credential types are supported:

| Type | Stored fields | Use case |
|------|--------------|----------|
| `api_key` | API key / PAT | Polarion, Slack, GitHub |
| `oauth` | Access token, refresh token, expiry, scopes | OneDrive, Google |
| `key_pair` | RSA private key (PEM) | Snowflake JWT |
| `credentials` | Username + password | PostgreSQL, SMTP |

Credentials are referenced by name in connection configs — secrets never appear in JCWF workflow files.

### SecretRedactor

All cloud-related secrets (Bearer tokens, JWTs, OAuth tokens, SigV4 signatures) are registered with the `SecretRedactor` singleton on acquisition. The redactor scrubs these values from all log output, replacing them with `[REDACTED]`. This prevents accidental secret leakage in `log/security.txt` and the application log.

### Cloud Connection Security

- All cloud API calls use HTTPS via libcurl (TLS 1.2+ enforced)
- `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST` remain enabled
- Connection configurations do not contain secrets — they reference credentials by `key_name`
- Connection CRUD events are logged to the security log

### MCP Security

The MCP server is a standalone TypeScript sidecar communicating with j9t over stdio or SSE. It is a thin proxy — every MCP tool call translates into a j9t REST request authenticated with an MCP API key. Engine and Studio use the **same** MCP auth path.

**Credential handling.**
- The sidecar reads its MCP API key from the `J9T_TOKEN` environment variable or the file pointed to by `J9T_TOKEN_FILE`. The key must start with `mcp_`; anything else is rejected by j9t.
- Each MCP user has their **own** key with their own identity, role, and (if granted) adhoc-submission flag. The shared service credential pattern is deliberately unsupported — audit log entries must tie back to a real human.
- Keys are stored on disk only as SHA-256 hashes inside the encrypted `mcp_keys.json.enc` store. Raw keys are visible to the human user once at activation / self-renewal.

**Enrollment lifecycle.**
- Admin: `POST /api/auth/mcp-keys/enroll` → short-lived enrollment token (30-min default TTL) shared with the user out-of-band.
- User: `POST /api/auth/mcp-keys/activate` → real MCP key, shown exactly once.
- User self-renews before the 90-day expiry via `POST /api/auth/mcp-keys/self-renew` (old key enters a 24-hour grace period).
- Admin revokes immediately via the Settings > MCP Keys tab or `DELETE /api/auth/mcp-keys/<key_id>`.

**RBAC.**
- MCP tools resolve to specific REST endpoints. The route's role gate is authoritative — a `viewer` key calling `manage_connections` receives HTTP 403 `insufficient_role`; an `operator` key without `adhoc_enabled` calling `run_adhoc_workflow` receives HTTP 403 `adhoc_not_enabled`.
- `whoami` lets an agent confirm its own permissions before attempting a restricted operation.

**Transport.**
- **stdio** — the sidecar is spawned by a local MCP client (Claude Code, Claude Desktop). No network exposure; the bearer token never leaves the machine.
- **SSE** — when exposed over SSE, j9t must sit behind TLS (built-in or reverse proxy). Without TLS the MCP key travels in cleartext.

**Audit.** Every MCP-authenticated request produces a `mcp_auth_success` / `mcp_auth_failure` line in `log/security.txt`, tagged with the user, role, and endpoint.

### JwtGenerator Security

- Minimum 2048-bit RSA key enforcement
- Generated JWTs are auto-registered with `SecretRedactor`
- Private key material is freed via `EVP_PKEY_free()` after signing

### Snowflake JWT Authentication

Snowflake uses RSA key-pair authentication via the `JwtGenerator`:

- **No password or shared secret** — authentication is based on an RSA key pair; the private key never leaves the j9t host
- **JWT expiry** — tokens are generated with a 1-hour expiry (`exp` claim), regenerated per request by `ResolveCredentials()`
- **Public key fingerprint** — the JWT `iss` claim includes the SHA-256 fingerprint of the public key, binding the token to a specific key pair
- **Key storage** — RSA private key (PEM) is stored in the encrypted key store as a `KeyPairCredential`
- **Request header** — `X-Snowflake-Authorization-Token-Type: KEYPAIR_JWT` signals Snowflake to validate the JWT against the user's assigned public key
- **Statement cancellation** — if a workflow run is cancelled during async polling, the executor sends a cancel request to Snowflake to release server-side resources

### OAuth 2.0 with PKCE (OneDrive)

The OneDrive integration uses the OAuth 2.0 authorization code flow with PKCE (Proof Key for Code Exchange):

- **No client secret** — PKCE replaces the client secret with a per-flow `code_verifier` / `code_challenge` pair, making it safe for public/native clients
- **code_verifier** is generated from 32 bytes of `RAND_bytes()` (OpenSSL CSPRNG), base64url-encoded to 43 characters
- **code_challenge** is SHA-256 of the verifier, sent in the authorization request; Microsoft validates it during token exchange
- **Code verifiers are ephemeral** — stored in-memory only (WebServer `m_OAuthCodeVerifiers` map), never persisted to disk
- **Token refresh** — `OAuthTokenManager` runs a background thread refreshing tokens 5 minutes before expiry via `POST /oauth2/v2.0/token`. Both access and refresh tokens are registered with `SecretRedactor` on acquisition and rotated on refresh
- **Scope restriction** — default scopes are `Files.ReadWrite offline_access` (minimum for file upload/download + token refresh). Operators should not grant broader scopes than needed
- **Redirect URI** — callback is `http://localhost:{port}/api/connections/{name}/oauth/callback`, only reachable on the local machine

### Messaging Security (Slack, Email)

- **Slack Bot tokens** (`xoxb-...`) are stored in the encrypted key store and registered with `SecretRedactor`
- **Email credentials** (username + password/app password) are stored as `BasicAuthCredential` in the encrypted key store
- **SMTP TLS** — STARTTLS is enforced for port 587; implicit TLS for port 465. `CURLOPT_SSL_VERIFYPEER` remains enabled
- **IMAP TLS** — IMAPS (port 993) with certificate verification
- **Attachment handling** — attachments are read from the task working directory only; path traversal is constrained by `TaskPathResolver`
- **Email content** — message body and subject may contain workflow template variables; operators should review templates to avoid exposing sensitive data

### Additional Integrations Security (GitHub, Jira, Google Sheets)

- **GitHub PATs** are stored in the encrypted key store; `User-Agent: j9t/1.0` header is set per GitHub API requirements
- **Jira Cloud** uses BasicAuth (email + API token) — the API token is stored as `BasicAuthCredential`; Jira Data Center uses PAT as Bearer token
- **Google Sheets** supports both API key (read-only, public sheets) and OAuth2 (read/write, private sheets). API keys are sent as `?key=` query parameters (not in headers) per Google's convention
- **GitHub file content** returned by `get_file` is base64-decoded before writing to disk; content is constrained to the task working directory by `TaskPathResolver`

### Phase 9 Hardening

- **Circuit breaker** — `CloudCircuitBreaker` prevents failure cascading during cloud outages. Per-connection state machine (Closed/Open/HalfOpen) with configurable thresholds. All cloud tasks automatically benefit via `ICloudTaskExecutor`.
- **Audit logging** — all cloud task executions are logged to `log/security.txt` with task ID, connection name, connection type, and run ID
- **OAuth CSRF protection** — random 16-byte `state` token generated per OAuth flow, validated on callback before token exchange
- **Download size limits** — `CURLOPT_MAXFILESIZE_LARGE` (256 MB) set on S3 and OneDrive download operations to prevent unbounded downloads
- **Path traversal validation** — `ValidateLocalPath()` rejects `local_path` params containing `..` or resolving outside the task working directory; logged to security log
- **Cancellation propagation** — `TaskCancellationToken` shared per run, cancelled via `POST /api/workflow-runs/{runId}/cancel`, checked by long-running cloud tasks (Snowflake async polling)

### Remaining Threats (Cloud-Specific)

- **Credential exposure if encrypted key file is compromised** — mitigated by AES-256-GCM + PBKDF2 (100k iterations), but ultimately depends on master password strength
- **OAuth token theft if master password is weak** — operator responsibility to use a strong master password
- **Outbound data exfiltration via misconfigured cloud tasks** — operator should review cloud connections and restrict OAuth scopes to minimum needed

### Admin Responsibility (Cloud-Specific)

- Review and audit cloud connection configurations
- Restrict OAuth scopes to the minimum required
- Configure egress firewall rules for cloud endpoints
- Monitor cloud task execution in `log/security.txt`
