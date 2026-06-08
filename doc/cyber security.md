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

Source-package builders that ship to public registries (`packaging/Linux/Ubuntu/24_04/build-{launchpad,ppa}.sh`) assemble the tarball with `git archive HEAD` — an **allowlist of version-controlled files only**. Gitignored dev-machine secrets (`certs/*.pem` private keys, `.mcp_admin_token`, `keys.json.enc`, `mcp_keys.json.enc`, `API.json.enc`, `connections.json.enc`) and runtime data (`queue/`, `workflows/`, `log/`) are absent by construction, so they cannot leak into a published source package. The pre-built React `dist/` and the `premake-core` source are the only untracked inputs, re-injected explicitly.

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
- **HMAC-SHA256 webhook authentication.** Webhook triggers require a per-workflow secret in **both** editions.  The caller must include an `X-Webhook-Signature: sha256=<hex>` header computed over the raw request body; signature verification uses constant-time comparison.  Defense in depth across two layers: (1) `WorkflowTriggerBinder::ParseWebhookParams` fails closed at JCWF parse time on every failure mode (empty params, parse error, non-object root, missing/empty `secret` field) — the trigger is never registered with an empty secret, with a specific ERROR log naming the cause; (2) the TriggerEngine validator additionally refuses webhook triggers with empty secrets at the registration call.  Studio-mode operators authoring a webhook trigger without a secret get the parse-time error immediately; runtime requests against a registered webhook all enforce the HMAC.
- **WebSocket authentication.** Browser WebSocket upgrades carry the session cookie automatically; Crow's `.onaccept` hook calls `Authenticate()` at handshake time and rejects upgrades that do not produce a valid auth result. No in-band `type:"auth"` message is used — there is no post-upgrade auth window to exploit.
- **Per-connection role pinning on `/ws`.** The role validated at upgrade time is pinned to the connection (stored in `m_WsClientRoles` keyed by the connection pointer). Message-type branches that mutate disk state — currently `ai-write-scripts`, which writes files under `scripts/` and chmod-s `+x` on `.sh` files — re-check the pinned role and reject non-admin callers with `LOG_SECURITY_WARN("[security] ai_write_scripts_role_denied role='…' ip=…")` and a typed JSON error reply. Without this gate, an operator/viewer holding any valid `/ws` upgrade could plant scripts that subsequent admin-triggered workflow runs would execute.
- **Path confinement on static-asset routes.** `GET /dash-assets/<path>` and the workflow editor's `GET /assets/<path>` / `GET /editor/assets/<path>` now resolve the candidate path under the dist root via `weakly_canonical(root / raw)` + `lexically_relative(root)` containment. `..`-traversal returns `400 Bad Request` and logs `[security] dashboard_static_path_escape len=…` / `editor_static_path_escape`. The same helper (`WebServerHelpers::ConfinePathUnder`) gates `ReadLogFile` against `<launchCwd>/log/` so any future caller that lets user input influence the log path is bounded structurally, not just by the hardcoded literals at today's call sites.
- **Caller-supplied `runId` validated on integration endpoints.** `POST /api/integrations/n8n/start` and `POST /api/webhook/<id>` accept an optional `runId` in the JSON body; both now validate it against the same `IsValidWorkflowId` allowlist (`[A-Za-z0-9_-]`) before using it as a path segment in the persisted-request directory. Pre-sitting-6, `runId = "../../foo"` could escape the run folder.
- **Heartbeat requires MCP key.** `POST /api/mcp/heartbeat` requires a valid MCP key, applies the pre-auth rate limiter on the source IP, and caps body size at 1 KB before doing any work. Pre-sitting-6 the endpoint was unauthenticated, so any network attacker could pin `IsMcpConnected()` to `true` and suppress dashboard staleness alerts.
- **Master-password unlock is rate-limited and records auth failures.** `POST /api/settings/keys/unlock` is intentionally pre-auth (the master password IS the credential), but each wrong-password response now records an auth failure (`RecordAuthFailure(req.remote_ip_address)` + `LOG_SECURITY_WARN("[security] keys_unlock_wrong_password ip=…")`) so brute-force attempts hit the standard 10-attempts-per-5-minute lockout. The pre-auth rate limit also bounds attack velocity even before the lockout triggers.
- **AI-routing and cloud-connection config are encrypted at rest, not plaintext.** The AI-interface table (each interface's `url` + `key_name`, plus the default / JCWF selectors) lives in `API.json.enc`, and cloud-connection definitions (endpoint URLs + credential references) live in `connections.json.enc` — both AES-256-GCM + PBKDF2, unlocked with the same master password, loaded at unlock and hydrated into the live dispatch state. They are **no longer in plaintext `config.json` / `connections.json`**. This closes a credential-exfiltration / reroute vector: previously, anyone who could edit the plaintext config (a stolen disk, or an attacker at an unattended-but-running machine) could repoint an interface's `url` while keeping its `key_name` so the next dispatch shipped the real key to the attacker's endpoint, or flip the default interface — all without the master password. The encrypted store closes the on-disk edit; the re-auth gate below closes the running-session edit.
- **Routing/connection mutations require a second-factor master-password re-auth.** Every AI-interface and cloud-connection mutation (create / update / delete / default-change) requires the master password to be **re-entered in that request** (`WebServer::CheckMasterPasswordReauth`), on top of the admin session — a `CRYPTO_memcmp` constant-time compare against the held password, `OPENSSL_cleanse`'d after use, with mismatches recorded through the same `RecordAuthFailure` lockout path as `/api/settings/keys/unlock`. This closes the "admin left an unlocked dashboard open" vector: an already-authenticated admin tab can no longer change *where credentials are sent* without proving knowledge of the master password again. The proof is never cached (per-mutation). Read-only list/get endpoints are unaffected.
- **OAuth callback explicitly verifies TLS peer + hostname.** `HandleOAuthCallbackGet` sets `CURLOPT_SSL_VERIFYPEER=1L` and `CURLOPT_SSL_VERIFYHOST=2L` explicitly, rather than relying on libcurl's defaults — defends against a future build that silently weakens the verification posture.
- **Two-tier rate limiting.** A token-bucket policy split by authentication state, so legitimate clients are not throttled together with unauthenticated probers:
  - **Pre-auth bucket (per-IP):** 100 req/min, burst 20. Applies when the request has no valid credential (missing/unrecognised header, or `mcp_`-prefixed token that fails validation). Defends against credential-stuffing and anonymous flooding. Combined with the failed-auth lockout (above), repeat offenders graduate from 429 → 15-minute ban. Logged as `rate_limited_preauth ip=…`.
  - **Authenticated bucket (per-user):** 1200 req/min, burst 200. Applies once an MCP key or session has validated, keyed by the credential's user. Sized to comfortably absorb dashboard polling, MCP heartbeats, and bursty automation; runaway authenticated traffic surfaces in the audit log (with the user's identity attached) for investigation rather than being blanket-blocked at the auth layer. Logged as `rate_limited_authenticated user=…`.

  Both tiers respond with HTTP 429 and a `Retry-After` header. Buckets idle for 10 minutes are evicted; the cleanup runs in the same lock as the bucket consume, so housekeeping never blocks the request path independently.
- **Security audit logging.** All auth-related events are logged to a dedicated rotating log file (`log/security.txt`, 10 MB x 5 files) as well as the application log (TUI/console). Logged events include: auth success/failure with IP and endpoint, rate limit triggers, lockout triggers, webhook accept/reject with workflow ID, shutdown requests, and run control actions (cancel/pause/resume/stop) with run ID. The security log is accessible via `GET /api/log/security` (admin-auth required) and visible in the dashboard Log Viewer's "Security" tab with 3-second polling. Log macros: `LOG_SECURITY_INFO` / `LOG_SECURITY_WARN`.
- **Built-in TLS (HTTPS).** Optional native TLS via Crow's SSL support. Set `"TlsCert"` and `"TlsKey"` in `config.json` to point to PEM certificate and key files. When configured, j9t serves HTTPS on port 8443 instead of HTTP on 8080. If both fields are set but the files don't exist, j9t generates a self-signed `localhost` certificate at those paths on startup (so a fresh install serves HTTPS without manual setup); if only one field is set, it refuses to start (no silent fallback to plaintext). `GET /api/status` includes `"tls": true/false`. This eliminates the cleartext last-mile between a reverse proxy and j9t, and can replace the reverse proxy entirely for simpler deployments.
- **Gateway-trusted identity headers.** When deployed behind an API gateway (Kong, AWS API Gateway, Traefik, nginx), j9t trusts identity headers injected by the gateway. Configure `"TrustedProxyHeader": "X-Forwarded-User"` and `"TrustedRoleHeader": "X-Forwarded-Role"` in `config.json`. The gateway handles authentication (OIDC, MFA, SSO) and j9t reads the authenticated user and role from the headers. This allows per-user identity in audit logs without j9t implementing its own identity provider integration. **Important:** the headers are a *cross-check* on top of the credential — they may downgrade the credential's role to a lower one but never authenticate alone, and never escalate. If `TrustedRoleHeader` is configured but the header is absent on a request, the credential's role passes through unchanged (no implicit default-deny). The credential remains the source of truth.
- **Role-Based Access Control (RBAC).** Three roles with descending privilege: **admin** (full access including shutdown, security logs, and MCP key CRUD), **operator** (run control, workflow monitoring, adhoc submission when enabled, application logs), **viewer** (read-only dashboard, workflow list, run status). The role is carried by the MCP key itself (set at enrollment), by the session cookie derived from it, or by the `X-Forwarded-Role` header injected by an API gateway (default `viewer` if absent). Routes enforce minimum required role — a viewer attempting to stop a run, access security logs, or create an MCP enrollment receives HTTP 403 `insufficient_role`.
- **Request body size limit.** Configurable maximum HTTP body size (`"MaxRequestBodyMB": 10` in `config.json`, default 10 MB). Oversized requests are rejected with HTTP 413 `payload_too_large` before parsing. Protects against memory exhaustion attacks via large webhook payloads.
- **Security response headers.** All HTTP responses include: `Content-Security-Policy` (restricts script/style/connection sources to `'self'`), `X-Frame-Options: DENY` (prevents clickjacking), `X-Content-Type-Options: nosniff`, `Referrer-Policy: strict-origin-when-cross-origin`, `Permissions-Policy: camera=(), microphone=(), geolocation=()`. When TLS is enabled, `Strict-Transport-Security` (HSTS) is also set.
- **Reduced attack surface.** Studio-only modules (workflow editor, AI assistant, AI JCWF generation, workflow CRUD, validation) are excluded at compile time. The Engine binary is physically smaller and exposes fewer endpoints. The `removefiles {...}` directives in `premake5.lua` are symmetric: the Engine branch (`_OPTIONS["engine"]`) drops Studio-only source files (`code/backend/application/assistant/**`, `aiJcwfService.*`, `webServer_studio.cpp`) from the engine's compile graph; the Studio branch (else) drops the Engine-only stub file (`webServer_engine.cpp` — empty no-op impls for edition-routed WebServer methods).  Each side drops its counterpart's edition file, so a stray definition can never link into the wrong binary.  Verified clean separation (2026-04-25): on a `premake5 clean` slate, `nm bin/Debug/jarvisAgent-engine | grep -c "AssistantController\|AiJcwfService"` returns **0** while the studio counterpart returns **>600**; the engine `bin-int/engine/Debug/` objdir contains no `aiJcwfService.o`, `assistant*.o`, or `webServerStudio.o`. Re-run that two-line check whenever the edition gating in `premake5.lua` is touched, and after any change to which sources are studio-only.
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
- **Log data sensitivity.** `GET /api/log` returns application logs that may contain prompt content, AI responses, file paths, and error traces. `GET /api/log/security` exposes IP addresses, user identities, and auth event history. Both are role-protected (operator+ for app log, admin for security log).  Registered credential values are scrubbed from log output by the `SecretRedactor` (every spdlog sink runs through a wrapping `RedactingFormatter` — see "SecretRedactor" below).  Non-credential prompt / response content, file paths, and operator-supplied configuration values are NOT redacted.
- **Unauthenticated shutdown via process signal.** The bearer token protects the `POST /api/shutdown` endpoint, but an attacker with OS-level access can still kill the process via signals (SIGTERM, SIGKILL). This is outside j9t's control.
- **Denial of service.** Rate limiting, auth lockout, and request body size limits mitigate application-level attacks, but do not protect against network-level attacks (SYN floods, bandwidth exhaustion). Use a WAF or cloud-level DDoS protection for internet-facing deployments.

### Adhoc workflow submission

`POST /api/workflows/run-adhoc` lets an MCP client stage and execute a JCWF that was *not* pre-registered. Because the JCWF is caller-supplied, the feature is deliberately locked down:

- **No script submission.** The adhoc payload is a canvas JSON only; any `shell` / `python` task must reference a script that already exists under `scripts/` (created by admins, the editor's Generate button, or the AI assistant). Executable code cannot be injected through this endpoint. The runtime additionally enforces this gate at task-execution time — `PythonEngine::ExecuteWorkflowTaskOnWorker` rejects `params.module` names that do not resolve via `ScriptRegistry::FindByModulePath`, so system modules (`os`, `subprocess`, `ctypes`) cannot be imported even if a hostile JCWF makes it past the submission-time check (e.g. a workflow registered through the admin `PUT /api/workflows/<id>` path).
- **Path confinement on every external filesystem string.** The shared `code/backend/application/file/pathConfinement.h` helper `ConfineUnderProjectRoot(path)` is the canonical gate for any user-influenced string before it touches `fs::remove*`, Python `sys.path`, or any other filesystem-touching API. Fail-closed (returns empty path on rejection); rejects `..` escapes after canonicalisation, absolute paths landing outside the project root, and symlink targets that point out of tree (closes the symlink-attack vector). The full use-site inventory lives in `code/backend/application/file/README.md`'s pathConfinement section — Python sandboxing surface, workflow-runtime cleanup, file-watch trigger normalization (registration AND event-side), AI-task output write/read pipeline (insert/lookup/cancel-key on the same canonical form), and JCWF write/delete in `WorkflowRegistry`.
- **Opt-in per key.** `adhoc_enabled` on the MCP key defaults to `false`. Admin must enable it explicitly at enrollment time.
- **Role gate.** Requires at least `operator` role — viewers cannot submit.
- **Per-user disk quota.** Each MCP key carries `disk_quota_mb` (default 1 GB). The `AdhocWorkflowManager` tracks cumulative usage across the user's active adhoc folders; submissions past the quota return HTTP 413 `quota_exceeded`.
- **Per-run AI call cap.** `max_ai_calls_per_jcwf` in `config.json` (default 0 = unlimited; test value 1000) bounds the total AI calls a single run can dispatch. Tasks exceeding the cap fail fast, preventing a single JCWF from consuming the entire AI budget.
- **Per-item fan-out cap.** `max_per_item_fan_out` in `config.json` (default 10000; 0 = unlimited) bounds the number of children a single per-item filter evaluation can spawn. A filter that returns millions of rows (malicious CSV, runaway Polarion query) would otherwise create one task child + downstream dispatch per item, exhausting threads, memory, and the AI provider quota. `WorkflowRuntimeManager::FanOutPerItemChildren` enforces the cap up-front and fails the parent task with an ERROR-logged `runId`/`workflowId`/`taskId` if exceeded.
- **Completion-callback SSRF gate.** When a workflow's run context contains `callbackUrl`, `WorkflowRuntimeManager::FireCompletionCallback` posts the completion payload to it on a detached thread. The URL is partially attacker-influenced (a JCWF or webhook trigger payload can seed context), so the call is gated by `IsCallbackUrlAllowed`:
  - **Scheme allowlist** — `https://` only (plain `http` leaks the payload over the wire and removes peer-cert verification).
  - **DNS-resolution gate** — `getaddrinfo` resolves the host; the call is refused if **any** returned address falls in loopback (127/8, ::1), RFC 1918 (10/8, 172.16/12, 192.168/16), CGNAT (100.64/10), link-local (169.254/16, fe80::/10), unique-local (fc00::/7), multicast (224/4, ff00::/8), or unspecified (0/8, ::). Closes the cloud-metadata-endpoint vector explicitly (`169.254.169.254`).
  - **IPv4-mapped IPv6 unwrap** — `::ffff:0:0/96` addresses re-check the embedded v4 against the same allowlist; an attacker cannot smuggle `::ffff:127.0.0.1` past the v6 gate.
  - **TLS hardening** — `CURLOPT_SSL_VERIFYPEER`, `CURLOPT_SSL_VERIFYHOST`, `CURLOPT_FOLLOWLOCATION 0`, `CURLOPT_PROTOCOLS_STR`/`CURLOPT_REDIR_PROTOCOLS_STR = "https"`. A redirect to `http://` or to an internal IP cannot silently downgrade the request.
  - Rejections emit `[error] [callback] refused completion callback for run '...' to '...': <reason>` so the dashboard's run analyser surfaces them as issues against the run.
- **Isolated folder per run, namespaced by user.** Each submission lives in `_adhoc/<user_slug>/<timestamp>_<counter>_del-<timestamp>/` with its own `workflows/` and `queue/` subfolders. The `user_slug` is `<body>_<8 hex chars of SHA-256(original_user)>` where the body is a character-collapsed view of the MCP key's `user` field (`[A-Za-z0-9._@-]`, everything else collapsed to `_`, body capped at 55 bytes; total slug ≤ 64). The collapse stage keeps slugs human-readable; the hash suffix guarantees distinct users get distinct filesystem dirs even when their names collapse identically (e.g. `bob+admin@x.com` and `bob_admin@x.com`). The filesystem layout thus enforces per-tenant isolation as a second-layer defence on top of the handler's identity check. The delete-at instant is encoded in the folder name so the reaper is stateless and restart-safe.
- **Retention policy.** `on_completion` wipes the folder inline the moment the run reaches a terminal state; TTL policies (`ttl_1h` / `ttl_24h` / `ttl_48h` / `ttl_72h`, default 72h) are enforced by a 60-second reaper thread; `retain` keeps the folder until admin deletion.
- **Audit trail.** Every submission logs `adhoc_submitted user=… key_id=… runId=… workflowId=… policy=…` to `log/security.txt`.
- **Error messages omit internal install paths.** Adhoc-submission failures (`jcwfContainer::Extract` / `Pack` / `ReadFile`, `WorkflowRegistry::SaveOrUpdateWorkflowFromJson`'s extracted-dir + canvas + global.json writes, `polarionClient::WriteItemFile`'s containment + open + write failures) surface only the basename or a generic message to the API consumer; the full absolute path stays in the operator-side `LOG_APP_ERROR` line in `log/log.txt`. Closes the install-path-disclosure class for the eventual SaaS / multi-tenant deployment where API callers are not the operator running j9t.
- **`.jcwf` container extraction is Zip-Slip / symlink / zip-bomb hardened.** A `.jcwf` is a zip; a shared one is extracted on reload, so `JcwfContainer::Extract` (`code/backend/application/workflow/jcwfContainer.cpp`) is hardened against a hostile archive. A no-I/O validation pass rejects the whole archive on the first bad entry — `..`/absolute/backslash-traversal names, Unix symlink entries, encrypted entries, and zip-bomb caps (per-entry ≤128 MiB, total ≤512 MiB, ≤8192 entries) — before any byte is written. The write pass then defends the symlink-race TOCTOU a concurrent process could open between validation and write: parent directories are created one component at a time refusing a symlink ancestor, the destination is rejected if it is a symlink, each entry is written via `EngineCore::AtomicWriteFile` (tmp-file + `rename`, which replaces a symlink at the destination rather than following it), and a post-write `canonical` containment check fails the whole extraction closed if anything still resolved out of the tree. `ReadFile` validates the requested entry name and bounds the entry size before `extract_to_heap` allocates. E2e coverage: `test/security/test_jcwf_zip_slip.py`.
- **Docker recommended.** For Engine deployments serving untrusted adhoc JCWFs, run j9t inside a container — the filesystem isolation caps the blast radius to `~/JarvisAgent/` even if a submitted JCWF misbehaves.

### Artifact retrieval — download plane

Once a run completes, its outputs are reachable through `GET /api/workflow-runs/<runId>/files` (listing) and `GET /api/workflow-runs/<runId>/files/<path>` (download). These are gated by the same RBAC rules as the run itself:

- **Ownership check.** The caller's user identity (`auth.m_User`, lifted from the MCP key record) must match the run's owner identity (`meta.json::user`), or the caller must hold the `admin` role. Non-matching access returns `403 not_owner`; viewer role returns `403 insufficient_role`. Authorisation is on the original user string, never on a derived slug — a slug-only comparison would re-introduce collision risk if two distinct users sanitise to the same body. The filesystem layout (`_adhoc/<user_slug>/…`) acts as a second-layer defence: even if a bug skipped the handler's ownership test, the hash-suffixed slug path still separates tenants.
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
| **Cross-tenant artifact leak** — operator A reads operator B's run | Low | Identity-based ownership check in handler (`auth.m_User == info->m_User`) *and* hash-suffixed per-user folder namespace (`_adhoc/<body>_<8hex>/`) — defence in depth, collision-free even for distinct users whose names sanitise to the same body. Admin cross-user reads are audit-logged at INFO with `admin_cross_user_read` |

### Empirical verification — D1 negative-path tests

`test/hardening/test_negative_paths.py` (REST-driven) and `test/config/test_malformed_configs.py` (subprocess sandbox-spawn for ConfigParser) cover the rejection branches above: adhoc size cap, JCWF path-confinement on cntx / registry id / file_watch, WorkflowRegistry mutex stress, ai_calls_inflight leak, db_query row / timeout / output-path caps, and ConfigParser malformed-input handling.  Re-run after touching `adhocWorkflowManager.cpp`, `triggerEngine.cpp`, `dbQueryCloudTaskExecutor.cpp`, `pathConfinement.h`, or `configParser.cpp`.

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
- **Log access.** Application and security logs may contain sensitive data (prompts, IP addresses, user identities, file paths). Security log access is restricted to admin role.  Credential values registered with the `SecretRedactor` are auto-scrubbed before write; for non-credential sensitive content (prompts, customer data) consider an additional log-pipeline redaction layer driven by your compliance regime.  Security log rotation is automatic (10 MB x 5 files).
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
- **No CA-issued certificate management** — j9t auto-generates a self-signed `localhost` certificate when TLS is configured without existing cert files, but there is no ACME / Let's Encrypt integration; supply your own CA-issued certificate (or a TLS-terminating proxy) for publicly-trusted HTTPS

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
| Webhook auth | HMAC-SHA256 (mandatory secret — fail-closed at parse time + registration time) | HMAC-SHA256 (mandatory secret — same gates) |
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

The `SecretRedactor` is a value-based (exact-substring match) singleton that scrubs registered secret values from log output, replacing them with `[REDACTED]`.  It is wired into spdlog as a custom `RedactingFormatter` wrapping each sink's `pattern_formatter`, so the scrub runs **before** any sink writes — `log/log.txt`, the rotating `log/security.txt`, and the ncurses TUI all receive redacted text.

By construction the redactor holds each registered secret as a plaintext `std::string` in its in-memory value pool — exact-substring matching requires the cleartext.  This is a bounded, intentional in-memory plaintext copy that sits **outside** the SecureString-only boundary (analogous to libcurl's `strdup` floor): the secret is already in `SecureString` everywhere else, and the redactor pool is never serialised to disk.  A consequence for tooling: the heap-scan audit cannot be pointed at any path that registers a secret with the redactor (e.g. `OAuthTokenManager::StoreTokens`) — the nonce would always be found in the redactor pool as an accepted residual rather than a leak; verify those paths live (confirm no plaintext reaches the logs) instead.

**Coverage at registration time** (every credential value is added via `SecretRedactor::Get().AddSecret(...)` at the point it enters memory).  AI-provider credential registration is handled by **per-type virtual `RegisterSecrets()` methods** on the `ICredential` hierarchy (`code/backend/engine/keys/credential.{h,cpp}`); KeyManager calls `cred->RegisterSecrets()` whenever a credential is loaded, added, or updated.  Each subtype lists its own SecureString fields:

| Credential subtype | Registered fields | Notes |
|---|---|---|
| `ApiKeyCredential::RegisterSecrets()` | `m_ApiKey` | Bearer / x-api-key / x-goog-api-key / api-key |
| `OAuthCredential::RegisterSecrets()` | `m_AccessToken`, `m_RefreshToken`, `m_ClientSecret` | `m_ClientId`, `m_TokenEndpoint`, `m_Scopes` are non-secret |
| `KeyPairCredential::RegisterSecrets()` | `m_PrivateKeyPem` | RSA / EC private key in PEM form |
| `BasicAuthCredential::RegisterSecrets()` | `m_Password` | `m_Username` is non-secret (logged for audit) |
| `AwsCredential::RegisterSecrets()` | `m_SecretAccessKey`, `m_SessionToken` | `m_AccessKeyId` intentionally NOT registered — public per AWS conventions (CloudTrail logs it) |

Other credential registration sites (outside the typed hierarchy):

| Credential | Registration site |
|------------|-------------------|
| OAuth tokens (re-registered after refresh) | `OAuthTokenManager::ApplyRefreshResult` (wiring sandwich: RemoveSecret old → Set new → AddSecret new) |
| Generated JWTs (Snowflake, GCS) | `JwtGenerator` after sign |
| MCP enrollment tokens (`enroll_…`) | `McpKeyManager::CreateEnrollment` |
| MCP API keys (`mcp_…`) | `McpKeyManager::ActivateEnrollment`, `CreateBootstrapAdminKey`, `SelfRenew`, `Authenticate` (success-only) |
| Webhook HMAC shared secrets | `TriggerEngine::AddWebhookTrigger` |
| Azure Storage Shared Keys (`CloudCredentials::m_SecretKey`, now `SecureString`) | `AzureBlobConnector::ResolveCredentials` registers `m_SecretKey.Get()` (defense-in-depth — KeyManager already registered the underlying SecureString) |

`KeyManager` calls `cred->RegisterSecrets()` from every mutation path: keystore load (`ParseProvidersJson`), REST `POST /api/settings/providers` (`AddCredential`), REST `PUT /api/settings/providers/<name>` (`ModifyCredential`), and the OAuth callback (`UpsertCredential`).  Adding a new credential subtype requires extending the hierarchy AND providing a `RegisterSecrets()` override — the virtual call site is the single point that drives redactor wiring.

For MCP raw keys, registration happens at the four creation sites and at successful `Authenticate` only — failed authentication attempts (wrong hash / unknown keyId) take the early-return paths without registering, so an attacker spamming guesses cannot pollute the redactor's value pool with attacker-chosen strings.

**AWS access_key_id is intentionally not registered** — it is treated as public per AWS conventions (CloudTrail logs it for audit).  The dual-secret material (`secret_access_key` + `session_token`) is what gets scrubbed.

**Design boundaries:**
- The redactor is exact-substring match, not regex / prefix.  There is no auto-detection of `sk-…`, `xai-…`, or AWS key shapes.  A new credential shape requires an explicit `AddSecret` call at its entry point.
- Minimum registered secret length is 8 bytes (`MIN_SECRET_LENGTH`).  Shorter values are rejected to limit false-positive collisions: a 4-byte value that happens to be a common dictionary word ("test", "demo", "1234") collides aggressively with unrelated log text — every appearance of that substring across all log lines gets redacted.  Real secrets (API keys, JWTs, HMAC shared secrets, RSA PEMs, `mcp_…` / `enroll_…` bearers) are always ≥8 bytes, so the floor sacrifices no real coverage.  A legitimate 4–7 byte secret would not redact; today no such value exists in the project.  **Rejections now emit a `LOG_CORE_WARN` line naming the length (not the value) so a developer who registers a too-short secret sees the silent skip in the engine log rather than discovering it post-incident in a leaked log line.**
- `SecretRedactor::HasSecrets()` is lock-free (atomic-bool mirror of `!m_Secrets.empty()`), so the per-formatted-log-line check from `RedactingFormatter::format()` doesn't acquire the redactor mutex on the common no-secrets path.  Mutation paths (AddSecret / RemoveSecret) update both the vector under the mutex AND the atomic with `release` ordering.  Race window between an in-flight `format()` and a concurrent `AddSecret` is bounded to a single log line.
- The redactor does **not** scrub prompt content, AI responses, file paths, or operator-supplied configuration unless those values are also registered as secrets.

### Cloud Connection Security

- All cloud API calls use HTTPS via libcurl (TLS 1.2+ enforced)
- `CURLOPT_SSL_VERIFYPEER = 1L` and `CURLOPT_SSL_VERIFYHOST = 2L` are set **explicitly** across the entire cloud surface — not relying on libcurl's build-time defaults that can fail-open on builds where the trust store is empty.  Coverage:
  - **Executor data paths** — every HTTP-based executor (azureBlob, gcs, gitHub, googleSheets, jira, oneDrive, redmine, s3, slack, snowflake) sets TLS verify explicitly per request.  The 10 whose vendor APIs respond directly route through `ConnectorHttp::ApplyHardenedDefaults()` (TLS verify + `CURLOPT_FOLLOWLOCATION = 0L` + `CAINFO` + DNS post-resolve check on https) — a 30x is treated as hostile, preventing redirect-amplified bearer-token leak.  The 4 sites that legitimately 30x (S3 cross-region, Microsoft Graph CDN download / large-file upload session pivots) route through `ConnectorHttp::ApplyExecutorRedirectDefaults()` (same TLS verify + post-resolve check + `CURLOPT_FOLLOWLOCATION = 1L` + `CURLOPT_REDIR_PROTOCOLS_STR = "https"` to refuse http-downgrade redirect targets + `CURLOPT_MAXREDIRS = 10L` to cap follow depth).  `emailCloudTaskExecutor` uses libcurl SMTP/IMAP transport with its own `use_ssl`-gated dance — see "Messaging Security" below.
  - **Connector `TestConnection` paths** — every HTTP-based connector (azureBlob, gcs, gitHub, googleSheets, jira, oneDrive, polarion, redmine, s3, slack, snowflake) routes through `ConnectorHttp::ApplyHardenedDefaults()`.  PolarionClient's 4 `Http*` data paths share the same helper.  `emailConnector` uses libcurl's SMTP / IMAP transport rather than HTTP and has its own `use_ssl`-gated TLS-verify dance for STARTTLS / implicit-TLS — see "Messaging Security" below.
- **Connector-layer SSRF gate (two layers)** — every connector that accepts a user-supplied endpoint URL (jira, redmine, polarion required; the rest support endpoint override) is gated at two complementary points:
  - **Syntactic gate** — `ConnectorHttp::ValidatePublicHttpEndpoint()` runs before any network I/O.  Rules: scheme is http or https only; host charset is conservative (alphanumeric + `.` + `-`); host is rejected if it's an IP literal in RFC 1918 / loopback / link-local / cloud-metadata ranges via `ConnectorHttp::IsLocalNetworkHost()`.  The IPv6 unique-local / link-local prefix check is gated on a structural IPv6-literal classifier (hex digits + colons), so public hostnames starting with `fc` / `fd` / `fe80` aren't false-positive flagged.  Plain `http://` permits local-network hosts as a dev-mode opt-out (mirrors email's `allowLocal = !useSsl` heuristic).  Rejections emit `[security] <type>_endpoint_rejected` lines.
  - **DNS-resolution-time gate** — `ApplyHardenedDefaults()` and `ApplyExecutorRedirectDefaults()` install a `CURLOPT_OPENSOCKETFUNCTION` callback when the URL scheme is `https://`.  The callback fires after libcurl resolves DNS but before TCP connect; it stringifies the resolved IP via `inet_ntop` and runs `IsLocalNetworkHost()` on it.  If the resolved IP is local-network, the callback returns `CURL_SOCKET_BAD` and the request fails.  Closes the SSRF vector where an attacker-controlled public DNS name (`evil.example.com`) resolves to an internal IP — the syntactic gate alone can't catch this.  Rejections emit `[security] dns_resolved_ip_local_network_rejected resolved_ip='...'` with the actual resolved IP for forensic analysis.
- **Postgres TLS posture** — `PostgresConnector::IsValidSslMode()` allowlists libpq's six `sslmode` values (`disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full`) and rejects the three plaintext-fallback modes (`disable` / `allow` / `prefer`) for non-localhost hosts.  Default sslmode is `require` (libpq's own default `prefer` silently falls back to plaintext if the server doesn't accept TLS — MITM-vulnerable).  For local-network hosts (loopback / RFC 1918 / link-local), all 6 modes are accepted as a dev opt-out.  Both `PostgresConnector::TestConnection` and `DbQueryCloudTaskExecutor::ExecuteCloud` gate on `IsValidSslMode` before reaching libpq's connect; rejections emit `[security] postgres_invalid_sslmode connection='{}' host='{}' sslmode='{}'`.  Bracketed IPv6 literals (`[fc00::1]:5432`) are bracket-stripped in `ParseHostPort` so the local-net detection works correctly.
- **Postgres forbidden libpq params (preventive tripwire)** — `PostgresConnector::ValidatePostgresParams()` rejects any `m_Params` key that resolves to a local file path or external file lookup: `sslcert`, `sslkey`, `sslrootcert`, `sslcrl`, `sslcrldir`, `sslpassword`, `service`, `passfile`.  Currently no code path surfaces these (`BuildConnectionString` only forwards `database` and `sslmode`), but the gate is in place so a future PR that adds `paramOrDefault("sslcert", ...)` without first adding `ValidateLocalPath` confinement on the cert paths is caught.  Both gate sites (`TestConnection` and `DbQueryCloudTaskExecutor`) call this before `BuildConnectionString`; rejections emit `[security] postgres_forbidden_param`.
- **Live counters for every gate** — every cloud-surface security gate has both (a) a security log line for forensics and (b) an atomic lifetime counter on `/api/debug/signals` (DEBUG-builds, admin-gated) for live operator monitoring of "is this gate firing at all?".  Six counters cover the cloud surface:
  - `cloud_dns_resolved_ip_rejections` — DNS-resolution-time gate (the `OpensocketStrictCallback` returning `CURL_SOCKET_BAD`).
  - `cloud_endpoint_ssrf_rejections` — syntactic SSRF gate (`ValidatePublicHttpEndpoint` rejecting URL-side input).
  - `cloud_credential_crlf_rejections` — bearer/PAT/JWT/API-key CRLF check at every connector + executor splice site.
  - `cloud_input_validation_rejections` — bucket / blob_name / spreadsheet_id / range / remote_path / handle / folder validators across the executors.
  - `cloud_postgres_invalid_sslmode_rejections` — postgres sslmode allowlist + non-localhost production posture.
  - `cloud_postgres_forbidden_param_rejections` — postgres libpq cert/key/file-path tripwire.
  Counters are atomic, lock-free, monotonically increasing — they reset to 0 on server restart.  Per-instance forensic detail (timestamp, task/run/connection identifiers, actual rejected value) stays in the security log; the counters answer the global "are we seeing attempts at all?" question without grepping `log/log.txt`.  Counters increment per socket family on multi-record DNS responses (a host with both A and AAAA records pointing to local IPs counts as 2 DNS rejections).
- **Bearer / PAT CRLF gate at the connector layer** — every `TestConnection` rejects a credential containing `\r` or `\n` via `ICloudTaskExecutor::ContainsCrlf()` before splicing it into an `Authorization` header.  libcurl's recent versions strip embedded newlines but the behaviour is version-dependent; the check is a defensive pre-condition.
- Connection configurations do not contain secrets — they reference credentials by `key_name`
- Connection CRUD events are logged to the security log

### AI Interface URL Policy

The AI-dispatch URL (`api_interfaces[].url`, stored in the encrypted `API.json.enc`) follows its own scheme + address policy distinct from the cloud-connector SSRF gates above.  The reference implementation is `AIAssistant::UrlPolicy::ValidateAiInterfaceUrl` in `code/backend/application/network/urlPolicy.{h,cpp}`; hooks live at store load + persist (`ApiInterfaceManager::ValidateAndNormalize`) and REST CRUD (`HandleAiInterfaceCreatePost` / `HandleAiInterfaceUpdatePut`).

- **Plain `http://` is loopback-only.**  At create/update time an `http://` URL is accepted only when every address returned by `getaddrinfo` falls in the IPv4 loopback range `127.0.0.0/8` or is the IPv6 loopback `::1` (IPv4-mapped IPv6 `::ffff:127.x.y.z` normalised); every record of a multi-record response must be loopback.  The supported use case is local LLM dispatch (ollama / llama.cpp / vLLM at `http://localhost:11434`); anything else would carry prompts (potentially containing PII or template-substituted secrets) in clear text.  `https://` URLs pass without further URL-policy checks — transport encryption + libcurl's own DNS / TLS path apply.
- **Connect-time loopback re-check (DNS-rebinding closure).**  The create-time resolution above is necessary but not sufficient: libcurl re-resolves the host at dispatch, so a hostname that returned `127.0.0.1` at create time but rebinds to a routable IP later would otherwise reach a non-loopback service over plaintext.  `InstallLoopbackGuardForPlaintextHttp` (`code/backend/engine/curlWrapper/loopbackGuard.{h,cpp}`) installs a `CURLOPT_OPENSOCKETFUNCTION` on every plain-`http://` dispatch — the inverse of the connector's `OpensocketStrictCallback` — that inspects the address libcurl actually resolved to and returns `CURL_SOCKET_BAD` for any non-loopback peer, aborting the connection.  Installed on both dispatch paths: the async `ai_call` (`LiveTransport::SetupEasyHandle`) and the synchronous probe (`CurlWrapper::Query` / `TestInterface`).  A rejection logs `[security] ai_dispatch_plaintext_http_nonloopback_rejected resolved_ip='...'`.  `https://` URLs are unaffected (no-op).
- **Credentialed plain HTTP is refused.**  Any `http://` URL combined with a non-empty `key_name` is rejected with `credentialed_plaintext_http`, BEFORE the DNS resolve.  Local ollama / llama.cpp doesn't require auth; the only reason to attach a `key_name` to a plaintext interface is operator confusion (likely copy-pasted from a cloud interface), and that confusion would silently leak the Bearer token in transit on every dispatch.  Pre-DNS check ensures the typo + the credential don't leak together in a single rejection log line.
- **Forbidden schemes.**  Anything other than `http://` or `https://` (`ws://`, `file://`, `ftp://`, missing scheme, empty URL) is refused with `url_policy_violation`.  The allowlist is closed — no env-var or flag escape hatch.
- **Two enforcement points, same validator.**  Config-load rejection is fail-closed: a non-compliant interface is dropped from `m_ApiInterfaces` and a `LOG_CORE_ERROR("ConfigParser: rejected AI interface ...")` line names the entry + the typed error code.  REST POST/PUT rejection returns HTTP 400 with `error: "url_policy_violation"` or `"credentialed_plaintext_http"` and a `LOG_SECURITY_WARN("[security] ai_interface_url_rejected ...")` line for the audit trail.  Update PUTs re-validate after applying partial updates so a swap of just the URL (or just the key_name) cannot sneak past the create-time gate.
- **Per-dispatch audit trail.**  `AiRequestPool::Submit` emits `LOG_SECURITY_INFO("[security] ai_dispatch_plaintext_http host='...' run='...' workflow='...' task='...' interface='...'")` for every successfully dispatched plain-HTTP request.  Combined with the create-time + update-time gates, every plaintext byte that ever leaves j9t is greppable in `log/security.txt`.
- **Live counters on `/api/debug/signals`** (debug builds):
  - `url_policy_rejections` — total non-loopback / forbidden-scheme / malformed rejections across config-load + REST.
  - `credentialed_plaintext_http_rejections` — separate counter for the http:// + key_name combo; operators want this distinct because that pattern indicates a configured credential the operator intended to use, which is the higher-priority signal.
  - `ai_dispatch_nonloopback_http_rejections` — connections aborted by the connect-time loopback guard (a host that rebound to a non-loopback IP at dispatch).
- **Request-header injection guard.**  `LiveTransport::SetupEasyHandle` rejects any request header (the signer-produced `publicHeaders` and the auth header) carrying a `\r` or `\n` before it reaches `curl_slist_append`, failing the dispatch closed (`[security] ai_dispatch_header_crlf_rejected`).  Defense-in-depth at the single splice boundary covering all auth signers, present and future.  Host strings echoed into URL-policy error/log details are capped at 256 chars so a hostile interface URL can't bloat the log or response.
- **Debug-only localhost TLS skip.**  Hermetic dispatcher tests point AI interfaces at the j9t self-signed mock (`https://localhost:8443`); `LiveTransport::SetupEasyHandle` disables peer/host verification for `localhost` / `127.0.0.1` / `::1` — but only inside a `#ifdef DEBUG` block.  `premake5.lua` defines `DEBUG` for the Debug configuration only (Release defines `NDEBUG`), so the skip cannot reach a shipped binary; each exercise additionally logs `[security] debug_localhost_tls_verification_disabled` so a debug build is never silent about it.
- **Coverage gap closed.**  This is the third leg of the outbound-HTTP SSRF cluster: completion callbacks (`IsCallbackUrlAllowed`, j9t Engine §) reject internal addresses; cloud connectors (`ValidatePublicHttpEndpoint`) reject internal addresses + non-loopback IP literals; AI interfaces (`UrlPolicy::ValidateAiInterfaceUrl` + the connect-time loopback guard) require loopback for plain HTTP at validation AND at connect.  The three gates collectively codify "plain HTTP is only safe when it cannot leave the machine, and never carries a credential".

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

### IAuthSigner Security

The polymorphic `IAuthSigner` (`code/backend/engine/curlWrapper/authSigner.{h,cpp}`) is the single auth-header production point for all AI-provider HTTP requests.  Every concrete signer (`BearerSigner`, `XGoogApiKeySigner`, `AnthropicXApiKeySigner`, `AzureApiKeySigner`, `SigV4Signer`) routes through the same `[[nodiscard]] bool Apply(QueryData const&, vector<string>& publicHeaders, SecureString& secretHeader, errorMessage)` interface so security guarantees apply uniformly to OpenAI Chat / OpenAI Responses / Gemini / Anthropic / Azure OpenAI / AWS Bedrock without per-provider branching.  `publicHeaders` carries non-secret headers (Content-Type, version, signed-not-secret); `secretHeader` is a caller-owned `SecureString` (mlock'd, zero-on-destruct) that the signer fills via `SecureString::Format(prefix, secret.Get())` so the secret never appears in a `std::string` heap allocation.  SigV4 emits its derived Authorization signature through `publicHeaders` (HMAC-SHA256 — derived from but not containing the raw secret) and routes the raw STS session token through `secretHeader` as `X-Amz-Security-Token` when present.  See "SecureString-only HTTP path" below for the full discipline across both AI dispatch and cloud connectors.

- **Empty + whitespace credential rejection at the signer.**  Each `Apply` validates the credential is non-empty AND has at least one non-whitespace character via the `IsBlank` helper.  An accidentally-blank credential (`""` or `"   "` from a misconfigured keystore entry or env var) is caught locally with a structured ERROR, rather than going on the wire and bouncing off the provider as an opaque 401.  SigV4's three required fields (`access_key_id`, `secret_access_key`, `region`) are read from the typed `QueryData::m_AwsCredential` snapshot and validated independently so a missing region produces a different error than a missing secret.  The signer ALSO fails closed if `m_AwsCredential` itself is null on the SigV4 path — a defensive backstop in case a caller routes a request to SigV4 without populating the typed credential.
- **No silent fallback in `IAuthSigner::Get`.**  The function throws `std::logic_error` with the unhandled style's integer value rather than falling back to Bearer — a missing case in the switch would otherwise silently sign every request with the wrong style.  `-Wswitch` catches the missing case at compile time on most builds; the throw is the runtime backstop.  Same closed-enum-no-default discipline as CurlMultiDispatcher.
- **Failure-path log routing.**  The signer subsystem has no run context (no runId/workflowId in `QueryData` directly), so it does NOT emit the failure log itself.  Instead it returns `false` + populates `errorMessage`, and the upstream caller (which has run context — `m_QuotaKey` ≈ host|model, `m_CancelKey` ≈ per-task ID, `m_Url`) emits the structured ERROR — every D1 fail-path ERROR log carries `runId`/`workflowId`/`taskId` as literal substrings so the dashboard run-analyser attributes them.  Two callers:
  - `CurlWrapper::Query` (sync path) — emits `LOG_CORE_ERROR("CurlWrapper::Query: auth signer rejected request url='{}' quotaKey='{}': {}", ...)` and returns `QueryResult::Fail(QueryErrorCode::NoApiKey, ...)`.
  - `LiveTransport::SetupEasyHandle` (async path, hosted under `CurlMultiDispatcher`) — emits `LOG_CORE_ERROR("LiveTransport: auth signer rejected url='{}' cancelKey='{}' quotaKey='{}': {}", ...)`, cleans up the partially-initialised curl easy handle, and surfaces the rejection through the request callback via the dispatcher's deferred-completion path.
- **Stateless singletons + `const Apply`.**  Each signer is a stateless singleton accessed via `IAuthSigner::Get(style)` returning `const IAuthSigner&`.  `Apply` is `const`, documenting the no-mutation invariant and allowing concurrent calls across worker threads with no internal synchronisation.
- **AnthropicXApiKey two-header atomicity.**  The Anthropic signer emits the `x-api-key` (secret) via `secretHeader.Format(...)` first; the non-secret `anthropic-version: 2023-06-01` only gets pushed into `publicHeaders` after the Format succeeds.  If `Format` throws `bad_alloc`, the caller's `publicHeaders` vector is still unmodified and `secretHeader` is unchanged (SecureString::Format uses copy-and-swap internally) — both succeed or neither does.

### SecureString-only HTTP path

Resolved credential material travels from `KeyManager` to `curl_slist_append` without ever materialising into a plain `std::string` heap allocation.  The path is:

```
KeyManager (ICredential::SecureString fields)
   → ResolveApiKey / ResolveCredentials (SecureString.Set(view) into request-scope container)
      → QueryData::m_ApiKey  (SecureString, mlock'd)
      → CloudCredentials::m_Token / m_SecretKey / m_Password  (SecureString, mlock'd)
         → IAuthSigner::Apply(...secretHeader)  /  AppendSecretHeader(curl_slist*, prefix, SecureString)
            → SecureString::Format(prefix, secret.Get())  (single mlock'd allocation)
               → curl_slist_append(list, secret.CStr())  (libcurl strdups internally)
```

The discipline is codified in CLAUDE.md.  Key invariants:

- **`SecureString` is non-copyable by design** — copying a secret to two heap allocations simultaneously is an explicit decision.  `QueryData::Clone()` is the audited deep-copy hook (used only by the dispatcher's retry path which keeps a spare while the original moves into the transport).  `CloudCredentials` is non-copyable (move-only); each request scope builds its own.
- **`SecureString::Format(prefix, secretView, suffix="")`** is the canonical "build a secret-bearing string" pattern for three-piece concatenation.  Single allocation sized to fit all three pieces plus trailing NUL; mlock + zero-on-replace via the existing `Release()`; copy-and-swap internally for strong exception safety (a `bad_alloc` mid-format leaves the prior contents intact).  Implementation forwards to `Build` so the two share their allocator paths.
- **`SecureString::Build`** is the N-piece variant — two overloads, `std::initializer_list<std::string_view>` for static piece sets and `std::span<std::string_view const>` for runtime-sized piece sets (used where the piece count depends on caller state — e.g. SigV4 canonical headers with an extra-headers map of unbounded size).  Used at the `CURLOPT_POSTFIELDS` boundary to build secret-bearing form-urlencoded bodies (OAuth refresh-token request, JWT-bearer grant) without a `std::string` heap intermediate, AND at the canonical-request assembly boundary in `code/backend/engine/curlWrapper/awsSigV4.cpp::Sign` to build canonical-headers + canonical-request directly into mlock'd buffers (the canonical-headers map carries the STS session token when present, so the build site MUST be mlock'd, not a `std::map<string,string>` + `std::string` concat chain).  Same strong-exception-guarantee posture as `Format`.
- **`SecureString::CStr()`** returns a guaranteed-null-terminated `const char*` — relies on the trailing-NUL invariant maintained by `Set()`, `Format()`, and `Build()`.  Used for `CURLOPT_PASSWORD` (libcurl basic-auth path), `CURLOPT_POSTFIELDS` (no-copy pointer; caller keeps SecureString alive through `curl_easy_perform`), and `curl_slist_append`.
- **`AppendSecretHeader(curl_slist*&, prefix, SecureString const&, SecureString& scratch)`** (`code/backend/engine/curlWrapper/curlSlistHelper.h`) is the canonical helper for cloud connectors and workflow filters that build `"Authorization: Bearer <secret>"` headers themselves (rather than routing through `IAuthSigner`).  The scratch is caller-owned (one per HTTP request scope, reusable across multiple calls in the same scope); wipes its mlock'd buffer on destruct or on the next `Format` call.
- **HMAC-input signers** (`AzureSharedKeySigner::Sign` and `code/backend/engine/curlWrapper/awsSigV4.cpp::Sign`) take `SecureString const&` for the secret key and consume via `Get()` views — no `std::string(view)` step.  Both signers build their HMAC chain (`kSecret` / `kDate` / `kRegion` / `kService` / `kSigning` for SigV4; the decoded Base64 `rawKey` for AzureSharedKey) as `std::vector<unsigned char>` buffers wrapped in `ScopedSecretBytes` (OPENSSL_cleanse on destruct).  Shared definition at `code/backend/engine/keys/scopedSecretBytes.h`.  The engine SigV4 signer serves both Bedrock AI dispatch and the S3 cloud-connector path — `SigV4Signer::Inputs` carries optional `m_ContentSha256Override` (callers that pre-hashed the body or use the canonical empty-string hash for empty-body requests) and `m_ExtraHeadersToSign` (Content-Type signing for S3 PUT) so a single signer handles both dispatch shapes.
- **Generator out-parameter SecureString** — `OAuthTokenManager::GetAccessToken`, `JwtGenerator::Generate` / `::GenerateSnowflakeJwt`, and `GcsConnector::ExchangeJwtForAccessToken` take a caller-owned `SecureString& out` instead of returning `std::string` by value.  Connectors pass `credentials.m_Token` (or a local `SecureString`) directly as the out-param; the secret bytes never appear in a `std::string` heap allocation on the return path.  The OAuth refresh round-trip is threaded `SecureString` end-to-end on both sides (request snapshots + `RefreshResult` response fields).
- **The irreducible residue floor is libcurl** — `curl_slist_append` always `strdup`s its input into curl-owned heap; `curl_slist_free_all` calls `free()` not `explicit_bzero+free`.  This residue persists for seconds during a streaming HTTPS request and is outside the threat-model boundary (would require forking libcurl to fix).

**Architectural residue floors (outside threat-model boundary).**  Two third-party-library internal copies of credentials survive `free()` without zeroing.  Same shape in each case: we hand the bytes to the library; the library makes its own copy in its own arena and frees it without `explicit_bzero`.  Fixing would require forking the library.

- **libcurl `curl_slist_append` strdup floor** — every header line and the `CURLOPT_POSTFIELDS` body (when libcurl's internal copy is used) lives in curl-owned heap until `curl_easy_cleanup`.  Empirically confirmed by the heap-scan audit's `AppendSecretHeader` scenario (1 hit at heap address).
- **libpq `PQconnectdbParams` password copy** — libpq copies the password value out of our `values[]` array into a `PGconn`-owned buffer immediately at connect time; `PQfinish` frees with `free()`.  Architecturally equivalent to the libcurl floor.

**Out-of-scope (separate hardening tracks, not credential material).**

- **`QueryData::m_Data`** — request body is `std::string`.  Anthropic prompts and Bedrock SigV4-signed bodies pass through it.  Not strictly a credential, but prompts often carry secret-equivalent material (PII, internal data).  Separate body-path hardening track outside the credential-leakage scope addressed here.

#### Empirical verification — heap-scan audit

The SecureString-only HTTP-path discipline above is verified empirically by an in-process heap-scan audit at `test/security/heapScan_test.{h,cpp}`.  The audit is compiled into the binary only when `--heapscan` is passed to premake (sets `J9T_HEAPSCAN_BUILD`); a production binary contains zero audit code (the engine.cpp call site and the audit module body are both `#ifdef`-guarded).  Audit builds invoke `RunHeapScanAudit()` after the SigV4 self-test and `std::exit()` with PASS/FAIL — the binary never reaches normal engine startup.

**Method.** For each auth style and each secret-bearing build path, the audit:

1. Generates a deterministic 64-byte hex nonce (per-scenario seed so a residue hit is attributable).
2. Plants the nonce as the secret credential in the production data-structure (`QueryData::m_ApiKey`, `AwsCredential::m_SecretAccessKey`, `CloudCredentials` analogues, or in the `client_secret` slot of an OAuth POST body).
3. Drives the auth-build path end-to-end (`IAuthSigner::Apply` / `AppendSecretHeader` / cloud `Sign()` / `SecureString::Build`), allowing all locals to destruct via RAII.
4. Allocates and frees a 64 MiB churn pad to flush fastbins / tcache slabs that recently held secret-bearing allocations.
5. Walks `/proc/self/maps` and reads `/proc/self/mem` over each in-scope mapping; uses `memmem` to detect the nonce.
6. Reports per-scenario hit counts.  The remaining `expected_residual` cell documents the libcurl strdup floor (architectural).

**Two scan phases per scenario:**

- **Smoke** — scans the `[heap]` mapping only (glibc main arena, `sbrk`-backed allocations).  Fast.  Catches the common case: secret-shaped `std::string`s and small heap buffers.
- **Deep** — scans `[heap]` + unnamed anonymous mappings + `[anon:*]` named arenas.  Slower.  Catches large-allocation slabs that bypass `sbrk` and land via `mmap`, plus thread-local arenas.

**Documented scope limitation: stack mappings excluded.**  The scanner holds the search needle as a local while scanning, so any walk of `[stack*]` would always self-detect at the scanner's own frame.  Heap residue is the threat-model target; stack residue is a separate (and harder to test from inside the same process) concern.  A future external-process scanner (`/proc/<j9t-pid>/mem` from a sibling test runner with `kernel.yama.ptrace_scope=0`) could close this gap.

**Scenarios** (10 total — 9 must-be-zero + 1 architectural floor):

| Scenario | Path | Expected smoke / deep |
|---|---|---|
| `Bearer` | `BearerSigner::Apply` (AI dispatch) | 0 / 0 |
| `XGoogApiKey` | `XGoogApiKeySigner::Apply` | 0 / 0 |
| `AnthropicXApiKey` | `AnthropicXApiKeySigner::Apply` | 0 / 0 |
| `AzureApiKey` | `AzureApiKeySigner::Apply` | 0 / 0 |
| `engineSigV4` | `code/backend/engine/curlWrapper/awsSigV4::Apply` — input phase + STS session token via `secretHeader` + canonical-headers via `SecureString::Build` | 0 / 0 |
| `engineSigV4(no-churn)` | Same path as `engineSigV4` but with the 64 MiB churn pad disabled — proves the canonical-headers residue is absent STRUCTURALLY rather than via allocator activity | 0 / 0 |
| `cloudSigV4::Sign` | `code/backend/engine/curlWrapper/awsSigV4::Sign` driven with the S3 dispatch shape — `m_ContentSha256Override` + signed `Content-Type` extra header — exercises `m_ExtraHeadersToSign` and the runtime-sized `Build(std::span<...>)` path | 0 / 0 |
| `AzureSharedKey::Sign` | `code/backend/application/cloud/azureSharedKeySigner::Sign` (Azure Blob connector path; decoded `rawKey` wrapped in `ScopedSecretBytes`) | 0 / 0 |
| `OAuthPostBody` | `oauthTokenManager.cpp::PerformRefresh` body build via `SecureString::Build` (`client_secret` + `refresh_token` form-urlencoded body in mlock'd memory) | 0 / 0 |
| `AppendSecretHeader` | `curlSlistHelper::AppendSecretHeader` → `curl_slist_append` → `curl_slist_free_all` | 1 / 1 (libcurl strdup floor — architectural) |

**Observed result (Studio Debug, clang + libc++):**

```
HeapScan[Bearer]:                 PASS (smoke=0 / deep=0)
HeapScan[XGoogApiKey]:            PASS (smoke=0 / deep=0)
HeapScan[AnthropicXApiKey]:       PASS (smoke=0 / deep=0)
HeapScan[AzureApiKey]:            PASS (smoke=0 / deep=0)
HeapScan[engineSigV4]:            PASS (smoke=0 / deep=0)
HeapScan[AppendSecretHeader]:     PASS (known residual) — libcurl strdup floor
HeapScan[cloudSigV4::Sign]:       PASS (smoke=0 / deep=0)
HeapScan[AzureSharedKey::Sign]:   PASS (smoke=0 / deep=0)
HeapScan[OAuthPostBody]:          PASS (smoke=0 / deep=0)
HeapScan[engineSigV4(no-churn)]:  PASS (smoke=0 / deep=0)
Summary: 9 pass, 0 fail, 1 expected-residual
AUDIT PASSED — exit 0
```

The `engineSigV4(no-churn)` scenario is the headline structural check: it exercises the same SigV4 path as `engineSigV4` but disables the 64 MiB churn pad between scenario teardown and scan.  A clean 0/0 result there proves the canonical-headers session-token residue is structurally absent rather than merely neutralised by allocator activity.  The only steady-state heap residue across all scenarios is the **libcurl `strdup` floor** at `AppendSecretHeader` — the irreducible architectural boundary.

**How to run:**

```bash
premake5 gmake --clang --heapscan
make config=debug
./bin/Debug/jarvisAgent-studio   # runs audit, std::exit(rc), never starts the server
grep HeapScan log/log.txt
```

Audit code is excluded from CI by default (opt-in via `--heapscan`); the artifact is intended as a pre-1.0 one-shot security review attachment and a regression net for future auth-style additions.

### CurlMultiDispatcher Security

The `CurlMultiDispatcher` (`code/backend/engine/curlWrapper/curlMultiDispatcher.{h,cpp}`) drives every parallel AI HTTP/2 request through a single dedicated I/O thread.  Its surface is small (`Submit`, `CancelByCancelKey`, `SignalStop`/`WaitStop`) but its blast radius is large — every AI provider request flows through it, so its safety posture matters disproportionately.

The curl machinery (easy/multi handles, `IAuthSigner` integration, write/header/progress callbacks, `SetupEasyHandle`) is hosted by `LiveTransport` (`code/backend/engine/curlWrapper/liveTransport.{h,cpp}`) behind the `IInterfaceTransport` interface (`code/backend/engine/curlWrapper/interfaceTransport.h`); the dispatcher composes with it via `std::unique_ptr<IInterfaceTransport>` and drives it from its I/O thread (`m_Transport->Submit / Pump / Wait / Wakeup / CancelByCancelKey`).  Each safety guarantee below is owned by whichever side actually holds the code, called out per bullet.

- **Bounded response body + header buffers.** *(LiveTransport)*  The libcurl write callback caps response body accumulation at 32 MiB and the header callback caps header accumulation at 1 MiB.  A buggy or hostile upstream can no longer stream gigabytes into a request's per-handle `std::string` and OOM the engine.  On overflow the callback returns a short-write to libcurl, which translates to `CURLE_WRITE_ERROR` and surfaces back to the dispatcher as a curl-level failure; the dispatcher's `OnTransportComplete` then logs the ERROR with `cancelKey` + `quotaKey` so the dashboard's run analyzer surfaces it.  The body cap is generous for very long structured-output completions (typical AI responses are <1 MiB); the header cap is paranoid for HTTP/2 (typical <16 KiB).
- **Exception-safe libcurl callbacks.** *(LiveTransport)*  `MultiWriteCallback` and `MultiHeaderCallback` are C-boundary callbacks; an exception escaping them is UB.  `std::string::append` can throw `bad_alloc` (commit-and-grow) or `length_error` (size > max_size).  Both callbacks now wrap their bodies in `try / catch (...)` and signal a short-write on any exception, so libcurl aborts cleanly without exception propagation through the C frame.
- **Submit-after-shutdown race closed.** *(Dispatcher)*  `Submit()` reads `m_Stopping` under `m_InboxMutex` before pushing.  The I/O thread's shutdown drain takes the same mutex, so any Submit either (a) observes stopping=false under the lock and queues — in which case the IO thread's drain in stopping mode will see and abort it — or (b) observes stopping=true under the lock and fires `CURLE_ABORTED_BY_CALLBACK` synchronously to the caller.  Pre-fix, a Submit landing AFTER the IO thread had drained-and-exited would orphan the callback (caller blocks forever on its own latch).  `m_Stopping` is monotonic (false→true once), so the under-lock read is sufficient.
- **Closed-set `SetupError` enum.** *(LiveTransport)*  `LiveTransport::SetupEasyHandle` reports failure via a private `enum class SetupError { None, CurlInit, AuthSigner }` plus a human-readable message.  Replaces a fragile `setupError.find("curl_easy_init") == std::string::npos` heuristic that mapped string-prefix to `QueryErrorCode`.  Adding a new failure mode triggers `-Wswitch` at the call site instead of silently falling into a default case.  Same anti-debugging-armor lesson called out in CLAUDE.md's switch-default rule.
- **Per-completion fail-path logs carry runId substring.** *(Dispatcher)*  Three completion-side failure paths in `OnTransportComplete` (curl error, HTTP 429 retries-exhausted, generic HTTP ≥400) log `cancelKey` and `quotaKey` alongside the existing `qnum`.  Per CLAUDE.md "Failure-path logs are ERROR-level AND mention the runId or workflowId as a literal substring", `qnum` alone is a process-wide monotonic counter the run analyzer's substring filter doesn't understand — `cancelKey` is the per-task identifier (= `expectedOutputPath`) the dashboard runs its filter against.  The dispatcher (not the transport) owns this log because the dispatcher carries the runId-bearing `PendingDispatch` entry into the completion path.
- **IPv6 host extraction.** *(Shared free function)*  `ExtractHostFromUrl` (declared in `interfaceTransport.h`, implemented in `liveTransport.cpp`) parses bracketed-IPv6 URLs (`https://[::1]:443/path` → `::1`) by detecting `[` after the scheme and using the matching `]` as the end marker.  Pre-fix the generic `find(':')` clipped at the first `:` of `::1` and returned `[`; the debug-build localhost SSL-skip allowlist could never match an IPv6 localhost request.  The non-IPv6 path is unchanged.  Single implementation feeds both the transport's TLS-suppression branch and the dispatcher's AIMD per-host log lines.
- **Curl handle lifecycle on error.** *(LiveTransport)*  `LiveTransport::SetupEasyHandle` calls `curl_easy_cleanup` on the partially-initialised handle when the auth signer rejects, so a signer rejection never leaks a curl easy handle.  The headers slist (`req.m_Headers`) is initialised to `nullptr` in `LiveTransport::ActiveRequest` and only populated AFTER the signer succeeds, so the rejection path also can't leak slist nodes.  `curl_slist_free_all(nullptr)` is a no-op per libcurl spec.  The `curl_multi_add_handle` return is checked too: on failure (e.g. `CURLM_OUT_OF_MEMORY`) the slist is freed, the easy handle is cleaned up, and a deferred completion is queued so the next `Pump()` fires the request callback as `QueryResult::Fail(CurlNotInitialized, …)` with a structured ERROR (from the dispatcher's `OnTransportComplete`) carrying `cancelKey` + `quotaKey`.  The previous code discarded the return and left a stale entry in `m_Active`.
- **Single source of truth for controller seeding (`EnsureController`).**  Both the admission gate (`DrainInbox`) and the rate-limit observation path (`ParseRateLimitHeaders`) need to find-or-create a `RateLimitController` for a given `QuotaKey`.  Pre-fix the construction logic (initial probe from per-interface strategy, hard cap from `QueryData::m_MaxConcurrency` / `kMaxActivePerHost`) was duplicated across both sites — a guaranteed drift surface as new strategies / config knobs land.  Centralised in a private `EnsureController(quotaKey, queryData)` helper called from both sites.
- **Selective throttle re-queue.**  When the controller for one `QuotaKey` refuses admission, only items targeting that key are deferred back to the inbox; items for OTHER `QuotaKeys` continue through the dispatch path.  Pre-fix, an Anthropic-Opus throttle would block queued OpenAI requests for the rest of the I/O loop iteration even though they had no contention at all.  The change is correctness-shaped (fairer head-of-line behaviour across providers) more than security-shaped, but tighter dispatch reduces the window where retried requests pile up under partial outages.
- **I/O thread loop body is exception-isolated.** *(Dispatcher)*  The whole iteration of `IoThreadFunc` is wrapped in `try / catch (std::bad_alloc) / catch (std::exception) / catch (...)`.  A transient OOM anywhere inside `DrainInbox` / `m_Transport->Pump()` / `DrainPendingCancellations` no longer silently terminates the I/O thread (which would freeze every subsequent AI dispatch with no log line pointing at the cause).  The policy is "log loudly + continue": `m_Inbox` / `m_Active` / `m_RetryQueue` are all unchanged on throw, so the next iteration retries the failed work from its source state.  `bad_alloc` adds a 100 ms sleep before the next iteration to avoid tight retry loops on persistent allocator pressure.

### MockTransport Security

The `MockTransport` (`code/backend/engine/curlWrapper/mockTransport.{h,cpp}`) is the hermetic-fixture sibling of `LiveTransport` — selected per-request by the dispatcher on the `is_mock: true` flag of the resolved AI interface.  Available in **all 4 build targets** (Studio/Engine × Debug/Release) — cyber-sec hardening is at the input boundary, not at the build mode, because operator decisions to use it survive across builds.  The whole point of accepting external paths (config-supplied fixture file + optional sibling `.meta.json`) is that the boundary fails closed:

- **Per-fixture path confinement.**  Every fixture path consumed at load time — primary fixture body AND sibling `<fixture>.meta.json` — is run through `ConfineUnderProjectRoot` (defense in depth — the config parser also validates at load time).  Absolute paths outside the project root, symlink targets outside the root, and `..` traversals that escape are all rejected with a structured ERROR carrying `cancelKey` + `quotaKey` substrings so the dashboard run analyzer surfaces them.  Same canonical-containment gate used by every other ai_call/queue path-handling site per `feedback_path_containment_scope`.
- **Per-fixture size cap.**  `kMaxFixtureBytes = 10 MiB` enforced via `fs::file_size` before the read; oversized fixtures fail closed at the request level.  A malicious or accidentally-large fixture can't OOM the engine through MockTransport.  Cap chosen one order of magnitude below `LiveTransport`'s 32 MiB response cap — fixtures are typically <1 MiB even for chunked multi-turn captures.
- **`.meta.json` HTTP-status allowlist.**  When the optional `<fixture>.meta.json` provides `http_status`, the value MUST be an integer in `[200, 599]`.  Out-of-range values are rejected with ERROR (a synthetic 999 status would otherwise corrupt the rate-limit-strategy state, since AIMD logic branches on status code).
- **`.meta.json` header key allowlist.**  Only `Content-Type` and `Retry-After` are honored in the `headers` block — any other key is dropped with WARN (visible in `log/log.txt`).  Tight allowlist by design: each entry is a header the dispatcher's AIMD parsers or downstream consumers actually read from the response.  Adding a key means committing to honor it in mocks identically to a real provider, so the allowlist gates that commitment.
- **`is_mock` is admin-only.**  It is an AI-interface field, so it lives in the encrypted `API.json.enc` (hydrated at unlock) and is set through the authenticated admin REST endpoint `POST /api/settings/ai-interfaces` — which additionally requires master-password re-auth (same gate as any routing mutation).  An operator without admin role can't enable mock routing on a live provider.
- **`is_mock` requires `fixture_path`.**  Config parser AND REST POST/PUT both enforce that `is_mock: true` requires a non-empty `fixture_path` that resolves under the project root.  Either condition failing marks the interface InvalidAPI with a structured ERROR; the active-index slot rejects InvalidAPI interfaces at submission time, so the only way to enable mock routing is to provide a valid, in-tree fixture.
- **Operator transparency.**  First call to MockTransport per `(quotaKey, fixturePath)` after startup emits one `LOG_APP_INFO` line through the existing log macros — visible in both the ncurses TUI and `log/log.txt`.  Subsequent calls for the same pair are silent so the log doesn't fill under load.  PROV sidecar records `"mocked": true` + the resolved `fixture_path` so post-mortem tooling distinguishes mock dispatches from live ones.
- **UTF-8 sanitization at log boundaries.**  Config-supplied paths flow through `SanitizeUtf8` before reaching `LOG_APP_INFO` — a malformed-UTF-8 fixture-path string from a tampered config can't corrupt the TUI or the log file.  Response body bytes flow through to `ReplyParserAPI1..6` unchanged (that's the test surface); sanitization happens at the parser's `<prob>.output.txt` write site per the existing `feedback_established_safety_patterns` pattern.
- **Pump-only completion delivery.**  MockTransport never fires its `CompletionCallback` synchronously from `Submit` — deferred completions queue into `m_PendingCompletions` and fire on the next `Pump()`.  Matches LiveTransport's contract so the dispatcher's `m_Active` population is race-free regardless of which transport handled a given request.
- **No TestInterface enum variant.**  The `InterfaceType` enum covers API1–API6 only; the `kInterfaceTypeMappings` table mirrors that range.  Legacy `api_type: "Test"` is rejected at parse time with an error pointing at the `is_mock` + `fixture_path` migration path — fixture bytes always go through the real parser, never bypass it.
- **Auth-signer capture for SigV4 KAT tests.**  MockTransport runs the live `IAuthSigner::Get(...).Apply(...)` pipeline against the request's `QueryData` when `AuthStyle == AwsSigV4` AND `m_AwsCredential != nullptr` (the conjunction keeps parser-only fault tests, which configure `key_name=""`, working unchanged).  The resulting headers are captured into a process-global FIFO ring (`MockSignatureCapture`, cap 32, mutex-guarded — see `code/backend/engine/curlWrapper/mockTransport.h`) and surfaced via `/api/debug/signals::last_mock_signatures` (debug builds only).  The `<fixture>.meta.json::x_amz_date_override` field threads through to `QueryData::m_AmzDateOverride` and from there to `SigV4Signer::Sign(in.m_AmzDate)` so the captured Authorization is byte-deterministic for the locked KAT.  Consumed by `test/dispatch/test_bedrock_sigv4.py` (hermetic, no AWS-side credit burn).  The captured Authorization carries the access_key_id (public per AWS conventions) and the signature hex (which is not directly invertible to the secret); the `secret_access_key` itself stays in the typed `SecureString` field of `m_AwsCredential` and is materialised only in the HMAC compute buffer, scoped to the `Sign()` call.

### CurlWrapper Security

The synchronous `CurlWrapper` (`code/backend/engine/curlWrapper/curlWrapper.{h,cpp}`) is the sister of the dispatcher — used by Test Connection paths, the assistant, and `jcwfService` for one-shot HTTP queries that don't need parallel dispatch.  Same auth-signer surface (`IAuthSigner::Apply`); same response-buffer / callback / fail-path-log-routing concerns.

- **Bounded + exception-safe `write_callback`.**  Same defensive posture as `LiveTransport`'s `MultiWriteCallback`: response body capped at 32 MiB, callback body wrapped in `try / catch (...)` so a `std::string::append` throw never crosses the libcurl C boundary.  No header callback in this surface (sync queries don't observe rate-limit headers — that's a dispatcher concern).
- **`m_ReadBuffer` cleared at the start of every `Query()`.**  `CurlManager::GetThreadCurl()` returns a thread-local `CurlWrapper` reused across calls.  Pre-fix, sequential `Query()` calls without an explicit `Clear()` would concatenate the previous response into the next one — a silent footgun that surfaced as parser confusion downstream.  Now the buffer is reset deterministically at every Query entry; the public `Clear()` becomes a defensive no-op rather than a required pre-condition.
- **`qnum` captured locally for log consistency.**  The static `m_QueryCounter` is incremented once at dispatch and the result is held in a local `uint32_t` used by every subsequent log line (success and failure).  Pre-fix the failure path called `m_QueryCounter.load()` again, which can read a higher value if a concurrent `Query()` on another thread incremented the counter in between — making the qnum in the error message diverge from the qnum logged on dispatch and breaking grep-based correlation.
- **Failure logs carry `url` + `quotaKey` substrings.**  Three fail paths (curl error, HTTP 429, generic HTTP ≥400) log `url='{}' quotaKey='{}'` alongside the existing `qnum`.  Same dashboard-run-analyzer-surfacing rationale as the dispatcher (CLAUDE.md "Failure-path logs ... mention the runId or workflowId as a literal substring").  `quotaKey` is the natural correlation key when the run identifier isn't in scope (Test Connection paths don't have a runId).
- **`IsValid()` log severity right-sized.**  Empty-field validation logs are `LOG_CORE_ERROR` rather than `LOG_CORE_CRITICAL`.  An empty `m_Url` or empty `m_ApiKey` (`SecureString::IsEmpty()`) is a per-request misconfiguration (legacy caller forgot to populate the field, etc.), not the engine-level "wake the operator at 3am" condition CRITICAL implies.  ERROR is the right level: still surfaced by the run analyzer, doesn't trigger paging.

### SigV4 Signing Security

The `SigV4Signer` (`code/backend/engine/curlWrapper/awsSigV4.{h,cpp}`) hand-rolls AWS Signature V4 on top of OpenSSL HMAC-SHA256 + SHA256 primitives — no `aws-sdk-cpp` dependency.  Used for AWS Bedrock today; reusable for any AWS service.

- **Startup self-test (4 sub-tests, debug builds).**  `RunSelfTest()` is called once at engine startup in debug builds and validates the implementation against AWS-published reference values:
  1. **SHA256 of empty string** — universal known constant `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.
  2. **Signing-key derivation chain** — verifies `kSigning` matches AWS's published intermediate hex (`f4780e2d…414db404d`) for the example secret + date `20120215` + region `us-east-1` + service `iam` from AWS docs.
  3. **Determinism** — two `Sign()` calls on identical inputs produce identical outputs; spot-checks Authorization-header structure (algorithm + credential-scope + signed-headers prefix).
  4. **Full-chain known-answer** — locks in the EXACT signature for a Bedrock-shaped request using AKIDEXAMPLE + the AWS-published example secret.  Catches regressions in canonical-request assembly, `UriEncode`, `CanonicalQuery`, or `stringToSign` concatenation that the kSigning derivation test (#2) doesn't reach.  A failure here means a downstream Bedrock request would be rejected by AWS as `SignatureDoesNotMatch`.
- **Intermediate signing keys cleansed via `OPENSSL_cleanse`.**  The chain (`kSecret`, `kDate`, `kRegion`, `kService`, `kSigning`) holds AWS-secret-derived material on the heap.  Each is wrapped in a file-local `ScopedSecretBytes` RAII struct that calls `OPENSSL_cleanse` on the buffer in its destructor — including on throw from any subsequent string-concat or HMAC call.  Same posture sittings 1-3 closed for the master-password KEK in `keyEncryption.cpp` via `ScopedKey<N>`; this is the `std::vector<unsigned char>` variant.  `OPENSSL_cleanse` uses memory barriers to prevent compiler dead-store-elimination of the zero — `std::memset` would be optimised away.
- **OpenSSL primitive return values checked.**  `HMAC()` and `SHA256()` are one-shot APIs that return NULL on (rare) failure.  Pre-fix the return was discarded — on failure the output buffer held uninitialised data, the resulting "signature" was gibberish, and AWS rejected with an opaque 401 with no local diagnostic.  Now each helper logs `LOG_CORE_ERROR` and returns empty on NULL; `Sign()` propagates the failure up by leaving `Authorization` empty; `Apply()` detects the empty Authorization, returns `false`, and emits a structured ERROR via the upstream caller (`LiveTransport::SetupEasyHandle`) with run context.
- **Canonical-headers includes `x-amz-content-sha256` always.**  AWS recommends — and S3/Bedrock require — that `x-amz-content-sha256` be included in the SigV4 canonical headers and signed.  Our implementation always includes it.  This means the AWS test-suite minimal vectors (`get-vanilla` etc.) which use only `host;x-amz-date` don't compare directly; sub-test #4 above bridges the gap with a Bedrock-shape vector locked in from a trusted run.
- **No `aws-sdk-cpp` dependency.**  Hand-rolled is ~400 LOC of well-documented crypto plumbing on OpenSSL primitives we already link.  The 50 MB SDK + cmake + maintenance burden isn't justified for request-signing-only.  The startup self-test is the AWS-conformance backstop.
- **Typed-credential threading via `QueryData::m_AwsCredential`.**  `Apply()` reads `m_SecretAccessKey` / `m_SessionToken` / `m_Region` / `m_AccessKeyId` from `q.m_AwsCredential` (a `shared_ptr<AwsCredential const>` snapshot taken under KeyManager's lock at submit time, see `code/backend/engine/keys.md` "Snapshot pattern for deferred dispatch") instead of legacy `QueryData::m_Params` stringy reinjection.  Secret material now stays in `SecureString` **end-to-end through the HMAC chain** — `SigV4Signer::Inputs::m_SecretKey` and `m_SessionToken` are `SecureString`; `Sign()` builds `kSecret` as a raw `std::vector<unsigned char>` directly from the SecureString view (no `"AWS4" + secret` std::string intermediate); HMAC intermediates (`kDate` / `kRegion` / `kService` / `kSigning`) are wrapped in `ScopedSecretBytes` for OPENSSL_cleanse-on-destruct.  The X-Amz-Security-Token header (raw STS session token on the wire) is routed through `secretHeader` via `Format` rather than `publicHeaders` (which would have leaked a std::string heap copy).  The non-secret `service` field (default `"bedrock"`) stays in `m_Params` because it's a request-target attribute, not a secret.  Integration-level KAT coverage is `test/dispatch/test_bedrock_sigv4.py` (hermetic, locked Authorization signature) — pairs with sub-test #2 above (AWS-published `kSigning` derivation) per the `feedback_crypto_test_bootstrap_pattern` constraint that bootstrap-locked tests must ride alongside an independent reference vector.

#### Crypto-test bootstrap pattern (reusable for future cryptographic-primitive tests)

Sub-test #4 above (full-chain SigV4 known-answer) was authored using the **placeholder → run → capture → lock** pattern, which is reusable for any future cryptographic-primitive test where the expected output is non-trivial to hand-derive (full-chain signatures, hand-crafted JWTs, AES-GCM ciphertexts for fixed plaintext + key + IV, etc.):

1. Write the test with `constexpr char const* kExpected = "PLACEHOLDER_TO_BE_FILLED_AFTER_FIRST_TRUSTED_RUN"`.
2. Make the failure log a multi-line `LOG_CORE_ERROR` showing both `got:` and `expected:` so the actual value can be grepped from `log/log.txt` after one boot.
3. Build + run; the test fails loudly.
4. Grep the actual value out of the log, lock it in by replacing the placeholder.
5. Rebuild; test now passes and locks in regression detection.

**Constraint.** Pair this pattern with at least one INDEPENDENT crypto sub-test in the same suite (e.g., a known-answer test against an RFC, NIST, or vendor-published intermediate hex — for SigV4 that's sub-test #2, the AWS-published `kSigning` derivation).  The independent test proves the cryptographic primitives are correct against an external reference; the bootstrap-locked test catches REGRESSIONS in how those primitives are wired together (canonical assembly, serialisation, encoding chain).  Never use the bootstrap pattern as the SOLE test of a new crypto implementation — that would lock in your own potentially-buggy output as the regression baseline.

**When NOT to use.** If the expected value should match an externally-published reference exactly (RFC test vectors, NIST FIPS vectors, vendor reference implementations), hand-type the expected from the spec and do NOT trust your own implementation to bootstrap it.  Use this pattern for "this pipeline's stable output for these inputs", never for "this matches the spec's published intermediate".

### JwtGenerator Security

- **Algorithm pinning.** RS256 is fixed in the `JwtGenerator::Generate` API — the JWT header `{"alg":"RS256","typ":"JWT"}` is built internally; callers cannot pass a header that lies about the algorithm.  Closes the alg-confusion footgun where a `{"alg":"none"}` header paired with a real RS256 signature could be accepted by a misconfigured verifier that trusts the header field.
- **Key-type validation.** The private key must be an RSA key (`EVP_PKEY_id(pkey) == EVP_PKEY_RSA`).  EC, DSA, Ed25519, Ed448, X25519 keys are rejected before signing — `EVP_DigestSign` would otherwise produce a signature of the wrong shape while the JWT header still claims RS256, a verifier that trusts the header would accept a signature it didn't actually validate against the right scheme.
- **Key-size enforcement.** Minimum 2048-bit RSA per NIST SP 800-131A (`MIN_RSA_KEY_BITS`).
- **Exception safety.** `EVP_PKEY*` and `EVP_MD_CTX*` are held via file-local `unique_ptr`-with-custom-deleter wrappers (`EvpPkeyPtr`, `EvpMdCtxPtr`); `std::bad_alloc` from any string / vector operation between alloc and use cannot leak the OpenSSL handle.
- **Secret-redactor coverage.** Generated JWTs are registered with `SecretRedactor` immediately after `EVP_DigestSignFinal` so any subsequent log line containing the JWT (executor traces, downstream connectors) is auto-scrubbed.
- **Private key material** is freed via the RAII deleter when the `EvpPkeyPtr` goes out of scope — no manual `EVP_PKEY_free` calls on success or any error path.

### Snowflake JWT Authentication

Snowflake uses RSA key-pair authentication via the `JwtGenerator`:

- **No password or shared secret** — authentication is based on an RSA key pair; the private key never leaves the j9t host
- **JWT expiry** — tokens are generated with a 1-hour expiry (`exp` claim), regenerated per request by `ResolveCredentials()`
- **Public key fingerprint** — the JWT `iss` claim includes the SHA-256 fingerprint of the public key, binding the token to a specific key pair
- **Key storage** — RSA private key (PEM) is stored in the encrypted key store as a `KeyPairCredential`
- **Request header** — `X-Snowflake-Authorization-Token-Type: KEYPAIR_JWT` signals Snowflake to validate the JWT against the user's assigned public key
- **Statement cancellation** — if a workflow run is cancelled during async polling, the executor sends a cancel request to Snowflake to release server-side resources

### OAuthTokenManager Security

- **Refresh-token storage at rest.** Refresh tokens persist in `KeyManager` (provider `m_RefreshToken`), encrypted with the master password via the AES-256-GCM keystore.  In-memory copies in `m_Tokens[name].m_RefreshToken` are registered with `SecretRedactor` on hydrate / store / rotate so any log line containing them is auto-scrubbed.
- **Refresh request body URL-encoded.** All form fields (`refresh_token`, `client_id`, `client_secret`) pass through `curl_easy_escape` before being concatenated into the `application/x-www-form-urlencoded` body.  RFC 6749 doesn't restrict the refresh-token charset; a token containing `&` or `=` (rare but legal) without encoding would corrupt the request and produce an opaque "invalid_request" error from the provider.
- **Refresh-failure backoff.** After a failed refresh, both on-demand refreshes (from `GetAccessToken`) and the background loop suppress re-attempts for `BACKOFF_AFTER_FAILURE_SECONDS` (60 s).  Prevents a refresh storm against the OAuth provider when the refresh token has been revoked at the provider side — without backoff, every `GetAccessToken` call would be a network round-trip.
- **`expires_in` floor.** Server-supplied `expires_in <= 0` (or absurdly small values) are clamped to `MIN_EXPIRES_IN_SECONDS` (60 s).  Without the floor, a misconfigured or hostile provider response with `expires_in: 0` would set the access token's `m_ExpiresAt` in the past, triggering immediate re-refresh on the next `GetAccessToken` call → refresh storm.
- **No data race on `TokenEntry` fields.** The refresh worker (whether `GetAccessToken`'s on-demand path or the background `RefreshLoop`) snapshots the inputs (`tokenEndpoint`, `clientId`, `clientSecret`, `refreshToken`) under the manager's mutex, then drops the lock for the network call with stack-local copies, then re-acquires the lock and applies the result via `ApplyRefreshResult`.  Concurrent `StoreTokens` / `RemoveTokens` calls cannot race with the network call's reads/writes of the entry.
- **No iterator-invalidation UB in `RefreshLoop`.** The background loop snapshots the list of pending refresh tasks under lock (a `std::vector<PendingRefresh>` of by-value copies), then iterates the snapshot with the lock released.  A concurrent `StoreTokens` that triggers an `unordered_map` rehash cannot invalidate the loop's iteration position because the iteration is over the snapshot vector, not over `m_Tokens` directly.
- **Race-safe `Start()`.** Two concurrent `Start()` calls cannot both spawn refresh threads; the `compare_exchange_strong` on `m_Running` lets exactly one caller proceed.  If `HydrateFromKeyManager` or `std::thread` construction throws after the flag is set, the flag is rolled back so a subsequent `Stop()` does not attempt to join a non-joinable thread.
- **Exception-safe `RefreshLoop`.** The loop body is wrapped in `try/catch` so an uncaught `std::bad_alloc` from any `std::string` operation or simdjson parse cannot terminate the background thread silently.
- **`RemoveTokens(keyName)` revocation API.** Public method that erases the entry from `m_Tokens` AND unregisters its secrets from the redactor.  Wakes any `GetAccessToken` waiter so they observe the removal and return an appropriate error rather than blocking on a refresh that will never complete.  (Wire-up from `KeyManager::DeleteProvider` is a follow-up — today the API exists but is not invoked by the delete path.)

### OAuth 2.0 with PKCE (OneDrive)

The OneDrive integration uses the OAuth 2.0 authorization code flow with PKCE (Proof Key for Code Exchange):

- **No client secret** — PKCE replaces the client secret with a per-flow `code_verifier` / `code_challenge` pair, making it safe for public/native clients
- **code_verifier** is generated from 32 bytes of `RAND_bytes()` (OpenSSL CSPRNG), base64url-encoded to 43 characters
- **code_challenge** is SHA-256 of the verifier, sent in the authorization request; Microsoft validates it during token exchange
- **Code verifiers are ephemeral** — stored in-memory only (WebServer `m_OAuthCodeVerifiers` map), never persisted to disk
- **Token refresh** — `OAuthTokenManager` runs a background thread refreshing tokens 5 minutes before expiry via `POST /oauth2/v2.0/token`. Both access and refresh tokens are registered with `SecretRedactor` on acquisition and rotated on refresh — see "OAuthTokenManager Security" below for the full safety guarantees
- **Scope restriction** — default scopes are `Files.ReadWrite offline_access` (minimum for file upload/download + token refresh). Operators should not grant broader scopes than needed
- **Redirect URI** — callback is `https://localhost:{port}/api/connections/{name}/oauth/callback` (scheme follows the server's TLS config — `https://` when `config.m_TlsCert` and `m_TlsKey` are set, which is the default; `http://` otherwise), only reachable on the local machine
- **OAuth callback is intentionally unauthenticated** — the user-agent redirect from Google / Microsoft cannot carry the j9t admin Bearer token, so the callback handler does NOT require it.  The CSRF gate is the `state` query parameter, a single-use 16-byte random nonce generated server-side at `/oauth/authorize` time, stored in the in-memory `m_OAuthStateTokens` map, and verified inside `HandleOAuthCallbackGet` before any code-for-token exchange.  Per RFC 6749 §10.12, `state` IS the security mechanism for an OAuth callback; the `/oauth/authorize` endpoint that creates the flow remains admin-gated.

### Messaging Security (Slack, Email)

- **Slack Bot tokens** (`xoxb-...`) are stored in the encrypted key store and registered with `SecretRedactor`
- **Email credentials** (username + password/app password) are stored as `BasicAuthCredential` in the encrypted key store
- **SMTP TLS** — When the connection's `use_ssl` param is `"true"` (the default), `CURLOPT_USE_SSL = CURLUSESSL_ALL` is set unconditionally so the SMTP send refuses to proceed without TLS, with `CURLOPT_SSL_VERIFYPEER = 1L` and `CURLOPT_SSL_VERIFYHOST = 2L` enforced explicitly.  libcurl's URL scheme detection (`smtps://` for 465, `smtp://` for 587) drives implicit-TLS-vs-STARTTLS choice; the policy gate is independent of port.  `use_ssl: "false"` is the local-testing opt-out (GreenMail / Mailpit) and emits `[security] email_send_tls_disabled` per send.
- **IMAP TLS** — Same `use_ssl`-gated posture as SMTP.  When `"true"` (default), `CURLOPT_USE_SSL = CURLUSESSL_ALL` plus `CURLOPT_SSL_VERIFYPEER = 1L` and `CURLOPT_SSL_VERIFYHOST = 2L` are set explicitly on every `EmailConnector::ImapCommand` call.  `use_ssl: "false"` selects `CURLUSESSL_NONE` and emits `[security] email_imap_tls_disabled` per IMAP request.  IMAPS (port 993) implies implicit TLS via libcurl's URL scheme detection; the policy gate is independent of port.
- **Attachment handling** — attachments are read from the task working directory only; path traversal is rejected by `ICloudTaskExecutor::ValidateLocalPath` (per-attachment, with skip-with-WARN on overflow).  Attachment file size is bounded at 25 MB.
- **Email content** — message body and subject may contain workflow template variables; operators should review templates to avoid exposing sensitive data

### Additional Integrations Security (GitHub, Jira, Google Sheets)

- **GitHub PATs** are stored in the encrypted key store; `User-Agent: j9t/1.0` header is set per GitHub API requirements
- **Jira Cloud** uses BasicAuth (email + API token) — the API token is stored as `BasicAuthCredential`; Jira Data Center uses PAT as Bearer token
- **Google Sheets** supports both API key (read-only, public sheets) and OAuth2 (read/write, private sheets). API keys are sent as `?key=` query parameters (not in headers) per Google's convention
- **GitHub file content** returned by `get_file` is base64-decoded before writing to disk; content is constrained to the task working directory by `TaskPathResolver`

### Phase 9 Hardening

- **Circuit breaker** — `CloudCircuitBreaker` prevents failure cascading during cloud outages. Per-connection state machine (Closed/Open/HalfOpen) with configurable thresholds. All cloud tasks automatically benefit via `ICloudTaskExecutor`.  Failure-class policy (`IsConnectionFailure(code)`) distinguishes connection-class codes (Network / Auth / Credential / OAuth / Http / InvalidConfig / InvalidEndpoint / Unknown — these tick the counter and can trip the breaker Open) from known-good-connection app-level rejections (`ValueOutOfRange` — db_query row/byte/timeout caps or storage upload-size caps; recorded for display but does not tick the counter).  Every cloud-task executor classifies its failures into a typed `ConnectorErrorCode`.  Closes a self-DoS path where a user repeatedly hitting an app-level cap would lock themselves out of a healthy connection.
- **Audit logging** — all cloud task executions are logged to `log/security.txt` with task ID, connection name, connection type, and run ID
- **OAuth CSRF protection** — random 16-byte `state` token generated per OAuth flow, validated on callback before token exchange
- **Resource caps (downloads + uploads + response bodies)** — Phase 9 set `CURLOPT_MAXFILESIZE_LARGE` (256 MB) on S3 and OneDrive download operations.  Subsequently extended with matching upload caps (256 MB on S3 / GCS / Azure Blob / OneDrive uploads — symmetric with downloads) and writeCallback response-body caps (64 MB on the 5 cloud-storage executors + Snowflake; 10 MB on email IMAP; 25 MB on email attachments).  Caps are file-local `static constexpr` per surface — values match each protocol's typical response size with margin, not a single global value.
- **Path traversal validation** — `ICloudTaskExecutor::ValidateLocalPath()` is the security gate for every `local_path` / `file_path` / `body_file` / `attachments` param across the cloud surface.  Resolution follows JCWF spec §3.2.1: relative paths resolve under the task `working_directory`, absolute values (typically `{{<task>.output_file}}` templates) pass through.  Both forms are then confined under the JarvisAgent launch CWD as the project-tree security boundary — anything resolving outside the project (e.g. `/etc/passwd`) is rejected.  All 6 cloud executors with local-file params (azureBlob, email, gcs, oneDrive, s3, sheets) plus Snowflake share this gate; rejections emit `[security] path_traversal_blocked` lines.
- **Cancellation propagation** — `TaskCancellationToken` shared per run, cancelled via `POST /api/workflow-runs/{runId}/cancel`, checked by long-running cloud tasks (Snowflake async polling)

### Remaining Threats (Cloud-Specific)

- **Credential exposure if encrypted key file is compromised** — mitigated by AES-256-GCM with the full file header (magic + version + salt + IV) bound as Additional Authenticated Data, plus PBKDF2-HMAC-SHA256 at 600,000 iterations (file format V2; OWASP 2023+ recommendation).  V1 blobs (100k iterations, no AAD) decrypt for back-compat and auto-promote to V2 on the next save.  Ultimately depends on master password strength.
- **OAuth token theft if master password is weak** — operator responsibility to use a strong master password
- **Outbound data exfiltration via misconfigured cloud tasks** — operator should review cloud connections and restrict OAuth scopes to minimum needed

### Admin Responsibility (Cloud-Specific)

- Review and audit cloud connection configurations
- Restrict OAuth scopes to the minimum required
- Configure egress firewall rules for cloud endpoints
- Monitor cloud task execution in `log/security.txt`
