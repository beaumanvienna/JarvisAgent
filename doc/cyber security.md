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
| **DAG** | Directed Acyclic Graph — a workflow structure where tasks have dependencies but no circular loops |
| **DoS / DDoS** | Denial of Service / Distributed Denial of Service — an attack that floods a server with requests to make it unavailable |
| **HMAC-SHA256** | Hash-based Message Authentication Code using SHA-256 — a way to sign a message so the receiver can verify it was not tampered with and came from a trusted sender |
| **HTTP / HTTPS** | HyperText Transfer Protocol (/ Secure) — the protocol web browsers and APIs use to communicate. HTTPS adds encryption via TLS |
| **JCWF** | JC Workflow Format — j9t's JSON-based file format for defining workflows |
| **j9t** | Short name for JarvisAgent |
| **Ops team** | Operations team — the people responsible for deploying, monitoring, and maintaining servers in production |
| **PII** | Personally Identifiable Information — data that can identify an individual (name, email, address, etc.) |
| **SIEM** | Security Information and Event Management — software that collects and analyzes security logs from multiple systems to detect threats (e.g. Splunk, Microsoft Sentinel) |
| **SPA** | Single-Page Application — a web app that loads once and updates dynamically (the j9t dashboard) |
| **TLS** | Transport Layer Security — encryption for network traffic (the "S" in HTTPS) |
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
| **Audit logging** | None | `log/security.log` (rotating, 10 MB x 5) |
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
- **Security audit logging.** All auth-related events are logged to a dedicated rotating log file (`log/security.log`, 10 MB x 5 files) as well as the application log (TUI/console). Logged events include: auth success/failure with IP and endpoint, rate limit triggers, lockout triggers, webhook accept/reject with workflow ID, shutdown requests, and run control actions (cancel/pause/resume/stop) with run ID. The security log is accessible via `GET /api/log/security` (admin-auth required) and visible in the dashboard Log Viewer's "Security" tab with 3-second polling. Log macros: `LOG_SECURITY_INFO` / `LOG_SECURITY_WARN`.
- **Built-in TLS (HTTPS).** Optional native TLS via Crow's SSL support. Set `"TlsCert"` and `"TlsKey"` in `config.json` to point to PEM certificate and key files. When configured, j9t serves HTTPS on port 8443 instead of HTTP on 8080. If only one field is set or the files don't exist, j9t refuses to start (no silent fallback). `GET /api/status` includes `"tls": true/false`. This eliminates the cleartext last-mile between a reverse proxy and j9t, and can replace the reverse proxy entirely for simpler deployments.
- **Reduced attack surface.** Studio-only modules (workflow editor, AI assistant, AI JCWF generation, settings API, script management) are excluded at compile time. The Engine binary is physically smaller and exposes fewer endpoints.
- **Public endpoints are read-only and non-sensitive.** Only `GET /api/status` (health check) and the dashboard HTML shell (`GET /`, `/dash-assets/*`) are served without authentication.

### Remaining threats

- **No per-user access control.** There is a single admin token. All token holders have identical privileges. There is no role separation (e.g. read-only monitoring vs full control).
- **Log data sensitivity.** `GET /api/log` returns application logs that may contain prompt content, AI responses, file paths, and error traces. `GET /api/log/security` exposes IP addresses and auth event history. Both are token-protected but log content is not redacted.
- **Unauthenticated shutdown via process signal.** The bearer token protects the `POST /api/shutdown` endpoint, but an attacker with OS-level access can still kill the process via signals (SIGTERM, SIGKILL). This is outside j9t's control.
- **Denial of service.** Rate limiting and auth lockout mitigate request flooding and brute-force attacks, but do not protect against network-level attacks (SYN floods, bandwidth exhaustion). Use a firewall or cloud-level DDoS protection for internet-facing deployments.

### Admin responsibility

The admin (operator) is responsible for:

- **TLS configuration.** Either enable built-in TLS (`TlsCert`/`TlsKey` in config.json → HTTPS on port 8443) or deploy behind a TLS-terminating reverse proxy. Never expose plain HTTP to the internet.
- **Token security.** Treat the admin token like a password. The token is stored in `engine_api_token.txt` (gitignored, file permissions `600`), not in `config.json`. Tokens auto-expire after 90 days and auto-rotate, but can also be manually rotated by deleting the file and restarting.
- **Webhook secret management.** Configure a strong, unique secret for every webhook trigger. Share secrets with integration partners over a secure channel.
- **Network segmentation.** Restrict access to the Engine port (default 8080, or 8443 with TLS) using firewall rules. Only the reverse proxy, webhook callers, and admin workstations should be able to reach it.
- **Security log monitoring.** Review `log/security.log` regularly or forward it to a SIEM. The security log records all auth decisions, lockouts, webhook events, and run control actions. The dashboard's "Security" tab provides a quick view.
- **Log access.** Application and security logs may contain sensitive data (prompts, IP addresses, file paths). Restrict who has the admin token and consider log rotation / redaction for compliance-sensitive environments. Security log rotation is automatic (10 MB x 5 files).
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
| `log/` | Application logs (`log.txt`) and security audit logs (`security.log`) |
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

## Summary

| Concern | Studio | Engine |
|---------|--------|--------|
| Who should run it | Developer, on localhost | Ops team, with TLS enabled or behind reverse proxy |
| Authentication | None | Bearer token + HMAC webhooks + WebSocket auth |
| Rate limiting | None | Per-IP token bucket (100 req/min, burst 20) |
| Auth lockout | None | 10 failures / 5 min → 15-min IP lockout |
| Token lifecycle | N/A | 90-day expiry, auto-rotation, 7-day warning |
| Audit logging | None | `log/security.log` (rotating, 10 MB x 5) + dashboard viewer |
| AI script execution | Yes (review before accept) | No (AI tooling removed at compile time) |
| AI assistant | Yes (approval required for mutations) | No (removed at compile time) |
| TLS | Optional (built-in or not needed on localhost) | Built-in (`TlsCert`/`TlsKey`) or reverse proxy |
| Log sensitivity | Developer sees own logs | Token-protected; security log exposes IPs and auth events |
| Cloud AI safety | Provider-managed content filtering, rate limits, and data policies apply to all AI calls |
