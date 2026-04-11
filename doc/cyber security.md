# Cyber Security

JarvisAgent ships as two editions with different security profiles. This document describes the safety measures in place, the remaining threats, and the responsibilities of operators and end users.

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
| **Authentication** | None | Bearer token (admin), HMAC-SHA256 (webhooks) |
| **Rate limiting** | None | Per-IP token bucket (100 req/min, burst 20) |
| **Failed auth lockout** | None | 10 failures / 5 min → 15-min IP lockout |
| **Token expiration** | N/A | 90-day max age, auto-rotation on expiry |
| **RBAC** | N/A (no auth) | 3 roles: admin, operator, viewer (via gateway headers or bearer token) |
| **Gateway identity** | N/A | Trusts `X-Forwarded-User` / `X-Forwarded-Role` from API gateway |
| **Audit logging** | None | `log/security.txt` (rotating, 10 MB x 5), includes user identity |
| **Security headers** | CSP, X-Frame-Options, Referrer-Policy, Permissions-Policy | Same + HSTS when TLS enabled |
| **Request body limit** | None | Configurable `MaxRequestBodyMB` (default 10 MB) |
| **Built-in TLS** | Optional | Optional (`TlsCert`/`TlsKey` in config.json) |
| **Webhook secrets** | Optional | Mandatory |
| **Workflow CRUD** | Open | Not available (compile-time removed) |
| **AI assistant** | Open | Not available (compile-time removed) |
| **Attack surface** | Full feature set | Minimal — runtime + monitoring only |

---

## j9t Studio — Developer Workstation

### Safety measures

- **Compile-time feature gating.** Studio-only code (workflow CRUD, AI assistant, AI JCWF generation, settings API) is included only when built with `J9T_STUDIO`. Engine builds physically exclude these modules.
- **Script path policy.** Shell tasks must reference scripts under the `scripts/` directory. Path traversal (`..`) is rejected by both the validator and the shell task executor.
- **Validation tiers.** The backend validator enforces schema correctness (Tier A), runtime policy (Tier B), feasibility checks (Tier C), and informational warnings (Tier D) before a workflow runs.
- **Disk-first design.** All inputs, outputs, and intermediate results are written to disk. Nothing is held only in memory, making post-incident forensics straightforward.

### Remaining threats

- **No authentication.** Studio has no login, no token, no access control. Anyone who can reach port 8080 can read workflows, trigger runs, modify config, and shut down the server.
- **AI-generated scripts.** The Generate and Fix Script features produce shell and Python scripts from AI output. A malicious or buggy prompt can produce scripts that delete files, exfiltrate data, or consume disk space. The script review panel lets the developer inspect before accepting, but there is no sandbox.
- **AI assistant tool access.** The assistant can read files, write files, edit files, and run shell commands (with user approval). A compromised or manipulated conversation could lead to unintended file modifications.
- **No TLS.** Studio serves HTTP on localhost. If exposed beyond localhost (e.g. via SSH tunnel or Docker port mapping), traffic is unencrypted.

### Operator responsibility

Studio is designed for **single-developer use on a local machine**. The operator is responsible for:

- Not exposing port 8080 beyond localhost.
- Reviewing AI-generated scripts before accepting them.
- Approving or rejecting assistant tool calls (mutating tools require explicit approval).
- Keeping API keys secure (stored encrypted in `keys.json.enc` with a master password).

---

## j9t Engine — Production Server

### Safety measures

- **Bearer token authentication.** All admin endpoints (workflow monitoring, run control, log viewer, shutdown) require an `Authorization: Bearer <token>` header. The token is a 256-bit cryptographically random hex string, auto-generated on first start and stored in a dedicated file (`engine_api_token.txt`) with restrictive file permissions (`600` — owner read/write only). The token is kept separate from `config.json` to prevent accidental commits. Token comparison uses constant-time logic to prevent timing attacks.
- **Token expiration and auto-rotation.** Each token carries an `issued_at` timestamp (stored as the second line in `engine_api_token.txt`). Tokens older than 90 days are rejected with HTTP 403 (`token_expired`). On expiry, a new token is auto-generated, persisted, and logged to stdout. A startup warning is logged 7 days before expiry. Legacy token files (without `issued_at`) are automatically upgraded on load.
- **Failed auth lockout.** After 10 failed authentication attempts from the same IP within 5 minutes, that IP is blocked for 15 minutes. Locked-out requests receive HTTP 403 with a `Retry-After: 900` header. The lockout is checked before rate limiting (locked IPs don't consume rate-limit tokens). Successful authentication clears the failure count. Lockout entries are cleaned up automatically.
- **HMAC-SHA256 webhook authentication.** Webhook triggers require a per-workflow secret. The caller must include an `X-Webhook-Signature: sha256=<hex>` header computed over the raw request body. Signature verification uses constant-time comparison. In Engine mode, a webhook secret is mandatory — webhooks without a configured secret are rejected with HTTP 403.
- **WebSocket authentication.** WebSocket clients must send `{"type":"auth","token":"<token>"}` as the first message. Unauthenticated connections cannot receive data or send commands.
- **Per-IP rate limiting.** Token bucket algorithm (100 requests/minute per IP, burst of 20) protects against brute-force token guessing and request flooding. Rate-limited requests receive HTTP 429 with a `Retry-After` header.
- **Security audit logging.** All auth-related events are logged to a dedicated rotating log file (`log/security.txt`, 10 MB x 5 files) as well as the application log (TUI/console). Logged events include: auth success/failure with IP and endpoint, rate limit triggers, lockout triggers, webhook accept/reject with workflow ID, shutdown requests, and run control actions (cancel/pause/resume/stop) with run ID. The security log is accessible via `GET /api/log/security` (admin-auth required) and visible in the dashboard Log Viewer's "Security" tab with 3-second polling. Log macros: `LOG_SECURITY_INFO` / `LOG_SECURITY_WARN`.
- **Built-in TLS (HTTPS).** Optional native TLS via Crow's SSL support. Set `"TlsCert"` and `"TlsKey"` in `config.json` to point to PEM certificate and key files. When configured, j9t serves HTTPS on port 8443 instead of HTTP on 8080. If only one field is set or the files don't exist, j9t refuses to start (no silent fallback). `GET /api/status` includes `"tls": true/false`. This eliminates the cleartext last-mile between a reverse proxy and j9t, and can replace the reverse proxy entirely for simpler deployments.
- **Gateway-trusted identity headers.** When deployed behind an API gateway (Kong, AWS API Gateway, Traefik, nginx), j9t trusts identity headers injected by the gateway. Configure `"TrustedProxyHeader": "X-Forwarded-User"` and `"TrustedRoleHeader": "X-Forwarded-Role"` in `config.json`. The gateway handles authentication (OIDC, MFA, SSO) and j9t reads the authenticated user and role from the headers. This allows per-user identity in audit logs without j9t implementing its own identity provider integration.
- **Role-Based Access Control (RBAC).** Three roles with descending privilege: **admin** (full access including shutdown and security logs), **operator** (run control, workflow monitoring, application logs), **viewer** (read-only dashboard, workflow list, run status). In gateway mode, the role comes from the `X-Forwarded-Role` header (default: `viewer` if missing). In bearer-token mode, the token grants `admin` (backward compatible). Routes enforce minimum required role — a viewer attempting to stop a run or access security logs receives HTTP 403 `insufficient_role`.
- **Request body size limit.** Configurable maximum HTTP body size (`"MaxRequestBodyMB": 10` in `config.json`, default 10 MB). Oversized requests are rejected with HTTP 413 `payload_too_large` before parsing. Protects against memory exhaustion attacks via large webhook payloads.
- **Security response headers.** All HTTP responses include: `Content-Security-Policy` (restricts script/style/connection sources to `'self'`), `X-Frame-Options: DENY` (prevents clickjacking), `X-Content-Type-Options: nosniff`, `Referrer-Policy: strict-origin-when-cross-origin`, `Permissions-Policy: camera=(), microphone=(), geolocation=()`. When TLS is enabled, `Strict-Transport-Security` (HSTS) is also set.
- **Reduced attack surface.** Studio-only modules (workflow editor, AI assistant, AI JCWF generation, settings API, script management) are excluded at compile time. The Engine binary is physically smaller and exposes fewer endpoints.
- **Public endpoints are read-only and non-sensitive.** Only `GET /api/status` (health check) and the dashboard HTML shell (`GET /`, `/dash-assets/*`) are served without authentication.

### Remaining threats

- **Gateway header spoofing.** When `TrustedProxyHeader` is configured, j9t trusts the identity header unconditionally. If j9t is accidentally exposed without a gateway in front, any client can inject a fake `X-Forwarded-User` header and impersonate any user. **Mitigation:** always deploy behind the gateway on a private subnet; never expose j9t directly to the internet when gateway trust is enabled.
- **No encryption at rest.** Workflow data, AI outputs, and logs are stored as plaintext files on disk. If the server's storage is compromised (stolen disk, leaked snapshot, improper decommissioning), all data is exposed. **The operator must deploy j9t on encrypted storage** — see "Admin responsibility" below.
- **Log data sensitivity.** `GET /api/log` returns application logs that may contain prompt content, AI responses, file paths, and error traces. `GET /api/log/security` exposes IP addresses, user identities, and auth event history. Both are role-protected (operator+ for app log, admin for security log) but log content is not redacted.
- **Unauthenticated shutdown via process signal.** The bearer token protects the `POST /api/shutdown` endpoint, but an attacker with OS-level access can still kill the process via signals (SIGTERM, SIGKILL). This is outside j9t's control.
- **Denial of service.** Rate limiting, auth lockout, and request body size limits mitigate application-level attacks, but do not protect against network-level attacks (SYN floods, bandwidth exhaustion). Use a WAF or cloud-level DDoS protection for internet-facing deployments.

### Admin responsibility

The admin (operator) is responsible for:

- **Encrypted storage.** j9t does not encrypt data at rest. All workflow data, AI outputs, and logs (`queue/`, `workflows/`, `log/`) are plaintext on disk. **The operator must deploy j9t on encrypted storage** — full-disk encryption (LUKS, BitLocker), encrypted cloud volumes (AWS EBS encryption, Azure Disk Encryption, GCP CMEK), or encrypted Docker volumes. This is the same requirement as PostgreSQL, Elasticsearch, and other backend services that rely on infrastructure-level encryption.
- **TLS configuration.** Either enable built-in TLS (`TlsCert`/`TlsKey` in config.json → HTTPS on port 8443) or deploy behind a TLS-terminating reverse proxy. Never expose plain HTTP to the internet.
- **API gateway.** Deploy j9t behind an API gateway (Kong, AWS API Gateway, Traefik) that handles OIDC/SAML authentication and MFA. Configure `TrustedProxyHeader` and `TrustedRoleHeader` so j9t receives per-user identity and role from the gateway.
- **Private subnet.** Place j9t on a private subnet with no direct internet access. Only the API gateway should be able to reach j9t's port.
- **Token security.** Treat the admin token like a password. The token is stored in `engine_api_token.txt` (gitignored, file permissions `600`), not in `config.json`. Tokens auto-expire after 90 days and auto-rotate, but can also be manually rotated by deleting the file and restarting. In gateway deployments, the bearer token serves as a service-to-service credential between gateway and j9t.
- **Webhook secret management.** Configure a strong, unique secret for every webhook trigger. Share secrets with integration partners over a secure channel.
- **Network segmentation.** Restrict access to the Engine port (default 8080, or 8443 with TLS) using firewall rules. Only the API gateway, webhook callers, and admin workstations should be able to reach it.
- **SIEM integration.** Forward `log/security.txt` to your organization's SIEM (Splunk, Elastic, Microsoft Sentinel, Datadog) for centralized monitoring, alerting, and compliance retention. Each event includes IP, user identity, role, endpoint, and outcome.
- **Multi-tenant isolation.** For deployments serving multiple teams or customers, run one j9t instance per tenant with separate data directories. Route traffic via the API gateway based on tenant identity.
- **Log access.** Application and security logs may contain sensitive data (prompts, IP addresses, user identities, file paths). Security log access is restricted to admin role. Consider log redaction for compliance-sensitive environments. Security log rotation is automatic (10 MB x 5 files).
- **Keeping j9t up to date.** Apply updates promptly to pick up security fixes.

### End user responsibility

End users interact with j9t **indirectly** through a frontend application (e.g. a chatbot, a web portal) that calls j9t's webhook API on their behalf. End users never see the admin token, the dashboard, or the log viewer. Their responsibilities are:

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
| Admin bearer token (`Authorization: Bearer ...`) | **Yes** with built-in TLS or reverse proxy; **No** otherwise | An attacker on the same network can intercept the token if unencrypted |
| Webhook HMAC signatures (`X-Webhook-Signature`) | **Yes** with built-in TLS or reverse proxy; **No** otherwise | The signature itself is not secret (it proves authenticity, not confidentiality), but the request body is visible if unencrypted |
| AI API keys (sent to OpenAI, Google, etc.) | **Yes** — j9t connects to cloud providers via HTTPS | These never travel unencrypted |
| Master password for `keys.json.enc` | **Yes** with built-in TLS or reverse proxy; **No** otherwise | Same network interception risk as the bearer token |
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

**Bottom line:** Either enable built-in TLS or deploy behind a reverse proxy. Without TLS, the admin token travels in cleartext and can be intercepted.

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
| `log/` | Application logs (`log.txt`) and security audit logs (`security.txt`) |
| `config.json` | Server configuration (AI provider URLs, thread count — no credentials) |
| `engine_api_token.txt` | Admin bearer token (auto-generated, file permissions `600`) |
| `keys.json.enc` | Encrypted AI API keys |

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

---

## Direct Public Internet Exposure — Not Supported

**j9t Engine must NOT be exposed directly to the public internet.**

j9t is designed as an **execution backend** that sits behind an infrastructure layer (API gateway, reverse proxy, load balancer). It is not a public-facing web application and lacks the following protections required for direct internet exposure:

- **No WAF** (Web Application Firewall) — j9t cannot inspect or block malicious HTTP traffic patterns (SQL injection, XSS payloads, bot signatures)
- **No DDoS protection** — rate limiting handles application-level flooding but not network-level attacks (SYN floods, amplification attacks, bandwidth exhaustion)
- **No session management** — there are no login sessions, session cookies, or session timeouts
- **No CSRF protection** — no anti-forgery tokens on state-changing requests (the bearer token acts as an implicit CSRF defense, but only if not stored in a cookie)
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

5. **Bearer token as service credential.** The auto-generated bearer token in `engine_api_token.txt` is now a service-to-service credential between the gateway and j9t, not a user-facing password. Store it in the gateway's upstream configuration.

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
| Authentication | None | Bearer token + HMAC webhooks + gateway identity headers |
| RBAC | N/A | 3 roles: admin, operator, viewer (gateway or bearer token) |
| Rate limiting | None | Per-IP token bucket (100 req/min, burst 20) |
| Auth lockout | None | 10 failures / 5 min → 15-min IP lockout |
| Token lifecycle | N/A | 90-day expiry, auto-rotation, 7-day warning |
| Audit logging | None | `log/security.txt` with user identity, rotating, dashboard viewer |
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
| MCP interface | Works (dev/test) | Production target (RBAC enforced) |

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

The MCP server is a standalone TypeScript sidecar communicating with j9t over localhost HTTP:
- Bearer token passthrough: MCP server reads the j9t API token from `engine_api_token.txt` or environment variable
- RBAC: MCP tools respect j9t roles — viewers can list/get, operators can run/cancel
- SSE transport should only be exposed when TLS is configured
- The MCP sidecar targets Engine edition for production (Studio exposes workflow CRUD and AI tooling that MCP clients should not access)

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
